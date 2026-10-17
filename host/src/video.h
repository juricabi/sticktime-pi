/* Video output. Backends:
 *   drm   KMS dumb buffers + page flips (the device: Composite-1 or HDMI-A-n)
 *   fb    /dev/fb0 (fallback when there is no KMS device, e.g. QEMU raspi3b)
 *   png   no display: frames are written as PNG files (tests, screenshots); no pacing
 *   none  no display at all (the web UI still works); a 60 Hz software clock paces the loop
 * The game draws at a render size with square pixels (it never changes while open). drm lets the
 * hardware scaler (vc4 HVS, legacy SETPLANE) stretch the render buffer into the view rectangle of
 * the mode (composite pixels are not square: 640x480 -> 720x480 for NTSC 4:3), inside the margin.
 * When the driver can't scale (vkms, some USB displays), the CPU does it (nearest neighbour,
 * per-row LUTs) into mode-size buffers. fb always scales on the CPU.
 *
 * Frame loop contract (all calls from one thread):
 *   video_present()  copies the frame into the back buffer and queues it for the next vblank;
 *                    if the previous frame is still queued it first waits for it (bounded).
 *   video_wait()     blocks up to a timeout until the queued frame is on screen. Instead of
 *                    blocking, the loop may poll video_fd() (POLLIN) and then call
 *                    video_wait(v, 0, &t). Backends without an fd (fb, none, drm while it has no
 *                    display) need video_wait() with a timeout (or the next present) to advance.
 *   video_deadline_us() tells when the next frame must be queued (late latching).
 * Recovery is internal: when the display is lost (flip errors, flip timeout, HDMI hotplug,
 * another client took the CRTC, the card vanished) drm re-initializes with back-off and keeps
 * pacing with a software clock meanwhile, so the loop never stops and never crash-loops.
 * When our VT is not in front (only possible after someone put it back into text mode: the
 * kernel ignores VT switches away from a KD_GRAPHICS VT), drm hands the CRTC back to the
 * console, drops DRM master and takes both back when our VT is in front again.
 *
 * The console: call sys_console_graphics() (sys.h) before video_open() on the device, and
 * sys_console_restore() before video_close() on exit (fbcon takes the CRTC back first; in the
 * other order video_close() puts the console's CRTC state back itself, without a black flash). */
#ifndef VIDEO_H
#define VIDEO_H

#include <stdint.h>
#include <stdbool.h>

typedef struct video video_t;

typedef struct {
  const char *backend;     /* "auto" (drm, then fb, then none), "drm", "fb", "png", "none" */
  const char *output;      /* "av", "hdmi" or "auto": which connector type to drive */
  const char *tv;          /* "ntsc", "ntsc-j", "ntsc-443", "pal", "pal-m", "pal-n", "pal60", "secam" */
  const char *aspect;      /* "4:3" or "16:9": the shape of the goggles' / screen's picture
                            * (composite only; HDMI pixels are square, so the mode is the shape) */
  const char *av_mode;     /* "interlaced" (480i/576i) or "progressive" (240p/288p) */
  const char *hdmi_mode;   /* "auto" (1280x720@60 if offered, else preferred), "WxH" or "WxH@Hz"
                            * (an "i" after H asks for an interlaced mode) */
  int margin_pct;          /* 0..20: black border for goggles that crop the picture */
  int render_w, render_h;  /* png/none: the render size (0 = 640x480); drm/fb: 0 = from the mode
                            * (video_layout), else this size, fitted into the view */
  const char *png_dir;     /* png: directory for frames (NULL = ".") */
  int png_every;           /* png: write every Nth presented frame (0 = only on request) */
  const char *device;      /* drm: "/dev/dri/cardN" (NULL = find the KMS card); fb: "/dev/fbN"
                            * (NULL = /dev/fb0) */
  const char *scale_filter;/* drm with HVS scaling: "smooth" (default) or "sharp" (nearest) */
  int card_wait_ms;        /* drm: wait for a KMS card to appear: 0 = 15 s while the system boots
                            * (uptime < 60 s), else no wait; < 0 = never wait */
  /* log lines (no trailing newline); NULL = stderr. Called from the frame loop thread. */
  void (*log)(void *ud, const char *line);
  void *log_ud;
} video_config_t;

