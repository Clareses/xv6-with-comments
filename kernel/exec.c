#include "defs.h"
#include "elf.h"
#include "memlayout.h"
#include "param.h"
#include "proc.h"
#include "riscv.h"
#include "spinlock.h"
#include "types.h"

static int loadseg(pde_t*, uint64, struct inode*, uint, uint);

int flags2perm(int flags) {
    int perm = 0;
    if (flags & 0x1)
        perm = PTE_X;
    if (flags & 0x2)
        perm |= PTE_W;
    return perm;
}

//! 加载器
//! 思考加载器是如何做到返回用户态的
int exec(char* path, char** argv) {
    char *s, *last;

    int i, off;

    uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;

    struct elfhdr elf;

    struct inode* ip;

    struct proghdr ph;

    pagetable_t pagetable = 0, oldpagetable;

    struct proc* p = myproc();

    begin_op();

    if ((ip = namei(path)) == 0) {
        end_op();
        return -1;
    }
    ilock(ip);

    // Check ELF header
    if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
        goto bad;

    if (elf.magic != ELF_MAGIC)
        goto bad;

    if ((pagetable = proc_pagetable(p)) == 0)
        goto bad;

    // Load program into memory.
    for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph)) {
        if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
            goto bad;
        if (ph.type != ELF_PROG_LOAD)
            continue;
        if (ph.memsz < ph.filesz)
            goto bad;
        if (ph.vaddr + ph.memsz < ph.vaddr)
            goto bad;
        if (ph.vaddr % PGSIZE != 0)
            goto bad;
        uint64 sz1;
        //! 直接申请整个用到的大小, 因为它堆和栈是反过来的...
        if ((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz, flags2perm(ph.flags))) == 0)
            goto bad;
        sz = sz1;
        if (loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
            goto bad;
    }
    iunlockput(ip);
    end_op();
    ip = 0;

    p = myproc();
    uint64 oldsz = p->sz;

    // Allocate two pages at the next page boundary.
    // Make the first inaccessible as a stack guard.
    // Use the second as the user stack.

    //! 开始准备栈空间，一个用于栈本身，一个用于栈空间的保护, 共两个 page

    sz = PGROUNDUP(sz);

    uint64 sz1;

    //! 申请两个页面
    if ((sz1 = uvmalloc(pagetable, sz, sz + 2 * PGSIZE, PTE_W)) == 0)
        goto bad;

    sz = sz1;

    //! 清空 used 标签
    uvmclear(pagetable, sz - 2 * PGSIZE);

    //! 初始化 sp 寄存器，指向栈顶
    sp = sz;

    stackbase = sp - PGSIZE;

    // Push argument strings, prepare rest of stack in ustack.
    // ! 初始化 main 的调用栈, 准备 argc 和 argv
    for (argc = 0; argv[argc]; argc++) {
        if (argc >= MAXARG)
            goto bad;
        sp -= strlen(argv[argc]) + 1;
        sp -= sp % 16;  // riscv sp must be 16-byte aligned
        if (sp < stackbase)
            goto bad;
        if (copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
            goto bad;
        ustack[argc] = sp;
    }
    ustack[argc] = 0;

    // push the array of argv[] pointers.
    sp -= (argc + 1) * sizeof(uint64);
    sp -= sp % 16;
    if (sp < stackbase)
        goto bad;
    //! 将 argv 的指针数组拷贝到用户栈中
    if (copyout(pagetable, sp, (char*)ustack, (argc + 1) * sizeof(uint64)) < 0)
        goto bad;

    // arguments to user main(argc, argv)
    // argc is returned via the system call return
    // value, which goes in a0.
    // ! 同样准备命令行参数
    // ! ( 所以命令行参数事实上是放入栈中，并设置寄存器完成的 )
    p->trapframe->a1 = sp;

    //! 这一部分用于 debug , ignore ------------------------------------
    // Save program name for debugging.
    for (last = s = path; *s; s++)
        if (*s == '/')
            last = s + 1;
    safestrcpy(p->name, last, sizeof(p->name));
    //!  ignore --------------------------------------------------------

    // Commit to the user image.
    // ! 销毁原来的页表并用加载时新建的页表替换
    // ! 设置 trapframe 的 epc 和 sp
    // ! 从 exec 返回系统调用后，继续执行中断处理程序时，会把这些上下文恢复
    // ! 从而实现回到用户态时从入口函数开始执行
    oldpagetable = p->pagetable;
    p->pagetable = pagetable;
    p->sz = sz;
    p->trapframe->epc = elf.entry;  // initial program counter = main
    p->trapframe->sp = sp;          // initial stack pointer
    proc_freepagetable(oldpagetable, oldsz);

    //! 这里的 return
    return argc;  // this ends up in a0, the first argument to main(argc, argv)

bad:
    if (pagetable)
        proc_freepagetable(pagetable, sz);
    if (ip) {
        iunlockput(ip);
        end_op();
    }
    return -1;
}

// Load a program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int loadseg(pagetable_t pagetable, uint64 va, struct inode* ip, uint offset, uint sz) {
    uint i, n;
    uint64 pa;

    for (i = 0; i < sz; i += PGSIZE) {
        pa = walkaddr(pagetable, va + i);
        if (pa == 0)
            panic("loadseg: address should exist");
        if (sz - i < PGSIZE)
            n = sz - i;
        else
            n = PGSIZE;
        if (readi(ip, 0, (uint64)pa, offset + i, n) != n)
            return -1;
    }

    return 0;
}
