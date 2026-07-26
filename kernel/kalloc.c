// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

/*
 * xv6 使用的物理内存范围是：
 *
 * KERNBASE <= pa < PHYSTOP
 *
 * 每个物理页大小为 PGSIZE，因此可以为每个物理页
 * 准备一个引用计数。
 */
#define NPHYPAGES ((PHYSTOP - KERNBASE) / PGSIZE)

/*
 * 把物理地址转换成引用计数数组下标。
 *
 * 例如：
 * pa == KERNBASE             -> 下标 0
 * pa == KERNBASE + PGSIZE    -> 下标 1
 */
#define PA2INDEX(pa) ((((uint64)(pa)) - KERNBASE) / PGSIZE)

struct {
  struct spinlock lock;
  int count[NPHYPAGES];
} kref;

/*
 * 增加一个物理页的引用计数。
 *
 * uvmcopy() 让子进程共享父进程物理页时调用。
 */
void
krefinc(void *pa)
{
  int index;

  if(((uint64)pa % PGSIZE) != 0 ||
     (char *)pa < end ||
     (uint64)pa >= PHYSTOP)
    panic("krefinc");

  index = PA2INDEX(pa);

  acquire(&kref.lock);

  /*
   * count <= 0 表示这个页面当前不应被使用，
   * 却有人试图增加它的引用。
   */
  if(kref.count[index] <= 0)
    panic("krefinc count");

  kref.count[index]++;

  release(&kref.lock);
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&kref.lock, "kref");

  freerange(end, (void *)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  int index;

  p = (char *)PGROUNDUP((uint64)pa_start);

  for(; p + PGSIZE <= (char *)pa_end; p += PGSIZE){
    /*
     * kfree() 的新语义是先把引用计数减一。
     *
     * 系统初始化时，这些页面还没有经过 kalloc()，
     * 引用计数原本是 0。
     *
     * 因此先设置为 1，再调用 kfree()：
     *
     * 1 -> 0 -> 加入空闲链表
     */
    index = PA2INDEX(p);

    acquire(&kref.lock);
    kref.count[index] = 1;
    release(&kref.lock);

    kfree(p);
  }
}

/*
 * 释放物理页。
 *
 * COW 实现后，kfree() 不一定真的释放页面。
 * 它首先减少引用计数，只有引用计数变成 0，
 * 页面才会重新进入空闲链表。
 */
void
kfree(void *pa)
{
  struct run *r;
  int index;
  int refs;

  if(((uint64)pa % PGSIZE) != 0 ||
     (char *)pa < end ||
     (uint64)pa >= PHYSTOP)
    panic("kfree");

  index = PA2INDEX(pa);

  acquire(&kref.lock);

  if(kref.count[index] <= 0)
    panic("kfree count");

  kref.count[index]--;
  refs = kref.count[index];

  release(&kref.lock);

  /*
   * 仍然有其他页表引用这个页面。
   * 此时不能放入 freelist。
   */
  if(refs > 0)
    return;

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

/*
 * 分配一个 4096 字节物理页。
 */
void *
kalloc(void)
{
  struct run *r;
  int index;

  acquire(&kmem.lock);

  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;

  release(&kmem.lock);

  if(r){
    memset((char *)r, 5, PGSIZE);

    index = PA2INDEX(r);

    acquire(&kref.lock);

    /*
     * 从 freelist 中取出的页面，引用计数必须为 0。
     */
    if(kref.count[index] != 0)
      panic("kalloc count");

    /*
     * 新分配出的页面暂时只有调用者持有，
     * 因此引用计数为 1。
     */
    kref.count[index] = 1;

    release(&kref.lock);
  }

  return (void *)r;
}