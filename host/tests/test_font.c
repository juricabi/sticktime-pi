/* font.c: screen classes, UTF-8 decoding, engine.js metrics (stub and Roboto), the glyph cache. */
#include "t.h"
#include "font.h"

#include <stdint.h>

static void check_class(const font_class_t *c, const char *name, const int px[8], const int lh[8]) {
  CHECK_STR(c->name, name);
  for (int i = 0; i < FONT_SIZES; i++) {
    CHECK_INT(c->px[i], px[i]);
    CHECK_INT(c->line_h[i], lh[i]);
  }
}

static const int STD_PX[8] = { 16, 16, 9, 13, 24, 32, 64, 48 }, STD_LH[8] = { 21, 20, 12, 17, 29, 40, 69, 51 };
static const int SML_PX[8] = { 13, 13, 8, 10, 19, 25, 48, 36 }, SML_LH[8] = { 17, 16, 10, 14, 23, 33, 52, 38 };
static const int LRG_PX[8] = { 22, 22, 12, 18, 33, 44, 88, 66 }, LRG_LH[8] = { 27, 28, 17, 23, 42, 55, 95, 70 };
static const int PC_PX[8] = { 32, 32, 18, 26, 48, 64, 128, 96 }, PC_LH[8] = { 42, 40, 24, 34, 58, 80, 138, 102 };
static const int HD_PX[8] = { 48, 48, 27, 39, 72, 96, 192, 144 }, HD_LH[8] = { 63, 60, 36, 51, 87, 120, 207, 153 };

static void test_classes(void) {
  font_class_t c;
  static const bool bold[8] = { false, true, false, false, false, true, true, true };
  for (int i = 0; i < FONT_SIZES; i++) CHECK_INT(FONT_BOLD[i], bold[i]);

  /* by name, whatever the size */
  font_class_for(&c, 100, 100, "std"); check_class(&c, "std", STD_PX, STD_LH);
  font_class_for(&c, 1920, 1080, "sml"); check_class(&c, "sml", SML_PX, SML_LH);
  font_class_for(&c, 480, 272, "lrg"); check_class(&c, "lrg", LRG_PX, LRG_LH);
  font_class_for(&c, 640, 480, "pc"); check_class(&c, "pc", PC_PX, PC_LH);
  font_class_for(&c, 640, 480, "hd"); check_class(&c, "hd", HD_PX, HD_LH);
  /* the tables are what engine.js says: pc = 2 x std line heights, hd = 3 x */
  for (int i = 0; i < FONT_SIZES; i++) {
    CHECK_INT(PC_LH[i], 2 * STD_LH[i]);
    CHECK_INT(HD_LH[i], 3 * STD_LH[i]);
  }

  /* auto: the emulator's screens */
  font_class_for(&c, 1280, 720, NULL); check_class(&c, "pc", PC_PX, PC_LH);
  font_class_for(&c, 1920, 1080, "auto"); check_class(&c, "hd", HD_PX, HD_LH);
  font_class_for(&c, 960, 720, NULL); CHECK_STR(c.name, "pc");
  font_class_for(&c, 1440, 1080, NULL); CHECK_STR(c.name, "hd");
  font_class_for(&c, 640, 480, NULL); check_class(&c, "lrg", LRG_PX, LRG_LH);
  font_class_for(&c, 800, 480, NULL); check_class(&c, "lrg", LRG_PX, LRG_LH);
  font_class_for(&c, 854, 480, NULL); check_class(&c, "lrg", LRG_PX, LRG_LH);
  font_class_for(&c, 480, 272, NULL); check_class(&c, "std", STD_PX, STD_LH);
  font_class_for(&c, 480, 320, NULL); check_class(&c, "std", STD_PX, STD_LH);
  font_class_for(&c, 320, 480, NULL); check_class(&c, "std", STD_PX, STD_LH);   /* NV14 */
  font_class_for(&c, 320, 240, NULL); check_class(&c, "sml", SML_PX, SML_LH);
  font_class_for(&c, 427, 240, NULL); check_class(&c, "sml", SML_PX, SML_LH);
  /* unknown names fall back to auto */
  font_class_for(&c, 640, 480, "huge"); CHECK_STR(c.name, "lrg");
  font_class_for(&c, 640, 480, ""); CHECK_STR(c.name, "lrg");

  /* other heights: std x H / 272 x 0.78, rounded (PAL 576 values from the research notes) */
  static const int PAL_PX[8] = { 26, 26, 15, 21, 40, 53, 106, 79 }, PAL_LH[8] = { 35, 33, 20, 28, 48, 66, 114, 84 };
  font_class_for(&c, 768, 576, NULL); check_class(&c, "custom", PAL_PX, PAL_LH);
  font_class_for(&c, 1024, 576, "auto"); check_class(&c, "custom", PAL_PX, PAL_LH);
  /* 288 (PAL progressive): k = 0.8259 */
  static const int P288_PX[8] = { 13, 13, 7, 11, 20, 26, 53, 40 }, P288_LH[8] = { 17, 17, 10, 14, 24, 33, 57, 42 };
  font_class_for(&c, 384, 288, NULL); check_class(&c, "custom", P288_PX, P288_LH);
  /* tiny and silly heights stay usable */
  font_class_for(&c, 100, 10, NULL);
  for (int i = 0; i < FONT_SIZES; i++) { CHECK(c.px[i] >= 1); CHECK(c.line_h[i] >= 1); }
  font_class_for(&c, 0, 0, NULL); check_class(&c, "custom", (const int[8]){ 12, 12, 7, 10, 19, 25, 50, 37 },
                                              (const int[8]){ 16, 16, 9, 13, 23, 31, 54, 40 });
  font_class_for(&c, 100, -5, NULL); CHECK_STR(c.name, "custom");
  font_class_for(&c, 100000, 100000, NULL);
  for (int i = 0; i < FONT_SIZES; i++) CHECK(c.px[i] <= 1024);
}

