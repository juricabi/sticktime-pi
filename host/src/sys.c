/* Process setup for low latency and the console: see sys.h. */
#ifdef __linux__
#define _GNU_SOURCE
#endif
#include "sys.h"
#include "plat.h"

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile int quit_flag = 0;

#ifdef __linux__
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <net/if.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#ifndef K_OFF
#define K_OFF 0x04
#endif

void sys_realtime(int prio, int cpu, char *msg, size_t msglen) {
  char buf[256] = "";
  size_t n = 0;
  if (prio > 0) {
    /* pthread_setschedparam, not sched_setscheduler: musl's sched_setscheduler always fails (ENOSYS) */
    struct sched_param sp = { .sched_priority = prio };
    int e = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (e != 0)
      n += (size_t)snprintf(buf + n, sizeof buf - n, "SCHED_FIFO: %s; ", strerror(e));
  }
  if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
    n += (size_t)snprintf(buf + n, sizeof buf - n, "mlockall: %s; ", strerror(errno));
  if (cpu >= 0 && cpu < CPU_SETSIZE) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0)
      n += (size_t)snprintf(buf + n, sizeof buf - n, "affinity: %s; ", strerror(errno));
  }
  if (msg && msglen) plat_strlcpy(msg, buf, msglen);
}

/* ---------------------------------------------------------------- console (VT) */

static int tty_fd = -1;             /* swapped atomically: the restore can run in a signal handler */
static int tty_vt = 0;              /* VT number of tty_fd (1..63), 0 = unknown */
static int tty_kbmode = -1;         /* keyboard mode before K_OFF */
static volatile int tty_graphics = 0;
static int handlers_installed = 0;

void sys_console_restore(void) {
  /* async-signal-safe: only syscalls. KD_TEXT first: fbcon force-restores its mode while we are
   * still DRM master, so our framebuffers aren't on screen when video_close removes them. */
  int fd = __atomic_exchange_n(&tty_fd, -1, __ATOMIC_SEQ_CST);
  if (fd < 0) return;
  tty_graphics = 0;
  ioctl(fd, KDSETMODE, KD_TEXT);
  /* K_OFF saved means an earlier run died without restoring: a usable console wants unicode */
  int kb = tty_kbmode;
  ioctl(fd, KDSKBMODE, kb < 0 || kb == K_OFF ? K_UNICODE : kb);
  close(fd);
}

static void fatal_signal(int sig) {
  sys_console_restore();
  raise(sig);          /* SA_RESETHAND put the default action back: core dump / exit status */
}

static void restore_at_exit(void) { sys_console_restore(); }

static void install_fatal_handlers(void) {
  if (handlers_installed) return;
  handlers_installed = 1;
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = fatal_signal;
  sa.sa_flags = SA_RESETHAND | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  /* SIGQUIT: Ctrl-\ in a dev run over SSH (default action: core dump, the tty stays dead) */
  static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGQUIT };
  for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) sigaction(sigs[i], &sa, NULL);
  atexit(restore_at_exit);
}

/* the VT number behind an fd: /dev/ttyN is char major 4 minor N; tty0 / console = the front one */
static int vt_number(int fd, int active) {
  struct stat st;
  if (fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && major(st.st_rdev) == 4 &&
      minor(st.st_rdev) >= 1 && minor(st.st_rdev) <= 63)
    return (int)minor(st.st_rdev);
  return active;
}

void sys_console_graphics(const char *tty) {
  if (__atomic_load_n(&tty_fd, __ATOMIC_SEQ_CST) >= 0) return;
  const char *path = tty && *tty ? tty : "/dev/tty1";
  /* O_NONBLOCK: a write to a VT stopped with ^S would otherwise block us forever */
  int fd = open(path, O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) return;                       /* no VT (container, serial only): nothing to do */
  struct vt_stat st;
  if (ioctl(fd, VT_GETSTATE, &st) != 0) {   /* not a virtual terminal */
    close(fd);
    return;
  }
  int vt = vt_number(fd, st.v_active);
  if (vt > 0 && st.v_active != vt) {
    ioctl(fd, VT_ACTIVATE, vt);
    /* poll rather than VT_WAITACTIVE: that can hang forever when a VT_PROCESS owner refuses,
     * and an alarm() to break it may hit another thread */
    for (int i = 0; i < 100; i++) {
      if (ioctl(fd, VT_GETSTATE, &st) == 0 && st.v_active == vt) break;
      plat_sleep_us(20000);
    }
  }
  int kb = -1;
  tty_kbmode = ioctl(fd, KDGKBMODE, &kb) == 0 ? kb : -1;
  /* cursor off, screen blanking off (also helps when KD_GRAPHICS is refused) */
  static const char hide[] = "\033[?25l\033[9;0]";
  if (write(fd, hide, sizeof hide - 1) < 0) {}
  install_fatal_handlers();                 /* before the mode changes: a crash right after restores */
  tty_vt = vt;
  __atomic_store_n(&tty_fd, fd, __ATOMIC_SEQ_CST);
  ioctl(fd, KDSKBMODE, K_OFF);              /* no typed chars, Alt+Fn or Ctrl-Alt-Del; evdev still works */
  if (ioctl(fd, KDSETMODE, KD_GRAPHICS) == 0) tty_graphics = 1;
}

