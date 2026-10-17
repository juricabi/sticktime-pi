/* input_evdev against the real kernel: virtual devices made with /dev/uinput (an EdgeTX radio,
 * an ELRS BLE joystick on BUS_BLUETOOTH, a gamepad, a keyboard, plus a touchpad and a mouse that
 * must be ignored). Checks the initial scan, hotplug add/remove, channel maps, key events with
 * kernel auto-repeat, keyboard flying, SYN_DROPPED resync, a node that can't be opened until its
 * permissions change (as an unprivileged child), fast create/destroy churn, no leaked fds, and
 * that a poll never takes long (open/close run on input_evdev's worker thread).
 * Linux only, as root (it makes devices and drops to "nobody" in a child). Build static for the Pi:
 *   zig cc -target aarch64-linux-musl -static -std=gnu99 -Ihost/src host/tests/linux/test_evdev_uinput.c \
 *     host/src/input_evdev.c host/src/input.c -o test_evdev_uinput -lm
 * (host/tests/linux/run_uinput.sh builds it and runs it over ssh). */
#define _GNU_SOURCE
#include "../t.h"
#include "input_evdev.h"
#include "etx.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static insrc_t src[INPUT_MAX_SOURCES];
static int keys[512], nkeys;
static int64_t max_poll_us, max_cpu_us;
static const char *phase = "start", *max_phase = "-";
static int slow_polls;
static evdev_t *E;

static int64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int64_t cpu_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* Wall time can include the scheduler (an emulated VM stalls whole vCPUs); the CPU time of
 * this thread is what evdev_poll itself costs, and blocking would show as wall >> CPU. */
static void pump(bool fly) {
  int64_t t0 = now_us(), c0 = cpu_us();
  int room = (int)(sizeof keys / sizeof keys[0]) - nkeys;
  nkeys += evdev_poll(E, src, t0, fly, keys + nkeys, room);
  int64_t dt = now_us() - t0, dc = cpu_us() - c0;
  if (dt > max_poll_us) { max_poll_us = dt; max_phase = phase; }
  if (dc > max_cpu_us) max_cpu_us = dc;
  if (dt > 20000 && slow_polls++ < 20)
    printf("slow poll in %s: %.2f ms wall, %.2f ms cpu\n", phase, dt / 1000.0, dc / 1000.0);
}

/* poll every 2 ms until cond holds or ms pass */
#define WAIT_FOR(fly, ms, cond) ({ bool ok_ = false; int64_t end_ = now_us() + (int64_t)(ms) * 1000; \
  for (;;) { pump(fly); if (cond) { ok_ = true; break; } if (now_us() > end_) break; usleep(2000); } ok_; })

static void run_for(bool fly, int ms) {
  int64_t end = now_us() + (int64_t)ms * 1000;
  while (now_us() < end) { pump(fly); usleep(2000); }
}

static int slot_named(const char *name) {
  for (int s = SLOT_EVDEV_FIRST; s < INPUT_MAX_SOURCES; s++)
    if (src[s].present && !strcmp(src[s].name, name)) return s;
  return -1;
}

static int count_key(int k) {
  int n = 0;
  for (int i = 0; i < nkeys; i++) n += keys[i] == k;
  return n;
}

/* ---------------------------------------------------------------- uinput */

typedef struct { int code, min, max, val; } absdef_t;

static int uinput_make(const char *name, int bus, const int *keyl, int nkey, const absdef_t *absl, int nabs,
                       const int *props, int nprop, bool rep) {
  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) { fprintf(stderr, "open /dev/uinput: %s\n", strerror(errno)); exit(2); }
  if (nkey) {
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    for (int i = 0; i < nkey; i++) ioctl(fd, UI_SET_KEYBIT, keyl[i]);
  }
  if (rep) ioctl(fd, UI_SET_EVBIT, EV_REP);
  if (nabs) {
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    for (int i = 0; i < nabs; i++) {
      struct uinput_abs_setup a;
      memset(&a, 0, sizeof a);
      a.code = (uint16_t)absl[i].code;
      a.absinfo.minimum = absl[i].min;
      a.absinfo.maximum = absl[i].max;
      a.absinfo.value = absl[i].val;
      ioctl(fd, UI_SET_ABSBIT, absl[i].code);
      if (ioctl(fd, UI_ABS_SETUP, &a) < 0) { fprintf(stderr, "UI_ABS_SETUP: %s\n", strerror(errno)); exit(2); }
    }
  }
  for (int i = 0; i < nprop; i++) ioctl(fd, UI_SET_PROPBIT, props[i]);
  struct uinput_setup us;
  memset(&us, 0, sizeof us);
  us.id.bustype = (uint16_t)bus;
  us.id.vendor = 0x1209;
  us.id.product = 0x4f54;
  snprintf(us.name, sizeof us.name, "%s", name);
  if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
    fprintf(stderr, "uinput create %s: %s\n", name, strerror(errno));
    exit(2);
  }
  return fd;
}

