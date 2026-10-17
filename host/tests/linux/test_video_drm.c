/* Linux test of the real video backends (drm, fb) and the console handling (sys.c).
 * Not part of build.sh's unit tests: it needs a KMS device (vc4 on the Pi, vkms in a VM) or
 * /dev/fb0, and root. It opens the backend through video_open(), runs a frame loop like
 * main.c's (late latching on video_deadline_us) with a moving test pattern, and reports the flip
 * intervals from the kernel's flip timestamps. Then it exercises the recovery paths through the
 * video_debug() hooks, VT switches, a second DRM master, close/reopen, and the VT handling of
 * sys.c (fatal signals in child processes).
 *
 *   test_video_drm [--backend drm|fb|auto] [--frames N] [--device PATH] [--output av|hdmi|auto]
 *                  [--tv NORM] [--av-mode interlaced|progressive] [--hdmi-mode WxH[@Hz]]
 *                  [--render WxH] [--margin PCT] [--filter smooth|sharp] [--budget US]
 *                  [--recovery] [--reopen N] [--vt] [--vt-switch] [--master] [--sys]
 *                  [--expect-hw | --expect-cpu] [--png FILE] [--hist] [--strict-timing]
 * Exit status: 0 when every check passed. */
#define _GNU_SOURCE
#include "video.h"
#include "sys.h"
#include "plat.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

#ifdef __aarch64__
/* the ioctl numbers of the spec's table (2.2) on aarch64 */
_Static_assert(DRM_IOCTL_MODE_PAGE_FLIP == 0xC01864B0u, "PAGE_FLIP");
_Static_assert(DRM_IOCTL_MODE_SETCRTC == 0xC06864A2u, "SETCRTC");
_Static_assert(DRM_IOCTL_MODE_SETPLANE == 0xC03064B7u, "SETPLANE");
_Static_assert(DRM_IOCTL_MODE_GETCONNECTOR == 0xC05064A7u, "GETCONNECTOR");
_Static_assert(DRM_IOCTL_MODE_ADDFB2 == 0xC06864B8u, "ADDFB2");
_Static_assert(DRM_IOCTL_MODE_CREATE_DUMB == 0xC02064B2u, "CREATE_DUMB");
_Static_assert(DRM_IOCTL_MODE_OBJ_SETPROPERTY == 0xC01864BAu, "OBJ_SETPROPERTY");
_Static_assert(sizeof(struct drm_event_vblank) == 32, "drm_event_vblank");
_Static_assert(sizeof(struct drm_mode_modeinfo) == 68, "drm_mode_modeinfo");
#endif

static int fails = 0, checks = 0;
#define EXPECT(cond, ...) do { checks++; if (!(cond)) { fails++; printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
  printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct {
  const char *backend, *device, *output, *tv, *av_mode, *hdmi_mode, *filter, *png;
  int frames, rw, rh, margin, budget_us, reopen;
  bool recovery, vt, vt_switch, master, sys, expect_hw, expect_cpu, hist, strict;
} opts_t;

static opts_t O = { .backend = "drm", .output = "auto", .tv = "ntsc", .av_mode = "interlaced", .hdmi_mode = "auto",
                    .frames = 300, .budget_us = 3000 };
static volatile int *quit;

/* ================================================================ the test pattern */

static uint16_t *fb;
static int FW, FH;

