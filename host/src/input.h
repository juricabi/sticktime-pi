/* Stick sources and the mixer that turns them into the game's ail/ele/thr/rud.
 *
 * Every source (the ELRS/Crossfire WiFi stream, each evdev joystick or gamepad, the keyboard)
 * reports channels CH1..CH16 in -1024..1024. The mixer picks the active source (the newest one
 * that moved, or the one the settings fix), maps channels to sticks with stick_channels (a channel
 * per stick) or else channel_order (default AETR: CH1 = ail, CH2 = ele, CH3 = thr, CH4 = rud),
 * applies inversion, and reports the pause channel. Sources whose channels are already sticks
 * (gamepads, the keyboard: fixed_aetr) skip both maps. Pure logic: no I/O, no allocation. */
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
  int64_t moved_us;                 /* last time a stick moved more than ~5% (CH1-4, or for a source that is
                                       not fixed_aetr the stick_channels of the last mix_sticks call) */
  uint32_t updates;                 /* total updates, for rates */
} insrc_t;

typedef struct {
  insrc_kind_t fixed;               /* INSRC_NONE = auto */
  char order[5];                    /* permutation of "AETR"; letter i = what CHi+1 is */
  uint8_t chan[4];                  /* stick_channels: the channel (1..16) of ail, ele, thr, rud, used instead
                                       of order when all four are set and different; all 0 = use order */
  bool invert[4];                   /* ail, ele, thr, rud */
  int pause_ch;                     /* 1..16, 0 = none */
} mix_config_t;

typedef struct {
  int ail, ele, thr, rud;           /* -1024..1024, after the channel map and invert */
  int pause;                        /* pause channel value, or INT32_MIN when none/unknown */
  int src;                          /* index of the active source, -1 when none */
  insrc_kind_t kind;
  bool live;                        /* the active source updated within INPUT_STALE_US */
} sticks_t;

/* Parse "AETR"-style orders (case-insensitive, must be a permutation of A, E, T, R). */
bool mix_parse_order(const char *s, char out[5]);
/* Parse stick_channels: four different channels 1-16 for ail, ele, thr, rud, separated by spaces,
 * tabs or commas, each 1-2 digits with an optional "CH" ("1 2 3 4", "3,1,2,4", "ch1 ch2 ch3 ch4").
 * Returns 1 with the channels in out, 0 when there are none (empty or only separators: use
 * channel_order), -1 when invalid; out is all 0 unless 1 is returned. */
int mix_parse_channels(const char *s, uint8_t out[4]);
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