static void uinput_destroy(int fd) {
  ioctl(fd, UI_DEV_DESTROY);
  close(fd);
}

static void emit(int fd, int type, int code, int value) {
  struct input_event ie;
  memset(&ie, 0, sizeof ie);
  ie.type = (uint16_t)type;
  ie.code = (uint16_t)code;
  ie.value = value;
  if (write(fd, &ie, sizeof ie) != (ssize_t)sizeof ie) fprintf(stderr, "uinput write: %s\n", strerror(errno));
}
static void report(int fd) { emit(fd, EV_SYN, SYN_REPORT, 0); }
static void key(int fd, int code, int v) { emit(fd, EV_KEY, code, v); report(fd); }
static void absv(int fd, int code, int v) { emit(fd, EV_ABS, code, v); report(fd); }

#define RADIO "EdgeTX RadioMaster TX16S Joystick"
#define ELRS "ELRS Joystick"
#define PAD "Microsoft X-Box One S pad"
#define KBD "StickTime Test Keyboard"

static int make_radio_named(const char *name) {
  int kl[24];
  for (int i = 0; i < 16; i++) kl[i] = BTN_TRIGGER + i;
  for (int i = 0; i < 8; i++) kl[16 + i] = BTN_TRIGGER_HAPPY1 + i;
  absdef_t al[8];
  for (int i = 0; i < 8; i++) al[i] = (absdef_t){ i, 0, 2047, 1024 };
  return uinput_make(name, BUS_USB, kl, 24, al, 8, NULL, 0, false);
}
static int make_radio(void) { return make_radio_named(RADIO); }
static int make_elrs(void) {
  int kl[8];
  for (int i = 0; i < 8; i++) kl[i] = BTN_SOUTH + i;
  absdef_t al[7] = { { ABS_X, 0, 32767, 16384 }, { ABS_Y, 0, 32767, 16384 }, { ABS_Z, 0, 32767, 16384 },
                     { ABS_RX, 0, 32767, 16384 }, { ABS_RY, 0, 32767, 0 }, { ABS_RZ, 0, 32767, 16384 },
                     { ABS_THROTTLE, 0, 32767, 0 } };
  return uinput_make(ELRS, BUS_BLUETOOTH, kl, 8, al, 7, NULL, 0, false);
}
static int make_pad(void) {
  int kl[] = { BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE,
               BTN_THUMBL, BTN_THUMBR };
  absdef_t al[] = { { ABS_X, -32768, 32767, 0 }, { ABS_Y, -32768, 32767, 0 }, { ABS_RX, -32768, 32767, 0 },
                    { ABS_RY, -32768, 32767, 0 }, { ABS_Z, 0, 1023, 0 }, { ABS_RZ, 0, 1023, 0 },
                    { ABS_HAT0X, -1, 1, 0 }, { ABS_HAT0Y, -1, 1, 0 } };
  return uinput_make(PAD, BUS_USB, kl, (int)(sizeof kl / sizeof kl[0]), al, (int)(sizeof al / sizeof al[0]), NULL, 0, false);
}
static int make_keyboard(void) {
  int kl[127];
  for (int i = 0; i < 127; i++) kl[i] = i + 1;
  return uinput_make(KBD, BUS_USB, kl, 127, NULL, 0, NULL, 0, true);
}

