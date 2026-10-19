/* gpu_geom: the triangles cover the pixels the CPU draws. A small rasterizer with the GPU's rules
 * (pixel centers, top-left ties) fills them; rects, rows, points, filled circles and glyph quads must
 * cover exactly lcd.c's pixels, solid lines all but a few ties, triangles what Adafruit fills give or
 * take their edges. */
#include "t.h"
#include "gpu_geom.h"
#include "lcd.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { W = 320, H = 200 };

static uint32_t rs = 777;
static int rnd_in(int lo, int hi) {
  rs ^= rs << 13;
  rs ^= rs >> 17;
  rs ^= rs << 5;
  return lo + (int)(rs % (uint32_t)(hi - lo + 1));
}

/* edge function, and whether an edge of a counter-clockwise (y down) triangle owns its pixel centers */
static double edge(double ax, double ay, double bx, double by, double px, double py) {
  return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}
static bool top_left(double ax, double ay, double bx, double by) {
  return (ay == by && bx > ax) || by < ay;
}

/* the coverage of the mesh: cov[y * W + x] = 1 + the index of the last triangle that covered it */
static void raster(const gmesh_t *m, int *cov) {
  memset(cov, 0, sizeof(int) * W * H);
  for (int t = 0; t + 2 < m->nv; t += 3) {
    double x[3], y[3];
    for (int i = 0; i < 3; i++) { x[i] = m->v[t + i].x; y[i] = m->v[t + i].y; }
    double area = edge(x[0], y[0], x[1], y[1], x[2], y[2]);
    if (area == 0) continue;
    if (area < 0) { double q = x[1]; x[1] = x[2]; x[2] = q; q = y[1]; y[1] = y[2]; y[2] = q; }
    int x0 = (int)floor(fmin(x[0], fmin(x[1], x[2]))), x1 = (int)ceil(fmax(x[0], fmax(x[1], x[2])));
    int y0 = (int)floor(fmin(y[0], fmin(y[1], y[2]))), y1 = (int)ceil(fmax(y[0], fmax(y[1], y[2])));
    for (int py = y0 < 0 ? 0 : y0; py < y1 && py < H; py++)
      for (int px = x0 < 0 ? 0 : x0; px < x1 && px < W; px++) {
        double cx = px + 0.5, cy = py + 0.5;
        bool in = true;
        for (int i = 0; i < 3 && in; i++) {
          int j = (i + 1) % 3;
          double e = edge(x[i], y[i], x[j], y[j], cx, cy);
          in = e > 0 || (e == 0 && top_left(x[i], y[i], x[j], y[j]));
        }
        if (in) cov[py * W + px] = t / 3 + 1;
      }
  }
}

static int *cov;
static lcd_t l;

/* the pixels lcd.c draws for one call, against the mesh's coverage: the number that differ */
static int differ(const rcmd_t *k, void (*draw)(void)) {
  lcd_clear(&l, 0);
  draw();
  gmesh_t m = { 0 };
  CHECK_INT(gmesh_build(&m, k, 1, W, H), 0);
  raster(&m, cov);
  int d = 0;
  for (int i = 0; i < W * H; i++) d += (l.fb[i] != 0) != (cov[i] != 0);
  gmesh_free(&m);
  return d;
}

static rcmd_t K;
static int A[8];
static void d_rect(void) { lcd_fill_rect(&l, A[0], A[1], A[2], A[3], 0xFFFF, 0); }
static void d_hline(void) { lcd_hline(&l, A[0], A[1], A[2], 0xFFFF, 0); }
static void d_line(void) { lcd_line(&l, A[0], A[1], A[2], A[3], 0xFF, 0xFFFF); }
static void d_tri(void) { lcd_tri(&l, A[0], A[1], A[2], A[3], A[4], A[5], 0xFFFF, 0); }
static void d_fcircle(void) { lcd_filled_circle(&l, A[0], A[1], A[2], 0xFFFF); }

/* what lcd.c records for the call (its clipping and line ends): a pool-less recording lcd_t */
static rcmd_t record(void (*draw)(void)) {
  lcd_t r;
  lcd_init(&r, W, H, NULL, NULL);
  lcd_set_mode(&r, NULL, true);
  lcd_t keep = l;
  l = r;
  draw();
  int n;
  const rcmd_t *c = lcd_calls(&l, &n);
  rcmd_t k;
  memset(&k, 0, sizeof k);
  k.op = 255;                                    /* nothing recorded (clipped away) */
  if (n > 0) k = c[n - 1];
  r = l;
  l = keep;
  lcd_free(&r);
  return k;
}

