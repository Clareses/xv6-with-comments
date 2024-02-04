// Saved registers for kernel context switches.
#ifndef PROC_H
#define PROC_H

#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "types.h"

//! context 和 trapframe 有什么区别？
//! 在 xv6 中，由于用户地址空间并没有映射整个内核, 无法像 Linux 0.11 一样
//! 直接将上下文保存在内核栈中
//! 只映射了一段内核态代码(trampoline.S), 将上下文直接保存在用户空间中，即 trapframe
//! 而进入了内核后，程序就在内核态中运行，此时如果有新的中断发生，就会保存在内核栈中
//! 即 kernelvec.S 中所实现的那样, 通过这种方式，实现了内核态下的中断嵌套
//! 而 context 记录的是内核态中的信息，即内核栈
//! context switch 所做的事情，是在内核态下，切换内核栈
//! 因此，trapframe 是需要保存用户态下的所有信息，包括页表等
//! 而 context 只需要保存内核态下的寄存器信息即可，而不需要保存页表（使用的都是内核页表）
struct context {
    uint64 ra;
    uint64 sp;

    // callee-saved
    uint64 s0;
    uint64 s1;
    uint64 s2;
    uint64 s3;
    uint64 s4;
    uint64 s5;
    uint64 s6;
    uint64 s7;
    uint64 s8;
    uint64 s9;
    uint64 s10;
    uint64 s11;
};

// Per-CPU state.
struct cpu {
    struct proc* proc;       // The process running on this cpu, or null.
    struct context context;  // swtch() here to enter scheduler().
    int noff;                // Depth of push_off() nesting.
    int intena;              // Were interrupts enabled before push_off()?
};

extern struct cpu cpus[NCPU];

// per-process data for the trap handling code in trampoline.S.
// sits in a page by itself just under the trampoline page in the
// user page table. not specially mapped in the kernel page table.
// uservec in trampoline.S saves user registers in the trapframe,
// then initializes registers from the trapframe's
// kernel_sp, kernel_hartid, kernel_satp, and jumps to kernel_trap.
// usertrapret() and userret in trampoline.S set up
// the trapframe's kernel_*, restore user registers from the
// trapframe, switch to the user page table, and enter user space.
// the trapframe includes callee-saved user registers like s0-s11 because the
// return-to-user path via usertrapret() doesn't return through
// the entire kernel call stack.
struct trapframe {
    /*   0 */ uint64 kernel_satp;    // kernel page table
    /*   8 */ uint64 kernel_sp;      // top of process's kernel stack
    /*  16 */ uint64 kernel_trap;    // usertrap()
    /*  24 */ uint64 epc;            // saved user program counter
    /*  32 */ uint64 kernel_hartid;  // saved kernel tp
    /*  40 */ uint64 ra;
    /*  48 */ uint64 sp;
    /*  56 */ uint64 gp;
    /*  64 */ uint64 tp;
    /*  72 */ uint64 t0;
    /*  80 */ uint64 t1;
    /*  88 */ uint64 t2;
    /*  96 */ uint64 s0;
    /* 104 */ uint64 s1;
    /* 112 */ uint64 a0;
    /* 120 */ uint64 a1;
    /* 128 */ uint64 a2;
    /* 136 */ uint64 a3;
    /* 144 */ uint64 a4;
    /* 152 */ uint64 a5;
    /* 160 */ uint64 a6;
    /* 168 */ uint64 a7;
    /* 176 */ uint64 s2;
    /* 184 */ uint64 s3;
    /* 192 */ uint64 s4;
    /* 200 */ uint64 s5;
    /* 208 */ uint64 s6;
    /* 216 */ uint64 s7;
    /* 224 */ uint64 s8;
    /* 232 */ uint64 s9;
    /* 240 */ uint64 s10;
    /* 248 */ uint64 s11;
    /* 256 */ uint64 t3;
    /* 264 */ uint64 t4;
    /* 272 */ uint64 t5;
    /* 280 */ uint64 t6;
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
// ! 观察整个进程的结构，可以看到以下信息
struct proc {
    //! 进程锁，防止多 CPU 操作同一个进程
    struct spinlock lock;

    // p->lock must be held when using these:

    //! 进程状态，包括：未使用、已使用、睡眠、可运行、运行、僵尸
    //! xv6 预设了固定数量个进程，已使用/未使用是作为进程分配时的 flag
    enum procstate state;  // Process state

    //! chan 是一个 tag, 用于唤醒时判断
    void* chan;  // If non-zero, sleeping on chan

    //! killed 会用于 usertrap 在返回用户态前，如果 killed，直接 exit
    int killed;  // If non-zero, have been killed

    //! 返回的状态码, 在 exit 后会给正在 wait 的父进程
    int xstate;  // Exit status to be returned to parent's wait

    //! pid, 使用了 allocpid 的方式分配... 其实就是进程在数组中的下标
    int pid;  // Process ID

    // wait_lock must be held when using this:
    //! 记录了 parent 的指针
    struct proc* parent;  // Parent process

    // these are private to the process, so p->lock need not be held.

    //! 内核栈地址
    uint64 kstack;  // Virtual address of kernel stack

    //! 这是用户空间堆的大小
    uint64 sz;  // Size of process memory (bytes)

    //! 页表, 只是一个长度为 512 的 uint64 数组，这是三级页表的大小
    pagetable_t pagetable;  // User page table

    //! trapframe 指向用户态和内核态切换时的上下文信息
    //! 这里保存的是物理地址（即内核页表的地址）
    //! 用户态下，trapframe 被放在 trampoline 后一个 page
    struct trapframe* trapframe;  // data page for trampoline.S

    //! 经典上下文
    //! 注意这里只保存了 callee saved (被调用者保存) 寄存器
    struct context context;  // swtch() here to run process

    //! 打开文件表
    //!? 目前并不清楚会如何被初始化, 但是父子进程一定会共享
    struct file* ofile[NOFILE];  // Open files

    //! 每个进程都会记录一个当前工作区, chdir 将作用于它
    struct inode* cwd;  // Current directory

    //! 呃没什么用的字段...
    char name[16];  // Process name (debugging)
};

#endif
