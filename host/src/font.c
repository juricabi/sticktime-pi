/* Color fonts: see font.h. Roboto through stb_truetype with the metrics of engine.js ColorFonts.
 *
 * The browser rasterizes with a canvas: the em size is the CSS font size, the pen sits at an
 * integer x and the baseline at asc = round(px * 0.93) below the top, and each glyph keeps the
 * box of its pixels with alpha > 8. The same here, so advances, widths and positions are the
 * browser's to the bit. The shapes differ a little: the browser hints, stb_truetype does not.
 * Its coverage is linear, which looks much thinner than canvas text, so it goes through a text
 * gamma first (Chromium's glyph masks on Windows, compared over text sheets of every size, are
 * closest for gammas of 2.2 to 2.6).
 *
 * Code points the canvas would not draw get the same treatment: ASCII whitespace is a space
 * (canvas replaces it before drawing), control and default-ignorable characters are zero-width.
 * Roboto here is the latin subset; where the browser takes a letter from a fallback font, Latin
 * Extended-A letters (Croatian, Czech, Polish, ...) are built from the base letter and Roboto's
 * own marks, and anything else becomes U+FFFD, else '?'. Measuring and drawing use the same
 * glyphs, so they agree.
 *
 * Memory is bounded: a fixed table of faces (least recently used one reused), per face printable
 * ASCII in a direct table and up to XCACHE_MAX other code points in a small hash table that is
 * emptied when full. A glyph pointer is valid until the next font_glyph() or fonts_face() call,
 * a face pointer until MAX_FACES other sizes have been asked for. */
#include "font.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

const bool FONT_BOLD[FONT_SIZES] = { false, true, false, false, false, true, true, true };

/* engine.js FONT_PX and LINE_H */
static const struct { const char *name; int px[FONT_SIZES]; int lh[FONT_SIZES]; } CLASSES[] = {
  { "std", { 16, 16, 9, 13, 24, 32, 64, 48 },      { 21, 20, 12, 17, 29, 40, 69, 51 } },
  { "sml", { 13, 13, 8, 10, 19, 25, 48, 36 },      { 17, 16, 10, 14, 23, 33, 52, 38 } },
  { "lrg", { 22, 22, 12, 18, 33, 44, 88, 66 },     { 27, 28, 17, 23, 42, 55, 95, 70 } },
  { "pc",  { 32, 32, 18, 26, 48, 64, 128, 96 },    { 42, 40, 24, 34, 58, 80, 138, 102 } },
  { "hd",  { 48, 48, 27, 39, 72, 96, 192, 144 },   { 63, 60, 36, 51, 87, 120, 207, 153 } },
};
#define NCLASSES ((int)(sizeof CLASSES / sizeof CLASSES[0]))
#define FONT_PX_MAX 1024           /* bounds glyph memory whatever a class asks for */

/* JS Math.round: halves go up (C round() takes them away from zero) */
static double js_round(double v) {
  double f = floor(v);
  return v - f >= 0.5 ? f + 1.0 : f;
}

static void set_class(font_class_t *out, int i) {
  memset(out, 0, sizeof *out);
  snprintf(out->name, sizeof out->name, "%s", CLASSES[i].name);
  memcpy(out->px, CLASSES[i].px, sizeof out->px);
  memcpy(out->line_h, CLASSES[i].lh, sizeof out->line_h);
}

void font_class_for(font_class_t *out, int w, int h, const char *name) {
  if (name) {
    for (int i = 0; i < NCLASSES; i++)
      if (!strcmp(name, CLASSES[i].name)) { set_class(out, i); return; }
  }
  /* auto: the emulator's screen for the size (PC screens, the radios' classes) */
  if (h == 720) { set_class(out, 3); return; }
  if (h == 1080) { set_class(out, 4); return; }
  if (w == 320 && h == 480) { set_class(out, 0); return; }   /* NV14 portrait: std in engine.js */
  if (h == 480) { set_class(out, 2); return; }
  if (h == 272 || h == 320) { set_class(out, 0); return; }
  if (h == 240) { set_class(out, 1); return; }
  /* std scaled with the height; 0.78 makes H 480 come out as lrg (k 1.376 vs 1.375) */
  double k = (h > 0 ? h : 272) / 272.0 * 0.78;
  set_class(out, 0);
  snprintf(out->name, sizeof out->name, "custom");
  for (int i = 0; i < FONT_SIZES; i++) {
    int px = (int)js_round(CLASSES[0].px[i] * k), lh = (int)js_round(CLASSES[0].lh[i] * k);
    out->px[i] = px < 1 ? 1 : px > FONT_PX_MAX ? FONT_PX_MAX : px;
    out->line_h[i] = lh < 1 ? 1 : lh;
  }
}

