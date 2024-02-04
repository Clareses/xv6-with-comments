#include "defs.h"
#include "memlayout.h"
#include "param.h"
#include "riscv.h"
#include "types.h"

volatile static int started = 0;

// start() jumps here in supervisor mode on all CPUs.
//! 到这一段，CPU 进入 Supervisor mode，开始一系列初始化操作
void main() {
    if (cpuid() == 0) {
        consoleinit();
        printfinit();
        printf("\n");
        printf("xv6 kernel is booting\n");
        printf("\n");

        //! 初始化物理内存(建立显式空闲链表，提供了 kalloc / kfree)
        kinit();  // physical page allocator

        //! 虚拟内存系统的初始化, 初始化了内核页表
        //! 直接映射了设备的物理地址/整个物理内存的地址
        //! 同时给每个进程都分配了一个内核栈, 位于虚拟空间的高处
        //! 需要特别注意的是, 虚拟内存的最顶端重复映射了物理内存的地址用于分配进程的内核栈
        kvminit();  // create kernel page table

        //! 开启了分页机制
        kvminithart();  // turn on paging

        //! 初始化进程表，将每个进程的内核栈都指向kvminit时分配的虚拟地址
        //! 注意这里用的还是内核的页表
        procinit();  // process table

        //! 在这里初始化了中断用的锁
        trapinit();  // trap vectors

        //! 这里设置了 内核 的中断入口: kernelvec
        //! 在触发中断时
        //! 如果是用户态，会跳转到 trampoline -> usertrap -> uesrret (userret 会将 stvec
        //! 设置为trampoline) 如果是内核态，会跳转到 kernelvec -> kerneltrap
        trapinithart();  // install kernel trap vector

        plicinit();  // set up interrupt controller

        plicinithart();  // ask PLIC for device interrupts

        //!
        //! 文件系统由以下几个部分组成：
        //! 1. buffer pool ， 通过提供 bget / brelease / bread / bwrite 接口，完成对磁盘的读写
        //! 2. log 层, 提供与 buffer pool 一致数量的 log block，用于记录对磁盘的修改
        //! 3. inode 层， 实现了内存 inode 缓存, 实现了对 inode 内的数据的读写(bmap, readi, writei)
        //! 4. directory 层，实现了对 T_DIR 类型的 inode 的操作
        //! 5. path 层，对路径解析，完成对 de 的读取与写入
        //! 6. file descriptor 层

        //! 初始化 buffer pool
        binit();  // buffer cache

        //! 分为 dinode 与 inode 两种，dinode 映射磁盘上的 inode 结构, inode 存在于内存中
        //! 用于管理文件系统的 inode。inode 以一张表的形式存在与内存中，可以理解为 dinode 的 cache
        //! 因此 inode 除了 dinode 的信息外，还需要 ref cnt 和对原 dinode 的引用 (dev , inum)
        //! 通过 iget / iput / ilock / iunlock 接口，完成对 inode 的读写
        iinit();  // inode table

        //! file 层实现了文件类型的分发
        //! 包括对 read /  write的分发 (设备 / inode / pipe)
        fileinit();  // file table

        virtio_disk_init();  // emulated hard disk

        //! userinit 中会启动第一个用户进程(加入 PCB 中，做出仿佛是刚 fork 出来的样子)
        //! 在 alloc proc 结束时，都会将 ra 设置为 forkret
        //! 这样，当一个新进程第一次执行时，就会跳入 forkret 并直接进入 usertrap, 从而设置 stvec
        //! 而第一个进程，并没有父进程的 trapframe 可复制，因此 pc 会从 0x0 开始, 即正常执行
        userinit();  // first user process

        __sync_synchronize();

        started = 1;

    } else {
        while (started == 0)
            ;

        __sync_synchronize();

        printf("hart %d starting\n", cpuid());

        kvminithart();  // turn on paging

        trapinithart();  // install kernel trap vector

        plicinithart();  // ask PLIC for device interrupts
    }

    scheduler();
}
