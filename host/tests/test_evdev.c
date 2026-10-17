/* input_evdev: axis scaling, classification, and the device logic through virtual devices
 * (channel maps, gamepad buttons, keyboard events and flying, slots, SYN_DROPPED resync). */
#include "t.h"
#include "input_evdev.h"
#include "etx.h"

#include <math.h>
#include <limits.h>

#ifdef __linux__
#include <linux/input.h>
#else
#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_ABS 0x03
#define SYN_REPORT 0
#define SYN_DROPPED 3
#define BUS_USB 0x03
#define BUS_BLUETOOTH 0x05
#define BUS_HOST 0x19
#define INPUT_PROP_POINTER 0x00
#define INPUT_PROP_DIRECT 0x01
#define INPUT_PROP_ACCELEROMETER 0x06
#define KEY_ESC 1
#define KEY_MINUS 12
#define KEY_EQUAL 13
#define KEY_BACKSPACE 14
#define KEY_Q 16
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
#define KEY_POWER 116
#define KEY_LEFTMETA 125
#define KEY_BACK 158
#define KEY_EXIT 174
#define KEY_OK 0x160
#define KEY_SELECT 0x161
#define BTN_LEFT 0x110
#define BTN_TRIGGER 0x120
#define BTN_SOUTH 0x130
#define BTN_EAST 0x131
#define BTN_NORTH 0x133
#define BTN_WEST 0x134
#define BTN_TL 0x136
#define BTN_TR 0x137
#define BTN_SELECT 0x13a
#define BTN_START 0x13b
#define BTN_MODE 0x13c
#define BTN_THUMBL 0x13d
#define BTN_THUMBR 0x13e
#define BTN_TOOL_PEN 0x140
#define BTN_TOOL_FINGER 0x145
#define BTN_TOUCH 0x14a
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
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#endif

/* ---------------------------------------------------------------- helpers */

static insrc_t src[INPUT_MAX_SOURCES];
static int keys[256];
static int64_t now;

static void setb(uint8_t *b, int n) { b[n >> 3] |= (uint8_t)(1u << (n & 7)); }

static evdev_caps_t caps(const char *name, int bus) {
  evdev_caps_t c;
  memset(&c, 0, sizeof c);
  snprintf(c.name, sizeof c.name, "%s", name);
  c.bus = bus;
  return c;
}
static void cap_abs(evdev_caps_t *c, int code, int min, int max, int val) {
  setb(c->abs, code);
  c->absmin[code] = min;
  c->absmax[code] = max;
  c->absval[code] = val;
}
static void cap_key(evdev_caps_t *c, int code) { setb(c->key, code); }
static void cap_keys(evdev_caps_t *c, int from, int to) { for (int k = from; k <= to; k++) setb(c->key, k); }

static evdev_caps_t dev_edgetx(void) {
  evdev_caps_t c = caps("EdgeTX RadioMaster TX16S Joystick", BUS_USB);
  for (int a = 0; a < 8; a++) cap_abs(&c, a, 0, 2047, 1024);
  cap_keys(&c, BTN_TRIGGER, BTN_TRIGGER + 15);             /* 24 buttons: 16 joystick ones ... */
  cap_keys(&c, BTN_TRIGGER_HAPPY, BTN_TRIGGER_HAPPY + 7);  /* ... and 8 past the 16th */
  return c;
}
static evdev_caps_t dev_elrs(const char *name) {
  evdev_caps_t c = caps(name, BUS_BLUETOOTH);
  static const int ax[7] = { ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_THROTTLE };
  for (int i = 0; i < 7; i++) cap_abs(&c, ax[i], 0, 32767, 16384);
  cap_keys(&c, BTN_SOUTH, BTN_TR);                         /* BLE gamepad report: buttons 1-8 */
  return c;
}
static evdev_caps_t dev_xbox(void) {                       /* xpad */
  evdev_caps_t c = caps("Microsoft X-Box One S pad", BUS_USB);
  cap_abs(&c, ABS_X, -32768, 32767, 0);
  cap_abs(&c, ABS_Y, -32768, 32767, 0);
  cap_abs(&c, ABS_RX, -32768, 32767, 0);
  cap_abs(&c, ABS_RY, -32768, 32767, 0);
  cap_abs(&c, ABS_Z, 0, 1023, 0);
  cap_abs(&c, ABS_RZ, 0, 1023, 0);
  cap_abs(&c, ABS_HAT0X, -1, 1, 0);
  cap_abs(&c, ABS_HAT0Y, -1, 1, 0);
  cap_key(&c, BTN_SOUTH); cap_key(&c, BTN_EAST); cap_key(&c, BTN_NORTH); cap_key(&c, BTN_WEST);
  cap_key(&c, BTN_TL); cap_key(&c, BTN_TR); cap_key(&c, BTN_SELECT); cap_key(&c, BTN_START);
  cap_key(&c, BTN_MODE); cap_key(&c, BTN_THUMBL); cap_key(&c, BTN_THUMBR);
  return c;
}
static evdev_caps_t dev_xbox_bt(void) {                    /* hid-generic over Bluetooth */
  evdev_caps_t c = caps("Xbox Wireless Controller", BUS_BLUETOOTH);
  cap_abs(&c, ABS_X, 0, 65535, 32768);
  cap_abs(&c, ABS_Y, 0, 65535, 32768);
  cap_abs(&c, ABS_Z, 0, 65535, 32768);
  cap_abs(&c, ABS_RZ, 0, 65535, 32768);
  cap_abs(&c, ABS_BRAKE, 0, 1023, 0);
  cap_abs(&c, ABS_GAS, 0, 1023, 0);
  cap_abs(&c, ABS_HAT0X, -1, 1, 0);
  cap_abs(&c, ABS_HAT0Y, -1, 1, 0);
  cap_keys(&c, BTN_SOUTH, BTN_THUMBR);
  cap_key(&c, KEY_BACK);
  return c;
}
static evdev_caps_t dev_ds4(void) {
  evdev_caps_t c = caps("Sony Interactive Entertainment Wireless Controller", BUS_USB);
  static const int ax[6] = { ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ };
  for (int i = 0; i < 6; i++) cap_abs(&c, ax[i], 0, 255, i == 2 || i == 5 ? 0 : 128);
  cap_abs(&c, ABS_HAT0X, -1, 1, 0);
  cap_abs(&c, ABS_HAT0Y, -1, 1, 0);
  cap_keys(&c, BTN_SOUTH, BTN_THUMBR);
  return c;
}
static evdev_caps_t dev_extreme3d(void) {
  evdev_caps_t c = caps("Logitech Logitech Extreme 3D", BUS_USB);
  cap_abs(&c, ABS_X, 0, 1023, 512);
  cap_abs(&c, ABS_Y, 0, 1023, 512);
  cap_abs(&c, ABS_RZ, 0, 255, 128);
  cap_abs(&c, ABS_THROTTLE, 0, 255, 0);
  cap_abs(&c, ABS_HAT0X, -1, 1, 0);
  cap_abs(&c, ABS_HAT0Y, -1, 1, 0);
  cap_keys(&c, BTN_TRIGGER, BTN_TRIGGER + 11);
  return c;
}
static evdev_caps_t dev_keyboard(const char *name) {
  evdev_caps_t c = caps(name, BUS_USB);
  cap_keys(&c, 1, 127);
  return c;
}
static evdev_caps_t dev_touchpad(void) {
  evdev_caps_t c = caps("SynPS/2 Synaptics TouchPad", 0x11);
  cap_abs(&c, ABS_X, 0, 4000, 0);
  cap_abs(&c, ABS_Y, 0, 3000, 0);
  cap_abs(&c, ABS_MT_POSITION_X, 0, 4000, 0);
  cap_abs(&c, ABS_MT_POSITION_Y, 0, 3000, 0);
  cap_key(&c, BTN_LEFT); cap_key(&c, BTN_TOUCH); cap_key(&c, BTN_TOOL_FINGER);
  setb(c.prop, INPUT_PROP_POINTER);
  return c;
}

static evdev_t *open_e(void) {
  memset(src, 0, sizeof src);
  now = 1000000;
  /* a folder with no event nodes: only virtual devices, also on Linux */
  evdev_t *e = evdev_open("host/tests/no-such-input-dir");
  CHECK(e != NULL);
  return e;
}
static int poll_e(evdev_t *e, bool fly) { return evdev_poll(e, src, now, fly, keys, 256); }
static void ev(evdev_t *e, int id, int type, int code, int value) { CHECK(evdev_inject(e, id, type, code, value)); }
static void syn(evdev_t *e, int id) { ev(e, id, EV_SYN, SYN_REPORT, 0); }
static void absv(evdev_t *e, int id, int code, int value) { ev(e, id, EV_ABS, code, value); syn(e, id); }
static void keyv(evdev_t *e, int id, int code, int value) { ev(e, id, EV_KEY, code, value); syn(e, id); }
/* press (and release) a key, poll, return the single event (0 none, -999 more than one) */
static int tap(evdev_t *e, int id, int code, bool fly) {
  keyv(e, id, code, 1);
  keyv(e, id, code, 0);
  int n = poll_e(e, fly);
  return n == 0 ? 0 : n == 1 ? keys[0] : -999;
}
static int slot_named(const char *name) {
  for (int s = SLOT_EVDEV_FIRST; s < INPUT_MAX_SOURCES; s++)
    if (src[s].present && !strcmp(src[s].name, name)) return s;
  return -1;
}

/* ---------------------------------------------------------------- scaling */