/* decode all of s, return the count; cps gets the code points */
static int decode(const char *s, int len, uint32_t *cps, int max) {
  int n = 0, i = 0;
  while (i < len) {
    int before = i;
    uint32_t c = font_utf8_next(s, len, &i);
    if (i <= before) { cps[n++] = 0xDEAD; break; }   /* must always advance */
    if (n < max) cps[n] = c;
    n++;
  }
  return n;
}

#define DEC(str, ...) do { \
  static const uint32_t want[] = { __VA_ARGS__ }; uint32_t got[32]; \
  int n = decode(str, (int)sizeof(str) - 1, got, 32); \
  CHECK_INT(n, (int)(sizeof want / sizeof want[0])); \
  for (int k = 0; k < n && k < (int)(sizeof want / sizeof want[0]); k++) CHECK_INT(got[k], want[k]); \
} while (0)

static void test_utf8(void) {
  DEC("A", 'A');
  DEC("Az~\x7f", 'A', 'z', '~', 0x7F);
  DEC("\xc2\xb0", 0xB0);                          /* degree */
  DEC("\xc4\x8c\xc4\x87\xc5\xbe\xc5\xa1\xc4\x91", 0x10C, 0x107, 0x17E, 0x161, 0x111);   /* Čćžšđ */
  DEC("\xe2\x82\xac", 0x20AC);                    /* euro */
  DEC("\xef\xbf\xbd", 0xFFFD);
  DEC("\xf0\x9f\x98\x80", 0x1F600);               /* one code point, like JS for..of */
  DEC("\xf4\x8f\xbf\xbf", 0x10FFFF);
  DEC("\xdf\xbf\xe0\xa0\x80", 0x7FF, 0x800);      /* boundaries */
  DEC("\xed\x9f\xbf\xee\x80\x80", 0xD7FF, 0xE000);
  /* invalid: one U+FFFD per maximal subpart (WHATWG / TextDecoder) */
  DEC("\x80", 0xFFFD);
  DEC("\xbf\x80", 0xFFFD, 0xFFFD);
  DEC("\xc0\x80", 0xFFFD, 0xFFFD);                /* overlong NUL */
  DEC("\xc1\xbf", 0xFFFD, 0xFFFD);
  DEC("\xe0\x80\x80", 0xFFFD, 0xFFFD, 0xFFFD);    /* overlong 3-byte */
  DEC("\xf0\x80\x80\x80", 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD);
  DEC("\xed\xa0\x80", 0xFFFD, 0xFFFD, 0xFFFD);    /* surrogate */
  DEC("\xf4\x90\x80\x80", 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD);   /* > U+10FFFF */
  DEC("\xf5\x80", 0xFFFD, 0xFFFD);
  DEC("\xff\xfe", 0xFFFD, 0xFFFD);
  DEC("\xe2\x82", 0xFFFD);                        /* truncated at the end: one */
  DEC("\xf0\x9f\x98", 0xFFFD);
  DEC("\xe2\x82" "A", 0xFFFD, 'A');               /* the bad byte is not swallowed */
  DEC("\xc4" "A\xc4", 0xFFFD, 'A', 0xFFFD);       /* a name cut at 16 bytes in the middle of Č */
  DEC("a\0b", 'a', 0, 'b');                       /* embedded NUL */

  /* i at or past the end: U+FFFD, and i moves to len (no endless loops) */
  int i = 3;
  CHECK_INT(font_utf8_next("abc", 3, &i), 0xFFFD);
  CHECK_INT(i, 3);
  i = 0;
  CHECK_INT(font_utf8_next("", 0, &i), 0xFFFD);
  i = -2;
  CHECK_INT(font_utf8_next("x", 1, &i), 'x');
  CHECK_INT(i, 1);
  /* a sequence that runs past len is truncated even if the bytes are there */
  i = 0;
  CHECK_INT(font_utf8_next("\xc2\xb0", 1, &i), 0xFFFD);
  CHECK_INT(i, 1);
}

