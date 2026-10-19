/* lcd.c: colors, blending, clipping and each primitive, checked by hand and against literal
 * transcriptions of the engine.js loops (which visit every row, also the invisible ones).
 * test_lcd_golden.c compares with engine.js itself. */
#include "t.h"
#include "lcd.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#define W 480
#define H 272

static int count_color(const lcd_t *l, uint16_t c) {
  int n = 0;
  for (int i = 0; i < l->w * l->h; i++) n += l->fb[i] == c;
  return n;
}

static uint16_t px(const lcd_t *l, int x, int y) { return l->fb[y * l->w + x]; }

static uint32_t rnd_state = 12345;
static uint32_t rnd(void) {
  rnd_state ^= rnd_state << 13;
  rnd_state ^= rnd_state >> 17;
  rnd_state ^= rnd_state << 5;
  return rnd_state;
}
static int rnd_in(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

/* ------------------------------------------------------------------ literal engine.js loops */

static double js_round(double v) { double f = floor(v); return v - f >= 0.5 ? f + 1 : f; }

static void ref_hline(uint16_t *fb, double x, double y, double w, uint16_t c, int opa) {
  if (y < 0 || y >= H || w <= 0) return;
  if (x < 0) { w += x; x = 0; }
  if (x + w > W) w = W - x;
  if (w <= 0) return;
  for (int i = 0; i < (int)w; i++) {
    uint16_t *p = &fb[(int)y * W + (int)x + i];
    *p = opa ? lcd_blend565(*p, c, (int)js_round((15 - opa) * 255 / 15.0)) : c;
  }
}

static void ref_tri(uint16_t *fb, double x0, double y0, double x1, double y1, double x2, double y2, uint16_t c, int opa) {
  double t;
  if (y0 > y1) { t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
  if (y1 > y2) { t = y2; y2 = y1; y1 = t; t = x2; x2 = x1; x1 = t; }
  if (y0 > y1) { t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
#define ROW(a, b, yy) do { double A_ = (a), B_ = (b); if (A_ > B_) { double q = A_; A_ = B_; B_ = q; } \
  ref_hline(fb, A_, yy, B_ - A_ + 1, c, opa); } while (0)
  if (y0 == y2) {
    double a = fmin(x0, fmin(x1, x2)), b = fmax(x0, fmax(x1, x2));
    ROW(a, b, y0);
    return;
  }
  double dx01 = x1 - x0, dy01 = y1 - y0, dx02 = x2 - x0, dy02 = y2 - y0, dx12 = x2 - x1, dy12 = y2 - y1;
  double sa = 0, sb = 0, y;
  double last = y1 == y2 ? y1 : y1 - 1;
  for (y = y0; y <= last; y++) {
    if (y >= 0 && y < H) ROW(x0 + trunc(sa / dy01), x0 + trunc(sb / dy02), y);
    sa += dx01;
    sb += dx02;
  }
  sa = dx12 * (y - y1);
  sb = dx02 * (y - y0);
  for (; y <= y2; y++) {
    if (y >= 0 && y < H) ROW(x1 + trunc(sa / dy12), x0 + trunc(sb / dy02), y);
    sa += dx12;
    sb += dx02;
  }
#undef ROW
}

static void ref_filled_circle(uint16_t *fb, double cx, double cy, double r, uint16_t c) {
  for (double dy = -r; dy <= r; dy++) {
    volatile double rr = r * r, dd = dy * dy;
    double dx = floor(sqrt(rr - dd));
    ref_hline(fb, cx - dx, cy + dy, dx * 2 + 1, c, 0);
  }
}

/* ------------------------------------------------------------------ tests */

static void test_colors(void) {
  CHECK_INT(LCD_NAMED_COLORS, 31);
  CHECK_STR(LCD_NAMED[0].name, "COLOR_THEME_PRIMARY1");
  CHECK_STR(LCD_NAMED[11].name, "BLACK");
  CHECK_STR(LCD_NAMED[12].name, "WHITE");
  CHECK_STR(LCD_NAMED[30].name, "MAGENTA");
  CHECK_INT(LCD_NAMED[23].r, 0x30); CHECK_INT(LCD_NAMED[23].g, 0xA0); CHECK_INT(LCD_NAMED[23].b, 0xE0);
  CHECK_INT(lcd_rgb565(255, 255, 255), 0xFFFF);
  CHECK_INT(lcd_rgb565(255, 0, 0), 0xF800);
  CHECK_INT(lcd_rgb565(0, 255, 0), 0x07E0);
  CHECK_INT(lcd_rgb565(0, 0, 255), 0x001F);
  CHECK_INT(lcd_rgb565(7, 3, 7), 0);              /* low bits dropped */
  CHECK_INT(lcd_rgb565(0x1FF, 0x1FF, 0xFF), 0xFFFF);
  /* named colors: index << 16, no RGB flag */
  for (int i = 0; i < LCD_NAMED_COLORS; i++)
    CHECK_INT(lcd_color_of((uint32_t)i << 16), lcd_rgb565(LCD_NAMED[i].r, LCD_NAMED[i].g, LCD_NAMED[i].b));
  CHECK_INT(lcd_color_of(0), 0);
  CHECK_INT(lcd_color_of(31u << 16), 0);           /* past the table: black */
  CHECK_INT(lcd_color_of(0xFFFFu << 16), 0);
  CHECK_INT(lcd_color_of((12u << 16) | 0x0583), 0xFFFF);   /* low flag bits don't matter */
  /* RGB flag: the top 16 bits are the color (lcd.RGB packs (rgb565 << 16) | 0x8000) */
  CHECK_INT(lcd_color_of(0xF8008000u), 0xF800);
  CHECK_INT(lcd_color_of(0x00008000u), 0);
  CHECK_INT(lcd_color_of(0xFFFF8380u), 0xFFFF);    /* SMLSIZE + white + SHADOWED as the game passes it */
  CHECK_INT(lcd_color_of((uint32_t)(int32_t)-31872), 0xFFFF);
}

static void test_blend(void) {
  CHECK_INT(lcd_blend565(0x1234, 0xABCD, 0), 0x1234);
  CHECK_INT(lcd_blend565(0x1234, 0xABCD, 255), 0xABCD);
  CHECK_INT(lcd_blend565(0x0000, 0xFFFF, 128), (16 << 11) | (32 << 5) | 16);
  CHECK_INT(lcd_blend565(0xFFFF, 0x0000, 128), (15 << 11) | (31 << 5) | 15);
  /* (31 * 17 + 0 * 238 + 127) / 255 = 2 */
  CHECK_INT(lcd_blend565(0x0000, 0xF800, 17), 2 << 11);
  for (int a = 0; a <= 255; a++) {
    uint16_t d = (uint16_t)rnd(), s = (uint16_t)rnd(), v = lcd_blend565(d, s, a);
    int r = ((s >> 11) * a + (d >> 11) * (255 - a) + 127) / 255;
    int g = (((s >> 5) & 63) * a + ((d >> 5) & 63) * (255 - a) + 127) / 255;
    int b = ((s & 31) * a + (d & 31) * (255 - a) + 127) / 255;
    CHECK_INT(v, (r << 11) | (g << 5) | b);
  }
  /* same colors stay the same at any alpha */
  for (int a = 0; a <= 255; a += 5) CHECK_INT(lcd_blend565(0x7BEF, 0x7BEF, a), 0x7BEF);
}

static void test_rects(void) {
  lcd_t l;
  CHECK_INT(lcd_init(&l, W, H, NULL, NULL), 0);
  CHECK_STR(l.cls.name, "std");
  CHECK_INT(count_color(&l, 0), W * H);
  lcd_clear(&l, 0x1234);
  CHECK_INT(count_color(&l, 0x1234), W * H);
  lcd_clear(&l, 0);

  lcd_fill_rect(&l, 10, 20, 5, 3, 0xF800, 0);
  CHECK_INT(count_color(&l, 0xF800), 15);
  CHECK_INT(px(&l, 10, 20), 0xF800); CHECK_INT(px(&l, 14, 22), 0xF800);
  CHECK_INT(px(&l, 15, 22), 0); CHECK_INT(px(&l, 14, 23), 0); CHECK_INT(px(&l, 9, 20), 0);
  /* negative sizes flip: (15, 23, -5, -3) is the same box */
  lcd_clear(&l, 0);
  lcd_fill_rect(&l, 15, 23, -5, -3, 0xF800, 0);
  CHECK_INT(count_color(&l, 0xF800), 15);
  CHECK_INT(px(&l, 10, 20), 0xF800);
  /* zero sizes: nothing */
  lcd_clear(&l, 0);
  lcd_fill_rect(&l, 15, 23, 0, 10, 0xF800, 0);
  lcd_fill_rect(&l, 15, 23, 10, 0, 0xF800, 0);
  CHECK_INT(count_color(&l, 0), W * H);
  /* clipping on every side, and int32 extremes without overflow */
  lcd_fill_rect(&l, -5, -5, 10, 10, 0x07E0, 0);
  CHECK_INT(count_color(&l, 0x07E0), 25);
  lcd_clear(&l, 0);
  lcd_fill_rect(&l, W - 3, H - 2, 100, 100, 0x07E0, 0);
  CHECK_INT(count_color(&l, 0x07E0), 6);
  lcd_clear(&l, 0);
  lcd_fill_rect(&l, INT_MIN, INT_MIN, INT_MAX, INT_MAX, 0x07E0, 0);   /* ends at -2: off */
  lcd_fill_rect(&l, INT_MAX, INT_MAX, INT_MAX, INT_MAX, 0x07E0, 0);
  lcd_fill_rect(&l, INT_MAX, 0, INT_MIN, 10, 0x07E0, 0);              /* flips to x = -1 */
  CHECK_INT(count_color(&l, 0x07E0), W * 10);
  lcd_clear(&l, 0);
  lcd_fill_rect(&l, INT_MIN, INT_MIN, -1, -1, 0x07E0, 0);
  lcd_fill_rect(&l, 0, 0, INT_MIN, INT_MIN, 0x07E0, 0);
  CHECK_INT(count_color(&l, 0), W * H);
  lcd_fill_rect(&l, 1, 1, INT_MAX, INT_MAX, 0x07E0, 0);
  CHECK_INT(count_color(&l, 0x07E0), (W - 1) * (H - 1));

  /* opacity: 15 invisible, 0 opaque, else blend with a = (15 - opa) * 17 */
  for (int opa = 0; opa <= 15; opa++) {
    lcd_clear(&l, 0x4208);
    lcd_fill_rect(&l, 0, 0, 4, 4, 0xFFE0, opa);
    uint16_t want = opa == 15 ? 0x4208 : opa == 0 ? 0xFFE0 : lcd_blend565(0x4208, 0xFFE0, (15 - opa) * 17);
    CHECK_INT(px(&l, 3, 3), want);
    CHECK_INT(px(&l, 4, 3), 0x4208);
  }
  /* translucent rects and runs (blend_run's arithmetic): lcd_blend565 for every pixel of a random
   * picture, every opacity */
  static uint16_t before[W * H];
  for (int opa = 1; opa <= 14; opa++) {
    for (int i = 0; i < W * H; i++) l.fb[i] = (uint16_t)rnd();
    memcpy(before, l.fb, sizeof before);
    uint16_t c = (uint16_t)rnd();
    int bad = 0, a = (15 - opa) * 17;
    lcd_fill_rect(&l, 7, 3, W - 20, H - 10, c, opa);
    lcd_hline(&l, 0, H - 2, W, c, opa);
    lcd_hline(&l, 0, H - 1, 100, c, opa);
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        bool in = (x >= 7 && x < W - 13 && y >= 3 && y < H - 7) || y == H - 2 || (y == H - 1 && x < 100);
        bad += px(&l, x, y) != (in ? lcd_blend565(before[y * W + x], c, a) : before[y * W + x]);
      }
    CHECK_INT(bad, 0);
  }
  /* opacity is masked like the Lua API does (opt(6) & 15) */
  lcd_clear(&l, 0x4208);
  lcd_fill_rect(&l, 0, 0, 4, 4, 0xFFE0, 16);       /* 16 & 15 = 0: opaque */
  CHECK_INT(px(&l, 0, 0), 0xFFE0);
  lcd_fill_rect(&l, 0, 0, 4, 4, 0x001F, -1);       /* -1 & 15 = 15: invisible */
  CHECK_INT(px(&l, 0, 0), 0xFFE0);

  /* hline */
  lcd_clear(&l, 0);
  lcd_hline(&l, -3, 5, 10, 0xF800, 0);
  CHECK_INT(count_color(&l, 0xF800), 7);
  lcd_hline(&l, W - 2, 6, 10, 0xF800, 0);
  lcd_hline(&l, 0, -1, 10, 0xF800, 0);
  lcd_hline(&l, 0, H, 10, 0xF800, 0);
  lcd_hline(&l, 0, 7, 0, 0xF800, 0);
  lcd_hline(&l, 0, 7, -4, 0xF800, 0);
  lcd_hline(&l, INT_MAX, 8, INT_MAX, 0xF800, 0);
  lcd_hline(&l, INT_MIN, 9, INT_MAX, 0xF800, 0);
  CHECK_INT(count_color(&l, 0xF800), 9);
  lcd_hline(&l, 0, 10, W, 0xF800, 15);            /* opa 15: a = 0, unchanged */
  CHECK_INT(px(&l, 0, 10), 0);
  lcd_hline(&l, 0, 10, W, 0xF800, 7);
  CHECK_INT(px(&l, W - 1, 10), lcd_blend565(0, 0xF800, 8 * 17));

  /* drawRectangle: four rects of thickness t inside the box */
  lcd_clear(&l, 0);
  lcd_draw_rectangle(&l, 10, 10, 20, 10, 0xFFFF8000u, 1, 0);
  CHECK_INT(count_color(&l, 0xFFFF), 2 * 20 + 2 * 8);
  CHECK_INT(px(&l, 10, 10), 0xFFFF); CHECK_INT(px(&l, 29, 19), 0xFFFF); CHECK_INT(px(&l, 11, 11), 0);
  lcd_clear(&l, 0);
  lcd_draw_rectangle(&l, 10, 10, 20, 10, 12u << 16, 3, 0);
  CHECK_INT(count_color(&l, 0xFFFF), 20 * 10 - 14 * 4);
  lcd_clear(&l, 0);
  lcd_draw_rectangle(&l, 10, 10, 20, 10, 12u << 16, 0, 0);    /* t = 0: nothing */
  CHECK_INT(count_color(&l, 0), W * H);
  lcd_draw_rectangle(&l, 10, 10, 20, 10, 12u << 16, 1, 15);   /* invisible */
  CHECK_INT(count_color(&l, 0), W * H);
  lcd_draw_rectangle(&l, INT_MAX, INT_MAX, INT_MAX, INT_MAX, 12u << 16, INT_MAX, 0);
  lcd_draw_rectangle(&l, INT_MIN, INT_MIN, INT_MIN, INT_MIN, 12u << 16, INT_MIN, 0);
  lcd_draw_rectangle(&l, 0, 0, 5, 5, 12u << 16, 1, 31);       /* 31 & 15 = 15 */
  CHECK_INT(count_color(&l, 0), W * H);

  /* invertRect */
  lcd_clear(&l, 0x1234);
  lcd_invert_rect(&l, -2, -2, 4, 4);
  CHECK_INT(count_color(&l, 0x1234 ^ 0xFFFF), 4);
  lcd_invert_rect(&l, -2, -2, 4, 4);
  CHECK_INT(count_color(&l, 0x1234), W * H);
  lcd_invert_rect(&l, 5, 5, -3, 3);               /* negative sizes: nothing (no flip here) */
  lcd_invert_rect(&l, 5, 5, 3, -3);
  lcd_invert_rect(&l, W, 0, 5, 5);
  lcd_invert_rect(&l, 0, H, 5, 5);
  lcd_invert_rect(&l, INT_MAX, INT_MAX, INT_MAX, INT_MAX);
  CHECK_INT(count_color(&l, 0x1234), W * H);
  lcd_invert_rect(&l, INT_MIN, INT_MIN, INT_MAX, INT_MAX);  /* ends at -2 */
  CHECK_INT(count_color(&l, 0x1234), W * H);
  lcd_invert_rect(&l, 0, 0, INT_MAX, INT_MAX);
  CHECK_INT(count_color(&l, 0x1234 ^ 0xFFFF), W * H);

  /* points */
  lcd_clear(&l, 0);
  lcd_point(&l, 0, 0, 1); lcd_point(&l, W - 1, H - 1, 2);
  lcd_point(&l, -1, 0, 3); lcd_point(&l, W, 0, 3); lcd_point(&l, 0, H, 3); lcd_point(&l, 0, -1, 3);
  CHECK_INT(px(&l, 0, 0), 1); CHECK_INT(px(&l, W - 1, H - 1), 2); CHECK_INT(count_color(&l, 3), 0);

  /* gauge: background DISABLED, then the bar */
  uint16_t dis = lcd_color_of(10u << 16);
  lcd_clear(&l, 0);
  lcd_draw_gauge(&l, 0, 0, 100, 2, 1, 4, 0xF8008000u);
  CHECK_INT(count_color(&l, 0xF800), 50); CHECK_INT(count_color(&l, dis), 150);
  lcd_clear(&l, 0);
  lcd_draw_gauge(&l, 0, 0, 100, 2, 7, 4, 0xF8008000u);      /* n > d: full */
  CHECK_INT(count_color(&l, 0xF800), 200);
  lcd_clear(&l, 0);
  lcd_draw_gauge(&l, 0, 0, 100, 2, 5, 0, 0xF8008000u);      /* n / 0 = Infinity: full */
  CHECK_INT(count_color(&l, 0xF800), 200);
  lcd_clear(&l, 0);
  lcd_draw_gauge(&l, 0, 0, 100, 2, -5, 0, 0xF8008000u);     /* -Infinity: empty */
  CHECK_INT(count_color(&l, 0xF800), 0); CHECK_INT(count_color(&l, dis), 200);
  lcd_clear(&l, 0);
  lcd_draw_gauge(&l, 0, 0, 100, 2, 0, 0, 0xF8008000u);      /* NaN: background only */
  CHECK_INT(count_color(&l, 0xF800), 0); CHECK_INT(count_color(&l, dis), 200);
  lcd_clear(&l, 0);
  lcd_draw_gauge(&l, 0, 0, 100, 2, -1, 4, 0xF8008000u);     /* negative: empty */
  CHECK_INT(count_color(&l, 0xF800), 0);
  lcd_clear(&l, 0);
  lcd_draw_gauge(&l, 0, 0, 100, 1, 1, -4, 0xF8008000u);     /* negative d: empty */
  CHECK_INT(count_color(&l, 0xF800), 0);
  lcd_clear(&l, 0);
  lcd_draw_gauge(&l, 0, 0, 5, 1, 1, 2, 0xF8008000u);        /* 2.5 -> 3 (halves up) */
  CHECK_INT(count_color(&l, 0xF800), 3);
  lcd_clear(&l, 0);
  lcd_draw_gauge(&l, 50, 0, -5, 1, 1, 2, 0xF8008000u);      /* -2.5 -> -2: x 48..49 */
  CHECK_INT(count_color(&l, 0xF800), 2);
  CHECK_INT(px(&l, 48, 0), 0xF800); CHECK_INT(px(&l, 47, 0), dis);
  lcd_clear(&l, 0);
  lcd_draw_gauge(&l, 0, 0, INT_MAX, 1, INT_MAX, INT_MAX, 0xF8008000u);
  CHECK_INT(count_color(&l, 0xF800), W);

  /* stats */
  memset(&l.st, 0, sizeof l.st);
  lcd_fill_rect(&l, 0, 0, 1, 1, 0, 0);
  lcd_fill_rect(&l, -5, 0, 1, 1, 0, 0);            /* clipped away: not counted */
  lcd_clear(&l, 0);
  CHECK_INT(l.st.rects, 2);
  lcd_free(&l);
  CHECK(l.fb == NULL);
  lcd_free(&l);                                    /* twice is fine */

  CHECK_INT(lcd_init(&l, 0, 10, NULL, NULL), -1);
  CHECK_INT(lcd_init(&l, 10, -1, NULL, NULL), -1);
  font_class_t pc;
  font_class_for(&pc, 0, 0, "pc");
  CHECK_INT(lcd_init(&l, 10, 10, NULL, &pc), 0);
  CHECK_STR(l.cls.name, "pc");
  lcd_free(&l);
}

static void test_lines(void) {
  lcd_t l;
  lcd_init(&l, W, H, NULL, NULL);
  lcd_line(&l, 10, 10, 20, 10, 0xFF, 0xFFFF);
  CHECK_INT(count_color(&l, 0xFFFF), 11);
  /* the pattern bit is x & 7 along shallow lines, y & 7 along steep ones */
  for (int pat = 0; pat < 256; pat++) {
    lcd_clear(&l, 0);
    lcd_line(&l, 3, 5, 3 + 63, 5, (uint8_t)pat, 0xFFFF);
    int ok = 1;
    for (int x = 3; x <= 66; x++) ok &= (px(&l, x, 5) == 0xFFFF) == !!(pat & (1 << (x & 7)));
    CHECK(ok);
    lcd_clear(&l, 0);
    lcd_line(&l, 7, 70, 7, 9, (uint8_t)pat, 0xFFFF);
    ok = 1;
    for (int y = 9; y <= 70; y++) ok &= (px(&l, 7, y) == 0xFFFF) == !!(pat & (1 << (y & 7)));
    CHECK(ok);
  }
  /* diagonal, all octants have max(|dx|, |dy|) + 1 pixels and both ends */
  static const int ends[8][2] = { { 30, 10 }, { 10, 30 }, { -10, 30 }, { -30, 10 }, { -30, -10 }, { -10, -30 }, { 10, -30 }, { 30, -10 } };
  for (int k = 0; k < 8; k++) {
    lcd_clear(&l, 0);
    lcd_line(&l, 100, 100, 100 + ends[k][0], 100 + ends[k][1], 0xFF, 0xFFFF);
    CHECK_INT(count_color(&l, 0xFFFF), 31);
    CHECK_INT(px(&l, 100, 100), 0xFFFF);
    CHECK_INT(px(&l, 100 + ends[k][0], 100 + ends[k][1]), 0xFFFF);
  }
  /* level solid lines are filled as one run: the pixels between the ends that are on the screen */
  for (int k = 0; k < 400; k++) {
    int x1 = rnd_in(-60, W + 60), x2 = rnd_in(-60, W + 60), y = rnd_in(-3, H + 2);
    lcd_clear(&l, 0);
    lcd_line(&l, x1, y, x2, y, 0xFF, 0x07E0);
    int lo = x1 < x2 ? x1 : x2, hi = x1 < x2 ? x2 : x1, n = 0;
    bool on = y >= 0 && y < H && hi >= 0 && lo < W;
    if (on) n = (hi > W - 1 ? W - 1 : hi) - (lo < 0 ? 0 : lo) + 1;
    CHECK_INT(count_color(&l, 0x07E0), n);
    if (on) CHECK(px(&l, lo < 0 ? 0 : lo, y) == 0x07E0 && px(&l, hi > W - 1 ? W - 1 : hi, y) == 0x07E0);
  }
  /* line_w 2 (a doubled render): the line, and the same line a pixel below it (running more across than
   * up) or right of it (more up); a point is 2 x 2 */
  {
    lcd_t t1, t2;
    lcd_init(&t1, W, H, NULL, NULL);
    lcd_init(&t2, W, H, NULL, NULL);
    t2.line_w = 2;
    int bad = 0;
    for (int k = 0; k < 500; k++) {
      int x1 = rnd_in(2, W - 3), y1 = rnd_in(2, H - 3), x2 = rnd_in(2, W - 3), y2 = rnd_in(2, H - 3);
      uint8_t pat = k % 3 ? 0xFF : (uint8_t)rnd();
      bool across = abs(x2 - x1) >= abs(y2 - y1);
      lcd_clear(&t1, 0);
      lcd_clear(&t2, 0);
      lcd_line(&t1, x1, y1, x2, y2, pat, 0xFFFF);
      for (int y = 0; y < H; y++)                         /* the line's pixels, and their partners */
        for (int x = 0; x < W; x++)
          if (t1.fb[y * W + x] == 0xFFFF) {
            int px = across ? x : x + 1, py = across ? y + 1 : y;
            if (px < W && py < H && t1.fb[py * W + px] != 0xFFFF) t1.fb[py * W + px] = 0x0001;
          }
      for (int i = 0; i < W * H; i++) if (t1.fb[i] == 0x0001) t1.fb[i] = 0xFFFF;
      lcd_line(&t2, x1, y1, x2, y2, pat, 0xFFFF);
      bad += memcmp(t1.fb, t2.fb, sizeof *t1.fb * W * H) != 0;
    }
    CHECK_INT(bad, 0);
    lcd_clear(&t2, 0);
    lcd_point(&t2, 10, 20, 0xFFFF);
    lcd_point(&t2, W - 1, H - 1, 0xFFFF);                 /* its partners off the screen */
    CHECK_INT(count_color(&t2, 0xFFFF), 5);
    CHECK(px(&t2, 11, 21) == 0xFFFF && px(&t2, 12, 20) == 0);
    lcd_free(&t1);
    lcd_free(&t2);
  }
  /* a point */
  lcd_clear(&l, 0);
  lcd_line(&l, 5, 6, 5, 6, 0xFF, 0xFFFF);
  CHECK_INT(count_color(&l, 0xFFFF), 1);
  lcd_line(&l, -5, 6, -5, 6, 0xFF, 0xFFFF);        /* off-screen point */
  lcd_line(&l, W, 6, W, 6, 0xFF, 0xFFFF);
  CHECK_INT(count_color(&l, 0xFFFF), 1);
  /* clipped to the screen: a horizontal line across everything has W pixels */
  lcd_clear(&l, 0);
  lcd_line(&l, -1000, 50, 5000, 50, 0xFF, 0xFFFF);
  CHECK_INT(count_color(&l, 0xFFFF), W);
  lcd_clear(&l, 0);
  lcd_line(&l, INT_MIN, 50, INT_MAX, 50, 0xFF, 0xFFFF);
  CHECK_INT(count_color(&l, 0xFFFF), W);
  lcd_clear(&l, 0);
  lcd_line(&l, 60, INT_MIN, 60, INT_MAX, 0xFF, 0xFFFF);
  CHECK_INT(count_color(&l, 0xFFFF), H);
  lcd_clear(&l, 0);
  lcd_line(&l, INT_MIN, INT_MIN, INT_MAX, INT_MAX, 0xFF, 0xFFFF);
  CHECK(count_color(&l, 0xFFFF) > 0);
  lcd_clear(&l, 0);
  lcd_line(&l, -10, -10, -1, -50, 0xFF, 0xFFFF);   /* entirely outside */
  lcd_line(&l, W, 0, W + 50, H, 0xFF, 0xFFFF);
  lcd_line(&l, 0, H, W, H + 10, 0xFF, 0xFFFF);
  CHECK_INT(count_color(&l, 0), W * H);
  CHECK_INT(l.st.lines, 1 + 512 + 8 + 400 + 3 + 4 + 3);

  /* the API rejects ends beyond W / H (not W - 1 / H - 1), keeps negative ones */
  memset(&l.st, 0, sizeof l.st);
  lcd_clear(&l, 0);
  lcd_draw_line_api(&l, 0, 0, W + 1, 0, 0xFF, 12u << 16);
  lcd_draw_line_api(&l, 0, 0, 0, H + 1, 0xFF, 12u << 16);
  lcd_draw_line_api(&l, W + 1, 0, 0, 0, 0xFF, 12u << 16);
  lcd_draw_line_api(&l, 0, H + 1, 0, 0, 0xFF, 12u << 16);
  lcd_draw_line_api(&l, INT_MAX, 0, 0, 0, 0xFF, 12u << 16);
  CHECK_INT(l.st.rejected, 5);
  CHECK_INT(l.st.lines, 5);
  CHECK_INT(count_color(&l, 0xFFFF), 0);
  lcd_draw_line_api(&l, 0, 0, W, 0, 0xFF, 12u << 16);       /* x == W: clipped, drawn */
  CHECK_INT(count_color(&l, 0xFFFF), W);
  lcd_clear(&l, 0);
  lcd_draw_line_api(&l, -50, 3, 10, 3, 0xFF, 12u << 16);    /* negative: clipped */
  CHECK_INT(count_color(&l, 0xFFFF), 11);
  lcd_clear(&l, 0);
  lcd_draw_line_api(&l, 0, 3, 10, 3, 0x1FF, 12u << 16);     /* pattern & 255 */
  CHECK_INT(count_color(&l, 0xFFFF), 11);
  lcd_draw_line_api(&l, 0, 4, 10, 4, 0x100, 12u << 16);     /* & 255 = 0: nothing */
  CHECK_INT(count_color(&l, 0xFFFF), 11);
  CHECK_INT(l.st.rejected, 5);
  lcd_free(&l);
}

static void test_tri_circle_ref(void) {
  lcd_t l;
  lcd_init(&l, W, H, NULL, NULL);
  uint16_t *ref = calloc(W * H, 2);
  /* random triangles, many with corners far off screen, against the full-loop transcription */
  int bad = 0;
  for (int k = 0; k < 2300; k++) {
    int span = k < 1000 ? 600 : k < 2000 ? 5000 : 100000;
    int x0 = rnd_in(-span, span), y0 = rnd_in(-span, span), x1 = rnd_in(-span, span), y1 = rnd_in(-span, span);
    int x2 = rnd_in(-span, span), y2 = rnd_in(-span, span);
    if (k % 7 == 0) y1 = y0;                       /* flat top / bottom */
    if (k % 11 == 0) y2 = y0 = y1;                 /* one row */
    if (k % 13 == 0) { x1 = x0; y1 = y0; }         /* degenerate */
    int opa = (int)(rnd() % 16);
    uint16_t c = (uint16_t)rnd();
    for (int i = 0; i < W * H; i++) ref[i] = l.fb[i] = (uint16_t)(i * 2654435761u >> 16);
    lcd_tri(&l, x0, y0, x1, y1, x2, y2, c, opa);
    ref_tri(ref, x0, y0, x1, y1, x2, y2, c, opa);
    if (memcmp(ref, l.fb, W * H * 2)) {
      if (!bad) fprintf(stderr, "tri mismatch: %d %d %d %d %d %d opa %d\n", x0, y0, x1, y1, x2, y2, opa);
      bad++;
    }
  }
  CHECK_INT(bad, 0);
  /* the int32 extremes, few rows tall (the full loop stays short) */
  int ext[][6] = { { INT_MIN, 10, INT_MAX, 20, 0, 30 }, { INT_MAX, -5, INT_MIN, 300, 7, 100 },
                   { INT_MIN, 0, INT_MIN, 271, INT_MAX, 271 }, { 0, -100000, 479, 100000, -100000, 5 } };
  for (int k = 0; k < 4; k++) {
    memset(ref, 0, W * H * 2);
    memset(l.fb, 0, W * H * 2);
    lcd_tri(&l, ext[k][0], ext[k][1], ext[k][2], ext[k][3], ext[k][4], ext[k][5], 0xFFFF, 0);
    ref_tri(ref, ext[k][0], ext[k][1], ext[k][2], ext[k][3], ext[k][4], ext[k][5], 0xFFFF, 0);
    CHECK(!memcmp(ref, l.fb, W * H * 2));
  }
  /* very tall: costs only the visible rows (this would take seconds row by row) */
  memset(l.fb, 0, W * H * 2);
  lcd_tri(&l, 0, INT_MIN, 10, INT_MAX, 200, 0, 0xFFFF, 0);
  CHECK(count_color(&l, 0xFFFF) > 0);
  /* a flat triangle is one row from min x to max x */
  memset(l.fb, 0, W * H * 2);
  lcd_tri(&l, 30, 5, 10, 5, 20, 5, 0xFFFF, 0);
  CHECK_INT(count_color(&l, 0xFFFF), 21);
  /* each of the 6 corner orders gives the same pixels */
  uint16_t *first = malloc(W * H * 2);
  int pts[3][2] = { { 10, 10 }, { 100, 40 }, { 30, 90 } }, ord[6][3] = { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 } };
  for (int k = 0; k < 6; k++) {
    memset(l.fb, 0, W * H * 2);
    int *a = pts[ord[k][0]], *b = pts[ord[k][1]], *c = pts[ord[k][2]];
    lcd_tri(&l, a[0], a[1], b[0], b[1], c[0], c[1], 0xFFFF, 0);
    if (!k) memcpy(first, l.fb, W * H * 2);
    else CHECK(!memcmp(first, l.fb, W * H * 2));
  }
  free(first);

  /* filled circles */
  bad = 0;
  for (int k = 0; k < 1500; k++) {
    int r = k < 500 ? rnd_in(-5, 40) : k < 1000 ? rnd_in(0, 400) : rnd_in(0, 50000);
    int cx = rnd_in(-600, 1100), cy = rnd_in(-600, 900);
    uint16_t c = (uint16_t)rnd();
    for (int i = 0; i < W * H; i++) ref[i] = l.fb[i] = (uint16_t)i;
    lcd_filled_circle(&l, cx, cy, r, c);
    ref_filled_circle(ref, cx, cy, r, c);
    if (memcmp(ref, l.fb, W * H * 2)) {
      if (!bad) fprintf(stderr, "circle mismatch: %d %d %d\n", cx, cy, r);
      bad++;
    }
  }
  CHECK_INT(bad, 0);
  memset(l.fb, 0, W * H * 2);
  lcd_filled_circle(&l, 50, 50, 0, 0xFFFF);
  CHECK_INT(count_color(&l, 0xFFFF), 1);
  lcd_filled_circle(&l, 50, 50, -1, 0xFFFF);
  lcd_filled_circle(&l, 50, 50, INT_MIN, 0xFFFF);
  CHECK_INT(count_color(&l, 0xFFFF), 1);
  lcd_filled_circle(&l, 0, 0, INT_MAX, 0xFFFF);    /* only the visible rows are visited */
  CHECK_INT(count_color(&l, 0xFFFF), W * H);
  memset(l.fb, 0, W * H * 2);
  lcd_filled_circle(&l, INT_MIN, INT_MAX, INT_MAX, 0xFFFF);
  lcd_filled_circle(&l, INT_MAX, INT_MIN, 5, 0xFFFF);
  CHECK_INT(count_color(&l, 0xFFFF), 0);
  memset(l.fb, 0, W * H * 2);
  lcd_filled_circle(&l, 100, 100, 3, 0xFFFF);      /* half widths 0 2 2 3 2 2 0: rows 1 5 5 7 5 5 1 */
  CHECK_INT(count_color(&l, 0xFFFF), 29);

  /* outline circles: 360 points, rounded */
  memset(l.fb, 0, W * H * 2);
  lcd_circle(&l, 100, 100, 0, 0xFFFF);
  CHECK_INT(count_color(&l, 0xFFFF), 1);
  memset(l.fb, 0, W * H * 2);
  lcd_circle(&l, 100, 100, 10, 0xFFFF);
  CHECK_INT(px(&l, 110, 100), 0xFFFF); CHECK_INT(px(&l, 90, 100), 0xFFFF);
  CHECK_INT(px(&l, 100, 110), 0xFFFF); CHECK_INT(px(&l, 100, 90), 0xFFFF);
  CHECK_INT(px(&l, 100, 100), 0);
  int n10 = count_color(&l, 0xFFFF);
  memset(l.fb, 0, W * H * 2);
  lcd_circle(&l, 100, 100, -10, 0xFFFF);          /* negative r: the same circle, mirrored */
  CHECK_INT(count_color(&l, 0xFFFF), n10);
  memset(l.fb, 0, W * H * 2);
  lcd_circle(&l, 0, 0, 1, 0xFFFF);                /* cos 60 deg = 0.5000000000000001 rounds to 1 */
  CHECK_INT(px(&l, 1, 1), 0xFFFF);
  CHECK_INT(px(&l, 0, 1), 0xFFFF);
  lcd_circle(&l, INT_MAX, INT_MAX, INT_MAX, 0xFFFF);
  lcd_circle(&l, INT_MIN, INT_MIN, INT_MIN, 0xFFFF);
  lcd_circle(&l, -1, -1, 0, 0xFFFF);
  free(ref);
  lcd_free(&l);
}

