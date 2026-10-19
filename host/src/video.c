/* Video output: backend dispatch, layout, CPU scaling, the software clock and the "none"
 * backend. See video.h; the device backends are video_drm.c and video_fb.c. */
#include "video.h"
#include "plat.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_PERIOD_US 16667

/* ================================================================ layout */

/* "16:9" -> 16, 9; anything unusable -> 4:3 (the default of the settings) */
static void parse_aspect(const char *a, int64_t *aw, int64_t *ah) {
  *aw = 4;
  *ah = 3;
  if (!a) return;
  int w = 0, h = 0;
  char extra;
  if (sscanf(a, " %d : %d %c", &w, &h, &extra) == 2 && w > 0 && h > 0 && w <= 100 && h <= 100) {
    *aw = w;
    *ah = h;
  }
}

static int div_round(int64_t a, int64_t b) { return (int)((2 * a + b) / (2 * b)); }

int video_layout(int mode_w, int mode_h, bool composite, const char *aspect, int margin_pct,
                 int render_w, int render_h, video_layout_t *o) {
  if (mode_w <= 0 || mode_h <= 0 || mode_w > 65535 || mode_h > 65535) {
    *o = (video_layout_t){ 640, 480, 0, 0, 640, 480 };
    return -1;
  }
  int m = margin_pct < 0 ? 0 : margin_pct > 20 ? 20 : margin_pct;
  /* the margin is a share of each side, so the inset area keeps the shape of the screen */
  int mx = div_round((int64_t)mode_w * m, 100), my = div_round((int64_t)mode_h * m, 100);
  int area_w = mode_w - 2 * mx, area_h = mode_h - 2 * my;
  if (area_w < 1) area_w = 1;
  if (area_h < 1) area_h = 1;
  /* the displayed shape of the whole mode: composite = the aspect setting, else square pixels */
  int64_t dw, dh;
  if (composite) parse_aspect(aspect, &dw, &dh);
  else { dw = mode_w; dh = mode_h; }
  /* a mode pixel is par times as wide as it is tall */
  double par = (double)(dw * mode_h) / (double)(dh * mode_w);

  /* the automatic render size for the area */
  int arw, arh;
  if (composite) {
    /* every mode line is a render line (no vertical scaling); width with square pixels,
     * rounded up: 853.3 -> 854, 426.7 -> 427 */
    arh = area_h;
    int64_t num = (int64_t)area_w * dw * mode_h, den = dh * mode_w;
    arw = (int)((num + den - 1) / den);
  } else {
    arw = area_w;
    arh = area_h;
    if (arw > 1920 || arh > 1200) {
      /* the display scales up; the game shouldn't draw 4K */
      if ((int64_t)arw * 1200 > (int64_t)arh * 1920) { arh = div_round((int64_t)arh * 1920, arw); arw = 1920; }
      else { arw = div_round((int64_t)arw * 1200, arh); arh = 1200; }
    }
  }
  if (arw < 1) arw = 1;
  if (arh < 1) arh = 1;
  int rw = render_w, rh = render_h;
  if (rw <= 0 || rh <= 0 || rw > 65535 || rh > 65535) {
    rw = arw;
    rh = arh;
  }
  int vw, vh;
  if (rw == arw && rh == arh) {
    /* the automatic size fills the area exactly: the rounding of its width (< 1 render pixel)
     * must not turn into a letterbox line, which would mean vertical scaling */
    vw = area_w;
    vh = area_h;
  } else {
    /* fit rw x rh (square pixels) into the area, keeping its shape */
    double need_w = (double)rw * area_h / ((double)rh * par);
    long nw = lround(need_w);
    if (nw <= area_w) {
      vw = (int)nw;
      vh = area_h;
    } else {
      vw = area_w;
      vh = (int)lround((double)area_w * rh * par / rw);
      if (vh > area_h) vh = area_h;
    }
  }
  if (vw < 1) vw = 1;
  if (vh < 1) vh = 1;
  o->render_w = rw;
  o->render_h = rh;
  o->view_w = vw;
  o->view_h = vh;
  o->view_x = (mode_w - vw) / 2;
  o->view_y = (mode_h - vh) / 2;
  return 0;
}

void video_render_size(int mode_w, int mode_h, bool composite, const char *aspect, int *rw, int *rh) {
  video_layout_t l;
  video_layout(mode_w, mode_h, composite, aspect, 0, 0, 0, &l);
  if (rw) *rw = l.render_w;
  if (rh) *rh = l.render_h;
}

int video_layout_double(int mode_w, int mode_h, const char *aspect, int margin_pct, video_layout_t *o) {
  video_layout_t a;
  int r = video_layout(mode_w, mode_h, true, aspect, margin_pct, 0, 0, &a);   /* view = the inset area */
  if (r) { *o = a; return r; }
  int64_t dw, dh;
  parse_aspect(aspect, &dw, &dh);
  /* as video_layout's automatic width, for twice the lines (rounded up the same way) */
  int64_t num = (int64_t)a.view_w * dw * 2 * mode_h, den = dh * mode_w;
  return video_layout(mode_w, mode_h, true, aspect, margin_pct, (int)((num + den - 1) / den), 2 * a.view_h, o);
}

