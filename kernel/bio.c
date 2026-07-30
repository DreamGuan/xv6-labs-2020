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


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

#define NBUCKET 13

/*
 * ticks 定义在 kernel/trap.c。
 * 用于记录缓存块最近一次释放的时间。
 */
extern uint ticks;

/*
 * 一个哈希桶。
 *
 * 每个桶有：
 * 1. 自己的自旋锁；
 * 2. 一条双向循环链表。
 */
struct bucket {
  struct spinlock lock;
  struct buf head;
};

struct {
  /*
   * 这把全局锁不再保护普通查找和释放。
   *
   * 它只用于串行化缓存未命中后的替换操作，
   * 防止两个 CPU 同时为同一个磁盘块创建两个缓存副本。
   */
  struct spinlock lock;

  struct buf buf[NBUF];

  /*
   * 哈希表：每个桶分别加锁。
   */
  struct bucket bucket[NBUCKET];
} bcache;

/*
 * 根据磁盘块号选择哈希桶。
 *
 * 不同设备上的相同 blockno 会进入同一个桶，
 * 但查找时仍然同时比较 dev 和 blockno。
 */
static int
bhash(uint blockno)
{
  return blockno % NBUCKET;
}

/*
 * 从当前所在的桶链表中删除 b。
 *
 * 调用者必须已经持有该桶的锁。
 */
static void
bremove(struct buf *b)
{
  b->prev->next = b->next;
  b->next->prev = b->prev;
}

/*
 * 把 b 插入某个桶链表的头部。
 *
 * 调用者必须已经持有该桶的锁，
 * 或者当前仍处于单线程初始化阶段。
 */
static void
binsert(struct buf *head, struct buf *b)
{
  b->next = head->next;
  b->prev = head;

  head->next->prev = b;
  head->next = b;
}

void
binit(void)
{
  struct buf *b;
  int i;

  /*
   * 全局锁只负责缓存替换。
   */
  initlock(&bcache.lock, "bcache");

  /*
   * 初始化所有哈希桶。
   *
   * 每个桶使用一个带哨兵节点的双向循环链表：
   *
   * head <-> buf1 <-> buf2 <-> head
   */
  for(i = 0; i < NBUCKET; i++){
    initlock(&bcache.bucket[i].lock, "bcache.bucket");

    bcache.bucket[i].head.prev = &bcache.bucket[i].head;
    bcache.bucket[i].head.next = &bcache.bucket[i].head;
  }

  /*
   * 初始化全部缓存块。
   *
   * 初始缓存块没有对应有效设备，因此 dev 设置为 -1。
   * 所有空缓存块暂时放进 bucket[0]。
   */
  for(b = bcache.buf; b < bcache.buf + NBUF; b++){
    initsleeplock(&b->lock, "buffer");

    b->valid = 0;
    b->disk = 0;
    b->dev = (uint)-1;
    b->blockno = 0;
    b->refcnt = 0;
    b->timestamp = 0;

    binsert(&bcache.bucket[0].head, b);
  }
}

/*
 * 查找指定磁盘块的缓存。
 *
 * 如果已经缓存：
 *   增加 refcnt，获取该 buffer 的睡眠锁并返回。
 *
 * 如果没有缓存：
 *   找一个 refcnt == 0 的缓存块重新使用。
 */
