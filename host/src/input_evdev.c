/* Joysticks, gamepads and keyboards from evdev (contract and mappings: input_evdev.h).
 * The device logic (channel maps, keys, keyboard flying, SYN_DROPPED resync) is platform
 * independent and also runs virtual devices for the tests; opening, reading and hotplug are Linux,
 * with open()/close() of nodes on a worker thread after the first scan (they can sleep). */
#ifdef __linux__
#define _GNU_SOURCE
#endif
#include "input_evdev.h"
#include "etx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>
#else
/* the Linux input codes used here (linux/input-event-codes.h) */
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_ABS 0x03
#define SYN_REPORT 0
#define SYN_DROPPED 3
#define BUS_BLUETOOTH 0x05
#define INPUT_PROP_POINTER 0x00
#define INPUT_PROP_DIRECT 0x01
#define INPUT_PROP_ACCELEROMETER 0x06
#define KEY_ESC 1
#define KEY_MINUS 12
#define KEY_EQUAL 13
#define KEY_BACKSPACE 14
#define KEY_W 17
#define KEY_R 19
#define KEY_I 23
#define KEY_ENTER 28
#define KEY_LEFTCTRL 29
#define KEY_A 30
#define KEY_S 31
#define KEY_D 32
#define KEY_J 36
#define KEY_K 37
#define KEY_L 38
#define KEY_LEFTSHIFT 42
#define KEY_M 50
#define KEY_RIGHTSHIFT 54
#define KEY_LEFTALT 56
#define KEY_SPACE 57
#define KEY_KPMINUS 74
#define KEY_KPPLUS 78
#define KEY_KPENTER 96
#define KEY_RIGHTCTRL 97
#define KEY_RIGHTALT 100
#define KEY_UP 103
#define KEY_PAGEUP 104
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_DOWN 108
#define KEY_PAGEDOWN 109
#define KEY_LEFTMETA 125
#define KEY_RIGHTMETA 126
#define KEY_BACK 158
#define KEY_EXIT 174
#define KEY_OK 0x160
#define KEY_SELECT 0x161
#define BTN_MISC 0x100
#define BTN_LEFT 0x110
#define BTN_JOYSTICK 0x120
#define BTN_GAMEPAD 0x130
#define BTN_SOUTH 0x130
#define BTN_EAST 0x131
#define BTN_TL 0x136
#define BTN_TR 0x137
#define BTN_SELECT 0x13a
#define BTN_START 0x13b
#define BTN_TOOL_PEN 0x140
#define BTN_TOOL_FINGER 0x145
#define BTN_TOUCH 0x14a
#define BTN_STYLUS 0x14b
#define BTN_DPAD_UP 0x220
#define BTN_DPAD_DOWN 0x221
#define BTN_DPAD_LEFT 0x222
#define BTN_DPAD_RIGHT 0x223
#define BTN_TRIGGER_HAPPY 0x2c0
#define ABS_X 0x00
#define ABS_Y 0x01
#define ABS_Z 0x02
#define ABS_RX 0x03
#define ABS_RY 0x04
#define ABS_RZ 0x05
#define ABS_THROTTLE 0x06
#define ABS_RUDDER 0x07
#define ABS_WHEEL 0x08
#define ABS_GAS 0x09
#define ABS_BRAKE 0x0a
#define ABS_HAT0X 0x10
#define ABS_HAT0Y 0x11
#define ABS_HAT3Y 0x17
#define ABS_MT_SLOT 0x2f
#endif

#define MAX_DEVS 24                 /* keyboards take no source slot, so more than the slots */
#define MAX_PEND 32
#define MAX_REQ 64                  /* opens and closes waiting for the worker */
#define MAX_RES 16                  /* opens in flight (each result carries the caps) */
#define QLEN 64
#define INJ_LEN 256
#define DEADZONE 20                 /* emulator gamepads: |v| < 0.02 -> 0 */
#define LONG_EXIT_US 900000         /* emulator: a longer EXIT closes the tool */
#define RETRY_US 100000             /* udev gives a new node its permissions a little later */
#define RETRY_FOR_US 2000000
#define MAX_READS 16                /* reads per device and poll: a flood can't stall the frame */

enum { AIL, ELE, THR, RUD };

typedef struct {
  int8_t ch;                        /* channel, -1 = not used */
  bool neg, dz;
} axmap_t;

typedef struct {
  bool used, virt;
  uint32_t gen;                     /* injected events for an earlier device in this entry are dropped */
  int fd;
  char node[32];                    /* "event3" */
  char name[64];
  insrc_kind_t kind;                /* INSRC_NONE: a keyboard only */
  bool kbd;
  int slot;                         /* src index, -1 = none (keyboard only, or no slot free yet) */
  uint8_t nch;
  bool changed;                     /* channels changed since the slot was updated */
  bool dropping;                    /* after SYN_DROPPED: skip to the next SYN_REPORT, then resync */
  bool toggle;                      /* gamepad CH5 */
  int hat[2];                       /* gamepad D-pad hat x, y: -1, 0, +1 */
  int16_t ch[INPUT_CH];
  uint8_t absbit[EVDEV_NABS / 8], keybit[EVDEV_NKEY / 8];
  int absmin[EVDEV_NABS], absmax[EVDEV_NABS], raw[EVDEV_NABS];
  axmap_t ax[EVDEV_NABS];
  uint16_t btn[INPUT_CH];           /* button code -> channel btnch[] */
  int8_t btnch[INPUT_CH];
  int nbtn;
  uint8_t keys[EVDEV_NKEY / 8];     /* held, as far as the events told */
  int vabs[EVDEV_NABS];             /* virtual devices: what the kernel would report */
  uint8_t vkeys[EVDEV_NKEY / 8];
} edev_t;

typedef struct {
  bool used, timed;                 /* timed: the clock was known (the scan at open has none) */
  bool busy;                        /* the worker is opening it (request seq) */
  bool redo;                        /* the node was made again meanwhile: that open may be the old device */
  uint32_t seq;
  char node[32];
  int64_t until_us, next_us;
} pend_t;

#ifdef __linux__
typedef struct {
  bool open;                        /* else close fd */
  int fd;
  uint32_t seq;
  char node[32];
} req_t;

typedef struct {
  int r, fd;                        /* probe_node() */
  uint32_t seq;
  char node[32];
  evdev_caps_t caps;
} res_t;
#endif

typedef struct {
  int16_t dev;
  uint16_t type, code;
  uint32_t gen;
  int32_t value;
} inj_t;

