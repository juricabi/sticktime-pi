/* The recorded calls as triangles: see gpu_geom.h. */
#include "gpu_geom.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int grow(gmesh_t *m, int more) {
  if (m->nv + more <= m->capv) return 0;
  int cap = m->capv ? m->capv : 16384;
  while (cap < m->nv + more) cap *= 2;
  gvert_t *v = realloc(m->v, (size_t)cap * sizeof *v);
  if (!v) return -1;
  m->v = v;
  m->capv = cap;
  return 0;
}

/* the batch the next vertices go to */
static int batch(gmesh_t *m, int mode) {
  if (m->nb && m->b[m->nb - 1].mode == mode) return 0;
  if (m->nb == m->capb) {
    int cap = m->capb ? m->capb * 2 : 64;
    gbatch_t *b = realloc(m->b, (size_t)cap * sizeof *b);
    if (!b) return -1;
    m->b = b;
    m->capb = cap;
  }
  m->b[m->nb++] = (gbatch_t){ .first = m->nv, .count = 0, .mode = mode };
  return 0;
}

static void put(gmesh_t *m, float x, float y, float u, float v, const uint8_t *rgba) {
  gvert_t *q = &m->v[m->nv++];
  q->x = x;
  q->y = y;
  q->u = u;
  q->v = v;
  q->r = rgba[0];
  q->g = rgba[1];
  q->b = rgba[2];
  q->a = rgba[3];
  m->b[m->nb - 1].count++;
}

/* RGB565 with its bits repeated into 8, and an alpha */
static void color(uint16_t c, int a, uint8_t *o) {
  int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
  o[0] = (uint8_t)(r << 3 | r >> 2);
  o[1] = (uint8_t)(g << 2 | g >> 4);
  o[2] = (uint8_t)(b << 3 | b >> 2);
  o[3] = (uint8_t)a;
}

/* a convex quad p0 p1 p2 p3 (in order round it); u0 >= 0: atlas texels from u0, v0 over x0..x1, y0..y1 */
static int quad4(gmesh_t *m, const float *x, const float *y, const uint8_t *rgba, bool invert) {
  if (batch(m, invert ? GB_INVERT : GB_BLEND) || grow(m, 6)) return -1;
  static const int ix[6] = { 0, 1, 2, 0, 2, 3 };
  for (int i = 0; i < 6; i++) put(m, x[ix[i]], y[ix[i]], -1, -1, rgba);
  return 0;
}

static int rect(gmesh_t *m, double x0, double y0, double x1, double y1, uint16_t c, int a, bool invert) {
  if (x1 <= x0 || y1 <= y0) return 0;
  uint8_t rgba[4];
  color(c, a, rgba);
  float x[4] = { (float)x0, (float)x1, (float)x1, (float)x0 }, y[4] = { (float)y0, (float)y0, (float)y1, (float)y1 };
  return quad4(m, x, y, rgba, invert);
}

static int glyph(gmesh_t *m, int64_t gx, int64_t gy, const glyph_t *g, uint16_t c) {
  if (batch(m, GB_BLEND) || grow(m, 6)) return -1;
  uintptr_t at = (uintptr_t)g->a;
  float u0 = (float)(at & 0xFFFF), v0 = (float)((at >> 16) & 0xFFFF);
  float x0 = (float)gx, y0 = (float)gy, x1 = x0 + g->w, y1 = y0 + g->h;
  uint8_t rgba[4];
  color(c, 255, rgba);
  float x[4] = { x0, x1, x1, x0 }, y[4] = { y0, y0, y1, y1 };
  float u[4] = { u0, u0 + g->w, u0 + g->w, u0 }, v[4] = { v0, v0, v0 + g->h, v0 + g->h };
  static const int ix[6] = { 0, 1, 2, 0, 2, 3 };
  for (int i = 0; i < 6; i++) put(m, x[ix[i]], y[ix[i]], u[ix[i]], v[ix[i]], rgba);
  return 0;
}

/* a solid line from the clipped, rounded ends: w pixels across (the extra ones below or right, as lcd.c
 * draws them), along the line from end pixel to end pixel (the pixel centers within half a pixel of
 * the line: what Bresenham picks, give or take a tie) */
