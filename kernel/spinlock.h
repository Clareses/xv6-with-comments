#ifndef SPINLOCK_H
#define SPINLOCK_H

#include "types.h"

// Mutual exclusion lock.
//! 简单的自旋锁
struct spinlock {
    uint locked;  // Is the lock held?

    // For debugging:
    char* name;       // Name of lock.
    struct cpu* cpu;  // The cpu holding the lock.
};

#endif