struct evdev {
  char dir[256];
  int ino_fd;
  bool rescan;
  edev_t dev[MAX_DEVS];
  pend_t pend[MAX_PEND];
  int mode;                         /* stick mode for gamepads */
  bool flying;
  int64_t now_us;
  int owner[INPUT_MAX_SOURCES];     /* slot -> device, -1 */
  bool dirty[INPUT_MAX_SOURCES];    /* the slot must be (re)initialized in src */
  int q[QLEN], qhead, qn;           /* key events not handed out yet */
  inj_t inj[INJ_LEN];
  int injhead, injn;
  /* keyboard */
  bool kbd_present, kbd_dirty, kbd_started;
  double ks[4];                     /* ail ele thr rud, -1..1 as in the emulator */
  int16_t kch[4];
  int64_t kbd_us;
  bool exit_down, exit_long;
  int exit_dev;
  int64_t exit_us;
#ifdef __linux__
  /* open() and close() of an input node can sleep: usbhid_open() waits 50 ms for a joystick, and
   * the release path waits for RCU grace periods (30-60 ms measured). After the scan at open a
   * worker does them; the frame loop only hands requests over and picks up results. */
  pthread_t th;
  bool th_ok, th_stop;
  pthread_mutex_t mu;
  pthread_cond_t cv;
  req_t req[MAX_REQ];
  int reqhead, reqn;
  res_t res[MAX_RES];
  int reshead, resn, inflight;
  uint32_t seq;
  int wake_fd;                      /* eventfd: a result is ready (for a loop that sleeps in poll) */
#endif
};

static bool bit(const uint8_t *b, int n) { return (b[n >> 3] >> (n & 7)) & 1; }

static void setbit(uint8_t *b, int n, bool on) {
  if (on) b[n >> 3] |= (uint8_t)(1u << (n & 7));
  else b[n >> 3] &= (uint8_t)~(1u << (n & 7));
}

/* ---------------------------------------------------------------- pure helpers */

int16_t evdev_scale_axis(int value, int min, int max) {
  int64_t d = (int64_t)max - min;
  if (d == 0) return 0;
  int64_t n = (2 * (int64_t)value - min - max) * 1024;   /* the result times d */
  if (d < 0) { d = -d; n = -n; }
  /* toward zero: an odd range (0..2047) has two middle values, +-0.5 from the midpoint, and both
   * give 0; the ends stay exact and the map stays odd-symmetric */
  int64_t q = (n < 0 ? -n : n) / d;
  if (q > 1024) q = 1024;
  return (int16_t)(n < 0 ? -q : q);
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* needle (lower case) in s, ignoring case; at_word: only where a word starts */
static bool has_text(const char *s, const char *needle, bool at_word) {
  size_t n = strlen(needle);
  for (const char *p = s; *p; p++) {
    if (at_word && p > s) {
      int c = lower((unsigned char)p[-1]);
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) continue;
    }
    size_t i = 0;
    while (i < n && p[i] && lower((unsigned char)p[i]) == needle[i]) i++;
    if (i == n) return true;
  }
  return false;
}

insrc_kind_t evdev_classify(const char *name, int bus, bool has_gamepad_btn, bool has_abs_xy, bool has_keys) {
  if (!name) name = "";
  /* a radio's name counts unless the device is a plain keyboard */
  bool stick = has_abs_xy || !has_keys;
  if (stick && (has_text(name, "edgetx", false) || has_text(name, "opentx", false))) return INSRC_USB;
  if (stick && (has_text(name, "elrs", true) || has_text(name, "expresslrs", false))) return INSRC_BLUETOOTH;
  /* a Bluetooth gamepad is a gamepad (its sticks are not ELRS channels) */
  if (bus == BUS_BLUETOOTH && has_abs_xy && !has_gamepad_btn) return INSRC_BLUETOOTH;
  if (has_gamepad_btn) return INSRC_GAMEPAD;
  if (has_abs_xy) return INSRC_JOYSTICK;
  if (has_keys) return INSRC_KEYBOARD;
  return INSRC_NONE;
}

insrc_kind_t evdev_classify_caps(const evdev_caps_t *c, bool *keyboard) {
  /* touchpads, touch screens, tablets, mice with absolute axes and motion sensors (a gamepad's
   * "Motion Sensors" node) report ABS_X/ABS_Y too: never sticks */
  bool pointer = bit(c->prop, INPUT_PROP_POINTER) || bit(c->prop, INPUT_PROP_DIRECT)
    || bit(c->prop, INPUT_PROP_ACCELEROMETER) || bit(c->key, BTN_LEFT) || bit(c->key, BTN_TOUCH)
    || bit(c->key, BTN_TOOL_PEN) || bit(c->key, BTN_TOOL_FINGER) || bit(c->key, BTN_STYLUS);
  bool xy = bit(c->abs, ABS_X) && bit(c->abs, ABS_Y) && !pointer;
  bool keys = bit(c->key, KEY_ENTER) || bit(c->key, KEY_A) || bit(c->key, KEY_OK) || bit(c->key, KEY_SELECT);
  if (keyboard) *keyboard = keys;
  char name[sizeof c->name + 1];
  memcpy(name, c->name, sizeof c->name);
  name[sizeof c->name] = 0;
  return evdev_classify(name, c->bus, bit(c->key, BTN_GAMEPAD), xy, keys);
}

static const char *kind_name(insrc_kind_t k) {
  static const char *n[INSRC_KINDS] = { "none", "keyboard", "wifi", "usb", "bluetooth", "gamepad", "joystick" };
  return (unsigned)k < INSRC_KINDS ? n[k] : "?";
}

/* ---------------------------------------------------------------- device logic */

/* [left x, left y, right x, right y] per stick mode (emulator app.js MODES) */
static const int8_t MODES[4][4] = {
  { RUD, ELE, AIL, THR },
  { RUD, THR, AIL, ELE },
  { AIL, ELE, RUD, THR },
  { AIL, THR, RUD, ELE },
};

static void push(evdev_t *e, int ev) {
  if (e->qn < QLEN) { e->q[(e->qhead + e->qn) % QLEN] = ev; e->qn++; }
}

static bool held(const evdev_t *e, int code) {
  for (int i = 0; i < MAX_DEVS; i++)
    if (e->dev[i].used && bit(e->dev[i].keys, code)) return true;
  return false;
}

static void dev_axis(edev_t *d, int code) {
  const axmap_t *m = &d->ax[code];
  if (m->ch < 0) return;
  int v = evdev_scale_axis(d->raw[code], d->absmin[code], d->absmax[code]);
  if (m->neg) v = -v;
  if (m->dz && v >= -DEADZONE && v <= DEADZONE) v = 0;
  if (d->ch[m->ch] != v) { d->ch[m->ch] = (int16_t)v; d->changed = true; }
}

