// Sleeping locks

#include "sleeplock.h"
#include "defs.h"
#include "memlayout.h"
#include "param.h"
#include "proc.h"
#include "riscv.h"
#include "spinlock.h"
#include "types.h"

void initsleeplock(struct sleeplock* lk, char* name) {
    initlock(&lk->lk, "sleep lock");
    lk->name = name;
    lk->locked = 0;
    lk->pid = 0;
}

void acquiresleep(struct sleeplock* lk) {
    acquire(&lk->lk);

    //! 如果无法获取到
    while (lk->locked) {
        //! 在锁上睡眠,等待锁被释放
        sleep(lk, &lk->lk);
    }
    lk->locked = 1;
    lk->pid = myproc()->pid;
    release(&lk->lk);
}

void releasesleep(struct sleeplock* lk) {
    acquire(&lk->lk);
    lk->locked = 0;
    lk->pid = 0;
    //! 将等待该锁的所有 SLEEPING 进程设置为 RUNNABLE
    wakeup(lk);
    release(&lk->lk);
}

int holdingsleep(struct sleeplock* lk) {
    int r;

    acquire(&lk->lk);
    r = lk->locked && (lk->pid == myproc()->pid);
    release(&lk->lk);
    return r;
}
