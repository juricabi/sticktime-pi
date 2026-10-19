/* Color LCD: an RGB565 frame buffer and the drawing calls of EdgeTX's Lua API,
 * a literal port of engine.js colorLcd() (which follows the firmware). */
#ifndef LCD_H
#define LCD_H

#include <stdint.h>
#include <stdbool.h>
#include "font.h"
#include "lcd_rec.h"
#include "par.h"

/* flags (EdgeTX color radios) */
#define LCDF_INVERS   0x01u
#define LCDF_VCENTER  0x02u
#define LCDF_CENTER   0x04u
#define LCDF_RIGHT    0x08u
#define LCDF_LEADING0 0x10u
#define LCDF_PREC1    0x20u
#define LCDF_PREC2    0x30u
#define LCDF_SHADOWED 0x80u
#define LCDF_BOLD     0x100u
#define LCDF_BLINK    0x1000u
#define LCDF_TIMEHOUR 0x2000u
#define LCDF_RGB      0x8000u     /* top 16 bits carry RGB565; else they index the named colors */

#define LCD_NAMED_COLORS 31       /* COLOR_THEME_PRIMARY1 .. MAGENTA, in engine.js NAMED order */
typedef struct { const char *name; uint8_t r, g, b; } lcd_named_t;
extern const lcd_named_t LCD_NAMED[LCD_NAMED_COLORS];

typedef struct {
  int lines, rejected, rects, tris, texts, glyphs;
} lcd_stats_t;

typedef struct {
  int w, h;
  uint16_t *fb;                   /* w * h RGB565, row-major */
  fonts_t *fonts;
  font_class_t cls;
  lcd_stats_t st;                 /* reset by the caller each frame */
  int row0, row1;                 /* the rows drawn: 0, h (a band while lcd_flush plays a recording) */
  struct lcd_rec *rec;            /* lcd_set_pool: the drawing calls since the last lcd_flush */
} lcd_t;

int  lcd_init(lcd_t *l, int w, int h, fonts_t *fonts, const font_class_t *cls);  /* 0 ok */
void lcd_free(lcd_t *l);

/* Drawing on several cores (drawing = multi): with a pool, the drawing calls are recorded (and counted
 * in st as they come), and lcd_flush() draws them in bands of rows on the pool's threads: the same
 * pixels as drawing at once. Whatever reads fb calls lcd_flush() first. NULL: draw at once (the
 * default). Returns 0, or -1 when out of memory (then drawing goes on at once). */
int  lcd_set_pool(lcd_t *l, par_t *par);
void lcd_flush(lcd_t *l);
/* drawing = gpu (keep): the calls are recorded and kept, lcd_flush() leaves them: another drawer (the
 * GPU) draws lcd_calls() and then lcd_drop()s them; lcd_play() draws them here after all (on the
 * pool when there is one). lcd_set_pool(l, par) is lcd_set_mode(l, par, false). */
int  lcd_set_mode(lcd_t *l, par_t *par, bool keep);
const rcmd_t *lcd_calls(const lcd_t *l, int *n);
void lcd_play(lcd_t *l);
void lcd_drop(lcd_t *l);

static inline uint16_t lcd_rgb565(int r, int g, int b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xFF) >> 3));
}
uint16_t lcd_color_of(uint32_t flags);                    /* engine.js colorOf */
uint16_t lcd_blend565(uint16_t dst, uint16_t src, int a); /* a 0..255, engine.js blend565 */

/* primitives (engine.js names and semantics; opa 0 = opaque .. 15 = invisible) */
void lcd_clear(lcd_t *l, uint16_t c);
void lcd_hline(lcd_t *l, int x, int y, int w, uint16_t c, int opa);
void lcd_fill_rect(lcd_t *l, int x, int y, int w, int h, uint16_t c, int opa);
void lcd_line(lcd_t *l, int x1, int y1, int x2, int y2, uint8_t pat, uint16_t c);  /* clip + Bresenham */
void lcd_tri(lcd_t *l, int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c, int opa);
void lcd_point(lcd_t *l, int x, int y, uint16_t c);
void lcd_filled_circle(lcd_t *l, int cx, int cy, int r, uint16_t c);
void lcd_circle(lcd_t *l, int cx, int cy, int r, uint16_t c);
void lcd_invert_rect(lcd_t *l, int x, int y, int w, int h);

/* the Lua-facing calls that do more than one primitive */
void lcd_draw_line_api(lcd_t *l, int x1, int y1, int x2, int y2, int pat, uint32_t flags); /* x > W || y > H rejects */
void lcd_draw_rectangle(lcd_t *l, int x, int y, int w, int h, uint32_t flags, int t, int opa);
void lcd_draw_gauge(lcd_t *l, int x, int y, int w, int h, int n, int d, uint32_t flags);
/* drawStr: text (UTF-8, len bytes) with alignment / INVERS / SHADOWED; returns the measured size */
void lcd_draw_text(lcd_t *l, int x, int y, const char *s, int len, uint32_t flags, int *mw, int *mh);
void lcd_draw_number(lcd_t *l, int x, int y, int v, uint32_t flags);
void lcd_draw_timer(lcd_t *l, int x, int y, int v, uint32_t flags);
/* lcd.sizeText: width = measure, height = the class's LINE_H for the size in flags */
void lcd_size_text(lcd_t *l, const char *s, int len, uint32_t flags, int *w, int *h);

#endif