static void map_axis(edev_t *d, int code, int ch, bool neg, bool dz) {
  if (code < 0 || code >= EVDEV_NABS || !bit(d->absbit, code)) return;
  d->ax[code].ch = (int8_t)ch;
  d->ax[code].neg = neg;
  d->ax[code].dz = dz;
}

/* joystick buttons (not mouse or tablet ones); HID puts buttons past the 16th at TRIGGER_HAPPY */
static bool stick_button(int c) {
  return (c >= BTN_MISC && c < BTN_LEFT) || (c >= BTN_JOYSTICK && c < BTN_TOOL_PEN)
    || (c >= BTN_TRIGGER_HAPPY && c < BTN_TRIGGER_HAPPY + 40);
}

static void map_buttons(edev_t *d, int first) {
  for (int c = BTN_MISC; c < EVDEV_NKEY && first + d->nbtn < INPUT_CH; c++)
    if (stick_button(c) && bit(d->keybit, c)) {
      d->btn[d->nbtn] = (uint16_t)c;
      d->btnch[d->nbtn] = (int8_t)(first + d->nbtn);
      d->nbtn++;
    }
}

static int hat_dir(const edev_t *d, int code, int value) {
  if (d->absmin[code] >= d->absmax[code]) return (value > 0) - (value < 0);
  int s = evdev_scale_axis(value, d->absmin[code], d->absmax[code]);
  return s < -512 ? -1 : s > 512 ? 1 : 0;
}

/* channel map from the kind (and the stick mode for gamepads), then the channels from the state */
static void dev_map(const evdev_t *e, edev_t *d) {
  for (int c = 0; c < EVDEV_NABS; c++) d->ax[c].ch = -1;
  d->nbtn = 0;
  memset(d->ch, 0, sizeof d->ch);
  switch (d->kind) {
  case INSRC_USB:
    for (int c = 0; c < 8; c++) map_axis(d, c, c, false, false);
    map_buttons(d, 8);
    d->nch = (uint8_t)(8 + d->nbtn);
    break;
  case INSRC_BLUETOOTH: {
    static const int8_t ble[8] = { ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_THROTTLE, ABS_RUDDER };
    for (int i = 0; i < 8; i++) map_axis(d, ble[i], i, false, false);
    if (!bit(d->absbit, ABS_RUDDER)) map_axis(d, ABS_WHEEL, 7, false, false);
    map_buttons(d, 8);
    d->nch = (uint8_t)(8 + d->nbtn);
    break;
  }
  case INSRC_GAMEPAD: {
    const int8_t *m = MODES[e->mode - 1];
    /* generic HID pads (and the Xbox pad over Bluetooth) put the right stick on Z/RZ */
    bool rxy = bit(d->absbit, ABS_RX) && bit(d->absbit, ABS_RY);
    map_axis(d, ABS_X, m[0], false, m[0] != THR);
    map_axis(d, ABS_Y, m[1], true, m[1] != THR);
    map_axis(d, rxy ? ABS_RX : ABS_Z, m[2], false, m[2] != THR);
    map_axis(d, rxy ? ABS_RY : ABS_RZ, m[3], true, m[3] != THR);
    map_axis(d, rxy ? ABS_Z : ABS_BRAKE, 5, false, false);
    map_axis(d, rxy ? ABS_RZ : ABS_GAS, 6, false, false);
    d->ch[4] = d->toggle ? 1024 : -1024;
    d->ch[5] = d->ch[6] = -1024;
    d->nch = 7;
    break;
  }
  case INSRC_JOYSTICK: {
    int n = 0;
    for (int c = 0; c < ABS_MT_SLOT && n < INPUT_CH; c++)
      if (bit(d->absbit, c) && !(c >= ABS_HAT0X && c <= ABS_HAT3Y)) map_axis(d, c, n++, false, true);
    map_buttons(d, n);
    d->nch = (uint8_t)(n + d->nbtn < 4 ? 4 : n + d->nbtn);
    break;
  }
  default:
    d->nch = 0;
    break;
  }
  for (int c = 0; c < EVDEV_NABS; c++) dev_axis(d, c);
  for (int i = 0; i < d->nbtn; i++) d->ch[d->btnch[i]] = bit(d->keys, d->btn[i]) ? 1024 : -1024;
  d->changed = true;
}

static bool exit_key(int c) { return c == KEY_ESC || c == KEY_BACKSPACE || c == KEY_BACK || c == KEY_EXIT; }

static bool modifier_held(const evdev_t *e) {
  return held(e, KEY_LEFTCTRL) || held(e, KEY_RIGHTCTRL) || held(e, KEY_LEFTALT) || held(e, KEY_RIGHTALT)
    || held(e, KEY_LEFTMETA) || held(e, KEY_RIGHTMETA);
}

/* keyboard keys, as the emulator's keydown/keyup handlers (app.js:876-905) */
static void kbd_key(evdev_t *e, int idx, int code, bool down) {
  if (!down) {
    if (exit_key(code) && e->exit_down) {
      if (!e->exit_long) push(e, ETX_EVT_EXIT);
      e->exit_down = false;
    }
    return;
  }
  if (modifier_held(e)) return;
  bool nav = !e->flying;                /* in flight the arrows fly */
  switch (code) {
  case KEY_ENTER: case KEY_KPENTER: case KEY_SPACE: case KEY_OK: case KEY_SELECT:
    push(e, ETX_EVT_ENTER); break;
  case KEY_ESC: case KEY_BACKSPACE: case KEY_BACK: case KEY_EXIT:
    e->exit_down = true; e->exit_long = false; e->exit_dev = idx; e->exit_us = e->now_us; break;
  case KEY_UP: if (nav) push(e, EVDEV_KEY_UP); break;
  case KEY_DOWN: if (nav) push(e, EVDEV_KEY_DOWN); break;
  case KEY_LEFT: if (nav) push(e, ETX_EVT_PREV); break;
  case KEY_RIGHT: if (nav) push(e, ETX_EVT_NEXT); break;
  case KEY_EQUAL: case KEY_KPPLUS: push(e, ETX_EVT_NEXT); break;
  case KEY_MINUS: case KEY_KPMINUS: push(e, ETX_EVT_PREV); break;
  case KEY_PAGEUP: push(e, ETX_EVT_PREV_PAGE); break;
  case KEY_PAGEDOWN: push(e, ETX_EVT_NEXT_PAGE); break;
  case KEY_M: push(e, ETX_EVT_MENU); break;
  case KEY_R: push(e, EVDEV_KEY_RELOAD); break;
  default: break;
  }
}

