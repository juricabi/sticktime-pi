/* Thread pool for one job at a time: see par.h. */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "par.h"
#include "plat.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#endif

#define PAR_MAX 16

struct par {
  plat_mutex mu;
  plat_cond go, done;
  plat_thread th[PAR_MAX];
  int nth;                         /* threads started (the caller not counted) */
  bool quit;
  unsigned gen;                    /* bumped for every job */
  par_fn fn;                       /* the job: set under mu before gen moves */
  void *ctx;
  int parts;
  int next;                        /* the next part to take (atomic) */
  int left;                        /* parts not done yet (atomic) */
  int inside;                      /* threads working on the job (under mu): par_run waits for them to
                                    * leave, so none takes a part of the next job with this one's fn */
#ifdef __linux__
  int policy, prio;
  cpu_set_t cpus;
  bool pin;
#endif
};

static void take_parts(par_t *p, par_fn fn, void *ctx, int parts) {
  for (;;) {
    int i = __atomic_fetch_add(&p->next, 1, __ATOMIC_ACQ_REL);
    if (i >= parts) return;
    fn(ctx, i);
    __atomic_sub_fetch(&p->left, 1, __ATOMIC_ACQ_REL);
  }
}

static void *worker(void *arg) {
  par_t *p = arg;
#ifdef __linux__
  /* as the creator set it up (pthread_attr may not have been allowed to) */
  struct sched_param sp;
  memset(&sp, 0, sizeof sp);
  sp.sched_priority = p->policy == SCHED_FIFO ? p->prio : 0;
  pthread_setschedparam(pthread_self(), p->policy, &sp);
  if (p->pin) pthread_setaffinity_np(pthread_self(), sizeof p->cpus, &p->cpus);
#else
  plat_thread_lower_priority();
#endif
  unsigned seen = 0;
  plat_mutex_lock(&p->mu);
  seen = p->gen;
  for (;;) {
    while (!p->quit && p->gen == seen) plat_cond_wait(&p->go, &p->mu, -1);
    if (p->quit) break;
    seen = p->gen;
    par_fn fn = p->fn;
    void *ctx = p->ctx;
    int parts = p->parts;
    p->inside++;
    plat_mutex_unlock(&p->mu);
    take_parts(p, fn, ctx, parts);
    plat_mutex_lock(&p->mu);
    p->inside--;
    plat_cond_broadcast(&p->done);
  }
  plat_mutex_unlock(&p->mu);
  return NULL;
}

#ifdef __linux__
/* the CPUs online (see audio.c: musl's sysconf counts only the caller's affinity mask) */
static int cpus_online(void) {
  char b[64];
  int fd = open("/sys/devices/system/cpu/online", O_RDONLY | O_CLOEXEC);
  ssize_t r = fd >= 0 ? read(fd, b, sizeof b - 1) : -1;
  if (fd >= 0) close(fd);
  long n = 0;
  if (r > 0) {
    b[r] = 0;
    for (char *s = b; *s && *s != '\n';) {
      char *e;
      long lo = strtol(s, &e, 10), hi = lo;
      if (e == s) break;
      if (*e == '-') hi = strtol(e + 1, &e, 10);
      n = hi + 1 > n ? hi + 1 : n;
      s = *e == ',' ? e + 1 : e;
    }
  }
  if (n <= 0) n = sysconf(_SC_NPROCESSORS_CONF);
  return n > 0 ? (n < CPU_SETSIZE ? (int)n : CPU_SETSIZE) : 1;
}

/* below the caller's SCHED_FIFO priority by 2 (the audio thread is 1 below: it goes first), on every
 * CPU but the one the caller is pinned to */
