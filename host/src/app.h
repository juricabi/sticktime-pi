/* State shared between the frame loop (main.c) and the worker threads (web server, WiFi
 * activation, helpers). The frame loop publishes a status snapshot a few times a second and
 * reads commands; workers never touch the Lua state, the LCD or the video device. */
#ifndef APP_H
#define APP_H

#include <stdint.h>
#include <stdbool.h>
#include "plat.h"
#include "input.h"
#include "settings.h"

#define APP_VERSION_DEFAULT "0.1.0-dev"

/* paths (all absolute or relative to the working directory) */
typedef struct {
  char base[512];          /* /boot/firmware/sticktime */
  char settings[512];      /* /boot/firmware/sticktime.txt */
  char sd[512];            /* base/sd: the game's SD card ("/") */
  char web[512];           /* base/web: the web UI */
  char fonts[512];         /* fonts directory (Roboto-Regular.ttf, Roboto-Bold.ttf) */
  char libexec[512];       /* helpers: sticktime-wifi, sticktime-update, sticktime-setup (/usr/lib/sticktime) */
  char state[512];         /* runtime state dir (/run/sticktime): update status, wifi scan results */
  char script[256];        /* SD path of the game: /SCRIPTS/TOOLS/StickTime.lua */
} app_paths_t;

typedef struct {
  insrc_kind_t kind;
  char name[64];
  bool live;
  float rate_hz;
  uint8_t nch;
  int16_t ch[INPUT_CH];
} app_source_status_t;

typedef struct {
  /* frame loop */
  int64_t uptime_ms;
  float fps;
  float frame_ms_avg, frame_ms_max;         /* time between shown frames */
  float run_ms_avg, run_ms_p95;             /* Lua run() incl. drawing */
  float present_ms_avg;                     /* scale + copy into the buffer */
  float latency_ms;                         /* stick sample -> vblank, average */
  uint32_t frames, missed;                  /* missed: frames that took more than one refresh */
  int game_state;                           /* GS_* */
  bool game_loaded;
  char game_error[512];
  char game_version[32];
  size_t lua_kb;
  /* video */
  char video_desc[128];
  int render_w, render_h;
  /* sticks */
  sticks_t sticks;
  int nsrc;
  app_source_status_t src[INPUT_MAX_SOURCES];
  /* wifi joystick */
  char wifi_module[64];                     /* "ELRS 10.0.0.1" or "" */
  char wifi_state[64];                      /* "listening", "activating 10.0.0.1", "streaming", ... */
  float wifi_rate_hz;
  uint32_t wifi_dropped;                    /* frames dropped by the Crossfire filter */
  uint32_t wifi_gaps;                       /* stick data gaps of WIFI_GAP_US or more (since the start) */
  uint32_t wifi_gap_max_ms;                 /* the longest of them */
  /* audio */
  bool audio_ok;
  char audio_device[48];
  char now_playing[128];
  /* the tool script that runs */
  char script[256];
} app_status_t;

/* commands from workers to the frame loop (bit flags) */
enum {
  APP_CMD_RELOAD_GAME     = 1 << 0,   /* re-create the Lua state and reload the script */
  APP_CMD_RELOAD_SETTINGS = 1 << 1,   /* re-read sticktime.txt and apply the live keys */
  APP_CMD_QUIT            = 1 << 2,   /* leave the loop (service restart, update) */
  APP_CMD_SCREENSHOT      = 1 << 3,   /* copy the next frame into app_t.shot */
  APP_CMD_MUSIC_NEXT      = 1 << 4,   /* skip to the next song */
  APP_CMD_SONGS_CHANGED   = 1 << 5,   /* the songs folder changed (upload / delete) */
};
/* special keys for app_post_key (web remote "up" / "down": editing-aware, see host.h HOST_KEY_*) */
#define APP_KEY_UP   (-101)
#define APP_KEY_DOWN (-102)

#define APP_KEYQ 16

typedef struct app {
  app_paths_t paths;
  char version[32];
  int64_t start_us;
  /* VTX control (vtx.h): set before the web server starts, cleared after it stopped; it has its own
   * lock (vtx_get_status, vtx_reload) */
  struct vtx *vtx;

  plat_mutex mu;                 /* guards everything below */
  app_status_t st;
  uint32_t cmds;
  int keyq[APP_KEYQ];            /* ETX_EVT_* from the web remote */
  int nkeys;
  /* screenshot handoff: the frame loop fills it when APP_CMD_SCREENSHOT is set */
  uint16_t *shot;
  int shot_w, shot_h;
  bool shot_ready;
  /* log ring (app_log) */
  char log[16384];
  size_t log_len;
} app_t;

void app_init(app_t *a);
/* thread-safe helpers */
void app_post(app_t *a, uint32_t cmds);
void app_post_key(app_t *a, int evt);
uint32_t app_take(app_t *a, int *keys, int *nkeys);     /* frame loop: take commands and keys */
void app_publish(app_t *a, const app_status_t *st);     /* frame loop: replace the snapshot */
void app_snapshot(app_t *a, app_status_t *out);         /* workers: copy the snapshot */
/* log a line (printf style) to stderr (journald) and the ring the web UI shows */
void app_log(app_t *a, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
  ;
size_t app_log_copy(app_t *a, char *out, size_t max);

#endif
