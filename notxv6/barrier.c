#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>

static int nthread = 1;
static int round = 0;

struct barrier {
  pthread_mutex_t barrier_mutex;
  pthread_cond_t barrier_cond;
  int nthread;      // Number of threads that have reached this round of the barrier
  int round;     // Barrier round
} bstate;

static void
barrier_init(void)
{
  assert(pthread_mutex_init(&bstate.barrier_mutex, NULL) == 0);
  assert(pthread_cond_init(&bstate.barrier_cond, NULL) == 0);
  bstate.nthread = 0;
}

static void
barrier()
{
  /*
   * bstate.nthread 和 bstate.round 是所有线程共享的数据，
   * 在读取和修改它们之前必须先获得锁。
   */
  pthread_mutex_lock(&bstate.barrier_mutex);

  /*
   * 记录当前线程进入屏障时所在的轮次。
   *
   * 后面等待时，通过比较这个局部变量和 bstate.round，
   * 判断当前这一轮是否已经结束。
   */
  int round = bstate.round;

  /*
   * 当前线程已经到达这一轮屏障。
   */
  bstate.nthread++;

  if(bstate.nthread == nthread){
    /*
     * 当前线程是最后一个到达屏障的线程。
     *
     * 所有线程已经到齐，可以结束当前轮。
     */

    /*
     * 为下一轮重新计数。
     */
    bstate.nthread = 0;

    /*
     * 进入下一轮。
     *
     * 等待线程会通过 round 的变化判断当前轮已经结束。
     */
    bstate.round++;

    /*
     * 唤醒所有正在 barrier_cond 上等待的线程。
     */
    pthread_cond_broadcast(&bstate.barrier_cond);
  } else {
    /*
     * 当前还不是最后一个到达的线程，
     * 必须等待当前轮结束。
     */
    while(round == bstate.round){
      pthread_cond_wait(
        &bstate.barrier_cond,
        &bstate.barrier_mutex
      );
    }
  }

  /*
   * 离开 barrier() 前释放互斥锁。
   */
  pthread_mutex_unlock(&bstate.barrier_mutex);
}

static void *
thread(void *xa)
{
  long n = (long) xa;
  long delay;
  int i;

  for (i = 0; i < 20000; i++) {
    int t = bstate.round;
    assert (i == t);
    barrier();
    usleep(random() % 100);
  }

  return 0;
}

int
main(int argc, char *argv[])
{
  pthread_t *tha;
  void *value;
  long i;
  double t1, t0;

  if (argc < 2) {
    fprintf(stderr, "%s: %s nthread\n", argv[0], argv[0]);
    exit(-1);
  }
  nthread = atoi(argv[1]);
  tha = malloc(sizeof(pthread_t) * nthread);
  srandom(0);

  barrier_init();

  for(i = 0; i < nthread; i++) {
    assert(pthread_create(&tha[i], NULL, thread, (void *) i) == 0);
  }
  for(i = 0; i < nthread; i++) {
    assert(pthread_join(tha[i], &value) == 0);
  }
  printf("OK; passed\n");
}