static void test_scale(void) {
  /* EdgeTX: 0..2048 with the center at 1024 */
  CHECK_INT(evdev_scale_axis(0, 0, 2048), -1024);
  CHECK_INT(evdev_scale_axis(1024, 0, 2048), 0);
  CHECK_INT(evdev_scale_axis(2048, 0, 2048), 1024);
  CHECK_INT(evdev_scale_axis(512, 0, 2048), -512);
  CHECK_INT(evdev_scale_axis(1536, 0, 2048), 512);
  /* odd range: both middle values are 0 (toward zero), the ends are full */
  CHECK_INT(evdev_scale_axis(1023, 0, 2047), 0);
  CHECK_INT(evdev_scale_axis(1024, 0, 2047), 0);
  CHECK_INT(evdev_scale_axis(1025, 0, 2047), 1);
  CHECK_INT(evdev_scale_axis(1022, 0, 2047), -1);
  CHECK_INT(evdev_scale_axis(0, 0, 2047), -1024);
  CHECK_INT(evdev_scale_axis(2047, 0, 2047), 1024);
  /* 8-bit pad */
  CHECK_INT(evdev_scale_axis(128, 0, 255), 4);
  CHECK_INT(evdev_scale_axis(127, 0, 255), -4);
  CHECK_INT(evdev_scale_axis(0, 0, 255), -1024);
  CHECK_INT(evdev_scale_axis(255, 0, 255), 1024);
  /* signed 16-bit */
  CHECK_INT(evdev_scale_axis(-32768, -32768, 32767), -1024);
  CHECK_INT(evdev_scale_axis(32767, -32768, 32767), 1024);
  CHECK_INT(evdev_scale_axis(0, -32768, 32767), 0);
  CHECK_INT(evdev_scale_axis(-1, -32768, 32767), 0);
  CHECK_INT(evdev_scale_axis(16384, -32768, 32767), 512);
  /* symmetric range */
  CHECK_INT(evdev_scale_axis(-512, -512, 512), -1024);
  CHECK_INT(evdev_scale_axis(256, -512, 512), 512);
  /* hats */
  CHECK_INT(evdev_scale_axis(-1, -1, 1), -1024);
  CHECK_INT(evdev_scale_axis(0, -1, 1), 0);
  CHECK_INT(evdev_scale_axis(1, -1, 1), 1024);
  /* outside the range: clamped */
  CHECK_INT(evdev_scale_axis(2100, 0, 2047), 1024);
  CHECK_INT(evdev_scale_axis(-50, 0, 2047), -1024);
  CHECK_INT(evdev_scale_axis(INT_MAX, 0, 2047), 1024);
  CHECK_INT(evdev_scale_axis(INT_MIN, 0, 2047), -1024);
  /* min == max: no range, always centered */
  CHECK_INT(evdev_scale_axis(0, 0, 0), 0);
  CHECK_INT(evdev_scale_axis(500, 7, 7), 0);
  CHECK_INT(evdev_scale_axis(INT_MIN, INT_MAX, INT_MAX), 0);
  /* min > max: still the linear map (min -> -1024, max -> +1024) */
  CHECK_INT(evdev_scale_axis(100, 100, 0), -1024);
  CHECK_INT(evdev_scale_axis(0, 100, 0), 1024);
  CHECK_INT(evdev_scale_axis(50, 100, 0), 0);
  CHECK_INT(evdev_scale_axis(25, 100, 0), 512);
  /* the widest range does not overflow */
  CHECK_INT(evdev_scale_axis(INT_MAX, INT_MIN, INT_MAX), 1024);
  CHECK_INT(evdev_scale_axis(INT_MIN, INT_MIN, INT_MAX), -1024);
  CHECK_INT(evdev_scale_axis(0, INT_MIN, INT_MAX), 0);
  CHECK_INT(evdev_scale_axis(INT_MAX / 2, INT_MIN, INT_MAX), 511);   /* 511.9999998 */
  CHECK_INT(evdev_scale_axis(INT_MAX / 2 + 1, INT_MIN, INT_MAX), 512);
  /* tiny range */
  CHECK_INT(evdev_scale_axis(0, 0, 1), -1024);
  CHECK_INT(evdev_scale_axis(1, 0, 1), 1024);
  /* monotonic, in range and odd-symmetric over whole ranges */
  static const int ranges[][2] = { { 0, 2047 }, { 0, 2048 }, { 0, 255 }, { -32768, 32767 }, { -100, 37 }, { 3, 4 } };
  for (size_t r = 0; r < sizeof ranges / sizeof ranges[0]; r++) {
    int lo = ranges[r][0], hi = ranges[r][1], step = (hi - lo) > 5000 ? 7 : 1;
    int prev = -2000, bad_mono = 0, bad_sym = 0, bad_range = 0;
    for (int v = lo; v <= hi; v += step) {
      int s = evdev_scale_axis(v, lo, hi);
      if (s < prev) bad_mono++;
      if (s < -1024 || s > 1024) bad_range++;
      if (s != -evdev_scale_axis(lo + hi - v, lo, hi)) bad_sym++;
      prev = s;
    }
    CHECK_INT(bad_mono, 0);
    CHECK_INT(bad_sym, 0);
    CHECK_INT(bad_range, 0);
    CHECK_INT(evdev_scale_axis(lo, lo, hi), -1024);
    CHECK_INT(evdev_scale_axis(hi, lo, hi), 1024);
  }
}

/* ---------------------------------------------------------------- classification */

static void test_classify(void) {
  /* name, bus, gamepad button, abs x/y, keyboard keys */
  CHECK_INT(evdev_classify("EdgeTX RadioMaster TX16S Joystick", BUS_USB, false, true, false), INSRC_USB);
  CHECK_INT(evdev_classify("OpenTX Taranis Joystick", BUS_USB, false, true, false), INSRC_USB);
  CHECK_INT(evdev_classify("edgetx jumper t-pro", BUS_USB, false, true, false), INSRC_USB);
  CHECK_INT(evdev_classify("FrSky EDGETX X10", BUS_USB, false, true, false), INSRC_USB);
  /* old kernels spill joystick buttons past 16 into the gamepad range: the name still wins */
  CHECK_INT(evdev_classify("EdgeTX RadioMaster TX16S Joystick", BUS_USB, true, true, false), INSRC_USB);
  /* a radio in advanced mode without X/Y is still the radio */
  CHECK_INT(evdev_classify("EdgeTX Zorro Joystick", BUS_USB, false, false, false), INSRC_USB);
  CHECK_INT(evdev_classify("ELRS Joystick", BUS_BLUETOOTH, false, true, false), INSRC_BLUETOOTH);
  CHECK_INT(evdev_classify("ExpressLRS Joystick", BUS_BLUETOOTH, false, true, false), INSRC_BLUETOOTH);
  /* the BLE report is a gamepad (buttons in the gamepad range): the name wins */
  CHECK_INT(evdev_classify("ELRS Joystick", BUS_BLUETOOTH, true, true, false), INSRC_BLUETOOTH);
  CHECK_INT(evdev_classify("elrs joystick", BUS_USB, false, true, false), INSRC_BLUETOOTH);
  CHECK_INT(evdev_classify("expresslrs", BUS_BLUETOOTH, true, true, false), INSRC_BLUETOOTH);
  CHECK_INT(evdev_classify("My-ELRS BLE", BUS_BLUETOOTH, true, true, false), INSRC_BLUETOOTH);
  /* "elrs" only where a word starts */
  CHECK_INT(evdev_classify("Shelrs Pad", BUS_USB, true, true, false), INSRC_GAMEPAD);
  CHECK_INT(evdev_classify("Wireless Controller", BUS_BLUETOOTH, true, true, false), INSRC_GAMEPAD);
  /* any other Bluetooth joystick reads like the ELRS one; a Bluetooth gamepad is a gamepad */
  CHECK_INT(evdev_classify("BT Flight Stick", BUS_BLUETOOTH, false, true, false), INSRC_BLUETOOTH);
  CHECK_INT(evdev_classify("Xbox Wireless Controller", BUS_BLUETOOTH, true, true, false), INSRC_GAMEPAD);
  CHECK_INT(evdev_classify("Xbox Wireless Controller", BUS_USB, true, true, false), INSRC_GAMEPAD);
  CHECK_INT(evdev_classify("Sony Interactive Entertainment Wireless Controller", BUS_USB, true, true, false), INSRC_GAMEPAD);
  CHECK_INT(evdev_classify("Sony Interactive Entertainment Wireless Controller", BUS_BLUETOOTH, true, true, false), INSRC_GAMEPAD);
  CHECK_INT(evdev_classify("8BitDo SN30 Pro", BUS_USB, true, false, false), INSRC_GAMEPAD);
  CHECK_INT(evdev_classify("Logitech Logitech Extreme 3D", BUS_USB, false, true, false), INSRC_JOYSTICK);
  CHECK_INT(evdev_classify("Logitech USB Keyboard", BUS_USB, false, false, true), INSRC_KEYBOARD);
  CHECK_INT(evdev_classify("Apple Magic Keyboard", BUS_BLUETOOTH, false, false, true), INSRC_KEYBOARD);
  /* a keyboard named like a radio is a keyboard; one with sticks is the radio */
  CHECK_INT(evdev_classify("EdgeTX Keyboard", BUS_USB, false, false, true), INSRC_KEYBOARD);
  CHECK_INT(evdev_classify("ELRS Keyboard", BUS_BLUETOOTH, false, false, true), INSRC_KEYBOARD);
  CHECK_INT(evdev_classify("EdgeTX Combo", BUS_USB, false, true, true), INSRC_USB);
  /* joystick + keys: the stick kind (the keys are handled on the side) */
  CHECK_INT(evdev_classify("Combo Pad Keyboard", BUS_USB, false, true, true), INSRC_JOYSTICK);
  CHECK_INT(evdev_classify("Power Button", BUS_HOST, false, false, false), INSRC_NONE);
  CHECK_INT(evdev_classify("", 0, false, false, false), INSRC_NONE);
  CHECK_INT(evdev_classify(NULL, 0, false, true, false), INSRC_JOYSTICK);
  CHECK_INT(evdev_classify(NULL, 0, false, false, false), INSRC_NONE);
}