/* ------------------------------------------------------------------ faces and glyphs */

#define ASCII_LO 32
#define ASCII_N 95                 /* 32..126 */
#define XCACHE 256                 /* hash slots per face for other code points */
#define XCACHE_MAX (XCACHE * 3 / 4)
#define MAX_FACES 16
#define NO_CP 0xFFFFFFFFu

typedef struct { uint32_t cp; glyph_t g; } xslot_t;

struct font_face {
  int px;
  bool bold, used;
  int asc, lh;
  float scale;                     /* stbtt_ScaleForMappingEmToPixels(px) */
  double adv_k;                    /* px / unitsPerEm, for the float advances */
  uint32_t last_use;
  bool have[ASCII_N];
  glyph_t ascii[ASCII_N];
  int xn;
  xslot_t x[XCACHE];
};

#define TEXT_GAMMA 2.2

/* marks for composed letters (see LATIN_A_MARK) */
enum { MK_ACUTE, MK_CIRC, MK_TILDE, MK_DIAER, MK_MACRON, MK_RING, MK_DOT, MK_CEDILLA, MK_APOS, MK_N };
static const uint32_t MARK_CP[MK_N] = { 0xB4, 0x2C6, 0x2DC, 0xA8, 0xAF, 0x2DA, '.', 0xB8, 0x2BC };

struct fonts {
  bool stub;
  float stub_adv;
  unsigned char *ttf[2];
  stbtt_fontinfo info[2];
  int upem[2];
  int fallback[2];                 /* glyph index for code points the font lacks */
  int mark[2][MK_N];               /* glyph indexes of the marks (0: missing) */
  int mark_gap[2];                 /* font units between a letter's top and a mark above it */
  int x_height[2];
  uint8_t gamma[256];
  uint32_t tick;
  struct font_face face[MAX_FACES];
};

static void glyph_free(glyph_t *g) {
  free(g->a);
  memset(g, 0, sizeof *g);
}

static void xcache_clear(struct font_face *fc) {
  for (int i = 0; i < XCACHE; i++) {
    if (fc->x[i].cp != NO_CP) glyph_free(&fc->x[i].g);
    fc->x[i].cp = NO_CP;
  }
  fc->xn = 0;
}

static void face_clear(struct font_face *fc) {
  for (int i = 0; i < ASCII_N; i++) if (fc->have[i]) glyph_free(&fc->ascii[i]);
  memset(fc->have, 0, sizeof fc->have);
  xcache_clear(fc);
  fc->used = false;
}

static unsigned char *read_all(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  unsigned char *buf = NULL;
  long n = -1;
  if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
  if (n > 0 && n < (64L << 20) && fseek(f, 0, SEEK_SET) == 0 && (buf = malloc((size_t)n)) != NULL) {
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
  }
  fclose(f);
  if (buf) *len = (size_t)n;
  return buf;
}

static fonts_t *fonts_alloc(void) {
  fonts_t *f = calloc(1, sizeof *f);
  if (!f) return NULL;
  for (int i = 0; i < MAX_FACES; i++) for (int j = 0; j < XCACHE; j++) f->face[i].x[j].cp = NO_CP;
  return f;
}

