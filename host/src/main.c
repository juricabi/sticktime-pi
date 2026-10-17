/* sticktime-pi: runs the StickTime EdgeTX tool natively and shows it on composite or HDMI.
 *
 *   sticktime-pi [options]
 *     --base DIR        /boot/firmware/sticktime (bin/, sd/, web/, editor/, fonts/, music/)
 *     --settings FILE   /boot/firmware/sticktime.txt
 *     --libexec DIR     helper programs (/usr/lib/sticktime)
 *     --state DIR       runtime state (/run/sticktime)
 *     --fonts DIR       Roboto TTFs (default: BASE/fonts)
 *     --video B         auto | drm | fb | png | none      --size WxH (png/none)
 *     --png-dir DIR     --png-every N                      frames as PNG (png backend)
 *     --frames N        stop after N frames                --port N (web, default 80; 0 = off)
 *     --no-inputs       no evdev / WiFi joystick           --no-audio, --audio OUT
 *     --vtx-port SPEC   VTX control line instead of the vtx_port setting: gpio4 (one pin), uart3..uart5,
 *                       gpio:/dev/gpiochipN:OFFSET, tty:PATH, tcp:HOST:PORT (a fake VTX in tests)
 *     --vtx-test SPEC   send VTX frames on that line, print the answers and the line's timing, exit
 *                       (--vtx-test-proto smartaudio|tramp, --vtx-test-frames N; stop the service first)
 *     --vtx-loopback-test TX RX   test patterns from pin TX to pin RX wired through 1 kOhm, no VTX
 *     --scenario FILE   run a headless test scenario (virtual time, see scenario.c) and exit
 *     --stub-fonts ADV  fonts that draw nothing (engine.js comparisons)
 *     --rt-prio N       SCHED_FIFO priority of the frame loop (default 50 on Linux, 0 = off)
 *     --cpu N           pin the frame loop to CPU N (default 3 on a 4-core Pi, -1 = off)
 *     --print-template  print the default sticktime.txt      --version */
#include "app.h"
#include "host.h"
#include "scenario.h"
#include "sys.h"
#include "vtx.h"
#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *base, *settings, *libexec, *state, *fonts, *scenario;
  int frames, port;
  const char *vtx_test, *vtx_test_proto, *vtx_loop_tx, *vtx_loop_rx;
  int vtx_test_frames;
  host_options_t h;
} args_t;

static void usage(void) {
  fputs("usage: sticktime-pi [--base DIR] [--settings FILE] [--libexec DIR] [--state DIR] [--fonts DIR]\n"
        "                    [--video auto|drm|fb|png|none] [--size WxH] [--png-dir DIR] [--png-every N]\n"
        "                    [--frames N] [--port N] [--no-inputs] [--no-audio] [--audio OUT] [--vtx-port SPEC]\n"
        "                    [--scenario FILE] [--stub-fonts ADV] [--rt-prio N] [--cpu N]\n"
        "                    [--print-template] [--version]\n", stderr);
}