static void test_stub(void) {
  fonts_t *f = fonts_open_stub(7.3f);
  CHECK(f != NULL);
  if (!f) return;
  font_face_t *a = fonts_face(f, 16, false);
  CHECK(a != NULL);
  CHECK(fonts_face(f, 16, false) == a);
  CHECK(fonts_face(f, 16, true) != a);
  CHECK_INT(font_asc(a), 15);
  CHECK_INT(font_lh(a), 19);
  /* Math.round(px * 0.93), Math.round(px * 1.17), with JS's halves */
  static const int px[] = { 1, 8, 9, 13, 22, 32, 33, 44, 48, 50, 66, 88, 128, 150, 192, 850 };
  static const int asc[] = { 1, 7, 8, 12, 20, 30, 31, 41, 45, 47, 61, 82, 119, 140, 179, 791 };
  static const int lh[] = { 1, 9, 11, 15, 26, 37, 39, 51, 56, 59, 77, 103, 150, 176, 225, 994 };
  for (int k = 0; k < (int)(sizeof px / sizeof px[0]); k++) {
    font_face_t *fc = fonts_face(f, px[k], k & 1);
    CHECK_INT(font_asc(fc), asc[k]);
    CHECK_INT(font_lh(fc), lh[k]);
  }
  /* sum of float advances, rounded: 7.3f = 7.300000190734863 */
  int w = -1, h = -1;
  a = fonts_face(f, 16, false);   /* the loop above asked for more sizes than there are slots */
  font_measure(f, a, "", 0, &w, &h); CHECK_INT(w, 0); CHECK_INT(h, 19);
  font_measure(f, a, "a", 1, &w, &h); CHECK_INT(w, 7);
  font_measure(f, a, "ab", 2, &w, &h); CHECK_INT(w, 15);
  font_measure(f, a, "abc", 3, &w, &h); CHECK_INT(w, 22);
  font_measure(f, a, "abcde", 5, &w, &h); CHECK_INT(w, 37);
  font_measure(f, a, "abcdefghij", 10, &w, &h); CHECK_INT(w, 73);
  font_measure(f, a, "abc", -1, &w, &h); CHECK_INT(w, 22);       /* len < 0: NUL-terminated */
  font_measure(f, a, NULL, 5, &w, &h); CHECK_INT(w, 0);
  font_measure(f, a, "abc", 3, NULL, NULL);                       /* NULL outputs are fine */
  /* every code point counts once, control characters and invalid bytes too (like the JS stub) */
  font_measure(f, a, "\xc2\xb0\n\x01", 4, &w, &h); CHECK_INT(w, 22);
  font_measure(f, a, "\xe2\x82" "A", 3, &w, &h); CHECK_INT(w, 15);
  font_measure(f, a, "\xf0\x9f\x98\x80", 4, &w, &h); CHECK_INT(w, 7);
  const glyph_t *g = font_glyph(f, a, 'W');
  CHECK(g->a == NULL);
  CHECK_INT(g->w, 0);
  CHECK_NEAR(g->adv, 7.3, 1e-6);
  fonts_close(f);

  /* halves round up: 2.5 -> 3, 3 x 0.5 = 1.5 -> 2 */
  f = fonts_open_stub(2.5f);
  font_measure(f, fonts_face(f, 16, false), "x", 1, &w, &h); CHECK_INT(w, 3);
  fonts_close(f);
  f = fonts_open_stub(0.5f);
  font_measure(f, fonts_face(f, 16, false), "xyz", 3, &w, &h); CHECK_INT(w, 2);
  font_measure(f, fonts_face(f, 16, false), "x", 1, &w, &h); CHECK_INT(w, 1);
  fonts_close(f);
  f = fonts_open_stub(0.0f);
  font_measure(f, fonts_face(f, 16, false), "xyz", 3, &w, &h); CHECK_INT(w, 0);
  fonts_close(f);
  fonts_close(NULL);

  /* px out of range is clamped, never NULL */
  f = fonts_open_stub(1);
  font_face_t *z = fonts_face(f, 0, false), *n = fonts_face(f, -7, false), *big = fonts_face(f, 100000, true);
  CHECK(z && n && big);
  CHECK(z == n);
  CHECK_INT(font_asc(z), 1);
  CHECK_INT(font_lh(big), 1198);   /* px 1024 */
  fonts_close(f);
}