fonts_t *fonts_open(const char *dir, char *err, int errlen) {
  static const char *names[2] = { "Roboto-Regular.ttf", "Roboto-Bold.ttf" };
  fonts_t *f = fonts_alloc();
  if (!f) { if (err && errlen > 0) snprintf(err, (size_t)errlen, "out of memory"); return NULL; }
  for (int b = 0; b < 2; b++) {
    char path[1024];
    size_t len = 0;
    snprintf(path, sizeof path, "%s/%s", dir && *dir ? dir : ".", names[b]);
    f->ttf[b] = read_all(path, &len);
    if (!f->ttf[b]) {
      if (err && errlen > 0) snprintf(err, (size_t)errlen, "cannot read %s", path);
      fonts_close(f);
      return NULL;
    }
    int off = stbtt_GetFontOffsetForIndex(f->ttf[b], 0);
    if (len < 12 || off < 0 || !stbtt_InitFont(&f->info[b], f->ttf[b], off)) {
      if (err && errlen > 0) snprintf(err, (size_t)errlen, "%s is not a TrueType font", path);
      fonts_close(f);
      return NULL;
    }
    f->upem[b] = ttUSHORT(f->info[b].data + f->info[b].head + 18);
    if (f->upem[b] <= 0) f->upem[b] = 2048;
    int fb = stbtt_FindGlyphIndex(&f->info[b], 0xFFFD);
    if (!fb) fb = stbtt_FindGlyphIndex(&f->info[b], '?');
    f->fallback[b] = fb;
    for (int m = 0; m < MK_N; m++) f->mark[b][m] = stbtt_FindGlyphIndex(&f->info[b], (int)MARK_CP[m]);
    /* the font's own placement: its spacing acute sits above lowercase letters */
    int x0, y0, x1, y1, ax0, ay0, ax1, ay1;
    f->x_height[b] = stbtt_GetCodepointBox(&f->info[b], 'x', &x0, &y0, &x1, &y1) ? y1 : f->upem[b] / 2;
    f->mark_gap[b] = f->mark[b][MK_ACUTE] && stbtt_GetGlyphBox(&f->info[b], f->mark[b][MK_ACUTE], &ax0, &ay0, &ax1, &ay1)
                     ? ay0 - f->x_height[b] : f->upem[b] / 20;
  }
  for (int i = 0; i < 256; i++) f->gamma[i] = (uint8_t)floor(255.0 * pow(i / 255.0, 1.0 / TEXT_GAMMA) + 0.5);
  return f;
}

fonts_t *fonts_open_stub(float adv_px) {
  fonts_t *f = fonts_alloc();
  if (!f) return NULL;
  f->stub = true;
  f->stub_adv = adv_px;
  return f;
}

void fonts_close(fonts_t *f) {
  if (!f) return;
  for (int i = 0; i < MAX_FACES; i++) face_clear(&f->face[i]);
  free(f->ttf[0]);
  free(f->ttf[1]);
  free(f);
}

font_face_t *fonts_face(fonts_t *f, int px, bool bold) {
  if (px < 1) px = 1;
  if (px > FONT_PX_MAX) px = FONT_PX_MAX;
  bold = bold ? true : false;
  f->tick++;
  struct font_face *free_slot = NULL, *oldest = NULL;
  for (int i = 0; i < MAX_FACES; i++) {
    struct font_face *fc = &f->face[i];
    if (!fc->used) { if (!free_slot) free_slot = fc; continue; }
    if (fc->px == px && fc->bold == bold) { fc->last_use = f->tick; return fc; }
    if (!oldest || (uint32_t)(f->tick - fc->last_use) > (uint32_t)(f->tick - oldest->last_use)) oldest = fc;
  }
  struct font_face *fc = free_slot ? free_slot : oldest;
  face_clear(fc);
  fc->used = true;
  fc->px = px;
  fc->bold = bold;
  fc->asc = (int)js_round(px * 0.93);
  fc->lh = (int)js_round(px * 1.17);
  fc->last_use = f->tick;
  if (!f->stub) {
    fc->scale = stbtt_ScaleForMappingEmToPixels(&f->info[bold], (float)px);
    fc->adv_k = (double)px / f->upem[bold];
  }
  return fc;
}

int font_asc(const font_face_t *face) { return face->asc; }
int font_lh(const font_face_t *face) { return face->lh; }