/* "/dev/input/eventN" of a uinput device, "" when not found */
static void node_of(int ufd, char *out, size_t n) {
  char sys[64] = "", dir[160];
  out[0] = 0;
  if (ioctl(ufd, UI_GET_SYSNAME(sizeof sys), sys) < 0) return;
  snprintf(dir, sizeof dir, "/sys/devices/virtual/input/%s", sys);
  for (int tries = 0; tries < 100 && !out[0]; tries++) {
    DIR *dp = opendir(dir);
    struct dirent *de;
    while (dp && (de = readdir(dp)))
      if (!strncmp(de->d_name, "event", 5)) snprintf(out, n, "/dev/input/%.32s", de->d_name);
    if (dp) closedir(dp);
    if (!out[0]) usleep(10000);
  }
}

static int count_fds(void) {
  DIR *dp = opendir("/proc/self/fd");
  if (!dp) return -1;
  int n = 0;
  struct dirent *de;
  while ((de = readdir(dp))) n += de->d_name[0] != '.';
  closedir(dp);
  return n - 1;                     /* the one opendir holds */
}

static char got(int rd) {
  char c;
  return read(rd, &c, 1) == 1 ? c : 0;
}

/* ---------------------------------------------------------------- unprivileged child */

#define PERM "EdgeTX Locked Radio"

/* As "nobody" every node is EACCES: the scan at open queues them, retries end after 2 s. The radio
 * the parent makes stays out of reach (udev's 0660 root:input, or devtmpfs' 0600) until the parent
 * chmods it 2.5 s later, after the retry window: the IN_ATTRIB alone must bring it in. */
static int perm_child(int rd, int wr) {
  if (setgid(65534) != 0 || setuid(65534) != 0) { perror("setuid nobody"); return 3; }
  fcntl(rd, F_SETFL, O_NONBLOCK);
  E = evdev_open(NULL);
  CHECK(E != NULL);
  if (!E) return 1;
  if (write(wr, "r", 1) != 1) return 3;
  char c = 0;
  int64_t end = now_us() + 10000000;
  while (c != 'c' && now_us() < end) {
    pump(false);
    CHECK_INT(slot_named(PERM), -1);
    char g = got(rd);
    if (g) c = g;
    usleep(2000);
  }
  CHECK(c == 'c');
  CHECK(WAIT_FOR(false, 2000, slot_named(PERM) >= 0));
  int s = slot_named(PERM);
  if (s >= 0) { CHECK_INT(src[s].kind, INSRC_USB); CHECK_INT(src[s].ch[0], 0); }
  evdev_close(E);
  printf("child (nobody): %d checks, %d failed\n", t_count, t_fail);
  return t_fail ? 1 : 0;
}

static void perm_test(void) {
  int down[2], up[2];
  if (pipe(down) || pipe(up)) { perror("pipe"); t_fail++; return; }
  pid_t pid = fork();
  if (pid == 0) {
    close(down[1]);
    close(up[0]);
    _exit(perm_child(down[0], up[1]));
  }
  close(down[0]);
  close(up[1]);
  t_count++;
  if (pid < 0) { perror("fork"); t_fail++; return; }
  char c = 0;
  if (read(up[0], &c, 1) != 1 || c != 'r') { fprintf(stderr, "child not ready\n"); t_fail++; }
  int dev = make_radio_named(PERM);
  char node[64];
  node_of(dev, node, sizeof node);
  CHECK(node[0] != 0);
  usleep(2500000);
  struct stat st;
  if (node[0] && stat(node, &st) == 0) printf("%s mode %o uid %d gid %d before chmod\n", node, (unsigned)(st.st_mode & 0777), (int)st.st_uid, (int)st.st_gid);
  CHECK(node[0] && chmod(node, 0666) == 0);
  if (write(down[1], "c", 1) != 1) t_fail++;
  int status = 0;
  waitpid(pid, &status, 0);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  uinput_destroy(dev);
  close(down[1]);
  close(up[0]);
}

/* ---------------------------------------------------------------- tests */

