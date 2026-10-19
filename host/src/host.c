/* The host: one frame of StickTime Pi. See host.h. */
#include "host.h"
#include "audio.h"
#include "audio_internal.h"
#include "sys.h"
#include "vtx.h"
#include "web.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RELOAD_DEBOUNCE_US 300000
#define ERROR_RETRY_US 10000000
#define LONG_SESSION_TICKS 4194304.0     /* 2^22 ticks: float32 R.pt loses resolution beyond (game-api §6.3) */
#define IP_CHECK_US 2000000
#define IP_SHOW_US 60000000              /* show_ip = auto: for a minute after the address appears or changes */
#define PUBLISH_US 250000

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ------------------------------------------------------------------ settings -> configs */
void host_audio_offline(host_t *h, double dt_ms) {
  if (!h->audio || !h->audio_offline) return;
  h->audio_frac += dt_ms * 48.0;
  int n = (int)h->audio_frac;
  h->audio_frac -= n;
  if (n > 0) audio_render_offline(h->audio, n);
}

static void read_mix(host_t *h) {
  mix_config_t *m = &h->mix;
  memset(m, 0, sizeof *m);
  m->fixed = mix_parse_input(settings_get(h->set, "input"));
  if (!mix_parse_order(settings_get(h->set, "channel_order"), m->order)) memcpy(m->order, "AETR", 5);
  mix_parse_channels(settings_get(h->set, "stick_channels"), m->chan);     /* empty or invalid: all 0, the order */
  mix_parse_invert(settings_get(h->set, "invert"), m->invert);
  m->pause_ch = settings_get_int(h->set, "pause_channel", 5, 0, 16);
  nav_default_config(&h->navc);
  h->navc.enabled = settings_get_bool(h->set, "stick_menus", true);
  if (h->evdev) evdev_set_stick_mode(h->evdev, settings_get_int(h->set, "stick_mode", 2, 1, 4));
}

static void read_audio_levels(host_t *h) {
  if (!h->audio) return;
  audio_levels_t lv;
  lv.sound = settings_get_bool(h->set, "sound", true);
  lv.volume = settings_get_int(h->set, "volume", 70, 0, 100);
  lv.music = settings_get_bool(h->set, "music", false);
  lv.music_volume = settings_get_int(h->set, "music_volume", 60, 0, 100);
  lv.music_source = strcmp(settings_get(h->set, "music_source"), "songs") == 0 ? AUDIO_MUSIC_SONGS : AUDIO_MUSIC_GENERATED;
  lv.mono = settings_get_bool(h->set, "audio_mono", false);
  audio_set_levels(h->audio, &lv);
}

static void read_wifi_config(host_t *h, wifi_config_t *wc) {
  memset(wc, 0, sizeof *wc);
  wc->port = 11000;
  wc->http_port = 80;
  plat_strlcpy(wc->module_ip, settings_get(h->set, "wifi_module_ip"), sizeof wc->module_ip);
  wc->interval_us = settings_get_int(h->set, "wifi_interval_us", 4000, 1000, 100000);
  wc->channels = 16;
}

/* STICKTIME_LAT: "0" = off (the emulator's PC mode), a number = seconds, "auto" = measured
 * sample-to-scanout time, "game" = undefined (the game predicts 1.1 frames itself) */
static void apply_latency(host_t *h, double measured_s) {
  if (!h->etx || !etx_loaded(h->etx)) return;
  const char *v = settings_get(h->set, "latency");
  if (!strcmp(v, "game")) etx_clear_global(h->etx, "STICKTIME_LAT");
  else if (!strcmp(v, "auto")) etx_set_global_number(h->etx, "STICKTIME_LAT", measured_s > 0 ? clampd(measured_s, 0, 0.2) : 0.0);
  else etx_set_global_number(h->etx, "STICKTIME_LAT", clampd(atof(v), 0, 0.2));
}

/* ------------------------------------------------------------------ text helpers for host screens */
static void text(host_t *h, int x, int y, const char *s, uint32_t flags) {
  lcd_draw_text(&h->lcd, x, y, s, (int)strlen(s), flags, NULL, NULL);
}