/* drawn as nothing by the browser (HarfBuzz hides default ignorables, Blink controls) */
static bool zero_width(uint32_t c) {
  return c < 0x20 || (c >= 0x7F && c < 0xA0) || c == 0xAD || c == 0x34F || c == 0x61C || c == 0x180E ||
         (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) || (c >= 0x2060 && c <= 0x206F) ||
         (c >= 0xFE00 && c <= 0xFE0F) || c == 0xFEFF || (c >= 0xFFF0 && c <= 0xFFF8) ||
         (c >= 0xE0000 && c <= 0xE0FFF);
}

/* Latin Extended-A, U+0100..U+017F: the base letter, and the mark to put on it: a acute, c circumflex,
 * v caron (the circumflex upside down), t tilde, d diaeresis, m macron, r ring, o dot above (a period),
 * h double acute, s cedilla (also for the Latvian comma below), p the apostrophe caron of d l t,
 * b the bar of d, E eth (which is Đ), '-' nothing (breve, ogonek and strokes: the plain letter) */
static const char LATIN_A_BASE[] =
  "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjKkkLlLlLlL"
  "lLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
static const char LATIN_A_MARK[] =
  "mm----aaccoovvvp" "Ebmm--oo--vvcc--" "oos-cc--ttmm----" "o---c-ss-aasspp-"
  "---aassvv---mm--" "hh--aassvvaaccss" "vvssvp--ttmm--rr" "hh--ccccdaaoovv-";

/* one glyph of a composed letter: moved by (tx, ty) font units, maybe upside down */
typedef struct { int gi; double tx, ty; bool flip; int x0, y0, x1, y1; float sx, sy; } part_t;

/* base glyph and marks of a Latin Extended-A letter; returns the number of parts (0: not composed) */
static int compose(fonts_t *f, int b, uint32_t cp, part_t *p, int *adv) {
  const stbtt_fontinfo *fi = &f->info[b];
  char mk = LATIN_A_MARK[cp - 0x100];
  uint32_t base = (uint8_t)LATIN_A_BASE[cp - 0x100];
  if (mk == 'E') base = 0xD0;
  else if (base == 'i' && mk != '-' && mk != 's') base = 0x131;   /* dotless under a mark */
  memset(p, 0, 3 * sizeof *p);
  p[0].gi = stbtt_FindGlyphIndex(fi, (int)base);
  if (!p[0].gi) return 0;
  int lsb, n = 1;
  stbtt_GetGlyphHMetrics(fi, p[0].gi, adv, &lsb);
  int bx0, by0, bx1, by1, mx0, my0, mx1, my1;
  if (mk == 'E' || mk == '-' || !stbtt_GetGlyphBox(fi, p[0].gi, &bx0, &by0, &bx1, &by1)) return n;
  static const char *KINDS = "acvtdmrohsbp";
  static const int KIND_MARK[] = { MK_ACUTE, MK_CIRC, MK_CIRC, MK_TILDE, MK_DIAER, MK_MACRON, MK_RING, MK_DOT,
                                   MK_ACUTE, MK_CEDILLA, MK_MACRON, MK_APOS };
  const char *k = mk ? strchr(KINDS, mk) : NULL;
  int gi = k ? f->mark[b][KIND_MARK[k - KINDS]] : 0;
  if (!gi || !stbtt_GetGlyphBox(fi, gi, &mx0, &my0, &mx1, &my1)) return n;
  const double em = f->upem[b], cx = (bx0 + bx1) / 2.0 - (mx0 + mx1) / 2.0;
  part_t m = { .gi = gi, .tx = cx, .ty = by1 + f->mark_gap[b] - my0, .flip = mk == 'v' };
  if (mk == 's') m.ty = 0;                                        /* the cedilla hangs from the baseline */
  if (mk == 'p') {                                                /* right of the stem, at its top */
    m.tx = bx1 + 0.04 * em - mx0;
    m.ty = by1 - my1;
    int right = (int)(m.tx + mx1 + 0.04 * em);
    if (right > *adv) *adv = right;
  }
  if (mk == 'b') {                                                /* across the ascender of the d */
    m.tx = bx1 - 0.14 * em - (mx0 + mx1) / 2.0;
    m.ty = (f->x_height[b] + by1) / 2.0 - (my0 + my1) / 2.0;
  }
  p[n++] = m;
  if (mk == 'h') {                                                /* two acutes side by side */
    double d = (mx1 - mx0) * 0.5;
    p[1].tx -= d;
    p[n] = p[1];
    p[n++].tx += 2 * d;
  }
  return n;
}

