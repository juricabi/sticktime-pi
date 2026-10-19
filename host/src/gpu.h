/* drawing = gpu, the host's side: starts the helper program sticktime-gpu (gpu_main.c, protocol
 * gpu_proto.h) and has it draw each frame's recorded calls into the display's buffers. Linux only
 * (elsewhere gpu_open fails). The frame loop never waits for the helper to start: until it says
 * READY, gpu_poll() answers 0 and the frame is drawn on the CPU. */
#ifndef GPU_H
#define GPU_H

#include <stdbool.h>
#include <stdint.h>
#include "lcd_rec.h"

typedef struct gpu gpu_t;

/* start helper (a path); NULL with the reason in err when it can't even start */
gpu_t *gpu_open(const char *helper, char *err, int errlen);
void gpu_close(gpu_t *g);
/* 1 ready to draw, 0 still starting, -1 failed or gone (gpu_info says why) */
int  gpu_poll(gpu_t *g);
const char *gpu_info(const gpu_t *g);       /* the GL renderer, or why the GPU isn't drawing */

/* where frames go: the display's nbuf buffers (DRM PRIME fds, the caller keeps its own: sent, not kept),
 * w x h of pitch bytes and fourcc; the calls draw on rw x rh; rows 0: the frame as it is, 1 or 2:
 * interlaced out through the flicker filter (pad, strength), as video_deflicker_lines. 0 or -1. */
int  gpu_set_target(gpu_t *g, const int *fds, int nbuf, int w, int h, int pitch, uint32_t fourcc, int rw, int rh,
                    int rows, int pad, int strength, uint32_t id);
uint32_t gpu_target_id(const gpu_t *g);     /* the id of the target set (0: none yet) */
/* draw calls c[0 .. n - 1] into buffer buf and wait until it is done; shot (rw x rh RGB565) or NULL:
 * a copy of the frame. 0, or -1 (the GPU is given up: gpu_poll says -1 from now on). */
int  gpu_draw(gpu_t *g, const rcmd_t *c, int n, int buf, uint16_t *shot);
float gpu_last_ms(const gpu_t *g);           /* the helper's time for the last frame */

#endif