/* ASCII case-insensitive prefix test (strncasecmp isn't everywhere) */
static bool prefix_ci(const char *s, const char *pre) {
  for (; *pre; s++, pre++)
    if (tolower((unsigned char)*s) != *pre) return false;
  return true;
}

int video_parse_mode(const char *s, int *w, int *h, double *hz, bool *interlaced) {
  *w = *h = 0;
  *hz = 0;
  *interlaced = false;
  if (!s) return 0;
  while (isspace((unsigned char)*s)) s++;
  if (!*s) return 0;
  if (prefix_ci(s, "auto")) {
    const char *p = s + 4;
    while (isspace((unsigned char)*p)) p++;
    return *p ? -1 : 0;
  }
  char *e;
  long W = strtol(s, &e, 10);
  if (e == s || (*e != 'x' && *e != 'X')) return -1;
  s = e + 1;
  long H = strtol(s, &e, 10);
  if (e == s) return -1;
  s = e;
  bool il = false, scan_given = false;
  if (*s == 'i' || *s == 'p') { il = *s == 'i'; scan_given = true; s++; }
  double f = 0;
  if (*s == '@') {
    s++;
    f = strtod(s, &e);
    if (e == s) return -1;
    s = e;
    if (!scan_given && (*s == 'i' || *s == 'p')) { il = *s == 'i'; s++; }
    if (prefix_ci(s, "hz")) s += 2;
  }
  while (isspace((unsigned char)*s)) s++;
  if (*s) return -1;
  if (W < 64 || W > 8192 || H < 64 || H > 8192) return -1;
  if (f != 0 && (f < 10 || f > 300)) return -1;
  *w = (int)W;
  *h = (int)H;
  *hz = f;
  *interlaced = il;
  return 1;
}

/* ================================================================ mode selection */

static bool mode_il(const video_mode_t *m) { return (m->flags & VIDEO_MODE_FLAG_INTERLACE) != 0; }
static bool mode_pref(const video_mode_t *m) { return (m->type & VIDEO_MODE_TYPE_PREFERRED) != 0; }
static bool mode_dbl(const video_mode_t *m) { return (m->flags & VIDEO_MODE_FLAG_DBLSCAN) != 0; }

double video_mode_hz(const video_mode_t *m) {
  if (!m->htotal || !m->vtotal || !m->clock) return m->vrefresh ? m->vrefresh : 60.0;
  double hz = m->clock * 1000.0 / ((double)m->htotal * m->vtotal);
  if (mode_il(m)) hz *= 2;
  if (mode_dbl(m)) hz /= 2;
  if (m->vscan > 1) hz /= m->vscan;
  return hz;
}

bool video_same_timing(const video_mode_t *a, const video_mode_t *b) {
  return a->clock == b->clock && a->hdisplay == b->hdisplay && a->hsync_start == b->hsync_start &&
         a->hsync_end == b->hsync_end && a->htotal == b->htotal && a->vdisplay == b->vdisplay &&
         a->vsync_start == b->vsync_start && a->vsync_end == b->vsync_end && a->vtotal == b->vtotal &&
         a->vscan == b->vscan &&
         (a->flags & ~VIDEO_MODE_FLAG_PIC_AR_MASK) == (b->flags & ~VIDEO_MODE_FLAG_PIC_AR_MASK);
}

static bool eq_ci(const char *a, const char *b) {
  if (!a) return false;
  for (; *a && *b; a++, b++)
    if (tolower((unsigned char)*a) != *b) return false;
  return !*a && !*b;
}

const char *video_tv_norm(const char *tv) {
  static const struct { const char *set, *norm; } map[] = {
    { "ntsc", "NTSC" }, { "ntsc-j", "NTSC-J" }, { "ntsc-443", "NTSC-443" }, { "pal-m", "PAL-M" },
    { "pal60", "PAL" }, { "pal", "PAL" }, { "pal-n", "PAL-N" }, { "secam", "SECAM" },
  };
  for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
    if (eq_ci(tv, map[i].set)) return map[i].norm;
  return "NTSC";
}

bool video_tv_625(const char *tv) { return eq_ci(tv, "pal") || eq_ci(tv, "pal-n") || eq_ci(tv, "secam"); }

bool video_vec_valid(const char *norm, unsigned htotal) {
  if (!norm) return false;
  if (!strcmp(norm, "PAL") || !strcmp(norm, "Mono")) return htotal == 858 || htotal == 864;
  if (!strcmp(norm, "PAL-N") || !strcmp(norm, "SECAM")) return htotal == 864;
  return htotal == 858;                       /* NTSC, NTSC-443, NTSC-J, PAL-M */
}