static void test_roboto(void) {
  char err[256] = "";
  fonts_t *bad = fonts_open("host/no-such-dir", err, sizeof err);
  CHECK(bad == NULL);
  CHECK(strstr(err, "Roboto-Regular.ttf") != NULL);
  bad = fonts_open("host/tests", err, 4);   /* short error buffer */
  CHECK(bad == NULL);
  CHECK(strlen(err) <= 3);

  fonts_t *f = fonts_open("host/fonts", err, sizeof err);
  CHECK(f != NULL);
  if (!f) { fprintf(stderr, "fonts_open: %s\n", err); return; }
  font_face_t *r16 = fonts_face(f, 16, false), *r22 = fonts_face(f, 22, false), *b16 = fonts_face(f, 16, true);
  font_face_t *b22 = fonts_face(f, 22, true), *r32 = fonts_face(f, 32, false), *r13 = fonts_face(f, 13, false);
  CHECK_INT(font_asc(r16), 15);
  CHECK_INT(font_lh(r22), 26);
  int w, h;
  /* Roboto's advances (2048 units/em) x px / 2048, summed and rounded like measure() */
  font_measure(f, r16, "StickTime", -1, &w, &h); CHECK_INT(w, 71); CHECK_INT(h, 19);
  font_measure(f, r22, "StickTime", -1, &w, &h); CHECK_INT(w, 98);
  font_measure(f, r22, "Race (10 laps, 3 AI)", -1, &w, &h); CHECK_INT(w, 189);
  font_measure(f, r32, "Hello, World!", -1, &w, &h); CHECK_INT(w, 179);
  font_measure(f, r13, "0123456789", -1, &w, &h); CHECK_INT(w, 73);
  font_measure(f, b16, "StickTime", -1, &w, &h); CHECK_INT(w, 73);
  font_measure(f, b22, "Race (10 laps, 3 AI)", -1, &w, &h); CHECK_INT(w, 192);
  font_measure(f, fonts_face(f, 18, false), "\xc2\xb0/s", -1, &w, &h); CHECK_INT(w, 23);
  font_measure(f, fonts_face(f, 48, true), "WWWW", -1, &w, &h); CHECK_INT(w, 168);
  /* stable: the same again (cached glyphs) */
  font_measure(f, r16, "StickTime", -1, &w, &h); CHECK_INT(w, 71);

  /* glyph geometry: 'H' sits on the baseline (asc rows below the top), cap height ~0.71 em */
  const glyph_t *H = font_glyph(f, r16, 'H');
  CHECK(H->a != NULL);
  CHECK_INT(H->oy + H->h, font_asc(r16));
  CHECK(H->h >= 11 && H->h <= 12);
  CHECK(H->ox >= 0 && H->ox <= 2);
  CHECK_NEAR(H->adv, 1461 * 16 / 2048.0, 1e-4);
  /* every kept row and column has a pixel above 8 (the canvas bbox rule) */
  for (int k = 0; k < 4; k++) {
    const glyph_t *g = font_glyph(f, r22, "Wg|."[k]);
    int top = 0, bottom = 0, left = 0, right = 0;
    for (int x = 0; x < g->w; x++) { top |= g->a[x] > 8; bottom |= g->a[(g->h - 1) * g->w + x] > 8; }
    for (int y = 0; y < g->h; y++) { left |= g->a[y * g->w] > 8; right |= g->a[y * g->w + g->w - 1] > 8; }
    CHECK(top && bottom && left && right);
  }
  /* descender below the baseline */
  const glyph_t *gq = font_glyph(f, r22, 'g');
  CHECK(gq->oy + gq->h > font_asc(r22));
  /* space: advance, no mask */
  const glyph_t *sp = font_glyph(f, r16, ' ');
  CHECK(sp->a == NULL);
  CHECK_INT(sp->w, 0);
  CHECK_NEAR(sp->adv, 508 * 16 / 2048.0, 1e-5);
  /* canvas turns ASCII whitespace into spaces */
  CHECK_NEAR(font_glyph(f, r16, '\t')->adv, sp->adv, 0);
  CHECK_NEAR(font_glyph(f, r16, '\n')->adv, sp->adv, 0);
  CHECK_NEAR(font_glyph(f, r16, '\r')->adv, sp->adv, 0);
  /* controls and default ignorables are zero-width */
  static const uint32_t zw[] = { 0, 1, 0x1F, 0x7F, 0x85, 0x9F, 0xAD, 0x200B, 0x200D, 0x2060, 0xFEFF, 0xFE0F, 0xE0001 };
  for (int k = 0; k < (int)(sizeof zw / sizeof zw[0]); k++) {
    const glyph_t *g = font_glyph(f, r16, zw[k]);
    CHECK_NEAR(g->adv, 0, 0);
    CHECK(g->a == NULL);
  }
  /* missing: Latin Extended-A letters are composed (test_compose), the rest is U+FFFD */
  CHECK_NEAR(font_glyph(f, r16, 0x10C)->adv, font_glyph(f, r16, 'C')->adv, 0);
  CHECK_NEAR(font_glyph(f, r16, 0x111)->adv, font_glyph(f, r16, 'd')->adv, 0);
  CHECK_NEAR(font_glyph(f, r16, 0x17E)->adv, font_glyph(f, r16, 'z')->adv, 0);
  float rep = font_glyph(f, r16, 0xFFFD)->adv;
  CHECK_NEAR(rep, 2101 * 16 / 2048.0, 1e-4);
  CHECK_NEAR(font_glyph(f, r16, 0x4E2D)->adv, rep, 0);
  CHECK_NEAR(font_glyph(f, r16, 0x1F600)->adv, rep, 0);
  CHECK_NEAR(font_glyph(f, r16, 0x110000)->adv, rep, 0);
  CHECK_NEAR(font_glyph(f, r16, 0xFFFFFFFFu)->adv, rep, 0);
  /* present in the latin subset: degree, euro, dotless i, U+FFFD itself */
  CHECK_NEAR(font_glyph(f, r16, 0xB0)->adv, 765 * 16 / 2048.0, 1e-4);
  CHECK_NEAR(font_glyph(f, r16, 0x20AC)->adv, 1151 * 16 / 2048.0, 1e-4);
  /* invalid UTF-8 measures as U+FFFD */
  int w1, w2;
  font_measure(f, r16, "\xc4", 1, &w1, &h);
  font_measure(f, r16, "\xef\xbf\xbd", 3, &w2, &h);
  CHECK_INT(w1, w2);
  CHECK(w1 > 0);
  /* Croatian name measures like its base letters */
  font_measure(f, r16, "\xc4\x8c\xc4\x87\xc5\xbe\xc5\xa1\xc4\x91", -1, &w1, &h);
  font_measure(f, r16, "Cczsd", -1, &w2, &h);
  CHECK_INT(w1, w2);
  /* bold differs from regular */
  CHECK(font_glyph(f, b16, 'W')->adv != font_glyph(f, r16, 'W')->adv);

  /* the non-ASCII cache empties itself when full: results stay the same */
  font_face_t *r24 = fonts_face(f, 24, false);
  float first[400];
  for (int k = 0; k < 400; k++) first[k] = font_glyph(f, r24, 0xA0 + (uint32_t)k)->adv;
  int same = 1;
  for (int k = 0; k < 400; k++) same &= font_glyph(f, r24, 0xA0 + (uint32_t)k)->adv == first[k];
  CHECK(same);
  /* and a mask fetched after a flush is still right */
  const glyph_t *deg = font_glyph(f, r24, 0xB0);
  CHECK(deg->a != NULL && deg->w > 0 && deg->h > 0);

  /* more faces than slots: the least recently used one is reused, values stay right */
  for (int p = 5; p < 60; p++) {
    font_face_t *fc = fonts_face(f, p, p & 1);
    font_measure(f, fc, "Wx", 2, &w, &h);
    CHECK_INT(font_lh(fc), (int)((p * 1.17) + 0.5));
  }
  font_measure(f, fonts_face(f, 16, false), "StickTime", -1, &w, &h);
  CHECK_INT(w, 71);
  /* a huge size is clamped (bounded memory) and still draws */
  font_face_t *huge = fonts_face(f, 5000, false);
  const glyph_t *hg = font_glyph(f, huge, 'A');
  CHECK(hg->a != NULL);
  CHECK(hg->h > 600 && hg->h < 1024);
  /* px 1 still works */
  font_measure(f, fonts_face(f, 1, false), "abc", 3, &w, &h);
  CHECK(w >= 0 && w <= 3);

  /* warm-up: all printable ASCII of a class is cached with masks */
  font_class_t cls;
  font_class_for(&cls, 640, 480, NULL);
  fonts_warm(f, &cls);
  for (int i = 0; i < FONT_SIZES; i++) {
    font_face_t *fc = fonts_face(f, cls.px[i], FONT_BOLD[i]);
    CHECK(font_glyph(f, fc, 'A')->a != NULL);
    CHECK(font_glyph(f, fc, '~')->a != NULL);
  }
  fonts_close(f);
}

