#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <sys/time.h>

#define NBUCKET 5
#define NKEYS 100000

struct entry {
  int key;
  int value;
  struct entry *next;
};
struct entry *table[NBUCKET];
int keys[NKEYS];
int nthread = 1;
pthread_mutex_t locks[NBUCKET];

double
now()
{
 struct timeval tv;
 gettimeofday(&tv, 0);
 return tv.tv_sec + tv.tv_usec / 1000000.0;
}

static void 
insert(int key, int value, struct entry **p, struct entry *n)
{
  struct entry *e = malloc(sizeof(struct entry));
  e->key = key;
  e->value = value;
  e->next = n;
  *p = e;
}

static
void
put(int key, int value)
{
  /*
   * 计算 key 属于哪个哈希桶。
   */
  int i = key % NBUCKET;

  /*
   * 只锁住当前 key 对应的桶。
   *
   * 同一个桶的链表查找和修改不能并发进行；
   * 不同桶仍然可以被其他线程同时访问。
   */
  pthread_mutex_lock(&locks[i]);

  /*
   * 检查 key 是否已经存在。
   */
  struct entry *e = 0;

  for(e = table[i]; e != 0; e = e->next){
    if(e->key == key)
      break;
  }

  if(e){
    /*
     * key 已存在，更新 value。
     */
    e->value = value;
  } else {
    /*
     * key 不存在，在当前桶的链表头插入新节点。
     */
    insert(key, value, &table[i], table[i]);
  }

  /*
   * 当前桶操作完成，释放锁。
   */
  pthread_mutex_unlock(&locks[i]);
}

static struct entry*
get(int key)
{
  /*
   * 计算 key 所属的桶。
   */
  int i = key % NBUCKET;

  /*
   * 锁住当前桶，避免读取链表时，
   * 另一个线程同时修改该桶的链表结构。
   */
  pthread_mutex_lock(&locks[i]);

  struct entry *e = 0;

  for(e = table[i]; e != 0; e = e->next){
    if(e->key == key)
      break;
  }

  pthread_mutex_unlock(&locks[i]);

  return e;
}

static void *
put_thread(void *xa)
{
  int n = (int) (long) xa; // thread number
  int b = NKEYS/nthread;

  for (int i = 0; i < b; i++) {
    put(keys[b*n + i], n);
  }

  return NULL;
}

static void *
get_thread(void *xa)
{
  int n = (int) (long) xa; // thread number
  int missing = 0;

  for (int i = 0; i < NKEYS; i++) {
    struct entry *e = get(keys[i]);
    if (e == 0) missing++;
  }
  printf("%d: %d keys missing\n", n, missing);
  return NULL;
}

int
main(int argc, char *argv[])
{
  pthread_t *tha;
  void *value;
  double t1, t0;

  if (argc < 2) {
    fprintf(stderr, "Usage: %s nthreads\n", argv[0]);
    exit(-1);
  }
  nthread = atoi(argv[1]);

  /*
   * 初始化每一个哈希桶对应的锁。
   */
  for(int i = 0; i < NBUCKET; i++){
    pthread_mutex_init(&locks[i], NULL);
  }
  
  tha = malloc(sizeof(pthread_t) * nthread);
  srandom(0);
  assert(NKEYS % nthread == 0);
  for (int i = 0; i < NKEYS; i++) {
    keys[i] = random();
  }

  //
  // first the puts
  //
  t0 = now();
  for(int i = 0; i < nthread; i++) {
    assert(pthread_create(&tha[i], NULL, put_thread, (void *) (long) i) == 0);
  }
  for(int i = 0; i < nthread; i++) {
    assert(pthread_join(tha[i], &value) == 0);
  }
  t1 = now();

  printf("%d puts, %.3f seconds, %.0f puts/second\n",
         NKEYS, t1 - t0, NKEYS / (t1 - t0));

  //
  // now the gets
  //
  t0 = now();
  for(int i = 0; i < nthread; i++) {
    assert(pthread_create(&tha[i], NULL, get_thread, (void *) (long) i) == 0);
  }
  for(int i = 0; i < nthread; i++) {
    assert(pthread_join(tha[i], &value) == 0);
  }
  t1 = now();

  printf("%d gets, %.3f seconds, %.0f gets/second\n",
         NKEYS*nthread, t1 - t0, (NKEYS*nthread) / (t1 - t0));
}
