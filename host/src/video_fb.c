/* fbdev video output (/dev/fb0): the fallback when there is no KMS device (QEMU raspi3b's
 * bcm2708_fb, simplefb, a DRM driver's fbdev emulation). See video.h and drm-kms.md [10].
 * 16 or 32 bpp, double buffering by panning when the virtual height allows it, vsync from
 * FBIO_WAITFORVSYNC when it really waits, else a software clock. Always CPU scaling. */
#ifdef __linux__
#define _GNU_SOURCE
#endif
#include "video.h"
#include "plat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef __linux__

video_t *video_fb_open(const video_config_t *c, video_info_t *info, char *err, int errlen) {
  (void)c; (void)info;
  if (err && errlen > 0) snprintf(err, (size_t)errlen, "fbdev needs Linux");
  return NULL;
}

#else

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, __u32)
#endif

enum { VS_SOFT, VS_IOCTL };

typedef struct {
  video_t base;
  int fd;
  char path[32];
  struct fb_var_screeninfo var, var0;   /* current; as found (put back at close) */
  struct fb_fix_screeninfo fix;
  bool var_changed;
  uint8_t *mem;
  size_t mem_len;
  int pages, page;                 /* page: the one on screen */
  size_t single_off;               /* single buffer: where the visible area starts */
  int bpp, w, h;
  uint32_t line;
  uint32_t *pixlut;                /* RGB565 -> device pixel for layouts other than RGB565/XRGB8888 */
  int rw, rh;
  video_layout_t lay;
  uint16_t *xlut, *ylut;
  uint16_t *shadow;                /* single buffer with vsync: the frame waits for the vblank here */
  bool direct;                     /* drawing in the vblank takes too long: draw at present (tears) */
  int64_t draw_us;                 /* how long the draw after the vblank takes (average) */
  int vs;
  int strikes;                     /* WAITFORVSYNC that returned at once */
  bool pending;                    /* VS_IOCTL: a frame waits for its vblank */
  bool pan_warned;
} fbv_t;

static size_t page_off(const fbv_t *f, int p) {
  return f->pages == 2 ? (size_t)p * (size_t)f->h * f->line : f->single_off;
}

static void draw(fbv_t *f, size_t off, const uint16_t *src) {
  video_scale_copy_lut(src, f->rw, f->mem + off, (int)f->line, f->bpp, f->lay.view_x, f->lay.view_y,
                       f->lay.view_w, f->lay.view_h, f->xlut, f->ylut, f->pixlut);
}

static int64_t next_vblank(const fbv_t *f, int64_t now) {
  int64_t a = f->base.phase_us, P = f->base.period_us > 0 ? f->base.period_us : 16667;
  if (a > now) return a - now <= P ? a : now + P;
  return a + ((now - a) / P + 1) * P;
}

/* a vblank was seen at t: period estimate + the frame counts as shown */
static void vblank_seen(fbv_t *f, int64_t t) {
  int64_t P = f->base.period_us > 0 ? f->base.period_us : 16667;
  int64_t n = (t - f->base.last_vblank_us + P / 2) / P;
  if (n >= 1 && n < 1000 && f->base.flips) {
    int64_t per = (t - f->base.last_vblank_us) / n;
    if (per > 4000 && per < 100000) f->base.period_us = (f->base.period_us * 15 + per) / 16;
  }
  video_note_shown(&f->base, t, t, n < 1 ? 1 : n > 1000000 ? 1000000 : (uint32_t)n);
}