typedef struct {
  char backend[8];         /* the one that opened */
  int render_w, render_h;  /* LCD_W, LCD_H for the game (fixed while open) */
  int mode_w, mode_h;      /* output mode (full frame for interlaced) */
  int view_x, view_y, view_w, view_h;   /* the picture inside the mode */
  double hz;               /* refresh (field rate for interlaced modes) */
  bool interlaced;
  bool composite;          /* non-square output pixels (layout used the aspect setting) */
  bool hw_scaling;         /* drm: the plane scales/positions the picture (SETPLANE); false: the
                            * CPU scales into mode-size buffers, or the render size is the mode */
  bool connected;          /* a display is (probably) there: false for HDMI "blind" mode */
  char connector[24];      /* "Composite-1", "HDMI-A-1", "Virtual-1", "fb0", "" */
  char device[32];         /* "/dev/dri/card1", "/dev/fb0", "" */
  char desc[128];          /* e.g. "Composite-1 720x480i 59.94 Hz, NTSC 4:3, render 640x480" */
} video_info_t;

/* Where the picture goes in a mode (exported for tests):
 *   render size: AV 720x480: 4:3 -> 640x480, 16:9 -> 854x480; 720x576: 768x576 / 1024x576;
 *   720x240 / 720x288 (progressive): 320x240 / 427x240 / 384x288 / 512x288;
 *   square-pixel modes (HDMI): the mode itself (but at most 1920x1200, scaled down keeping the
 *   shape, so a 4K screen doesn't make the game draw 8 MP).
 * margin_pct (clamped to 0..20) insets the picture by that share of the width and of the
 * height on every side. A computed render size follows the inset view (e.g. AV 4:3 with 10%:
 * 512x384 into 576x384 at 72,48), so the picture is never scaled vertically.
 * render_w/h > 0: use that size and fit it into the inset area keeping its shape (centered);
 * a size equal to the automatic one fills the area exactly (drm re-fits its fixed render size
 * after every mode change, and the rounded-up width must not cost a line).
 * Width needs rounding up (854, 427): ceil. Returns 0, or -1 for a bad mode size (out is then
 * a 640x480 full-screen layout). */
typedef struct {
  int render_w, render_h;
  int view_x, view_y, view_w, view_h;
} video_layout_t;
int  video_layout(int mode_w, int mode_h, bool composite, const char *aspect, int margin_pct,
                  int render_w, int render_h, video_layout_t *out);
/* the render size alone, without margin (the original contract) */
void video_render_size(int mode_w, int mode_h, bool composite, const char *aspect, int *rw, int *rh);

video_t *video_open(const video_config_t *c, video_info_t *info, char *err, int errlen);
/* Scale/convert fb (render_w x render_h RGB565) into the back buffer and queue it for the
 * next vblank. 0 ok (also when the frame was dropped because the display is gone: recovery runs
 * by itself), -1 bad arguments. If the previous flip is still pending, it waits for it first. */
int  video_present(video_t *v, const uint16_t *fb);
/* Wait until the queued frame is on screen, up to timeout_ms (0 = just check).
 * *t_us = when the last frame went on screen (vblank time on the plat_now_us clock: the kernel's
 * flip timestamp on drm, estimated for backends without events). 0 shown (or nothing pending:
 * returns at once), 1 not yet (timeout, or a signal interrupted the wait), -1 bad arguments. */
int  video_wait(video_t *v, int timeout_ms, int64_t *t_us);
bool video_pending(const video_t *v);
/* pollable flip-event fd, -1 if none. It can change after a recovery: ask every frame. */
int  video_fd(const video_t *v);
/* the current state (mode and view change after an HDMI hotplug; render size never does) */
const video_info_t *video_get_info(const video_t *v);

typedef struct {
  int64_t period_us;       /* measured refresh period (field period for interlaced) */
  int64_t last_vblank_us;  /* when the last frame went on screen */
  int64_t margin_us;       /* safety margin before the vblank used by video_deadline_us */
  uint32_t flips;          /* frames that went on screen */
  uint32_t missed;         /* refreshes that showed an old frame again (late frames) */
  uint32_t reinits;        /* recoveries (full re-initializations) */
  bool down;               /* no display right now (drm: waiting to recover) */
} video_stats_t;
void    video_get_stats(const video_t *v, video_stats_t *st);
int64_t video_period_us(const video_t *v);
/* Late latching: the latest time (plat_now_us clock) after now_us by which video_present() must
 * have queued a frame so it shows at the coming refresh (vblank phase - safety margin). */
int64_t video_deadline_us(const video_t *v, int64_t now_us);

