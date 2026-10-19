/* par: every part of every job runs exactly once, on the pool and the caller, job after job */
#include "t.h"
#include "par.h"

#include <string.h>

typedef struct {
  int runs[64];
  int job;
  int wrong_job;                   /* a part run with another job's context */
} ctx_t;

static void count_part(void *p, int part) {
  ctx_t *c = p;
  __atomic_add_fetch(&c->runs[part], 1, __ATOMIC_RELAXED);
}

static void spin_part(void *p, int part) {
  ctx_t *c = p;
  volatile int x = 0;
  for (int i = 0; i < (part % 7) * 2000; i++) x += i;
  __atomic_add_fetch(&c->runs[part], 1, __ATOMIC_RELAXED);
}

static void check_job(void *p, int part) {
  ctx_t *c = p;
  (void)part;
  if (__atomic_load_n(&c->job, __ATOMIC_ACQUIRE) == 0) __atomic_add_fetch(&c->wrong_job, 1, __ATOMIC_RELAXED);
}

int main(void) {
  /* no pool: the caller runs every part */
  CHECK(par_open(1) == NULL);
  CHECK_INT(par_threads(NULL), 1);
  ctx_t c;
  memset(&c, 0, sizeof c);
  par_run(NULL, 5, count_part, &c);
  for (int i = 0; i < 5; i++) CHECK_INT(c.runs[i], 1);
  par_run(NULL, 0, count_part, &c);
  par_close(NULL);

  par_t *p = par_open(4);
  CHECK(p != NULL);
  if (!p) T_DONE();
  CHECK_INT(par_threads(p), 4);
  int bad = 0;
  for (int job = 0; job < 3000; job++) {
    int parts = 1 + job % 64;
    memset(&c, 0, sizeof c);
    par_run(p, parts, job % 3 ? count_part : spin_part, &c);
    for (int i = 0; i < 64; i++) bad += c.runs[i] != (i < parts);
  }
  CHECK_INT(bad, 0);
  /* a job's parts never run with the next job's context: each job's context is cleared when it ends */
  ctx_t jobs[2];
  memset(jobs, 0, sizeof jobs);
  for (int job = 0; job < 3000; job++) {
    ctx_t *j = &jobs[job & 1];
    __atomic_store_n(&j->job, 1, __ATOMIC_RELEASE);
    par_run(p, 1 + job % 9, check_job, j);
    __atomic_store_n(&j->job, 0, __ATOMIC_RELEASE);
  }
  CHECK_INT(jobs[0].wrong_job + jobs[1].wrong_job, 0);
  par_close(p);
  /* one per CPU */
  p = par_open(0);
  CHECK(p == NULL || par_threads(p) >= 2);
  par_close(p);
  T_DONE();
}
