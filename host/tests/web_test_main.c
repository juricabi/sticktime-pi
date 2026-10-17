/* A test server for host/tests/web_api_test.py: the real web server (web_start) on a base folder
 * the test prepares, with a fake frame loop in place of the game.
 *
 *   web_test_main <base> <port> [hostname] [max_upload]
 *
 * Paths: settings <base>/sticktime.txt, sd <base>/sd, web <base>/web, libexec <base>/libexec (fake
 * helpers), state <base>/state. The fake frame loop publishes a status every 10 ms, answers
 * screenshot requests with a 64x48 test pattern (unless <state>/no_shot exists) and appends what it
 * receives to <state>/events.log: "cmd <bits>", "key <evt>", "sd_dirty". Prints "READY" once it
 * listens on 127.0.0.1 (STP_WEB_BIND overrides) and runs until it is killed (or <state>/stop
 * appears). Without arguments it exits 0, so "build.sh test" can run it like the unit tests.
 * STP_VTX_PORT=tcp:127.0.0.1:PORT starts VTX control on that line (a fake VTX); without it /api/vtx
 * says "off". */
#include "app.h"
#include "etx.h"
#include "vtx.h"
#include "web.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char events[600];

static void ev(const char *fmt, ...) {
  FILE *f = fopen(events, "ab");
  if (!f) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fclose(f);
}

static bool exists(const char *dir, const char *name) {
  char p[700];
  plat_stat_t st;
  snprintf(p, sizeof p, "%s/%s", dir, name);
  return plat_stat(p, &st) == 0 && st.exists;
}

