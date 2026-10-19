/* plat: worker threads. On Linux a thread inherits its creator's CPU mask, so a worker started by the
 * frame loop (pinned to its core) would share that one core; plat_thread_lower_priority() lets it run
 * on every core the process may use. (Its SCHED_OTHER half needs a real-time creator: root only.) */
#ifdef __linux__
#define _GNU_SOURCE
#endif
#include "t.h"
#include "plat.h"

#ifdef __linux__
#include <pthread.h>
#include <sched.h>

static cpu_set_t seen;

static void *worker(void *arg) {
  (void)arg;
  plat_thread_lower_priority();
  CPU_ZERO(&seen);
  pthread_getaffinity_np(pthread_self(), sizeof seen, &seen);
  return NULL;
}

static void test_worker_unpinned(void) {
  cpu_set_t all, one, both;
  CHECK_INT(pthread_getaffinity_np(pthread_self(), sizeof all, &all), 0);
  if (CPU_COUNT(&all) < 2) { printf("one CPU: worker pinning not tested\n"); return; }
  CPU_ZERO(&one);
  for (int i = 0; i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &all)) { CPU_SET(i, &one); break; }
  CHECK_INT(pthread_setaffinity_np(pthread_self(), sizeof one, &one), 0);   /* like the frame loop */
  plat_thread t;
  CHECK_INT(plat_thread_start(&t, worker, NULL, false), 0);
  plat_thread_join(t);
  pthread_setaffinity_np(pthread_self(), sizeof all, &all);
  CPU_AND(&both, &seen, &all);
  CHECK(CPU_EQUAL(&both, &all));                                            /* every core, not one */
}
#endif

int main(void) {
#ifdef __linux__
  test_worker_unpinned();
#endif
  T_DONE();
}