static void pad_button(evdev_t *e, edev_t *d, int code, bool down) {
  if (!down) return;
  switch (code) {
  case BTN_SOUTH: push(e, ETX_EVT_ENTER); break;
  case BTN_EAST: case BTN_START: case BTN_SELECT: push(e, ETX_EVT_EXIT); break;
  case BTN_DPAD_UP: push(e, EVDEV_KEY_UP); break;
  case BTN_DPAD_DOWN: push(e, EVDEV_KEY_DOWN); break;
  case BTN_DPAD_LEFT: push(e, ETX_EVT_PREV); break;
  case BTN_DPAD_RIGHT: push(e, ETX_EVT_NEXT); break;
  case BTN_TL: case BTN_TR:
    d->toggle = !d->toggle;
    d->ch[4] = d->toggle ? 1024 : -1024;
    d->changed = true;
    break;
  default: break;
  }
}

/* key codes (not buttons) act as keyboard keys on any device: a TV remote, a pad's Back key */
static bool keyboard_code(int c) {
  return c < BTN_MISC || (c >= KEY_OK && c < BTN_TRIGGER_HAPPY && !(c >= BTN_DPAD_UP && c <= BTN_DPAD_RIGHT));
}

static void dev_key(evdev_t *e, int idx, int code, int value) {
  edev_t *d = &e->dev[idx];
  if (code < 0 || code >= EVDEV_NKEY) return;
  bool down = value != 0, was = bit(d->keys, code);
  setbit(d->keys, code, down);
  if (value == 2 || down == was) return;    /* auto-repeat only keeps the key held */
  for (int i = 0; i < d->nbtn; i++)
    if (d->btn[i] == code) { d->ch[d->btnch[i]] = down ? 1024 : -1024; d->changed = true; }
  if (d->kind == INSRC_GAMEPAD) pad_button(e, d, code, down);
  if (keyboard_code(code)) kbd_key(e, idx, code, down);
}

static void dev_abs(evdev_t *e, edev_t *d, int code, int value) {
  if (code < 0 || code >= EVDEV_NABS) return;
  d->raw[code] = value;
  if (d->kind == INSRC_GAMEPAD && (code == ABS_HAT0X || code == ABS_HAT0Y)) {
    int dir = hat_dir(d, code, value);
    int *h = &d->hat[code - ABS_HAT0X];
    if (dir && dir != *h) {
      if (code == ABS_HAT0X) push(e, dir < 0 ? ETX_EVT_PREV : ETX_EVT_NEXT);
      else push(e, dir < 0 ? EVDEV_KEY_UP : EVDEV_KEY_DOWN);
    }
    *h = dir;
  }
  dev_axis(d, code);
}

#ifdef __linux__
static void dev_query(const edev_t *d, int *absval, uint8_t *keys) {
  for (int c = 0; c < EVDEV_NABS; c++) {
    struct input_absinfo ai;
    if (bit(d->absbit, c) && ioctl(d->fd, EVIOCGABS(c), &ai) == 0) absval[c] = ai.value;
  }
  uint8_t k[EVDEV_NKEY / 8];
  if (ioctl(d->fd, EVIOCGKEY(sizeof k), k) >= 0) memcpy(keys, k, sizeof k);
}
#endif

/* After SYN_DROPPED the kernel's state is the truth: read it and feed the differences in as
 * events (a key that went down in the gap gives its event late rather than never). */
static void dev_resync(evdev_t *e, int idx) {
  edev_t *d = &e->dev[idx];
  int absval[EVDEV_NABS];
  uint8_t keys[EVDEV_NKEY / 8];
  memcpy(absval, d->raw, sizeof absval);
  memcpy(keys, d->keys, sizeof keys);
  if (d->virt) {
    memcpy(absval, d->vabs, sizeof absval);
    memcpy(keys, d->vkeys, sizeof keys);
  }
#ifdef __linux__
  else dev_query(d, absval, keys);
#endif
  for (int c = 0; c < EVDEV_NABS; c++)
    if (bit(d->absbit, c) && absval[c] != d->raw[c]) dev_abs(e, d, c, absval[c]);
  for (int c = 0; c < EVDEV_NKEY; c++)
    if (bit(keys, c) != bit(d->keys, c)) dev_key(e, idx, c, bit(keys, c));
}

static void dev_event(evdev_t *e, int idx, int type, int code, int value) {
  edev_t *d = &e->dev[idx];
  if (type == EV_SYN) {
    if (code == SYN_DROPPED) d->dropping = true;
    else if (code == SYN_REPORT && d->dropping) { d->dropping = false; dev_resync(e, idx); }
    return;
  }
  if (d->dropping) return;
  if (type == EV_ABS) dev_abs(e, d, code, value);
  else if (type == EV_KEY) dev_key(e, idx, code, value);
}

static void assign_slots(evdev_t *e) {
  for (int i = 0; i < MAX_DEVS; i++) {
    edev_t *d = &e->dev[i];
    if (!d->used || d->kind == INSRC_NONE || d->slot >= 0) continue;
    for (int s = SLOT_EVDEV_FIRST; s < INPUT_MAX_SOURCES; s++)
      if (e->owner[s] < 0) { e->owner[s] = i; e->dirty[s] = true; d->slot = s; break; }
  }
}

static int dev_add(evdev_t *e, const evdev_caps_t *c, int fd, const char *node, bool virt) {
  bool kbd = false;
  insrc_kind_t k = evdev_classify_caps(c, &kbd);
  if (k == INSRC_KEYBOARD) k = INSRC_NONE;
  if (k == INSRC_NONE && !kbd) return -1;
  int i = 0;
  while (i < MAX_DEVS && e->dev[i].used) i++;
  if (i == MAX_DEVS) return -1;
  edev_t *d = &e->dev[i];
  uint32_t gen = d->gen + 1;
  memset(d, 0, sizeof *d);
  d->gen = gen;
  d->used = true;
  d->virt = virt;
  d->fd = fd;
  d->kind = k;
  d->kbd = kbd;
  d->slot = -1;
  snprintf(d->node, sizeof d->node, "%s", node ? node : "");
  memcpy(d->name, c->name, sizeof d->name);
  d->name[sizeof d->name - 1] = 0;
  memcpy(d->absbit, c->abs, sizeof d->absbit);
  memcpy(d->keybit, c->key, sizeof d->keybit);
  memcpy(d->absmin, c->absmin, sizeof d->absmin);
  memcpy(d->absmax, c->absmax, sizeof d->absmax);
  memcpy(d->raw, c->absval, sizeof d->raw);
  memcpy(d->vabs, c->absval, sizeof d->vabs);
  memcpy(d->keys, c->keystate, sizeof d->keys);
  memcpy(d->vkeys, c->keystate, sizeof d->vkeys);
  /* what is held at open is state, not a press */
  if (bit(d->absbit, ABS_HAT0X)) d->hat[0] = hat_dir(d, ABS_HAT0X, d->raw[ABS_HAT0X]);
  if (bit(d->absbit, ABS_HAT0Y)) d->hat[1] = hat_dir(d, ABS_HAT0Y, d->raw[ABS_HAT0Y]);
  dev_map(e, d);
  assign_slots(e);
  return i;
}