static int line(gmesh_t *m, int ax, int ay, int bx, int by, uint16_t c, int w) {
  int adx = abs(bx - ax), ady = abs(by - ay);
  if (!adx && !ady) return rect(m, ax, ay, ax + w, ay + w, c, 255, false);
  uint8_t rgba[4];
  color(c, 255, rgba);
  float x[4], y[4];
  if (adx >= ady) {
    if (ax > bx) { int t = ax; ax = bx; bx = t; t = ay; ay = by; by = t; }
    float s = (float)(by - ay) / (float)(bx - ax), yl = ay + 0.5f - 0.5f * s, yr = by + 0.5f + 0.5f * s;
    x[0] = (float)ax; y[0] = yl - 0.5f;
    x[1] = (float)bx + 1; y[1] = yr - 0.5f;
    x[2] = (float)bx + 1; y[2] = yr + 0.5f + (float)(w - 1);
    x[3] = (float)ax; y[3] = yl + 0.5f + (float)(w - 1);
  } else {
    if (ay > by) { int t = ax; ax = bx; bx = t; t = ay; ay = by; by = t; }
    float s = (float)(bx - ax) / (float)(by - ay), xt = ax + 0.5f - 0.5f * s, xb = bx + 0.5f + 0.5f * s;
    x[0] = xt - 0.5f; y[0] = (float)ay;
    x[1] = xt + 0.5f + (float)(w - 1); y[1] = (float)ay;
    x[2] = xb + 0.5f + (float)(w - 1); y[2] = (float)by + 1;
    x[3] = xb - 0.5f; y[3] = (float)by + 1;
  }
  return quad4(m, x, y, rgba, false);
}

/* a patterned line: its pixels as lcd.c's Bresenham sets them (w wide) */
static int dotted(gmesh_t *m, int ax, int ay, int bx, int by, uint8_t pat, uint16_t c, int W, int H, int w) {
  int ddx = bx - ax, ddy = by - ay;
  int adx = abs(ddx), ady = abs(ddy), sx = (ddx > 0) - (ddx < 0), sy = (ddy > 0) - (ddy < 0);
  int ex = ady >> 1, ey = adx >> 1, px = ax, py = ay;
  if (adx >= ady) {
    for (int i = 0; i <= adx; i++) {
      if (((1 << (px & 7)) & pat) && px >= 0 && px < W && py >= 0 && py < H && rect(m, px, py, px + 1, py + w, c, 255, false)) return -1;
      ey += ady;
      if (ey >= adx) { ey -= adx; py += sy; }
      px += sx;
    }
  } else {
    for (int i = 0; i <= ady; i++) {
      if (((1 << (py & 7)) & pat) && px >= 0 && px < W && py >= 0 && py < H && rect(m, px, py, px + w, py + 1, c, 255, false)) return -1;
      ex += adx;
      if (ex >= ady) { ex -= ady; px += sx; }
      py += sy;
    }
  }
  return 0;
}

/* Adafruit's fill takes both ends of every row: a triangle flat in x or y is its row or column; else
 * the GPU fills it through the pixel centers */
static int tri(gmesh_t *m, const rcmd_t *k, uint16_t c, int a) {
  int x0 = k->u.p.x0, y0 = k->u.p.y0, x1 = k->u.p.x1, y1 = k->u.p.y1, x2 = k->u.p.x2, y2 = k->u.p.y2;
  int lx = x0 < x1 ? x0 : x1, hx = x0 > x1 ? x0 : x1, ly = y0 < y1 ? y0 : y1, hy = y0 > y1 ? y0 : y1;
  if (x2 < lx) lx = x2;
  if (x2 > hx) hx = x2;
  if (y2 < ly) ly = y2;
  if (y2 > hy) hy = y2;
  if (ly == hy || lx == hx) return rect(m, lx, ly, (double)hx + 1, (double)hy + 1, c, a, false);
  if (batch(m, GB_BLEND) || grow(m, 3)) return -1;
  uint8_t rgba[4];
  color(c, a, rgba);
  put(m, x0 + 0.5f, y0 + 0.5f, -1, -1, rgba);
  put(m, x1 + 0.5f, y1 + 0.5f, -1, -1, rgba);
  put(m, x2 + 0.5f, y2 + 0.5f, -1, -1, rgba);
  return 0;
}