static void test_classify_caps(void) {
  bool kb = true;
  evdev_caps_t c = dev_edgetx();
  CHECK_INT(evdev_classify_caps(&c, &kb), INSRC_USB); CHECK(!kb);
  c = dev_elrs("ELRS Joystick");
  CHECK_INT(evdev_classify_caps(&c, &kb), INSRC_BLUETOOTH); CHECK(!kb);
  c = dev_xbox();
  CHECK_INT(evdev_classify_caps(&c, &kb), INSRC_GAMEPAD); CHECK(!kb);
  c = dev_xbox_bt();
  CHECK_INT(evdev_classify_caps(&c, &kb), INSRC_GAMEPAD); CHECK(!kb);   /* KEY_BACK alone is no keyboard */
  c = dev_ds4();
  CHECK_INT(evdev_classify_caps(&c, NULL), INSRC_GAMEPAD);
  c = dev_extreme3d();
  CHECK_INT(evdev_classify_caps(&c, &kb), INSRC_JOYSTICK); CHECK(!kb);
  c = dev_keyboard("Logitech USB Keyboard");
  CHECK_INT(evdev_classify_caps(&c, &kb), INSRC_KEYBOARD); CHECK(kb);
  /* pointers and sensors are never sticks */
  c = dev_touchpad();
  CHECK_INT(evdev_classify_caps(&c, &kb), INSRC_NONE); CHECK(!kb);
  c = caps("Sony Interactive Entertainment Wireless Controller Motion Sensors", BUS_BLUETOOTH);
  for (int a = ABS_X; a <= ABS_RZ; a++) cap_abs(&c, a, -32768, 32767, 0);
  setb(c.prop, INPUT_PROP_ACCELEROMETER);
  CHECK_INT(evdev_classify_caps(&c, &kb), INSRC_NONE);
  c = caps("QEMU QEMU USB Tablet", BUS_USB);
  cap_abs(&c, ABS_X, 0, 32767, 0); cap_abs(&c, ABS_Y, 0, 32767, 0); cap_key(&c, BTN_LEFT);
  CHECK_INT(evdev_classify_caps(&c, &kb), INSRC_NONE);
  c = caps("raspberrypi-ts", BUS_HOST);
  cap_abs(&c, ABS_X, 0, 799, 0); cap_abs(&c, ABS_Y, 0, 479, 0); cap_key(&c, BTN_TOUCH);
  setb(c.prop, INPUT_PROP_DIRECT);
  CHECK_INT(evdev_classify_caps(&c, &kb), INSRC_NONE);
  c = caps("Wacom Intuos", BUS_USB);
  cap_abs(&c, ABS_X, 0, 15200, 0); cap_abs(&c, ABS_Y, 0, 9500, 0); cap_key(&c, BTN_TOOL_PEN);
  CHECK_INT(evdev_classify_caps(&c, &kb), INSRC_NONE);
  c = caps("Some Touch Screen", BUS_USB);
  cap_abs(&c, ABS_X, 0, 4095, 0); cap_abs(&c, ABS_Y, 0, 4095, 0); setb(c.prop, INPUT_PROP_POINTER);
  CHECK_INT(evdev_classify_caps(&c, &kb), INSRC_NONE);
  /* a Bluetooth touchpad is not an ELRS joystick */
  c = dev_touchpad(); c.bus = BUS_BLUETOOTH;
  CHECK_INT(evdev_classify_caps(&c, &kb), INSRC_NONE);
  c = caps("Power Button", BUS_HOST);
  cap_key(&c, KEY_POWER);
  CHECK_INT(evdev_classify_caps(&c, &kb), INSRC_NONE); CHECK(!kb);
  c = caps("Logitech USB Optical Mouse", BUS_USB);
  cap_key(&c, BTN_LEFT);
  CHECK_INT(evdev_classify_caps(&c, &kb), INSRC_NONE);
  /* a TV remote (HDMI CEC) is a keyboard */
  c = caps("vc4-hdmi", 0x1e);
  cap_key(&c, KEY_OK); cap_key(&c, KEY_UP); cap_key(&c, KEY_DOWN); cap_key(&c, KEY_EXIT);
  CHECK_INT(evdev_classify_caps(&c, &kb), INSRC_KEYBOARD); CHECK(kb);
  c = caps("Remote", 0); cap_key(&c, KEY_SELECT);
  CHECK_INT(evdev_classify_caps(&c, &kb), INSRC_KEYBOARD); CHECK(kb);
  /* both a joystick and a keyboard */
  c = caps("Combo Pad Keyboard", BUS_USB);
  cap_abs(&c, ABS_X, 0, 255, 128); cap_abs(&c, ABS_Y, 0, 255, 128); cap_key(&c, KEY_ENTER);
  CHECK_INT(evdev_classify_caps(&c, &kb), INSRC_JOYSTICK); CHECK(kb);
  /* only ABS_X: no stick */
  c = caps("Knob", BUS_USB); cap_abs(&c, ABS_X, 0, 255, 0);
  CHECK_INT(evdev_classify_caps(&c, &kb), INSRC_NONE);
  /* a name that fills the field without a NUL is read safely */
  c = caps("", BUS_USB);
  memset(c.name, 'E', sizeof c.name);
  cap_abs(&c, ABS_X, 0, 1, 0); cap_abs(&c, ABS_Y, 0, 1, 0);
  CHECK_INT(evdev_classify_caps(&c, &kb), INSRC_JOYSTICK);
}

/* ---------------------------------------------------------------- radios */

static void test_usb_radio(void) {
  evdev_t *e = open_e();
  evdev_caps_t c = dev_edgetx();
  c.absval[ABS_X] = 2047;                   /* the state at open is used */
  int id = evdev_add_virtual(e, &c);
  CHECK(id >= 0);
  CHECK_INT(poll_e(e, false), 0);
  int s = slot_named("EdgeTX RadioMaster TX16S Joystick");
  CHECK_INT(s, SLOT_EVDEV_FIRST);
  if (s < 0) { evdev_close(e); return; }
  CHECK_INT(src[s].kind, INSRC_USB);
  CHECK(!src[s].fixed_aetr);
  CHECK_INT(src[s].nch, 16);
  CHECK_INT(src[s].last_us, now);
  CHECK_INT(src[s].ch[0], 1024);
  for (int i = 1; i < 8; i++) CHECK_INT(src[s].ch[i], 0);
  for (int i = 8; i < 16; i++) CHECK_INT(src[s].ch[i], -1024);
  CHECK(!src[SLOT_KEYBOARD].present);
  /* axes 0..7 = CH1..CH8 */
  ev(e, id, EV_ABS, ABS_X, 0);
  ev(e, id, EV_ABS, ABS_Y, 2047);
  ev(e, id, EV_ABS, ABS_Z, 1023);
  ev(e, id, EV_ABS, ABS_RUDDER, 1);
  syn(e, id);
  now += 4000;
  CHECK_INT(poll_e(e, false), 0);
  CHECK_INT(src[s].ch[0], -1024);
  CHECK_INT(src[s].ch[1], 1024);
  CHECK_INT(src[s].ch[2], 0);
  CHECK_INT(src[s].ch[7], -1022);            /* -1022.999 */
  CHECK_INT(src[s].last_us, now);
  /* no dead zone on a radio */
  absv(e, id, ABS_RX, 1030);
  poll_e(e, false);
  CHECK_INT(src[s].ch[3], 6);                 /* 13 * 1024 / 2047 = 6.503 */
  /* overshoot is clamped */
  absv(e, id, ABS_RY, 2200);
  poll_e(e, false);
  CHECK_INT(src[s].ch[4], 1024);
  /* buttons in code order: the first 8 are CH9..CH16, the rest have no channel; no events */
  keyv(e, id, BTN_TRIGGER, 1);
  CHECK_INT(poll_e(e, false), 0);
  CHECK_INT(src[s].ch[8], 1024);
  keyv(e, id, BTN_TRIGGER + 7, 1);
  poll_e(e, false);
  CHECK_INT(src[s].ch[15], 1024);
  int16_t before[INPUT_CH];
  memcpy(before, src[s].ch, sizeof before);
  keyv(e, id, BTN_TRIGGER + 8, 1);
  keyv(e, id, BTN_TRIGGER_HAPPY, 1);
  CHECK_INT(poll_e(e, false), 0);
  CHECK(!memcmp(before, src[s].ch, sizeof before));
  keyv(e, id, BTN_TRIGGER, 0);
  poll_e(e, false);
  CHECK_INT(src[s].ch[8], -1024);
  /* silent but plugged in = live (HID reports changes only) */
  now += 3000000;
  poll_e(e, false);
  CHECK_INT(src[s].last_us, now);
  CHECK(src[s].present);
  /* unplugged: the slot is cleared */
  evdev_remove_virtual(e, id);
  poll_e(e, false);
  CHECK(!src[s].present);
  CHECK_INT(src[s].kind, INSRC_NONE);
  evdev_close(e);

  /* a radio with 4 axes and no buttons: 8 channels */
  e = open_e();
  c = caps("OpenTX Taranis Joystick", BUS_USB);
  for (int a = 0; a < 4; a++) cap_abs(&c, a, -1024, 1024, 0);
  id = evdev_add_virtual(e, &c);
  poll_e(e, false);
  s = slot_named("OpenTX Taranis Joystick");
  CHECK(s >= 0);
  if (s >= 0) {
    CHECK_INT(src[s].nch, 8);
    absv(e, id, ABS_Z, 1024);
    poll_e(e, false);
    CHECK_INT(src[s].ch[2], 1024);
    /* an axis it doesn't have changes nothing */
    absv(e, id, 6, 0);
    poll_e(e, false);
    CHECK_INT(src[s].ch[6], 0);
  }
  evdev_close(e);
}

static void test_ble_radio(void) {
  static const char *names[2] = { "ELRS Joystick", "ExpressLRS Joystick" };
  for (int n = 0; n < 2; n++) {
    evdev_t *e = open_e();
    evdev_caps_t c = dev_elrs(names[n]);
    int id = evdev_add_virtual(e, &c);
    CHECK(id >= 0);
    poll_e(e, false);
    int s = slot_named(names[n]);
    CHECK(s >= 0);
    if (s < 0) { evdev_close(e); continue; }
    CHECK_INT(src[s].kind, INSRC_BLUETOOTH);
    CHECK(!src[s].fixed_aetr);
    CHECK_INT(src[s].nch, 16);
    for (int i = 0; i < 7; i++) CHECK_INT(src[s].ch[i], 0);
    /* X=CH1 Y=CH2 RX=CH3 RY=CH4 Z=CH5 RZ=CH6 slider=CH7 */
    static const int ax[7] = { ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_THROTTLE };
    for (int i = 0; i < 7; i++) {
      absv(e, id, ax[i], 32767);
      poll_e(e, false);
      CHECK_INT(src[s].ch[i], 1024);
      absv(e, id, ax[i], 0);
      poll_e(e, false);
      CHECK_INT(src[s].ch[i], -1024);
    }
    absv(e, id, ABS_X, 16384 + 300);       /* no dead zone */
    poll_e(e, false);
    CHECK_INT(src[s].ch[0], 18);              /* 18.78 */
    /* buttons 1-8 = CH9-CH16 and never events (BTN_SOUTH is no ENTER here) */
    for (int b = 0; b < 8; b++) CHECK_INT(src[s].ch[8 + b], -1024);
    keyv(e, id, BTN_SOUTH, 1);
    CHECK_INT(poll_e(e, false), 0);
    CHECK_INT(src[s].ch[8], 1024);
    keyv(e, id, BTN_TR, 1);
    keyv(e, id, BTN_EAST, 1);
    CHECK_INT(poll_e(e, false), 0);
    CHECK_INT(src[s].ch[15], 1024);
    CHECK_INT(src[s].ch[9], 1024);
    keyv(e, id, BTN_SOUTH, 0);
    poll_e(e, false);
    CHECK_INT(src[s].ch[8], -1024);
    evdev_close(e);
  }
  /* a second slider shows up as the next code: CH8 */
  evdev_t *e = open_e();
  evdev_caps_t c = dev_elrs("ELRS Joystick");
  cap_abs(&c, ABS_RUDDER, 0, 32767, 0);
  int id = evdev_add_virtual(e, &c);
  poll_e(e, false);
  int s = slot_named("ELRS Joystick");
  CHECK(s >= 0);
  if (s >= 0) CHECK_INT(src[s].ch[7], -1024);
  absv(e, id, ABS_RUDDER, 32767);
  poll_e(e, false);
  if (s >= 0) CHECK_INT(src[s].ch[7], 1024);
  evdev_close(e);
  /* or ABS_WHEEL when there is no RUDDER */
  e = open_e();
  c = dev_elrs("ELRS Joystick");
  cap_abs(&c, ABS_WHEEL, 0, 32767, 32767);
  id = evdev_add_virtual(e, &c);
  poll_e(e, false);
  s = slot_named("ELRS Joystick");
  if (s >= 0) CHECK_INT(src[s].ch[7], 1024);
  evdev_close(e);
  /* a Bluetooth joystick that is no gamepad, buttons in the joystick range */
  e = open_e();
  c = caps("BT Flight Stick", BUS_BLUETOOTH);
  cap_abs(&c, ABS_X, -127, 127, 0); cap_abs(&c, ABS_Y, -127, 127, 0); cap_abs(&c, ABS_RX, -127, 127, 0);
  cap_keys(&c, BTN_TRIGGER, BTN_TRIGGER + 1);
  id = evdev_add_virtual(e, &c);
  poll_e(e, false);
  s = slot_named("BT Flight Stick");
  CHECK(s >= 0);
  if (s >= 0) {
    CHECK_INT(src[s].kind, INSRC_BLUETOOTH);
    CHECK_INT(src[s].nch, 10);
    absv(e, id, ABS_RX, 127);
    keyv(e, id, BTN_TRIGGER + 1, 1);
    poll_e(e, false);
    CHECK_INT(src[s].ch[2], 1024);
    CHECK_INT(src[s].ch[9], 1024);
  }
  evdev_close(e);
}