#ifdef __linux__
static void close_fd(evdev_t *e, int fd);
#endif

static void dev_remove(evdev_t *e, int i, bool log) {
  edev_t *d = &e->dev[i];
  if (!d->used) return;
#ifdef __linux__
  if (!d->virt && d->fd >= 0) {
    close_fd(e, d->fd);
    if (log) fprintf(stderr, "evdev: - %s \"%s\"\n", d->node, d->name);
  }
#else
  (void)log;
#endif
  if (d->slot >= 0) { e->owner[d->slot] = -1; e->dirty[d->slot] = true; }
  if (e->exit_down && e->exit_dev == i) e->exit_down = false;   /* no long-press reload from a lost key */
  d->used = false;
  d->fd = -1;
  d->slot = -1;
}

/* ---------------------------------------------------------------- Linux devices and hotplug */

#ifdef __linux__
/* The EVIOCG* bitmaps are arrays of unsigned long; on little-endian (the Pi, x86) that is the
 * byte layout bit() reads. */
static int read_caps(int fd, evdev_caps_t *c) {
  uint8_t ev[4] = { 0 };
  memset(c, 0, sizeof *c);
  if (ioctl(fd, EVIOCGBIT(0, sizeof ev), ev) < 0) return -1;
  if (ioctl(fd, EVIOCGNAME(sizeof c->name - 1), c->name) < 0) c->name[0] = 0;
  c->name[sizeof c->name - 1] = 0;
  struct input_id id;
  if (ioctl(fd, EVIOCGID, &id) == 0) c->bus = id.bustype;
  if (bit(ev, EV_KEY)) {
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof c->key), c->key) < 0) memset(c->key, 0, sizeof c->key);
    if (ioctl(fd, EVIOCGKEY(sizeof c->keystate), c->keystate) < 0) memset(c->keystate, 0, sizeof c->keystate);
  }
  if (bit(ev, EV_ABS)) {
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof c->abs), c->abs) < 0) memset(c->abs, 0, sizeof c->abs);
    for (int a = 0; a < EVDEV_NABS; a++) {
      struct input_absinfo ai;
      if (!bit(c->abs, a)) continue;
      if (ioctl(fd, EVIOCGABS(a), &ai) == 0) { c->absmin[a] = ai.minimum; c->absmax[a] = ai.maximum; c->absval[a] = ai.value; }
      else setbit(c->abs, a, false);
    }
  }
  if (ioctl(fd, EVIOCGPROP(sizeof c->prop), c->prop) < 0) memset(c->prop, 0, sizeof c->prop);
  return 0;
}

/* 1 opened (*fd, caps), 0 not an input we use (closed again), < 0 -errno of open().
 * Runs on the worker (or at open): touches nothing in evdev_t but the constant dir. */
static int probe_node(const char *dir, const char *node, int *fd_out, evdev_caps_t *c) {
  char path[300];
  snprintf(path, sizeof path, "%s/%s", dir, node);
  *fd_out = -1;
  int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) return -errno;
  bool kbd = false;
  insrc_kind_t k = INSRC_NONE;
  if (read_caps(fd, c) == 0) k = evdev_classify_caps(c, &kbd);
  if (k == INSRC_NONE && !kbd) { close(fd); return 0; }
  *fd_out = fd;
  return 1;
}

static void *worker(void *arg) {
  evdev_t *e = arg;
  struct sched_param sp = { 0 };
  pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);   /* never above the frame loop */
  pthread_mutex_lock(&e->mu);
  for (;;) {
    while (!e->reqn && !e->th_stop) pthread_cond_wait(&e->cv, &e->mu);
    if (!e->reqn) break;
    req_t q = e->req[e->reqhead];
    e->reqhead = (e->reqhead + 1) % MAX_REQ;
    e->reqn--;
    if (q.open && e->th_stop) { e->inflight--; continue; }   /* closing down: closes only */
    pthread_mutex_unlock(&e->mu);
    if (!q.open) {
      close(q.fd);
    } else {
      res_t r;
      r.seq = q.seq;
      memcpy(r.node, q.node, sizeof r.node);
      r.r = probe_node(e->dir, q.node, &r.fd, &r.caps);
      pthread_mutex_lock(&e->mu);
      /* inflight <= MAX_RES, so there is room */
      if (e->resn < MAX_RES) { e->res[(e->reshead + e->resn) % MAX_RES] = r; e->resn++; }
      else { if (r.fd >= 0) close(r.fd); e->inflight--; }
      pthread_mutex_unlock(&e->mu);
      uint64_t one = 1;
      if (write(e->wake_fd, &one, sizeof one) < 0) { /* the counter is full: already awake */ }
    }
    pthread_mutex_lock(&e->mu);
  }
  pthread_mutex_unlock(&e->mu);
  return NULL;
}

/* close() sleeps in the kernel: the worker does it (or here, without one) */
static void close_fd(evdev_t *e, int fd) {
  if (fd < 0) return;
  bool queued = false;
  if (e->th_ok) {
    pthread_mutex_lock(&e->mu);
    if (!e->th_stop && e->reqn < MAX_REQ) {
      req_t *q = &e->req[(e->reqhead + e->reqn) % MAX_REQ];
      q->open = false;
      q->fd = fd;
      e->reqn++;
      queued = true;
      pthread_cond_signal(&e->cv);
    }
    pthread_mutex_unlock(&e->mu);
  }
  if (!queued) close(fd);
}

static bool request_open(evdev_t *e, pend_t *p) {
  bool ok = false;
  pthread_mutex_lock(&e->mu);
  if (e->reqn < MAX_REQ && e->inflight < MAX_RES) {
    req_t *q = &e->req[(e->reqhead + e->reqn) % MAX_REQ];
    q->open = true;
    q->fd = -1;
    q->seq = ++e->seq;
    memcpy(q->node, p->node, sizeof q->node);
    p->seq = q->seq;
    e->reqn++;
    e->inflight++;
    ok = true;
    pthread_cond_signal(&e->cv);
  }
  pthread_mutex_unlock(&e->mu);
  if (ok) p->busy = true;
  return ok;
}