int main(void) {
  cov = malloc(sizeof(int) * W * H);
  lcd_init(&l, W, H, NULL, NULL);
  int bad_exact = 0, line_px = 0, line_diff = 0, tri_px = 0, tri_diff = 0;
  for (int it = 0; it < 400; it++) {
    for (int i = 0; i < 6; i++) A[i] = i % 2 ? rnd_in(-40, H + 40) : rnd_in(-40, W + 40);
    A[2] = rnd_in(-60, 200);
    A[3] = rnd_in(-60, 150);
    K = record(d_rect);
    if (K.op != 255) bad_exact += differ(&K, d_rect) != 0;
    K = record(d_hline);
    if (K.op != 255) bad_exact += differ(&K, d_hline) != 0;
    int r = A[2] / 4;
    int keep2 = A[2];
    A[2] = r;
    K = record(d_fcircle);
    if (K.op != 255) bad_exact += differ(&K, d_fcircle) != 0;
    A[2] = keep2;
    for (int i = 0; i < 4; i++) A[i] = i % 2 ? rnd_in(-40, H + 40) : rnd_in(-40, W + 40);
    K = record(d_line);
    if (K.op != 255) {
      line_diff += differ(&K, d_line);
      for (int i = 0; i < W * H; i++) line_px += l.fb[i] != 0;
    }
    for (int i = 0; i < 6; i++) A[i] = i % 2 ? rnd_in(0, H - 1) : rnd_in(0, W - 1);
    K = record(d_tri);
    if (K.op != 255) {
      tri_diff += differ(&K, d_tri);
      for (int i = 0; i < W * H; i++) tri_px += l.fb[i] != 0;
    }
  }
  CHECK_INT(bad_exact, 0);
  /* lines: ties at exactly half a pixel may go the other way */
  CHECK(line_diff * 50 < line_px);
  /* triangles: Adafruit's rows take both ends, the GPU the centers inside: their edges */
  CHECK(tri_diff * 4 < tri_px);
  printf("lines: %d of %d pixels differ; triangles: %d of %d\n", line_diff, line_px, tri_diff, tri_px);

  /* a glyph is a quad of its size with the atlas texels from its place */
  rcmd_t g;
  memset(&g, 0, sizeof g);
  g.op = RC_GLYPH;
  g.c = 0xF800;
  g.u.gl.x = 10;
  g.u.gl.y = 20;
  g.u.gl.g.w = 7;
  g.u.gl.g.h = 9;
  g.u.gl.g.a = (uint8_t *)(uintptr_t)(300u | 40u << 16);
  gmesh_t m = { 0 };
  CHECK_INT(gmesh_build(&m, &g, 1, W, H), 0);
  CHECK_INT(m.nv, 6);
  raster(&m, cov);
  int n = 0;
  for (int i = 0; i < W * H; i++) n += cov[i] != 0;
  CHECK_INT(n, 63);
  CHECK(cov[20 * W + 10] && cov[28 * W + 16] && !cov[29 * W + 16] && !cov[20 * W + 17]);
  float umin = 1e9f, vmin = 1e9f, umax = 0, vmax = 0;
  for (int i = 0; i < 6; i++) {
    umin = fminf(umin, m.v[i].u); umax = fmaxf(umax, m.v[i].u);
    vmin = fminf(vmin, m.v[i].v); vmax = fmaxf(vmax, m.v[i].v);
  }
  CHECK(umin == 300 && umax == 307 && vmin == 40 && vmax == 49);
  CHECK(m.v[0].r == 255 && m.v[0].g == 0 && m.v[0].b == 0 && m.v[0].a == 255);
  /* opacity is alpha (15 - opa) * 17; an invert rect is a batch of its own */
  rcmd_t c3[3];
  memset(c3, 0, sizeof c3);
  c3[0].op = RC_RECT; c3[0].u.r.w = 5; c3[0].u.r.h = 5; c3[0].opa = 7; c3[0].c = 0x07E0;
  c3[1].op = RC_INVERT; c3[1].u.p.x1 = 4; c3[1].u.p.y1 = 4;
  c3[2].op = RC_POINT; c3[2].u.p.x0 = 3; c3[2].u.p.y0 = 3;
  CHECK_INT(gmesh_build(&m, c3, 3, W, H), 0);
  CHECK_INT(m.nb, 3);
  CHECK(!m.b[0].invert && m.b[1].invert && !m.b[2].invert);
  CHECK_INT(m.v[0].a, 8 * 17);
  CHECK(m.v[0].g == 255 && m.v[0].r == 0);
  gmesh_free(&m);
  lcd_free(&l);
  free(cov);
  T_DONE();
}