#define MODE(clk, hd, hss, hse, ht, vd, vss, vse, vt, hz, fl, nm) \
  { .clock = clk, .hdisplay = hd, .hsync_start = hss, .hsync_end = hse, .htotal = ht, \
    .vdisplay = vd, .vsync_start = vss, .vsync_end = vse, .vtotal = vt, .vrefresh = hz, \
    .flags = fl, .type = VIDEO_MODE_TYPE_USERDEF, .name = nm }
#define NHNV (VIDEO_MODE_FLAG_NHSYNC | VIDEO_MODE_FLAG_NVSYNC)
#define PHPV (VIDEO_MODE_FLAG_PHSYNC | VIDEO_MODE_FLAG_PVSYNC)

/* the four VEC modes of rpi-6.12.y: for lists that lack the target (older kernels: no 240p) */
static const video_mode_t av_modes[] = {
  MODE(13500, 720, 736, 800, 858, 480, 486, 492, 525, 60, NHNV | VIDEO_MODE_FLAG_INTERLACE, "720x480i"),
  MODE(13500, 720, 732, 796, 864, 576, 581, 587, 625, 50, NHNV | VIDEO_MODE_FLAG_INTERLACE, "720x576i"),
  MODE(13500, 720, 734, 798, 858, 240, 243, 246, 262, 60, 0, "720x240"),
  MODE(13500, 720, 740, 804, 864, 288, 290, 293, 312, 50, 0, "720x288"),
};

/* CEA-861 / DMT modes for HDMI without a usable list (blind mode, explicit hdmi_mode) */
static const video_mode_t hdmi_modes[] = {
  MODE(74250, 1280, 1390, 1430, 1650, 720, 725, 730, 750, 60, PHPV, "1280x720"),       /* VIC 4: the blind mode */
  MODE(148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, 60, PHPV, "1920x1080"), /* VIC 16 */
  MODE(74250, 1280, 1720, 1760, 1980, 720, 725, 730, 750, 50, PHPV, "1280x720"),       /* VIC 19 */
  MODE(148500, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125, 50, PHPV, "1920x1080"), /* VIC 31 */
  MODE(27000, 720, 736, 798, 858, 480, 489, 495, 525, 60, NHNV, "720x480"),            /* VIC 2 */
  MODE(27000, 720, 732, 796, 864, 576, 581, 586, 625, 50, NHNV, "720x576"),            /* VIC 17 */
  MODE(25175, 640, 656, 752, 800, 480, 490, 492, 525, 60, NHNV, "640x480"),            /* VIC 1 */
  MODE(40000, 800, 840, 968, 1056, 600, 601, 605, 628, 60, PHPV, "800x600"),           /* DMT */
  MODE(65000, 1024, 1048, 1184, 1344, 768, 771, 777, 806, 60, NHNV, "1024x768"),       /* DMT */
};

static const video_mode_t *builtin_av(int h, bool il) {
  for (size_t i = 0; i < sizeof av_modes / sizeof av_modes[0]; i++)
    if (av_modes[i].vdisplay == h && mode_il(&av_modes[i]) == il) return &av_modes[i];
  return NULL;
}

static const video_mode_t *builtin_hdmi(int w, int h, double hz) {
  const video_mode_t *best = NULL;
  for (size_t i = 0; i < sizeof hdmi_modes / sizeof hdmi_modes[0]; i++) {
    const video_mode_t *m = &hdmi_modes[i];
    if (m->hdisplay != w || m->vdisplay != h) continue;
    if (hz > 0 && fabs(video_mode_hz(m) - hz) > 0.6) continue;
    if (!best || (hz <= 0 && fabs(video_mode_hz(m) - 60) < fabs(video_mode_hz(best) - 60))) best = m;
  }
  return best;
}

static void cand_add(video_mode_t *o, int *n, const video_mode_t *m) {
  if (!m || *n >= VIDEO_MAX_CANDIDATES) return;
  for (int i = 0; i < *n; i++)
    if (video_same_timing(&o[i], m)) return;
  o[*n] = *m;
  o[*n].flags &= ~VIDEO_MODE_FLAG_PIC_AR_MASK;
  (*n)++;
}

int video_composite_modes(const video_mode_t *list, int n, const char *tv, bool progressive, video_mode_t *out) {
  int k = 0;
  if (!list || n < 0) n = 0;
  bool p625 = video_tv_625(tv), il = !progressive;
  int h = p625 ? (il ? 576 : 288) : (il ? 480 : 240);
  const video_mode_t *best = NULL;
  for (int i = 0; i < n; i++) {
    const video_mode_t *m = &list[i];
    if (m->hdisplay == 720 && m->vdisplay == h && mode_il(m) == il && !mode_dbl(m) &&
        (!best || (mode_pref(m) && !mode_pref(best))))
      best = m;
  }
  cand_add(out, &k, best);
  cand_add(out, &k, builtin_av(h, il));
  for (int i = 0; i < n; i++)
    if (mode_pref(&list[i])) cand_add(out, &k, &list[i]);
  if (n) cand_add(out, &k, &list[0]);
  cand_add(out, &k, builtin_av(p625 ? (il ? 288 : 576) : (il ? 240 : 480), !il));   /* the other scan */
  cand_add(out, &k, builtin_av(p625 ? 480 : 576, true));                              /* the other norm */
  return k;
}

