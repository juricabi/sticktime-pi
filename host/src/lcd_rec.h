/* The recorded drawing calls of a frame (lcd.c, drawing = multi or gpu): what each needs to draw, after
 * the work that counts it (clipping a rect, the ends of a line, a glyph's place). The GPU helper
 * (gpu_main.c) draws the same records, so this layout is shared by both programs. */
#ifndef LCD_REC_H
#define LCD_REC_H

#include <stdint.h>
#include "font.h"

enum { RC_CLEAR, RC_HLINE, RC_RECT, RC_LINE, RC_TRI, RC_POINT, RC_FCIRCLE, RC_CIRCLE, RC_INVERT, RC_GLYPH };

typedef struct {
  uint8_t op, opa, pat;            /* opa 0..15 (0 opaque), pat: a line's 8-bit pattern */
  uint8_t w;                       /* lines, points, circles: pixels wide (lcd_t.line_w) */
  uint16_t c;                      /* RGB565 */
  int ylo, yhi;                    /* the rows it can touch (ylo > yhi: none) */
  union {
    struct { int64_t x, y, w, h; } r;             /* hline (h unused), rect (clipped to the screen) */
    struct { int x0, y0, x1, y1, x2, y2; } p;     /* line ends (clipped, rounded), tri, point, circles
                                                   * (x1 = r), invert (x1, y1 = w, h) */
    struct { int64_t x, y; glyph_t g; } gl;       /* a glyph's mask at x, y (kept by fonts_hold; for the
                                                   * GPU helper g.a holds its place in the atlas) */
  } u;
} rcmd_t;

#endif
