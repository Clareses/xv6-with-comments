#include "defs.h"
#include "file.h"
#include "fs.h"
#include "param.h"
#include "proc.h"
#include "riscv.h"
#include "sleeplock.h"
#include "spinlock.h"
#include "types.h"

#define PIPESIZE 512

//! pipe 本质上只是一个加了进程间同步的 kernel buffer
//! 内核持有双方的页表，因此可以做到从内核态copy数据至用户态
//! pipe 被绑定到一对打开文件上，进程间因此可以做到通过 read / write 读取
struct pipe {
    struct spinlock lock;
    char data[PIPESIZE];
    uint nread;     // number of bytes read
    uint nwrite;    // number of bytes written
    int readopen;   // read fd is still open
    int writeopen;  // write fd is still open
};

//! 新建俩个打开文件作为 pipe 的输入输出文件
//! 这里可以看出，file 并不一定指向文件系统的INODE
//! 还可以指向内存中的管道或是设备, 文件是一个抽象的概念
int pipealloc(struct file** f0, struct file** f1) {
    struct pipe* pi;

    pi = 0;
    *f0 = *f1 = 0;

    //! resource allocate
    if ((*f0 = filealloc()) == 0 || (*f1 = filealloc()) == 0)
        goto bad;
    if ((pi = (struct pipe*)kalloc()) == 0)
        goto bad;

    //! init pipe data
    pi->readopen = 1;
    pi->writeopen = 1;
    pi->nwrite = 0;
    pi->nread = 0;

    initlock(&pi->lock, "pipe");

    //! init file
    (*f0)->type = FD_PIPE;
    (*f0)->readable = 1;
    (*f0)->writable = 0;
    (*f0)->pipe = pi;
    (*f1)->type = FD_PIPE;
    (*f1)->readable = 0;
    (*f1)->writable = 1;
    (*f1)->pipe = pi;

    return 0;

bad:
    if (pi)
        kfree((char*)pi);
    if (*f0)
        fileclose(*f0);
    if (*f1)
        fileclose(*f1);
    return -1;
}

void pipeclose(struct pipe* pi, int writable) {
    acquire(&pi->lock);
    if (writable) {
        pi->writeopen = 0;
        wakeup(&pi->nread);
    } else {
        pi->readopen = 0;
        wakeup(&pi->nwrite);
    }
    if (pi->readopen == 0 && pi->writeopen == 0) {
        release(&pi->lock);
        kfree((char*)pi);
    } else
        release(&pi->lock);
}

int pipewrite(struct pipe* pi, uint64 addr, int n) {
    int i = 0;
    struct proc* pr = myproc();

    //! 锁定管道
    acquire(&pi->lock);

    while (i < n) {
        //! 如果对方已经被 killed 或是关闭了 pipe
        if (pi->readopen == 0 || killed(pr)) {
            release(&pi->lock);
            return -1;
        }

        //! 如果对方一直没读,空间容不下写数据
        if (pi->nwrite == pi->nread + PIPESIZE) {  // DOC: pipewrite-full
            //! 唤醒读取方
            wakeup(&pi->nread);
            //! 自己则等待
            sleep(&pi->nwrite, &pi->lock);
        } else {
            //! 从用户空间复制内容到内核空间的 pipe buffer 中
            char ch;
            if (copyin(pr->pagetable, &ch, addr + i, 1) == -1)
                break;
            pi->data[pi->nwrite++ % PIPESIZE] = ch;
            i++;
        }
    }

    wakeup(&pi->nread);
    release(&pi->lock);

    return i;
}

//! 原理大致同 pipewrite
int piperead(struct pipe* pi, uint64 addr, int n) {
    int i;
    struct proc* pr = myproc();
    char ch;

    acquire(&pi->lock);

    while (pi->nread == pi->nwrite && pi->writeopen) {  // DOC: pipe-empty
        if (killed(pr)) {
            release(&pi->lock);
            return -1;
        }
        sleep(&pi->nread, &pi->lock);  // DOC: piperead-sleep
    }
    for (i = 0; i < n; i++) {  // DOC: piperead-copy
        if (pi->nread == pi->nwrite)
            break;
        ch = pi->data[pi->nread++ % PIPESIZE];
        if (copyout(pr->pagetable, addr + i, &ch, 1) == -1)
            break;
    }
    wakeup(&pi->nwrite);  // DOC: piperead-wakeup
    release(&pi->lock);
    return i;
}