/* draw s word-wrapped into width w from y; returns the y after the last line */
static int text_wrapped(host_t *h, int x, int y, int w, const char *s, uint32_t flags, int max_lines) {
  char line[256];
  int lines = 0;
  while (*s && lines < max_lines) {
    int n = 0, last_space = -1, tw, th = 0;
    /* longest prefix that fits, broken at a space when possible, always at '\n' */
    while (s[n] && s[n] != '\n' && n < (int)sizeof line - 1) {
      lcd_size_text(&h->lcd, s, n + 1, flags, &tw, &th);
      if (tw > w && n > 0) break;
      if (s[n] == ' ') last_space = n;
      n++;
    }
    int cut = n;
    if (s[n] && s[n] != '\n' && last_space > 0) cut = last_space;
    memcpy(line, s, (size_t)cut);
    line[cut] = 0;
    text(h, x, y, line, flags);
    lcd_size_text(&h->lcd, "Ag", 2, flags, &tw, &th);
    y += th;
    lines++;
    s += cut;
    while (*s == ' ') s++;
    if (*s == '\n') s++;
  }
  return y;
}

/* ------------------------------------------------------------------ the host's own screens */
static const uint32_t RGBF_WHITE = (0xFFFFu << 16) | LCDF_RGB;

static uint32_t rgbf(int r, int g, int b) { return ((uint32_t)lcd_rgb565(r, g, b) << 16) | LCDF_RGB; }

static void draw_error_screen(host_t *h, int64_t now_us) {
  lcd_t *l = &h->lcd;
  int W = l->w, H = l->h, mg = W / 32;
  lcd_clear(l, lcd_rgb565(12, 24, 48));
  const char *err = etx_error(h->etx);
  bool missing = strstr(err, "No such file") || strstr(err, "cannot open") || strstr(err, "not found");
  int y = mg;
  text(h, mg, y, missing ? "StickTime is not installed" : "The game stopped", 0x500u | RGBF_WHITE);   /* DBLSIZE */
  int tw, th;
  lcd_size_text(l, "Ag", 2, 0x500u, &tw, &th);
  y += th + mg / 2;
  if (missing) {
    y = text_wrapped(h, mg, y, W - 2 * mg,
                     "Copy StickTime.lua into sticktime/sd/SCRIPTS/TOOLS/ on the SD card, or install it from the "
                     "web page (Updates), then press ENTER.", RGBF_WHITE, 6);
  } else {
    y = text_wrapped(h, mg, y, W - 2 * mg, err, 0x300u | rgbf(255, 200, 120), (H - y) / 24 > 3 ? (H - y) / 24 - 3 : 3);
  }
  y += mg / 2;
  char buf[200];
  int64_t left = (h->err_retry_us - now_us) / 1000000;
  if (h->err_retry_us && left >= 0) snprintf(buf, sizeof buf, "ENTER or roll right: restart (again by itself in %d s)", (int)left + 1);
  else snprintf(buf, sizeof buf, "ENTER or roll right: restart");
  y = text_wrapped(h, mg, y, W - 2 * mg, buf, RGBF_WHITE, 2);
  if (h->ip[0]) {
    snprintf(buf, sizeof buf, "Web page: http://%s", h->ip);
    text_wrapped(h, mg, y, W - 2 * mg, buf, rgbf(140, 200, 255), 2);
  }
}

/* the web address at the bottom of the main menu (show_ip) */
static void draw_ip_overlay(host_t *h) {
  lcd_t *l = &h->lcd;
  char buf[160];
  bool hotspot = !strncmp(h->ip, "10.42.0.", 8);
  if (hotspot)
    snprintf(buf, sizeof buf, "WiFi %s  -  http://%s", settings_get(h->set, "hotspot_ssid"), h->ip);
  else
    snprintf(buf, sizeof buf, "http://%s  -  http://%s.local", h->ip, settings_get(h->set, "hostname"));
  int tw, th;
  lcd_size_text(l, buf, (int)strlen(buf), 0x300u, &tw, &th);   /* SMLSIZE */
  int y = l->h - th - 2;
  lcd_fill_rect(l, 0, y - 1, l->w, th + 3, 0, 7);
  text(h, l->w / 2, y, buf, 0x300u | LCDF_CENTER | RGBF_WHITE);
}

