struct buf {
  int valid;
  int disk;
  uint dev;
  uint blockno;
  struct sleeplock lock;
  uint refcnt;

  // 记录该缓存块最近一次不再被使用的时间。
  // bget() 缓存未命中时，用它寻找最久未使用的缓存块。
  uint timestamp;

  struct buf *prev;
  struct buf *next;
  uchar data[BSIZE];
};