static void make_glyph(fonts_t *f, struct font_face *fc, uint32_t cp, glyph_t *g) {
  memset(g, 0, sizeof *g);
  if (f->stub) { g->adv = f->stub_adv; return; }
  if (cp == '\t' || cp == '\n' || cp == '\f' || cp == '\r') cp = ' ';
  else if (zero_width(cp)) return;
  const int b = fc->bold;
  const stbtt_fontinfo *fi = &f->info[b];
  part_t part[3];
  int n = 0, adv = 0, lsb = 0;
  int gi = cp <= 0x10FFFF ? stbtt_FindGlyphIndex(fi, (int)cp) : 0;
  if (!gi && cp >= 0x100 && cp < 0x180) n = compose(f, b, cp, part, &adv);
  if (!n) {
    memset(part, 0, sizeof part[0]);
    part[0].gi = gi ? gi : f->fallback[b];
    n = 1;
    stbtt_GetGlyphHMetrics(fi, part[0].gi, &adv, &lsb);
  }
  g->adv = (float)(adv * fc->adv_k);
  /* each part's pixel box (whole pixels plus a subpixel shift), and the box around them all */
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool any = false;
  for (int i = 0; i < n; i++) {
    part_t *q = &part[i];
    double px = q->tx * fc->scale, py = -q->ty * fc->scale, fx = floor(px), fy = floor(py);
    q->sx = (float)(px - fx);
    q->sy = (float)(py - fy);
    stbtt_GetGlyphBitmapBoxSubpixel(fi, q->gi, fc->scale, fc->scale, q->sx, q->sy, &q->x0, &q->y0, &q->x1, &q->y1);
    if (q->x1 <= q->x0 || q->y1 <= q->y0) { q->gi = 0; continue; }
    q->x0 += (int)fx; q->x1 += (int)fx; q->y0 += (int)fy; q->y1 += (int)fy;
    if (!any || q->x0 < x0) x0 = q->x0;
    if (!any || q->y0 < y0) y0 = q->y0;
    if (!any || q->x1 > x1) x1 = q->x1;
    if (!any || q->y1 > y1) y1 = q->y1;
    any = true;
  }
  int bw = x1 - x0, bh = y1 - y0;
  if (!any || bw > 4 * FONT_PX_MAX || bh > 4 * FONT_PX_MAX) return;
  unsigned char *buf = calloc((size_t)bw * (size_t)bh, 1), *tmp = NULL;
  if (!buf) return;
  if (n == 1) {
    stbtt_MakeGlyphBitmapSubpixel(fi, buf, bw, bh, bw, fc->scale, fc->scale, part[0].sx, part[0].sy, part[0].gi);
  } else {
    /* parts may overlap (the bar on the stem of the d): render each alone, keep the larger coverage */
    tmp = malloc((size_t)bw * (size_t)bh);
    for (int i = 0; tmp && i < n; i++) {
      const part_t *q = &part[i];
      if (!q->gi) continue;
      int w = q->x1 - q->x0, h = q->y1 - q->y0;
      memset(tmp, 0, (size_t)w * (size_t)h);
      stbtt_MakeGlyphBitmapSubpixel(fi, tmp, w, h, w, fc->scale, fc->scale, q->sx, q->sy, q->gi);
      for (int y = 0; y < h; y++) {
        const unsigned char *s = tmp + (q->flip ? h - 1 - y : y) * w;
        unsigned char *d = buf + (q->y0 - y0 + y) * bw + (q->x0 - x0);
        for (int x = 0; x < w; x++) if (s[x] > d[x]) d[x] = s[x];
      }
    }
    free(tmp);
  }
  for (int i = 0; i < bw * bh; i++) buf[i] = f->gamma[buf[i]];
  /* keep the box of the pixels the canvas version keeps (alpha > 8) */
  int cx0 = bw, cy0 = bh, cx1 = -1, cy1 = -1;
  for (int y = 0; y < bh; y++)
    for (int x = 0; x < bw; x++)
      if (buf[y * bw + x] > 8) {
        if (x < cx0) cx0 = x;
        if (x > cx1) cx1 = x;
        if (y < cy0) cy0 = y;
        if (y > cy1) cy1 = y;
      }
  if (cx1 >= 0) {
    int w = cx1 - cx0 + 1, h = cy1 - cy0 + 1;
    g->a = malloc((size_t)w * (size_t)h);
    if (g->a) {
      for (int y = 0; y < h; y++) memcpy(g->a + y * w, buf + (y + cy0) * bw + cx0, (size_t)w);
      g->w = (int16_t)w;
      g->h = (int16_t)h;
      g->ox = (int16_t)(x0 + cx0);
      g->oy = (int16_t)(fc->asc + y0 + cy0);
    }
  }
  free(buf);
}