/* ------------------------------------------------------------------ game (re)loading */
/* "StickTime v1.7.2" (or "StickTime BW v..", "StickTime Lite v..") in the first 600 bytes */
static void read_game_version(host_t *h) {
  h->game_version[0] = 0;
  char path[1024];
  if (etx_sd_path(h->etx, h->script, path, sizeof path) != 0) return;
  size_t n = 0;
  char *s = plat_read_file(path, &n, 1 << 20);
  if (!s) return;
  if (n > 600) s[600] = 0;
  for (char *p = strstr(s, "StickTime"); p; p = strstr(p + 1, "StickTime")) {
    char *v = strstr(p, " v");
    if (!v || v - p > 16) continue;
    v += 2;
    int k = 0;
    while (k < (int)sizeof h->game_version - 1 && ((v[k] >= '0' && v[k] <= '9') || v[k] == '.')) {
      h->game_version[k] = v[k];
      k++;
    }
    h->game_version[k] = 0;
    if (k && h->game_version[k - 1] == '.') h->game_version[k - 1] = 0;
    if (k) break;
  }
  free(s);
}

/* STICKTIME_DT: the color game steps its simulation by it instead of the 10 ms getTime() ticks */
static void set_frame_dt(host_t *h) {
  if (h->frame_dt_us > 0) etx_set_global_number(h->etx, "STICKTIME_DT", (double)h->frame_dt_us / 1000.0);
}

int host_reload_game(host_t *h) {
  if (h->audio) audio_game_reset(h->audio);
  nav_reset(&h->nav);
  h->nav_pending = 0;
  h->game_state = GS_UNKNOWN;
  h->reload_at_us = 0;
  plat_strlcpy(h->script, settings_get(h->set, "script"), sizeof h->script);
  h->ecfg.script = h->script;
  h->ecfg.stick_mode = settings_get_int(h->set, "stick_mode", 2, 1, 4);
  /* STICKTIME_AV: composite now (a recovered display may be another connector) */
  const video_info_t *vi = video_get_info(h->video);
  h->ecfg.av = h->av_override ? h->av_override > 0 : vi ? vi->composite : h->vinfo.composite;
  etx_free(h->etx);
  h->etx = etx_new(&h->lcd, &h->ecfg);
  if (!h->etx) { snprintf(h->err, sizeof h->err, "out of memory"); return -1; }
  etx_set_clock_ms(h->etx, (h->now_us - h->clock0_us) / 1000);
  etx_set_sticks(h->etx, 0, 0, -1024, 0);
  int r = etx_load(h->etx);
  if (r != 0) {
    app_log(h->app, "game: %s", etx_error(h->etx));
    h->last_result = -1;
    return -1;
  }
  h->last_result = 0;
  apply_latency(h, h->lat_measured_s);
  set_frame_dt(h);                        /* for a run() before the next frame's (a restart) */
  read_game_version(h);
  app_log(h->app, "game: loaded %s %s", h->script, h->game_version);
  return 0;
}

static void note_error(host_t *h, int64_t now) {
  /* auto-restart after 10 s, unless it keeps failing (3 errors within a minute): then wait for ENTER */
  if (now - h->err_window_us > 60000000) { h->err_window_us = now; h->err_count = 0; }
  h->err_count++;
  h->err_retry_us = h->err_count <= 3 ? now + ERROR_RETRY_US : 0;
}

/* ------------------------------------------------------------------ init / close */
static void video_config_from_settings(host_t *h) {
  video_config_t *v = &h->vcfg;
  memset(v, 0, sizeof *v);
  v->backend = h->opt.video_backend ? h->opt.video_backend : "auto";
  v->output = settings_get(h->set, "video");
  v->tv = settings_get(h->set, "tv");
  v->aspect = settings_get(h->set, "aspect");
  v->av_mode = settings_get(h->set, "av_mode");
  v->av_detail = settings_get(h->set, "av_detail");
  v->av_flicker = settings_get(h->set, "av_flicker");
  v->hdmi_mode = settings_get(h->set, "hdmi_mode");
  v->margin_pct = settings_get_int(h->set, "margin", 0, 0, 20);
  v->render_w = h->opt.render_w;
  v->render_h = h->opt.render_h;
  v->png_dir = h->opt.png_dir;
  v->png_every = h->opt.png_every;
}

