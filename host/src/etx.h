/* EdgeTX Lua API for one tool script: the Lua state, the globals and functions of engine.js
 * installApi() (color radio), io on a real folder (the "SD card"), loadScript, and access to
 * the game's STICKTIME_TEST hooks. Lua is 5.3.6 with EdgeTX's numbers
 * (-DLUA_32BITS -DLUA_FLOORN2I=1 -DLUA_COMPAT_5_2): API arguments use luaL_checkinteger. */
#ifndef ETX_H
#define ETX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "lcd.h"

/* events (engine.js values) */
#define ETX_K_EXIT 1
#define ETX_K_ENTER 2
#define ETX_K_PAGEUP 3
#define ETX_K_PAGEDN 4
#define ETX_K_MODEL 6
#define ETX_K_SYS 7
#define ETX_K_TELE 8
#define ETX_BREAK(k) (0x200 | (k))
#define ETX_LONG(k)  (0x800 | (k))
#define ETX_FIRST(k) (0x600 | (k))
#define ETX_REPT(k)  (0x400 | (k))
#define ETX_EVT_ENTER      ETX_BREAK(ETX_K_ENTER)    /* EVT_VIRTUAL_ENTER */
#define ETX_EVT_ENTER_LONG ETX_LONG(ETX_K_ENTER)
#define ETX_EVT_EXIT       ETX_BREAK(ETX_K_EXIT)     /* EVT_VIRTUAL_EXIT */
#define ETX_EVT_EXIT_LONG  ETX_LONG(ETX_K_EXIT)
#define ETX_EVT_PREV       0x1D00                    /* EVT_VIRTUAL_PREV = DEC = ROT_LEFT */
#define ETX_EVT_NEXT       0x1E00                    /* EVT_VIRTUAL_NEXT = INC = ROT_RIGHT */
#define ETX_EVT_PREV_PAGE  ETX_BREAK(ETX_K_PAGEUP)
#define ETX_EVT_NEXT_PAGE  ETX_BREAK(ETX_K_PAGEDN)
#define ETX_EVT_MENU       ETX_BREAK(ETX_K_MODEL)

/* game states (src/sticktime.lua), from STICKTIME_TEST.get()'s 16th value */
enum { GS_UNKNOWN = 0, GS_MENU = 1, GS_SETUP = 2, GS_COUNT = 3, GS_FLY = 4, GS_CRASHED = 5,
       GS_READY = 6, GS_PAUSED = 7, GS_DONE = 8 };

typedef struct etx etx_t;

typedef struct {
  const char *sd_root;       /* real folder that is "/" of the radio's SD card */
  const char *script;        /* SD path of the tool, e.g. "/SCRIPTS/TOOLS/StickTime.lua" */
  bool test_hooks;           /* define the global STICKTIME_TEST = {} before the script loads */
  double lat;                /* STICKTIME_LAT in seconds; < 0 leaves it undefined */
  int stick_mode;            /* getStickMode(): 1..4 */
  bool async_writes;         /* io writes are handed to a writer thread at io.close (device) */
  /* (zero = default) */
  bool no_touch;             /* leave EVT_TOUCH_* undefined: the game's HUD then draws no pause button */
  int watchdog_ms;           /* one chunk/init()/run()/test call longer than this is stopped: 0 = 2000 */
} etx_config_t;

typedef struct {
  int64_t instr_count;       /* VM instructions so far, counted by the watchdog hook (1000 granularity) */
  size_t lua_kb;             /* collectgarbage('count') */
  int frames;
} etx_stats_t;

etx_t *etx_new(lcd_t *lcd, const etx_config_t *cfg);
void   etx_free(etx_t *e);

/* (Re)create the Lua state, load and run the script chunk, call init().
 * 0 ok; -1 on error (etx_error() says what: "Syntax error: ...", "init() error: ..."). */
int  etx_load(etx_t *e);
/* One radio cycle: run(event). 0 ok, 1 the script asked to close (nonzero return),
 * -1 script error (etx_error()). Does nothing (returns the last result) after an error. */
int  etx_run(etx_t *e, int event);
const char *etx_error(const etx_t *e);   /* "" when there is none */
bool etx_loaded(const etx_t *e);