/* a glyph with its own copy of the mask (font_glyph's pointer only lives until the next call) */
typedef struct { float adv; int w, h, ox, oy; uint8_t *a; } gcopy_t;

static gcopy_t gcopy(fonts_t *f, font_face_t *fc, uint32_t cp) {
  const glyph_t *g = font_glyph(f, fc, cp);
  gcopy_t c = { g->adv, g->w, g->h, g->ox, g->oy, NULL };
  if (g->a) { c.a = malloc((size_t)g->w * g->h); memcpy(c.a, g->a, (size_t)g->w * g->h); }
  return c;
}

/* alpha at (x, y) relative to the pen and the text top; 0 outside the mask */
static int at(const gcopy_t *g, int x, int y) {
  x -= g->ox;
  y -= g->oy;
  return g->a && x >= 0 && y >= 0 && x < g->w && y < g->h ? g->a[y * g->w + x] : 0;
}

static bool same_glyph(const gcopy_t *a, const gcopy_t *b) {
  return a->adv == b->adv && a->w == b->w && a->h == b->h && a->ox == b->ox && a->oy == b->oy &&
         (!a->a || !memcmp(a->a, b->a, (size_t)a->w * a->h));
}

/* the base letter's pixels are all there, unchanged, in the composed one (marks don't touch it) */
static bool contains(const gcopy_t *big, const gcopy_t *base) {
  for (int y = base->oy; y < base->oy + base->h; y++)
    for (int x = base->ox; x < base->ox + base->w; x++)
      if (at(big, x, y) != at(base, x, y)) return false;
  return true;
}