int host_init(host_t *h, app_t *app, const host_options_t *opt) {
  memset(h, 0, sizeof *h);
  h->app = app;
  h->opt = *opt;
  h->cur_src = -1;
  h->clock0_us = h->now_us = opt->start_us ? opt->start_us : plat_now_us();
  h->sticks.src = -1;
  h->sticks.thr = -1024;
  h->sticks.pause = INT32_MIN;

  /* settings: a missing file gets the documented template */
  bool exists = false;
  h->set = settings_load(app->paths.settings, &exists);
  if (!h->set) { snprintf(h->err, sizeof h->err, "out of memory"); return -1; }
  if (!exists) {
    if (plat_write_file_atomic(app->paths.settings, SETTINGS_TEMPLATE, strlen(SETTINGS_TEMPLATE)) == 0)
      app_log(app, "settings: wrote a fresh %s", app->paths.settings);
    else
      app_log(app, "settings: %s missing and can't be written (defaults in use)", app->paths.settings);
  }
  if (settings_warnings(h->set)[0]) app_log(app, "settings: %s", settings_warnings(h->set));

  /* video first: the render size decides LCD_W/LCD_H and the fonts */
  video_config_from_settings(h);
  char err[256] = "";
  h->video = video_open(&h->vcfg, &h->vinfo, err, sizeof err);
  if (!h->video) {
    app_log(app, "video: %s; running without a display", err);
    video_config_t none = h->vcfg;
    none.backend = "none";
    h->video = video_open(&none, &h->vinfo, err, sizeof err);
    if (!h->video) { snprintf(h->err, sizeof h->err, "video: %s", err); return -1; }
  }
  app_log(app, "video: %s", h->vinfo.desc);

  if (opt->stub_fonts) h->fonts = fonts_open_stub(opt->stub_adv > 0 ? opt->stub_adv : 8.0f);
  else h->fonts = fonts_open(app->paths.fonts, err, sizeof err);
  if (!h->fonts) { snprintf(h->err, sizeof h->err, "fonts: %s", err); return -1; }
  font_class_for(&h->fcls, h->vinfo.render_w, h->vinfo.render_h, settings_get(h->set, "fonts"));
  if (lcd_init(&h->lcd, h->vinfo.render_w, h->vinfo.render_h, h->fonts, &h->fcls) != 0) {
    snprintf(h->err, sizeof h->err, "out of memory for a %dx%d screen", h->vinfo.render_w, h->vinfo.render_h);
    return -1;
  }
  fonts_warm(h->fonts, &h->fcls);

  /* inputs */
  read_mix(h);
  nav_init(&h->nav);
  h->src[SLOT_KEYBOARD].kind = INSRC_KEYBOARD;
  h->src[SLOT_KEYBOARD].fixed_aetr = true;
  h->src[SLOT_KEYBOARD].nch = 4;
  h->src[SLOT_KEYBOARD].ch[2] = -1024;
  h->nsrc = INPUT_MAX_SOURCES;
  if (!opt->no_inputs) {
    h->evdev = evdev_open(NULL);
    if (h->evdev) evdev_set_stick_mode(h->evdev, settings_get_int(h->set, "stick_mode", 2, 1, 4));
    wifi_config_t wc;
    read_wifi_config(h, &wc);
    h->wifi = wifi_open(app, &wc);
    if (h->wifi) wifi_start(h->wifi, plat_now_us());
  }
  /* VTX control: its own thread reads the settings and the VTX table (off unless vtx is set) */
  if (!opt->no_vtx) {
    vtx_options_t vo = { opt->vtx_transport };
    h->vtx = vtx_open(app, &vo);
    app->vtx = h->vtx;
  }

  /* sound */
  if (!opt->no_audio) {
    audio_config_t ac;
    memset(&ac, 0, sizeof ac);
    ac.out = opt->audio_out ? opt->audio_out : settings_get(h->set, "audio_out");
    ac.video_hdmi = !strcmp(settings_get(h->set, "video"), "hdmi");
    ac.rate = 48000;
    char music[600];
    snprintf(music, sizeof music, "%s/music", app->paths.base);
    ac.music_dir = music;
    ac.seed = opt->audio_seed;
    h->audio = audio_open(&ac, err, sizeof err);
    if (err[0]) app_log(app, "audio: %s", err);
    h->audio_offline = h->audio && (!strncmp(ac.out, "wav:", 4) || !strcmp(ac.out, "null"));
    read_audio_levels(h);
  }

  /* the game */
  h->ecfg.sd_root = app->paths.sd;
  h->ecfg.test_hooks = true;
  h->ecfg.lat = -1;
  h->ecfg.async_writes = opt->async_writes;
  if (host_reload_game(h) != 0) note_error(h, h->now_us);
  h->hs.window_us = h->now_us;
  return 0;
}