static void fake_status(app_status_t *st, uint32_t frame, int64_t up_ms) {
  memset(st, 0, sizeof *st);
  st->uptime_ms = up_ms;
  st->fps = 59.9f;
  st->frame_ms_avg = 16.7f;
  st->frame_ms_max = 17.2f;
  st->run_ms_avg = 3.1f;
  st->run_ms_p95 = 4.0f;
  st->present_ms_avg = 0.6f;
  st->latency_ms = 9.8f;
  st->frames = frame;
  st->missed = 3;
  st->game_state = GS_MENU;
  st->game_loaded = true;
  plat_strlcpy(st->game_error, "boom: \"x\" <b>\n\tline 2 \xC3\xA9", sizeof st->game_error);
  plat_strlcpy(st->game_version, "1.7.2", sizeof st->game_version);
  st->lua_kb = 812;
  plat_strlcpy(st->video_desc, "Composite-1 720x480i 59.94 Hz, NTSC 4:3, render 640x480", sizeof st->video_desc);
  st->render_w = 640;
  st->render_h = 480;
  st->sticks.ail = 100;
  st->sticks.ele = -200;
  st->sticks.thr = -1024;
  st->sticks.rud = 0;
  st->sticks.pause = (frame & 1) ? 1024 : INT32_MIN;
  st->sticks.src = 0;
  st->sticks.kind = INSRC_WIFI;
  st->sticks.live = true;
  st->nsrc = 2;
  st->src[0].kind = INSRC_WIFI;
  plat_strlcpy(st->src[0].name, "ELRS 10.0.0.1", sizeof st->src[0].name);
  st->src[0].live = true;
  st->src[0].rate_hz = 250.0f;
  st->src[0].nch = 16;
  for (int i = 0; i < INPUT_CH; i++) st->src[0].ch[i] = (int16_t)(i * 100 - 800 + (int)(frame % 10));
  st->src[1].kind = INSRC_KEYBOARD;
  plat_strlcpy(st->src[1].name, "Keyboard", sizeof st->src[1].name);
  st->src[1].nch = 4;
  plat_strlcpy(st->wifi_module, "ELRS 10.0.0.1", sizeof st->wifi_module);
  plat_strlcpy(st->wifi_state, "streaming", sizeof st->wifi_state);
  st->wifi_rate_hz = 250.0f;
  st->audio_ok = true;
  plat_strlcpy(st->audio_device, "jack", sizeof st->audio_device);
  plat_strlcpy(st->now_playing, "Song 3 (generated)", sizeof st->now_playing);
  plat_strlcpy(st->script, "/SCRIPTS/TOOLS/StickTime.lua", sizeof st->script);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    printf("usage: web_test_main <base> <port> [hostname] [max_upload] (run by host/tests/web_api_test.py)\n");
    return 0;
  }
  static app_t app;
  app_init(&app);
  const char *base = argv[1];
  snprintf(app.paths.base, sizeof app.paths.base, "%s", base);
  snprintf(app.paths.settings, sizeof app.paths.settings, "%s/sticktime.txt", base);
  snprintf(app.paths.sd, sizeof app.paths.sd, "%s/sd", base);
  snprintf(app.paths.web, sizeof app.paths.web, "%s/web", base);
  snprintf(app.paths.libexec, sizeof app.paths.libexec, "%s/libexec", base);
  snprintf(app.paths.state, sizeof app.paths.state, "%s/state", base);
  snprintf(app.paths.script, sizeof app.paths.script, "/SCRIPTS/TOOLS/StickTime.lua");
  plat_mkdirs(app.paths.state);
  snprintf(events, sizeof events, "%s/events.log", app.paths.state);

  vtx_t *vtx = NULL;
  if (getenv("STP_VTX_PORT") && *getenv("STP_VTX_PORT")) {
    vtx_options_t vo = { getenv("STP_VTX_PORT") };
    vtx = vtx_open(&app, &vo);
    app.vtx = vtx;
  }

  web_config_t wc;
  memset(&wc, 0, sizeof wc);
  /* loopback unless asked: a test server has no business on the LAN (and no firewall prompts) */
  wc.bind_addr = getenv("STP_WEB_BIND") ? getenv("STP_WEB_BIND") : "127.0.0.1";
  wc.port = (uint16_t)atoi(argv[2]);
  wc.max_clients = 16;
  wc.hostname = argc > 3 ? argv[3] : "sticktime";
  wc.max_upload = argc > 4 ? (size_t)strtoull(argv[4], NULL, 10) : 0;
  if (web_start(&app, &wc) != 0) {
    fprintf(stderr, "web_test_main: can't listen on port %s\n", argv[2]);
    return 1;
  }
  printf("READY\n");
  fflush(stdout);

  int64_t t0 = plat_now_us();
  for (uint32_t frame = 0;; frame++) {
    app_status_t st;
    fake_status(&st, frame, (plat_now_us() - t0) / 1000);
    app_publish(&app, &st);
    int keys[APP_KEYQ], nk = 0;
    uint32_t cmds = app_take(&app, keys, &nk);
    if (cmds) ev("cmd %u", (unsigned)cmds);
    if (cmds & APP_CMD_RELOAD_SETTINGS) vtx_reload(vtx);     /* like host_apply_settings */
    for (int i = 0; i < nk; i++) ev("key %d", keys[i]);
    if ((cmds & APP_CMD_SCREENSHOT) && !exists(app.paths.state, "no_shot")) {
      const int w = 64, h = 48;
      plat_mutex_lock(&app.mu);
      if (!app.shot) app.shot = malloc(sizeof(uint16_t) * (size_t)(w * h));
      if (app.shot) {
        for (int y = 0; y < h; y++)
          for (int x = 0; x < w; x++) app.shot[y * w + x] = (uint16_t)(((x * 31 / w) << 11) | ((y * 63 / h) << 5) | 0x1F);
        app.shot_w = w;
        app.shot_h = h;
        app.shot_ready = true;
      }
      plat_mutex_unlock(&app.mu);
    }
    if (web_take_sd_dirty()) ev("sd_dirty");
    if (frame % 50 == 0 && exists(app.paths.state, "stop")) break;
    plat_sleep_us(10000);
  }
  web_stop();
  app.vtx = NULL;
  vtx_close(vtx);
  return 0;
}