static int dev_by_node(const evdev_t *e, const char *node) {
  for (int i = 0; i < MAX_DEVS; i++)
    if (e->dev[i].used && !e->dev[i].virt && !strcmp(e->dev[i].node, node)) return i;
  return -1;
}

static void accept_node(evdev_t *e, const char *node, int fd, const evdev_caps_t *c) {
  int i = dev_by_node(e, node) >= 0 ? -1 : dev_add(e, c, fd, node, false);
  if (i < 0) { close_fd(e, fd); return; }   /* already open, or no room */
  const edev_t *d = &e->dev[i];
  if (d->kind != INSRC_NONE)
    fprintf(stderr, "evdev: + %s \"%s\" %s%s, slot %d\n", node, d->name, kind_name(d->kind), d->kbd ? " + keyboard" : "", d->slot);
  else
    fprintf(stderr, "evdev: + %s \"%s\" keyboard\n", node, d->name);
}

static bool gone(int r) { return r == -ENOENT || r == -ENODEV || r == -ENXIO; }

static pend_t *pend_find(evdev_t *e, const char *node) {
  for (int i = 0; i < MAX_PEND; i++)
    if (e->pend[i].used && !strcmp(e->pend[i].node, node)) return &e->pend[i];
  return NULL;
}

/* replaced: the node was made again (IN_CREATE), so an open in flight may have the old device */
static void pend_add(evdev_t *e, const char *node, int64_t now, bool timed, bool replaced) {
  pend_t *p = pend_find(e, node);
  if (p) {
    p->next_us = now;
    if (p->busy && replaced) p->redo = true;
    if (replaced) p->until_us = now + RETRY_FOR_US;
    return;
  }
  for (int i = 0; i < MAX_PEND && !p; i++)
    if (!e->pend[i].used) p = &e->pend[i];
  if (!p) { fprintf(stderr, "evdev: too many new devices, %s skipped\n", node); return; }
  memset(p, 0, sizeof *p);
  p->used = true;
  p->timed = timed;
  snprintf(p->node, sizeof p->node, "%s", node);
  p->until_us = now + RETRY_FOR_US;
  p->next_us = now;
}

static void pend_drop(evdev_t *e, const char *node) {
  pend_t *p = pend_find(e, node);
  if (p) p->used = false;           /* an open in flight is closed when it comes back */
}

static void pend_result(evdev_t *e, pend_t *p, int r, int fd, const evdev_caps_t *c, int64_t now) {
  if (r == 1) { accept_node(e, p->node, fd, c); p->used = false; }
  else if (r == 0 || gone(r)) p->used = false;
  else if (now >= p->until_us) {
    fprintf(stderr, "evdev: can't open %s/%s: %s\n", e->dir, p->node, strerror(-r));
    p->used = false;
  } else p->next_us = now + RETRY_US;   /* EACCES: udev sets the permissions a little later */
}

static void take_results(evdev_t *e, int64_t now) {
  if (!e->th_ok) return;
  uint64_t n;
  while (read(e->wake_fd, &n, sizeof n) > 0) {}
  for (;;) {
    res_t r;
    pthread_mutex_lock(&e->mu);
    bool got = e->resn > 0;
    if (got) {
      r = e->res[e->reshead];
      e->reshead = (e->reshead + 1) % MAX_RES;
      e->resn--;
      e->inflight--;
    }
    pthread_mutex_unlock(&e->mu);
    if (!got) break;
    pend_t *p = pend_find(e, r.node);
    bool mine = p && p->busy && p->seq == r.seq;
    if (!mine || p->redo) {
      /* the node went away (or came again) while it was opened */
      if (r.r == 1) close_fd(e, r.fd);
      if (mine) { p->busy = false; p->redo = false; p->next_us = now; }
      continue;
    }
    p->busy = false;
    pend_result(e, p, r.r, r.fd, &r.caps, now);
  }
}

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

/* every event node not open yet, in number order. now_open: open them here (the scan in
 * evdev_open), else leave them to the worker. opendir allocates: only at start and after an
 * inotify overflow. */
static void scan_dir(evdev_t *e, int64_t now, bool timed, bool now_open) {
  DIR *dp = opendir(e->dir);
  if (!dp) return;
  int nums[128], n = 0;
  struct dirent *de;
  while ((de = readdir(dp)) && n < 128) {
    int k;
    char extra;
    if (sscanf(de->d_name, "event%d%c", &k, &extra) == 1 && k >= 0) nums[n++] = k;
  }
  closedir(dp);
  qsort(nums, (size_t)n, sizeof nums[0], cmp_int);
  for (int i = 0; i < n; i++) {
    char node[32];
    snprintf(node, sizeof node, "event%d", nums[i]);
    if (dev_by_node(e, node) >= 0) continue;
    if (!now_open) { pend_add(e, node, now, timed, false); continue; }
    int fd;
    evdev_caps_t c;
    int r = probe_node(e->dir, node, &fd, &c);
    if (r == 1) accept_node(e, node, fd, &c);
    else if (r < 0 && !gone(r)) pend_add(e, node, now, timed, false);
  }
}

static bool is_event_node(const char *s) { return !strncmp(s, "event", 5) && s[5] >= '0' && s[5] <= '9'; }

static void hotplug(evdev_t *e, int64_t now) {
  take_results(e, now);
  if (e->ino_fd >= 0) {
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (int k = 0; k < 8; k++) {
      ssize_t n = read(e->ino_fd, buf, sizeof buf);
      if (n <= 0) break;
      for (char *p = buf; p + sizeof(struct inotify_event) <= buf + n; ) {
        const struct inotify_event *ev = (const struct inotify_event *)p;
        p += sizeof *ev + ev->len;
        if (ev->mask & IN_Q_OVERFLOW) { e->rescan = true; continue; }
        if (!ev->len || !is_event_node(ev->name)) continue;
        int i = dev_by_node(e, ev->name);
        if (ev->mask & IN_DELETE) {
          if (i >= 0) dev_remove(e, i, true);
          pend_drop(e, ev->name);
        } else if (ev->mask & IN_CREATE) {
          int ver;
          /* a node of that name we still have open is gone if its fd says so */
          if (i >= 0 && ioctl(e->dev[i].fd, EVIOCGVERSION, &ver) == 0) continue;
          if (i >= 0) dev_remove(e, i, true);
          pend_add(e, ev->name, now, true, true);
        } else if ((ev->mask & IN_ATTRIB) && i < 0) {
          pend_add(e, ev->name, now, true, false);   /* permissions came: try again now */
        }
      }
    }
  }
  if (e->rescan) { e->rescan = false; scan_dir(e, now, true, false); }
  for (int i = 0; i < MAX_PEND; i++) {
    pend_t *p = &e->pend[i];
    if (!p->used) continue;
    if (!p->timed) { p->timed = true; p->until_us = now + RETRY_FOR_US; p->next_us = now; }
    if (p->busy || now < p->next_us) continue;
    if (dev_by_node(e, p->node) >= 0) { p->used = false; continue; }
    if (e->th_ok) { request_open(e, p); continue; }   /* queue full: the next poll */
    int fd;
    evdev_caps_t c;
    int r = probe_node(e->dir, p->node, &fd, &c);
    pend_result(e, p, r, fd, &c, now);
  }
}

