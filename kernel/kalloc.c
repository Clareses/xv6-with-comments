// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "defs.h"
#include "memlayout.h"
#include "param.h"
#include "riscv.h"
#include "spinlock.h"
#include "types.h"

void freerange(void* pa_start, void* pa_end);

//! 使用 end 标注了 kernel 结束位置
//! 这一部分将被排除在 kernel 的物理内存管理之外
extern char end[];  // first address after kernel.
                    // defined by kernel.ld.

//! 使用了简单的显式空闲链表管理物理内存
//! 该数据结构会被放在 kernel 的 .data 段中
struct run {
    struct run* next;
};

struct {
    //! 用于多核并发的锁
    struct spinlock lock;

    //! 显式空闲链表
    struct run* freelist;

} kmem;

void kinit() {
    initlock(&kmem.lock, "kmem");

    //! 将一整段空间添加入空闲链表中 ( 从内核结束的位置开始一直到物理内存结束 )
    freerange(end, (void*)PHYSTOP);
}

void freerange(void* pa_start, void* pa_end) {
    char* p;

    //! 做了一个向上取整的对齐操作, 将地址对齐到 4096 的倍数
    p = (char*)PGROUNDUP((uint64)pa_start);

    //! 对于每块地址，进行单独的释放
    for (; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
        kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(void* pa) {
    struct run* r;

    //! 空间不对齐，panic
    if (((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
        panic("kfree");

    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);

    //! 让 run 的 next 指向当前的 physical address
    //! 这里其实可以写的不用那么...抽象的...
    r = (struct run*)pa;

    acquire(&kmem.lock);

    //! 进行了一个头插的链表操作
    r->next = kmem.freelist;
    kmem.freelist = r;

    release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void* kalloc(void) {
    struct run* r;

    acquire(&kmem.lock);

    //! 很简单的 pop_front 操作
    r = kmem.freelist;
    if (r)
        kmem.freelist = r->next;

    release(&kmem.lock);

    if (r)
        memset((char*)r, 5, PGSIZE);  // fill with junk
    return (void*)r;
}