/* ---------------------------------------------------------------- gamepads */

enum { A_AIL, A_ELE, A_THR, A_RUD };

static void test_gamepad(void) {
  evdev_t *e = open_e();
  evdev_caps_t c = dev_xbox();
  int id = evdev_add_virtual(e, &c);
  CHECK(id >= 0);
  CHECK_INT(poll_e(e, false), 0);
  int s = slot_named("Microsoft X-Box One S pad");
  CHECK(s >= 0);
  if (s < 0) { evdev_close(e); return; }
  CHECK_INT(src[s].kind, INSRC_GAMEPAD);
  CHECK(src[s].fixed_aetr);
  CHECK_INT(src[s].nch, 7);
  CHECK_INT(src[s].ch[A_AIL], 0);
  CHECK_INT(src[s].ch[A_ELE], 0);
  CHECK_INT(src[s].ch[A_THR], 0);
  CHECK_INT(src[s].ch[A_RUD], 0);
  CHECK_INT(src[s].ch[4], -1024);
  CHECK_INT(src[s].ch[5], -1024);
  CHECK_INT(src[s].ch[6], -1024);
  /* mode 2: thr = -ABS_Y, rud = ABS_X, ele = -ABS_RY, ail = ABS_RX */
  absv(e, id, ABS_Y, -32768);
  absv(e, id, ABS_X, 32767);
  absv(e, id, ABS_RY, -32768);
  absv(e, id, ABS_RX, -32768);
  poll_e(e, false);
  CHECK_INT(src[s].ch[A_THR], 1024);
  CHECK_INT(src[s].ch[A_RUD], 1024);
  CHECK_INT(src[s].ch[A_ELE], 1024);
  CHECK_INT(src[s].ch[A_AIL], -1024);
  absv(e, id, ABS_Y, 32767);
  poll_e(e, false);
  CHECK_INT(src[s].ch[A_THR], -1024);
  /* dead zone 2% on ail/ele/rud, none on the throttle */
  absv(e, id, ABS_X, 600);
  absv(e, id, ABS_RX, -600);
  absv(e, id, ABS_RY, 600);
  absv(e, id, ABS_Y, -600);
  poll_e(e, false);
  CHECK_INT(src[s].ch[A_RUD], 0);
  CHECK_INT(src[s].ch[A_AIL], 0);
  CHECK_INT(src[s].ch[A_ELE], 0);
  CHECK_INT(src[s].ch[A_THR], 18);
  absv(e, id, ABS_X, 700);
  poll_e(e, false);
  CHECK_INT(src[s].ch[A_RUD], 21);
  /* triggers: CH6 = Z, CH7 = RZ */
  absv(e, id, ABS_Z, 1023);
  absv(e, id, ABS_RZ, 512);
  poll_e(e, false);
  CHECK_INT(src[s].ch[5], 1024);
  CHECK_INT(src[s].ch[6], 1);
  /* CH5: TL or TR toggles */
  keyv(e, id, BTN_TL, 1);
  CHECK_INT(poll_e(e, false), 0);
  CHECK_INT(src[s].ch[4], 1024);
  keyv(e, id, BTN_TL, 0);
  poll_e(e, false);
  CHECK_INT(src[s].ch[4], 1024);
  keyv(e, id, BTN_TR, 1);
  keyv(e, id, BTN_TR, 0);
  poll_e(e, false);
  CHECK_INT(src[s].ch[4], -1024);
  /* buttons -> events, on press */
  CHECK_INT(tap(e, id, BTN_SOUTH, false), ETX_EVT_ENTER);
  CHECK_INT(tap(e, id, BTN_EAST, false), ETX_EVT_EXIT);
  CHECK_INT(tap(e, id, BTN_START, false), ETX_EVT_EXIT);
  CHECK_INT(tap(e, id, BTN_SELECT, false), ETX_EVT_EXIT);
  CHECK_INT(tap(e, id, BTN_START, true), ETX_EVT_EXIT);     /* pauses in flight */
  CHECK_INT(tap(e, id, BTN_NORTH, false), 0);
  CHECK_INT(tap(e, id, BTN_WEST, false), 0);
  CHECK_INT(tap(e, id, BTN_MODE, false), 0);
  CHECK_INT(tap(e, id, BTN_THUMBL, false), 0);
  keyv(e, id, BTN_SOUTH, 1);
  CHECK_INT(poll_e(e, false), 1);
  keyv(e, id, BTN_SOUTH, 0);
  CHECK_INT(poll_e(e, false), 0);                           /* nothing on release */
  /* D-pad hat */
  absv(e, id, ABS_HAT0Y, -1);
  CHECK_INT(poll_e(e, false), 1); CHECK_INT(keys[0], EVDEV_KEY_UP);
  absv(e, id, ABS_HAT0Y, -1);                               /* same value again: nothing */
  CHECK_INT(poll_e(e, false), 0);
  absv(e, id, ABS_HAT0Y, 0);
  CHECK_INT(poll_e(e, false), 0);
  absv(e, id, ABS_HAT0Y, 1);
  CHECK_INT(poll_e(e, false), 1); CHECK_INT(keys[0], EVDEV_KEY_DOWN);
  absv(e, id, ABS_HAT0Y, -1);                               /* straight across: a new press */
  CHECK_INT(poll_e(e, false), 1); CHECK_INT(keys[0], EVDEV_KEY_UP);
  absv(e, id, ABS_HAT0Y, 0);
  absv(e, id, ABS_HAT0X, -1);
  CHECK_INT(poll_e(e, false), 1); CHECK_INT(keys[0], ETX_EVT_PREV);
  absv(e, id, ABS_HAT0X, 1);
  CHECK_INT(poll_e(e, false), 1); CHECK_INT(keys[0], ETX_EVT_NEXT);
  absv(e, id, ABS_HAT0X, 0);
  CHECK_INT(poll_e(e, false), 0);
  /* hats are no channels */
  CHECK_INT(src[s].ch[A_AIL], 0);
  /* several events in one poll keep their order */
  keyv(e, id, BTN_SOUTH, 1);
  absv(e, id, ABS_HAT0Y, 1);
  keyv(e, id, BTN_EAST, 1);
  CHECK_INT(poll_e(e, false), 3);
  CHECK_INT(keys[0], ETX_EVT_ENTER);
  CHECK_INT(keys[1], EVDEV_KEY_DOWN);
  CHECK_INT(keys[2], ETX_EVT_EXIT);
  keyv(e, id, BTN_SOUTH, 0);
  keyv(e, id, BTN_EAST, 0);
  absv(e, id, ABS_HAT0Y, 0);
  poll_e(e, false);

  /* stick modes: [left x, left y, right x, right y] */
  absv(e, id, ABS_X, 32767);      /* left x  full right */
  absv(e, id, ABS_Y, -16400);     /* left y  half up (-512.49) */
  absv(e, id, ABS_RX, -32768);    /* right x full left */
  absv(e, id, ABS_RY, 32767);     /* right y full down */
  poll_e(e, false);
  CHECK_INT(src[s].ch[A_RUD], 1024); CHECK_INT(src[s].ch[A_THR], 512);
  CHECK_INT(src[s].ch[A_AIL], -1024); CHECK_INT(src[s].ch[A_ELE], -1024);
  evdev_set_stick_mode(e, 1);     /* rud, ele, ail, thr */
  poll_e(e, false);
  CHECK_INT(src[s].ch[A_RUD], 1024); CHECK_INT(src[s].ch[A_ELE], 512);
  CHECK_INT(src[s].ch[A_AIL], -1024); CHECK_INT(src[s].ch[A_THR], -1024);
  evdev_set_stick_mode(e, 3);     /* ail, ele, rud, thr */
  poll_e(e, false);
  CHECK_INT(src[s].ch[A_AIL], 1024); CHECK_INT(src[s].ch[A_ELE], 512);
  CHECK_INT(src[s].ch[A_RUD], -1024); CHECK_INT(src[s].ch[A_THR], -1024);
  evdev_set_stick_mode(e, 4);     /* ail, thr, rud, ele */
  poll_e(e, false);
  CHECK_INT(src[s].ch[A_AIL], 1024); CHECK_INT(src[s].ch[A_THR], 512);
  CHECK_INT(src[s].ch[A_RUD], -1024); CHECK_INT(src[s].ch[A_ELE], -1024);
  CHECK_INT(src[s].ch[4], -1024);                 /* the toggle survives a mode change */
  /* the dead zone follows the throttle: mode 1 has it on the right stick's y */
  evdev_set_stick_mode(e, 1);
  absv(e, id, ABS_RY, -600);
  absv(e, id, ABS_Y, 600);
  poll_e(e, false);
  CHECK_INT(src[s].ch[A_THR], 18);
  CHECK_INT(src[s].ch[A_ELE], 0);
  /* out of range: mode 2 */
  evdev_set_stick_mode(e, 0);
  poll_e(e, false);
  CHECK_INT(src[s].ch[A_THR], -18);
  evdev_set_stick_mode(e, 1);
  evdev_set_stick_mode(e, 9);
  poll_e(e, false);
  CHECK_INT(src[s].ch[A_THR], -18);
  evdev_close(e);

  /* Xbox pad over Bluetooth: right stick on Z/RZ, triggers BRAKE/GAS, Back key = EXIT on release */
  e = open_e();
  c = dev_xbox_bt();
  id = evdev_add_virtual(e, &c);
  poll_e(e, false);
  s = slot_named("Xbox Wireless Controller");
  CHECK(s >= 0);
  if (s >= 0) {
    CHECK_INT(src[s].kind, INSRC_GAMEPAD);
    CHECK_INT(src[s].ch[A_AIL], 0);
    CHECK_INT(src[s].ch[5], -1024);
    absv(e, id, ABS_Z, 65535);
    absv(e, id, ABS_RZ, 0);
    absv(e, id, ABS_BRAKE, 1023);
    absv(e, id, ABS_GAS, 0);
    poll_e(e, false);
    CHECK_INT(src[s].ch[A_AIL], 1024);
    CHECK_INT(src[s].ch[A_ELE], 1024);
    CHECK_INT(src[s].ch[5], 1024);
    CHECK_INT(src[s].ch[6], -1024);
    CHECK(!src[SLOT_KEYBOARD].present);
    keyv(e, id, KEY_BACK, 1);
    CHECK_INT(poll_e(e, false), 0);
    keyv(e, id, KEY_BACK, 0);
    CHECK_INT(poll_e(e, false), 1);
    CHECK_INT(keys[0], ETX_EVT_EXIT);
  }
  evdev_close(e);

  /* DS4: 8-bit sticks */
  e = open_e();
  c = dev_ds4();
  id = evdev_add_virtual(e, &c);
  poll_e(e, false);
  s = slot_named("Sony Interactive Entertainment Wireless Controller");
  CHECK(s >= 0);
  if (s >= 0) {
    CHECK_INT(src[s].ch[A_RUD], 0);          /* 128 of 0..255 is +4: inside the dead zone */
    CHECK_INT(src[s].ch[A_THR], -4);         /* the throttle has none */
    absv(e, id, ABS_Y, 0);
    poll_e(e, false);
    CHECK_INT(src[s].ch[A_THR], 1024);
  }
  evdev_close(e);

  /* D-pad as buttons; a hat already held at open is no press */
  e = open_e();
  c = caps("8BitDo Pro 2", BUS_USB);
  cap_abs(&c, ABS_X, -32768, 32767, 0); cap_abs(&c, ABS_Y, -32768, 32767, 0);
  cap_abs(&c, ABS_HAT0Y, -1, 1, -1);
  cap_keys(&c, BTN_SOUTH, BTN_THUMBR);
  cap_keys(&c, BTN_DPAD_UP, BTN_DPAD_RIGHT);
  id = evdev_add_virtual(e, &c);
  CHECK_INT(poll_e(e, false), 0);
  CHECK_INT(tap(e, id, BTN_DPAD_UP, false), EVDEV_KEY_UP);
  CHECK_INT(tap(e, id, BTN_DPAD_DOWN, false), EVDEV_KEY_DOWN);
  CHECK_INT(tap(e, id, BTN_DPAD_LEFT, false), ETX_EVT_PREV);
  CHECK_INT(tap(e, id, BTN_DPAD_RIGHT, false), ETX_EVT_NEXT);
  absv(e, id, ABS_HAT0Y, -1);
  CHECK_INT(poll_e(e, false), 0);
  absv(e, id, ABS_HAT0Y, 0);
  absv(e, id, ABS_HAT0Y, -1);
  CHECK_INT(poll_e(e, false), 1);
  /* a hat with a 0..8-style range uses its middle */
  evdev_close(e);
  e = open_e();
  c = caps("Odd Pad", BUS_USB);
  cap_abs(&c, ABS_X, 0, 255, 128); cap_abs(&c, ABS_Y, 0, 255, 128);
  cap_abs(&c, ABS_HAT0X, 0, 2, 1); cap_abs(&c, ABS_HAT0Y, 5, 5, 5);   /* bogus range: by sign */
  cap_key(&c, BTN_SOUTH);
  id = evdev_add_virtual(e, &c);
  poll_e(e, false);
  absv(e, id, ABS_HAT0X, 0);
  CHECK_INT(poll_e(e, false), 1); CHECK_INT(keys[0], ETX_EVT_PREV);
  absv(e, id, ABS_HAT0X, 1);
  CHECK_INT(poll_e(e, false), 0);
  absv(e, id, ABS_HAT0X, 2);
  CHECK_INT(poll_e(e, false), 1); CHECK_INT(keys[0], ETX_EVT_NEXT);
  absv(e, id, ABS_HAT0Y, -3);
  CHECK_INT(poll_e(e, false), 1); CHECK_INT(keys[0], EVDEV_KEY_UP);
  evdev_close(e);
}

