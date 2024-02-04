#include "buf.h"
#include "defs.h"
#include "fs.h"
#include "param.h"
#include "riscv.h"
#include "sleeplock.h"
#include "spinlock.h"
#include "types.h"

// Simple logging that allows concurrent FS system calls.
//
// A log transaction contains the updates of multiple FS system
// calls. The logging system only commits when there are
// no FS system calls active. Thus there is never
// any reasoning required about whether a commit might
// write an uncommitted system call's updates to disk.
//
// A system call should call begin_op()/end_op() to mark
// its start and end. Usually begin_op() just increments
// the count of in-progress FS system calls and returns.
// But if it thinks the log is close to running out, it
// sleeps until the last outstanding end_op() commits.
//
// The log is a physical re-do log containing disk blocks.
// The on-disk log format:
//   header block, containing block #s for block A, B, C, ...
//   block A
//   block B
//   block C
//   ...
// Log appends are synchronous.

// Contents of the header block, used for both the on-disk header block
// and to keep track in memory of logged block# before commit.
struct logheader {
    int n;
    int block[LOGSIZE];
};

struct log {
    struct spinlock lock;
    int start;
    int size;
    int outstanding;  // how many FS sys calls are executing.
    int committing;   // in commit(), please wait.
    int dev;
    struct logheader lh;
};

struct log log;

static void recover_from_log(void);
static void commit();

void initlog(int dev, struct superblock* sb) {
    if (sizeof(struct logheader) >= BSIZE)
        panic("initlog: too big logheader");

    //! 初始化日志锁
    initlock(&log.lock, "log");

    //! 更新日志信息到另一块内存中
    log.start = sb->logstart;
    log.size = sb->nlog;
    log.dev = dev;

    //! 试图 recover
    recover_from_log();
}

// Copy committed blocks from log to their home location
//! 简单将 log 中的内容拷贝到对应的位置
//! log 的核心思想就是将一系列磁盘修改记录在log区中, 保证已经进入 log 的是已经一致的(通过修改 log header)
//! 通过记录对应的block号，然后在commit的时候将log中的内容拷贝到对应的位置
//! 如果在拷贝的过程中出现了 crash，那么在下次启动的时候会通过recover来恢复
//! 本质上，是将一系列磁盘操作原子化
static void install_trans(int recovering) {
    int tail;

    for (tail = 0; tail < log.lh.n; tail++) {
        struct buf* lbuf = bread(log.dev, log.start + tail + 1);  // read log block
        struct buf* dbuf = bread(log.dev, log.lh.block[tail]);    // read dst
        memmove(dbuf->data, lbuf->data, BSIZE);                   // copy block to dst
        bwrite(dbuf);                                             // write dst to disk

        //! 如果不是 recover , unpin , 表示该块已经不需要继续放在 cache 中
        //!? 为什么？
        if (recovering == 0)
            bunpin(dbuf);

        brelse(lbuf);
        brelse(dbuf);
    }
}

// Read the log header from disk into the in-memory log header
static void read_head(void) {
    //! 读取日志的第一个块
    struct buf* buf = bread(log.dev, log.start);

    //! logheader 中存储了 log block 的数量与 blockid
    struct logheader* lh = (struct logheader*)(buf->data);

    int i;

    //! 从 buffer cache 中将 logheader 的信息拷贝到 log.lh 中
    log.lh.n = lh->n;
    for (i = 0; i < log.lh.n; i++) {
        log.lh.block[i] = lh->block[i];
    }

    brelse(buf);
}

// Write in-memory log header to disk.
// This is the true point at which the
// current transaction commits.
//! 简单地将 log header 写入到 disk 中
//! 意味着当前的 transaction 已经 commit (log 区内的东西已经逻辑上做到了持久化)
static void write_head(void) {
    struct buf* buf = bread(log.dev, log.start);
    struct logheader* hb = (struct logheader*)(buf->data);
    int i;
    hb->n = log.lh.n;
    for (i = 0; i < log.lh.n; i++) {
        hb->block[i] = log.lh.block[i];
    }
    bwrite(buf);
    brelse(buf);
}

