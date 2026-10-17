/* The host: everything one frame needs (settings, LCD, the game, video, inputs, menus by
 * sticks), shared by the device loop (main.c, real time, page flips) and the headless
 * scenarios (scenario.c, virtual time, PNG frames). */
#ifndef HOST_H
#define HOST_H

#include <stdint.h>
#include <stdbool.h>
#include "app.h"
#include "etx.h"
#include "input.h"
#include "lcd.h"
#include "nav.h"
#include "settings.h"
#include "video.h"
#include "input_wifi.h"
#include "input_evdev.h"

typedef struct {
  const char *video_backend;   /* overrides: NULL = from settings ("auto") */
  int render_w, render_h;      /* png/none backends */
  const char *png_dir;
  int png_every;
  bool no_inputs;              /* don't open evdev / WiFi (scenarios, tests) */
  bool no_web;
  bool test_hooks;             /* STICKTIME_TEST = {} (always on: the menus by sticks need the state) */
  bool stub_fonts;             /* fonts that measure but draw nothing (engine.js comparisons) */
  float stub_adv;
  int rt_prio, rt_cpu;         /* device: SCHED_FIFO priority, CPU to pin the frame loop to */
  bool no_audio;
  const char *audio_out;       /* override of the audio_out setting ("null", "wav:<path>" in tests) */
  uint32_t audio_seed;
  bool async_writes;           /* game saves on a writer thread (device) */
  int64_t start_us;            /* virtual start time (scenarios); 0 = plat_now_us() */
  bool no_vtx;                 /* no VTX control thread (scenarios) */
  const char *vtx_transport;   /* --vtx-port: overrides the vtx_port setting (tcp:HOST:PORT, tty:PATH, uartN) */
} host_options_t;

/* special key codes in the key queue (keyboard Up/Down, web remote up/down): mapped to PREV/NEXT
 * or INC/DEC by whether a value is being edited (nav_pitch_event) */
#define HOST_KEY_UP   (-101)
#define HOST_KEY_DOWN (-102)

typedef struct {
  float run_ms[64];            /* ring of recent run() times */
  int run_n;
  double run_sum, present_sum, frame_sum, lat_sum;
  int count;
  float frame_max;
  uint32_t frames, missed;
  int64_t window_us;           /* start of the stats window */
} host_stats_t;

struct audio;
struct vtx;

typedef struct host {
  app_t *app;
  host_options_t opt;
  settings_t *set;
  fonts_t *fonts;
  font_class_t fcls;
  lcd_t lcd;
  etx_t *etx;
  etx_config_t ecfg;
  video_t *video;
  video_config_t vcfg;
  video_info_t vinfo;
  /* inputs */
  insrc_t src[INPUT_MAX_SOURCES];
  int nsrc;
  mix_config_t mix;
  sticks_t sticks;
  int cur_src;
  nav_t nav;
  nav_config_t navc;
  wifi_t *wifi;
  struct vtx *vtx;             /* VTX control (its own thread) */
  evdev_t *evdev;
  /* game */
  int game_state;
  int last_result;             /* etx_run: 0, 1 closed, -1 error */
  int64_t clock0_us;           /* getTime() zero */
  int keyq[32];                /* events for the next frames (one per frame) */
  int nkeys;
  int nav_pending;             /* a stick event that lost to a key; dropped if state/editing change */
  int nav_pending_state;
  bool nav_pending_editing;
  /* scenario overrides */
  bool stick_override;
  int ov[4];
  /* overlay: the web address on the main menu */
  char ip[64];
  int64_t ip_since_us;
  bool show_ip;
  int64_t ip_checked_us;
  /* the game */
  char script[256];
  char game_version[32];
  int64_t reload_at_us;        /* reload the game at/after this time (0 = none), in the menu */
  int64_t err_retry_us;        /* error screen: restart by itself at this time (0 = wait for ENTER) */
  int64_t err_window_us;
  int err_count;
  bool long_session;
  int64_t long_check_us;
  struct audio *audio;
  bool audio_offline;          /* "wav:"/"null" output: rendered by the caller per frame */
  double audio_frac;
  bool want_shot;
  /* stats */
  host_stats_t hs;
  int frames_drawn, frames_at_window;
  uint32_t src_updates_prev[INPUT_MAX_SOURCES];
  int64_t last_run_us, last_show_us, last_present_us, present_start_us, last_frame_us;
  double lat_measured_s;       /* smoothed sample -> mid-scanout time (latency = auto) */
  int64_t last_publish_us;
  int64_t last_flip_us;
  int64_t now_us;              /* the time of the current frame (real or virtual) */
  uint32_t video_missed_seen;
  bool quit;                   /* APP_CMD_QUIT seen */
  char err[512];
} host_t;

/* Set up from the settings at app->paths.settings. 0 ok, -1 fatal (err says why). */
int  host_init(host_t *h, app_t *app, const host_options_t *opt);
void host_close(host_t *h);

/* One frame at time now_us (plat_now_us clock, or virtual): read inputs, mix the sticks,
 * menus by sticks, web commands, run() the game (or draw the error screen), overlay.
 * Does not present: the caller calls host_present(). */
void host_frame(host_t *h, int64_t now_us);
int  host_present(host_t *h);              /* video_present of the LCD; 0 ok */
void host_queue_key(host_t *h, int evt);   /* an event for a coming frame */
int  host_reload_game(host_t *h);          /* re-create the Lua state; 0 ok */
void host_apply_settings(host_t *h);       /* re-read sticktime.txt, apply live keys */
void host_note_shown(host_t *h, int64_t vblank_us, int64_t sample_us, int64_t period_us);
void host_publish(host_t *h, int64_t now_us);  /* status snapshot for the web UI */
void host_audio_offline(host_t *h, double dt_ms);  /* offline audio: render one frame's samples */

#endif