int video_hdmi_modes(const video_mode_t *list, int n, const char *hdmi_mode, video_mode_t *out, int *note) {
  int k = 0, W, H;
  double HZ;
  bool IL;
  if (!list || n < 0) n = 0;
  if (note) *note = 0;
  int pm = video_parse_mode(hdmi_mode, &W, &H, &HZ, &IL);
  if (pm < 0 && note) *note = 1;
  if (pm > 0) {
    const video_mode_t *best = NULL;
    double bs = -1e9;
    for (int i = 0; i < n; i++) {
      const video_mode_t *m = &list[i];
      if (m->hdisplay != W || m->vdisplay != H || mode_il(m) != IL || mode_dbl(m)) continue;
      double hz = video_mode_hz(m), s;
      if (HZ > 0) {
        if (fabs(hz - HZ) > 0.6) continue;
        s = -fabs(hz - HZ) * 100;              /* @59.94 picks 59.94 over 60.00 */
      } else {
        s = fabs(hz - 60) < 0.6 ? 1000 : hz;   /* 60 Hz, else the fastest */
      }
      s += (mode_pref(m) ? 0.5 : 0) + m->clock * 1e-7;
      if (!best || s > bs) { best = m; bs = s; }
    }
    cand_add(out, &k, best);
    if (!IL) cand_add(out, &k, builtin_hdmi(W, H, HZ));
    if (!best && n && note) *note = 2;
  }
  /* auto: 720p60, 1080p60, a sane preferred mode, the progressive mode nearest 720 lines */
  static const int sizes[2][2] = { { 1280, 720 }, { 1920, 1080 } };
  for (int s = 0; s < 2; s++) {
    const video_mode_t *best = NULL;
    for (int i = 0; i < n; i++) {
      const video_mode_t *m = &list[i];
      if (m->hdisplay != sizes[s][0] || m->vdisplay != sizes[s][1] || mode_il(m) || mode_dbl(m) ||
          fabs(video_mode_hz(m) - 60) >= 0.6)
        continue;
      /* PREFERRED first, then the higher clock: 74250 (60.00) over 74176 (59.94) */
      if (!best || (mode_pref(m) && !mode_pref(best)) || (mode_pref(m) == mode_pref(best) && m->clock > best->clock)) best = m;
    }
    cand_add(out, &k, best);
  }
  for (int i = 0; i < n; i++) {
    const video_mode_t *m = &list[i];
    /* a 4K@30 preferred mode would be slow and make the game draw 1920x1080 for nothing */
    if (mode_pref(m) && !mode_il(m) && !mode_dbl(m) && video_mode_hz(m) >= 49.5 && m->hdisplay <= 1920 && m->vdisplay <= 1200)
      cand_add(out, &k, m);
  }
  const video_mode_t *nearest = NULL;
  for (int i = 0; i < n; i++) {
    const video_mode_t *m = &list[i];
    if (mode_il(m) || mode_dbl(m) || video_mode_hz(m) < 49.5 || m->hdisplay > 4096) continue;
    if (!nearest) { nearest = m; continue; }
    int dm = abs(m->vdisplay - 720), dn = abs(nearest->vdisplay - 720);
    if (dm < dn || (dm == dn && fabs(video_mode_hz(m) - 60) < fabs(video_mode_hz(nearest) - 60))) nearest = m;
  }
  cand_add(out, &k, nearest);
  cand_add(out, &k, &hdmi_modes[0]);       /* blind mode: 1280x720@60 passes the checks unconnected */
  for (int i = 0; i < n; i++)
    if (mode_pref(&list[i])) cand_add(out, &k, &list[i]);
  if (n) cand_add(out, &k, &list[0]);
  return k;
}

/* ================================================================ CPU scaling */

void video_scale_luts(int sw, int sh, int dw, int dh, uint16_t *xlut, uint16_t *ylut) {
  /* nearest neighbour sampled at pixel centres: src = floor((i + 0.5) * s / d) */
  if (xlut && sw > 0)
    for (int i = 0; i < dw; i++) {
      int64_t s = ((int64_t)(2 * i + 1) * sw) / (2 * (int64_t)dw);
      xlut[i] = (uint16_t)(s > sw - 1 ? sw - 1 : s);
    }
  if (ylut && sh > 0)
    for (int i = 0; i < dh; i++) {
      int64_t s = ((int64_t)(2 * i + 1) * sh) / (2 * (int64_t)dh);
      ylut[i] = (uint16_t)(s > sh - 1 ? sh - 1 : s);
    }
}

static inline uint32_t rgb565_xrgb(uint16_t p) {
  uint32_t r = p >> 11, g = (p >> 5) & 63, b = p & 31;
  return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}

