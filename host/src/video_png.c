/* Headless video: frames as PNG files, plus the PNG encoder for snapshots and the web
 * screenshot. See video.h. stb_image_write is compiled in here as static functions, so no
 * other module can collide with its symbols. */
#include "video.h"
#include "plat.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO          /* we write the files ourselves (errors, temp + rename) */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "stb_image_write.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

/* RGB565 -> packed RGB888 rows (the PNG's pixels) */
static void to_rgb(const uint16_t *fb, int w, int h, uint8_t *rgb) {
  size_t n = (size_t)w * (size_t)h;
  for (size_t i = 0; i < n; i++) video_rgb565_to_rgb888(fb[i], rgb + 3 * i);
}

/* the size checks every entry point shares: positive, and w*h*3 fits an int (stb's limit) */
static bool size_ok(int w, int h) {
  return w > 0 && h > 0 && w <= 65535 && h <= 65535 && (int64_t)w * h * 3 < INT_MAX / 2;
}

static unsigned char *encode_rgb(const uint8_t *rgb, int w, int h, int *len) {
  int n = 0;
  unsigned char *png = stbi_write_png_to_mem(rgb, w * 3, w, h, 3, &n);
  if (!png || n <= 0) {
    free(png);
    return NULL;
  }
  if (len) *len = n;
  return png;
}

unsigned char *video_png_encode(const uint16_t *fb, int w, int h, int *len) {
  if (len) *len = 0;
  if (!fb || !size_ok(w, h)) return NULL;
  uint8_t *rgb = malloc((size_t)w * (size_t)h * 3);
  if (!rgb) return NULL;
  to_rgb(fb, w, h, rgb);
  unsigned char *png = encode_rgb(rgb, w, h, len);
  free(rgb);
  return png;
}

int video_snapshot_png(const uint16_t *fb, int w, int h, const char *path) {
  if (!path || !*path) return -1;
  int n = 0;
  unsigned char *png = video_png_encode(fb, w, h, &n);
  if (!png) return -1;
  int r = plat_write_file_atomic(path, png, (size_t)n);
  free(png);
  return r == 0 ? 0 : -1;
}

/* ================================================================ the png backend */

typedef struct {
  video_t base;
  int w, h;
  int every;
  uint32_t n;              /* frames presented */
  uint8_t *rgb;            /* conversion buffer, allocated once */
  char dir[512];
  bool warned;
} pngv_t;

static int png_present(video_t *v, const uint16_t *fb) {
  pngv_t *p = (pngv_t *)v;
  uint32_t idx = p->n++;
  if (p->every > 0 && idx % (uint32_t)p->every == 0) {
    char path[600];
    snprintf(path, sizeof path, "%s/frame-%06u.png", p->dir, (unsigned)idx);
    to_rgb(fb, p->w, p->h, p->rgb);
    int len = 0;
    unsigned char *png = encode_rgb(p->rgb, p->w, p->h, &len);
    FILE *f = png ? fopen(path, "wb") : NULL;
    bool ok = f && fwrite(png, 1, (size_t)len, f) == (size_t)len;
    if (f && fclose(f) != 0) ok = false;
    free(png);
    if (!ok && !p->warned) {     /* once: a full disk shouldn't flood the log */
      video_log(v, "can't write %s", path);
      p->warned = true;
    }
  }
  /* no display: the frame is "shown" at once (headless runs use virtual time) */
  int64_t now = plat_now_us();
  video_note_shown(v, now, now, 1);
  return 0;
}

static int png_wait(video_t *v, int timeout_ms, int64_t *t_us) {
  (void)timeout_ms;
  if (t_us) *t_us = v->last_vblank_us;
  return 0;
}
static bool png_pending(const video_t *v) { (void)v; return false; }
static int  png_fd(const video_t *v) { (void)v; return -1; }
static void png_close(video_t *v) {
  pngv_t *p = (pngv_t *)v;
  free(p->rgb);
  free(p);
}
static const video_ops_t png_ops = { png_present, png_wait, png_pending, png_fd, png_close, NULL };

video_t *video_png_open(const video_config_t *c, video_info_t *info, char *err, int errlen) {
  int w = 640, h = 480;
  if (c && (c->render_w || c->render_h)) {
    w = c->render_w;
    h = c->render_h;
  }
  if (!size_ok(w, h) || w > 8192 || h > 8192) {
    if (err && errlen > 0) snprintf(err, (size_t)errlen, "png: bad render size %dx%d", w, h);
    return NULL;
  }
  const char *dir = c && c->png_dir && *c->png_dir ? c->png_dir : ".";
  int every = c && c->png_every > 0 ? c->png_every : 0;
  if (strlen(dir) >= sizeof ((pngv_t *)0)->dir) {
    if (err && errlen > 0) snprintf(err, (size_t)errlen, "png: directory name too long");
    return NULL;
  }
  if (every > 0) {
    plat_stat_t st;
    /* mkdir -p succeeds when the path exists, even as a file: check it is a directory */
    if (plat_mkdirs(dir) != 0 || plat_stat(dir, &st) != 0 || !st.exists || !st.is_dir) {
      if (err && errlen > 0) snprintf(err, (size_t)errlen, "png: can't create %s", dir);
      return NULL;
    }
  }
  pngv_t *p = calloc(1, sizeof *p);
  uint8_t *rgb = malloc((size_t)w * (size_t)h * 3);
  if (!p || !rgb) {
    free(p);
    free(rgb);
    if (err && errlen > 0) snprintf(err, (size_t)errlen, "png: out of memory");
    return NULL;
  }
  p->w = w;
  p->h = h;
  p->every = every;
  p->rgb = rgb;
  plat_strlcpy(p->dir, dir, sizeof p->dir);
  video_base_init(&p->base, &png_ops, c, 60.0);
  video_info_t *i = &p->base.info;
  plat_strlcpy(i->backend, "png", sizeof i->backend);
  i->render_w = i->mode_w = i->view_w = w;
  i->render_h = i->mode_h = i->view_h = h;
  i->hz = 60.0;
  if (every > 0) snprintf(i->desc, sizeof i->desc, "png %dx%d, every %d frame%s to %.60s", w, h, every, every == 1 ? "" : "s", dir);
  else snprintf(i->desc, sizeof i->desc, "png %dx%d (on request)", w, h);
  if (info) *info = p->base.info;
  return &p->base;
}