static int fb_wait(video_t *v, int timeout_ms, int64_t *t_us) {
  fbv_t *f = (fbv_t *)v;
  if (!f->pending) return video_soft_wait(v, timeout_ms, t_us);
  int64_t now = plat_now_us(), next = next_vblank(f, now);
  if ((int64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000 + 2000 < next - now) {
    if (timeout_ms > 0) plat_sleep_us((int64_t)timeout_ms * 1000);
    if (t_us) *t_us = f->base.last_vblank_us;
    return 1;
  }
  uint32_t crtc = 0;
  int r = ioctl(f->fd, FBIO_WAITFORVSYNC, &crtc);
  int64_t t = plat_now_us();
  if (r != 0) {
    video_log(v, "%s: FBIO_WAITFORVSYNC failed (%s): software clock", f->path, strerror(errno));
    f->vs = VS_SOFT;
  } else if (t - now < 500 && next - now > 3000) {
    /* "waited" no time although the next vblank is far: this driver doesn't really wait */
    if (++f->strikes > 5) {
      video_log(v, "%s: FBIO_WAITFORVSYNC doesn't wait: software clock", f->path);
      f->vs = VS_SOFT;
    }
  } else {
    f->strikes = 0;
  }
  if (f->shadow && f->pages == 1 && !f->direct) {
    /* in the vblank: no tear, if it fits. Slow framebuffer memory (deferred I/O fbdev of a USB or
     * virtual display) would add the whole draw to every wait: then draw at present instead */
    int64_t d0 = plat_now_us();
    draw(f, page_off(f, 0), f->shadow);
    int64_t dt = plat_now_us() - d0;
    f->draw_us = f->draw_us ? (f->draw_us * 7 + dt) / 8 : dt;
    if (f->draw_us > 2000) {
      video_log(v, "%s: drawing takes %.1f ms, longer than a vblank: drawing at once (may tear)", f->path, f->draw_us / 1000.0);
      f->direct = true;
    }
  }
  f->pending = false;
  vblank_seen(f, t);
  if (t_us) *t_us = t;
  return 0;
}

static int fb_present(video_t *v, const uint16_t *fb) {
  fbv_t *f = (fbv_t *)v;
  if (f->pending) fb_wait(v, 100, NULL);
  if (f->base.soft_due_us) video_soft_wait(v, 1000, NULL);
  if (f->pages == 2) {
    int back = f->page ^ 1;
    draw(f, page_off(f, back), fb);
    f->var.xoffset = 0;
    f->var.yoffset = (uint32_t)(back * f->h);
    f->var.activate = FB_ACTIVATE_VBL;
    int64_t t0 = plat_now_us();
    int r = ioctl(f->fd, FBIOPAN_DISPLAY, &f->var);
    int64_t t1 = plat_now_us();
    if (r == 0) {
      f->page = back;
      if (t1 - t0 >= 2000) {
        vblank_seen(f, t1);          /* the pan blocked until the flip (DRM emulation) */
        return 0;
      }
    } else {
      /* EBUSY (a DRM master appeared) or EINVAL: single buffer on the visible page */
      if (!f->pan_warned) video_log(v, "%s: pan failed (%s): single buffer", f->path, strerror(errno));
      f->pan_warned = true;
      f->single_off = page_off(f, f->page);
      f->pages = 1;
      draw(f, f->single_off, fb);
    }
  } else if (f->vs == VS_IOCTL && f->shadow && !f->direct) {
    memcpy(f->shadow, fb, (size_t)f->rw * (size_t)f->rh * 2);   /* drawn after the vblank */
    f->pending = true;
    return 0;
  } else {
    draw(f, page_off(f, 0), fb);
  }
  if (f->vs == VS_IOCTL) f->pending = true;
  else video_soft_queue(v, plat_now_us());
  return 0;
}

static bool fb_pending(const video_t *v) {
  const fbv_t *f = (const fbv_t *)v;
  return f->pending || video_soft_pending(v);
}

static int fb_fd(const video_t *v) { (void)v; return -1; }

static void fb_close(video_t *v) {
  fbv_t *f = (fbv_t *)v;
  if (f->mem) munmap(f->mem, f->mem_len);
  if (f->fd >= 0) {
    if (f->var_changed) {          /* give fbcon its layout back */
      f->var0.activate = FB_ACTIVATE_NOW;
      ioctl(f->fd, FBIOPUT_VSCREENINFO, &f->var0);
    } else if (f->pages == 2 && f->page != 0) {
      f->var.yoffset = 0;
      f->var.activate = FB_ACTIVATE_NOW;
      ioctl(f->fd, FBIOPAN_DISPLAY, &f->var);
    }
    close(f->fd);
  }
  free(f->pixlut);
  free(f->xlut);
  free(f->ylut);
  free(f->shadow);
  free(f);
}

static const video_ops_t fb_ops = { fb_present, fb_wait, fb_pending, fb_fd, fb_close, NULL, NULL };

static uint32_t chan(unsigned c8, const struct fb_bitfield *b) {
  if (!b->length) return 0;
  unsigned len = b->length > 16 ? 16 : b->length;
  uint32_t v = (uint32_t)((c8 * ((1u << len) - 1) + 127) / 255);
  return v << b->offset;
}

/* RGB565 and XRGB8888 are copied directly; anything else goes through a table */
static int setup_pixels(fbv_t *f) {
  const struct fb_var_screeninfo *v = &f->var;
  if (f->bpp == 16 && v->red.offset == 11 && v->red.length == 5 && v->green.offset == 5 &&
      v->green.length == 6 && v->blue.offset == 0 && v->blue.length == 5)
    return 0;
  if (f->bpp == 32 && v->red.offset == 16 && v->red.length == 8 && v->green.offset == 8 &&
      v->green.length == 8 && v->blue.offset == 0 && v->blue.length == 8)
    return 0;
  if (!v->red.length || !v->green.length || !v->blue.length) return -1;
  f->pixlut = malloc(sizeof(uint32_t) * 65536);
  if (!f->pixlut) return -1;
  uint32_t alpha = v->transp.length ? (((1u << (v->transp.length > 16 ? 16 : v->transp.length)) - 1) << v->transp.offset) : 0;
  for (uint32_t p = 0; p < 65536; p++) {
    uint8_t rgb[3];
    video_rgb565_to_rgb888((uint16_t)p, rgb);
    f->pixlut[p] = chan(rgb[0], &v->red) | chan(rgb[1], &v->green) | chan(rgb[2], &v->blue) | alpha;
  }
  return 0;
}

static double timing_hz(const struct fb_var_screeninfo *v) {
  if (!v->pixclock) return 0;     /* most drivers leave the timings empty */
  double ht = (double)v->xres + v->left_margin + v->right_margin + v->hsync_len;
  double vt = (double)v->yres + v->upper_margin + v->lower_margin + v->vsync_len;
  if (v->vmode & FB_VMODE_INTERLACED) vt /= 2;
  double hz = 1e12 / ((double)v->pixclock * ht * vt);
  return hz > 20 && hz < 250 ? hz : 0;
}

static int cmp_i64(const void *a, const void *b) {
  int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
  return x < y ? -1 : x > y;
}

/* Do vblank waits really wait? 8 waits: the median interval in 4..60 ms and most intervals
 * within 20% of it (one late wakeup on a busy system must not lose the vsync); 0 = no */
static double probe_vsync(int fd) {
  enum { N = 8 };
  int64_t t[N + 1], d[N - 1], s[N - 1];
  uint32_t crtc = 0;
  t[0] = plat_now_us();
  for (int i = 1; i <= N; i++) {
    if (ioctl(fd, FBIO_WAITFORVSYNC, &crtc) != 0) return 0;
    t[i] = plat_now_us();
  }
  for (int i = 0; i < N - 1; i++) d[i] = s[i] = t[i + 2] - t[i + 1];   /* the first wait ends at a random phase */
  qsort(s, N - 1, sizeof s[0], cmp_i64);
  int64_t med = s[(N - 1) / 2], sum = 0;
  if (med < 4000 || med > 60000) return 0;
  int good = 0;
  for (int i = 0; i < N - 1; i++)
    if (d[i] * 5 >= med * 4 && d[i] * 5 <= med * 6) { good++; sum += d[i]; }
  if (good < 4) return 0;
  return 1e6 / ((double)sum / good);
}

video_t *video_fb_open(const video_config_t *c, video_info_t *info, char *err, int errlen) {
  static const video_config_t defaults = { 0 };
  if (!c) c = &defaults;
  const char *path = c->device && !strncmp(c->device, "/dev/fb", 7) ? c->device : "/dev/fb0";
#define FAIL(...) do { if (err && errlen > 0) snprintf(err, (size_t)errlen, __VA_ARGS__); goto fail; } while (0)
  fbv_t *f = calloc(1, sizeof *f);
  if (!f) {
    if (err && errlen > 0) snprintf(err, (size_t)errlen, "out of memory");
    return NULL;
  }
  f->fd = -1;
  plat_strlcpy(f->path, path, sizeof f->path);
  video_base_init(&f->base, &fb_ops, c, 60.0);
  f->fd = open(path, O_RDWR | O_CLOEXEC);
  if (f->fd < 0) FAIL("%s: %s", path, strerror(errno));
  if (ioctl(f->fd, FBIOGET_VSCREENINFO, &f->var) || ioctl(f->fd, FBIOGET_FSCREENINFO, &f->fix))
    FAIL("%s: %s%s", path, strerror(errno), errno == EBUSY ? " (a DRM master is active)" : "");
  f->var0 = f->var;
  if (f->fix.type != FB_TYPE_PACKED_PIXELS || f->fix.visual != FB_VISUAL_TRUECOLOR)
    FAIL("%s: not a true-colour framebuffer (type %u visual %u)", path, f->fix.type, f->fix.visual);
  f->bpp = (int)f->var.bits_per_pixel;
  if (f->bpp != 16 && f->bpp != 32) FAIL("%s: %d bits per pixel (need 16 or 32)", path, f->bpp);
  f->w = (int)f->var.xres;
  f->h = (int)f->var.yres;
  if (f->w < 16 || f->h < 16 || f->w > 16384 || f->h > 16384) FAIL("%s: odd size %dx%d", path, f->w, f->h);
  /* double buffering wants a virtual height of 2x (vc4: needs drm_fbdev_overalloc=200) */
  if (f->var.yres_virtual < 2 * f->var.yres) {
    struct fb_var_screeninfo v2 = f->var;
    v2.yres_virtual = 2 * f->var.yres;
    v2.yoffset = 0;
    v2.activate = FB_ACTIVATE_NOW;
    if (!ioctl(f->fd, FBIOPUT_VSCREENINFO, &v2)) {
      f->var_changed = true;
      if (ioctl(f->fd, FBIOGET_VSCREENINFO, &f->var) || ioctl(f->fd, FBIOGET_FSCREENINFO, &f->fix))
        FAIL("%s: %s", path, strerror(errno));
      if ((int)f->var.xres != f->w || (int)f->var.yres != f->h || (int)f->var.bits_per_pixel != f->bpp)
        FAIL("%s: the driver changed the mode", path);
    }
  }
  f->line = f->fix.line_length ? f->fix.line_length : (uint32_t)f->w * (uint32_t)f->bpp / 8;
  if (f->line < (uint32_t)f->w * (uint32_t)f->bpp / 8) FAIL("%s: line length %u too short", path, f->line);
  f->mem_len = f->fix.smem_len ? f->fix.smem_len : (size_t)f->var.yres_virtual * f->line;
  size_t page_bytes = (size_t)f->h * f->line;
  if (f->mem_len < page_bytes) FAIL("%s: framebuffer memory too small", path);
  void *p = mmap(NULL, f->mem_len, PROT_READ | PROT_WRITE, MAP_SHARED, f->fd, 0);
  if (p == MAP_FAILED) FAIL("%s: mmap: %s", path, strerror(errno));
  f->mem = p;
  f->pages = f->var.yres_virtual >= 2 * f->var.yres && f->mem_len >= 2 * page_bytes ? 2 : 1;
  if (f->pages == 2) {             /* page 0 in front; a driver that can't pan: single buffer */
    f->var.xoffset = f->var.yoffset = 0;
    f->var.activate = FB_ACTIVATE_NOW;
    if (ioctl(f->fd, FBIOPAN_DISPLAY, &f->var)) {
      /* the DRM fbdev emulation refuses everything but reads while a DRM master exists */
      if (errno == EBUSY) video_log(&f->base, "%s: busy: a DRM master is active, the picture may not show", path);
      f->pages = 1;
    }
  }
  f->page = 0;
  f->single_off = (size_t)f->var.yoffset * f->line + (size_t)f->var.xoffset * (size_t)(f->bpp / 8);
  if (f->single_off + page_bytes > f->mem_len) f->single_off = 0;
  if (setup_pixels(f)) FAIL("%s: unsupported pixel layout", path);
  video_clear_buffer(f->mem, (int)f->line, f->bpp, f->w, f->h * f->pages);
  /* the picture: composite-sized framebuffers (fbcon on the VEC) have non-square pixels */
  bool hdmi = c->output && !strcmp(c->output, "hdmi");
  bool composite = !hdmi && f->w == 720 && (f->h == 480 || f->h == 576 || f->h == 240 || f->h == 288);
  int crw = c->render_w > 0 && c->render_h > 0 && c->render_w <= 8192 && c->render_h <= 8192 ? c->render_w : 0;
  int crh = crw ? c->render_h : 0;
  video_layout(f->w, f->h, composite, c->aspect, c->margin_pct, crw, crh, &f->lay);
  if (c->av_flicker && *c->av_flicker && strcmp(c->av_flicker, "off"))
    video_log(&f->base, "av_flicker works with KMS (vc4) only: not on %s", f->path);
  f->rw = f->lay.render_w;
  f->rh = f->lay.render_h;
  if (f->lay.view_w != f->rw && !(f->xlut = malloc(sizeof(uint16_t) * (size_t)f->lay.view_w))) FAIL("out of memory");
  if (f->lay.view_h != f->rh && !(f->ylut = malloc(sizeof(uint16_t) * (size_t)f->lay.view_h))) FAIL("out of memory");
  video_scale_luts(f->rw, f->rh, f->lay.view_w, f->lay.view_h, f->xlut, f->ylut);
  /* vsync: a real FBIO_WAITFORVSYNC, else the timings, else 60 Hz on a software clock */
  double hz = probe_vsync(f->fd);
  f->vs = hz > 0 ? VS_IOCTL : VS_SOFT;
  if (hz <= 0) hz = timing_hz(&f->var);
  if (hz <= 0) hz = 60.0;
  f->base.period_us = (int64_t)llround(1e6 / hz);
  f->base.phase_us = f->base.last_vblank_us = plat_now_us();
  if (f->vs == VS_IOCTL && f->pages == 1 && !(f->shadow = malloc((size_t)f->rw * (size_t)f->rh * 2))) FAIL("out of memory");

  video_info_t *i = &f->base.info;
  plat_strlcpy(i->backend, "fb", sizeof i->backend);
  i->render_w = f->rw;
  i->render_h = f->rh;
  i->mode_w = f->w;
  i->mode_h = f->h;
  i->view_x = f->lay.view_x;
  i->view_y = f->lay.view_y;
  i->view_w = f->lay.view_w;
  i->view_h = f->lay.view_h;
  i->hz = hz;
  i->interlaced = (f->var.vmode & FB_VMODE_INTERLACED) != 0;
  i->composite = composite;
  i->connected = true;
  const char *base = strrchr(path, '/');
  plat_strlcpy(i->connector, base ? base + 1 : path, sizeof i->connector);
  plat_strlcpy(i->device, path, sizeof i->device);
  char id[17];
  memcpy(id, f->fix.id, 16);
  id[16] = 0;
  snprintf(i->desc, sizeof i->desc, "%s (%s) %dx%d %d bpp, %s, %s %.2f Hz, render %dx%d", i->connector, id, f->w, f->h,
           f->bpp, f->pages == 2 ? "2 pages" : "1 page", f->vs == VS_IOCTL ? "vsync" : "software clock", hz, f->rw, f->rh);
  video_log(&f->base, "%s", i->desc);
  if (info) *info = f->base.info;
  return &f->base;
fail:
  fb_close(&f->base);
  return NULL;
#undef FAIL
}

#endif
