/* drawing = gpu: a frame's recorded calls (lcd_rec.h) as triangles for the GPU helper's one shader.
 * Coordinates are render pixels (0, 0 the top left corner of the top left pixel); u < 0 is a plain
 * color, else u, v are texels of the glyph atlas whose alpha the color takes. The GPU's coverage
 * rules are not Bresenham's or Adafruit's: edges may differ by a pixel from drawing = single/multi.
 * Inverting rects need a blend of their own, so they come in batches of their own. */
#ifndef GPU_GEOM_H
#define GPU_GEOM_H

#include <stdbool.h>
#include <stdint.h>
#include "lcd_rec.h"

typedef struct { float x, y, u, v; uint8_t r, g, b, a; } gvert_t;
typedef struct { int first, count; bool invert; } gbatch_t;
typedef struct {
  gvert_t *v;
  int nv, capv;
  gbatch_t *b;
  int nb, capb;
} gmesh_t;

/* the calls c[0 .. n - 1] on a w x h screen, replacing what m held; 0, or -1 when out of memory */
int  gmesh_build(gmesh_t *m, const rcmd_t *c, int n, int w, int h);
void gmesh_free(gmesh_t *m);

#endif