/* ---------------------------------------------------------------- generic joysticks */

static void test_joystick(void) {
  evdev_t *e = open_e();
  evdev_caps_t c = dev_extreme3d();
  int id = evdev_add_virtual(e, &c);
  poll_e(e, false);
  int s = slot_named("Logitech Logitech Extreme 3D");
  CHECK(s >= 0);
  if (s < 0) { evdev_close(e); return; }
  CHECK_INT(src[s].kind, INSRC_JOYSTICK);
  CHECK(!src[s].fixed_aetr);
  CHECK_INT(src[s].nch, 16);                 /* 4 axes (hats skipped) + 12 buttons */
  CHECK_INT(src[s].ch[0], 0);                /* 512 of 0..1023 is +1: dead zone */
  CHECK_INT(src[s].ch[2], 0);
  CHECK_INT(src[s].ch[3], -1024);            /* throttle lever at its end */
  for (int i = 4; i < 16; i++) CHECK_INT(src[s].ch[i], -1024);
  absv(e, id, ABS_RZ, 255);
  absv(e, id, ABS_THROTTLE, 255);
  absv(e, id, ABS_Y, 0);
  poll_e(e, false);
  CHECK_INT(src[s].ch[1], -1024);
  CHECK_INT(src[s].ch[2], 1024);
  CHECK_INT(src[s].ch[3], 1024);
  absv(e, id, ABS_X, 515);                   /* +7: dead zone on every axis */
  poll_e(e, false);
  CHECK_INT(src[s].ch[0], 0);
  absv(e, id, ABS_X, 530);
  poll_e(e, false);
  CHECK_INT(src[s].ch[0], 37);
  keyv(e, id, BTN_TRIGGER, 1);
  keyv(e, id, BTN_TRIGGER + 11, 1);
  CHECK_INT(poll_e(e, false), 0);
  CHECK_INT(src[s].ch[4], 1024);
  CHECK_INT(src[s].ch[15], 1024);
  absv(e, id, ABS_HAT0Y, -1);                /* not a gamepad: hats do nothing */
  CHECK_INT(poll_e(e, false), 0);
  evdev_close(e);

  /* two axes, no buttons: 4 channels */
  e = open_e();
  c = caps("Tiny Stick", BUS_USB);
  cap_abs(&c, ABS_X, 0, 255, 0); cap_abs(&c, ABS_Y, 0, 255, 255);
  evdev_add_virtual(e, &c);
  poll_e(e, false);
  s = slot_named("Tiny Stick");
  CHECK(s >= 0);
  if (s >= 0) {
    CHECK_INT(src[s].nch, 4);
    CHECK_INT(src[s].ch[0], -1024);
    CHECK_INT(src[s].ch[1], 1024);
    CHECK_INT(src[s].ch[2], 0);
  }
  evdev_close(e);

  /* more axes than channels: the first 16, no room for buttons */
  e = open_e();
  c = caps("Big Stick", BUS_USB);
  for (int a = 0; a < 0x2f; a++) if (a < 0x10 || a > 0x17) cap_abs(&c, a, 0, 100, 100);
  cap_keys(&c, BTN_TRIGGER, BTN_TRIGGER + 3);
  evdev_add_virtual(e, &c);
  poll_e(e, false);
  s = slot_named("Big Stick");
  CHECK(s >= 0);
  if (s >= 0) {
    CHECK_INT(src[s].nch, 16);
    for (int i = 0; i < 16; i++) CHECK_INT(src[s].ch[i], 1024);
  }
  evdev_close(e);
}

/* ---------------------------------------------------------------- keyboard events */

