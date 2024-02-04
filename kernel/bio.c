// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.

#include "buf.h"
#include "defs.h"
#include "fs.h"
#include "param.h"
#include "riscv.h"
#include "sleeplock.h"
#include "spinlock.h"
#include "types.h"

//! 同数据库一样，需要用 Buffer 来缓存磁盘内容
//! 一方面提高效率, 另一方面保证鲁棒性
//! bacache 放置在 kernel .data 段中, 并不是由 kalloc 管理的
struct {
    struct spinlock lock;

    //! 每个 buffer 块中可以放 1024 byte
    //! 总共 30 个 Buffer 块
    //! buf 块中除了一些常用的元数据之外，这些 buffer 还通过 prev 和 next 组成双向链表
    struct buf buf[NBUF];

    // Linked list of all buffers, through prev/next.
    // Sorted by how recently the buffer was used.
    // head.next is most recent, head.prev is least.
    //! 使用 LRU 替换算法
    struct buf head;
} bcache;

void binit(void) {
    struct buf* b;

    //!? buffer pool 的全局锁 (已经使用了 sleeplock, 要它做什么?)
    initlock(&bcache.lock, "bcache");

    // Create linked list of buffers
    //! 组成双向链表，并把所有 block 添加进去
    bcache.head.prev = &bcache.head;
    bcache.head.next = &bcache.head;
    for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
        b->next = bcache.head.next;
        b->prev = &bcache.head;
        initsleeplock(&b->lock, "buffer");
        bcache.head.next->prev = b;
        bcache.head.next = b;
    }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
//! 申请 cache 块并初始化
static struct buf* bget(uint dev, uint blockno) {
    struct buf* b;

    acquire(&bcache.lock);

    // Is the block already cached?
    for (b = bcache.head.next; b != &bcache.head; b = b->next) {
        if (b->dev == dev && b->blockno == blockno) {
            b->refcnt++;
            release(&bcache.lock);
            acquiresleep(&b->lock);
            return b;
        }
    }

    // Not cached.
    // Recycle the least recently used (LRU) unused buffer.
    for (b = bcache.head.prev; b != &bcache.head; b = b->prev) {
        if (b->refcnt == 0) {
            b->dev = dev;
            b->blockno = blockno;
            b->valid = 0;
            b->refcnt = 1;
            release(&bcache.lock);
            acquiresleep(&b->lock);
            return b;
        }
    }
    panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf* bread(uint dev, uint blockno) {
    struct buf* b;

    b = bget(dev, blockno);
    if (!b->valid) {
        virtio_disk_rw(b, 0);
        b->valid = 1;
    }
    return b;
}

// Write b's contents to disk.  Must be locked.
void bwrite(struct buf* b) {
    if (!holdingsleep(&b->lock))
        panic("bwrite");
    virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
//! 放弃 cache 块, 投入其他地方使用
void brelse(struct buf* b) {
    if (!holdingsleep(&b->lock))
        panic("brelse");

    releasesleep(&b->lock);

    acquire(&bcache.lock);
    b->refcnt--;
    if (b->refcnt == 0) {
        // no one is waiting for it.
        b->next->prev = b->prev;
        b->prev->next = b->next;
        b->next = bcache.head.next;
        b->prev = &bcache.head;
        bcache.head.next->prev = b;
        bcache.head.next = b;
    }

    release(&bcache.lock);
}

//! 这个会在什么情况被使用？
void bpin(struct buf* b) {
    acquire(&bcache.lock);
    b->refcnt++;
    release(&bcache.lock);
}

void bunpin(struct buf* b) {
    acquire(&bcache.lock);
    b->refcnt--;
    release(&bcache.lock);
}