/* text drawn through font_glyph by hand: what engine.js put() does */
static void ref_text(lcd_t *l, fonts_t *f, int px_, bool bold, const char *s, int x, int y, uint16_t c) {
  font_face_t *face = fonts_face(f, px_, bold);
  double cx = x;
  int len = (int)strlen(s);
  for (int k = 0; k < len;) {
    const glyph_t *g = font_glyph(f, face, font_utf8_next(s, len, &k));
    if (g->a) {
      int gx = (int)js_round(cx + g->ox), gy = y + g->oy;
      for (int j = 0; j < g->h; j++)
        for (int i = 0; i < g->w; i++) {
          int a = g->a[j * g->w + i], xx = gx + i, yy = gy + j;
          if (!a || xx < 0 || yy < 0 || xx >= l->w || yy >= l->h) continue;
          uint16_t *p = &l->fb[yy * l->w + xx];
          *p = a >= 250 ? c : lcd_blend565(*p, c, a);
        }
    }
    cx += g->adv;
  }
}

static void same_fb(const lcd_t *a, const lcd_t *b, const char *what) {
  int diff = memcmp(a->fb, b->fb, (size_t)a->w * a->h * 2);
  if (diff) fprintf(stderr, "frame buffers differ: %s\n", what);
  CHECK(!diff);
}

