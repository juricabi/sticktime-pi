/* Joysticks, gamepads and keyboards from /dev/input/event* (Linux; a stub elsewhere).
 *
 * Classification (first match):
 *   name contains "EdgeTX" or "OpenTX"                 -> INSRC_USB: ABS codes 0..7 = CH1..CH8
 *   name "ELRS Joystick" / "ExpressLRS Joystick", or
 *   a Bluetooth joystick (bus BUS_BLUETOOTH)           -> INSRC_BLUETOOTH: X=CH1 Y=CH2 RY=CH3 RZ=CH4 Z=CH5 RX=CH6
 *                                                         THROTTLE=CH7 RUDDER (or WHEEL)=CH8 (ELRS 4.x BLE);
 *                                                         a name with "ExpressLRS" (ELRS 3.x): RX=CH3 RY=CH4
 *                                                         RZ=CH6; buttons 1-8 = CH9-CH16: pressed = +1024
 *   has BTN_GAMEPAD                                     -> INSRC_GAMEPAD (fixed AETR, mode 2): thr = -ABS_Y,
 *                                                         rud = ABS_X, ele = -ABS_RY, ail = ABS_RX; CH5 = BTN_TL/TR toggles...
 *                                                         Start/Select -> EXIT (pause / back), A -> ENTER, B -> EXIT,
 *                                                         D-pad -> PREV/NEXT
 *   has ABS_X and ABS_Y                                 -> INSRC_JOYSTICK: absolute axes in code order = CH1..
 *   has KEY_ENTER or KEY_A                              -> keyboard: keys -> events (see below)
 * Axis values are scaled from the device's absinfo min..max to -1024..1024 (center = midpoint,
 * flat/fuzz ignored: the radio sends calibrated channels).
 * Keyboard: Up/Down = PREV/NEXT, Enter/KP_Enter/Space = ENTER, Esc/Backspace = EXIT,
 * +/= and KP_Plus = NEXT(INC), - and KP_Minus = PREV(DEC), PageUp/PageDown = PREV_PAGE/NEXT_PAGE,
 * M = MENU. Keyboard flying (slot INSRC_KEYBOARD, fixed AETR): W/S throttle up/down (stays),
 * A/D rudder, arrow keys or I/K/J/L pitch/roll while held (only while the game is flying).
 * Hotplug: inotify on the directory; devices that vanish (ENODEV) are closed and their slot freed. */
#ifndef INPUT_EVDEV_H
#define INPUT_EVDEV_H

#include <stdint.h>
#include <stdbool.h>
#include "input.h"

typedef struct evdev evdev_t;

/* Source slots: the host's src[] array is shared by the input modules. */
#define SLOT_KEYBOARD 0
#define SLOT_WIFI 1
#define SLOT_EVDEV_FIRST 2         /* evdev devices use slots 2 .. INPUT_MAX_SOURCES-1 */

/* Open every input device in dir (NULL = "/dev/input") and start watching it.
 * Returns NULL only on allocation failure (no devices is fine). */
evdev_t *evdev_open(const char *dir);
/* Drain all pending events without blocking. Updates src[SLOT_KEYBOARD] and the evdev slots
 * (present, kind, name, channels), appends key events (ETX_EVT_*) to keys (up to maxkeys).
 * flying: the game is in flight (keyboard sticks active, arrow keys fly instead of navigating).
 * Returns the number of key events. */
int  evdev_poll(evdev_t *e, insrc_t *src, int64_t now_us, bool flying, int *keys, int maxkeys);
/* fds to poll (for a loop that sleeps in poll()): returns count written to fds */
int  evdev_fds(evdev_t *e, int *fds, int max);
void evdev_close(evdev_t *e);

/* ---- pure helpers (unit tests) ---- */
int16_t evdev_scale_axis(int value, int min, int max);       /* -> -1024..1024, clamped */
/* classify from name, bus and capability bits; returns INSRC_* or INSRC_NONE (keyboard-only devices
 * return INSRC_KEYBOARD) */
insrc_kind_t evdev_classify(const char *name, int bus, bool has_gamepad_btn, bool has_abs_xy, bool has_keys);

