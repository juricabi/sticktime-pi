/* drawing = gpu: the host (gpu.c) and the GPU helper (gpu_main.c, the program sticktime-gpu) talk over
 * one UNIX seqpacket socket, a gpu_msg_t per message, and share one memory block (GPU_SHM_*).
 *
 *   host -> helper   HELLO (the shared memory is fd 4, the socket fd 3, from the start)
 *   helper -> host   READY (text: the GL renderer) or ERROR (text: why)
 *   host -> helper   TARGET + the display buffers as dma-buf fds (SCM_RIGHTS): where frames go
 *   helper -> host   DONE or ERROR
 *   host -> helper   FRAME: ncmd recorded calls (lcd_rec.h) at GPU_SHM_CMD, their glyph masks in the
 *                    atlas at GPU_SHM_ATLAS (a glyph's g.a = atlas x | y << 16); drawn into buffer buf
 *   helper -> host   DONE (gpu_ms) when the buffer holds the frame, or ERROR
 *   host -> helper   QUIT (or the socket closes) */
#ifndef GPU_PROTO_H
#define GPU_PROTO_H

#include <stdint.h>

#define GPU_PROTO 1
#define GPU_FD_SOCK 3
#define GPU_FD_SHM 4
#define GPU_MAX_BUF 4

enum { GM_HELLO = 1, GM_READY, GM_ERROR, GM_TARGET, GM_FRAME, GM_DONE, GM_QUIT };

typedef struct {
  uint32_t type, proto;
  uint32_t frame;                  /* FRAME, DONE: a counter */
  int32_t buf;                     /* FRAME: the target buffer */
  /* TARGET */
  int32_t nbuf, w, h, pitch;       /* the buffers (dma-bufs, in the order sent): w x h, pitch bytes per row */
  uint32_t fourcc;                 /* DRM_FORMAT_RGB565 or DRM_FORMAT_XRGB8888 */
  int32_t rw, rh;                  /* the render size: what the calls draw on */
  int32_t rows;                    /* 0: the frame as it is (w, h = rw, rh); 1 or 2: interlaced out, h lines of
                                    * rows render lines each, through the flicker filter (video_deflicker_lines) */
  int32_t pad, strength;           /* the filter: a line more above and below; 0 none, 1 light, 2 full */
  /* FRAME */
  uint32_t ncmd;
  int32_t atlas_h;                 /* the atlas rows in use */
  uint32_t readback;               /* 1: the frame as RGB565 (rw x rh) into GPU_SHM_READ as well */
  /* DONE */
  float gpu_ms;
  char text[160];                  /* READY: the GL renderer; ERROR: why */
} gpu_msg_t;

#define GPU_SHM_CMD 0u
#define GPU_SHM_CMD_MAX (6u << 20)
#define GPU_ATLAS_W 1024
#define GPU_ATLAS_MAXH 2048
#define GPU_SHM_ATLAS GPU_SHM_CMD_MAX
#define GPU_SHM_READ (GPU_SHM_ATLAS + (uint32_t)GPU_ATLAS_W * GPU_ATLAS_MAXH)
#define GPU_SHM_READ_MAX (2048u * 2048u * 2u)
#define GPU_SHM_SIZE (GPU_SHM_READ + GPU_SHM_READ_MAX)

#endif