static void test_text_stub(void) {
  fonts_t *f = fonts_open_stub(7.3f);
  lcd_t l;
  lcd_init(&l, W, H, f, NULL);
  int w = -1, h = -1;
  /* measure: round(n * 7.3f); STD 16 px: lh 19 */
  lcd_draw_text(&l, 100, 50, "abc", 3, 0, &w, &h);
  CHECK_INT(w, 22); CHECK_INT(h, 19);
  CHECK_INT(count_color(&l, 0), W * H);            /* stub glyphs draw nothing */
  lcd_draw_text(&l, 100, 50, "abc", -1, 0, &w, &h);
  CHECK_INT(w, 22);
  lcd_draw_text(&l, 100, 50, NULL, 5, 0, &w, &h);
  CHECK_INT(w, 0);
  lcd_draw_text(&l, 100, 50, "abc", 3, 0, NULL, NULL);
  /* INVERS: a box from x - 1, w + 2 wide, lh tall, in the color; the text in white */
  lcd_draw_text(&l, 100, 50, "abc", 3, LCDF_INVERS | 0xF8008000u, &w, &h);
  CHECK_INT(count_color(&l, 0xF800), 24 * 19);
  CHECK_INT(px(&l, 99, 50), 0xF800); CHECK_INT(px(&l, 122, 68), 0xF800);
  CHECK_INT(px(&l, 98, 50), 0); CHECK_INT(px(&l, 123, 50), 0); CHECK_INT(px(&l, 99, 69), 0);
  /* RIGHT: x - w; CENTER: x - w / 2 rounded with halves up; VCENTER: y - h / 2 */
  struct { uint32_t fl; int x, y, ex, ey; } al[] = {
    { LCDF_RIGHT, 100, 50, 78, 50 }, { LCDF_CENTER, 100, 50, 89, 50 }, { LCDF_RIGHT | LCDF_CENTER, 100, 50, 78, 50 },
    { LCDF_VCENTER, 100, 50, 100, 41 }, { LCDF_CENTER | LCDF_VCENTER, 100, 50, 89, 41 },
    { LCDF_CENTER, -100, -50, -111, -50 }, { LCDF_VCENTER, 0, -50, 0, -59 },
  };
  for (int k = 0; k < (int)(sizeof al / sizeof al[0]); k++) {
    lcd_clear(&l, 0);
    lcd_draw_text(&l, al[k].x, al[k].y, "abc", 3, al[k].fl | LCDF_INVERS | 0xF8008000u, NULL, NULL);
    lcd_t r;
    lcd_init(&r, W, H, NULL, NULL);
    lcd_fill_rect(&r, al[k].ex - 1, al[k].ey, 24, 19, 0xF800, 0);
    same_fb(&l, &r, "stub alignment");
    lcd_free(&r);
  }
  /* 22 / 2 = 11 exactly; with 3 glyphs of 2.5 px: w = round(7.5) = 8, 100 - 4 = 96; 5 px glyphs
   * w 15: 100 - 7.5 = 92.5 -> 93, and -100 - 7.5 = -107.5 -> -107 (halves up, also negative) */
  fonts_t *f5 = fonts_open_stub(5.0f);
  l.fonts = f5;
  lcd_clear(&l, 0);
  lcd_draw_text(&l, 100, 50, "abc", 3, LCDF_CENTER | LCDF_INVERS | 0xF8008000u, &w, NULL);
  CHECK_INT(w, 15);
  CHECK_INT(px(&l, 92, 50), 0xF800); CHECK_INT(px(&l, 91, 50), 0);
  lcd_clear(&l, 0);
  lcd_draw_text(&l, 4, 50, "abc", 3, LCDF_CENTER | LCDF_INVERS | 0xF8008000u, &w, NULL);  /* -3.5 -> -3: box -4..12 */
  CHECK_INT(px(&l, 12, 50), 0xF800); CHECK_INT(px(&l, 13, 50), 0);
  /* odd line heights: VCENTER of lh 19 = 9.5 -> y - 9.5 rounds up */
  lcd_clear(&l, 0);
  lcd_draw_text(&l, 10, 20, "a", 1, LCDF_VCENTER | LCDF_INVERS | 0xF8008000u, NULL, &h);
  CHECK_INT(h, 19);
  CHECK_INT(px(&l, 10, 11), 0xF800); CHECK_INT(px(&l, 10, 10), 0); CHECK_INT(px(&l, 10, 29), 0xF800);
  l.fonts = f;
  fonts_close(f5);

  /* font sizes: (flags >> 8) & 15 picks the class size; 8..15 are 16 px regular */
  for (int i = 0; i < 16; i++) {
    lcd_draw_text(&l, 0, 0, "x", 1, (uint32_t)i << 8, NULL, &h);
    int p = i < 8 ? l.cls.px[i] : 16;
    CHECK_INT(h, (int)(p * 1.17 + 0.5));
    int sw, sh;
    lcd_size_text(&l, "x", 1, (uint32_t)i << 8, &sw, &sh);
    CHECK_INT(sw, 7);
    CHECK_INT(sh, i < 8 ? l.cls.line_h[i] : 19);   /* LINE_H, else the measured height */
  }
  int sw = -1, sh = -1;
  lcd_size_text(&l, "", 0, 0, &sw, &sh);
  CHECK_INT(sw, 0); CHECK_INT(sh, 21);
  lcd_size_text(&l, NULL, 0, 0x500, &sw, &sh);
  CHECK_INT(sw, 0); CHECK_INT(sh, 40);
  lcd_size_text(&l, "abc", -1, 0x300, &sw, &sh);
  CHECK_INT(sw, 22); CHECK_INT(sh, 17);
  lcd_size_text(&l, "abc", 3, 0, NULL, NULL);

  /* a far-away position does not overflow */
  lcd_clear(&l, 0);
  lcd_draw_text(&l, INT_MIN, INT_MIN, "abc", 3, LCDF_RIGHT | LCDF_VCENTER | LCDF_INVERS | LCDF_SHADOWED, NULL, NULL);
  lcd_draw_text(&l, INT_MAX, INT_MAX, "abc", 3, LCDF_INVERS | LCDF_SHADOWED, NULL, NULL);
  CHECK_INT(count_color(&l, 0), W * H);

  /* stats: texts and glyphs (SHADOWED walks the glyphs twice) */
  memset(&l.st, 0, sizeof l.st);
  lcd_draw_text(&l, 0, 0, "ab\xc2\xb0", -1, LCDF_SHADOWED, NULL, NULL);
  CHECK_INT(l.st.texts, 1);
  CHECK_INT(l.st.glyphs, 6);

  /* no fonts: nothing, zero size */
  lcd_t n;
  font_class_t std;
  font_class_for(&std, 0, 0, "std");
  lcd_init(&n, 10, 10, NULL, &std);
  lcd_draw_text(&n, 0, 0, "abc", 3, LCDF_INVERS, &w, &h);
  CHECK_INT(w, 0); CHECK_INT(h, 0);
  lcd_size_text(&n, "abc", 3, 0, &w, &h);
  CHECK_INT(w, 0); CHECK_INT(h, 21);
  CHECK_INT(count_color(&n, 0), 100);
  lcd_free(&n);
  lcd_free(&l);
  fonts_close(f);
}