/* ---- additions (input_evdev.c) ----
 * Special key codes from evdev_poll (negative, never game events; the host acts on them):
 *   EVDEV_KEY_RELOAD  R pressed, or Esc/Backspace held > 900 ms: reload the game (the emulator
 *                     closes the tool on a long EXIT; no EXIT is sent for that press)
 *   EVDEV_KEY_UP/DOWN Up/Down arrow (not flying) and gamepad D-pad up/down: the host turns them
 *                     into the editing-aware event with nav_pitch_event(&nav, -1 / +1)
 * Keyboard details (emulator app.js:867-931): Esc/Backspace send EXIT on release; Left/Right =
 * PREV/NEXT (0x1D00/0x1E00) and Up/Down are only events while not flying (in flight they fly);
 * auto-repeat (value 2) sends nothing; presses with Ctrl/Alt/Meta held send nothing. TV remotes:
 * KEY_OK/KEY_SELECT = Enter, KEY_BACK/KEY_EXIT = Esc. Keyboard flying is the emulator's: axes
 * approach +-0.6 (+-1 with Shift) with tau 70 ms and recentre, throttle sweeps in 2.8 s (1 s with
 * Shift) and stays, starting at -1024.
 * Gamepads (sticks placed by evdev_set_stick_mode, mode 2 by default; right stick = RX/RY, or Z/RZ
 * when the pad has no RX/RY): CH5 = BTN_TL/BTN_TR toggle (-1024 / +1024), CH6/CH7 = left/right
 * trigger (Z/RZ, else BRAKE/GAS), a 2% dead zone on ail/ele/rud; A (BTN_SOUTH) = ENTER, B (BTN_EAST),
 * Start, Select = EXIT, D-pad (hat or BTN_DPAD_*) up/down = EVDEV_KEY_UP/DOWN, left/right = PREV/NEXT.
 * Radios: buttons (in code order) 1-8 = CH9-CH16 (pressed +1024, else -1024) for USB and Bluetooth;
 * generic joysticks: axes in code order (no hats), then buttons, 2% dead zone on the axes.
 * Pointers, touch screens/pads and motion sensors (INPUT_PROP_POINTER/DIRECT/ACCELEROMETER,
 * BTN_TOUCH, BTN_LEFT, BTN_TOOL_*) never count as joysticks. Devices that are neither joystick nor
 * keyboard are not kept open. A joystick with keyboard keys is both: its sticks use its own slot,
 * its keys act as keyboard keys. A present evdev source is live (last_us = now every poll): HID
 * only reports changes, so a radio held still is silent; unplugging (or BlueZ dropping the link)
 * frees the slot. src[SLOT_KEYBOARD] is present while any keyboard is attached.
 * Linux threading: evdev_open opens the devices already there before it returns. After that,
 * opening new nodes and closing old ones runs on a worker thread that evdev_open starts (open()
 * sleeps 50 ms in usbhid for a joystick, close() 30-60 ms for RCU), so evdev_poll never blocks and
 * a new device shows up a poll or two after its node. evdev_fds includes the worker's eventfd;
 * evdev_close joins the thread. A node that can't be opened yet (EACCES before udev's chmod) is
 * retried every 100 ms for 2 s, and again whenever its attributes change (IN_ATTRIB). */
#define EVDEV_KEY_RELOAD (-1)
#define EVDEV_KEY_UP     (-2)
#define EVDEV_KEY_DOWN   (-3)

/* stick mode 1..4 for gamepads (the stick_mode setting); radios are not affected */
void evdev_set_stick_mode(evdev_t *e, int mode);

/* Capabilities and state of a device, as read with the EVIOCG* ioctls (bit n of key[] = key code n,
 * Linux codes). Used to add virtual devices (unit tests, scenarios) and by evdev_classify_caps. */
#define EVDEV_NKEY  0x300          /* KEY_CNT */
#define EVDEV_NABS  0x40           /* ABS_CNT */
#define EVDEV_NPROP 0x20           /* INPUT_PROP_CNT */
typedef struct {
  char name[64];
  int bus;                         /* BUS_USB 3, BUS_BLUETOOTH 5, ... */
  uint8_t key[EVDEV_NKEY / 8];     /* EVIOCGBIT(EV_KEY) */
  uint8_t abs[EVDEV_NABS / 8];     /* EVIOCGBIT(EV_ABS) */
  uint8_t prop[EVDEV_NPROP / 8];   /* EVIOCGPROP */
  int absmin[EVDEV_NABS], absmax[EVDEV_NABS], absval[EVDEV_NABS];   /* EVIOCGABS */
  uint8_t keystate[EVDEV_NKEY / 8];   /* EVIOCGKEY: keys held now */
} evdev_caps_t;

/* evdev_classify on the capability bits (with the pointer/sensor exclusions above).
 * *keyboard (may be NULL) = the device also has keyboard keys (KEY_ENTER, KEY_A, KEY_OK, KEY_SELECT). */
insrc_kind_t evdev_classify_caps(const evdev_caps_t *c, bool *keyboard);

/* Virtual devices go through the same code as kernel ones. add returns an id >= 0, or -1 when the
 * device is full or not an input we use. inject queues one event for the next evdev_poll (false when
 * the queue is full) and updates the device's "kernel" state; set_state changes that state without
 * an event (an event lost before a SYN_DROPPED). */
int  evdev_add_virtual(evdev_t *e, const evdev_caps_t *caps);
void evdev_remove_virtual(evdev_t *e, int id);
bool evdev_inject(evdev_t *e, int id, int type, int code, int value);
void evdev_set_state(evdev_t *e, int id, int type, int code, int value);

#endif
