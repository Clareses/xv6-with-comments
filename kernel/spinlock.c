// Mutual exclusion spin locks.

#include "spinlock.h"
#include "defs.h"
#include "memlayout.h"
#include "param.h"
#include "proc.h"
#include "riscv.h"
#include "types.h"

void initlock(struct spinlock* lk, char* name) {
    lk->name = name;
    lk->locked = 0;
    lk->cpu = 0;
}

// Acquire the lock.
// Loops (spins) until the lock is acquired.
void acquire(struct spinlock* lk) {
    push_off();  // disable interrupts to avoid deadlock.
    if (holding(lk))
        panic("acquire");

    // On RISC-V, sync_lock_test_and_set turns into an atomic swap:
    //   a5 = 1
    //   s1 = &lk->locked
    //   amoswap.w.aq a5, a5, (s1)
    //! 使用 test_adn_set 完成上锁
    //! while 自旋，直到加锁成功
    while (__sync_lock_test_and_set(&lk->locked, 1) != 0)
        ;

    // Tell the C compiler and the processor to not move loads or stores
    // past this point, to ensure that the critical section's memory
    // references happen strictly after the lock is acquired.
    // On RISC-V, this emits a fence instruction.
    __sync_synchronize();

    // Record info about lock acquisition for holding() and debugging.
    lk->cpu = mycpu();
}

// Release the lock.
void release(struct spinlock* lk) {
    if (!holding(lk))
        panic("release");

    lk->cpu = 0;

    // Tell the C compiler and the CPU to not move loads or stores
    // past this point, to ensure that all the stores in the critical
    // section are visible to other CPUs before the lock is released,
    // and that loads in the critical section occur strictly before
    // the lock is released.
    // On RISC-V, this emits a fence instruction.
    __sync_synchronize();

    // Release the lock, equivalent to lk->locked = 0.
    // This code doesn't use a C assignment, since the C standard
    // implies that an assignment might be implemented with
    // multiple store instructions.
    // On RISC-V, sync_lock_release turns into an atomic swap:
    //   s1 = &lk->locked
    //   amoswap.w zero, zero, (s1)
    __sync_lock_release(&lk->locked);

    pop_off();
}

// Check whether this cpu is holding the lock.
// Interrupts must be off.
int holding(struct spinlock* lk) {
    int r;
    r = (lk->locked && lk->cpu == mycpu());
    return r;
}

// push_off/pop_off are like intr_off()/intr_on() except that they are matched:
// it takes two pop_off()s to undo two push_off()s.  Also, if interrupts
// are initially off, then push_off, pop_off leaves them off.

//! 在内核态获取锁后如果正好触发了一个需要获取此锁的中断将导致死锁发生：
//! 中断处理程序要获取锁，spin
//! 原程序需要等中断程序结束后才能执行
//! 故如果一个锁是 interrupt handler 和其他内核态代码共享在获取锁时必须终止中断

//! 需要记录获取锁之前的中断使能状态
//! 支持多层嵌套，但是只在 nested = 0 时恢复状态
//! 因为nested != 0 时中断应该已经关闭了
//!? 但是直接让中断失能, 是否会导致错过中断？

void push_off(void) {
    int old = intr_get();

    intr_off();
    if (mycpu()->noff == 0)
        mycpu()->intena = old;
    mycpu()->noff += 1;
}

void pop_off(void) {
    struct cpu* c = mycpu();
    if (intr_get())
        panic("pop_off - interruptible");
    if (c->noff < 1)
        panic("pop_off");
    c->noff -= 1;
    if (c->noff == 0 && c->intena)
        intr_on();
}