int main(void) {
  if (access("/dev/uinput", W_OK) != 0 && system("modprobe uinput") != 0) {
    fprintf(stderr, "no /dev/uinput (modprobe uinput)\n");
    return 2;
  }
  setvbuf(stdout, NULL, _IOLBF, 0);
  int fd0 = count_fds();

  /* first, while this process has one thread: fork() then is safe */
  phase = "permissions";
  perm_test();

  /* a keyboard that exists before evdev_open: found by the scan, there at the first poll */
  phase = "scan";
  int kb = make_keyboard();
  usleep(300000);
  E = evdev_open(NULL);
  CHECK(E != NULL);
  if (!E) T_DONE();
  pump(false);
  CHECK(src[SLOT_KEYBOARD].present);
  if (strcmp(src[SLOT_KEYBOARD].name, KBD)) printf("note: keyboard slot named \"%s\" (another keyboard?)\n", src[SLOT_KEYBOARD].name);
  CHECK_INT(src[SLOT_KEYBOARD].kind, INSRC_KEYBOARD);
  CHECK_INT(src[SLOT_KEYBOARD].ch[2], -1024);
  /* keep the test's keystrokes away from the console: grab what we read */
  int fds[64];
  int nf = evdev_fds(E, fds, 64);
  CHECK(nf >= 3);                         /* inotify, the worker's eventfd, the keyboard */
  for (int i = 2; i < nf; i++) CHECK(ioctl(fds[i], EVIOCGRAB, 1) == 0);

  /* hotplug: devices made after evdev_open */
  phase = "radio";
  int radio = make_radio();
  CHECK(WAIT_FOR(false, 3000, slot_named(RADIO) >= 0));
  int rs = slot_named(RADIO);
  printf("radio in slot %d\n", rs);
  if (rs >= 0) {
    CHECK_INT(src[rs].kind, INSRC_USB);
    CHECK_INT(src[rs].nch, 16);
    CHECK_INT(src[rs].ch[0], 0);
    CHECK_INT(src[rs].ch[8], -1024);
    emit(radio, EV_ABS, ABS_X, 2047);
    emit(radio, EV_ABS, ABS_Y, 0);
    emit(radio, EV_ABS, 7, 1536);
    report(radio);
    CHECK(WAIT_FOR(false, 1000, src[rs].ch[0] == 1024 && src[rs].ch[1] == -1024));
    CHECK_INT(src[rs].ch[7], evdev_scale_axis(1536, 0, 2047));
    key(radio, BTN_TRIGGER, 1);
    key(radio, BTN_TRIGGER_HAPPY1, 1);   /* the 17th button: no channel */
    CHECK(WAIT_FOR(false, 1000, src[rs].ch[8] == 1024));
    for (int i = 9; i < 16; i++) CHECK_INT(src[rs].ch[i], -1024);
    /* a radio held still is silent but live */
    run_for(false, 700);
    CHECK(now_us() - src[rs].last_us < 100000);
  }

  phase = "elrs";
  int elrs = make_elrs();
  CHECK(WAIT_FOR(false, 3000, slot_named(ELRS) >= 0));
  int es = slot_named(ELRS);
  if (es >= 0) {
    CHECK_INT(src[es].kind, INSRC_BLUETOOTH);
    CHECK_INT(src[es].nch, 16);
    CHECK_INT(src[es].ch[2], -1024);     /* RY at 0 = CH3 (ELRS 4.x: the throttle) */
    nkeys = 0;
    absv(elrs, ABS_RX, 32767);           /* CH6 */
    absv(elrs, ABS_Z, 0);                /* CH5 */
    key(elrs, BTN_SOUTH, 1);             /* button 1 = CH9, not ENTER */
    CHECK(WAIT_FOR(false, 1000, src[es].ch[5] == 1024 && src[es].ch[4] == -1024 && src[es].ch[8] == 1024));
    run_for(false, 100);
    CHECK_INT(nkeys, 0);
  }

  phase = "pad";
  int pad = make_pad();
  CHECK(WAIT_FOR(false, 3000, slot_named(PAD) >= 0));
  int ps =slot_named(PAD);
  if (ps >= 0) {
    CHECK_INT(src[ps].kind, INSRC_GAMEPAD);
    CHECK(src[ps].fixed_aetr);
    absv(pad, ABS_Y, -32768);            /* left stick up: throttle */
    absv(pad, ABS_RX, 32767);            /* right stick right: roll */
    CHECK(WAIT_FOR(false, 1000, src[ps].ch[2] == 1024 && src[ps].ch[0] == 1024));
    nkeys = 0;
    key(pad, BTN_SOUTH, 1);
    key(pad, BTN_SOUTH, 0);
    absv(pad, ABS_HAT0Y, -1);
    absv(pad, ABS_HAT0Y, 0);
    key(pad, BTN_START, 1);
    key(pad, BTN_START, 0);
    CHECK(WAIT_FOR(false, 1000, nkeys >= 3));
    run_for(false, 50);
    CHECK_INT(nkeys, 3);
    CHECK_INT(keys[0], ETX_EVT_ENTER);
    CHECK_INT(keys[1], EVDEV_KEY_UP);
    CHECK_INT(keys[2], ETX_EVT_EXIT);
    key(pad, BTN_TL, 1);
    CHECK(WAIT_FOR(false, 1000, src[ps].ch[4] == 1024));
  }

  /* keyboard events, with the kernel's auto-repeat running */
  phase = "keyboard";
  nkeys = 0;
  key(kb, KEY_ENTER, 1);
  key(kb, KEY_ENTER, 0);
  CHECK(WAIT_FOR(false, 1000, nkeys == 1));
  CHECK_INT(keys[0], ETX_EVT_ENTER);
  nkeys = 0;
  key(kb, KEY_ESC, 1);
  run_for(false, 100);
  CHECK_INT(nkeys, 0);                    /* EXIT comes on release */
  key(kb, KEY_ESC, 0);
  CHECK(WAIT_FOR(false, 1000, nkeys == 1));
  CHECK_INT(keys[0], ETX_EVT_EXIT);
  nkeys = 0;
  key(kb, KEY_ESC, 1);                    /* held 1.3 s: repeats, one reload, nothing on release */
  run_for(false, 1300);
  key(kb, KEY_ESC, 0);
  run_for(false, 100);
  CHECK_INT(nkeys, 1);
  CHECK_INT(count_key(EVDEV_KEY_RELOAD), 1);
  nkeys = 0;
  key(kb, KEY_DOWN, 1);                   /* held: one event despite the repeats */
  run_for(false, 700);
  key(kb, KEY_DOWN, 0);
  run_for(false, 50);
  CHECK_INT(nkeys, 1);
  CHECK_INT(count_key(EVDEV_KEY_DOWN), 1);
  nkeys = 0;
  key(kb, KEY_UP, 1);                     /* in flight the arrows fly */
  run_for(true, 300);
  CHECK_INT(nkeys, 0);
  CHECK(src[SLOT_KEYBOARD].ch[1] > 500);
  key(kb, KEY_UP, 0);
  run_for(true, 600);
  CHECK_INT(src[SLOT_KEYBOARD].ch[1], 0);
  /* W held for ~700 ms in flight: the throttle climbs about half way and stays */
  key(kb, KEY_W, 1);
  run_for(true, 700);
  key(kb, KEY_W, 0);
  run_for(true, 50);
  int thr = src[SLOT_KEYBOARD].ch[2];
  printf("throttle after W for 700 ms: %d (about -512 expected)\n", thr);
  CHECK(thr > -800 && thr < -250);
  run_for(true, 300);
  CHECK_INT(src[SLOT_KEYBOARD].ch[2], thr);

  /* SYN_DROPPED: flood the radio without reading; a change only in the dropped part (CH2) must
   * still arrive, through the EVIOCGABS resync */
  phase = "syn_dropped";
  if (rs >= 0) {
    pump(false);
    emit(radio, EV_ABS, ABS_Y, 2047);
    report(radio);
    for (int i = 0; i < 3000; i++) {
      emit(radio, EV_ABS, ABS_X, i % 2000);
      report(radio);
    }
    emit(radio, EV_ABS, ABS_X, 1999 - 7);
    report(radio);
    pump(false);
    CHECK(WAIT_FOR(false, 1000, src[rs].ch[1] == 1024));
    CHECK_INT(src[rs].ch[0], evdev_scale_axis(1992, 0, 2047));
    /* the button pressed before is still known */
    CHECK_INT(src[rs].ch[8], 1024);
    /* a key released during a flood comes through too */
    key(radio, BTN_TRIGGER, 0);
    for (int i = 0; i < 3000; i++) { emit(radio, EV_ABS, ABS_Z, i % 2000); report(radio); }
    CHECK(WAIT_FOR(false, 1000, src[rs].ch[8] == -1024));
  }

  /* things that are no sticks are not taken */
  phase = "ignored";
  int tkeys[] = { BTN_LEFT, BTN_TOUCH, BTN_TOOL_FINGER };
  absdef_t tabs[] = { { ABS_X, 0, 4000, 0 }, { ABS_Y, 0, 3000, 0 } };
  int tprops[] = { INPUT_PROP_POINTER };
  int touch = uinput_make("Test TouchPad", BUS_I8042, tkeys, 3, tabs, 2, tprops, 1, false);
  int mkeys[] = { BTN_LEFT, BTN_RIGHT };
  int mouse = uinput_make("Test Mouse", BUS_USB, mkeys, 2, NULL, 0, NULL, 0, false);
  int nf0 = evdev_fds(E, fds, 64);
  run_for(false, 500);
  CHECK_INT(slot_named("Test TouchPad"), -1);
  CHECK_INT(slot_named("Test Mouse"), -1);
  CHECK_INT(evdev_fds(E, fds, 64), nf0);   /* not kept open */

  /* churn: made and destroyed before (or while) the worker opens them, the kernel reusing the same
   * eventN each time; none may end up in a slot or leak an fd, and the one that stays comes in */
  phase = "churn";
  for (int i = 0; i < 6; i++) {
    int t = make_radio_named("EdgeTX Churn Radio");
    if (i % 2) run_for(false, 4);            /* sometimes the worker gets to it first */
    uinput_destroy(t);
  }
  int keep = make_radio_named("EdgeTX Churn Keep");
  CHECK(WAIT_FOR(false, 3000, slot_named("EdgeTX Churn Keep") >= 0));
  run_for(false, 500);
  CHECK_INT(slot_named("EdgeTX Churn Radio"), -1);
  CHECK_INT(evdev_fds(E, fds, 64), nf0 + 1);
  uinput_destroy(keep);
  CHECK(WAIT_FOR(false, 3000, slot_named("EdgeTX Churn Keep") < 0));
  CHECK_INT(evdev_fds(E, fds, 64), nf0);

  /* unplug and plug again */
  phase = "replug";
  if (ps >= 0) {
    uinput_destroy(pad);
    CHECK(WAIT_FOR(false, 3000, slot_named(PAD) < 0));
    CHECK(!src[ps].present);
    pad = make_pad();
    CHECK(WAIT_FOR(false, 3000, slot_named(PAD) >= 0));
    int ps2 = slot_named(PAD);
    if (ps2 >= 0) CHECK_INT(src[ps2].ch[4], -1024);   /* a new device: the toggle starts off */
  }
  uinput_destroy(kb);
  CHECK(WAIT_FOR(false, 3000, !src[SLOT_KEYBOARD].present));
  int fl = evdev_fds(E, fds, 64);
  uinput_destroy(radio);
  uinput_destroy(elrs);
  uinput_destroy(pad);
  CHECK(WAIT_FOR(false, 3000, slot_named(RADIO) < 0 && slot_named(ELRS) < 0 && slot_named(PAD) < 0));
  CHECK_INT(evdev_fds(E, fds, 64), fl - 3);
  uinput_destroy(touch);
  uinput_destroy(mouse);
  run_for(false, 200);
  printf("longest evdev_poll: %.2f ms wall (in %s), %.2f ms cpu; %d polls over 20 ms wall\n",
         max_poll_us / 1000.0, max_phase, max_cpu_us / 1000.0, slow_polls);
  CHECK(max_cpu_us < 20000);
  CHECK(max_poll_us < 50000);   /* no sleeping in open()/close(): wall time is CPU time plus scheduling */
  /* closing down with a device open and another one handed to the worker by the last poll */
  int open_r = make_radio_named("EdgeTX Open Radio");
  CHECK(WAIT_FOR(false, 3000, slot_named("EdgeTX Open Radio") >= 0));
  int late = make_radio_named("EdgeTX Late Radio");
  usleep(20000);                    /* its node is there: the next poll reads IN_CREATE and asks for it */
  pump(false);
  evdev_close(E);
  uinput_destroy(open_r);
  uinput_destroy(late);
  /* the worker closed everything it was handed; nothing open is left behind */
  CHECK_INT(count_fds(), fd0);
  T_DONE();
}