/* inputs of the next run() */
void etx_set_clock_ms(etx_t *e, int64_t ms);           /* getTime() = ms / 10 */
void etx_set_sticks(etx_t *e, int ail, int ele, int thr, int rud);  /* -1024..1024 */
void etx_set_global_number(etx_t *e, const char *name, double v);
void etx_clear_global(etx_t *e, const char *name);

/* STICKTIME_TEST.<fn>(args...). args: numbers. Results: up to nout numbers (booleans 0/1,
 * other types NaN). Returns the number of results, or -1 (no such hook / error). */
int  etx_test_call(etx_t *e, const char *fn, const double *args, int nargs, double *out, int nout);
/* string result variant: the first string result of STICKTIME_TEST.<fn>(args) (e.g. a track name) */
int  etx_test_call_str(etx_t *e, const char *fn, const double *args, int nargs, int idx, char *out, size_t outlen);
int  etx_game_state(etx_t *e);   /* GS_* via STICKTIME_TEST.get(), GS_UNKNOWN without hooks */

void etx_gc_step(etx_t *e, int kb);      /* lua_gc(LUA_GCSTEP) after the frame is shown */
void etx_stats(etx_t *e, etx_stats_t *st);

/* SD paths -> real paths ("/SCRIPTS/x" -> sd_root + "/SCRIPTS/x"); rejects ".." components.
 * Returns 0 ok, -1 if the path is unsafe or too long. */
int  etx_sd_path(const etx_t *e, const char *sdpath, char *out, size_t outlen);
/* Wait until queued io writes are on disk (shutdown, tests). */
void etx_flush_writes(etx_t *e);

/* ---- additions */

/* Sandbox: base, coroutine, table, string, math, utf8, bit32 and EdgeTX's io; no os, debug,
 * package, dofile or loadfile. load() and loadScript() take text chunks only. Two differences
 * from EdgeTX keep the watchdog effective (Lua runs those calls with hooks off): setmetatable
 * refuses a metatable with __gc, and once the watchdog fired xpcall skips the script's message
 * handler. The watchdog can't stop one long C call (a pathological string pattern). */

/* playTone(freq, len, pause [, flags [, freqIncr [, volume]]]) and playHaptic(len [, pause]) calls,
 * oldest first. The queue keeps the newest ETX_TONE_QUEUE; a reload empties it. */
#define ETX_TONE_QUEUE 32
#define ETX_PLAY_NOW 0x10
#define ETX_PLAY_BACKGROUND 0x20
typedef struct {
  bool haptic;               /* playHaptic: len_ms, pause_ms only */
  int freq;                  /* Hz */
  int len_ms, pause_ms;
  int flags;                 /* ETX_PLAY_NOW (cut the queue), ETX_PLAY_BACKGROUND */
  int freq_incr;             /* EdgeTX's frequency sweep */
  int volume;                /* 6th argument, 0 when not given */
} etx_tone_t;
/* Move up to max queued calls into out (the rest stay queued). Returns how many. */
int  etx_take_tones(etx_t *e, etx_tone_t *out, int max);

/* print() and io errors go here (default: stderr). Commit errors of async writes are reported
 * from the writer thread, so fn must be thread-safe (app_log is). */
typedef void (*etx_log_fn)(void *ud, const char *line);
void etx_set_log(etx_t *e, etx_log_fn fn, void *ud);

/* run(event, touch) with a touch table (EVT_TOUCH_* events); t NULL = etx_run */
typedef struct { int x, y, start_x, start_y, slide_x, slide_y, tap_count; } etx_touch_t;
int  etx_run_touch(etx_t *e, int event, const etx_touch_t *t);

/* The message of the last failed etx_test_call / etx_test_call_str ("" when it succeeded).
 * etx_test_call_str: idx >= 0 is the result's position counted like etx_test_call's out[]
 * (info()'s name is 10), idx < 0 the first string result. Returns 0 with the string in out
 * (truncated to outlen - 1 bytes), or -1 (out = "") when there is no such hook, the call failed
 * or that result is not a string. */
const char *etx_test_error(const etx_t *e);

/* tests: hold queued async writes (they stay readable through io.open) until released.
 * etx_flush_writes() does not wait while writes are held. */
void etx_writer_hold(bool hold);

#endif
