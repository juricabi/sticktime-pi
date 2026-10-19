/* A small pool of threads for one job at a time: par_run() splits the job into parts, which the pool's
 * threads and the calling thread take one by one, and returns when every part is done. The frame loop
 * draws a frame with it (drawing = multi): the threads run just below the audio thread, on the cores
 * neither the frame loop nor the audio thread is pinned to. One caller at a time. */
#ifndef PAR_H
#define PAR_H

typedef struct par par_t;
typedef void (*par_fn)(void *ctx, int part);

/* threads: how many take part in a job, the caller included (<= 0: one per CPU the pool may use and
 * the caller: 3 on a Pi 4). NULL when no thread could start (or threads is 1): par_run() then runs
 * every part in the caller. */
par_t *par_open(int threads);
void par_close(par_t *p);
int  par_threads(const par_t *p);                          /* the caller included; 1 for NULL */
void par_run(par_t *p, int parts, par_fn fn, void *ctx);   /* parts 0 .. parts - 1, any order */

#endif