const glyph_t *font_glyph(fonts_t *f, font_face_t *face, uint32_t cp) {
  struct font_face *fc = face;
  if (cp > 0x10FFFF) cp = 0xFFFD;   /* also keeps NO_CP out of the table */
  if (cp >= ASCII_LO && cp < ASCII_LO + ASCII_N) {
    int i = (int)(cp - ASCII_LO);
    if (!fc->have[i]) { make_glyph(f, fc, cp, &fc->ascii[i]); fc->have[i] = true; }
    return &fc->ascii[i];
  }
  uint32_t h = (cp * 2654435761u) >> 24;   /* 8 bits: XCACHE slots */
  for (;;) {
    xslot_t *s = &fc->x[h];
    if (s->cp == cp) return &s->g;
    if (s->cp == NO_CP) break;
    h = (h + 1) & (XCACHE - 1);
  }
  if (fc->xn >= XCACHE_MAX) {
    xcache_clear(fc);
    h = (cp * 2654435761u) >> 24;
  }
  xslot_t *s = &fc->x[h];
  s->cp = cp;
  fc->xn++;
  make_glyph(f, fc, cp, &s->g);
  return &s->g;
}

/* WHATWG UTF-8 decoding: an invalid or truncated sequence is one U+FFFD per maximal subpart */
uint32_t font_utf8_next(const char *s, int len, int *i) {
  const unsigned char *p = (const unsigned char *)s;
  int k = *i;
  if (k < 0) k = 0;
  if (k >= len) { *i = len > k ? len : k; return 0xFFFD; }
  unsigned c = p[k++];
  if (c < 0x80) { *i = k; return c; }
  int need;
  uint32_t cp;
  unsigned lo = 0x80, hi = 0xBF;
  if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; }
  else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; if (c == 0xE0) lo = 0xA0; if (c == 0xED) hi = 0x9F; }
  else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; if (c == 0xF0) lo = 0x90; if (c == 0xF4) hi = 0x8F; }
  else { *i = k; return 0xFFFD; }
  while (need--) {
    if (k >= len || p[k] < lo || p[k] > hi) { *i = k; return 0xFFFD; }
    cp = (cp << 6) | (p[k++] & 0x3F);
    lo = 0x80;
    hi = 0xBF;
  }
  *i = k;
  return cp;
}

void font_measure(fonts_t *f, font_face_t *face, const char *s, int len, int *w, int *h) {
  double sum = 0;
  if (s && len < 0) len = (int)strlen(s);
  for (int i = 0; s && i < len;) sum += font_glyph(f, face, font_utf8_next(s, len, &i))->adv;
  double r = js_round(sum);
  if (w) *w = r > 2147483647.0 ? 2147483647 : r < -2147483647.0 ? -2147483647 : (int)r;
  if (h) *h = face->lh;
}

void fonts_warm(fonts_t *f, const font_class_t *cls) {
  for (int i = 0; i < FONT_SIZES; i++) {
    font_face_t *fc = fonts_face(f, cls->px[i], FONT_BOLD[i]);
    for (uint32_t c = ASCII_LO; c < ASCII_LO + ASCII_N; c++) font_glyph(f, fc, c);
    font_glyph(f, fc, 0xB0);      /* the game's degree sign */
    font_glyph(f, fc, 0xFFFD);
  }
}