void video_scale_copy_lut(const uint16_t *src, int sw, void *dst, int pitch, int bpp,
                          int dx, int dy, int dw, int dh, const uint16_t *xlut, const uint16_t *ylut,
                          const uint32_t *pixlut) {
  if (!src || !dst || sw <= 0 || dw <= 0 || dh <= 0 || (bpp != 16 && bpp != 32)) return;
  if (!xlut && dw > sw) return;                       /* 1:1 needs dw == sw */
  uint8_t *base = (uint8_t *)dst + (size_t)dy * (size_t)pitch + (size_t)dx * (size_t)(bpp / 8);
  for (int y = 0; y < dh; y++) {
    const uint16_t *s = src + (size_t)(ylut ? ylut[y] : y) * (size_t)sw;
    uint8_t *row = base + (size_t)y * (size_t)pitch;
    if (bpp == 16) {
      uint16_t *d = (uint16_t *)row;
      if (pixlut) {
        if (xlut) for (int x = 0; x < dw; x++) d[x] = (uint16_t)pixlut[s[xlut[x]]];
        else for (int x = 0; x < dw; x++) d[x] = (uint16_t)pixlut[s[x]];
      } else if (!xlut) {
        memcpy(d, s, (size_t)dw * 2);               /* the common case: rows go out in one go */
      } else {
        for (int x = 0; x < dw; x++) d[x] = s[xlut[x]];
      }
    } else {
      uint32_t *d = (uint32_t *)row;
      if (pixlut) {
        if (xlut) for (int x = 0; x < dw; x++) d[x] = pixlut[s[xlut[x]]];
        else for (int x = 0; x < dw; x++) d[x] = pixlut[s[x]];
      } else {
        if (xlut) for (int x = 0; x < dw; x++) d[x] = rgb565_xrgb(s[xlut[x]]);
        else for (int x = 0; x < dw; x++) d[x] = rgb565_xrgb(s[x]);
      }
    }
  }
}

void video_scale_copy(const uint16_t *src, int sw, void *dst, int pitch, int bpp,
                      int dx, int dy, int dw, int dh, const uint16_t *xlut, const uint16_t *ylut) {
  video_scale_copy_lut(src, sw, dst, pitch, bpp, dx, dy, dw, dh, xlut, ylut, NULL);
}

/* RGB565 spread over 32 bits as 00000GGGGGG00000RRRRR000000BBBBB (0x07E0F81F): room above each field
 * for the sum of up to 16 pixels, so the three channels add up in one integer */
#define SPREAD_MASK 0x07E0F81Fu
static inline uint32_t spread565(uint16_t p) { return ((uint32_t)p | ((uint32_t)p << 16)) & SPREAD_MASK; }
static inline uint16_t pack565(uint32_t v) { v &= SPREAD_MASK; return (uint16_t)(v | (v >> 16)); }
/* video_deflicker_copy's spread lines (one display, filtered on the frame loop's thread: no allocation
 * per frame), and a black line (only read) */
#define DEFLICKER_MAXW VIDEO_DEFLICKER_MAXW
static uint32_t dfl_lines[3][DEFLICKER_MAXW], dfl_black[DEFLICKER_MAXW];

/* picture line y spread: the sum of its `rows` source lines */
static void dfl_line(uint32_t *o, const uint16_t *src, int w, int rows, int y) {
  const uint16_t *s = src + (size_t)y * (size_t)rows * (size_t)w, *s2 = s + w;
  if (rows == 2) for (int x = 0; x < w; x++) o[x] = spread565(s[x]) + spread565(s2[x]);
  else for (int x = 0; x < w; x++) o[x] = spread565(s[x]);
}

/* one pixel of the same, for any line: black past the picture with pad, else its edge line */
static inline uint32_t dfl_px(const uint16_t *src, int w, int h, int rows, int y, int x, bool pad) {
  if (y < 0 || y >= h) {
    if (pad) return 0;
    y = y < 0 ? 0 : h - 1;
  }
  const uint16_t *s = src + (size_t)y * (size_t)rows * (size_t)w + (size_t)x;
  return rows == 2 ? spread565(s[0]) + spread565(s[w]) : spread565(s[0]);
}

void video_deflicker_copy(const uint16_t *src, int w, int h, int rows, void *dst, int pitch, int bpp, int strength,
                          bool pad) {
  video_deflicker_lines(src, w, h, rows, dst, pitch, bpp, strength, pad, 0, h + 2, &dfl_lines[0][0]);
}