/* write the last presented frame as PNG (any backend; temp file + rename); 0 ok, -1 error */
int  video_snapshot_png(const uint16_t *fb, int w, int h, const char *path);
/* encode RGB565 as PNG into a malloc'd buffer (web screenshots; thread-safe); NULL on failure */
unsigned char *video_png_encode(const uint16_t *fb, int w, int h, int *len);
void video_close(video_t *v);

/* Test hooks (drm, for test_video_drm): "reinit" (a full re-init now), "hotplug" (probe the
 * connectors as after a uevent), "rmfb" (remove the front buffer under us), "disable" (switch
 * the CRTC off), "dropmaster", "steal" (a foreign fb on the CRTC, like fbcon's forced restore),
 * "flipfail" (the next page flip fails with EINVAL), "loseevent" (the next flip's event never
 * arrives). 0 done, -1 unknown here or not possible now. */
int  video_debug(video_t *v, const char *action);

/* ------------------------------------------------------------------ backends (video.c dispatches) */
video_t *video_drm_open(const video_config_t *c, video_info_t *info, char *err, int errlen);
video_t *video_fb_open(const video_config_t *c, video_info_t *info, char *err, int errlen);
video_t *video_png_open(const video_config_t *c, video_info_t *info, char *err, int errlen);
video_t *video_none_open(const video_config_t *c, video_info_t *info, char *err, int errlen);

/* the per-backend ops, filled by each backend */
typedef struct {
  int  (*present)(video_t *v, const uint16_t *fb);
  int  (*wait)(video_t *v, int timeout_ms, int64_t *t_us);
  bool (*pending)(const video_t *v);
  int  (*fd)(const video_t *v);
  void (*close)(video_t *v);
  int  (*debug)(video_t *v, const char *action);   /* optional */
} video_ops_t;
struct video {
  const video_ops_t *ops;
  video_info_t info;
  /* timing on the plat_now_us clock, kept by the helpers below */
  int64_t period_us;       /* refresh period */
  int64_t last_vblank_us;  /* when the last frame went on screen */
  int64_t phase_us;        /* vblank phase reference for deadlines (drm: flip-event arrival) */
  int64_t margin_us;       /* safety margin before the vblank */
  int64_t soft_due_us;     /* software clock: when the queued frame shows (0 = nothing queued) */
  uint32_t flips, missed, reinits;
  bool down;
  void (*log_fn)(void *ud, const char *line);   /* video_config_t.log, NULL = stderr */
  void *log_ud;
};