static struct buf *
bget(uint dev, uint blockno)
{
  struct buf *b;
  struct buf *victim;
  uint oldest;
  int index;
  int old_index;
  int i;

  /*
   * 第一步：只锁目标哈希桶，查找缓存。
   *
   * 这是最常见的命中路径。
   * 不需要获取全局 bcache.lock。
   */
  index = bhash(blockno);

  acquire(&bcache.bucket[index].lock);

  for(b = bcache.bucket[index].head.next;
      b != &bcache.bucket[index].head;
      b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;

      release(&bcache.bucket[index].lock);

      /*
       * 不能持有自旋锁时获取睡眠锁。
       */
      acquiresleep(&b->lock);

      return b;
    }
  }

  release(&bcache.bucket[index].lock);

  /*
   * 第二步：缓存未命中。
   *
   * 使用全局 bcache.lock 串行化缓存替换。
   *
   * 这样同一时刻只有一个 CPU 能执行：
   *   查找空闲缓存块
   *   修改 dev/blockno
   *   把缓存块移入新桶
   */
  acquire(&bcache.lock);

  /*
   * 必须重新检查目标缓存块。
   *
   * 在第一次检查结束到获得 bcache.lock 之间，
   * 另一个 CPU 可能已经为该磁盘块创建了缓存。
   *
   * 不重新检查就可能产生两个相同块的缓存副本。
   */
  acquire(&bcache.bucket[index].lock);

  for(b = bcache.bucket[index].head.next;
      b != &bcache.bucket[index].head;
      b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;

      release(&bcache.bucket[index].lock);
      release(&bcache.lock);

      acquiresleep(&b->lock);

      return b;
    }
  }

  release(&bcache.bucket[index].lock);

  /*
   * 第三步：寻找一个 refcnt == 0 的缓存块。
   *
   * timestamp 越小，说明越久没有被使用。
   */
  for(;;){
    victim = 0;
    old_index = -1;
    oldest = 0xffffffff;

    /*
     * 依次扫描所有桶。
     *
     * 每次只持有一把桶锁，避免多个桶锁之间形成死锁。
     */
    for(i = 0; i < NBUCKET; i++){
      acquire(&bcache.bucket[i].lock);

      for(b = bcache.bucket[i].head.next;
          b != &bcache.bucket[i].head;
          b = b->next){
        if(b->refcnt == 0 &&
           (victim == 0 || b->timestamp < oldest)){
          victim = b;
          oldest = b->timestamp;
          old_index = i;
        }
      }

      release(&bcache.bucket[i].lock);
    }

    if(victim == 0){
      release(&bcache.lock);
      panic("bget: no buffers");
    }

    /*
     * 扫描完成后，victim 可能刚刚被另一个 CPU 使用。
     *
     * 因此重新获取其旧桶锁，再检查一次 refcnt。
     */
    acquire(&bcache.bucket[old_index].lock);

    if(victim->refcnt != 0){
      /*
       * victim 已被其他 CPU 使用。
       * 重新扫描并选择另一个缓存块。
       */
      release(&bcache.bucket[old_index].lock);
      continue;
    }

    /*
     * 仍然空闲，可以从旧桶链表中移除。
     */
    bremove(victim);

    release(&bcache.bucket[old_index].lock);

    break;
  }

  /*
   * victim 已经不属于任何桶。
   *
   * 它的 refcnt 为 0，并且全局替换锁仍然被持有，
   * 所以现在修改身份是安全的。
   */
  victim->dev = dev;
  victim->blockno = blockno;
  victim->valid = 0;
  victim->disk = 0;
  victim->refcnt = 1;
  victim->timestamp = ticks;

  /*
   * 把 victim 插入目标磁盘块对应的哈希桶。
   */
  acquire(&bcache.bucket[index].lock);

  binsert(&bcache.bucket[index].head, victim);

  release(&bcache.bucket[index].lock);

  /*
   * 缓存块已经完整加入哈希表，
   * 此时才能结束缓存替换操作。
   */
  release(&bcache.lock);

  /*
   * 所有自旋锁都释放后，再获取 buffer 睡眠锁。
   */
  acquiresleep(&victim->lock);

  return victim;
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

/*
 * 释放一个已经持有睡眠锁的缓存块。
 */
void
brelse(struct buf *b)
{
  int index;

  if(!holdingsleep(&b->lock))
    panic("brelse");

  /*
   * 当前仍然持有 b->lock，
   * 所以 b->blockno 不会被修改。
   */
  index = bhash(b->blockno);

  /*
   * 先释放缓存块的睡眠锁。
   */
  releasesleep(&b->lock);

  /*
   * refcnt 由该缓存块所在桶的锁保护。
   */
  acquire(&bcache.bucket[index].lock);

  if(b->refcnt < 1){
    release(&bcache.bucket[index].lock);
    panic("brelse: refcnt");
  }

  b->refcnt--;

  /*
   * refcnt 变成 0，说明当前缓存块已经没有使用者。
   * 记录它最后一次被使用的时间。
   */
  if(b->refcnt == 0)
    b->timestamp = ticks;

  release(&bcache.bucket[index].lock);
}

void
bpin(struct buf *b)
{
  int index;

  index = bhash(b->blockno);

  acquire(&bcache.bucket[index].lock);

  b->refcnt++;

  release(&bcache.bucket[index].lock);
}

void
bunpin(struct buf *b)
{
  int index;

  index = bhash(b->blockno);

  acquire(&bcache.bucket[index].lock);

  if(b->refcnt < 1){
    release(&bcache.bucket[index].lock);
    panic("bunpin: refcnt");
  }

  b->refcnt--;

  /*
   * 日志系统可能通过 bpin() 保留缓存块。
   *
   * 当 bunpin() 使 refcnt 变成 0 时，
   * 也应该刷新最后使用时间。
   */
  if(b->refcnt == 0)
    b->timestamp = ticks;

  release(&bcache.bucket[index].lock);
}