/* pixels above 128 in a row (pen / top coordinates), and the number of separate runs of them */
static int row_ink(const gcopy_t *g, int y, int *runs) {
  int n = 0, r = 0, prev = 0;
  for (int x = g->ox; x < g->ox + g->w; x++) {
    int on = at(g, x, y) > 128;
    n += on;
    r += on && !prev;
    prev = on;
  }
  if (runs) *runs = r;
  return n;
}

/* Is the mark of a (the rows above base_top) the mark of b upside down? The flip axis is the mark's
 * own box, unknown here: try them all. A row the mark shares with the base is skipped. */
static bool flipped_marks(const gcopy_t *a, const gcopy_t *b, int base_top) {
  int top = a->oy < b->oy ? a->oy : b->oy;
  int x0 = a->ox < b->ox ? a->ox : b->ox, x1 = a->ox + a->w > b->ox + b->w ? a->ox + a->w : b->ox + b->w;
  if (!a->a || !b->a || base_top - top < 2) return false;
  int inked_a = 0, inked_b = 0;
  for (int r = top; r < base_top; r++) { inked_a += row_ink(a, r, NULL) > 0; inked_b += row_ink(b, r, NULL) > 0; }
  if (!inked_a || inked_a != inked_b) return false;
  for (int s = 2 * top - 2; s <= 2 * base_top + 2; s++) {
    bool ok = true;
    int n = 0;
    for (int r = top; r < base_top && ok; r++) {
      int r2 = s - r;
      if (r2 < top || r2 >= base_top) continue;
      for (int x = x0; x < x1 && ok; x++) ok = at(a, x, r) == at(b, x, r2);
      n += row_ink(a, r, NULL) > 0;
    }
    if (ok && n == inked_a) return true;
  }
  return false;
}