static void test_text_roboto(void) {
  char err[200];
  fonts_t *f = fonts_open("host/fonts", err, sizeof err);
  CHECK(f != NULL);
  if (!f) { fprintf(stderr, "%s\n", err); return; }
  lcd_t a, b;
  font_class_t cls;
  font_class_for(&cls, 640, 480, NULL);
  lcd_init(&a, 640, 480, f, &cls);
  lcd_init(&b, 640, 480, f, &cls);
  /* glyphs land where engine.js puts them: round(pen + ox), top + oy, blended below alpha 250 */
  static const char *texts[] = { "StickTime 1.7.2", "Race (10 laps, 3 AI)", "\xc2\xb0/s -12.5", "Wg|.~", "" };
  static const int xs[] = { 10, -7, 630, 300, 50 }, ys[] = { 10, -5, 470, 200, 470 };
  for (int k = 0; k < 5; k++)
    for (int size = 0; size < 8; size++) {
      uint32_t fl = (uint32_t)size << 8 | 0xFFE08000u;
      lcd_clear(&a, 0x0010); lcd_clear(&b, 0x0010);
      lcd_draw_text(&a, xs[k], ys[k], texts[k], -1, fl, NULL, NULL);
      ref_text(&b, f, cls.px[size], FONT_BOLD[size], texts[k], xs[k], ys[k], 0xFFE0);
      same_fb(&a, &b, texts[k]);
      /* SHADOWED: black at +1,+1 first */
      lcd_clear(&a, 0x0010); lcd_clear(&b, 0x0010);
      lcd_draw_text(&a, xs[k], ys[k], texts[k], -1, fl | LCDF_SHADOWED, NULL, NULL);
      ref_text(&b, f, cls.px[size], FONT_BOLD[size], texts[k], xs[k] + 1, ys[k] + 1, 0);
      ref_text(&b, f, cls.px[size], FONT_BOLD[size], texts[k], xs[k], ys[k], 0xFFE0);
      same_fb(&a, &b, "shadowed");
    }
  /* INVERS with real glyphs: white text on the color box */
  lcd_clear(&a, 0); lcd_clear(&b, 0);
  int w, h;
  lcd_draw_text(&a, 100, 100, "Menu", 4, LCDF_INVERS | (3u << 16), &w, &h);
  lcd_fill_rect(&b, 99, 100, w + 2, h, lcd_color_of(3u << 16), 0);
  ref_text(&b, f, cls.px[0], false, "Menu", 100, 100, 0xFFFF);
  same_fb(&a, &b, "invers");
  /* something visible was drawn, and every glyph pixel is inside the measured box */
  lcd_clear(&a, 0);
  lcd_draw_text(&a, 50, 60, "Hello", 5, 12u << 16, &w, &h);
  int minx = 9999, maxx = -1, miny = 9999, maxy = -1;
  for (int y = 0; y < 480; y++)
    for (int x = 0; x < 640; x++)
      if (a.fb[y * 640 + x]) { if (x < minx) minx = x; if (x > maxx) maxx = x; if (y < miny) miny = y; if (y > maxy) maxy = y; }
  CHECK(maxx > minx && maxy > miny);
  CHECK(minx >= 50 && maxx < 50 + w + 1 && miny >= 60 && maxy < 60 + h);

  /* drawNumber / drawTimer print what engine.js prints (toFixed, padStart) */
  struct { int v; uint32_t fl; const char *s; } nums[] = {
    { 0, 0, "0" }, { 7, 0, "7" }, { -7, 0, "-7" }, { 7, LCDF_LEADING0, "7" }, { 12, LCDF_PREC1, "1.2" },
    { -5, LCDF_PREC1, "-0.5" }, { 5, LCDF_PREC1, "0.5" }, { 0, LCDF_PREC1, "0.0" }, { -1, LCDF_PREC2, "-0.01" },
    { 5, LCDF_PREC2, "0.05" }, { -15, LCDF_PREC2, "-0.15" }, { 0, LCDF_PREC2, "0.00" }, { 1000, LCDF_PREC2, "10.00" },
    { 2147483647, 0, "2147483647" }, { INT_MIN, 0, "-2147483648" }, { 2147483647, LCDF_PREC2, "21474836.47" },
    { INT_MIN, LCDF_PREC1, "-214748364.8" }, { INT_MIN, LCDF_PREC2, "-21474836.48" }, { 99, LCDF_PREC1, "9.9" },
    { -100, LCDF_PREC1, "-10.0" },
  };
  for (int k = 0; k < (int)(sizeof nums / sizeof nums[0]); k++) {
    uint32_t fl = nums[k].fl | 0x0400 | LCDF_CENTER | (12u << 16);
    lcd_clear(&a, 0); lcd_clear(&b, 0);
    lcd_draw_number(&a, 320, 240, nums[k].v, fl);
    lcd_draw_text(&b, 320, 240, nums[k].s, -1, fl, NULL, NULL);
    same_fb(&a, &b, nums[k].s);
    lcd_clear(&b, 0);
    lcd_draw_text(&b, 320, 240, "8", -1, fl, NULL, NULL);
    CHECK(memcmp(a.fb, b.fb, 640 * 480 * 2) != 0);  /* the check can fail */
  }
  struct { int v; const char *s; } tim[] = {
    { 0, "00:00" }, { 59, "00:59" }, { 60, "01:00" }, { 61, "01:01" }, { -61, "-01:01" }, { -1, "-00:01" },
    { 3599, "59:59" }, { 6000, "100:00" }, { INT_MAX, "35791394:07" }, { INT_MIN, "-35791394:08" },
  };
  for (int k = 0; k < (int)(sizeof tim / sizeof tim[0]); k++) {
    lcd_clear(&a, 0); lcd_clear(&b, 0);
    lcd_draw_timer(&a, 320, 240, tim[k].v, LCDF_RIGHT | (12u << 16));
    lcd_draw_text(&b, 320, 240, tim[k].s, -1, LCDF_RIGHT | (12u << 16), NULL, NULL);
    same_fb(&a, &b, tim[k].s);
  }
  lcd_free(&a);
  lcd_free(&b);
  fonts_close(f);
}