void host_close(host_t *h) {
  if (h->etx) { etx_flush_writes(h->etx); etx_free(h->etx); }
  if (h->audio) audio_close(h->audio);
  if (h->wifi) wifi_close(h->wifi);
  if (h->vtx) {
    h->app->vtx = NULL;                   /* (the web server has stopped: main.c) */
    vtx_close(h->vtx);
  }
  if (h->evdev) evdev_close(h->evdev);
  if (h->video) video_close(h->video);
  lcd_free(&h->lcd);
  if (h->fonts) fonts_close(h->fonts);
  if (h->set) settings_free(h->set);
  memset(h, 0, sizeof *h);
}

void host_apply_settings(host_t *h) {
  bool exists;
  settings_t *s = settings_load(h->app->paths.settings, &exists);
  if (!s) return;
  char old_script[256];
  plat_strlcpy(old_script, settings_get(h->set, "script"), sizeof old_script);
  int old_mode = settings_get_int(h->set, "stick_mode", 2, 1, 4);
  settings_free(h->set);
  h->set = s;
  read_mix(h);
  read_audio_levels(h);
  if (h->wifi) {
    wifi_config_t wc;
    read_wifi_config(h, &wc);
    wifi_configure(h->wifi, &wc, h->now_us);
  }
  vtx_reload(h->vtx);                     /* never blocks: the VTX thread re-reads the files */
  apply_latency(h, h->lat_measured_s);
  if (strcmp(old_script, settings_get(h->set, "script")) || old_mode != settings_get_int(h->set, "stick_mode", 2, 1, 4))
    h->reload_at_us = h->now_us;
  app_log(h->app, "settings: applied");
}

void host_queue_key(host_t *h, int evt) {
  if (evt && h->nkeys < (int)(sizeof h->keyq / sizeof h->keyq[0])) h->keyq[h->nkeys++] = evt;
}

/* ------------------------------------------------------------------ the frame */
static int take_key(host_t *h) {
  if (!h->nkeys) return 0;
  int k = h->keyq[0];
  memmove(h->keyq, h->keyq + 1, sizeof(int) * (size_t)(h->nkeys - 1));
  h->nkeys--;
  return k;
}

/* map the special codes of the keyboard / web remote to events */
static int resolve_key(host_t *h, int k) {
  if (k == HOST_KEY_UP) return nav_pitch_event(&h->nav, -1);
  if (k == HOST_KEY_DOWN) return nav_pitch_event(&h->nav, +1);
  return k;
}

static void poll_inputs(host_t *h, int64_t now) {
  bool flying = h->game_state == GS_FLY || h->game_state == GS_READY || h->game_state == GS_COUNT ||
                h->game_state == GS_CRASHED;
  if (h->evdev) {
    int keys[16];
    int n = evdev_poll(h->evdev, h->src, now, flying, keys, 16);
    for (int i = 0; i < n; i++) {
      if (keys[i] == EVDEV_KEY_RELOAD) h->reload_at_us = now;
      else if (keys[i] == EVDEV_KEY_UP) host_queue_key(h, HOST_KEY_UP);
      else if (keys[i] == EVDEV_KEY_DOWN) host_queue_key(h, HOST_KEY_DOWN);
      else host_queue_key(h, keys[i]);
    }
  }
  if (h->wifi) wifi_poll(h->wifi, now, &h->src[SLOT_WIFI]);
}

static void check_ip(host_t *h, int64_t now) {
  if (now - h->ip_checked_us < IP_CHECK_US) return;
  h->ip_checked_us = now;
  char ip[64], iface[32];
  sys_ip_address(ip, sizeof ip, iface, sizeof iface);
  if (strcmp(ip, h->ip)) {
    plat_strlcpy(h->ip, ip, sizeof h->ip);
    h->ip_since_us = now;
    if (ip[0]) app_log(h->app, "network: %s on %s", ip, iface);
  }
  const char *m = settings_get(h->set, "show_ip");
  h->show_ip = h->ip[0] && (!strcmp(m, "on") || (!strcmp(m, "auto") && now - h->ip_since_us < IP_SHOW_US));
}