static uint16_t rgb(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

static void draw(uint32_t k) {
  static const uint16_t bars[8] = { 0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000 };
  int top = FH * 2 / 3;
  for (int y = 0; y < FH; y++) {
    uint16_t *row = fb + (size_t)y * FW;
    for (int x = 0; x < FW; x++) {
      if (y < top) row[x] = bars[x * 8 / FW];
      else { int g = x * 255 / (FW > 1 ? FW - 1 : 1); row[x] = rgb(g, g, g); }
    }
  }
  /* a moving bar (motion shows tearing and dropped frames) and the frame number in binary */
  int bw = FW / 40 > 2 ? FW / 40 : 2, bx = (int)((k * 4) % (uint32_t)FW);
  for (int y = 0; y < FH; y++)
    for (int x = bx; x < bx + bw && x < FW; x++) fb[(size_t)y * FW + x] = (k & 1) ? 0xF800 : 0xFFFF;
  for (int b = 0; b < 16; b++)
    for (int y = 4; y < 12 && y < FH; y++)
      for (int x = 4 + b * 12; x < 12 + b * 12 && x < FW; x++) fb[(size_t)y * FW + x] = (k >> b) & 1 ? 0x07E0 : 0x2104;
  /* a 1 px white border: the edges must survive the scaling */
  for (int x = 0; x < FW; x++) fb[x] = fb[(size_t)(FH - 1) * FW + x] = 0xFFFF;
  for (int y = 0; y < FH; y++) fb[(size_t)y * FW] = fb[(size_t)y * FW + FW - 1] = 0xFFFF;
}

/* ================================================================ statistics */

typedef struct { int64_t *v; int n, cap; } series_t;

static void s_add(series_t *s, int64_t x) {
  if (s->n == s->cap) {
    int c = s->cap ? s->cap * 2 : 1024;
    int64_t *p = realloc(s->v, sizeof(int64_t) * (size_t)c);
    if (!p) return;
    s->v = p;
    s->cap = c;
  }
  s->v[s->n++] = x;
}

static int cmp64(const void *a, const void *b) {
  int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
  return x < y ? -1 : x > y;
}

static void s_print(const char *name, series_t *s) {
  if (!s->n) { printf("  %-14s (none)\n", name); return; }
  int64_t *t = malloc(sizeof(int64_t) * (size_t)s->n);
  if (!t) return;
  memcpy(t, s->v, sizeof(int64_t) * (size_t)s->n);
  qsort(t, (size_t)s->n, sizeof *t, cmp64);
  double sum = 0, sq = 0;
  for (int i = 0; i < s->n; i++) { sum += (double)t[i]; sq += (double)t[i] * (double)t[i]; }
  double mean = sum / s->n, sd = sqrt(fmax(0, sq / s->n - mean * mean));
  printf("  %-14s n=%d mean=%.1f sd=%.1f min=%lld p50=%lld p95=%lld p99=%lld max=%lld us\n", name, s->n, mean, sd,
         (long long)t[0], (long long)t[s->n / 2], (long long)t[(s->n * 95) / 100], (long long)t[(s->n * 99) / 100],
         (long long)t[s->n - 1]);
  free(t);
}

static void s_hist(series_t *s, int64_t period) {
  if (!s->n || period <= 0) return;
  int bins[6] = { 0 };   /* < 0.5 P, ~1 P, ~2 P, ~3 P, ~4 P, more */
  for (int i = 0; i < s->n; i++) {
    double r = (double)s->v[i] / (double)period;
    int b = r < 0.5 ? 0 : r < 1.5 ? 1 : r < 2.5 ? 2 : r < 3.5 ? 3 : r < 4.5 ? 4 : 5;
    bins[b]++;
  }
  printf("  intervals in periods: <0.5:%d 1:%d 2:%d 3:%d 4:%d more:%d\n", bins[0], bins[1], bins[2], bins[3], bins[4], bins[5]);
}

typedef struct {
  int frames;
  uint32_t flips, missed, reinits;
  bool down_end, down_seen;
  int64_t elapsed_us, period_us, margin_us;
} run_t;

/* main.c's pacer: the render budget follows the p95 of the last 64 render+present times */
static int64_t budget_now(const int64_t *ring, int n, int64_t period) {
  int64_t t[64];
  int m = n < 64 ? n : 64;
  if (m < 8) return O.budget_us;
  memcpy(t, ring, sizeof(int64_t) * (size_t)m);
  qsort(t, (size_t)m, sizeof t[0], cmp64);
  int64_t b = t[(m * 95) / 100] + 1500;
  if (b < O.budget_us) b = O.budget_us;
  if (b > period - 1000) b = period - 1000;
  return b;
}

/* the frame loop of main.c: wait for the queued frame, sleep to deadline - budget, draw, present */
static run_t run_frames(video_t *v, int n, const char *label, bool print) {
  static uint32_t k = 0;
  static int64_t ring[64];
  static int nring = 0;
  series_t vbi = { 0 }, arr = { 0 }, pres = { 0 }, lat = { 0 }, rend = { 0 };
  video_stats_t s0, s1;
  video_get_stats(v, &s0);
  int64_t prev_vb = 0, prev_arr = 0, t0 = plat_now_us();
  bool down_seen = false;
  int i;
  for (i = 0; i < n && !*quit; i++) {
    if (video_pending(v)) {
      int64_t vb = 0;
      if (video_wait(v, 100, &vb) == 0) {
        int64_t a = plat_now_us();
        if (prev_vb && vb > prev_vb) s_add(&vbi, vb - prev_vb);
        if (prev_arr) s_add(&arr, a - prev_arr);
        s_add(&lat, a - vb);                      /* event arrival vs the kernel's timestamp */
        prev_vb = vb;
        prev_arr = a;
      }
    }
    video_stats_t st;
    video_get_stats(v, &st);
    if (st.down) down_seen = true;
    int64_t now = plat_now_us(), period = video_period_us(v);
    int64_t start = video_deadline_us(v, now) - budget_now(ring, nring, period);
    if (start > now && start - now < 2 * period) plat_sleep_until_us(start);
    int64_t d0 = plat_now_us();
    draw(k++);
    int64_t p0 = plat_now_us();
    video_present(v, fb);
    int64_t p1 = plat_now_us();
    s_add(&pres, p1 - p0);
    if (!video_pending(v)) {                        /* shown inside present (fb pan, png) */
      video_get_stats(v, &st);
      if (prev_vb && st.last_vblank_us > prev_vb) s_add(&vbi, st.last_vblank_us - prev_vb);
      if (st.last_vblank_us > prev_vb) prev_vb = st.last_vblank_us;
    }
    s_add(&rend, p0 - d0);
    ring[nring++ % 64] = p1 - d0;
  }
  if (video_pending(v)) video_wait(v, 200, NULL);
  video_get_stats(v, &s1);
  run_t r = { .frames = i, .flips = s1.flips - s0.flips, .missed = s1.missed - s0.missed, .reinits = s1.reinits - s0.reinits,
              .down_end = s1.down, .down_seen = down_seen || s1.down, .elapsed_us = plat_now_us() - t0,
              .period_us = s1.period_us, .margin_us = s1.margin_us };
  if (print) {
    printf("[%s] %d frames in %.3f s (%.2f fps): flips %u, missed refreshes %u, re-inits %u, down at end %s%s\n", label,
           r.frames, r.elapsed_us / 1e6, r.frames * 1e6 / (double)(r.elapsed_us ? r.elapsed_us : 1), r.flips, r.missed,
           r.reinits, r.down_end ? "yes" : "no", r.down_seen ? " (was down)" : "");
    printf("  period estimate %lld us (%.3f Hz), margin %lld us\n", (long long)r.period_us, 1e6 / (double)r.period_us,
           (long long)r.margin_us);
    s_print("flip interval", &vbi);
    s_print("arrival intvl", &arr);
    s_print("arrival-stamp", &lat);
    s_print("draw (render)", &rend);
    s_print("present()", &pres);
    if (O.hist) s_hist(&vbi, r.period_us);
  }
  free(vbi.v);
  free(arr.v);
  free(pres.v);
  free(lat.v);
  free(rend.v);
  return r;
}

/* ================================================================ VT helpers */

static int vt_get(int fd) {
  struct vt_stat st;
  return ioctl(fd, VT_GETSTATE, &st) == 0 ? st.v_active : -1;
}

static bool vt_switch_to(int fd, int n) {
  if (ioctl(fd, VT_ACTIVATE, n) != 0) return false;
  for (int i = 0; i < 200; i++) {                   /* poll: VT_WAITACTIVE could hang */
    if (vt_get(fd) == n) return true;
    plat_sleep_us(10000);
  }
  return false;
}

static void tty_modes(const char *tty, int *kd, int *kb) {
  *kd = *kb = -1;
  int fd = open(tty, O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) return;
  if (ioctl(fd, KDGETMODE, kd) != 0) *kd = -1;
  if (ioctl(fd, KDGKBMODE, kb) != 0) *kb = -1;
  close(fd);
}

/* ================================================================ suites */

static video_config_t make_config(void) {
  video_config_t c;
  memset(&c, 0, sizeof c);
  c.backend = O.backend;
  c.output = O.output;
  c.tv = O.tv;
  c.aspect = "4:3";
  c.av_mode = O.av_mode;
  c.hdmi_mode = O.hdmi_mode;
  c.margin_pct = O.margin;
  c.render_w = O.rw;
  c.render_h = O.rh;
  c.device = O.device;
  c.scale_filter = O.filter;
  c.card_wait_ms = 2000;
  return c;
}

static void print_info(const video_info_t *i) {
  printf("video: backend %s, device %s, connector %s, mode %dx%d%s %.3f Hz, view %d,%d %dx%d, render %dx%d, %s scaling%s\n",
         i->backend, i->device, i->connector, i->mode_w, i->mode_h, i->interlaced ? "i" : "", i->hz, i->view_x, i->view_y,
         i->view_w, i->view_h, i->render_w, i->render_h, i->hw_scaling ? "hardware" : "CPU/no",
         i->connected ? "" : ", no display seen");
  printf("video: %s\n", i->desc);
}

static bool alloc_fb(int w, int h) {
  free(fb);
  FW = w;
  FH = h;
  fb = calloc((size_t)w * (size_t)h, 2);
  return fb != NULL;
}

static void check_run(const run_t *r, const char *what, int min_pct) {
  EXPECT(r->frames > 0, "%s: no frames", what);
  EXPECT(!r->down_end, "%s: display down at the end", what);
  EXPECT(r->flips * 100 >= (uint32_t)(r->frames * min_pct), "%s: only %u flips for %d frames", what, r->flips, r->frames);
  /* paced: never faster than the refresh (a little slack for the first frame) */
  if (r->period_us > 0 && r->frames > 10)
    EXPECT(r->elapsed_us >= (int64_t)(r->frames - 2) * r->period_us * 9 / 10, "%s: %d frames in %lld us: not paced", what,
           r->frames, (long long)r->elapsed_us);
}

static void suite_recovery(video_t *v) {
  struct { const char *hook; int min_reinits, max_reinits; } h[] = {
    { "reinit", 1, 1 }, { "flipfail", 1, 1 }, { "loseevent", 1, 1 }, { "disable", 1, 1 }, { "rmfb", 1, 1 },
    { "dropmaster", 1, 1 }, { "steal", 0, 1 }, { "hotplug", 0, 1 },
  };
  for (size_t i = 0; i < sizeof h / sizeof h[0] && !*quit; i++) {
    video_stats_t s0, s1;
    video_get_stats(v, &s0);
    int64_t t0 = plat_now_us();
    int r = video_debug(v, h[i].hook);              /* "reinit" re-initializes right here */
    EXPECT(r == 0, "hook %s returned %d", h[i].hook, r);
    run_t rr = run_frames(v, 120, h[i].hook, true);
    video_get_stats(v, &s1);
    rr.reinits = s1.reinits - s0.reinits;
    printf("  -> %s: %u re-init(s), %d frames lost, %.0f ms\n", h[i].hook, rr.reinits, rr.frames - (int)rr.flips,
           (plat_now_us() - t0) / 1000.0);
    EXPECT(rr.reinits >= (uint32_t)h[i].min_reinits && rr.reinits <= (uint32_t)h[i].max_reinits, "%s: %u re-inits",
           h[i].hook, rr.reinits);
    check_run(&rr, h[i].hook, 85);
  }
  /* several faults in a row: the back-off must not turn into a crash loop, and it comes back */
  for (int i = 0; i < 6; i++) {
    video_debug(v, "flipfail");
    run_frames(v, 2, "burst", false);
  }
  run_t rr = run_frames(v, 300, "after burst", true);
  EXPECT(!rr.down_end && rr.flips > 100, "after a burst of failures: down %d, flips %u", rr.down_end, rr.flips);
  EXPECT(video_debug(v, "nonsense") == -1, "unknown hook");
}

/* KD_GRAPHICS makes the kernel ignore VT switches (change_console() with VT_AUTO), so another VT
 * can only come to the front after someone puts ours back into text mode */
static void suite_vt_switch(video_t *v) {
  int t0 = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC), t1 = open("/dev/tty1", O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (t0 < 0 || t1 < 0) {
    printf("vt-switch: no /dev/tty0 or /dev/tty1, skipped\n");
    if (t0 >= 0) close(t0);
    if (t1 >= 0) close(t1);
    return;
  }
  int kd = -1, kb = -1, orig = vt_get(t0);
  printf("vt-switch: VT %d in front\n", orig);
  /* a VT switch while we are in graphics mode is ignored */
  EXPECT(ioctl(t0, VT_ACTIVATE, 2) == 0, "VT_ACTIVATE");
  plat_sleep_us(300000);
  EXPECT(vt_get(t0) == orig, "the VT switch was not ignored in KD_GRAPHICS");

  /* A: someone puts our VT into text mode: fbcon force-restores its CRTC; we take it back */
  printf("vt-switch: KD_TEXT on tty1 (VT %d stays in front)\n", orig);
  EXPECT(ioctl(t1, KDSETMODE, KD_TEXT) == 0, "KD_TEXT");
  video_stats_t s0, s1;
  video_get_stats(v, &s0);
  run_t r = run_frames(v, 150, "text mode on our VT", true);
  video_get_stats(v, &s1);
  EXPECT(s1.reinits > s0.reinits, "fbcon's forced restore was not noticed");
  EXPECT(!r.down_end && r.flips > 100, "no picture after taking the display back: down %d flips %u", r.down_end, r.flips);
  tty_modes("/dev/tty1", &kd, &kb);
  EXPECT(kd == KD_GRAPHICS && kb == K_OFF, "our VT not back in graphics mode: kd %d kb %d", kd, kb);

  /* B: text mode and a switch to VT 2: give the display and master away, keep pacing */
  printf("vt-switch: KD_TEXT on tty1 and VT 2 to the front\n");
  ioctl(t1, KDSETMODE, KD_TEXT);
  EXPECT(vt_switch_to(t0, 2), "VT_ACTIVATE 2 after KD_TEXT");
  EXPECT(!sys_console_foreground(), "foreground while VT 2 is in front");
  r = run_frames(v, 150, "VT 2 in front", true);
  EXPECT(r.down_seen && r.down_end, "the display was not given up while another VT was in front");
  EXPECT(r.elapsed_us >= 140 * r.period_us * 9 / 10, "loop not paced while in the background");
  int fd = open(O.device ? O.device : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
  int m = fd >= 0 ? ioctl(fd, DRM_IOCTL_SET_MASTER, 0) : -1;
  EXPECT(m == 0, "another client can't become DRM master while we are in the background (%s)", strerror(errno));
  if (fd >= 0) close(fd);                          /* gives master up again */

  /* C: back to our VT: graphics mode again, master and the picture back */
  printf("vt-switch: back to VT %d\n", orig > 0 ? orig : 1);
  EXPECT(vt_switch_to(t0, orig > 0 ? orig : 1), "VT_ACTIVATE back");
  EXPECT(sys_console_foreground(), "not foreground after switching back");
  r = run_frames(v, 300, "VT back", true);
  EXPECT(!r.down_end && r.flips > 150, "picture not back after the VT switch: down %d flips %u", r.down_end, r.flips);
  tty_modes("/dev/tty1", &kd, &kb);
  EXPECT(kd == KD_GRAPHICS && kb == K_OFF, "our VT not back in graphics mode: kd %d kb %d", kd, kb);
  close(t0);
  close(t1);
}

/* another DRM master holds the card while we open: we must keep pacing and take over later */
static void suite_master(void) {
  video_config_t c = make_config();
  if (strcmp(O.backend, "drm")) return;
  const char *dev = O.device ? O.device : "/dev/dri/card0";
  int p[2];
  if (pipe(p)) return;
  pid_t pid = fork();
  if (pid == 0) {
    close(p[0]);
    int fd = open(dev, O_RDWR | O_CLOEXEC);
    int m = fd >= 0 ? ioctl(fd, DRM_IOCTL_SET_MASTER, 0) : -1;
    char ok = m == 0 ? 'y' : 'n';
    if (write(p[1], &ok, 1) != 1) _exit(2);
    plat_sleep_us(4000000);                         /* hold it for 4 s */
    _exit(0);
  }
  close(p[1]);
  char ok = 0;
  if (read(p[0], &ok, 1) != 1 || ok != 'y') {
    printf("master: the helper couldn't become master of %s, skipped\n", dev);
    close(p[0]);
    waitpid(pid, NULL, 0);
    return;
  }
  close(p[0]);
  printf("master: pid %d is DRM master of %s for 4 s\n", (int)pid, dev);
  video_info_t info;
  char err[200] = "";
  int64_t t0 = plat_now_us();
  video_t *v = video_open(&c, &info, err, sizeof err);
  printf("master: video_open took %.0f ms: %s\n", (plat_now_us() - t0) / 1000.0, v ? info.desc : err);
  EXPECT(v != NULL, "video_open must not fail while another master holds the card: %s", err);
  if (!v) { waitpid(pid, NULL, 0); return; }
  if (!alloc_fb(info.render_w, info.render_h)) { video_close(v); waitpid(pid, NULL, 0); return; }
  video_stats_t st;
  video_get_stats(v, &st);
  EXPECT(st.down, "should be down while another process is master");
  run_t r = run_frames(v, 120, "other master", true);
  EXPECT(r.down_seen && r.elapsed_us >= 110 * r.period_us * 9 / 10, "not paced while waiting for the master");
  waitpid(pid, NULL, 0);
  printf("master: the helper exited\n");
  run_t r2 = run_frames(v, 900, "after master left", true);   /* the back-off can be up to 10 s */
  EXPECT(!r2.down_end && r2.flips > 100, "no picture after the other master left: down %d flips %u", r2.down_end, r2.flips);
  video_close(v);
}

/* sys.c: VT graphics mode, keyboard off, restore on exit and on fatal signals */
static void suite_sys(void) {
  const char *tty = "/dev/tty1";
  int kd0, kb0;
  tty_modes(tty, &kd0, &kb0);
  if (kd0 < 0) { printf("sys: %s not usable, skipped\n", tty); return; }
  printf("sys: %s is in mode %d (0 text, 1 graphics), keyboard mode %d\n", tty, kd0, kb0);
  int kbr = kb0 == K_OFF ? K_UNICODE : kb0;       /* what a restore puts back */
  int kd, kb;
  /* in this process */
  EXPECT(!sys_console_is_graphics(), "graphics before the call");
  sys_console_graphics(tty);
  tty_modes(tty, &kd, &kb);
  EXPECT(sys_console_is_graphics() && kd == KD_GRAPHICS && kb == K_OFF, "after graphics: kd %d kb %d", kd, kb);
  sys_console_graphics(tty);                        /* twice: no change, no second fd */
  EXPECT(sys_console_is_graphics(), "second call");
  sys_console_restore();
  tty_modes(tty, &kd, &kb);
  EXPECT(!sys_console_is_graphics() && kd == KD_TEXT && kb == kbr, "after restore: kd %d kb %d", kd, kb);
  sys_console_restore();                            /* twice: harmless */
  /* a crash restores and still dies of the signal */
  static const int crash[] = { SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGQUIT };
  for (size_t i = 0; i < sizeof crash / sizeof crash[0]; i++) {
    pid_t pid = fork();
    if (pid == 0) {
      sys_console_graphics(tty);
      raise(crash[i]);
      _exit(3);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    tty_modes(tty, &kd, &kb);
    EXPECT(WIFSIGNALED(status) && WTERMSIG(status) == crash[i], "child after signal %d: status 0x%x", crash[i], status);
    EXPECT(kd == KD_TEXT && kb == kbr, "after signal %d: kd %d kb %d", crash[i], kd, kb);
  }
  /* exit() without a restore: the atexit handler */
  pid_t pid = fork();
  if (pid == 0) {
    sys_console_graphics(tty);
    exit(0);
  }
  int status = 0;
  waitpid(pid, &status, 0);
  tty_modes(tty, &kd, &kb);
  EXPECT(WIFEXITED(status) && kd == KD_TEXT && kb == kbr, "after exit(): kd %d kb %d", kd, kb);
  /* SIGTERM: the first sets the flag (the loop stops normally), a second one restores and kills */
  pid = fork();
  if (pid == 0) {
    volatile int *q = sys_quit_flag();
    sys_console_graphics(tty);
    raise(SIGTERM);
    int k1, b1;
    tty_modes(tty, &k1, &b1);
    if (!*q || k1 != KD_GRAPHICS) _exit(4);         /* still in graphics: the main loop restores */
    raise(SIGTERM);
    _exit(5);
  }
  waitpid(pid, &status, 0);
  tty_modes(tty, &kd, &kb);
  EXPECT(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM, "double SIGTERM: status 0x%x", status);
  EXPECT(kd == KD_TEXT && kb == kbr, "after double SIGTERM: kd %d kb %d", kd, kb);
  /* SIGKILL can't be handled: sys_console_reset (--restore-vt) repairs a dead program's VT */
  pid = fork();
  if (pid == 0) {
    sys_console_graphics(tty);
    for (;;) pause();
  }
  plat_sleep_us(200000);
  kill(pid, SIGKILL);
  waitpid(pid, &status, 0);
  tty_modes(tty, &kd, &kb);
  EXPECT(kd == KD_GRAPHICS && kb == K_OFF, "after SIGKILL the VT stays in graphics (expected): kd %d kb %d", kd, kb);
  EXPECT(sys_console_reset(tty) == 0, "sys_console_reset");
  tty_modes(tty, &kd, &kb);
  EXPECT(kd == KD_TEXT && kb == K_UNICODE, "after reset: kd %d kb %d", kd, kb);
  /* a run that finds K_OFF left over (a dead predecessor) restores to unicode, not K_OFF */
  int fd = open(tty, O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (fd >= 0) {
    ioctl(fd, KDSKBMODE, K_OFF);
    sys_console_graphics(tty);
    sys_console_restore();
    tty_modes(tty, &kd, &kb);
    EXPECT(kb == K_UNICODE, "restore after a leftover K_OFF: kb %d", kb);
    if (kb0 >= 0 && kb0 != K_OFF) ioctl(fd, KDSKBMODE, kb0);
    close(fd);
  }
  EXPECT(sys_console_reset("/dev/nonexistent-tty") == -1, "reset of a missing tty");
  sys_console_graphics("/dev/null");                /* not a VT: ignored */
  EXPECT(!sys_console_is_graphics(), "/dev/null is not a VT");
  EXPECT(sys_console_foreground(), "no VT handling means foreground");
  tty_modes(tty, &kd, &kb);
  printf("sys: %s back in mode %d, keyboard mode %d\n", tty, kd, kb);
}

static int parse_args(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
#define VAL() (i + 1 < argc ? argv[++i] : (fprintf(stderr, "%s needs a value\n", a), exit(2), ""))
    if (!strcmp(a, "--backend")) O.backend = VAL();
    else if (!strcmp(a, "--frames")) O.frames = atoi(VAL());
    else if (!strcmp(a, "--device")) O.device = VAL();
    else if (!strcmp(a, "--output")) O.output = VAL();
    else if (!strcmp(a, "--tv")) O.tv = VAL();
    else if (!strcmp(a, "--av-mode")) O.av_mode = VAL();
    else if (!strcmp(a, "--hdmi-mode")) O.hdmi_mode = VAL();
    else if (!strcmp(a, "--filter")) O.filter = VAL();
    else if (!strcmp(a, "--png")) O.png = VAL();
    else if (!strcmp(a, "--render")) { if (sscanf(VAL(), "%dx%d", &O.rw, &O.rh) != 2) return -1; }
    else if (!strcmp(a, "--margin")) O.margin = atoi(VAL());
    else if (!strcmp(a, "--budget")) O.budget_us = atoi(VAL());
    else if (!strcmp(a, "--reopen")) O.reopen = atoi(VAL());
    else if (!strcmp(a, "--recovery")) O.recovery = true;
    else if (!strcmp(a, "--vt")) O.vt = true;
    else if (!strcmp(a, "--vt-switch")) O.vt_switch = O.vt = true;
    else if (!strcmp(a, "--master")) O.master = true;
    else if (!strcmp(a, "--sys")) O.sys = true;
    else if (!strcmp(a, "--expect-hw")) O.expect_hw = true;
    else if (!strcmp(a, "--expect-cpu")) O.expect_cpu = true;
    else if (!strcmp(a, "--hist")) O.hist = true;
    else if (!strcmp(a, "--strict-timing")) O.strict = true;
    else { fprintf(stderr, "unknown option %s\n", a); return -1; }
#undef VAL
  }
  return 0;
}

int main(int argc, char **argv) {
  if (parse_args(argc, argv)) return 2;
  setvbuf(stdout, NULL, _IOLBF, 0);
  quit = sys_quit_flag();
  if (O.sys) suite_sys();
  if (O.master) suite_master();
  if (O.frames <= 0 && !O.recovery && !O.reopen) goto done;
  if (O.vt) sys_console_graphics(NULL);
  video_config_t c = make_config();
  video_info_t info;
  char err[256] = "";
  int64_t t0 = plat_now_us();
  video_t *v = video_open(&c, &info, err, sizeof err);
  printf("video_open took %.1f ms\n", (plat_now_us() - t0) / 1000.0);
  EXPECT(v != NULL, "video_open: %s", err);
  if (!v) goto done;
  print_info(&info);
  EXPECT(info.render_w > 0 && info.render_h > 0, "render size");
  if (O.rw) EXPECT(info.render_w == O.rw && info.render_h == O.rh, "render size %dx%d", info.render_w, info.render_h);
  if (O.expect_hw) EXPECT(info.hw_scaling, "expected hardware scaling/positioning");
  if (O.expect_cpu) EXPECT(!info.hw_scaling && strstr(info.desc, "CPU scaling"), "expected the CPU scaling fallback");
  if (!alloc_fb(info.render_w, info.render_h)) goto done;
  if (O.frames > 0) {
    run_t r = run_frames(v, O.frames, "steady", true);
    check_run(&r, "steady", 90);
    /* with the adaptive budget, frames go out at the refresh rate (not every 2nd refresh); only
     * on real hardware: in an emulated VM the drawing itself can take longer than a refresh */
    if (O.strict) EXPECT(r.missed * 20 <= (uint32_t)r.frames, "%u missed refreshes in %d frames", r.missed, r.frames);
    /* the period must match the mode (vblank timestamps on the plat_now_us clock) */
    const video_info_t *ci = video_get_info(v);
    if (ci->hz > 0) EXPECT(fabs(r.period_us - 1e6 / ci->hz) < 1e6 / ci->hz * 0.05, "period %lld us vs mode %.3f Hz",
                           (long long)r.period_us, ci->hz);
  }
  if (O.png) {
    EXPECT(video_snapshot_png(fb, FW, FH, O.png) == 0, "snapshot %s", O.png);
    printf("snapshot of the last frame: %s\n", O.png);
  }
  if (O.recovery && !strcmp(info.backend, "drm")) suite_recovery(v);
  if (O.vt_switch && !strcmp(info.backend, "drm")) suite_vt_switch(v);
  for (int i = 0; i < O.reopen && !*quit; i++) {
    video_close(v);
    t0 = plat_now_us();
    v = video_open(&c, &info, err, sizeof err);
    printf("reopen %d: video_open took %.1f ms: %s\n", i + 1, (plat_now_us() - t0) / 1000.0, v ? info.desc : err);
    EXPECT(v != NULL, "reopen %d: %s", i + 1, err);
    if (!v) break;
    EXPECT(info.render_w == FW && info.render_h == FH, "reopen changed the render size");
    run_t r = run_frames(v, 60, "after reopen", true);
    check_run(&r, "reopen", 85);
  }
  if (v) {
    t0 = plat_now_us();
    if (O.vt) sys_console_restore();                /* main.c's order should be: console, then video */
    video_close(v);
    printf("close took %.1f ms\n", (plat_now_us() - t0) / 1000.0);
  }
done:
  sys_console_restore();
  free(fb);
  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