/* drawing = multi: the calls recorded and drawn in bands on a pool give the same pixels and counts as
 * drawing at once, for every kind of call (text with Roboto: glyph masks), frame after frame */
static const char *const REC_TEXT[] = { "StickTime", "Gr\xC3\xBC\xC3\x9F" "e 25\xC2\xB0", "Lap 3/3  1:02.45", "\xC5\xBD" "aba \xC4\x8D" "ok",
                                        "", "W", "Race (3 laps, 2 AI)" };

static void rec_call(lcd_t *l, const int *v) {
  uint16_t c = (uint16_t)v[5];
  switch (v[0]) {
    case 0: lcd_fill_rect(l, v[1], v[2], v[3], v[4], c, v[6]); break;
    case 1: lcd_hline(l, v[1], v[2], v[3], c, v[6]); break;
    case 2: lcd_line(l, v[1], v[2], v[3] * 3 - 200, v[4] * 2 - 100, (uint8_t)v[7], c); break;
    case 3: lcd_tri(l, v[1], v[2], v[3] * 2 - 100, v[4], v[8], v[9], c, v[6]); break;
    case 4: lcd_point(l, v[1], v[2], c); break;
    case 5: lcd_filled_circle(l, v[1], v[2], v[3] / 6, c); break;
    case 6: lcd_circle(l, v[1], v[2], v[3] / 6, c); break;
    case 7: lcd_invert_rect(l, v[1], v[2], v[3], v[4]); break;
    case 8: lcd_draw_text(l, v[1], v[2], REC_TEXT[v[7] % 7], -1, (uint32_t)v[10], NULL, NULL); break;
    case 9: lcd_draw_number(l, v[1], v[2], v[3] * 37 - 900, (uint32_t)v[10]); break;
    case 10: lcd_draw_rectangle(l, v[1], v[2], v[3], v[4], (uint32_t)v[10], v[7] & 7, v[6]); break;
    case 11: lcd_draw_gauge(l, v[1], v[2], v[3], v[4] / 8 + 1, v[8], v[9] % 50, (uint32_t)v[10]); break;
    case 12: lcd_draw_line_api(l, v[1], v[2], v[3] * 2, v[4] * 2, v[7], (uint32_t)v[10]); break;
    case 13: lcd_draw_timer(l, v[1], v[2], v[3] * 61, (uint32_t)v[10]); break;
    default: lcd_clear(l, c); break;
  }
}