static void test_compose(void) {
  char err[256];
  fonts_t *f = fonts_open("host/fonts", err, sizeof err);
  CHECK(f != NULL);
  if (!f) return;
  for (int bold = 0; bold < 2; bold++)
    for (int pi = 0; pi < 3; pi++) {
      static const int pxs[3] = { 16, 22, 48 };
      font_face_t *fc = fonts_face(f, pxs[pi], bold);
      gcopy_t C = gcopy(f, fc, 'C'), Cv = gcopy(f, fc, 0x10C), c = gcopy(f, fc, 'c'), cv = gcopy(f, fc, 0x10D);
      gcopy_t cc = gcopy(f, fc, 0x109), ca = gcopy(f, fc, 0x107);
      /* Č: the C as it is, a mark above it, the C's advance */
      CHECK(Cv.adv == C.adv);
      CHECK(contains(&Cv, &C));
      CHECK(Cv.oy < C.oy);
      CHECK_INT(Cv.oy + Cv.h, C.oy + C.h);
      CHECK(contains(&cv, &c) && contains(&cc, &c) && contains(&ca, &c));
      CHECK(cv.oy < c.oy && cc.oy < c.oy && ca.oy < c.oy);
      CHECK(cv.oy > Cv.oy);                       /* lowercase marks sit lower */
      /* caron = circumflex upside down, exactly; big enough to see it, the V is wide at the top */
      CHECK(flipped_marks(&cv, &cc, c.oy));
      CHECK(!flipped_marks(&cc, &cc, c.oy));      /* a hat is not a V: the check can fail */
      if (pxs[pi] >= 22) {
        int top = cv.oy, bot = c.oy - 1;
        while (bot > top && !row_ink(&cv, bot, NULL)) bot--;
        CHECK(row_ink(&cv, top, NULL) > row_ink(&cv, bot, NULL));
        top = cc.oy;
        bot = c.oy - 1;
        while (bot > top && !row_ink(&cc, bot, NULL)) bot--;
        CHECK(row_ink(&cc, top, NULL) < row_ink(&cc, bot, NULL));
      }
      /* ő: two acutes next to each other */
      gcopy_t o = gcopy(f, fc, 'o'), odbl = gcopy(f, fc, 0x151);
      CHECK(contains(&odbl, &o));
      int two = 0;
      for (int y = odbl.oy; y < o.oy; y++) { int runs; row_ink(&odbl, y, &runs); two |= runs == 2; }
      CHECK(two);
      /* ş: the cedilla hangs below the s */
      gcopy_t s = gcopy(f, fc, 's'), sc = gcopy(f, fc, 0x15F);
      CHECK(sc.adv == s.adv);
      CHECK(sc.oy + sc.h > s.oy + s.h);
      CHECK_INT(sc.oy, s.oy);
      /* Đ is Ð; đ is a d with a bar across the ascender, below its top */
      gcopy_t Dj = gcopy(f, fc, 0x110), Eth = gcopy(f, fc, 0xD0), d = gcopy(f, fc, 'd'), dj = gcopy(f, fc, 0x111);
      CHECK(same_glyph(&Dj, &Eth));
      CHECK(dj.adv == d.adv);
      CHECK_INT(dj.oy, d.oy);
      int more = 0;
      for (int y = d.oy; y < d.oy + d.h; y++) more |= row_ink(&dj, y, NULL) > row_ink(&d, y, NULL);
      CHECK(more);
      /* ď ľ ť: an apostrophe right of the stem, so a little wider than the letter */
      gcopy_t dv = gcopy(f, fc, 0x10F), lv = gcopy(f, fc, 0x13E), l = gcopy(f, fc, 'l');
      CHECK(dv.adv > d.adv && lv.adv > l.adv);
      CHECK(contains(&dv, &d) && contains(&lv, &l));
      CHECK(dv.ox + dv.w > d.ox + d.w);
      /* marks on i go on the dotless i */
      gcopy_t im = gcopy(f, fc, 0x12B), dotless = gcopy(f, fc, 0x131), i = gcopy(f, fc, 'i');
      CHECK(im.adv == dotless.adv);
      CHECK(contains(&im, &dotless));
      CHECK(!contains(&im, &i));
      /* breve, ogonek and strokes: the plain letter */
      static const uint32_t plain[][2] = { { 0x102, 'A' }, { 0x103, 'a' }, { 0x105, 'a' }, { 0x141, 'L' }, { 0x142, 'l' },
                                           { 0x127, 'h' }, { 0x11F, 'g' }, { 0x17F, 's' }, { 0x149, 'n' } };
      for (int k = 0; k < (int)(sizeof plain / sizeof plain[0]); k++) {
        gcopy_t a = gcopy(f, fc, plain[k][0]), b = gcopy(f, fc, plain[k][1]);
        CHECK(same_glyph(&a, &b));
        free(a.a); free(b.a);
      }
      gcopy_t all[] = { C, Cv, c, cv, cc, ca, o, odbl, s, sc, Dj, Eth, d, dj, dv, lv, l, im, dotless, i };
      for (int k = 0; k < (int)(sizeof all / sizeof all[0]); k++) free(all[k].a);
    }
  /* every Latin Extended-A letter has a mask and the advance of its base letter (Đ: Ð's; ď ľ ť: more) */
  font_face_t *fc = fonts_face(f, 22, false);
  static const char base[] = "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlL"
                             "lLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
  int bad = 0;
  for (uint32_t cp = 0x100; cp < 0x180; cp++) {
    const glyph_t *g = font_glyph(f, fc, cp);
    float adv = g->adv;
    bool mask = g->a != NULL && g->w > 0 && g->h > 0;
    uint32_t b = (uint8_t)base[cp - 0x100];
    if (cp == 0x110) b = 0xD0;
    if (cp == 0x129 || cp == 0x12B) b = 0x131;                  /* ĩ ī: on the dotless i */
    if (cp == 0x131 || cp == 0x152 || cp == 0x153) b = cp;      /* ı Œ œ: in the font */
    float badv = font_glyph(f, fc, b)->adv;
    bool wide = cp == 0x10F || cp == 0x13D || cp == 0x13E || cp == 0x165;
    if (!mask || (wide ? !(adv > badv) : adv != badv)) {
      if (bad++ < 5) fprintf(stderr, "U+%04X: mask %d adv %g, base U+%04X adv %g\n", cp, mask, adv, b, badv);
    }
  }
  CHECK_INT(bad, 0);
  fonts_close(f);
}

/* the coverage goes through the text gamma: a(255 * (v / 255) ^ (1 / 2.2)); v = 1 already gives 21 */
static void test_gamma(void) {
  char err[256];
  fonts_t *f = fonts_open("host/fonts", err, sizeof err);
  CHECK(f != NULL);
  if (!f) return;
  int low = 0, full = 0, total = 0;
  for (int b = 0; b < 2; b++) {
    font_face_t *fc = fonts_face(f, 16, b);
    for (uint32_t c = 33; c < 127; c++) {
      const glyph_t *g = font_glyph(f, fc, c);
      for (int i = 0; i < g->w * g->h; i++) {
        low += g->a[i] > 0 && g->a[i] < 21;
        full += g->a[i] == 255;
        total++;
      }
    }
  }
  CHECK_INT(low, 0);
  CHECK(full > 0 && total > 0);
  fonts_close(f);
}

int main(void) {
  test_classes();
  test_utf8();
  test_stub();
  test_roboto();
  test_compose();
  test_gamma();
  T_DONE();
}