void video_deflicker_lines(const uint16_t *src, int w, int h, int rows, void *dst, int pitch, int bpp, int strength,
                           bool pad, int line0, int line1, uint32_t *scratch) {
  if (!src || !dst || w <= 0 || h <= 0 || (rows != 1 && rows != 2) || (bpp != 16 && bpp != 32)) return;
  if (strength <= 0) pad = false;
  int p = pad ? 1 : 0;
  if (line0 < 0) line0 = 0;
  if (line1 > h + 2 * p) line1 = h + 2 * p;
  if (line0 >= line1) return;
  if (strength <= 0 && rows == 1) {
    video_scale_copy(src + (size_t)line0 * (size_t)w, w, (uint8_t *)dst + (size_t)line0 * (size_t)pitch, pitch, bpp, 0, 0,
                     w, line1 - line0, NULL, NULL);
    return;
  }
  /* each line e, m, e with the lines above and below: full [1 2 1]/4, light [1 6 1]/8, none [0 1 0];
   * over the sum of `rows` source lines, so the divisor (2e + m) * rows is 1 .. 16, a power of two; half
   * of it added in each field rounds */
  uint32_t e = strength > 0 ? 1 : 0, m = strength >= 2 ? 2 : strength == 1 ? 6 : 1;
  int shift = 0;
  while ((1u << shift) < (2 * e + m) * (uint32_t)rows) shift++;
  uint32_t hv = shift ? 1u << (shift - 1) : 0, half = hv | hv << 11 | hv << 21;
  int y0 = line0 - p, y1 = line1 - p;              /* picture lines; -1 and h are the pad lines */
  uint8_t *out = (uint8_t *)dst + (size_t)line0 * (size_t)pitch;
  if (scratch && w <= DEFLICKER_MAXW) {
    /* each picture line spread once, into ring[line % 3]: the lines y - 1, y, y + 1 are there */
    uint32_t *ring[3] = { scratch, scratch + w, scratch + 2 * (size_t)w };
    int next = y0 - 1 > 0 ? y0 - 1 : 0;
    for (int y = y0; y < y1; y++, out += pitch) {
      for (int need = y + 1 < h ? y + 1 : h - 1; next <= need; next++) dfl_line(ring[next % 3], src, w, rows, next);
      const uint32_t *l[3];
      for (int k = 0; k < 3; k++) {
        int ly = y - 1 + k;
        l[k] = ly >= 0 && ly < h ? ring[ly % 3] : pad ? dfl_black : ring[(ly < 0 ? 0 : h - 1) % 3];
      }
      const uint32_t *a = l[0], *b = l[1], *c = l[2];
      if (bpp == 16) {
        uint16_t *d = (uint16_t *)out;
        for (int x = 0; x < w; x++) d[x] = pack565((e * (a[x] + c[x]) + m * b[x] + half) >> shift);
      } else {
        uint32_t *d = (uint32_t *)out;
        for (int x = 0; x < w; x++) d[x] = rgb565_xrgb(pack565((e * (a[x] + c[x]) + m * b[x] + half) >> shift));
      }
    }
    return;
  }
  for (int y = y0; y < y1; y++, out += pitch)
    for (int x = 0; x < w; x++) {
      uint32_t v = (e * (dfl_px(src, w, h, rows, y - 1, x, pad) + dfl_px(src, w, h, rows, y + 1, x, pad)) +
                    m * dfl_px(src, w, h, rows, y, x, pad) + half) >> shift;
      if (bpp == 16) ((uint16_t *)out)[x] = pack565(v);
      else ((uint32_t *)out)[x] = rgb565_xrgb(pack565(v));
    }
}

void video_set_pool(video_t *v, par_t *par) {
  if (v) v->par = par;
}

const video_out_t *video_out_begin(video_t *v) {
  return v && v->ops->out_begin ? v->ops->out_begin(v) : NULL;
}

void video_out_done(video_t *v) {
  if (v && v->ops->out_done) v->ops->out_done(v, true);
}

void video_out_cancel(video_t *v) {
  if (v && v->ops->out_done) v->ops->out_done(v, false);
}

/* picture lines y0 .. y1 - 1 (-1 and h: the pad lines) of o's copy */
static void out_lines(const video_out_t *o, const uint16_t *fb, int y0, int y1, uint32_t *scratch) {
  video_deflicker_lines(fb, o->w, o->h, o->rows, o->map, o->pitch, o->bpp, o->strength, o->pad > 0, y0 + o->pad,
                        y1 + o->pad, scratch);
}

void video_out_band(const video_out_t *o, const uint16_t *fb, int band, int bands, int row0, int row1, int *edges) {
  uint32_t scratch[3 * VIDEO_DEFLICKER_MAXW];
  int a = row0 / o->rows, b = row1 / o->rows, last = band == bands - 1;
  /* its own lines: those whose neighbours are in it too (the first band's top and the last's bottom
   * have the border, or the edge line itself, for a neighbour) */
  out_lines(o, fb, band == 0 ? -o->pad : a + 1, last ? o->h + o->pad : b - 1, scratch);
  /* the two lines at an edge need both bands: the second of them to be drawn writes them */
  if (band > 0 && __atomic_add_fetch(&edges[band], 1, __ATOMIC_ACQ_REL) == 2) out_lines(o, fb, a - 1, a + 1, scratch);
  if (!last && __atomic_add_fetch(&edges[band + 1], 1, __ATOMIC_ACQ_REL) == 2) out_lines(o, fb, b - 1, b + 1, scratch);
}