/* lcd_flush_bands: every band once, on rows aligned as asked, covering the screen; the pixels as ever */
static int seen[64][3];
static int nseen;
static void band_done(void *ctx, int band, int bands, int row0, int row1) {
  (void)ctx;
  int i = __atomic_fetch_add(&nseen, 1, __ATOMIC_ACQ_REL);
  if (i < 64) { seen[i][0] = band; seen[i][1] = row0; seen[i][2] = row1; }
  (void)bands;
}

static void test_flush_bands(par_t *pool, fonts_t *f, const font_class_t *cls) {
  lcd_t a, b;
  lcd_init(&a, W, H, f, cls);
  lcd_init(&b, W, H, f, cls);
  CHECK(!lcd_flush_bands(&b, 2, band_done, NULL));           /* no pool: nothing done */
  CHECK_INT(lcd_set_pool(&b, pool), 0);
  for (int k = 0; k < 300; k++) {
    int x = rnd_in(-50, W + 50), y = rnd_in(-50, H + 50), w = rnd_in(0, 200), h = rnd_in(0, 200);
    uint16_t c = (uint16_t)rnd();
    int opa = rnd_in(0, 15);
    lcd_fill_rect(&a, x, y, w, h, c, opa);
    lcd_fill_rect(&b, x, y, w, h, c, opa);
    lcd_line(&a, x, y, x + w * 2, y - h, 0xFF, c);
    lcd_line(&b, x, y, x + w * 2, y - h, 0xFF, c);
  }
  int bands = lcd_bands(&b);
  nseen = 0;
  CHECK(lcd_flush_bands(&b, 2, band_done, NULL));
  CHECK(!memcmp(a.fb, b.fb, sizeof *a.fb * W * H));
  CHECK_INT(nseen, bands);
  int covered = 0, once = 1;
  for (int i = 0; i < nseen && i < 64; i++) {
    CHECK(seen[i][1] % 2 == 0 && seen[i][2] % 2 == 0 && seen[i][2] - seen[i][1] >= 4);
    covered += seen[i][2] - seen[i][1];
    for (int j = 0; j < i; j++) once &= seen[j][0] != seen[i][0];
  }
  CHECK_INT(covered, H);
  CHECK(once);
  CHECK(!lcd_flush_bands(&b, 7, band_done, NULL));           /* 272 rows aren't a multiple of 7 */
  lcd_free(&a);
  lcd_free(&b);
}