static void test_keyboard_keys(void) {
  evdev_t *e = open_e();
  evdev_caps_t c = dev_keyboard("Logitech USB Keyboard");
  int id = evdev_add_virtual(e, &c);
  CHECK(id >= 0);
  CHECK_INT(poll_e(e, false), 0);
  CHECK(src[SLOT_KEYBOARD].present);
  CHECK_INT(src[SLOT_KEYBOARD].kind, INSRC_KEYBOARD);
  CHECK(src[SLOT_KEYBOARD].fixed_aetr);
  CHECK_INT(src[SLOT_KEYBOARD].nch, 4);
  CHECK_STR(src[SLOT_KEYBOARD].name, "Logitech USB Keyboard");
  CHECK_INT(src[SLOT_KEYBOARD].ch[A_THR], -1024);
  CHECK_INT(src[SLOT_KEYBOARD].ch[A_AIL], 0);
  CHECK_INT(src[SLOT_KEYBOARD].last_us, now);
  for (int s = SLOT_EVDEV_FIRST; s < INPUT_MAX_SOURCES; s++) CHECK(!src[s].present);

  CHECK_INT(tap(e, id, KEY_ENTER, false), ETX_EVT_ENTER);
  CHECK_INT(tap(e, id, KEY_KPENTER, false), ETX_EVT_ENTER);
  CHECK_INT(tap(e, id, KEY_SPACE, false), ETX_EVT_ENTER);
  CHECK_INT(tap(e, id, KEY_ENTER, true), ETX_EVT_ENTER);
  CHECK_INT(tap(e, id, KEY_UP, false), EVDEV_KEY_UP);
  CHECK_INT(tap(e, id, KEY_DOWN, false), EVDEV_KEY_DOWN);
  CHECK_INT(tap(e, id, KEY_LEFT, false), ETX_EVT_PREV);
  CHECK_INT(tap(e, id, KEY_RIGHT, false), ETX_EVT_NEXT);
  CHECK_INT(tap(e, id, KEY_UP, true), 0);              /* in flight the arrows fly */
  CHECK_INT(tap(e, id, KEY_DOWN, true), 0);
  CHECK_INT(tap(e, id, KEY_LEFT, true), 0);
  CHECK_INT(tap(e, id, KEY_RIGHT, true), 0);
  CHECK_INT(tap(e, id, KEY_EQUAL, false), ETX_EVT_NEXT);
  CHECK_INT(tap(e, id, KEY_KPPLUS, false), ETX_EVT_NEXT);
  CHECK_INT(tap(e, id, KEY_MINUS, false), ETX_EVT_PREV);
  CHECK_INT(tap(e, id, KEY_KPMINUS, false), ETX_EVT_PREV);
  CHECK_INT(tap(e, id, KEY_EQUAL, true), ETX_EVT_NEXT);  /* +/- are events in flight too */
  CHECK_INT(tap(e, id, KEY_PAGEUP, false), ETX_EVT_PREV_PAGE);
  CHECK_INT(tap(e, id, KEY_PAGEDOWN, false), ETX_EVT_NEXT_PAGE);
  CHECK_INT(tap(e, id, KEY_M, false), ETX_EVT_MENU);
  CHECK_INT(tap(e, id, KEY_R, false), EVDEV_KEY_RELOAD);
  CHECK_INT(tap(e, id, KEY_R, true), EVDEV_KEY_RELOAD);
  CHECK_INT(tap(e, id, KEY_Q, false), 0);
  CHECK_INT(tap(e, id, KEY_W, false), 0);
  CHECK_INT(tap(e, id, KEY_A, true), 0);
  CHECK_INT(tap(e, id, KEY_I, false), 0);
  /* TV remote keys */
  CHECK_INT(tap(e, id, KEY_OK, false), ETX_EVT_ENTER);
  CHECK_INT(tap(e, id, KEY_SELECT, false), ETX_EVT_ENTER);
  CHECK_INT(tap(e, id, KEY_EXIT, false), ETX_EVT_EXIT);
  CHECK_INT(tap(e, id, KEY_BACK, false), ETX_EVT_EXIT);

  /* Esc / Backspace: EXIT on release */
  keyv(e, id, KEY_ESC, 1);
  CHECK_INT(poll_e(e, false), 0);
  keyv(e, id, KEY_ESC, 0);
  CHECK_INT(poll_e(e, false), 1); CHECK_INT(keys[0], ETX_EVT_EXIT);
  CHECK_INT(tap(e, id, KEY_BACKSPACE, true), ETX_EVT_EXIT);   /* pauses in flight */
  /* held up to 900 ms: still EXIT */
  keyv(e, id, KEY_ESC, 1);
  poll_e(e, false);
  now += 900000;
  CHECK_INT(poll_e(e, false), 0);
  keyv(e, id, KEY_ESC, 0);
  CHECK_INT(poll_e(e, false), 1); CHECK_INT(keys[0], ETX_EVT_EXIT);
  /* held longer: one reload while held, nothing on release */
  keyv(e, id, KEY_BACKSPACE, 1);
  poll_e(e, false);
  now += 900000;
  CHECK_INT(poll_e(e, false), 0);
  now += 1;
  CHECK_INT(poll_e(e, false), 1); CHECK_INT(keys[0], EVDEV_KEY_RELOAD);
  now += 2000000;
  CHECK_INT(poll_e(e, false), 0);
  keyv(e, id, KEY_BACKSPACE, 0);
  CHECK_INT(poll_e(e, false), 0);
  /* auto-repeat keeps the key held but sends nothing, also for Esc */
  keyv(e, id, KEY_DOWN, 1);
  CHECK_INT(poll_e(e, false), 1);
  keyv(e, id, KEY_DOWN, 2); keyv(e, id, KEY_DOWN, 2); keyv(e, id, KEY_DOWN, 2);
  CHECK_INT(poll_e(e, false), 0);
  keyv(e, id, KEY_DOWN, 0);
  CHECK_INT(poll_e(e, false), 0);
  keyv(e, id, KEY_ESC, 1);
  keyv(e, id, KEY_ESC, 2);
  now += 300000;
  keyv(e, id, KEY_ESC, 2);
  CHECK_INT(poll_e(e, false), 0);
  keyv(e, id, KEY_ESC, 0);
  CHECK_INT(poll_e(e, false), 1); CHECK_INT(keys[0], ETX_EVT_EXIT);
  /* a repeat for a key we never saw go down is no press */
  keyv(e, id, KEY_ENTER, 2);
  CHECK_INT(poll_e(e, false), 0);
  keyv(e, id, KEY_ENTER, 0);
  /* a second press without a release in between is not a new press */
  keyv(e, id, KEY_ENTER, 1);
  keyv(e, id, KEY_ENTER, 1);
  CHECK_INT(poll_e(e, false), 1);
  keyv(e, id, KEY_ENTER, 0);
  poll_e(e, false);
  /* a release without a press is nothing */
  keyv(e, id, KEY_ESC, 0);
  CHECK_INT(poll_e(e, false), 0);
  /* Ctrl, Alt or Meta held: no events; Shift is fine */
  keyv(e, id, KEY_LEFTCTRL, 1);
  CHECK_INT(tap(e, id, KEY_R, false), 0);
  CHECK_INT(tap(e, id, KEY_ENTER, false), 0);
  keyv(e, id, KEY_ESC, 1);
  keyv(e, id, KEY_LEFTCTRL, 0);
  keyv(e, id, KEY_ESC, 0);
  CHECK_INT(poll_e(e, false), 0);                    /* its press didn't count */
  CHECK_INT(tap(e, id, KEY_R, false), EVDEV_KEY_RELOAD);
  keyv(e, id, KEY_RIGHTALT, 1);
  CHECK_INT(tap(e, id, KEY_ENTER, false), 0);
  keyv(e, id, KEY_RIGHTALT, 0);
  keyv(e, id, KEY_LEFTMETA, 1);
  CHECK_INT(tap(e, id, KEY_M, false), 0);
  keyv(e, id, KEY_LEFTMETA, 0);
  keyv(e, id, KEY_LEFTSHIFT, 1);
  CHECK_INT(tap(e, id, KEY_EQUAL, false), ETX_EVT_NEXT);
  keyv(e, id, KEY_LEFTSHIFT, 0);
  poll_e(e, false);
  /* events wait for room: maxkeys */
  keyv(e, id, KEY_ENTER, 1); keyv(e, id, KEY_ENTER, 0);
  keyv(e, id, KEY_UP, 1); keyv(e, id, KEY_UP, 0);
  keyv(e, id, KEY_PAGEUP, 1); keyv(e, id, KEY_PAGEUP, 0);
  keyv(e, id, KEY_M, 1); keyv(e, id, KEY_M, 0);
  keyv(e, id, KEY_R, 1); keyv(e, id, KEY_R, 0);
  CHECK_INT(evdev_poll(e, src, now, false, keys, 2), 2);
  CHECK_INT(keys[0], ETX_EVT_ENTER); CHECK_INT(keys[1], EVDEV_KEY_UP);
  CHECK_INT(evdev_poll(e, src, now, false, NULL, 5), 0);
  CHECK_INT(evdev_poll(e, src, now, false, keys, 0), 0);
  CHECK_INT(evdev_poll(e, src, now, false, keys, -3), 0);
  CHECK_INT(evdev_poll(e, src, now, false, keys, 2), 2);
  CHECK_INT(keys[0], ETX_EVT_PREV_PAGE); CHECK_INT(keys[1], ETX_EVT_MENU);
  CHECK_INT(evdev_poll(e, src, now, false, keys, 2), 1);
  CHECK_INT(keys[0], EVDEV_KEY_RELOAD);
  CHECK_INT(evdev_poll(e, src, now, false, keys, 2), 0);
  /* the event queue holds 64; more are dropped, nothing breaks */
  for (int i = 0; i < 70; i++) { ev(e, id, EV_KEY, KEY_ENTER, 1); ev(e, id, EV_KEY, KEY_ENTER, 0); }
  syn(e, id);
  CHECK_INT(poll_e(e, false), 64);
  CHECK_INT(poll_e(e, false), 0);
  CHECK_INT(tap(e, id, KEY_ENTER, false), ETX_EVT_ENTER);

  /* unplugged while Esc is held: no reload later, no EXIT */
  keyv(e, id, KEY_ESC, 1);
  poll_e(e, false);
  evdev_remove_virtual(e, id);
  now += 2000000;
  CHECK_INT(poll_e(e, false), 0);
  CHECK(!src[SLOT_KEYBOARD].present);
  CHECK_INT(src[SLOT_KEYBOARD].kind, INSRC_KEYBOARD);   /* the slot keeps its shape */
  CHECK_INT(src[SLOT_KEYBOARD].nch, 4);
  CHECK_INT(src[SLOT_KEYBOARD].ch[A_THR], -1024);
  evdev_close(e);

  /* Esc held on two keyboards: one EXIT; a long one on the second gives one reload */
  e = open_e();
  c = dev_keyboard("Keyboard A");
  int ka = evdev_add_virtual(e, &c);
  c = dev_keyboard("Keyboard B");
  int kb = evdev_add_virtual(e, &c);
  poll_e(e, false);
  CHECK_STR(src[SLOT_KEYBOARD].name, "Keyboard A");
  keyv(e, ka, KEY_ESC, 1);
  keyv(e, kb, KEY_ESC, 1);
  poll_e(e, false);
  keyv(e, ka, KEY_ESC, 0);
  keyv(e, kb, KEY_ESC, 0);
  CHECK_INT(poll_e(e, false), 1); CHECK_INT(keys[0], ETX_EVT_EXIT);
  keyv(e, ka, KEY_ESC, 1);
  poll_e(e, false);
  evdev_remove_virtual(e, kb);                 /* the other keyboard going away doesn't cancel it */
  now += 1000000;
  CHECK_INT(poll_e(e, false), 1); CHECK_INT(keys[0], EVDEV_KEY_RELOAD);
  CHECK_STR(src[SLOT_KEYBOARD].name, "Keyboard A");
  evdev_remove_virtual(e, ka);
  poll_e(e, false);
  CHECK(!src[SLOT_KEYBOARD].present);
  /* the name follows the keyboards there are */
  c = dev_keyboard("");
  evdev_add_virtual(e, &c);
  poll_e(e, false);
  CHECK(src[SLOT_KEYBOARD].present);
  CHECK_STR(src[SLOT_KEYBOARD].name, "keyboard");
  evdev_close(e);
}

/* ---------------------------------------------------------------- keyboard flying */

static double kbd_axis(double v, double t, double dt) {   /* app.js keyboardSticks, one step */
  v += (t - v) * (1 - exp(-dt / 70));
  if (t == 0 && fabs(v) < 0.003) v = 0;
  return v;
}
static int js_round(double v) { return (int)floor(v * 1024 + 0.5); }

static void step(evdev_t *e, int ms, bool fly) { now += ms * 1000; poll_e(e, fly); }