void video_set_gpu(video_t *v, gpu_t *g) {
  if (v) v->gpu = g;
}

int video_present_gpu(video_t *v, const rcmd_t *c, int n, uint16_t *shot) {
  if (!v || !v->gpu || !v->ops->present_gpu) return 0;
  return v->ops->present_gpu(v, c, n, shot);
}

void video_clear_buffer(void *dst, int pitch, int bpp, int w, int h) {
  if (!dst || w <= 0 || h <= 0 || pitch <= 0) return;
  size_t row = (size_t)w * (size_t)((bpp + 7) / 8);
  if (row > (size_t)pitch) row = (size_t)pitch;
  if (row == (size_t)pitch) { memset(dst, 0, (size_t)pitch * (size_t)h); return; }
  for (int y = 0; y < h; y++) memset((uint8_t *)dst + (size_t)y * (size_t)pitch, 0, row);
}

/* ================================================================ timing helpers */

void video_log(video_t *v, const char *fmt, ...) {
  char line[320];
  int n = snprintf(line, sizeof line, "video: ");
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
  va_end(ap);
  if (v && v->log_fn) v->log_fn(v->log_ud, line);
  else fprintf(stderr, "%s\n", line);
}

void video_base_init(video_t *v, const video_ops_t *ops, const video_config_t *c, double hz) {
  v->ops = ops;
  v->period_us = hz > 1 && hz < 1000 ? (int64_t)llround(1e6 / hz) : DEFAULT_PERIOD_US;
  int64_t now = plat_now_us();
  v->last_vblank_us = now;
  v->phase_us = now;
  v->margin_us = 0;
  v->soft_due_us = 0;
  if (c) { v->log_fn = c->log; v->log_ud = c->log_ud; }
}

/* the first point a + k*P that is later than t */
static int64_t grid_next(int64_t a, int64_t P, int64_t t) {
  if (P <= 0) P = DEFAULT_PERIOD_US;
  if (a > t) return a - t <= P ? a : t + P;   /* a phase in the future: don't trust it far */
  return a + ((t - a) / P + 1) * P;
}

void video_note_shown(video_t *v, int64_t t_us, int64_t phase_us, uint32_t vblanks) {
  if (v->flips && vblanks > 1) v->missed += vblanks - 1;
  v->flips++;
  v->last_vblank_us = t_us;
  v->phase_us = phase_us;
  if (phase_us - t_us < 100000 && t_us - phase_us < 100000)
    v->lag_us = v->lag_us ? (v->lag_us * 15 + (phase_us - t_us)) / 16 : phase_us - t_us;
}

void video_soft_queue(video_t *v, int64_t now_us) {
  if (v->soft_due_us && v->soft_due_us <= now_us) video_soft_wait(v, 0, NULL);   /* already shown */
  v->soft_due_us = grid_next(v->phase_us, v->period_us, now_us);
}

int video_soft_wait(video_t *v, int timeout_ms, int64_t *t_us) {
  if (!v->soft_due_us) {
    if (t_us) *t_us = v->last_vblank_us;
    return 0;
  }
  int64_t now = plat_now_us();
  if (now < v->soft_due_us) {
    if (t_us) *t_us = v->last_vblank_us;
    if (timeout_ms <= 0) return 1;
    int64_t until = now + (int64_t)timeout_ms * 1000;
    if (until < v->soft_due_us) {
      plat_sleep_until_us(until);
      return 1;
    }
    plat_sleep_until_us(v->soft_due_us);
  }
  int64_t due = v->soft_due_us, P = v->period_us > 0 ? v->period_us : DEFAULT_PERIOD_US;
  v->soft_due_us = 0;
  int64_t n = (due - v->last_vblank_us + P / 2) / P;
  video_note_shown(v, due, due, n < 1 ? 1 : (uint32_t)(n > 1000000 ? 1000000 : n));
  if (t_us) *t_us = due;
  return 0;
}

bool video_soft_pending(const video_t *v) { return v->soft_due_us && plat_now_us() < v->soft_due_us; }

/* ================================================================ public API */

int video_present(video_t *v, const uint16_t *fb) {
  if (!v || !fb) return -1;
  return v->ops->present(v, fb);
}

int video_wait(video_t *v, int timeout_ms, int64_t *t_us) {
  if (!v) return -1;
  return v->ops->wait(v, timeout_ms, t_us);
}

bool video_pending(const video_t *v) { return v && v->ops->pending(v); }
int  video_fd(const video_t *v) { return v ? v->ops->fd(v) : -1; }
const video_info_t *video_get_info(const video_t *v) { return v ? &v->info : NULL; }