void host_frame(host_t *h, int64_t now) {
  h->now_us = now;
  /* commands and keys from the web thread */
  int keys[APP_KEYQ], nk = 0;
  uint32_t cmds = app_take(h->app, keys, &nk);
  if (cmds & APP_CMD_QUIT) h->quit = true;
  for (int i = 0; i < nk; i++) host_queue_key(h, keys[i]);
  if (cmds & APP_CMD_RELOAD_SETTINGS) host_apply_settings(h);
  if (cmds & APP_CMD_RELOAD_GAME) h->reload_at_us = now;
  if (cmds & APP_CMD_SCREENSHOT) h->want_shot = true;
  if (web_take_sd_dirty()) h->reload_at_us = now + RELOAD_DEBOUNCE_US;
  if (cmds & APP_CMD_MUSIC_NEXT && h->audio) audio_music_next(h->audio);
  if (cmds & APP_CMD_SONGS_CHANGED && h->audio) audio_songs_changed(h->audio);

  if (!h->opt.no_inputs) poll_inputs(h, now);
  check_ip(h, now);

  /* sticks */
  mix_sticks(&h->mix, h->src, h->nsrc, h->cur_src, now, &h->sticks);
  h->cur_src = h->sticks.src;
  if (h->stick_override) {
    h->sticks.ail = h->ov[0]; h->sticks.ele = h->ov[1]; h->sticks.thr = h->ov[2]; h->sticks.rud = h->ov[3];
    h->sticks.live = true;
  }

  bool running = h->etx && etx_loaded(h->etx) && h->last_result == 0;

  /* a pending reload happens in the main menu (not while a value is edited), or at once when the
   * game isn't running */
  if (h->reload_at_us && now >= h->reload_at_us &&
      (!running || h->game_state == GS_MENU || h->game_state == GS_UNKNOWN) && !h->nav.editing) {
    if (host_reload_game(h) != 0) note_error(h, now);
    running = h->last_result == 0;
  }

  /* one event per run(): queued keys first, then the sticks */
  int nav_state = running ? h->game_state : GS_PAUSED;    /* error screen: roll right = ENTER */
  int ev_nav = nav_update(&h->nav, &h->navc, nav_state, &h->sticks, now);
  if (h->nav_pending && (h->nav_pending_state != nav_state || h->nav_pending_editing != h->nav.editing))
    h->nav_pending = 0;
  int ev = resolve_key(h, take_key(h));
  if (!ev && h->nav_pending) {
    ev = h->nav_pending;
    h->nav_pending = 0;
  }
  if (!ev) ev = ev_nav;
  else if (ev_nav) {
    h->nav_pending = ev_nav;
    h->nav_pending_state = nav_state;
    h->nav_pending_editing = h->nav.editing;
  }

  if (!running) {
    if (ev == ETX_EVT_ENTER || (h->err_retry_us && now >= h->err_retry_us)) {
      if (host_reload_game(h) != 0) note_error(h, now);
    }
    if (h->last_result != 0 || !etx_loaded(h->etx)) {
      draw_error_screen(h, now);
      h->frames_drawn++;
      return;
    }
    ev = 0;
  }

  /* the game */
  etx_set_clock_ms(h->etx, (now - h->clock0_us) / 1000);
  etx_set_sticks(h->etx, h->sticks.ail, h->sticks.ele, h->sticks.thr, h->sticks.rud);
  set_frame_dt(h);
  if (settings_get_bool(h->set, "stats", false)) {
    etx_set_global_number(h->etx, "TOOL_RUN_US", (double)h->last_run_us);
    etx_set_global_number(h->etx, "TOOL_SHOW_US", (double)h->last_show_us);
  }
  memset(&h->lcd.st, 0, sizeof h->lcd.st);
  int64_t t0 = plat_now_us();
  int r = etx_run(h->etx, ev);
  int64_t t1 = plat_now_us();
  h->last_run_us = t1 - t0;
  h->hs.run_ms[h->hs.run_n++ % 64] = (float)(h->last_run_us / 1000.0);
  h->hs.run_sum += h->last_run_us / 1000.0;

  if (r == 1) {                           /* the script closed itself (Exit, long EXIT): start it again */
    app_log(h->app, "game: closed, starting it again");
    if (host_reload_game(h) != 0) note_error(h, now);
    else etx_run(h->etx, 0);              /* draw its first frame now */
    h->frames_drawn++;
    return;
  }
  if (r < 0) {
    app_log(h->app, "game: %s", etx_error(h->etx));
    h->last_result = -1;
    note_error(h, now);
    draw_error_screen(h, now);
    h->frames_drawn++;
    return;
  }
  h->err_count = h->err_count > 0 && now - h->err_window_us > 60000000 ? 0 : h->err_count;

  int st = etx_game_state(h->etx);
  nav_after_run(&h->nav, ev, st);
  if (st != h->game_state) {
    h->game_state = st;
    if (st == GS_MENU && h->long_session) h->reload_at_us = now;
  }

  /* sound: the motor synth follows STICKTIME_TEST.audio(); the game's beeps */
  if (h->audio) {
    double vals[16];
    int n = etx_test_call(h->etx, "audio", NULL, 0, vals, 16);
    double dt_ms = h->last_frame_us ? (now - h->last_frame_us) / 1000.0 : 16.7;
    if (n > 0) audio_game(h->audio, vals, n, clampd(dt_ms, 0, 100));
    etx_tone_t tones[8];
    int nt = etx_take_tones(h->etx, tones, 8);
    for (int i = 0; i < nt; i++) {
      if (tones[i].haptic) audio_haptic(h->audio, tones[i].len_ms);
      else audio_tone(h->audio, tones[i].freq, tones[i].len_ms, tones[i].pause_ms, tones[i].flags, tones[i].freq_incr);
    }
  } else {
    etx_tone_t tones[8];
    etx_take_tones(h->etx, tones, 8);
  }
  h->last_frame_us = now;

  /* a Lua state left running for many hours: start it again at the next main menu */
  if (now - h->long_check_us > 10000000) {
    h->long_check_us = now;
    double g[20];
    if (etx_test_call(h->etx, "get", NULL, 0, g, 20) >= 20 && g[19] > LONG_SESSION_TICKS) h->long_session = true;
  }

  if (h->show_ip && h->game_state == GS_MENU) draw_ip_overlay(h);
  h->frames_drawn++;
}