int gmesh_build(gmesh_t *m, const rcmd_t *c, int n, int W, int H) {
  m->nv = 0;
  m->nb = 0;
  for (int i = 0; i < n; i++) {
    const rcmd_t *k = &c[i];
    int a = k->opa >= 15 ? 0 : (15 - k->opa) * 17, e = 0;
    switch (k->op) {
      case RC_CLEAR:
        e = rect(m, 0, 0, W, H, k->c, 255, false);
        break;
      case RC_HLINE: {                           /* lcd.c hline64's clipping */
        int64_t x = k->u.r.x, y = k->u.r.y, w = k->u.r.w;
        if (!a || y < 0 || y >= H || w <= 0) break;
        if (x < 0) { w += x; x = 0; }
        if (x + w > W) w = W - x;
        if (w > 0) e = rect(m, (double)x, (double)y, (double)(x + w), (double)y + 1, k->c, a, false);
        break;
      }
      case RC_RECT:
        if (a) e = rect(m, (double)k->u.r.x, (double)k->u.r.y, (double)(k->u.r.x + k->u.r.w), (double)(k->u.r.y + k->u.r.h), k->c, a, false);
        break;
      case RC_LINE:
        if (k->pat == 0xFF) e = line(m, k->u.p.x0, k->u.p.y0, k->u.p.x1, k->u.p.y1, k->c, k->w > 1 ? k->w : 1);
        else if (k->pat) e = dotted(m, k->u.p.x0, k->u.p.y0, k->u.p.x1, k->u.p.y1, k->pat, k->c, W, H, k->w > 1 ? k->w : 1);
        break;
      case RC_TRI:
        if (a) e = tri(m, k, k->c, a);
        break;
      case RC_POINT:
        if (k->u.p.x0 > -k->w && k->u.p.y0 > -k->w && k->u.p.x0 < W && k->u.p.y0 < H)
          e = rect(m, k->u.p.x0, k->u.p.y0, k->u.p.x0 + (k->w > 1 ? k->w : 1), k->u.p.y0 + (k->w > 1 ? k->w : 1), k->c, 255, false);
        break;
      case RC_FCIRCLE: {                         /* lcd.c's rows */
        int64_t cx = k->u.p.x0, cy = k->u.p.y0, r = k->u.p.x1;
        for (int64_t dy = -r; dy <= r && !e; dy++) {
          if (cy + dy < 0 || cy + dy >= H) continue;
          int64_t dx = (int64_t)floor(sqrt((double)r * r - (double)dy * dy));
          e = rect(m, (double)(cx - dx), (double)(cy + dy), (double)(cx + dx + 1), (double)(cy + dy + 1), k->c, 255, false);
        }
        break;
      }
      case RC_CIRCLE:
        for (int d = 0; d < 360 && !e; d++) {
          double x = floor(k->u.p.x0 + k->u.p.x1 * cos(d * M_PI / 180) + 0.5), y = floor(k->u.p.y0 + k->u.p.x1 * sin(d * M_PI / 180) + 0.5);
          int w = k->w > 1 ? k->w : 1;
          if (x > -w && y > -w && x < W && y < H) e = rect(m, x, y, x + w, y + w, k->c, 255, false);
        }
        break;
      case RC_INVERT:
        if (k->u.p.x1 > 0 && k->u.p.y1 > 0)
          e = rect(m, k->u.p.x0, k->u.p.y0, (double)k->u.p.x0 + k->u.p.x1, (double)k->u.p.y0 + k->u.p.y1, 0xFFFF, 255, true);
        break;
      case RC_GLYPH:
        if (k->u.gl.g.w > 0 && k->u.gl.g.h > 0) e = glyph(m, k->u.gl.x, k->u.gl.y, &k->u.gl.g, k->c);
        break;
    }
    if (e) return -1;
  }
  return 0;
}

void gmesh_free(gmesh_t *m) {
  free(m->v);
  free(m->b);
  memset(m, 0, sizeof *m);
}
