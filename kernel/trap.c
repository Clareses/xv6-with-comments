#include "defs.h"
#include "memlayout.h"
#include "param.h"
#include "proc.h"
#include "riscv.h"
#include "spinlock.h"
#include "types.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void trapinit(void) {
    initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void trapinithart(void) {
    w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
//
//! 在 trampoline 中保存好上下文后，会跳转到这里
void usertrap(void) {
    int which_dev = 0;

    if ((r_sstatus() & SSTATUS_SPP) != 0)
        panic("usertrap: not from user mode");

    // send interrupts and exceptions to kerneltrap(),
    // since we're now in the kernel.
    //! 此时已经进入内核，如果再发生中断，就应该使用 kerneltrap 处理
    w_stvec((uint64)kernelvec);

    struct proc* p = myproc();

    // save user program counter.
    //! 将 epc 写入 (这一步在汇编中其实也可以实现)
    p->trapframe->epc = r_sepc();

    if (r_scause() == 8) {
        // system call
        if (killed(p))
            exit(-1);

        // sepc points to the ecall instruction,
        // but we want to return to the next instruction.
        //! 是 system call 的话，返回后是执行下一条指令，故 +4
        p->trapframe->epc += 4;

        // an interrupt will change sepc, scause, and sstatus,
        // so enable only now that we're done with those registers.
        //! 中断会在触发一个中断时自动关闭，因此在此时已经保存完需要的寄存器后，可以打开
        intr_on();

        //! 转交 syscall 分发处理
        syscall();

        //! 外设中断处理
    } else if ((which_dev = devintr()) != 0) {
        // ok
    } else {
        printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
        printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
        setkilled(p);
    }

    if (killed(p))
        exit(-1);

    // give up the CPU if this is a timer interrupt.
    if (which_dev == 2)
        yield();

    usertrapret();
}

//
// return to user space
//
void usertrapret(void) {
    struct proc* p = myproc();

    // we're about to switch the destination of traps from
    // kerneltrap() to usertrap(), so turn off interrupts until
    // we're back in user space, where usertrap() is correct.
    intr_off();

    // send syscalls, interrupts, and exceptions to uservec in trampoline.S
    //! 返回用户态前，将 stvec 设置为 uservec
    uint64 trampoline_uservec = TRAMPOLINE + (uservec - trampoline);
    w_stvec(trampoline_uservec);

    // set up trapframe values that uservec will need when
    // the process next traps into the kernel.
    //! 把所有信息还原了... 包括内核栈 / 页表指针
    //! 第一个进程启动时，做出的上下文的样子就是在执行 forkret 之前
    //! 在 forkret 中会调用 usertrapret，从而完成 stvec 的设置
    p->trapframe->kernel_satp = r_satp();          // kernel page table
    p->trapframe->kernel_sp = p->kstack + PGSIZE;  // process's kernel stack
    p->trapframe->kernel_trap = (uint64)usertrap;
    p->trapframe->kernel_hartid = r_tp();  // hartid for cpuid()

    // set up the registers that trampoline.S's sret will use
    // to get to user space.

    //! 设置返回到 user mode
    unsigned long x = r_sstatus();
    x &= ~SSTATUS_SPP;  // clear SPP to 0 for user mode
    x |= SSTATUS_SPIE;  // enable interrupts in user mode
    w_sstatus(x);

    // set S Exception Program Counter to the saved user pc.
    //! 将返回地址也设置好
    w_sepc(p->trapframe->epc);

    // tell trampoline.S the user page table to switch to.
    uint64 satp = MAKE_SATP(p->pagetable);

    // jump to userret in trampoline.S at the top of memory, which
    // switches to the user page table, restores user registers,
    // and switches to user mode with sret.
    uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
    //! 从 userret (trampoline.S) 中恢复上下文和页表，回到用户态
    ((void (*)(uint64))trampoline_userret)(satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void kerneltrap() {
    int which_dev = 0;
    uint64 sepc = r_sepc();
    uint64 sstatus = r_sstatus();
    uint64 scause = r_scause();

    if ((sstatus & SSTATUS_SPP) == 0)
        panic("kerneltrap: not from supervisor mode");
    if (intr_get() != 0)
        panic("kerneltrap: interrupts enabled");

    //! 通过 devintr 进行设备中断的分发
    if ((which_dev = devintr()) == 0) {
        printf("scause %p\n", scause);
        printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
        panic("kerneltrap");
    }

    // give up the CPU if this is a timer interrupt.
    //! 如果是 Timer Interrupt， yield 即可
    if (which_dev == 2 && myproc() != 0 && myproc()->state == RUNNING)
        yield();

    // the yield() may have caused some traps to occur,
    // so restore trap registers for use by kernelvec.S's sepc instruction.
    w_sepc(sepc);
    w_sstatus(sstatus);
}

void clockintr() {
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int devintr() {
    uint64 scause = r_scause();

    if ((scause & 0x8000000000000000L) && (scause & 0xff) == 9) {
        // this is a supervisor external interrupt, via PLIC.

        // irq indicates which device interrupted.
        int irq = plic_claim();

        if (irq == UART0_IRQ) {
            uartintr();
        } else if (irq == VIRTIO0_IRQ) {
            virtio_disk_intr();
        } else if (irq) {
            printf("unexpected interrupt irq=%d\n", irq);
        }

        // the PLIC allows each device to raise at most one
        // interrupt at a time; tell the PLIC the device is
        // now allowed to interrupt again.
        if (irq)
            plic_complete(irq);

        return 1;
    } else if (scause == 0x8000000000000001L) {
        // software interrupt from a machine-mode timer interrupt,
        // forwarded by timervec in kernelvec.S.

        if (cpuid() == 0) {
            clockintr();
        }

        // acknowledge the software interrupt by clearing
        // the SSIP bit in sip.
        w_sip(r_sip() & ~2);

        return 2;
    } else {
        return 0;
    }
}
