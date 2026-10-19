/* Color fonts: Roboto through stb_truetype, with the metrics of engine.js ColorFonts.
 *
 * engine.js rasterizes each glyph once per (px, bold) with a canvas: font-size px (the em size),
 * pen at an integer x, baseline at asc = round(px * 0.93) below the text's top y. A glyph keeps
 * the bounding box of its pixels with alpha > 8, and offsets (ox, oy) from the pen position and
 * the text's top. The advance is the unrounded float width. Line height lh = round(px * 1.17).
 * measure() sums the float advances of the code points and rounds the total (Math.round). */
#ifndef FONT_H
#define FONT_H

#include <stdint.h>
#include <stdbool.h>

/* screen classes of engine.js: STD, BOLD, XXS, XS, L, XL, XXL, LXL (flags >> 8 & 0xF picks one) */
#define FONT_SIZES 8
typedef struct {
  char name[8];                /* "std", "sml", "lrg", "pc", "hd" or "custom" */
  int px[FONT_SIZES];          /* FONT_PX */
  int line_h[FONT_SIZES];      /* LINE_H: what lcd.sizeText reports as the height */
} font_class_t;
extern const bool FONT_BOLD[FONT_SIZES];   /* false, true, false, false, false, true, true, true */

/* The class for a screen: by name ("std", "sml", "lrg", "pc", "hd"), or NULL/"auto" to pick
 * by size like the emulator does; heights without a class get std scaled (see font.c). */
void font_class_for(font_class_t *out, int w, int h, const char *name);

typedef struct {
  float adv;                   /* advance in px (unrounded) */
  int16_t w, h;                /* mask size; 0 for blank glyphs */
  int16_t ox, oy;              /* mask offset from (pen x, text top y) */
  uint8_t *a;                  /* w*h alpha 0..255, or NULL */
} glyph_t;

typedef struct font_face font_face_t;     /* one (px, bold) */
typedef struct fonts fonts_t;

/* Load Roboto-Regular.ttf and Roboto-Bold.ttf from dir. Returns NULL with a message in err. */
fonts_t *fonts_open(const char *dir, char *err, int errlen);
/* Fonts that draw nothing but measure every code point with adv_px (tests that compare
 * geometry with engine.js run with the same stub on both sides). */
fonts_t *fonts_open_stub(float adv_px);
void fonts_close(fonts_t *f);

font_face_t *fonts_face(fonts_t *f, int px, bool bold);   /* cached; never NULL */
int  font_asc(const font_face_t *face);                   /* round(px * 0.93) */
int  font_lh(const font_face_t *face);                    /* round(px * 1.17) */
/* the glyph of a code point, rasterized on first use and cached (never NULL) */
const glyph_t *font_glyph(fonts_t *f, font_face_t *face, uint32_t cp);
/* engine.js measure(): w = Math.round(sum of advances), h = lh. s is UTF-8 (invalid bytes count as U+FFFD). */
void font_measure(fonts_t *f, font_face_t *face, const char *s, int len, int *w, int *h);
/* decode one UTF-8 code point at s[*i] (len bytes), advancing *i */
uint32_t font_utf8_next(const char *s, int len, int *i);
/* hold: glyphs the cache evicts keep their masks (a glyph_t copied before stays drawable) until as many
 * fonts_hold(f, false) as holds free them; for the recorded drawing of a frame (lcd_flush) */
void fonts_hold(fonts_t *f, bool hold);
/* warm the cache for printable ASCII at every size of a class (startup, so frames don't allocate) */
void fonts_warm(fonts_t *f, const font_class_t *cls);

#endif
