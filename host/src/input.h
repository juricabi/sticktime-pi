/* Stick sources and the mixer that turns them into the game's ail/ele/thr/rud.
 *
 * Every source (the ELRS/Crossfire WiFi stream, each evdev joystick or gamepad, the keyboard)
 * reports channels CH1..CH16 in -1024..1024. The mixer picks the active source (the newest one
 * that moved, or the one the settings fix), maps channels to sticks with channel_order (default
 * AETR: CH1 = ail, CH2 = ele, CH3 = thr, CH4 = rud), applies inversion, and reports the pause
 * channel. Pure logic: no I/O, no allocation. */
#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>
#include <stdbool.h>

#define INPUT_CH 16
#define INPUT_MAX_SOURCES 12
#define INPUT_STALE_US 500000       /* a source silent this long is no longer live */

typedef enum {
  INSRC_NONE = 0,
  INSRC_KEYBOARD,                   /* keys fly too (see keyboard sticks in input_evdev.c) */
  INSRC_WIFI,                       /* ELRS / Crossfire WiFi joystick */
  INSRC_USB,                        /* EdgeTX/OpenTX radio as a USB joystick */
  INSRC_BLUETOOTH,                  /* ELRS BLE joystick */
  INSRC_GAMEPAD,                    /* channels already in AETR order (mode 2 layout) */
  INSRC_JOYSTICK,                   /* any other joystick: axes in order */
  INSRC_KINDS
} insrc_kind_t;

const char *insrc_kind_name(insrc_kind_t k);   /* "keyboard", "wifi", "usb", "bluetooth", "gamepad", "joystick" */

typedef struct {
  insrc_kind_t kind;
  bool present;                     /* slot in use (device open / stream known) */
  bool fixed_aetr;                  /* channels are already ail, ele, thr, rud (gamepad, keyboard) */
  char name[64];                    /* device name or "ELRS 192.168.1.50" */
  int16_t ch[INPUT_CH];             /* -1024..1024 */
  uint8_t nch;                      /* channels this source has (4..16) */
  int64_t last_us;                  /* last update (packet / event) */
  int64_t moved_us;                 /* last time a stick moved more than ~5% */
  uint32_t updates;                 /* total updates, for rates */
} insrc_t;

typedef struct {
  insrc_kind_t fixed;               /* INSRC_NONE = auto */
  char order[5];                    /* permutation of "AETR"; letter i = what CHi+1 is */
  bool invert[4];                   /* ail, ele, thr, rud */
  int pause_ch;                     /* 1..16, 0 = none */
} mix_config_t;

typedef struct {
  int ail, ele, thr, rud;           /* -1024..1024, after order and invert */
  int pause;                        /* pause channel value, or INT32_MIN when none/unknown */
  int src;                          /* index of the active source, -1 when none */
  insrc_kind_t kind;
  bool live;                        /* the active source updated within INPUT_STALE_US */
} sticks_t;

/* Parse "AETR"-style orders (case-insensitive, must be a permutation of A, E, T, R). */
bool mix_parse_order(const char *s, char out[5]);
/* Parse an invert list like "ail, thr" (also "a,t", "roll/pitch/throttle/yaw"). */
void mix_parse_invert(const char *s, bool inv[4]);
insrc_kind_t mix_parse_input(const char *s);   /* "auto" -> NONE, "wifi", "usb", "bluetooth", "keyboard", "gamepad", "joystick" */

/* note an update of a source: copies channels, stamps last_us, moved_us when it moved */
void insrc_update(insrc_t *s, const int16_t *ch, int nch, int64_t now_us);

/* Pick the active source and compute the sticks. `cur` is last frame's active index (-1):
 * auto mode keeps it while it is live unless another live source moved more recently than
 * it did by over 1 s. No live source: sticks centered, throttle -1024, live = false. */
void mix_sticks(const mix_config_t *cfg, const insrc_t *src, int n, int cur, int64_t now_us, sticks_t *out);

#endif