static void test_recorded(void) {
  char err[256];
  fonts_t *f = fonts_open("host/fonts", err, sizeof err);
  CHECK(f != NULL);
  if (!f) return;
  font_class_t cls;
  font_class_for(&cls, W, H, NULL);
  par_t *pool = par_open(4);
  CHECK(pool != NULL);
  lcd_t a, b;
  lcd_init(&a, W, H, f, &cls);
  lcd_init(&b, W, H, f, &cls);
  CHECK_INT(lcd_set_pool(&b, pool), 0);
  int bad = 0;
  for (int frame = 0; frame < 60; frame++) {
    memset(&a.st, 0, sizeof a.st);
    memset(&b.st, 0, sizeof b.st);
    a.line_w = b.line_w = frame % 3 == 2 ? 2 : 1;        /* a doubled render's lines too */
    int calls = rnd_in(1, 400);
    for (int n = 0; n < calls; n++) {
      int v[11];
      v[0] = rnd_in(0, 99) < 2 ? 14 : rnd_in(0, 13);                   /* clears now and then */
      v[1] = rnd_in(-60, W + 60);
      v[2] = rnd_in(-60, H + 60);
      v[3] = rnd_in(-80, 300);
      v[4] = rnd_in(-80, 300);
      v[5] = (int)(rnd() & 0xFFFF);
      v[6] = rnd_in(0, 15);
      v[7] = rnd_in(0, 255);
      v[8] = rnd_in(-60, W + 60);
      v[9] = rnd_in(-60, H + 60);
      v[10] = (int)((rnd() & (LCDF_INVERS | LCDF_VCENTER | LCDF_CENTER | LCDF_RIGHT | LCDF_SHADOWED)) |
                    ((uint32_t)rnd_in(0, 7) << 8) | (rnd() & 1 ? LCDF_RGB | (rnd() << 16) : (uint32_t)rnd_in(0, 30) << 16));
      rec_call(&a, v);
      rec_call(&b, v);
    }
    lcd_flush(&b);
    bad += memcmp(a.fb, b.fb, sizeof *a.fb * W * H) != 0;
    bad += memcmp(&a.st, &b.st, sizeof a.st) != 0;
  }
  CHECK_INT(bad, 0);
  /* lines: a band starts at its first step of the line (any slope, direction, pattern, partly off; 1 and
   * 2 pixels wide) */
  for (int k = 0; k < 3000; k++) {
    a.line_w = b.line_w = k < 1500 ? 1 : 2;
    if (k % 300 == 0) {
      lcd_clear(&a, 0);
      lcd_clear(&b, 0);
    }
    int x1 = rnd_in(-200, W + 200), y1 = rnd_in(-200, H + 200), x2 = rnd_in(-200, W + 200), y2 = rnd_in(-200, H + 200);
    uint8_t pat = k & 1 ? 0xFF : (uint8_t)rnd();
    uint16_t c = (uint16_t)(rnd() | 1);
    lcd_line(&a, x1, y1, x2, y2, pat, c);
    lcd_line(&b, x1, y1, x2, y2, pat, c);
    if (k % 300 == 299) {
      lcd_flush(&b);
      bad += memcmp(a.fb, b.fb, sizeof *a.fb * W * H) != 0;
    }
  }
  CHECK_INT(bad, 0);
  /* more code points than a face's cache holds, in one frame: glyphs evicted while recorded are drawn */
  char many[3000];
  int len = 0;
  for (uint32_t cp = 0x400; cp < 0x400 + 400; cp++) {
    many[len++] = (char)(0xC0 | (cp >> 6));
    many[len++] = (char)(0x80 | (cp & 63));
  }
  lcd_clear(&a, 0);
  lcd_clear(&b, 0);
  for (int k = 0; k < 6; k++) {
    lcd_draw_text(&a, -k * 700, k * 40, many, len, ((uint32_t)k << 8) | (12u << 16), NULL, NULL);
    lcd_draw_text(&b, -k * 700, k * 40, many, len, ((uint32_t)k << 8) | (12u << 16), NULL, NULL);
  }
  lcd_flush(&b);
  CHECK(!memcmp(a.fb, b.fb, sizeof *a.fb * W * H));
  CHECK(count_color(&b, 0) < W * H);
  test_flush_bands(pool, f, &cls);
  /* dropping the pool draws what was recorded; drawing goes on at once */
  lcd_fill_rect(&a, 10, 10, 50, 50, 0xF800, 0);
  lcd_fill_rect(&b, 10, 10, 50, 50, 0xF800, 0);
  CHECK_INT(lcd_set_pool(&b, NULL), 0);
  CHECK(!memcmp(a.fb, b.fb, sizeof *a.fb * W * H));
  lcd_line(&b, 0, 0, W - 1, H - 1, 0xFF, 0x07E0);
  CHECK_INT(px(&b, W - 1, H - 1), 0x07E0);
  lcd_free(&a);
  lcd_free(&b);
  par_close(pool);
  fonts_close(f);
}

int main(void) {
  test_colors();
  test_blend();
  test_rects();
  test_lines();
  test_tri_circle_ref();
  test_text_stub();
  test_text_roboto();
  test_recorded();
  T_DONE();
}
