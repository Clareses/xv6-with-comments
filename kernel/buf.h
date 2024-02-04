#ifndef BUF_H
#define BUF_H

#include "fs.h"
#include "sleeplock.h"
#include "types.h"

struct buf {
    //! meta data
    int valid;  // has data been read from disk?
    int disk;   // does disk "own" buf? //!? 什么意思
    uint dev;
    uint blockno;
    struct sleeplock lock;
    uint refcnt;

    //! structure for LRU
    struct buf* prev;  // LRU cache list
    struct buf* next;

    //! data
    uchar data[BSIZE];
};

#endif