static int parse(args_t *a, int argc, char **argv) {
  memset(a, 0, sizeof *a);
  a->base = "/boot/firmware/sticktime";
  a->settings = "/boot/firmware/sticktime.txt";
  a->libexec = "/usr/lib/sticktime";
  a->state = "/run/sticktime";
  a->port = 80;
#ifdef __linux__
  a->h.rt_prio = 50;
  a->h.rt_cpu = 3;
#else
  a->h.rt_cpu = -1;
#endif
  a->h.async_writes = true;
  for (int i = 1; i < argc; i++) {
    const char *o = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define TAKE(dst) do { if (!v) { fprintf(stderr, "%s needs a value\n", o); return -1; } dst = v; i++; } while (0)
    if (!strcmp(o, "--base")) TAKE(a->base);
    else if (!strcmp(o, "--settings")) TAKE(a->settings);
    else if (!strcmp(o, "--libexec")) TAKE(a->libexec);
    else if (!strcmp(o, "--state")) TAKE(a->state);
    else if (!strcmp(o, "--fonts")) TAKE(a->fonts);
    else if (!strcmp(o, "--video")) TAKE(a->h.video_backend);
    else if (!strcmp(o, "--png-dir")) TAKE(a->h.png_dir);
    else if (!strcmp(o, "--scenario")) TAKE(a->scenario);
    else if (!strcmp(o, "--audio")) TAKE(a->h.audio_out);
    else if (!strcmp(o, "--vtx-port")) TAKE(a->h.vtx_transport);
    else if (!strcmp(o, "--vtx-test")) TAKE(a->vtx_test);
    else if (!strcmp(o, "--vtx-test-proto")) TAKE(a->vtx_test_proto);
    else if (!strcmp(o, "--vtx-test-frames")) { const char *s; TAKE(s); a->vtx_test_frames = atoi(s); }
    else if (!strcmp(o, "--vtx-loopback-test")) {
      if (i + 2 >= argc) { fprintf(stderr, "--vtx-loopback-test needs TX and RX (like gpio4 gpio17)\n"); return -1; }
      a->vtx_loop_tx = argv[++i];
      a->vtx_loop_rx = argv[++i];
    }
    else if (!strcmp(o, "--size")) {
      const char *s; TAKE(s);
      if (sscanf(s, "%dx%d", &a->h.render_w, &a->h.render_h) != 2 || a->h.render_w < 64 || a->h.render_h < 64 ||
          a->h.render_w > 4096 || a->h.render_h > 4096) { fprintf(stderr, "bad --size %s\n", s); return -1; }
    }
    else if (!strcmp(o, "--png-every")) { const char *s; TAKE(s); a->h.png_every = atoi(s); }
    else if (!strcmp(o, "--frames")) { const char *s; TAKE(s); a->frames = atoi(s); }
    else if (!strcmp(o, "--port")) { const char *s; TAKE(s); a->port = atoi(s); }
    else if (!strcmp(o, "--rt-prio")) { const char *s; TAKE(s); a->h.rt_prio = atoi(s); }
    else if (!strcmp(o, "--cpu")) { const char *s; TAKE(s); a->h.rt_cpu = atoi(s); }
    else if (!strcmp(o, "--stub-fonts")) { const char *s; TAKE(s); a->h.stub_fonts = true; a->h.stub_adv = (float)atof(s); }
    else if (!strcmp(o, "--no-inputs")) a->h.no_inputs = true;
    else if (!strcmp(o, "--no-audio")) a->h.no_audio = true;
    else if (!strcmp(o, "--sync-writes")) a->h.async_writes = false;
    else if (!strcmp(o, "--print-template")) { fputs(SETTINGS_TEMPLATE, stdout); exit(0); }
    else if (!strcmp(o, "--version")) { printf("sticktime-pi %s\n", STP_VERSION); exit(0); }
    else if (!strcmp(o, "--help") || !strcmp(o, "-h")) { usage(); exit(0); }
    else { fprintf(stderr, "unknown option %s\n", o); usage(); return -1; }
#undef TAKE
  }
  return 0;
}

static void set_paths(app_t *app, const args_t *a) {
  app_paths_t *p = &app->paths;
  plat_strlcpy(p->base, a->base, sizeof p->base);
  plat_strlcpy(p->settings, a->settings, sizeof p->settings);
  snprintf(p->sd, sizeof p->sd, "%s/sd", a->base);
  snprintf(p->web, sizeof p->web, "%s/web", a->base);
  if (a->fonts) plat_strlcpy(p->fonts, a->fonts, sizeof p->fonts);
  else snprintf(p->fonts, sizeof p->fonts, "%s/fonts", a->base);
  plat_strlcpy(p->libexec, a->libexec, sizeof p->libexec);
  plat_strlcpy(p->state, a->state, sizeof p->state);
  plat_strlcpy(p->script, "/SCRIPTS/TOOLS/StickTime.lua", sizeof p->script);
}

/* frame pacing ("late latching"): start each frame just early enough to make the next vblank,
 * so the sticks are read as late as possible. The budget follows the 95th percentile of the
 * frame's work (run + copy) plus a margin, and grows after a missed vblank. */
typedef struct {
  float work_ms[128];
  int n;
  int64_t budget_us;
} pacer_t;

static int cmpf(const void *a, const void *b) {
  float x = *(const float *)a, y = *(const float *)b;
  return x < y ? -1 : x > y;
}