static void pick_sched(par_t *p, int ncpu) {
  int pol = SCHED_OTHER;
  struct sched_param sp;
  memset(&sp, 0, sizeof sp);
  p->policy = SCHED_OTHER;
  p->prio = 0;
  if (pthread_getschedparam(pthread_self(), &pol, &sp) == 0 && (pol == SCHED_FIFO || pol == SCHED_RR) &&
      sp.sched_priority > 2) {
    p->policy = SCHED_FIFO;
    p->prio = sp.sched_priority - 2;
  }
  cpu_set_t cur;
  CPU_ZERO(&cur);
  int pinned = -1;
  if (sched_getaffinity(0, sizeof cur, &cur) == 0 && CPU_COUNT(&cur) == 1)
    for (int i = 0; i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &cur)) { pinned = i; break; }
  CPU_ZERO(&p->cpus);
  for (int i = 0; i < ncpu; i++) if (i != pinned || ncpu < 2) CPU_SET(i, &p->cpus);
  p->pin = true;
}
#endif

par_t *par_open(int threads) {
#ifdef __linux__
  int ncpu = cpus_online();
#else
  int ncpu = 4;
#endif
  if (threads <= 0) threads = ncpu;
  if (threads > PAR_MAX + 1) threads = PAR_MAX + 1;
  if (threads <= 1) return NULL;
  par_t *p = calloc(1, sizeof *p);
  if (!p) return NULL;
  plat_mutex_init(&p->mu);
  plat_cond_init(&p->go);
  plat_cond_init(&p->done);
#ifdef __linux__
  pick_sched(p, ncpu);
#endif
  for (int i = 0; i < threads - 1; i++) {
#ifdef __linux__
    /* started with its own policy, not the caller's (a SCHED_FIFO caller pinned to one core would
     * keep an inherited thread from running until it blocks), and moved to its cores at once */
    pthread_attr_t at;
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = p->policy == SCHED_FIFO ? p->prio : 0;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 256 * 1024);
    pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&at, p->policy);
    pthread_attr_setschedparam(&at, &sp);
    int r = pthread_create(&p->th[p->nth], &at, worker, p);
    if (r == EPERM || r == EINVAL) {
      pthread_attr_setinheritsched(&at, PTHREAD_INHERIT_SCHED);
      r = pthread_create(&p->th[p->nth], &at, worker, p);
    }
    pthread_attr_destroy(&at);
    if (r) break;
    pthread_setaffinity_np(p->th[p->nth], sizeof p->cpus, &p->cpus);
#else
    if (plat_thread_start(&p->th[p->nth], worker, p, false)) break;
#endif
    p->nth++;
  }
  if (!p->nth) {
    par_close(p);
    return NULL;
  }
  return p;
}

void par_close(par_t *p) {
  if (!p) return;
  plat_mutex_lock(&p->mu);
  p->quit = true;
  plat_cond_broadcast(&p->go);
  plat_mutex_unlock(&p->mu);
  for (int i = 0; i < p->nth; i++) plat_thread_join(p->th[i]);
  plat_mutex_destroy(&p->mu);
  free(p);
}

int par_threads(const par_t *p) { return p ? p->nth + 1 : 1; }

void par_run(par_t *p, int parts, par_fn fn, void *ctx) {
  if (parts <= 0) return;
  if (!p || parts == 1) {
    for (int i = 0; i < parts; i++) fn(ctx, i);
    return;
  }
  plat_mutex_lock(&p->mu);
  /* a thread that woke too late for the last job may still be on its way out: it must not take a
   * part of this one with that job's fn */
  while (p->inside > 0) plat_cond_wait(&p->done, &p->mu, -1);
  p->fn = fn;
  p->ctx = ctx;
  p->parts = parts;
  __atomic_store_n(&p->next, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&p->left, parts, __ATOMIC_RELEASE);
  p->gen++;
  plat_cond_broadcast(&p->go);
  plat_mutex_unlock(&p->mu);
  take_parts(p, fn, ctx, parts);
  /* the last parts finish on other threads within microseconds: look a little before sleeping */
  int64_t until = plat_now_us() + 200;
  while (__atomic_load_n(&p->left, __ATOMIC_ACQUIRE) > 0 && plat_now_us() < until) {}
  plat_mutex_lock(&p->mu);
  while (__atomic_load_n(&p->left, __ATOMIC_ACQUIRE) > 0 || p->inside > 0) plat_cond_wait(&p->done, &p->mu, -1);
  plat_mutex_unlock(&p->mu);
}