static void read_dev(evdev_t *e, int i) {
  edev_t *d = &e->dev[i];
  struct input_event ev[64];
  for (int k = 0; k < MAX_READS; k++) {
    ssize_t n = read(d->fd, ev, sizeof ev);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      dev_remove(e, i, true);              /* ENODEV: unplugged */
      return;
    }
    if (n == 0) { dev_remove(e, i, true); return; }
    int cnt = (int)((size_t)n / sizeof ev[0]);
    for (int j = 0; j < cnt && d->used; j++) dev_event(e, i, ev[j].type, ev[j].code, ev[j].value);
    if ((size_t)n < sizeof ev) return;
  }
}
#endif

/* ---------------------------------------------------------------- API */

evdev_t *evdev_open(const char *dir) {
  evdev_t *e = calloc(1, sizeof *e);
  if (!e) return NULL;
  snprintf(e->dir, sizeof e->dir, "%s", dir && *dir ? dir : "/dev/input");
  e->ino_fd = -1;
  e->mode = 2;
  e->ks[THR] = -1.0;
  e->kch[THR] = -1024;
  for (int s = 0; s < INPUT_MAX_SOURCES; s++) { e->owner[s] = -1; e->dirty[s] = s >= SLOT_EVDEV_FIRST; }
  e->kbd_dirty = true;
  for (int i = 0; i < MAX_DEVS; i++) e->dev[i].fd = -1;
#ifdef __linux__
  e->wake_fd = -1;
  pthread_mutex_init(&e->mu, NULL);
  pthread_cond_init(&e->cv, NULL);
  /* watch first, then scan: a device that appears in between is seen by one or the other */
  e->ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (e->ino_fd >= 0 && inotify_add_watch(e->ino_fd, e->dir, IN_CREATE | IN_ATTRIB | IN_DELETE) < 0) {
    close(e->ino_fd);
    e->ino_fd = -1;
  }
  scan_dir(e, 0, false, true);      /* here, so the devices are there at the first poll */
  bool any = false;
  for (int i = 0; i < MAX_DEVS; i++) any |= e->dev[i].used;
  /* nothing to watch and nothing open (no such folder): no thread; without one, all is done inline */
  if (e->ino_fd >= 0 || any) {
    e->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (e->wake_fd >= 0) {
      /* SIGINT/SIGTERM keep going to the other threads; a fault here still reaches the handler
       * that restores the console */
      sigset_t all, old;
      sigfillset(&all);
      static const int faults[] = { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP, SIGSYS };
      for (size_t i = 0; i < sizeof faults / sizeof faults[0]; i++) sigdelset(&all, faults[i]);
      pthread_sigmask(SIG_SETMASK, &all, &old);
      pthread_attr_t a;
      pthread_attr_init(&a);
      pthread_attr_setstacksize(&a, 256 * 1024);
      e->th_ok = pthread_create(&e->th, &a, worker, e) == 0;
      pthread_attr_destroy(&a);
      pthread_sigmask(SIG_SETMASK, &old, NULL);
      if (!e->th_ok) { close(e->wake_fd); e->wake_fd = -1; }
    }
  }
#endif
  return e;
}

static void sync_slots(evdev_t *e, insrc_t *src, int64_t now) {
  for (int s = SLOT_EVDEV_FIRST; s < INPUT_MAX_SOURCES; s++) {
    insrc_t *x = &src[s];
    int i = e->owner[s];
    edev_t *d = i >= 0 ? &e->dev[i] : NULL;
    if (e->dirty[s]) {
      e->dirty[s] = false;
      memset(x, 0, sizeof *x);
      if (d) {
        x->kind = d->kind;
        x->present = true;
        x->fixed_aetr = d->kind == INSRC_GAMEPAD;
        snprintf(x->name, sizeof x->name, "%s", d->name[0] ? d->name : kind_name(d->kind));
        x->nch = d->nch;
        memcpy(x->ch, d->ch, sizeof x->ch);
        d->changed = false;
      }
    }
    if (!d) continue;
    if (d->changed) { insrc_update(x, d->ch, d->nch, now); d->changed = false; }
    x->last_us = now;                     /* HID reports changes only: open = alive */
  }
}

static void approach(double *v, double t, double k) {
  *v += (t - *v) * k;
  if (t == 0 && fabs(*v) < 0.003) *v = 0;
}