int host_present(host_t *h) {
  if (h->want_shot) {
    /* hand a copy of this frame to the web thread */
    plat_mutex_lock(&h->app->mu);
    size_t n = (size_t)h->lcd.w * (size_t)h->lcd.h;
    if (!h->app->shot || h->app->shot_w != h->lcd.w || h->app->shot_h != h->lcd.h) {
      free(h->app->shot);
      h->app->shot = malloc(n * sizeof(uint16_t));
    }
    if (h->app->shot) {
      memcpy(h->app->shot, h->lcd.fb, n * sizeof(uint16_t));
      h->app->shot_w = h->lcd.w;
      h->app->shot_h = h->lcd.h;
      h->app->shot_ready = true;
    }
    plat_mutex_unlock(&h->app->mu);
    h->want_shot = false;
  }
  int64_t t0 = plat_now_us();
  h->present_start_us = t0;
  int r = video_present(h->video, h->lcd.fb);
  h->last_present_us = plat_now_us() - t0;
  h->hs.present_sum += h->last_present_us / 1000.0;
  return r;
}

void host_note_shown(host_t *h, int64_t vblank_us, int64_t sample_us, int64_t period_us) {
  host_stats_t *s = &h->hs;
  if (h->last_flip_us) {
    double f = (vblank_us - h->last_flip_us) / 1000.0;
    s->frame_sum += f;
    if (f > s->frame_max) s->frame_max = (float)f;
    if (period_us > 0 && vblank_us - h->last_flip_us > period_us * 3 / 2) s->missed++;
  }
  /* TOOL_SHOW_US: from the start of the copy until the frame is on screen */
  if (h->present_start_us && vblank_us > h->present_start_us) h->last_show_us = vblank_us - h->present_start_us;
  /* sample -> the middle of the scanout of this frame */
  double lat = (vblank_us - sample_us + period_us / 2) / 1000.0;
  if (lat > 0 && lat < 200) {
    s->lat_sum += lat;
    h->lat_measured_s = h->lat_measured_s * 0.95 + lat / 1000.0 * 0.05;
  }
  s->count++;
  s->frames++;
  h->last_flip_us = vblank_us;
}