/* helpers for backends (video.c) */
void video_base_init(video_t *v, const video_ops_t *ops, const video_config_t *c, double hz);
/* a frame went on screen at t_us; vblanks = refreshes since the previous shown frame (0 = unknown) */
void video_note_shown(video_t *v, int64_t t_us, int64_t phase_us, uint32_t vblanks);
/* software clock (none, fb without vsync, drm while down): the frame shows at the next tick */
void video_soft_queue(video_t *v, int64_t now_us);
int  video_soft_wait(video_t *v, int timeout_ms, int64_t *t_us);
bool video_soft_pending(const video_t *v);
void video_log(video_t *v, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
  ;
/* parse an hdmi_mode string: 1 = WxH[i][@Hz] (hz 0 when absent), 0 = "auto"/empty, -1 = invalid */
int  video_parse_mode(const char *s, int *w, int *h, double *hz, bool *interlaced);

/* ------------------------------------------------------------------ mode selection
 * Portable, so the choices can be tested anywhere; video_drm.c feeds it the connector's list.
 * video_mode_t has the layout of struct drm_mode_modeinfo and the flag/type bits have the DRM
 * values (video_drm.c asserts both). */
#define VIDEO_MODE_FLAG_PHSYNC      (1u << 0)
#define VIDEO_MODE_FLAG_NHSYNC      (1u << 1)
#define VIDEO_MODE_FLAG_PVSYNC      (1u << 2)
#define VIDEO_MODE_FLAG_NVSYNC      (1u << 3)
#define VIDEO_MODE_FLAG_INTERLACE   (1u << 4)
#define VIDEO_MODE_FLAG_DBLSCAN     (1u << 5)
#define VIDEO_MODE_FLAG_PIC_AR_MASK (0xFu << 19)   /* SETCRTC refuses these without a client cap */
#define VIDEO_MODE_TYPE_PREFERRED   (1u << 3)
#define VIDEO_MODE_TYPE_USERDEF     (1u << 5)
#define VIDEO_MODE_TYPE_DRIVER      (1u << 6)
typedef struct {
  uint32_t clock;          /* pixel clock, kHz */
  uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
  uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
  uint32_t vrefresh, flags, type;
  char name[32];
} video_mode_t;
#define VIDEO_MAX_CANDIDATES 12
/* refresh in Hz from the timings: the field rate for interlaced modes (what flips follow) */
double video_mode_hz(const video_mode_t *m);
bool   video_same_timing(const video_mode_t *a, const video_mode_t *b);   /* ignores AR bits, name, type */
/* the "TV mode" connector property value for a tv setting: ntsc NTSC, ntsc-j NTSC-J, ntsc-443
 * NTSC-443, pal-m PAL-M, pal PAL, pal-n PAL-N, secam SECAM, and pal60 PAL (PAL colour on the
 * 525-line mode: vc4.tv_norm=PAL60 isn't parsed by the kernel); unknown -> NTSC */
const char *video_tv_norm(const char *tv);
bool   video_tv_625(const char *tv);                /* 625 lines / 50 Hz: pal, pal-n, secam */
/* (norm, htotal) pairs the vc4 VEC accepts: PAL and Mono with 858 or 864, PAL-N and SECAM with
 * 864, the NTSC family and PAL-M with 858 */
bool   video_vec_valid(const char *norm, unsigned htotal);
/* Candidate modes, best first (the first one SETCRTC accepts wins), AR bits cleared, no
 * duplicates, at most VIDEO_MAX_CANDIDATES; returns how many (always >= 1: built-in copies of
 * the standard modes come after the list's own).
 * composite: 720x480i / 720x576i by norm (interlaced) or 720x240 / 720x288 (progressive),
 * preferring the list's PREFERRED copy; then the built-in mode, the list's preferred ones, its
 * first, the other scan type of the norm, the other norm.
 * hdmi: an explicit hdmi_mode (closest refresh, 60 Hz when none is given), then auto:
 * 1280x720@60, 1920x1080@60, a preferred progressive mode >= 50 Hz up to 1920x1200, the
 * progressive mode nearest 720 lines, the built-in CEA 1280x720@60 (what an empty list, i.e. no
 * display, gets: blind mode), then the rest. *note: 0, 1 = hdmi_mode not understood (auto used),
 * 2 = the list doesn't offer the hdmi_mode (a built-in copy is tried when there is one). */
int    video_composite_modes(const video_mode_t *list, int n, const char *tv, bool progressive, video_mode_t *out);
int    video_hdmi_modes(const video_mode_t *list, int n, const char *hdmi_mode, video_mode_t *out, int *note);

/* Scaling helpers shared by drm and fb: copy src (sw x sh RGB565) into dst (pitch bytes per
 * row, bpp 16 = RGB565 or 32 = XRGB8888) at (dx, dy) size dw x dh, nearest neighbour with per-row
 * LUTs. xlut / ylut: dw / dh entries prepared by video_scale_luts() (NULL, NULL = 1:1, then
 * dw == sw and dh == sh). Sizes up to 65535. dst may be write-combined memory: only written. */
void video_scale_luts(int sw, int sh, int dw, int dh, uint16_t *xlut, uint16_t *ylut);
void video_scale_copy(const uint16_t *src, int sw, void *dst, int pitch, int bpp,
                      int dx, int dy, int dw, int dh, const uint16_t *xlut, const uint16_t *ylut);
/* the same through a 65536-entry table RGB565 -> device pixel (other fb layouts: BGR, 555 ...) */
void video_scale_copy_lut(const uint16_t *src, int sw, void *dst, int pitch, int bpp,
                          int dx, int dy, int dw, int dh, const uint16_t *xlut, const uint16_t *ylut,
                          const uint32_t *pixlut);
/* fill the area outside the view black (both buffers, once after a mode set) */
void video_clear_buffer(void *dst, int pitch, int bpp, int w, int h);
/* RGB565 -> RGB888 with bit replication (like engine.js toRGBA) */
static inline void video_rgb565_to_rgb888(uint16_t p, uint8_t *rgb) {
  unsigned r = p >> 11, g = (p >> 5) & 63, b = p & 31;
  rgb[0] = (uint8_t)((r << 3) | (r >> 2));
  rgb[1] = (uint8_t)((g << 2) | (g >> 4));
  rgb[2] = (uint8_t)((b << 3) | (b >> 2));
}

#endif