void video_get_stats(const video_t *v, video_stats_t *st) {
  memset(st, 0, sizeof *st);
  if (!v) return;
  st->period_us = v->period_us;
  st->last_vblank_us = v->last_vblank_us;
  st->margin_us = v->margin_us;
  st->event_lag_us = v->lag_us;
  st->late_on_time = v->late_on_time;
  st->flips = v->flips;
  st->missed = v->missed;
  st->reinits = v->reinits;
  st->down = v->down;
}

int64_t video_period_us(const video_t *v) { return v && v->period_us > 0 ? v->period_us : DEFAULT_PERIOD_US; }

int64_t video_deadline_us(const video_t *v, int64_t now_us) {
  if (!v) return now_us + DEFAULT_PERIOD_US;
  return grid_next(v->phase_us, v->period_us, now_us + v->margin_us) - v->margin_us;
}

int64_t video_frame_dt(int64_t deadline_us, int64_t last_deadline_us, int64_t period_us) {
  if (period_us <= 0) period_us = DEFAULT_PERIOD_US;
  int64_t n = last_deadline_us ? (deadline_us - last_deadline_us + period_us / 2) / period_us : 1;
  int64_t max = 1000000 / period_us;
  if (n > max) n = max;
  if (n < 1) n = 1;
  return n * period_us;
}

int video_debug(video_t *v, const char *action) {
  if (!v || !action || !v->ops->debug) return -1;
  return v->ops->debug(v, action);
}

void video_close(video_t *v) {
  if (v) v->ops->close(v);
}

/* ================================================================ none: a 60 Hz software clock */

static int none_present(video_t *v, const uint16_t *fb) {
  (void)fb;
  if (v->soft_due_us) video_soft_wait(v, 1000, NULL);  /* the previous frame first (<= 1 period) */
  video_soft_queue(v, plat_now_us());
  return 0;
}
static int  none_wait(video_t *v, int timeout_ms, int64_t *t_us) { return video_soft_wait(v, timeout_ms, t_us); }
static bool none_pending(const video_t *v) { return video_soft_pending(v); }
static int  none_fd(const video_t *v) { (void)v; return -1; }
static void none_close(video_t *v) { free(v); }
static const video_ops_t none_ops = { none_present, none_wait, none_pending, none_fd, none_close, NULL, NULL, NULL, NULL };

video_t *video_none_open(const video_config_t *c, video_info_t *info, char *err, int errlen) {
  (void)err; (void)errlen;
  video_t *v = calloc(1, sizeof *v);
  if (!v) {
    if (err && errlen > 0) snprintf(err, (size_t)errlen, "out of memory");
    return NULL;
  }
  int w = 640, h = 480;
  if (c && c->render_w > 0 && c->render_w <= 8192 && c->render_h > 0 && c->render_h <= 8192) {
    w = c->render_w;
    h = c->render_h;
  }
  video_base_init(v, &none_ops, c, 60.0);
  video_info_t *i = &v->info;
  plat_strlcpy(i->backend, "none", sizeof i->backend);
  i->render_w = i->mode_w = i->view_w = w;
  i->render_h = i->mode_h = i->view_h = h;
  i->hz = 60.0;
  snprintf(i->desc, sizeof i->desc, "no display, %dx%d at 60 Hz (software clock)", w, h);
  if (info) *info = v->info;
  return v;
}

/* ================================================================ dispatch */

static void set_err(char *err, int errlen, const char *fmt, const char *a, const char *b) {
  if (err && errlen > 0) snprintf(err, (size_t)errlen, fmt, a, b);
}

video_t *video_open(const video_config_t *c, video_info_t *info, char *err, int errlen) {
  static const video_config_t defaults = { 0 };
  if (!c) c = &defaults;
  if (err && errlen > 0) err[0] = 0;
  const char *b = c->backend && *c->backend ? c->backend : "auto";
  video_t *v = NULL;
  if (!strcmp(b, "drm")) v = video_drm_open(c, info, err, errlen);
  else if (!strcmp(b, "fb") || !strcmp(b, "fbdev")) v = video_fb_open(c, info, err, errlen);
  else if (!strcmp(b, "png")) v = video_png_open(c, info, err, errlen);
  else if (!strcmp(b, "none")) v = video_none_open(c, info, err, errlen);
  else if (!strcmp(b, "auto")) {
    char e1[160] = "", e2[160] = "";
    /* drm closes its fd when it fails: fbdev ioctls are refused while a DRM master exists */
    v = video_drm_open(c, info, e1, sizeof e1);
    if (!v) v = video_fb_open(c, info, e2, sizeof e2);
    if (!v) {
      v = video_none_open(c, info, NULL, 0);
      if (v) {
        snprintf(v->info.desc, sizeof v->info.desc, "no display (drm: %.48s; fb: %.48s)", e1, e2);
        video_log(v, "%s", v->info.desc);
      }
      set_err(err, errlen, "drm: %s; fb: %s", e1, e2);
    }
  } else {
    set_err(err, errlen, "unknown video backend '%s'%s", b, "");
    return NULL;
  }
  if (v && info) *info = v->info;
  return v;
}