static int cmpf(const void *a, const void *b) {
  float x = *(const float *)a, y = *(const float *)b;
  return x < y ? -1 : x > y;
}

void host_publish(host_t *h, int64_t now) {
  if (now - h->last_publish_us < PUBLISH_US) return;
  host_stats_t *s = &h->hs;
  app_status_t st;
  memset(&st, 0, sizeof st);
  double win_s = (now - s->window_us) / 1e6;
  int frames = h->frames_drawn - h->frames_at_window;
  st.uptime_ms = (now - h->app->start_us) / 1000;
  st.fps = win_s > 0 ? (float)(frames / win_s) : 0;
  st.frame_ms_avg = s->count ? (float)(s->frame_sum / s->count) : 0;
  st.frame_ms_max = s->frame_max;
  int nr = s->run_n < 64 ? s->run_n : 64;
  if (nr) {
    float tmp[64];
    memcpy(tmp, s->run_ms, sizeof(float) * (size_t)nr);
    qsort(tmp, (size_t)nr, sizeof(float), cmpf);
    st.run_ms_p95 = tmp[(nr * 95) / 100 < nr ? (nr * 95) / 100 : nr - 1];
  }
  st.run_ms_avg = frames > 0 ? (float)(s->run_sum / frames) : 0;
  st.present_ms_avg = frames > 0 ? (float)(s->present_sum / frames) : 0;
  st.latency_ms = s->count ? (float)(s->lat_sum / s->count) : 0;
  st.frames = s->frames;
  st.missed = s->missed;
  st.game_state = h->game_state;
  st.game_loaded = h->etx && etx_loaded(h->etx) && h->last_result == 0;
  plat_strlcpy(st.game_error, h->etx ? etx_error(h->etx) : h->err, sizeof st.game_error);
  if (h->etx) {
    etx_stats_t es;
    etx_stats(h->etx, &es);
    st.lua_kb = es.lua_kb;
  }
  plat_strlcpy(st.video_desc, h->vinfo.desc, sizeof st.video_desc);
  st.render_w = h->vinfo.render_w;
  st.render_h = h->vinfo.render_h;
  st.sticks = h->sticks;
  int n = 0;
  for (int i = 0; i < h->nsrc && n < INPUT_MAX_SOURCES; i++) {
    const insrc_t *src = &h->src[i];
    if (!src->present && !(i == SLOT_KEYBOARD && src->updates)) continue;
    app_source_status_t *o = &st.src[n++];
    o->kind = src->kind;
    plat_strlcpy(o->name, src->name[0] ? src->name : insrc_kind_name(src->kind), sizeof o->name);
    o->live = now - src->last_us < INPUT_STALE_US;
    uint32_t du = src->updates - h->src_updates_prev[i];
    o->rate_hz = win_s > 0 ? (float)(du / win_s) : 0;
    h->src_updates_prev[i] = src->updates;
    o->nch = src->nch;
    memcpy(o->ch, src->ch, sizeof o->ch);
  }
  st.nsrc = n;
  if (h->wifi) wifi_fill_status(h->wifi, &st);
  if (h->audio) {
    audio_status_t as;
    audio_status(h->audio, &as);
    st.audio_ok = as.ok;
    plat_strlcpy(st.audio_device, as.device, sizeof st.audio_device);
    plat_strlcpy(st.now_playing, as.now_playing, sizeof st.now_playing);
  }
  plat_strlcpy(st.script, h->script, sizeof st.script);
  /* game version from the script header ("StickTime v1.7.2") */
  plat_strlcpy(st.game_version, h->game_version, sizeof st.game_version);
  app_publish(h->app, &st);

  /* new stats window */
  memset(s->run_ms, 0, sizeof s->run_ms);
  s->run_n = 0;
  s->run_sum = s->present_sum = s->frame_sum = s->lat_sum = 0;
  s->count = 0;
  s->frame_max = 0;
  s->window_us = now;
  h->frames_at_window = h->frames_drawn;
  h->last_publish_us = now;
  if (!strcmp(settings_get(h->set, "latency"), "auto")) apply_latency(h, h->lat_measured_s);
}
