/* A small pool of threads for one job at a time: par_run() splits the job into parts, which the pool's
 * threads and the calling thread take one by one, and returns when every part is done. The frame loop
 * draws a frame with it (drawing = multi): the threads run just below the audio thread, on the cores
 * neither the frame loop nor the audio thread is pinned to. One caller at a time. */
#ifndef PAR_H
#define PAR_H

typedef struct par par_t;
typedef void (*par_fn)(void *ctx, int part);

/* threads: how many take part in a job, the caller included (<= 0: par_plan's: 3 on a Pi 4). NULL when
 * no thread could start (or threads is 1): par_run() then runs every part in the caller. */
par_t *par_open(int threads);
void par_close(par_t *p);
int  par_threads(const par_t *p);                          /* the caller included; 1 for NULL */
void par_run(par_t *p, int parts, par_fn fn, void *ctx);   /* parts 0 .. parts - 1, any order */

/* The plan for ncpu CPUs with the caller pinned to CPU pinned (-1: not pinned): the CPUs the pool's
 * threads may use (bit i: CPU i) and *threads, how many take part in a job with the caller. The audio
 * thread is on the CPU before the caller's (audio.c); the threads keep off both, and off the caller's
 * only when that leaves none (2 CPUs: they run below the audio thread). On a Pi 4 (and 3, Zero 2, 5)
 * the frame loop is on core 3, audio on 2, the threads on 0 and 1: 3 in all. A 4th among them draws a
 * frame only a little sooner (the memory is the limit by then) and would hold the audio thread up. */
unsigned long long par_plan(int ncpu, int pinned, int *threads);

#endif