static void test_keyboard_flying(void) {
  evdev_t *e = open_e();
  evdev_caps_t c = dev_keyboard("Logitech USB Keyboard");
  int id = evdev_add_virtual(e, &c);
  poll_e(e, true);
  insrc_t *k = &src[SLOT_KEYBOARD];
  /* one 70 ms step toward 0.6: 388; with Shift toward 1.0: 647 (the emulator's numbers) */
  keyv(e, id, KEY_RIGHT, 1);
  poll_e(e, true);
  step(e, 70, true);
  CHECK_INT(k->ch[A_AIL], 388);
  keyv(e, id, KEY_RIGHT, 0);
  for (int i = 0; i < 100; i++) step(e, 20, true);
  CHECK_INT(k->ch[A_AIL], 0);                    /* recentred, snapped to exactly 0 */
  keyv(e, id, KEY_LEFTSHIFT, 1);
  keyv(e, id, KEY_RIGHT, 1);
  poll_e(e, true);
  step(e, 70, true);
  CHECK_INT(k->ch[A_AIL], 647);
  for (int i = 0; i < 100; i++) step(e, 20, true);
  CHECK_INT(k->ch[A_AIL], 1024);
  keyv(e, id, KEY_LEFTSHIFT, 0);
  for (int i = 0; i < 100; i++) step(e, 20, true);
  CHECK_INT(k->ch[A_AIL], 614);                  /* back to 0.6 */
  keyv(e, id, KEY_RIGHT, 0);
  for (int i = 0; i < 100; i++) step(e, 20, true);

  /* the exact curve, step by step, against the emulator's formula */
  double v = 0;
  int bad = 0;
  keyv(e, id, KEY_UP, 1);
  poll_e(e, true);
  for (int i = 0; i < 30; i++) {
    int ms = 5 + (i * 7) % 40;
    step(e, ms, true);
    v = kbd_axis(v, 0.6, ms);
    if (k->ch[A_ELE] != js_round(v)) bad++;
  }
  keyv(e, id, KEY_UP, 0);
  poll_e(e, true);
  for (int i = 0; i < 60; i++) {
    step(e, 16, true);
    v = kbd_axis(v, 0, 16);
    if (k->ch[A_ELE] != js_round(v)) bad++;
  }
  CHECK_INT(bad, 0);
  CHECK_INT(k->ch[A_ELE], 0);

  /* axes and directions: arrows and I/K/J/L for pitch/roll, A/D for the rudder */
  static const struct { int key, axis, sign; } dirs[] = {
    { KEY_RIGHT, A_AIL, 1 }, { KEY_LEFT, A_AIL, -1 }, { KEY_L, A_AIL, 1 }, { KEY_J, A_AIL, -1 },
    { KEY_UP, A_ELE, 1 }, { KEY_DOWN, A_ELE, -1 }, { KEY_I, A_ELE, 1 }, { KEY_K, A_ELE, -1 },
    { KEY_D, A_RUD, 1 }, { KEY_A, A_RUD, -1 },
  };
  for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
    keyv(e, id, dirs[i].key, 1);
    CHECK_INT(poll_e(e, true), 0);               /* no events in flight */
    for (int j = 0; j < 50; j++) step(e, 20, true);
    CHECK_INT(k->ch[dirs[i].axis], 614 * dirs[i].sign);
    static const int sticks[3] = { A_AIL, A_ELE, A_RUD };
    for (int a = 0; a < 3; a++) if (sticks[a] != dirs[i].axis) CHECK_INT(k->ch[sticks[a]], 0);
    CHECK_INT(k->ch[A_THR], -1024);
    keyv(e, id, dirs[i].key, 0);
    for (int j = 0; j < 100; j++) step(e, 20, true);
    CHECK_INT(k->ch[dirs[i].axis], 0);
  }
  /* both directions held: centre */
  keyv(e, id, KEY_LEFT, 1);
  keyv(e, id, KEY_RIGHT, 1);
  for (int j = 0; j < 30; j++) step(e, 20, true);
  CHECK_INT(k->ch[A_AIL], 0);
  keyv(e, id, KEY_LEFT, 0);
  keyv(e, id, KEY_RIGHT, 0);
  poll_e(e, true);

  /* throttle: W/S sweep (2.8 s end to end, 1 s with Shift) and stay */
  CHECK_INT(k->ch[A_THR], -1024);
  keyv(e, id, KEY_W, 1);
  poll_e(e, true);
  for (int i = 0; i < 70; i++) step(e, 20, true);    /* 1400 ms: -1 -> 0 */
  CHECK_INT(k->ch[A_THR], 0);
  keyv(e, id, KEY_W, 0);
  for (int i = 0; i < 50; i++) step(e, 20, true);
  CHECK_INT(k->ch[A_THR], 0);                        /* stays */
  keyv(e, id, KEY_RIGHTSHIFT, 1);
  keyv(e, id, KEY_W, 1);
  poll_e(e, true);
  for (int i = 0; i < 25; i++) step(e, 10, true);    /* 250 ms fast: +0.5 */
  CHECK_INT(k->ch[A_THR], 512);
  for (int i = 0; i < 100; i++) step(e, 10, true);
  CHECK_INT(k->ch[A_THR], 1024);                     /* clamped */
  keyv(e, id, KEY_W, 0);
  keyv(e, id, KEY_S, 1);
  poll_e(e, true);
  for (int i = 0; i < 50; i++) step(e, 10, true);    /* 500 ms fast: -1.0 */
  CHECK_INT(k->ch[A_THR], 0);
  keyv(e, id, KEY_RIGHTSHIFT, 0);
  for (int i = 0; i < 200; i++) step(e, 10, true);
  CHECK_INT(k->ch[A_THR], -1024);
  keyv(e, id, KEY_S, 0);
  /* W and S together: they cancel */
  keyv(e, id, KEY_W, 1);
  keyv(e, id, KEY_S, 1);
  poll_e(e, true);
  for (int i = 0; i < 50; i++) step(e, 20, true);
  CHECK_INT(k->ch[A_THR], -1024);
  keyv(e, id, KEY_S, 0);
  poll_e(e, true);
  /* a long gap counts as 250 ms at most */
  step(e, 10000, true);
  CHECK_INT(k->ch[A_THR], js_round(-1 + 250.0 / 1400));
  /* not flying: nothing moves, the throttle stays, the axes recentre */
  int thr = k->ch[A_THR];
  for (int i = 0; i < 50; i++) step(e, 20, false);
  CHECK_INT(k->ch[A_THR], thr);
  keyv(e, id, KEY_W, 0);
  keyv(e, id, KEY_UP, 1);
  CHECK_INT(poll_e(e, false), 1); CHECK_INT(keys[0], EVDEV_KEY_UP);
  for (int i = 0; i < 20; i++) step(e, 20, false);
  CHECK_INT(k->ch[A_ELE], 0);
  for (int i = 0; i < 50; i++) step(e, 20, true);    /* the held key flies once in flight */
  CHECK_INT(k->ch[A_ELE], 614);
  for (int i = 0; i < 100; i++) step(e, 20, false);  /* and recentres when the flight ends */
  CHECK_INT(k->ch[A_ELE], 0);
  keyv(e, id, KEY_UP, 0);
  poll_e(e, false);
  /* time going backwards changes nothing */
  keyv(e, id, KEY_W, 1);
  poll_e(e, true);
  thr = k->ch[A_THR];
  now -= 500000;
  poll_e(e, true);
  CHECK_INT(k->ch[A_THR], thr);
  keyv(e, id, KEY_W, 0);
  poll_e(e, true);

  /* the source: updated when the sticks change, live while the keyboard is there */
  uint32_t u = k->updates;
  for (int i = 0; i < 10; i++) step(e, 20, true);
  CHECK_INT(k->updates, u);
  CHECK_INT(k->last_us, now);
  keyv(e, id, KEY_D, 1);
  for (int i = 0; i < 10; i++) step(e, 20, true);
  CHECK(k->updates > u);
  keyv(e, id, KEY_D, 0);
  for (int i = 0; i < 100; i++) step(e, 20, true);

  /* two keyboards: keys held on either count; unplugging one lets go of its keys */
  c = dev_keyboard("Second Keyboard");
  int id2 = evdev_add_virtual(e, &c);
  keyv(e, id2, KEY_D, 1);
  keyv(e, id, KEY_LEFTSHIFT, 1);
  for (int i = 0; i < 50; i++) step(e, 20, true);
  CHECK_INT(k->ch[A_RUD], 1024);
  evdev_remove_virtual(e, id2);
  for (int i = 0; i < 100; i++) step(e, 20, true);
  CHECK_INT(k->ch[A_RUD], 0);
  CHECK(k->present);
  keyv(e, id, KEY_LEFTSHIFT, 0);
  poll_e(e, true);
  evdev_close(e);

  /* keys of a joystick that is also a keyboard fly and navigate too */
  e = open_e();
  c = caps("Combo Pad Keyboard", BUS_USB);
  cap_abs(&c, ABS_X, 0, 255, 0); cap_abs(&c, ABS_Y, 0, 255, 255);
  cap_keys(&c, 1, 127);
  id = evdev_add_virtual(e, &c);
  poll_e(e, true);
  int s = slot_named("Combo Pad Keyboard");
  CHECK(s >= 0);
  CHECK(src[SLOT_KEYBOARD].present);
  CHECK_STR(src[SLOT_KEYBOARD].name, "Combo Pad Keyboard");
  if (s >= 0) { CHECK_INT(src[s].kind, INSRC_JOYSTICK); CHECK_INT(src[s].ch[0], -1024); }
  CHECK_INT(tap(e, id, KEY_ENTER, false), ETX_EVT_ENTER);
  keyv(e, id, KEY_W, 1);
  poll_e(e, true);
  for (int i = 0; i < 70; i++) step(e, 20, true);
  CHECK_INT(src[SLOT_KEYBOARD].ch[A_THR], 0);
  if (s >= 0) CHECK_INT(src[s].ch[0], -1024);        /* its sticks are its own */
  evdev_close(e);
}

/* ---------------------------------------------------------------- slots */