/* the emulator's keyboardSticks(dt) (app.js:908-931), only while flying */
static void keyboard(evdev_t *e, insrc_t *src, int64_t now) {
  double dt = e->kbd_started ? (double)(now - e->kbd_us) / 1000.0 : 0;
  e->kbd_started = true;
  e->kbd_us = now;
  if (dt < 0) dt = 0;
  if (dt > 250) dt = 250;
  bool fly = e->flying;
  bool fast = held(e, KEY_LEFTSHIFT) || held(e, KEY_RIGHTSHIFT);
  double defl = fast ? 1 : 0.6, k = 1 - exp(-dt / 70);
#define TARGET(neg, pos) (fly ? ((pos) ? defl : 0) - ((neg) ? defl : 0) : 0)
  approach(&e->ks[AIL], TARGET(held(e, KEY_LEFT) || held(e, KEY_J), held(e, KEY_RIGHT) || held(e, KEY_L)), k);
  approach(&e->ks[ELE], TARGET(held(e, KEY_DOWN) || held(e, KEY_K), held(e, KEY_UP) || held(e, KEY_I)), k);
  approach(&e->ks[RUD], TARGET(held(e, KEY_A), held(e, KEY_D)), k);
#undef TARGET
  if (fly) {
    double step = dt / (fast ? 500 : 1400);
    if (held(e, KEY_W)) e->ks[THR] = e->ks[THR] + step > 1 ? 1 : e->ks[THR] + step;
    if (held(e, KEY_S)) e->ks[THR] = e->ks[THR] - step < -1 ? -1 : e->ks[THR] - step;
  }
  for (int i = 0; i < 4; i++) {
    double v = floor(e->ks[i] * 1024 + 0.5);   /* engine.js getValue: Math.round */
    e->kch[i] = (int16_t)(v > 1024 ? 1024 : v < -1024 ? -1024 : v);
  }
  if (!src) return;
  const edev_t *kd = NULL;
  for (int i = 0; i < MAX_DEVS && !kd; i++)
    if (e->dev[i].used && e->dev[i].kbd) kd = &e->dev[i];
  insrc_t *x = &src[SLOT_KEYBOARD];
  bool present = kd != NULL;
  if (present != e->kbd_present || e->kbd_dirty) {
    e->kbd_present = present;
    e->kbd_dirty = false;
    /* without a keyboard the slot keeps its shape (as host_init sets it), just not present */
    memset(x, 0, sizeof *x);
    x->kind = INSRC_KEYBOARD;
    x->present = present;
    x->fixed_aetr = true;
    x->nch = 4;
    memcpy(x->ch, e->kch, sizeof e->kch);
  }
  if (!present) return;
  const char *name = kd->name[0] ? kd->name : "keyboard";
  if (strcmp(x->name, name)) snprintf(x->name, sizeof x->name, "%s", name);
  if (memcmp(x->ch, e->kch, sizeof e->kch)) insrc_update(x, e->kch, 4, now);
  x->last_us = now;
}

int evdev_poll(evdev_t *e, insrc_t *src, int64_t now_us, bool flying, int *keys, int maxkeys) {
  if (!e) return 0;
  e->now_us = now_us;
  e->flying = flying;
#ifdef __linux__
  hotplug(e, now_us);
  for (int i = 0; i < MAX_DEVS; i++)
    if (e->dev[i].used && !e->dev[i].virt) read_dev(e, i);
#endif
  while (e->injn > 0) {
    inj_t v = e->inj[e->injhead];
    e->injhead = (e->injhead + 1) % INJ_LEN;
    e->injn--;
    if (e->dev[v.dev].used && e->dev[v.dev].gen == v.gen) dev_event(e, v.dev, v.type, v.code, v.value);
  }
  if (e->exit_down && !e->exit_long && now_us - e->exit_us > LONG_EXIT_US) {
    e->exit_long = true;
    push(e, EVDEV_KEY_RELOAD);
  }
  assign_slots(e);
  if (src) sync_slots(e, src, now_us);
  keyboard(e, src, now_us);
  int n = 0;
  while (keys && n < maxkeys && e->qn > 0) {
    keys[n++] = e->q[e->qhead];
    e->qhead = (e->qhead + 1) % QLEN;
    e->qn--;
  }
  return n;
}

int evdev_fds(evdev_t *e, int *fds, int max) {
  int n = 0;
  if (!e || !fds) return 0;
#ifdef __linux__
  if (e->ino_fd >= 0 && n < max) fds[n++] = e->ino_fd;
  if (e->wake_fd >= 0 && n < max) fds[n++] = e->wake_fd;
  for (int i = 0; i < MAX_DEVS && n < max; i++)
    if (e->dev[i].used && !e->dev[i].virt && e->dev[i].fd >= 0) fds[n++] = e->dev[i].fd;
#else
  (void)max;
#endif
  return n;
}

void evdev_close(evdev_t *e) {
  if (!e) return;
#ifdef __linux__
  if (e->th_ok) {
    /* the worker finishes the closes it has and skips the opens */
    pthread_mutex_lock(&e->mu);
    e->th_stop = true;
    pthread_cond_signal(&e->cv);
    pthread_mutex_unlock(&e->mu);
    pthread_join(e->th, NULL);
    e->th_ok = false;
    for (; e->resn > 0; e->resn--, e->reshead = (e->reshead + 1) % MAX_RES)
      if (e->res[e->reshead].fd >= 0) close(e->res[e->reshead].fd);
  }
#endif
  for (int i = 0; i < MAX_DEVS; i++) dev_remove(e, i, false);   /* shutting down: closes inline */
#ifdef __linux__
  if (e->ino_fd >= 0) close(e->ino_fd);
  if (e->wake_fd >= 0) close(e->wake_fd);
  pthread_cond_destroy(&e->cv);
  pthread_mutex_destroy(&e->mu);
#endif
  free(e);
}

void evdev_set_stick_mode(evdev_t *e, int mode) {
  if (!e) return;
  if (mode < 1 || mode > 4) mode = 2;
  if (mode == e->mode) return;
  e->mode = mode;
  for (int i = 0; i < MAX_DEVS; i++)
    if (e->dev[i].used && e->dev[i].kind == INSRC_GAMEPAD) dev_map(e, &e->dev[i]);
}

/* ---------------------------------------------------------------- virtual devices */

static edev_t *vdev(evdev_t *e, int id) {
  if (!e || id < 0 || id >= MAX_DEVS || !e->dev[id].used || !e->dev[id].virt) return NULL;
  return &e->dev[id];
}

static void vstate(edev_t *d, int type, int code, int value) {
  if (type == EV_ABS && code >= 0 && code < EVDEV_NABS) d->vabs[code] = value;
  else if (type == EV_KEY && code >= 0 && code < EVDEV_NKEY) setbit(d->vkeys, code, value != 0);
}

int evdev_add_virtual(evdev_t *e, const evdev_caps_t *caps) {
  if (!e || !caps) return -1;
  return dev_add(e, caps, -1, "virtual", true);
}

void evdev_remove_virtual(evdev_t *e, int id) {
  if (vdev(e, id)) dev_remove(e, id, false);
}

bool evdev_inject(evdev_t *e, int id, int type, int code, int value) {
  edev_t *d = vdev(e, id);
  if (!d) return false;
  vstate(d, type, code, value);
  if (e->injn == INJ_LEN) return false;
  inj_t *v = &e->inj[(e->injhead + e->injn) % INJ_LEN];
  v->dev = (int16_t)id;
  v->gen = d->gen;
  v->type = (uint16_t)type;
  v->code = (uint16_t)code;
  v->value = value;
  e->injn++;
  return true;
}

void evdev_set_state(evdev_t *e, int id, int type, int code, int value) {
  edev_t *d = vdev(e, id);
  if (d) vstate(d, type, code, value);
}