static void recover_from_log(void) {
    //! 从 disk 上读取 log header, 其中记录了 log block 的数量与 blockid
    read_head();

    install_trans(1);  // if committed, copy from log to disk
    log.lh.n = 0;
    write_head();  // clear the log
}

// called at the start of each FS system call.
void begin_op(void) {
    acquire(&log.lock);
    while (1) {
        if (log.committing) {
            sleep(&log, &log.lock);
        } else if (log.lh.n + (log.outstanding + 1) * MAXOPBLOCKS > LOGSIZE) {
            // this op might exhaust log space; wait for commit.
            sleep(&log, &log.lock);
        } else {
            //! log.outstanding 记录当前有几个事务在执行
            log.outstanding += 1;
            release(&log.lock);
            break;
        }
    }
}

// called at the end of each FS system call.
// commits if this was the last outstanding operation.
void end_op(void) {
    int do_commit = 0;

    acquire(&log.lock);
    log.outstanding -= 1;
    if (log.committing)
        panic("log.committing");
    //! 如果当前已经没有事务在进行
    if (log.outstanding == 0) {
        do_commit = 1;
        log.committing = 1;
    } else {
        // begin_op() may be waiting for log space,
        // and decrementing log.outstanding has decreased
        // the amount of reserved space.
        wakeup(&log);
    }
    release(&log.lock);

    if (do_commit) {
        // call commit w/o holding locks, since not allowed
        // to sleep with locks.
        commit();
        acquire(&log.lock);
        log.committing = 0;
        wakeup(&log);
        release(&log.lock);
    }
}

// Copy modified blocks from cache to log.
static void write_log(void) {
    int tail;

    for (tail = 0; tail < log.lh.n; tail++) {
        struct buf* to = bread(log.dev, log.start + tail + 1);  // log block
        struct buf* from = bread(log.dev, log.lh.block[tail]);  // cache block
        memmove(to->data, from->data, BSIZE);
        bwrite(to);  // write the log
        brelse(from);
        brelse(to);
    }
}

static void commit() {
    if (log.lh.n > 0) {
        write_log();       // Write modified blocks from cache to log
        write_head();      // Write header to disk -- the real commit
        install_trans(0);  // Now install writes to home locations
        log.lh.n = 0;
        write_head();  // Erase the transaction from the log
    }
}

// Caller has modified b->data and is done with the buffer.
// Record the block number and pin in the cache by increasing refcnt.
// commit()/write_log() will do the disk write.
//
// log_write() replaces bwrite(); a typical use is:
//   bp = bread(...)
//   modify bp->data[]
//   log_write(bp)
//   brelse(bp)
//
//! 将自己的修改记录在 log 中(修改 log header, 记录 block id)
//! 每个 cache 都对应一个 log block, 并由 logheader 维护
//! 在写入事务时，会先将 cache 中的内容写入 log 中( 一一对应 )
//! 在 recover 时，通过 header 记录的 Log block id 与对应的 data block 同步
void log_write(struct buf* b) {
    int i;

    acquire(&log.lock);
    if (log.lh.n >= LOGSIZE || log.lh.n >= log.size - 1)
        panic("too big a transaction");
    if (log.outstanding < 1)
        panic("log_write outside of trans");

    //! 查看当前 cache 是否已经记录在 logheader 中
    for (i = 0; i < log.lh.n; i++) {
        if (log.lh.block[i] == b->blockno)  // log absorption
            break;
    }
    log.lh.block[i] = b->blockno;

    //! 如果当前的 block 不在 log 中, 那么需要将其添加到 log 中
    if (i == log.lh.n) {  // Add new block to log?
        bpin(b);
        log.lh.n++;
    }
    release(&log.lock);
}