static void test_slots(void) {
  evdev_t *e = open_e();
  /* the WiFi slot is never touched */
  src[SLOT_WIFI].present = true;
  src[SLOT_WIFI].kind = INSRC_WIFI;
  src[SLOT_WIFI].ch[0] = 777;
  strcpy(src[SLOT_WIFI].name, "ELRS 10.0.0.1");
  /* garbage in the evdev slots is cleared by the first poll */
  src[5].present = true;
  src[5].kind = INSRC_GAMEPAD;
  CHECK_INT(poll_e(e, false), 0);
  CHECK(!src[5].present);
  CHECK_INT(src[5].kind, INSRC_NONE);
  CHECK(!src[SLOT_KEYBOARD].present);
  CHECK_INT(src[SLOT_KEYBOARD].kind, INSRC_KEYBOARD);
  CHECK_INT(src[SLOT_KEYBOARD].ch[A_THR], -1024);

  /* 10 joystick slots; the 11th waits for one */
  int ids[12];
  char name[64];
  for (int i = 0; i < 11; i++) {
    evdev_caps_t c = dev_edgetx();
    snprintf(c.name, sizeof c.name, "EdgeTX Radio %d", i);
    c.absval[ABS_X] = i * 100;
    ids[i] = evdev_add_virtual(e, &c);
    CHECK(ids[i] >= 0);
  }
  poll_e(e, false);
  for (int i = 0; i < 10; i++) {
    snprintf(name, sizeof name, "EdgeTX Radio %d", i);
    CHECK_INT(slot_named(name), SLOT_EVDEV_FIRST + i);
  }
  CHECK_INT(slot_named("EdgeTX Radio 10"), -1);
  CHECK_INT(src[SLOT_WIFI].ch[0], 777);
  CHECK_STR(src[SLOT_WIFI].name, "ELRS 10.0.0.1");
  CHECK(src[SLOT_WIFI].present);
  /* events of the waiting one are kept in its state */
  absv(e, ids[10], ABS_Y, 2047);
  poll_e(e, false);
  evdev_remove_virtual(e, ids[3]);
  now += 1000;
  poll_e(e, false);
  int s = slot_named("EdgeTX Radio 10");
  CHECK_INT(s, SLOT_EVDEV_FIRST + 3);
  if (s >= 0) {
    CHECK_INT(src[s].ch[1], 1024);
    CHECK_INT(src[s].ch[0], evdev_scale_axis(1000, 0, 2047));
    CHECK_INT(src[s].last_us, now);
    CHECK_INT(src[s].moved_us, 0);               /* a new source has not moved yet */
  }
  CHECK_INT(slot_named("EdgeTX Radio 3"), -1);
  /* keyboards take no slot */
  evdev_caps_t kc = dev_keyboard("Keyboard");
  int k1 = evdev_add_virtual(e, &kc);
  CHECK(k1 >= 0);
  poll_e(e, false);
  CHECK(src[SLOT_KEYBOARD].present);
  /* devices: 24 at most; things that are no input are refused */
  int added = 11, last = -1;                     /* 10 radios and a keyboard so far */
  while (added < 30) {
    int r = evdev_add_virtual(e, &kc);
    if (r < 0) break;
    last = r;
    added++;
  }
  CHECK_INT(added, 24);
  CHECK(last >= 0);
  evdev_remove_virtual(e, last);
  evdev_caps_t none = caps("Power Button", BUS_HOST);
  cap_key(&none, KEY_POWER);
  CHECK_INT(evdev_add_virtual(e, &none), -1);
  evdev_caps_t tp = dev_touchpad();
  CHECK_INT(evdev_add_virtual(e, &tp), -1);
  CHECK(evdev_add_virtual(e, &kc) >= 0);
  CHECK_INT(evdev_add_virtual(e, NULL), -1);
  /* bad ids */
  CHECK(!evdev_inject(e, -1, EV_KEY, KEY_ENTER, 1));
  CHECK(!evdev_inject(e, 24, EV_KEY, KEY_ENTER, 1));
  CHECK(!evdev_inject(e, 1000, EV_KEY, KEY_ENTER, 1));
  evdev_remove_virtual(e, -1);
  evdev_remove_virtual(e, 99);
  evdev_set_state(e, -1, EV_ABS, 0, 0);
  /* out-of-range codes are ignored */
  CHECK(evdev_inject(e, ids[0], EV_ABS, 0x40, 5));
  CHECK(evdev_inject(e, ids[0], EV_ABS, 0xffff, 5));
  CHECK(evdev_inject(e, ids[0], EV_KEY, 0x300, 1));
  CHECK(evdev_inject(e, ids[0], EV_KEY, 0xfff, 1));
  CHECK(evdev_inject(e, ids[0], 0x15, 0, 1));            /* EV_FF etc. */
  CHECK(evdev_inject(e, ids[0], EV_SYN, 0x7, 0));
  syn(e, ids[0]);
  CHECK_INT(poll_e(e, false), 0);
  CHECK_INT(src[SLOT_EVDEV_FIRST].ch[0], evdev_scale_axis(0, 0, 2047));
  evdev_close(e);

  /* an event for a removed device never reaches the next one in its entry */
  e = open_e();
  evdev_caps_t c = dev_edgetx();
  int a = evdev_add_virtual(e, &c);
  CHECK(evdev_inject(e, a, EV_ABS, ABS_X, 0));
  evdev_remove_virtual(e, a);
  int b = evdev_add_virtual(e, &c);
  CHECK_INT(b, a);
  syn(e, b);
  poll_e(e, false);
  s = slot_named("EdgeTX RadioMaster TX16S Joystick");
  CHECK(s >= 0);
  if (s >= 0) CHECK_INT(src[s].ch[0], 0);
  /* the inject queue is bounded */
  int ok = 0;
  for (int i = 0; i < 300; i++) ok += evdev_inject(e, b, EV_ABS, ABS_Y, i);
  CHECK_INT(ok, 256);
  CHECK_INT(poll_e(e, false), 0);
  /* the "kernel" state still took the last value: a SYN_DROPPED resync finds it */
  CHECK(evdev_inject(e, b, EV_SYN, SYN_DROPPED, 0));
  syn(e, b);
  poll_e(e, false);
  if (s >= 0) CHECK_INT(src[s].ch[1], evdev_scale_axis(299, 0, 2047));
  evdev_close(e);

  /* NULL safety and the stub's fds */
  CHECK_INT(evdev_poll(NULL, src, now, false, keys, 4), 0);
  CHECK_INT(evdev_fds(NULL, NULL, 0), 0);
  evdev_close(NULL);
  evdev_set_stick_mode(NULL, 1);
  CHECK_INT(evdev_add_virtual(NULL, NULL), -1);
  CHECK(!evdev_inject(NULL, 0, 0, 0, 0));
  evdev_set_state(NULL, 0, 0, 0, 0);
  evdev_remove_virtual(NULL, 0);
  e = open_e();
  int fds[8];
  CHECK_INT(evdev_fds(e, fds, 8), 0);                    /* no such folder: nothing to watch */
  CHECK_INT(evdev_fds(e, NULL, 8), 0);
  /* poll without a src array: devices still work, the slots catch up later */
  c = dev_keyboard("Keyboard");
  int kid = evdev_add_virtual(e, &c);
  keyv(e, kid, KEY_ENTER, 1);
  CHECK_INT(evdev_poll(e, NULL, now, false, keys, 8), 1);
  CHECK(!src[SLOT_KEYBOARD].present);
  poll_e(e, false);
  CHECK(src[SLOT_KEYBOARD].present);
  evdev_close(e);
  /* NULL and "" mean the default folder (only checked to open) */
  e = evdev_open("");
  CHECK(e != NULL);
  evdev_close(e);
}

/* ---------------------------------------------------------------- SYN_DROPPED */

static void test_resync(void) {
  evdev_t *e = open_e();
  evdev_caps_t c = dev_edgetx();
  int id = evdev_add_virtual(e, &c);
  absv(e, id, ABS_X, 100);
  poll_e(e, false);
  int s = slot_named("EdgeTX RadioMaster TX16S Joystick");
  CHECK(s >= 0);
  if (s < 0) { evdev_close(e); return; }
  CHECK_INT(src[s].ch[0], evdev_scale_axis(100, 0, 2047));
  /* the kernel dropped events: what follows up to SYN_REPORT is skipped, then the state is read */
  evdev_set_state(e, id, EV_ABS, ABS_X, 1500);           /* lost */
  evdev_set_state(e, id, EV_KEY, BTN_TRIGGER, 1);        /* lost */
  ev(e, id, EV_SYN, SYN_DROPPED, 0);
  ev(e, id, EV_ABS, ABS_Y, 7);                           /* the rest of a broken packet */
  evdev_set_state(e, id, EV_ABS, ABS_Y, 2000);           /* ... and the kernel moved on */
  ev(e, id, EV_ABS, ABS_Z, 2047);
  evdev_set_state(e, id, EV_ABS, ABS_Z, 1024);
  CHECK_INT(poll_e(e, false), 0);
  CHECK_INT(src[s].ch[0], evdev_scale_axis(100, 0, 2047));   /* still dropping: nothing applied */
  CHECK_INT(src[s].ch[1], 0);
  syn(e, id);
  CHECK_INT(poll_e(e, false), 0);
  CHECK_INT(src[s].ch[0], evdev_scale_axis(1500, 0, 2047));
  CHECK_INT(src[s].ch[1], evdev_scale_axis(2000, 0, 2047));
  CHECK_INT(src[s].ch[2], 0);
  CHECK_INT(src[s].ch[8], 1024);
  /* afterwards events flow again */
  absv(e, id, ABS_X, 0);
  poll_e(e, false);
  CHECK_INT(src[s].ch[0], -1024);
  /* SYN_DROPPED and SYN_REPORT in different polls */
  ev(e, id, EV_SYN, SYN_DROPPED, 0);
  poll_e(e, false);
  evdev_set_state(e, id, EV_ABS, ABS_X, 2047);
  syn(e, id);
  poll_e(e, false);
  CHECK_INT(src[s].ch[0], 1024);
  /* two drops in a row */
  ev(e, id, EV_SYN, SYN_DROPPED, 0);
  ev(e, id, EV_SYN, SYN_DROPPED, 0);
  evdev_set_state(e, id, EV_KEY, BTN_TRIGGER, 0);
  syn(e, id);
  poll_e(e, false);
  CHECK_INT(src[s].ch[8], -1024);
  evdev_close(e);

  /* keyboard: keys that changed in the gap become late events (and held keys let go) */
  e = open_e();
  c = dev_keyboard("Logitech USB Keyboard");
  id = evdev_add_virtual(e, &c);
  keyv(e, id, KEY_W, 1);
  poll_e(e, true);
  for (int i = 0; i < 20; i++) step(e, 20, true);
  int thr = src[SLOT_KEYBOARD].ch[A_THR];
  CHECK(thr > -1024);
  keyv(e, id, KEY_ESC, 1);
  poll_e(e, true);
  evdev_set_state(e, id, EV_KEY, KEY_W, 0);
  evdev_set_state(e, id, EV_KEY, KEY_ESC, 0);
  evdev_set_state(e, id, EV_KEY, KEY_ENTER, 1);
  ev(e, id, EV_SYN, SYN_DROPPED, 0);
  syn(e, id);
  int n = poll_e(e, true);
  CHECK_INT(n, 2);
  bool got_exit = false, got_enter = false;
  for (int i = 0; i < n; i++) { got_exit |= keys[i] == ETX_EVT_EXIT; got_enter |= keys[i] == ETX_EVT_ENTER; }
  CHECK(got_exit);
  CHECK(got_enter);
  thr = src[SLOT_KEYBOARD].ch[A_THR];
  for (int i = 0; i < 20; i++) step(e, 20, true);
  CHECK_INT(src[SLOT_KEYBOARD].ch[A_THR], thr);          /* W is no longer held */
  /* nothing changed in the gap: no events */
  ev(e, id, EV_SYN, SYN_DROPPED, 0);
  syn(e, id);
  CHECK_INT(poll_e(e, true), 0);
  evdev_close(e);

  /* gamepad: a D-pad press in the gap, the sticks and the toggle */
  e = open_e();
  c = dev_xbox();
  id = evdev_add_virtual(e, &c);
  poll_e(e, false);
  s = slot_named("Microsoft X-Box One S pad");
  CHECK(s >= 0);
  evdev_set_state(e, id, EV_ABS, ABS_HAT0Y, 1);
  evdev_set_state(e, id, EV_ABS, ABS_Y, -32768);
  evdev_set_state(e, id, EV_KEY, BTN_TR, 1);
  ev(e, id, EV_SYN, SYN_DROPPED, 0);
  syn(e, id);
  n = poll_e(e, false);
  CHECK_INT(n, 1);
  CHECK_INT(keys[0], EVDEV_KEY_DOWN);
  if (s >= 0) {
    CHECK_INT(src[s].ch[A_THR], 1024);
    CHECK_INT(src[s].ch[4], 1024);
  }
  evdev_close(e);
}

int main(void) {
  test_scale();
  test_classify();
  test_classify_caps();
  test_usb_radio();
  test_ble_radio();
  test_gamepad();
  test_joystick();
  test_keyboard_keys();
  test_keyboard_flying();
  test_slots();
  test_resync();
  T_DONE();
}