static void pacer_note(pacer_t *p, int64_t work_us, int64_t period_us, bool missed) {
  p->work_ms[p->n++ % 128] = (float)(work_us / 1000.0);
  int n = p->n < 128 ? p->n : 128;
  float tmp[128];
  memcpy(tmp, p->work_ms, sizeof(float) * (size_t)n);
  qsort(tmp, (size_t)n, sizeof(float), cmpf);
  int64_t p95 = (int64_t)(tmp[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1] * 1000.0f);
  int64_t b = p95 + 500;
  if (missed) b = p->budget_us + 2000 > b ? p->budget_us + 2000 : b;
  /* fall back slowly, jump up fast */
  if (b < p->budget_us) b = p->budget_us - (p->budget_us - b) / 16;
  if (b < 1000) b = 1000;
  if (b > period_us - 1000) b = period_us - 1000;
  p->budget_us = b;
}

static int device_loop(host_t *h, int max_frames) {
  volatile int *quit = sys_quit_flag();
  pacer_t pc;
  memset(&pc, 0, sizeof pc);
  int64_t period = video_period_us(h->video);
  if (period <= 0) period = 16667;
  pc.budget_us = period / 3;
  int64_t sample = plat_now_us();
  int frames = 0;
  while (!*quit && !h->quit && (!max_frames || frames < max_frames)) {
    bool missed = false;
    if (video_pending(h->video)) {
      int64_t vb = 0;
      if (video_wait(h->video, 100, &vb) == 0) {
        video_stats_t vs;
        video_get_stats(h->video, &vs);
        missed = vs.missed != h->video_missed_seen;
        h->video_missed_seen = vs.missed;
        host_note_shown(h, vb, sample, period);
      }
    }
    period = video_period_us(h->video);
    if (period <= 0) period = 16667;
    /* the video module recovers lost displays itself and keeps a software clock meanwhile;
     * the deadline is the latest time to queue a frame for the coming refresh */
    int64_t now = plat_now_us();
    int64_t start = video_deadline_us(h->video, now) - pc.budget_us;
    if (start > now && start - now < 2 * period) plat_sleep_until_us(start);
    sample = plat_now_us();
    host_frame(h, sample);
    host_present(h);
    int64_t done = plat_now_us();
    pacer_note(&pc, done - sample, period, missed);
    host_publish(h, done);
    if (h->etx) etx_gc_step(h->etx, 2);
    frames++;
  }
  return 0;
}

int main(int argc, char **argv) {
  args_t a;
  if (parse(&a, argc, argv) != 0) return 2;
  static app_t app;
  app_init(&app);
  set_paths(&app, &a);
  plat_net_init();
  plat_mkdirs(app.paths.state);

  if (a.scenario) return scenario_run(&app, &a.h, a.scenario);
  if (a.vtx_test) return vtx_line_test(a.vtx_test, a.vtx_test_proto, a.vtx_test_frames > 0 ? a.vtx_test_frames : 10, stdout);
  if (a.vtx_loop_tx) return vtx_loopback_test(a.vtx_loop_tx, a.vtx_loop_rx, a.vtx_test_frames > 0 ? a.vtx_test_frames : 50, stdout);

  char msg[256] = "";
  sys_realtime(a.h.rt_prio, a.h.rt_cpu, msg, sizeof msg);
  if (msg[0]) app_log(&app, "realtime: %s", msg);

  /* a real display: take the console (tty1) out of text mode before the video opens */
  const char *vb = a.h.video_backend;
  if (!vb || (strcmp(vb, "png") && strcmp(vb, "none"))) sys_console_graphics(NULL);

  static host_t h;
  if (host_init(&h, &app, &a.h) != 0) {
    app_log(&app, "fatal: %s", h.err);
    return 1;
  }
  app_log(&app, "sticktime-pi %s: %s, render %dx%d", app.version, h.vinfo.desc, h.vinfo.render_w, h.vinfo.render_h);
  if (a.port > 0) {
    static char hostname[64];
    plat_strlcpy(hostname, settings_get(h.set, "hostname"), sizeof hostname);   /* settings reloads free it */
    web_config_t wc;
    memset(&wc, 0, sizeof wc);
    wc.port = (uint16_t)a.port;
    wc.max_clients = 16;
    wc.hostname = hostname;
    wc.max_upload = 64u << 20;
    if (web_start(&app, &wc) != 0) app_log(&app, "web: can't listen on port %d", a.port);
  }
  web_sync_system(&app);
  device_loop(&h, a.frames);
  app_log(&app, "stopping");
  web_stop();
  sys_console_restore();   /* console first, then video (fbcon takes the display back while we are DRM master) */
  host_close(&h);
  return 0;
}