bool sys_console_is_graphics(void) { return tty_graphics != 0; }

void sys_console_refresh(void) {
  int fd = __atomic_load_n(&tty_fd, __ATOMIC_SEQ_CST);
  if (fd < 0) return;
  int kd = -1, kb = -1;
  /* someone (an admin, a --restore-vt) put our VT back into text mode: fbcon would draw and VT
   * switches work again. The saved keyboard mode stays the one from before our start. */
  if (ioctl(fd, KDGKBMODE, &kb) == 0 && kb != K_OFF) ioctl(fd, KDSKBMODE, K_OFF);
  if (ioctl(fd, KDGETMODE, &kd) == 0 && kd != KD_GRAPHICS && ioctl(fd, KDSETMODE, KD_GRAPHICS) == 0) tty_graphics = 1;
}

bool sys_console_foreground(void) {
  int fd = __atomic_load_n(&tty_fd, __ATOMIC_SEQ_CST);
  if (fd < 0 || tty_vt <= 0) return true;
  struct vt_stat st;
  if (ioctl(fd, VT_GETSTATE, &st) != 0) return true;
  return st.v_active == tty_vt;
}

int sys_console_reset(const char *tty) {
  int fd = open(tty && *tty ? tty : "/dev/tty1", O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) return -1;
  int r = ioctl(fd, KDSETMODE, KD_TEXT) == 0 ? 0 : -1;
  int kb = -1;
  /* a keyboard left off or raw by a dead program makes the console unusable */
  if (ioctl(fd, KDGKBMODE, &kb) == 0 && (kb == K_OFF || kb == K_RAW || kb == K_MEDIUMRAW) &&
      ioctl(fd, KDSKBMODE, K_UNICODE) != 0)
    r = -1;
  static const char show[] = "\033[?25h";
  if (write(fd, show, sizeof show - 1) < 0) {}
  close(fd);
  return r;
}

double sys_cpu_temp(void) {
  FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
  if (!f) return NAN;
  long v = 0;
  int ok = fscanf(f, "%ld", &v) == 1;
  fclose(f);
  return ok ? v / 1000.0 : NAN;
}

void sys_ip_address(char *ip, size_t iplen, char *iface, size_t ifacelen) {
  if (ip && iplen) ip[0] = 0;
  if (iface && ifacelen) iface[0] = 0;
  struct ifaddrs *ifs = NULL, *i;
  if (getifaddrs(&ifs) != 0) return;
  /* prefer wlan, then eth, then anything */
  for (int pass = 0; pass < 3; pass++) {
    for (i = ifs; i; i = i->ifa_next) {
      if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
      if (pass == 0 && strncmp(i->ifa_name, "wlan", 4)) continue;
      if (pass == 1 && strncmp(i->ifa_name, "eth", 3) && strncmp(i->ifa_name, "en", 2)) continue;
      struct sockaddr_in *a = (struct sockaddr_in *)i->ifa_addr;
      if (ip) inet_ntop(AF_INET, &a->sin_addr, ip, (socklen_t)iplen);
      if (iface) plat_strlcpy(iface, i->ifa_name, ifacelen);
      freeifaddrs(ifs);
      return;
    }
  }
  freeifaddrs(ifs);
}
#else
void sys_realtime(int prio, int cpu, char *msg, size_t msglen) {
  (void)prio; (void)cpu;
  if (msg && msglen) msg[0] = 0;
}
void sys_console_graphics(const char *tty) { (void)tty; }
void sys_console_restore(void) {}
void sys_console_refresh(void) {}
bool sys_console_is_graphics(void) { return false; }
bool sys_console_foreground(void) { return true; }
int  sys_console_reset(const char *tty) { (void)tty; return -1; }
double sys_cpu_temp(void) { return NAN; }
void sys_ip_address(char *ip, size_t iplen, char *iface, size_t ifacelen) {
  if (ip && iplen) plat_strlcpy(ip, "127.0.0.1", iplen);
  if (iface && ifacelen) plat_strlcpy(iface, "lo", ifacelen);
}
#endif

static void on_quit(int sig) {
  if (quit_flag) {
    /* a second signal: the loop didn't stop in time; restore the console and terminate */
    sys_console_restore();
    signal(sig, SIG_DFL);
    raise(sig);
    return;
  }
  quit_flag = 1;
}

volatile int *sys_quit_flag(void) {
#ifdef __linux__
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_quit;
  sa.sa_flags = 0;                  /* no SA_RESTART: ppoll/poll return EINTR at once */
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);
#else
  signal(SIGINT, on_quit);
  signal(SIGTERM, on_quit);
#endif
  return &quit_flag;
}
