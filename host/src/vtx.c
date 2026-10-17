/* VTX control: the engine (find the VTX, read it, set it like the settings, keep it so), the lines
 * (a PL011 UART or any tty on Linux, TCP for tests) and the thread. See vtx.h. */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "vtx.h"
#include "vtx_soft.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif
#ifndef _WIN32
#include <netinet/tcp.h>
#include <sys/select.h>
#endif

#define SA_TIMEOUT_MS 300          /* a reply comes within 100 ms of the frame (spec), plus its own 40 ms */
#define TRAMP_TIMEOUT_MS 200
#define TRAMP_SET_WAIT_MS 200      /* Tramp doesn't answer set commands: read the state again after this */
#define POLL_SA_MS 2000            /* reading the state while it is set right (a VTX button press shows) */
#define POLL_TRAMP_MS 1000
#define LOST_AFTER 3               /* polls in a row without an answer: search again */
#define TRIES_SA 5                 /* sending one setting without the VTX taking it, then an error */
#define TRIES_TRAMP 10             /* (some Tramp VTXes miss requests) */
#define RETRY_FAILED_MS 30000      /* a setting the VTX didn't take is tried again after this */
#define NOT_FOUND_WAIT_MS 2000
#define REOPEN_MS 2000
#define PROBE_TRIES 2

/* ================================================================ names */
const char *vtx_state_name(vtx_state_t s) {
  static const char *const n[] = { "off", "no_port", "searching", "not_found", "syncing", "ready", "send_only", "error" };
  return (unsigned)s < sizeof n / sizeof n[0] ? n[s] : "error";
}
const char *vtx_proto_name(vtx_proto_t p) { return p == VTX_PROTO_SA ? "smartaudio" : p == VTX_PROTO_TRAMP ? "tramp" : ""; }
const char *vtx_mode_name(vtx_mode_t m) {
  static const char *const n[] = { "off", "auto", "smartaudio", "tramp" };
  return (unsigned)m < 4 ? n[m] : "off";
}
vtx_mode_t vtx_mode_parse(const char *s) {
  if (!s) return VTX_MODE_OFF;
  if (!strcmp(s, "auto")) return VTX_MODE_AUTO;
  if (!strcmp(s, "smartaudio")) return VTX_MODE_SA;
  if (!strcmp(s, "tramp")) return VTX_MODE_TRAMP;
  return VTX_MODE_OFF;
}

static void fmt(char *out, size_t n, const char *f, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 3, 4)))
#endif
  ;
static void fmt(char *out, size_t n, const char *f, ...) {
  va_list ap;
  va_start(ap, f);
  vsnprintf(out, n, f, ap);
  va_end(ap);
}

/* ================================================================ configuration */
void vtx_config_defaults(vtx_config_t *c) {
  memset(c, 0, sizeof *c);
  c->mode = VTX_MODE_OFF;
  plat_strlcpy(c->port, "gpio4", sizeof c->port);
  c->channel = 1;
}

void vtx_config_from_settings(vtx_config_t *c, const settings_t *s) {
  c->mode = vtx_mode_parse(settings_get(s, "vtx"));
  const char *p = settings_get(s, "vtx_port");
  plat_strlcpy(c->port, vtx_port_known(p) ? p : "gpio4", sizeof c->port);
  const char *b = settings_get(s, "vtx_band");
  c->band = b[0] && !b[1] ? b[0] : 0;
  if (c->band >= 'a' && c->band <= 'z') c->band = (char)(c->band - 32);
  c->channel = settings_get_int(s, "vtx_channel", 1, 1, VTX_MAX_CHANNELS);
  c->freq = settings_get_int(s, "vtx_freq", 0, 0, VTX_FREQ_MAX);
  c->power = settings_get_int(s, "vtx_power", 0, 0, VTX_MAX_POWERS);
  c->pit = settings_get_bool(s, "vtx_pit", false);
  c->send_only = settings_get_bool(s, "vtx_send_only", false);
}

int vtx_config_load(vtx_config_t *c, const char *settings_path, const char *table_path) {
  vtx_config_defaults(c);
  bool exists;
  settings_t *s = settings_load(settings_path, &exists);
  if (!s) return -1;
  vtx_config_from_settings(c, s);
  settings_free(s);
  plat_stat_t st;
  if (table_path && plat_stat(table_path, &st) == 0 && st.exists && !st.is_dir) {
    size_t len = 0;
    char *j = plat_read_file(table_path, &len, VTX_TABLE_FILE_MAX);
    if (!j) {
      fmt(c->table_error, sizeof c->table_error, "vtxtable.json can't be read (bigger than %d KB?): using the built-in table.",
          VTX_TABLE_FILE_MAX / 1024);
    } else {
      char err[160];
      if (vtx_table_parse_json(j, len, &c->table, err, sizeof err) == 0) c->have_table = true;
      else fmt(c->table_error, sizeof c->table_error, "vtxtable.json can't be used: %s The built-in table is in use.", err);
      free(j);
    }
  }
  return 0;
}

void vtx_table_path(const app_t *app, char *out, size_t n) { fmt(out, n, "%s/vtxtable.json", app->paths.base); }

/* ================================================================ UART lookup */
/* BCM2711's PL011 UARTs by device-tree node (the bus address; the ARM address is 0xfe...) */
static const struct { const char *port, *node, *dev; } UARTS[] = {
  { "uart3", "serial@7e201600", "fe201600.serial" },
  { "uart4", "serial@7e201800", "fe201800.serial" },
  { "uart5", "serial@7e201a00", "fe201a00.serial" },
};

void vtx_uart_overlay(const settings_t *s, char *out, size_t n) {
  const char *p = settings_get(s, "vtx_port");
  bool on = vtx_mode_parse(settings_get(s, "vtx")) != VTX_MODE_OFF;
  plat_strlcpy(out, on && vtx_uart_node(p) ? p : "", n);
}

bool vtx_port_known(const char *port) {
  if (!port) return false;
  if (vtx_uart_node(port)) return true;
  return !strncmp(port, "gpio", 4) && port[4] && vtx_soft_is_spec(port) && strchr(port, ':') == NULL;
}

const char *vtx_uart_node(const char *port) {
  for (size_t i = 0; port && i < sizeof UARTS / sizeof UARTS[0]; i++)
    if (!strcmp(port, UARTS[i].port)) return UARTS[i].node;
  return NULL;
}

#ifdef __linux__
static const char *uart_dev(const char *port) {
  for (size_t i = 0; port && i < sizeof UARTS / sizeof UARTS[0]; i++)
    if (!strcmp(port, UARTS[i].port)) return UARTS[i].dev;
  return NULL;
}

static bool link_ends(const char *path, const char *tail) {
  char t[512];
  ssize_t n = readlink(path, t, sizeof t - 1);
  if (n <= 0) return false;
  t[n] = 0;
  const char *b = strrchr(t, '/');
  b = b ? b + 1 : t;
  return !strcmp(b, tail);
}

int vtx_find_uart_in(const char *dir, const char *port, char *name, size_t n) {
  const char *node = vtx_uart_node(port), *dev = uart_dev(port);
  if (!node) return -1;
  DIR *d = opendir(dir);
  if (!d) return -1;
  int r = -1;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (strncmp(e->d_name, "ttyAMA", 6)) continue;
    char p[700];
    snprintf(p, sizeof p, "%s/%s/device/of_node", dir, e->d_name);
    bool hit = link_ends(p, node);
    if (!hit) {
      snprintf(p, sizeof p, "%s/%s/device", dir, e->d_name);
      hit = link_ends(p, dev);
    }
    if (hit) {
      plat_strlcpy(name, e->d_name, n);
      r = 0;
      break;
    }
  }
  closedir(d);
  return r;
}

int vtx_find_uart(const char *port, char *path, size_t n) {
  char name[64];
  if (vtx_find_uart_in("/sys/class/tty", port, name, sizeof name) != 0) return -1;
  fmt(path, n, "/dev/%s", name);
  return 0;
}
#else
int vtx_find_uart_in(const char *dir, const char *port, char *name, size_t n) { return -1; }
int vtx_find_uart(const char *port, char *path, size_t n) { return -1; }
#endif

/* ================================================================ lines */
static int64_t io_now(vtx_io_t *io) { return plat_now_us(); }

/* ---- TCP (a fake VTX in tests, on every OS) */
typedef struct { plat_sock s; char host[128]; uint16_t port; } tcp_line_t;

static int tcp_open(vtx_io_t *io, char *err, size_t errlen) {
  tcp_line_t *t = io->ud;
  t->s = plat_tcp_connect(t->host, t->port, 1000);
  if (t->s == PLAT_BADSOCK) { fmt(err, errlen, "Can't connect to %s:%u.", t->host, t->port); return -1; }
  int one = 1;
  setsockopt(t->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
  return 0;
}

static void tcp_close(vtx_io_t *io) {
  tcp_line_t *t = io->ud;
  if (t->s != PLAT_BADSOCK) plat_sock_close(t->s);
  t->s = PLAT_BADSOCK;
}

static int tcp_set_line(vtx_io_t *io, int baud, int stop) { return 0; }

static int tcp_write(vtx_io_t *io, const uint8_t *p, size_t n) {
  tcp_line_t *t = io->ud;
  while (n) {
    int k = (int)send(t->s, (const char *)p, (int)n, 0);
    if (k <= 0) return -1;
    p += k;
    n -= (size_t)k;
  }
  return 0;
}

static int tcp_wait(plat_sock s, int timeout_ms) {
  fd_set rf;
  FD_ZERO(&rf);
  FD_SET(s, &rf);
  struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
  return select((int)s + 1, &rf, NULL, NULL, &tv);
}

static int tcp_read(vtx_io_t *io, uint8_t *p, size_t n, int timeout_ms) {
  tcp_line_t *t = io->ud;
  int r = tcp_wait(t->s, timeout_ms < 0 ? 0 : timeout_ms);
  if (r < 0) return -1;
  if (r == 0) return 0;
  int k = (int)recv(t->s, (char *)p, (int)n, 0);
  return k > 0 ? k : -1;                     /* 0: the fake closed the connection */
}

static void tcp_flush(vtx_io_t *io) {
  tcp_line_t *t = io->ud;
  uint8_t b[256];
  while (tcp_wait(t->s, 0) > 0)
    if (recv(t->s, (char *)b, sizeof b, 0) <= 0) break;
}

static void tcp_destroy(vtx_io_t *io) {
  tcp_close(io);
  free(io->ud);
  free(io);
}

#ifdef __linux__
/* ---- a tty: a PL011 UART found by its device-tree node, or a path (a pty in tests) */
typedef struct { int fd; char path[256]; char port[16]; } tty_line_t;

/* termios2 (custom baud rates): not in every libc's headers; the kernel ABI of arm64 and x86 */
struct vtx_termios2 {
  unsigned int c_iflag, c_oflag, c_cflag, c_lflag;
  unsigned char c_line;
  unsigned char c_cc[19];
  unsigned int c_ispeed, c_ospeed;
};
#define VTX_TCGETS2 _IOR('T', 0x2A, struct vtx_termios2)
#define VTX_TCSETS2 _IOW('T', 0x2B, struct vtx_termios2)
#define VTX_CBAUD   0010017
#define VTX_BOTHER  0010000

static int tty_open(vtx_io_t *io, char *err, size_t errlen) {
  tty_line_t *t = io->ud;
  if (t->port[0]) {
    if (vtx_find_uart(t->port, t->path, sizeof t->path) != 0) {
      fmt(err, errlen, "%s isn't turned on (no tty for %s): the Pi turns it on when it starts with VTX control on. Reboot once.",
          t->port, vtx_uart_node(t->port));
      return -2;
    }
    fmt(io->desc, sizeof io->desc, "%s (%s)", t->path, t->port);
  }
  t->fd = open(t->path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (t->fd < 0) {
    int e = errno;
    fmt(err, errlen, "Can't open %s: %s.", t->path, strerror(e));
    return e == ENOENT ? -2 : -1;
  }
  ioctl(t->fd, TIOCEXCL);                    /* nobody else on this line (a getty, a second copy) */
  return 0;
}

static void tty_close(vtx_io_t *io) {
  tty_line_t *t = io->ud;
  if (t->fd >= 0) {
    ioctl(t->fd, TIOCNXCL);                  /* TIOCEXCL outlives our fd while another opener remains */
    close(t->fd);
  }
  t->fd = -1;
}

static speed_t std_speed(int baud) {
  switch (baud) {
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    default: return 0;
  }
}

static int tty_set_line(vtx_io_t *io, int baud, int stop) {
  tty_line_t *t = io->ud;
  struct termios tio;
  if (tcgetattr(t->fd, &tio) != 0) return -1;
  cfmakeraw(&tio);
  tio.c_cflag &= ~(tcflag_t)(CSIZE | PARENB | CSTOPB | CRTSCTS);
  tio.c_cflag |= CS8 | CLOCAL | CREAD | (stop == 2 ? CSTOPB : 0);
  tio.c_iflag &= ~(tcflag_t)(IXON | IXOFF | IXANY | INPCK | ISTRIP);
  tio.c_iflag |= IGNBRK;                     /* a low line between frames reads as nothing, not 0x00s */
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 0;
  speed_t sp = std_speed(baud);
  cfsetispeed(&tio, sp ? sp : B4800);
  cfsetospeed(&tio, sp ? sp : B4800);
  if (tcsetattr(t->fd, TCSANOW, &tio) != 0) return -1;
  if (!sp) {                                 /* 4900, 4700, ...: the PL011 divides its clock finely */
    struct vtx_termios2 t2;
    if (ioctl(t->fd, VTX_TCGETS2, &t2) != 0) return -1;
    t2.c_cflag &= ~(unsigned)VTX_CBAUD;
    t2.c_cflag |= VTX_BOTHER;
    t2.c_ispeed = t2.c_ospeed = (unsigned)baud;
    if (ioctl(t->fd, VTX_TCSETS2, &t2) != 0) return -1;
  }
  tcflush(t->fd, TCIOFLUSH);
  return 0;
}

static int tty_write(vtx_io_t *io, const uint8_t *p, size_t n) {
  tty_line_t *t = io->ud;
  while (n) {
    ssize_t k = write(t->fd, p, n);
    if (k < 0 && (errno == EAGAIN || errno == EINTR)) {
      struct pollfd pf = { t->fd, POLLOUT, 0 };
      poll(&pf, 1, 100);
      continue;
    }
    if (k <= 0) return -1;
    p += k;
    n -= (size_t)k;
  }
  tcdrain(t->fd);                            /* back when the last stop bit is out: the reply follows */
  return 0;
}

static int tty_read(vtx_io_t *io, uint8_t *p, size_t n, int timeout_ms) {
  tty_line_t *t = io->ud;
  struct pollfd pf = { t->fd, POLLIN, 0 };
  int r = poll(&pf, 1, timeout_ms < 0 ? 0 : timeout_ms);
  if (r < 0) return errno == EINTR ? 0 : -1;
  if (r == 0) return 0;
  if (pf.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
  ssize_t k = read(t->fd, p, n);
  if (k < 0) return errno == EAGAIN || errno == EINTR ? 0 : -1;
  return k == 0 ? -1 : (int)k;
}

static int tty_break(vtx_io_t *io, bool low) {
  tty_line_t *t = io->ud;
  return ioctl(t->fd, low ? TIOCSBRK : TIOCCBRK) == 0 ? 0 : -1;
}

static void tty_flush(vtx_io_t *io) {
  tty_line_t *t = io->ud;
  tcflush(t->fd, TCIFLUSH);
}

static void tty_destroy(vtx_io_t *io) {
  tty_close(io);
  free(io->ud);
  free(io);
}
#endif

vtx_io_t *vtx_io_new(const char *spec, char *err, size_t errlen) {
  if (vtx_soft_is_spec(spec)) return vtx_soft_io_new(spec, err, errlen);   /* one pin (vtx_soft.c) */
  vtx_io_t *io = calloc(1, sizeof *io);
  if (!io) { fmt(err, errlen, "out of memory"); return NULL; }
  io->now_us = io_now;
  if (!strncmp(spec, "tcp:", 4)) {
    tcp_line_t *t = calloc(1, sizeof *t);
    const char *colon = strrchr(spec + 4, ':');
    int port = colon ? atoi(colon + 1) : 0;
    if (!t || !colon || port <= 0 || port > 65535 || (size_t)(colon - spec - 4) >= sizeof t->host) {
      free(t);
      free(io);
      fmt(err, errlen, "\"%s\" isn't tcp:HOST:PORT.", spec);
      return NULL;
    }
    memcpy(t->host, spec + 4, (size_t)(colon - spec - 4));
    t->port = (uint16_t)port;
    t->s = PLAT_BADSOCK;
    io->ud = t;
    io->open = tcp_open; io->close = tcp_close; io->set_line = tcp_set_line; io->write = tcp_write;
    io->read = tcp_read; io->flush = tcp_flush; io->destroy = tcp_destroy;
    plat_strlcpy(io->desc, spec, sizeof io->desc);
    return io;
  }
#ifdef __linux__
  bool path = !strncmp(spec, "tty:", 4), uart = vtx_uart_node(spec) != NULL;
  if (path || uart) {
    tty_line_t *t = calloc(1, sizeof *t);
    if (!t) { free(io); fmt(err, errlen, "out of memory"); return NULL; }
    t->fd = -1;
    if (path) plat_strlcpy(t->path, spec + 4, sizeof t->path);
    else plat_strlcpy(t->port, spec, sizeof t->port);
    io->ud = t;
    io->open = tty_open; io->close = tty_close; io->set_line = tty_set_line; io->write = tty_write;
    io->read = tty_read; io->flush = tty_flush; io->destroy = tty_destroy; io->set_break = tty_break;
    plat_strlcpy(io->desc, path ? spec + 4 : spec, sizeof io->desc);
    return io;
  }
#else
  if (vtx_uart_node(spec) || !strncmp(spec, "tty:", 4)) {
    free(io);
    fmt(err, errlen, "There is no %s here: UARTs work on the Pi (Linux) only.", spec);
    return NULL;
  }
#endif
  free(io);
  fmt(err, errlen, "\"%s\" isn't a VTX port (gpio4, gpio17, gpio22, gpio23, gpio27, uart3, uart4, uart5).", spec);
  return NULL;
}

void vtx_io_free(vtx_io_t *io) {
  if (io && io->destroy) io->destroy(io);
}

/* a line that can't be (an unknown port, a UART on a PC): opening it says why */
typedef struct { char err[200]; } null_line_t;
static int null_open(vtx_io_t *io, char *err, size_t n) { plat_strlcpy(err, ((null_line_t *)io->ud)->err, n); return -1; }
static void null_destroy(vtx_io_t *io) { free(io->ud); free(io); }
static vtx_io_t *null_io_new(const char *spec, const char *why) {
  vtx_io_t *io = calloc(1, sizeof *io);
  null_line_t *l = calloc(1, sizeof *l);
  if (!io || !l) { free(io); free(l); return NULL; }
  plat_strlcpy(l->err, why, sizeof l->err);
  io->ud = l;
  io->open = null_open;
  io->destroy = null_destroy;
  io->now_us = io_now;
  plat_strlcpy(io->desc, spec, sizeof io->desc);
  return io;
}

/* ================================================================ the engine */
/* how to talk on the line: protocol, baud, stop bits, a 0x00 before / after each SmartAudio frame,
 * the line held low (a break) between frames */
typedef struct { vtx_proto_t proto; int baud, stop; bool lead, trail, low; } cand_t;
#define SA_(b, st, l, t, lo) { VTX_PROTO_SA, b, st, l, t, lo }
#define TR_(b) { VTX_PROTO_TRAMP, b, 1, false, false, false }
/* auto: SmartAudio the INAV/ELRS/PX4 way (idle high, 0x00 first), Tramp; then SmartAudio with the line
 * low between frames and no lead-in (Betaflight's pull-down way), with neither, with a 0x00 after the
 * frame (AKK/RDQ clones), with 1 stop bit (TBS Sixty9, per INAV); a Tramp a little slow (a Foxeer
 * answers at ~9400 baud, per ArduPilot); SmartAudio baud rates within the spec's +-5 % (TBS: the
 * Unify's software UART drifts with temperature); a Tramp a little fast */
static const cand_t CANDS_AUTO[] = {
  SA_(4800, 2, true, false, false), TR_(9600), SA_(4800, 2, false, false, true), SA_(4800, 2, false, false, false),
  SA_(4800, 2, true, true, false), SA_(4800, 1, true, false, false), TR_(9400), SA_(4900, 2, true, false, false),
  SA_(4700, 2, true, false, false), SA_(5000, 2, true, false, false), SA_(4600, 2, true, false, false), TR_(9800),
};
static const cand_t CANDS_SA[] = {
  SA_(4800, 2, true, false, false), SA_(4800, 2, false, false, true), SA_(4800, 2, false, false, false),
  SA_(4800, 2, true, true, false), SA_(4800, 1, true, false, false), SA_(4900, 2, true, false, false),
  SA_(4700, 2, true, false, false), SA_(5000, 2, true, false, false), SA_(4600, 2, true, false, false),
};
static const cand_t CANDS_TRAMP[] = { TR_(9600), TR_(9400), TR_(9800) };

static bool same_cand(const cand_t *a, const cand_t *b) {
  return a->proto == b->proto && a->baud == b->baud && a->stop == b->stop && a->lead == b->lead && a->trail == b->trail &&
         a->low == b->low;
}

enum { PH_IDLE, PH_DETECT, PH_ONLINE, PH_SEND_ONLY };
enum { AP_FREQ, AP_POWER, AP_PIT, AP_N };
enum { ECHO_NONE, ECHO_OK, ECHO_MISSING, ECHO_BAD };

typedef struct {
  bool freq_set, by_channel;
  int freq, index, band, channel;
  bool power_set, power_dbm;
  int power_value, power_level;
  bool pit;
  char err[200];
} want_t;

struct vtx_engine {
  vtx_io_t *io;
  bool io_open, cur_low;
  int cur_baud, cur_stop;
  vtx_proto_t cur_proto;                      /* the protocol the line was told (vtx_io_t.set_proto) */
  int64_t reopen_at;
  vtx_config_t cfg;
  bool configured;
  int phase;
  /* searching */
  const cand_t *cands;
  int ncands, ci, tries, round_start;
  cand_t good;
  bool have_good, alt;
  int round_echo[4];
  /* found */
  cand_t cur;
  vtx_sa_settings_t sa;
  bool have_sa;
  vtx_tramp_reply_t lim, tv;
  bool have_lim, have_tv;
  int fails, polls;
  bool poll_now;
  int64_t next_poll;
  struct { int tries; uint32_t seen; bool failed; int64_t retry_at; } ap[AP_N];
  /* send-only */
  uint8_t soq[6][TRAMP_FRAME + 4];
  size_t soq_len[6];
  int soq_n, soq_i, so_rounds;
  vtx_status_t st;
  vtx_sa_parser_t sap;
  vtx_tramp_parser_t trp;
};

vtx_engine_t *vtx_engine_new(void) {
  vtx_engine_t *e = calloc(1, sizeof *e);
  if (!e) return NULL;
  vtx_config_defaults(&e->cfg);
  e->st.state = VTX_ST_OFF;
  e->st.band = -1;
  e->st.power_value = -1;
  plat_strlcpy(e->st.message, "VTX control is off.", sizeof e->st.message);
  return e;
}

static int64_t now_us(vtx_engine_t *e) { return e->io && e->io->now_us ? e->io->now_us(e->io) : plat_now_us(); }

static void close_line(vtx_engine_t *e) {
  if (e->io && e->io_open && e->cur_low && e->io->set_break) e->io->set_break(e->io, false);
  if (e->io && e->io_open && e->io->close) e->io->close(e->io);
  e->io_open = false;
  e->cur_low = false;
  e->cur_baud = e->cur_stop = 0;
  e->cur_proto = VTX_PROTO_NONE;
}

static void io_sleep(vtx_engine_t *e, int64_t us) {
  if (e->io && e->io->sleep_us) e->io->sleep_us(e->io, us);
  else plat_sleep_us(us);
}

void vtx_engine_free(vtx_engine_t *e) {
  if (!e) return;
  close_line(e);
  free(e);
}

static void set_state(vtx_engine_t *e, vtx_state_t s, const char *f, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 3, 4)))
#endif
  ;
static void set_state(vtx_engine_t *e, vtx_state_t s, const char *f, ...) {
  e->st.state = s;
  va_list ap;
  va_start(ap, f);
  vsnprintf(e->st.message, sizeof e->st.message, f, ap);
  va_end(ap);
}

static void forget_device(vtx_engine_t *e) {
  e->have_sa = e->have_lim = e->have_tv = false;
  memset(&e->sa, 0, sizeof e->sa);
  e->cur.proto = VTX_PROTO_NONE;
  e->fails = e->polls = 0;
  e->st.found = e->st.in_sync = false;
  e->st.proto = VTX_PROTO_NONE;
  e->st.sa_version = 0;
  e->st.freq = 0;
  e->st.band = -1;
  e->st.channel = 0;
  e->st.power_level = 0;
  e->st.power_value = -1;
  e->st.power_mw = 0;
  e->st.pit = e->st.pit_at_boot = e->st.locked = e->st.have_temp = false;
  e->st.min_freq = e->st.max_freq = e->st.max_power = 0;
  memset(e->ap, 0, sizeof e->ap);
}

static void start_search(vtx_engine_t *e) {
  forget_device(e);
  e->phase = PH_DETECT;
  if (e->cfg.mode == VTX_MODE_SA) { e->cands = CANDS_SA; e->ncands = (int)(sizeof CANDS_SA / sizeof CANDS_SA[0]); }
  else if (e->cfg.mode == VTX_MODE_TRAMP) { e->cands = CANDS_TRAMP; e->ncands = (int)(sizeof CANDS_TRAMP / sizeof CANDS_TRAMP[0]); }
  else { e->cands = CANDS_AUTO; e->ncands = (int)(sizeof CANDS_AUTO / sizeof CANDS_AUTO[0]); }
  e->ci = 0;
  if (e->have_good)                           /* the variant that worked last time first */
    for (int i = 0; i < e->ncands; i++)
      if (same_cand(&e->cands[i], &e->good)) e->ci = i;
  e->round_start = e->ci;
  e->tries = 0;
  memset(e->round_echo, 0, sizeof e->round_echo);
}

/* the table in use: the file's, or the built-in one for what was found (or is configured) */
static void pick_table(vtx_engine_t *e) {
  vtx_table_t *t = &e->st.table;
  if (e->cfg.have_table) { *t = e->cfg.table; return; }
  vtx_proto_t p = e->cur.proto;
  if (p == VTX_PROTO_NONE) p = e->cfg.mode == VTX_MODE_TRAMP ? VTX_PROTO_TRAMP : VTX_PROTO_SA;
  if (p == VTX_PROTO_TRAMP) vtx_table_default(t, VTX_TABLE_TRAMP, NULL, 0);
  else if (e->have_sa && e->sa.version == 1) vtx_table_default(t, VTX_TABLE_SA1, NULL, 0);
  else if (e->have_sa && e->sa.version == 3) vtx_table_default(t, VTX_TABLE_SA21, e->sa.levels, e->sa.nlevels);
  else vtx_table_default(t, VTX_TABLE_SA2, NULL, 0);
}

/* from the top: off, send-only, or searching */
static void restart(vtx_engine_t *e) {
  forget_device(e);
  e->so_rounds = 0;
  e->soq_n = e->soq_i = 0;
  e->phase = e->cfg.mode == VTX_MODE_OFF ? PH_IDLE : e->cfg.send_only ? PH_SEND_ONLY : PH_DETECT;
  if (e->phase == PH_DETECT) start_search(e);
}

void vtx_engine_set_io(vtx_engine_t *e, vtx_io_t *io) {
  close_line(e);
  e->io = io;
  e->reopen_at = 0;
  plat_strlcpy(e->st.device, io ? io->desc : "", sizeof e->st.device);
  if (e->configured) restart(e);
}

static bool same_line(const vtx_config_t *a, const vtx_config_t *b) {
  return a->mode == b->mode && a->send_only == b->send_only && !strcmp(a->port, b->port);
}

void vtx_engine_configure(vtx_engine_t *e, const vtx_config_t *c) {
  bool again = !e->configured || !same_line(&e->cfg, c);
  e->cfg = *c;
  e->configured = true;
  e->st.cfg = *c;
  e->st.mode = c->mode;
  memset(e->ap, 0, sizeof e->ap);            /* new settings: every one gets fresh tries */
  e->st.error[0] = 0;
  e->so_rounds = 0;
  e->soq_n = e->soq_i = 0;
  e->poll_now = true;
  if (again) {
    close_line(e);
    e->reopen_at = 0;
    restart(e);
  }
  pick_table(e);
}

void vtx_engine_status(const vtx_engine_t *e, vtx_status_t *out) { *out = e->st; }

/* ---------------------------------------------------------------- the line */
static int line(vtx_engine_t *e, const cand_t *c) {
  if (!e->io) {
    set_state(e, VTX_ST_ERROR, "\"%s\" isn't a VTX port (gpio4, gpio17, gpio22, gpio23, gpio27, uart3, uart4, uart5).", e->cfg.port);
    return -1;
  }
  int64_t now = now_us(e);
  if (!e->io_open) {
    if (now < e->reopen_at) return -1;
    char err[200] = "";
    int r = e->io->open(e->io, err, sizeof err);
    if (r != 0) {
      e->reopen_at = now + (int64_t)REOPEN_MS * 1000;
      set_state(e, r == -2 ? VTX_ST_NO_PORT : VTX_ST_ERROR, "%s", err[0] ? err : "Can't open the VTX port.");
      plat_strlcpy(e->st.error, e->st.message, sizeof e->st.error);
      return -1;
    }
    e->io_open = true;
    e->cur_low = false;
    e->cur_baud = e->cur_stop = 0;
    e->cur_proto = VTX_PROTO_NONE;
    plat_strlcpy(e->st.device, e->io->desc, sizeof e->st.device);
  }
  if (e->io->set_proto && e->cur_proto != c->proto) {   /* (a single pin: the bias, the idle level) */
    e->io->set_proto(e->io, c->proto);
    e->cur_proto = c->proto;
  }
  if (e->cur_baud != c->baud || e->cur_stop != c->stop) {
    if (e->io->set_line(e->io, c->baud, c->stop) != 0) return -2;   /* this baud rate can't be: skip it */
    e->cur_baud = c->baud;
    e->cur_stop = c->stop;
  }
  if (e->cur_low != c->low) {
    if (!e->io->set_break || e->io->set_break(e->io, c->low) != 0) return -2;   /* (TCP has no break) */
    e->cur_low = c->low;
  }
  e->st.baud = c->baud;
  return 0;
}

static void line_text(const cand_t *c, char *out, size_t n) {
  if (c->proto == VTX_PROTO_TRAMP) { fmt(out, n, "%d baud 8N1", c->baud); return; }
  fmt(out, n, "%d baud 8N%d%s%s%s", c->baud, c->stop, c->low ? ", line low between frames" : c->lead ? ", a 0x00 before each frame" : "",
      c->trail ? ", a 0x00 after it (clone)" : "", !c->low && !c->lead ? ", no lead-in" : "");
}

static void line_lost(vtx_engine_t *e) {
  close_line(e);
  e->reopen_at = now_us(e) + (int64_t)REOPEN_MS * 1000;
  bool was = e->phase == PH_ONLINE;
  if (e->phase == PH_ONLINE) start_search(e);
  set_state(e, VTX_ST_SEARCHING, "The VTX line closed%s: opening it again.", was ? " (lost the VTX)" : "");
}

/* the echo of what we sent, at the start of what comes back (one wire: RX hears TX) */
typedef struct {
  const uint8_t *tx;
  size_t n, i, nheld;
  int state;                                  /* 0 matching, 1 complete, 2 broke off */
  uint8_t held[64];
} echo_t;

static size_t echo_feed(echo_t *x, uint8_t b, uint8_t *out) {
  if (x->state == 0) {
    if (x->i < x->n && b == x->tx[x->i]) {
      if (x->nheld < sizeof x->held) x->held[x->nheld++] = b;
      if (++x->i == x->n) { x->state = 1; x->nheld = 0; }
      return 0;
    }
    if (x->i == 0 && x->n > 1 && x->tx[0] == 0 && b == x->tx[1]) {   /* the 0x00 lead-in didn't come back */
      x->held[x->nheld++] = b;
      x->i = 2;
      if (x->i == x->n) { x->state = 1; x->nheld = 0; }
      return 0;
    }
    x->state = 2;                             /* not (all) our echo: what was held may be a reply's start */
    size_t k = x->nheld;
    memcpy(out, x->held, k);
    x->nheld = 0;
    out[k++] = b;
    return k;
  }
  out[0] = b;
  return 1;
}

static int echo_result(const echo_t *x, bool got_reply, bool saw_own_frame, size_t received) {
  if (x->state == 1 || saw_own_frame) return ECHO_OK;
  if (x->i == 0 && (received == 0 || got_reply)) return ECHO_MISSING;
  if (got_reply) return ECHO_MISSING;          /* the reply came, our bytes didn't */
  return ECHO_BAD;
}

static void note_echo(vtx_engine_t *e, int r) {
  if (r == ECHO_OK) e->st.echo_ok++;
  else if (r == ECHO_MISSING) e->st.echo_missing++;
  else if (r == ECHO_BAD) e->st.echo_bad++;
  if (r >= 0 && r < 4) e->round_echo[r]++;
}

/* Send a SmartAudio frame (0x00 first, and after it for the clone variant) and wait for the reply to
 * cmd (SA_GET_SETTINGS takes the v1/v2/v2.1 replies). 1 reply in *f, 0 none, -1 the line is gone. */
static int sa_xfer(vtx_engine_t *e, const uint8_t *frame, size_t n, int cmd, vtx_sa_frame_t *f) {
  uint8_t tx[SA_FRAME_MAX + 4];
  size_t txn = 0;
  if (e->cur.lead) tx[txn++] = 0x00;
  memcpy(tx + txn, frame, n);
  txn += n;
  if (e->cur.trail) tx[txn++] = 0x00;
  e->io->flush(e->io);
  vtx_sa_parser_reset(&e->sap);
  bool low = e->cur_low && e->io->set_break;
  if (low) {                                  /* the line has been low: high (mark) for ~10 bits, then the frame */
    e->io->set_break(e->io, false);
    io_sleep(e, 2000);
  }
  int wr = e->io->write(e->io, tx, txn);
  if (low) e->io->set_break(e->io, true);     /* low again at once: the VTX answers from there */
  if (wr != 0) return -1;
  e->st.sent++;
  echo_t x = { tx, txn, 0, 0, 0, { 0 } };
  bool got = false, own = false;
  size_t received = 0;
  int64_t deadline = now_us(e) + (int64_t)SA_TIMEOUT_MS * 1000;
  while (!got) {
    int64_t left = (deadline - now_us(e)) / 1000;
    if (left <= 0) break;
    uint8_t rx[128];
    int k = e->io->read(e->io, rx, sizeof rx, (int)left);
    if (k < 0) return -1;
    if (k == 0) break;
    received += (size_t)k;
    for (int i = 0; i < k && !got; i++) {
      uint8_t out[80];
      size_t m = echo_feed(&x, rx[i], out);
      for (size_t j = 0; j < m && !got; j++) {
        vtx_sa_frame_t fr;
        if (!vtx_sa_parser_feed(&e->sap, out[j], &fr)) continue;
        if (fr.host) {                         /* a host frame: ours, late or shifted */
          if ((size_t)fr.n + 5 == n && !memcmp(fr.payload, frame + 4, fr.n) && fr.cmd == frame[2]) own = true;
          continue;
        }
        bool want = cmd == SA_GET_SETTINGS ? (fr.cmd == SA_GET_SETTINGS || fr.cmd == SA_SETTINGS_V2 || fr.cmd == SA_SETTINGS_V21)
                                           : fr.cmd == cmd;
        if (want) { *f = fr; got = true; }
      }
    }
  }
  e->st.bad_frames += e->sap.bad_crc + e->sap.bad_len;
  note_echo(e, echo_result(&x, got, own, received));
  if (got) e->st.replies++;
  else e->st.timeouts++;
  return got ? 1 : 0;
}

/* Send a Tramp frame; for a query ('r', 'v', 's') wait for its reply. 1 reply, 0 none, -1 gone. */
static int tramp_xfer(vtx_engine_t *e, char cmd, uint16_t param, vtx_tramp_reply_t *r) {
  uint8_t tx[TRAMP_FRAME];
  vtx_tramp_frame(tx, cmd, param);
  bool query = cmd == 'r' || cmd == 'v' || cmd == 's';
  e->io->flush(e->io);
  vtx_tramp_parser_reset(&e->trp);
  if (e->io->write(e->io, tx, TRAMP_FRAME) != 0) return -1;
  e->st.sent++;
  echo_t x = { tx, TRAMP_FRAME, 0, 0, 0, { 0 } };
  bool got = false, own = false;
  size_t received = 0;
  int64_t deadline = now_us(e) + (int64_t)(query ? TRAMP_TIMEOUT_MS : 30) * 1000;
  while (!got) {
    int64_t left = (deadline - now_us(e)) / 1000;
    if (left <= 0) break;
    uint8_t rx[128];
    int k = e->io->read(e->io, rx, sizeof rx, (int)left);
    if (k < 0) return -1;
    if (k == 0) break;
    received += (size_t)k;
    for (int i = 0; i < k && !got; i++) {
      uint8_t out[80];
      size_t m = echo_feed(&x, rx[i], out);
      for (size_t j = 0; j < m && !got; j++) {
        uint8_t fr[TRAMP_FRAME];
        if (!vtx_tramp_parser_feed(&e->trp, out[j], fr)) continue;
        if (!memcmp(fr, tx, TRAMP_FRAME)) { own = true; continue; }   /* our own frame, shifted */
        vtx_tramp_reply_t rr;
        if (query && fr[1] == (uint8_t)cmd && vtx_tramp_decode(fr, &rr) == 0) { *r = rr; got = true; }
      }
    }
  }
  e->st.bad_frames += e->trp.bad_sum;
  note_echo(e, echo_result(&x, got, own, received));
  if (query) {
    if (got) e->st.replies++;
    else e->st.timeouts++;
  }
  return got ? 1 : 0;
}

/* ---------------------------------------------------------------- what the VTX is set to */
static const char *proto_desc(const vtx_engine_t *e, char *buf, size_t n) {
  if (e->cur.proto == VTX_PROTO_TRAMP) return "Tramp";
  if (e->cur.proto == VTX_PROTO_SA) {
    int v = e->have_sa ? e->sa.version : 0;
    fmt(buf, n, "SmartAudio%s", v == 1 ? " 1.0" : v == 2 ? " 2.0" : v == 3 ? " 2.1" : "");
    return buf;
  }
  return e->cfg.mode == VTX_MODE_TRAMP ? "Tramp" : "SmartAudio";
}

static int sa_power_now(const vtx_engine_t *e, bool dbm) { return dbm ? e->sa.dbm : e->sa.power; }

/* the frequency the SmartAudio VTX is on: by channel number through the table, else its own figure */
static int sa_freq_now(const vtx_engine_t *e) {
  if (e->sa.mode & SA_MODE_FREQ) return e->sa.freq;
  const vtx_table_t *t = &e->st.table;
  int nch = t->nchannels > 0 ? t->nchannels : 8;
  int b = e->sa.channel / nch, c = e->sa.channel % nch + 1;
  uint16_t f = b < t->nbands && t->band[b].factory ? vtx_table_freq(t, b, c) : 0;
  return f ? f : e->sa.freq;
}

/* SmartAudio 2.1 takes power in dBm. A table file with level numbers (0..9) means the old 2.0 levels
 * (a 2.1 VTX takes those too), unless the VTX itself offers that dBm value. */
static bool sa_uses_dbm(const vtx_engine_t *e, int value) {
  if (!e->have_sa || e->sa.version != 3) return false;
  if (!e->cfg.have_table || value >= 10) return true;
  for (int i = 0; i < e->sa.nlevels; i++)
    if (e->sa.levels[i] == value) return true;
  return false;
}

/* mW from a table label: "25" -> 25, "1W" -> 1000, "1W5" -> 1500; 0 when it isn't one */
static int label_mw(const char *l) {
  if (l[0] < '0' || l[0] > '9') return 0;
  int n = atoi(l);
  const char *w = strpbrk(l, "Ww");
  if (!w) return n;
  return n * 1000 + (w[1] >= '0' && w[1] <= '9' ? (w[1] - '0') * 100 : 0);
}

static int sa_mw(const vtx_engine_t *e, int level) {
  static const int std[4] = { 25, 200, 500, 800 };     /* the spec's table for 1.0 and 2.0 */
  const vtx_table_t *t = &e->st.table;
  if (e->sa.version == 3 && e->sa.dbm > 0) return vtx_dbm_to_mw(e->sa.dbm);
  if (level > 0 && level <= t->npowers && label_mw(t->power[level - 1].label)) return label_mw(t->power[level - 1].label);
  if (e->sa.version == 2 && e->sa.power <= 3) return std[e->sa.power];
  return 0;
}

static void update_status(vtx_engine_t *e) {
  vtx_status_t *s = &e->st;
  const vtx_table_t *t = &s->table;
  s->proto = e->cur.proto;
  s->baud = e->cur.baud;
  s->clone_zero = e->cur.trail;
  line_text(&e->cur, s->line_desc, sizeof s->line_desc);
  s->band = -1;
  s->channel = 0;
  if (e->cur.proto == VTX_PROTO_SA && e->have_sa) {
    s->sa_version = e->sa.version;
    s->freq = sa_freq_now(e);
    s->freq_mode = (e->sa.mode & SA_MODE_FREQ) != 0;
    int nch = t->nchannels > 0 ? t->nchannels : 8;
    int b = e->sa.channel / nch;
    if (!s->freq_mode && b < t->nbands && t->band[b].factory && vtx_table_freq(t, b, e->sa.channel % nch + 1)) {
      s->band = b;
      s->channel = e->sa.channel % nch + 1;
    } else vtx_table_find_freq(t, (uint16_t)s->freq, &s->band, &s->channel);
    bool dbm = e->sa.version == 3;
    s->power_value = sa_power_now(e, dbm);
    s->power_level = vtx_table_power_level(t, s->power_value);
    if (!s->power_level && dbm) s->power_level = vtx_table_power_level(t, e->sa.power);   /* a level-number table */
    s->power_mw = sa_mw(e, s->power_level);
    s->pit = (e->sa.mode & SA_MODE_PIT) != 0;
    s->pit_at_boot = (e->sa.mode & (SA_MODE_PIR | SA_MODE_POR)) != 0;
    s->locked = e->sa.version >= 2 && !(e->sa.mode & SA_MODE_UNLOCKED);
  } else if (e->cur.proto == VTX_PROTO_TRAMP && e->have_tv) {
    s->sa_version = 0;
    s->freq = e->tv.freq;
    s->freq_mode = true;
    vtx_table_find_freq(t, e->tv.freq, &s->band, &s->channel);
    s->power_value = e->tv.power;
    s->power_level = vtx_table_power_level(t, e->tv.power);
    s->power_mw = e->tv.power;
    s->pit = e->tv.pit != 0;
    s->pit_at_boot = false;
    s->locked = (e->tv.control & TRAMP_LOCKED) != 0;
    if (e->have_lim) { s->min_freq = e->lim.min_freq; s->max_freq = e->lim.max_freq; s->max_power = e->lim.max_power; }
  }
  s->updated_us = now_us(e);
}

static void where_text(const vtx_table_t *t, int band, int channel, int freq, char *out, size_t n) {
  if (band >= 0 && band < t->nbands && channel > 0) fmt(out, n, "%c%d %d MHz", t->band[band].letter, channel, freq);
  else if (freq > 0) fmt(out, n, "%d MHz", freq);
  else fmt(out, n, "an unknown frequency");
}

static void power_text(const vtx_table_t *t, int level, int mw, char *out, size_t n) {
  if (level > 0 && level <= t->npowers) {
    const char *l = t->power[level - 1].label;
    bool digits = l[0] >= '0' && l[0] <= '9' && !strpbrk(l, "Ww");
    fmt(out, n, "%s%s", l, digits ? " mW" : "");
  } else if (mw > 0) fmt(out, n, "%d mW", mw);
  else fmt(out, n, "unknown power");
}

/* what the settings ask of this VTX with this table */
static void compute_want(const vtx_engine_t *e, want_t *w) {
  memset(w, 0, sizeof *w);
  w->band = -1;
  const vtx_table_t *t = &e->st.table;
  const vtx_config_t *c = &e->cfg;
  if (c->freq > 0) {
    w->freq_set = true;
    w->freq = c->freq;
    vtx_table_find_freq(t, (uint16_t)c->freq, &w->band, &w->channel);
  } else if (c->band) {
    int b = vtx_table_band(t, c->band);
    uint16_t f = b >= 0 ? vtx_table_freq(t, b, c->channel) : 0;
    if (b < 0) fmt(w->err, sizeof w->err, "Band %c isn't in the VTX table.", c->band);
    else if (!f) fmt(w->err, sizeof w->err, "Band %c has no channel %d in the VTX table.", c->band, c->channel);
    else {
      w->freq_set = true;
      w->freq = f;
      w->band = b;
      w->channel = c->channel;
      int nch = t->nchannels > 0 ? t->nchannels : 8;
      w->by_channel = e->cur.proto == VTX_PROTO_SA && t->band[b].factory && b * nch + c->channel - 1 <= 255;
      w->index = b * nch + c->channel - 1;
    }
  }
  if (w->freq_set && !w->by_channel) {
    int lo = 5000, hi = 5999;                 /* SmartAudio's range; a Tramp VTX says its own */
    if (e->cur.proto == VTX_PROTO_TRAMP && e->have_lim && e->lim.min_freq && e->lim.max_freq > e->lim.min_freq) {
      lo = e->lim.min_freq;
      hi = e->lim.max_freq;
    }
    if (w->freq < lo || w->freq > hi) {
      fmt(w->err, sizeof w->err, "%s can't be set to %d MHz (%d to %d MHz).", e->cur.proto == VTX_PROTO_TRAMP ? "This Tramp VTX" : "A SmartAudio VTX",
          w->freq, lo, hi);
      w->freq_set = false;
    }
  }
  if (c->power > 0) {
    if (c->power > t->npowers) {
      if (!w->err[0]) fmt(w->err, sizeof w->err, "Power level %d isn't in the VTX table (it has %d).", c->power, t->npowers);
    } else {
      w->power_set = true;
      w->power_level = c->power;
      w->power_value = t->power[c->power - 1].value;
      w->power_dbm = sa_uses_dbm(e, w->power_value);
      if (e->cur.proto == VTX_PROTO_TRAMP && e->have_lim && e->lim.max_power && w->power_value > e->lim.max_power) {
        if (!w->err[0]) fmt(w->err, sizeof w->err, "Power level %d (%d mW) is more than this VTX's %d mW.", c->power, w->power_value, e->lim.max_power);
        w->power_set = false;
      }
    }
  }
  w->pit = c->pit;
}

static bool sat_freq(const vtx_engine_t *e, const want_t *w) {
  if (!w->freq_set) return true;
  if (e->cur.proto == VTX_PROTO_TRAMP) return e->tv.freq == w->freq;
  if (w->by_channel) return !(e->sa.mode & SA_MODE_FREQ) && e->sa.channel == w->index;
  return sa_freq_now(e) == w->freq;
}

static bool sat_power(const vtx_engine_t *e, const want_t *w) {
  if (!w->power_set) return true;
  if (e->cur.proto == VTX_PROTO_TRAMP) return e->tv.power == w->power_value;
  if (w->pit && (e->sa.mode & SA_MODE_PIT)) return true;   /* in pit mode a power command would end it */
  return sa_power_now(e, w->power_dbm) == w->power_value;
}

static bool sat_pit(const vtx_engine_t *e, const want_t *w) {
  if (e->cur.proto == VTX_PROTO_TRAMP) return (e->tv.pit != 0) == w->pit;
  if (e->sa.version < 2) return true;          /* SmartAudio 1.0 has no pit mode */
  if (!w->pit) return !(e->sa.mode & SA_MODE_PIT);
  if (e->sa.version == 2) return (e->sa.mode & (SA_MODE_PIT | SA_MODE_PIR | SA_MODE_POR)) != 0;   /* 2.0: at the next power-up */
  return (e->sa.mode & SA_MODE_PIT) != 0;
}

/* a fingerprint of the VTX's state for one setting: a change (a button press) gives fresh tries */
static uint32_t seen_sig(const vtx_engine_t *e, int which) {
  if (e->cur.proto == VTX_PROTO_TRAMP)
    return which == AP_FREQ ? e->tv.freq : which == AP_POWER ? e->tv.power : e->tv.pit;
  if (which == AP_FREQ) return (uint32_t)e->sa.freq << 9 | (uint32_t)e->sa.channel << 1 | (e->sa.mode & SA_MODE_FREQ);
  if (which == AP_POWER) return (uint32_t)e->sa.power << 8 | (uint8_t)e->sa.dbm;
  return e->sa.mode & (SA_MODE_PIT | SA_MODE_PIR | SA_MODE_POR);
}

/* one setting command; -1 the line is gone */
static int send_setting(vtx_engine_t *e, int which, const want_t *w) {
  if (e->cur.proto == VTX_PROTO_TRAMP) {
    vtx_tramp_reply_t r;
    char cmd = which == AP_FREQ ? 'F' : which == AP_POWER ? 'P' : 'I';
    uint16_t v = which == AP_FREQ ? (uint16_t)w->freq : which == AP_POWER ? (uint16_t)w->power_value : (uint16_t)(w->pit ? 0 : 1);
    return tramp_xfer(e, cmd, v, &r) < 0 ? -1 : 0;
  }
  uint8_t fr[16];
  size_t n;
  int cmd;
  if (which == AP_FREQ) {
    n = w->by_channel ? vtx_sa_set_channel(fr, (uint8_t)w->index) : vtx_sa_set_freq(fr, (uint16_t)w->freq);
    cmd = w->by_channel ? SA_SET_CHANNEL : SA_SET_FREQ;
  } else if (which == AP_POWER) {
    n = vtx_sa_set_power(fr, (uint8_t)(w->power_value | (w->power_dbm ? SA_POWER_DBM : 0)));
    cmd = SA_SET_POWER;
  } else if (w->pit && e->sa.version == 3) {
    n = vtx_sa_set_power(fr, SA_POWER_DBM | 0);       /* 2.1: 0 dBm = pit mode now, without the boot flag */
    cmd = SA_SET_POWER;
  } else {
    n = vtx_sa_set_mode(fr, (uint8_t)((w->pit ? SA_SET_PIR : SA_SET_PIT_OFF) | SA_SET_UNLOCK));
    cmd = SA_SET_MODE;
  }
  vtx_sa_frame_t f;
  return sa_xfer(e, fr, n, cmd, &f) < 0 ? -1 : 0;
}

static const char *setting_name(int which) { return which == AP_FREQ ? "frequency" : which == AP_POWER ? "power" : "pit mode"; }

/* read the state: 1 ok, 0 no answer, -1 the line is gone */
static int poll_state(vtx_engine_t *e) {
  if (e->cur.proto == VTX_PROTO_TRAMP) {
    vtx_tramp_reply_t r;
    int k = tramp_xfer(e, 'v', 0, &r);
    if (k <= 0) return k;
    e->tv = r;
    e->have_tv = true;
    if (e->st.in_sync && ++e->polls % 5 == 1) {      /* the temperature now and then */
      vtx_tramp_reply_t t;
      int kt = tramp_xfer(e, 's', 0, &t);
      if (kt < 0) return -1;
      if (kt > 0) { e->st.temp_c = t.temp; e->st.have_temp = true; }
    }
    return 1;
  }
  uint8_t fr[8];
  size_t n = vtx_sa_get_settings(fr);
  vtx_sa_frame_t f;
  int k = sa_xfer(e, fr, n, SA_GET_SETTINGS, &f);
  if (k <= 0) return k;
  vtx_sa_settings_t s;
  if (vtx_sa_decode_settings(&f, &s) != 0) return 0;
  bool levels_changed = e->have_sa && (s.version != e->sa.version || s.nlevels != e->sa.nlevels ||
                                       memcmp(s.levels, e->sa.levels, sizeof s.levels));
  e->sa = s;
  bool first = !e->have_sa;
  e->have_sa = true;
  if (first || levels_changed) pick_table(e);
  return 1;
}

static void found(vtx_engine_t *e, const cand_t *c) {
  e->cur = *c;
  e->good = *c;
  e->have_good = true;
  e->phase = PH_ONLINE;
  e->fails = 0;
  e->polls = 0;
  e->poll_now = false;
  e->next_poll = now_us(e) + (int64_t)(c->proto == VTX_PROTO_TRAMP ? POLL_TRAMP_MS : POLL_SA_MS) * 1000;
  memset(e->ap, 0, sizeof e->ap);
  e->st.found = true;
  e->st.found_us = now_us(e);
  pick_table(e);
  update_status(e);
  char pd[32];
  set_state(e, VTX_ST_SYNCING, "Found a %s VTX.", proto_desc(e, pd, sizeof pd));
}

static int step_detect(vtx_engine_t *e) {
  const cand_t *c = &e->cands[e->ci];
  /* every other probe goes to the variant that worked before: a VTX plugged back in is found at once */
  bool good_turn = false;
  if (e->have_good && e->alt && !same_cand(c, &e->good) &&
      (e->cfg.mode == VTX_MODE_AUTO || (e->cfg.mode == VTX_MODE_SA) == (e->good.proto == VTX_PROTO_SA))) {
    c = &e->good;
    good_turn = true;
  }
  e->alt = !e->alt;
  int lr = line(e, c);
  if (lr == -1) return REOPEN_MS;
  if (lr == 0) {
    if (e->st.state != VTX_ST_NOT_FOUND) {
      char lt[96];
      line_text(c, lt, sizeof lt);
      set_state(e, VTX_ST_SEARCHING, "Looking for the VTX: %s, %s…", c->proto == VTX_PROTO_TRAMP ? "Tramp" : "SmartAudio", lt);
    }
    e->cur = *c;
    int k;
    if (c->proto == VTX_PROTO_TRAMP) {
      vtx_tramp_reply_t r;
      k = tramp_xfer(e, 'r', 0, &r);
      if (k > 0) {
        e->lim = r;
        e->have_lim = true;
        int kv = tramp_xfer(e, 'v', 0, &r);
        if (kv < 0) { line_lost(e); return REOPEN_MS; }
        if (kv > 0) { e->tv = r; e->have_tv = true; }
      }
    } else {
      e->have_sa = false;
      k = poll_state(e);
    }
    if (k < 0) { line_lost(e); return REOPEN_MS; }
    if (k > 0) {
      found(e, c);
      return 0;
    }
    e->cur.proto = VTX_PROTO_NONE;
    if (good_turn) return 20;
    if (++e->tries < PROBE_TRIES) return 50;
  } else if (good_turn) return 20;
  /* the next variant; a whole round without an answer: say why it may be */
  e->tries = 0;
  e->ci = (e->ci + 1) % e->ncands;
  if (e->ci != e->round_start) return 20;
  const char *what = e->cfg.mode == VTX_MODE_SA ? "SmartAudio" : e->cfg.mode == VTX_MODE_TRAMP ? "Tramp" : "SmartAudio and Tramp";
  int ok = e->round_echo[ECHO_OK], miss = e->round_echo[ECHO_MISSING], bad = e->round_echo[ECHO_BAD];
  if (ok >= miss && ok >= bad && ok > 0)
    set_state(e, VTX_ST_NOT_FOUND, "No answer from the VTX (tried %s). Check the wire to its SmartAudio/Tramp pad, that the VTX has power, "
              "and the protocol; or try send-only.", what);
  else if (miss >= bad)
    set_state(e, VTX_ST_NOT_FOUND, "Nothing comes back on RX (GPIO5, pin 29), not even what the Pi sends. Check that wire and its "
              "resistor; without an RX wire, turn on send-only.");
  else
    set_state(e, VTX_ST_NOT_FOUND, "What comes back on RX is garbled. Check the resistors, the ground, and that the VTX's pad "
              "isn't a 5 V one (see the guide).");
  memset(e->round_echo, 0, sizeof e->round_echo);
  return NOT_FOUND_WAIT_MS;
}

static int step_online(vtx_engine_t *e) {
  if (line(e, &e->cur) != 0) return REOPEN_MS;
  int64_t now = now_us(e);
  bool tramp = e->cur.proto == VTX_PROTO_TRAMP;
  if (e->poll_now || now >= e->next_poll) {
    int k = poll_state(e);
    if (k < 0) { line_lost(e); return REOPEN_MS; }
    if (k == 0) {
      if (++e->fails >= LOST_AFTER) {
        start_search(e);
        set_state(e, VTX_ST_SEARCHING, "The VTX stopped answering: looking for it again.");
        plat_strlcpy(e->st.error, "The VTX stopped answering (unplugged, or no power?).", sizeof e->st.error);
        return 0;
      }
      return 100;
    }
    e->fails = 0;
    e->poll_now = false;
    update_status(e);
  }
  want_t w;
  compute_want(e, &w);
  bool sat[AP_N] = { sat_freq(e, &w), sat_power(e, &w), sat_pit(e, &w) };
  int max_tries = tramp ? TRIES_TRAMP : TRIES_SA;
  char failed_msg[200] = "";
  now = now_us(e);
  for (int i = 0; i < AP_N; i++) {
    if (sat[i]) { e->ap[i].tries = 0; e->ap[i].failed = false; continue; }
    if (e->ap[i].failed) {
      /* the VTX didn't take it: again after a while, or when its state changed (a button press) */
      if (now < e->ap[i].retry_at && seen_sig(e, i) == e->ap[i].seen) {
        if (!failed_msg[0]) fmt(failed_msg, sizeof failed_msg, "The VTX didn't take the %s setting.", setting_name(i));
        continue;
      }
      e->ap[i].failed = false;
      e->ap[i].tries = 0;
    }
    if (e->ap[i].tries > 0 && seen_sig(e, i) != e->ap[i].seen) e->ap[i].tries = 0;
    if (e->ap[i].tries >= max_tries) {
      e->ap[i].failed = true;
      e->ap[i].retry_at = now + (int64_t)RETRY_FAILED_MS * 1000;
      if (!failed_msg[0]) fmt(failed_msg, sizeof failed_msg, "The VTX didn't take the %s setting.", setting_name(i));
      continue;
    }
    if (tramp && i != AP_PIT && (e->tv.control & TRAMP_LOCKED)) {
      if (!failed_msg[0]) fmt(failed_msg, sizeof failed_msg, "The VTX is race-locked: unlock it on the VTX to change the %s.", setting_name(i));
      continue;
    }
    e->ap[i].tries++;
    e->ap[i].seen = seen_sig(e, i);
    char where[48], pw[24];
    where_text(&e->st.table, w.band, w.channel, w.freq, where, sizeof where);
    power_text(&e->st.table, w.power_level, 0, pw, sizeof pw);
    if (i == AP_FREQ) set_state(e, VTX_ST_SYNCING, "Setting the VTX to %s…", where);
    else if (i == AP_POWER) set_state(e, VTX_ST_SYNCING, "Setting the VTX's power to %s…", pw);
    else set_state(e, VTX_ST_SYNCING, "Turning pit mode %s…", w.pit ? "on" : "off");
    e->st.in_sync = false;
    if (send_setting(e, i, &w) < 0) { line_lost(e); return REOPEN_MS; }
    e->poll_now = true;
    return tramp ? TRAMP_SET_WAIT_MS : 30;
  }
  /* nothing (more) to send */
  bool all = sat[0] && sat[1] && sat[2];
  e->st.in_sync = all && !w.err[0];
  char pd[32], where[48], pw[24];
  where_text(&e->st.table, e->st.band, e->st.channel, e->st.freq, where, sizeof where);
  power_text(&e->st.table, e->st.power_level, e->st.power_mw, pw, sizeof pw);
  const char *pit = e->cur.proto == VTX_PROTO_SA && e->sa.version < 2 ? "" :
                    e->st.pit ? ", pit mode on" : (e->st.pit_at_boot && e->cfg.pit) ? ", pit mode at the next power-up" : ", pit mode off";
  if (w.err[0] || failed_msg[0]) {
    plat_strlcpy(e->st.error, w.err[0] ? w.err : failed_msg, sizeof e->st.error);
    set_state(e, VTX_ST_ERROR, "%s It is on %s, %s%s.", e->st.error, where, pw, pit);
  } else {
    e->st.error[0] = 0;
    if (e->cfg.pit && e->cur.proto == VTX_PROTO_SA && e->sa.version < 2)
      plat_strlcpy(e->st.error, "SmartAudio 1.0 has no pit mode.", sizeof e->st.error);
    set_state(e, VTX_ST_READY, "%s: %s, %s%s.", proto_desc(e, pd, sizeof pd), where, pw, pit);
  }
  e->next_poll = now_us(e) + (int64_t)(tramp ? POLL_TRAMP_MS : POLL_SA_MS) * 1000;
  return tramp ? POLL_TRAMP_MS : POLL_SA_MS;
}

/* send-only: SmartAudio's power unit from the table (levels 0..3: 2.0; dBm: 2.1; DAC values: 1.0) */
static int so_sa_version(const vtx_table_t *t) {
  int mx = 0;
  bool sa1 = t->npowers == 4;
  static const int dac[4] = { 7, 16, 25, 40 };
  for (int i = 0; i < t->npowers; i++) {
    if (t->power[i].value > mx) mx = t->power[i].value;
    if (i < 4 && t->power[i].value != dac[i]) sa1 = false;
  }
  if (mx <= 3) return 2;
  if (sa1 || mx > 30) return 1;
  return 3;
}

static void so_build(vtx_engine_t *e) {
  want_t w;
  compute_want(e, &w);
  e->soq_n = e->soq_i = 0;
  if (e->cur.proto == VTX_PROTO_TRAMP) {
    if (w.freq_set) { vtx_tramp_frame(e->soq[e->soq_n], 'F', (uint16_t)w.freq); e->soq_len[e->soq_n++] = TRAMP_FRAME; }
    if (w.power_set) { vtx_tramp_frame(e->soq[e->soq_n], 'P', (uint16_t)w.power_value); e->soq_len[e->soq_n++] = TRAMP_FRAME; }
    vtx_tramp_frame(e->soq[e->soq_n], 'I', w.pit ? 0 : 1);
    e->soq_len[e->soq_n++] = TRAMP_FRAME;
  } else {
    int v = so_sa_version(&e->st.table);
    if (w.freq_set) {
      uint8_t *q = e->soq[e->soq_n];
      e->soq_len[e->soq_n++] = w.by_channel ? vtx_sa_set_channel(q, (uint8_t)w.index) : vtx_sa_set_freq(q, (uint16_t)w.freq);
    }
    if (w.power_set) {
      e->soq_len[e->soq_n] = vtx_sa_set_power(e->soq[e->soq_n], (uint8_t)(w.power_value | (v == 3 && w.power_value >= 10 ? SA_POWER_DBM : 0)));
      e->soq_n++;
    }
    if (w.pit && v == 3) e->soq_len[e->soq_n] = vtx_sa_set_power(e->soq[e->soq_n], SA_POWER_DBM);
    else e->soq_len[e->soq_n] = vtx_sa_set_mode(e->soq[e->soq_n], (uint8_t)((w.pit ? SA_SET_PIR : SA_SET_PIT_OFF) | SA_SET_UNLOCK));
    e->soq_n++;
  }
  plat_strlcpy(e->st.error, w.err, sizeof e->st.error);
}

static int step_send_only(vtx_engine_t *e) {
  if (e->cfg.mode == VTX_MODE_AUTO) {
    set_state(e, VTX_ST_ERROR, "Send-only can't find out the protocol: set VTX control to SmartAudio or Tramp.");
    return 10000;
  }
  cand_t c = e->cfg.mode == VTX_MODE_TRAMP ? (cand_t)TR_(TRAMP_BAUD) : (cand_t)SA_(SA_BAUD, 2, true, false, false);
  if (line(e, &c) != 0) return REOPEN_MS;
  line_text(&c, e->st.line_desc, sizeof e->st.line_desc);
  if (e->cur.proto != c.proto) {
    e->cur = c;
    pick_table(e);
  }
  e->st.proto = c.proto;
  e->st.baud = c.baud;
  if (e->so_rounds < 2) {
    if (e->soq_i == 0 && e->soq_n == 0) so_build(e);
    if (e->soq_i < e->soq_n) {
      uint8_t tx[TRAMP_FRAME + 6];
      size_t n = 0;
      if (c.lead) tx[n++] = 0x00;
      memcpy(tx + n, e->soq[e->soq_i], e->soq_len[e->soq_i]);
      n += e->soq_len[e->soq_i];
      e->soq_i++;
      if (e->io->write(e->io, tx, n) != 0) { line_lost(e); return REOPEN_MS; }
      e->io->flush(e->io);                   /* nothing is read in this mode */
      e->st.sent++;
      set_state(e, VTX_ST_SEND_ONLY, "Sending the settings (send-only)…");
      return 200;                            /* a SmartAudio VTX answers on the wire meanwhile: don't talk over it */
    }
    e->so_rounds++;
    e->soq_n = e->soq_i = 0;
    if (e->so_rounds < 2) return 300;
  }
  want_t w;
  compute_want(e, &w);
  char where[48], pw[24];
  where_text(&e->st.table, w.band, w.channel, w.freq, where, sizeof where);
  power_text(&e->st.table, w.power_level, 0, pw, sizeof pw);
  set_state(e, w.err[0] ? VTX_ST_ERROR : VTX_ST_SEND_ONLY, "%s%sSent (send-only, %s): %s, %s, pit mode %s. The VTX's answers aren't read, so "
            "this page can't show what it is set to.", w.err, w.err[0] ? " " : "", c.proto == VTX_PROTO_TRAMP ? "Tramp" : "SmartAudio",
            w.freq_set ? where : "frequency unchanged", w.power_set ? pw : "power unchanged", w.pit ? "on" : "off");
  return 60000;
}

int vtx_engine_step(vtx_engine_t *e) {
  if (!e->configured || e->cfg.mode == VTX_MODE_OFF) {
    close_line(e);
    forget_device(e);
    e->phase = PH_IDLE;
    set_state(e, VTX_ST_OFF, "VTX control is off.");
    return 60000;
  }
  if (e->cfg.table_error[0] && !e->st.error[0]) plat_strlcpy(e->st.error, e->cfg.table_error, sizeof e->st.error);
  int r;
  switch (e->phase) {
    case PH_SEND_ONLY: r = step_send_only(e); break;
    case PH_ONLINE: r = step_online(e); break;
    default:
      if (e->phase != PH_DETECT) start_search(e);
      r = step_detect(e);
      break;
  }
  if (e->cfg.table_error[0] && e->st.state == VTX_ST_READY) {
    e->st.state = VTX_ST_ERROR;
    plat_strlcpy(e->st.error, e->cfg.table_error, sizeof e->st.error);
  }
  if (e->io && e->io_open && e->io->stats) e->io->stats(e->io, e->st.line_stats, sizeof e->st.line_stats);
  e->st.updated_us = now_us(e);
  return r;
}

/* ================================================================ the thread */
struct vtx {
  app_t *app;
  plat_thread th;
  plat_mutex mu;
  plat_cond cv;
  bool quit, reload, started;
  char transport[256];
  vtx_status_t st;
  vtx_engine_t *eng;
  vtx_io_t *io;
};

#ifdef __linux__
/* the CPUs there are (not the inherited mask: the frame loop may have pinned the process to its core) */
static int cpus_present(void) {
  size_t len = 0;
  char *t = plat_read_file("/sys/devices/system/cpu/online", &len, 256);
  int n = 0;
  if (t) {
    const char *dash = strrchr(t, '-'), *comma = strrchr(t, ',');
    const char *last = dash && (!comma || dash > comma) ? dash + 1 : comma ? comma + 1 : t;
    n = atoi(last) + 1;
    free(t);
  }
  return n > 0 && n <= CPU_SETSIZE ? n : 1;
}

/* The VTX thread on core 1 of a 4-core Pi: the frame loop has core 3, the audio thread core 2, and most
 * interrupts go to core 0; the single-pin line's bit timing wants a quiet core. Smaller machines: any. */
static void vtx_pin_thread(void) {
  int n = cpus_present();
  cpu_set_t set;
  CPU_ZERO(&set);
  if (n >= 4) CPU_SET(1, &set);
  else for (int i = 0; i < n; i++) CPU_SET(i, &set);
  pthread_setaffinity_np(pthread_self(), sizeof set, &set);
}
#endif

static void *vtx_main(void *arg) {
  vtx_t *v = arg;
  plat_thread_lower_priority();
#ifdef __linux__
  vtx_pin_thread();
#endif
  vtx_config_t *cfg = malloc(sizeof *cfg);
  vtx_status_t *st = malloc(sizeof *st);
  char spec[256] = "\x01", table[700];
  vtx_table_path(v->app, table, sizeof table);
  int last_state = -1;
  long last_resent = 0;
  int64_t last_stats_us = plat_now_us();
  while (cfg && st) {
    plat_mutex_lock(&v->mu);
    bool quit = v->quit, reload = v->reload;
    v->reload = false;
    plat_mutex_unlock(&v->mu);
    if (quit) break;
    if (reload && vtx_config_load(cfg, v->app->paths.settings, table) == 0) {
      const char *want = v->transport[0] ? v->transport : cfg->port;
      if (strcmp(want, spec)) {
        char err[200] = "";
        vtx_engine_set_io(v->eng, NULL);
        vtx_io_free(v->io);
        v->io = cfg->mode == VTX_MODE_OFF && !v->transport[0] ? NULL : vtx_io_new(want, err, sizeof err);
        if (!v->io && err[0]) {
          app_log(v->app, "vtx: %s", err);
          v->io = null_io_new(want, err);      /* the status page says why */
        }
        if (v->io || cfg->mode == VTX_MODE_OFF) plat_strlcpy(spec, v->io ? want : "\x01", sizeof spec);
        vtx_engine_set_io(v->eng, v->io);
      }
      vtx_engine_configure(v->eng, cfg);
    }
    int wait = vtx_engine_step(v->eng);
    vtx_engine_status(v->eng, st);
    if ((int)st->state != last_state) {
      app_log(v->app, "vtx: %s: %s", vtx_state_name(st->state), st->message);
      last_state = (int)st->state;
    }
    if (st->line_stats[0]) {                    /* the single pin's timing: every 10 min, and when a frame went again */
      const char *re = strstr(st->line_stats, " sent again");
      long resent = 0;
      if (re) { const char *q = re; while (q > st->line_stats && q[-1] >= '0' && q[-1] <= '9') q--; resent = atol(q); }
      int64_t now = plat_now_us();
      if (resent != last_resent || now - last_stats_us > 600000000LL) {
        app_log(v->app, "vtx: line: %s", st->line_stats);
        last_resent = resent;
        last_stats_us = now;
      }
    }
    plat_mutex_lock(&v->mu);
    v->st = *st;
    if (!v->quit && !v->reload && wait > 0) plat_cond_wait(&v->cv, &v->mu, wait);
    plat_mutex_unlock(&v->mu);
  }
  vtx_engine_set_io(v->eng, NULL);
  free(cfg);
  free(st);
  return NULL;
}

vtx_t *vtx_open(app_t *app, const vtx_options_t *opt) {
  vtx_t *v = calloc(1, sizeof *v);
  if (!v) return NULL;
  v->app = app;
  v->eng = vtx_engine_new();
  if (!v->eng) { free(v); return NULL; }
  if (opt && opt->transport) plat_strlcpy(v->transport, opt->transport, sizeof v->transport);
  plat_mutex_init(&v->mu);
  plat_cond_init(&v->cv);
  v->reload = true;
  vtx_engine_status(v->eng, &v->st);
  if (plat_thread_start(&v->th, vtx_main, v, false) != 0) {
    vtx_engine_free(v->eng);
    plat_mutex_destroy(&v->mu);
    free(v);
    return NULL;
  }
  v->started = true;
  return v;
}

void vtx_reload(vtx_t *v) {
  if (!v) return;
  plat_mutex_lock(&v->mu);
  v->reload = true;
  plat_cond_signal(&v->cv);
  plat_mutex_unlock(&v->mu);
}

void vtx_get_status(vtx_t *v, vtx_status_t *out) {
  if (!v) {
    memset(out, 0, sizeof *out);
    out->state = VTX_ST_OFF;
    out->band = -1;
    out->power_value = -1;
    vtx_config_defaults(&out->cfg);
    vtx_table_default(&out->table, VTX_TABLE_SA2, NULL, 0);
    plat_strlcpy(out->message, "VTX control is off.", sizeof out->message);
    return;
  }
  plat_mutex_lock(&v->mu);
  *out = v->st;
  plat_mutex_unlock(&v->mu);
}

void vtx_close(vtx_t *v) {
  if (!v) return;
  plat_mutex_lock(&v->mu);
  v->quit = true;
  plat_cond_signal(&v->cv);
  plat_mutex_unlock(&v->mu);
  if (v->started) plat_thread_join(v->th);
  vtx_engine_free(v->eng);
  vtx_io_free(v->io);
  plat_mutex_destroy(&v->mu);
  free(v);
}

void vtx_status_line(const vtx_status_t *s, char *out, size_t n) {
  if (s->state == VTX_ST_OFF) { fmt(out, n, "off"); return; }
  if ((s->state == VTX_ST_READY || s->state == VTX_ST_SYNCING || s->state == VTX_ST_ERROR) && s->found) {
    char where[48], pw[24], pd[32];
    where_text(&s->table, s->band, s->channel, s->freq, where, sizeof where);
    power_text(&s->table, s->power_level, s->power_mw, pw, sizeof pw);
    if (s->proto == VTX_PROTO_TRAMP) fmt(pd, sizeof pd, "Tramp");
    else fmt(pd, sizeof pd, "SmartAudio%s", s->sa_version == 1 ? " 1.0" : s->sa_version == 2 ? " 2.0" : s->sa_version == 3 ? " 2.1" : "");
    fmt(out, n, "%s · %s · %s%s%s", pd, where, pw, s->pit ? " · pit mode" : "",
        s->state == VTX_ST_SYNCING ? " (setting)" : s->state == VTX_ST_ERROR ? " (see Video)" : "");
    return;
  }
  static const char *const words[] = { "off", "UART not on yet (reboot)", "searching", "not found", "setting", "ready", "send-only", "error" };
  if (s->state == VTX_ST_NO_PORT && !vtx_uart_node(s->cfg.port)) { fmt(out, n, "pin not available"); return; }
  fmt(out, n, "%s", (unsigned)s->state < 8 ? words[s->state] : "error");
}

/* ================================================================ line tests for a real Pi */
static vtx_io_t *test_open(const char *spec, bool tramp, FILE *out) {
  char err[200] = "";
  vtx_io_t *io = vtx_io_new(spec, err, sizeof err);
  if (!io) { fprintf(out, "%s\n", err); return NULL; }
  if (io->open(io, err, sizeof err) != 0) {
    fprintf(out, "%s\n", err);
    vtx_io_free(io);
    return NULL;
  }
  if (io->set_proto) io->set_proto(io, tramp ? VTX_PROTO_TRAMP : VTX_PROTO_SA);
  if (io->set_line(io, tramp ? TRAMP_BAUD : SA_BAUD, tramp ? 1 : 2) != 0) fprintf(out, "can't set the line's speed\n");
  return io;
}

/* bytes that arrive within ms (after the echo of what was sent, when there is one) */
static size_t test_collect(vtx_io_t *io, uint8_t *buf, size_t max, int ms) {
  size_t n = 0;
  int64_t end = plat_now_us() + (int64_t)ms * 1000;
  while (n < max) {
    int64_t left = (end - plat_now_us()) / 1000;
    if (left <= 0) break;
    int k = io->read(io, buf + n, max - n, (int)left);
    if (k <= 0) break;
    n += (size_t)k;
  }
  return n;
}

int vtx_line_test(const char *spec, const char *proto, int frames, FILE *out) {
  bool tramp = proto && !strcmp(proto, "tramp");
  vtx_io_t *io = test_open(spec, tramp, out);
  if (!io) return 2;
  fprintf(out, "%s: %s, %d frames\n", io->desc, tramp ? "Tramp 9600 8N1, 'v' queries" : "SmartAudio 4800 8N2, GET_SETTINGS", frames);
  int answered = 0;
  for (int i = 0; i < frames; i++) {
    uint8_t tx[24], rx[512];
    size_t n;
    if (tramp) { vtx_tramp_frame(tx, 'v', 0); n = TRAMP_FRAME; }
    else { tx[0] = 0x00; n = 1 + vtx_sa_get_settings(tx + 1); }
    io->flush(io);
    if (io->write(io, tx, n) != 0) { fprintf(out, "frame %d: the line failed\n", i + 1); break; }
    size_t got = test_collect(io, rx, sizeof rx, tramp ? TRAMP_TIMEOUT_MS : SA_TIMEOUT_MS);
    size_t skip = got >= n && !memcmp(rx, tx, n) ? n : 0;          /* the echo */
    bool ok = false;
    char what[160] = "no answer";
    if (tramp) {
      vtx_tramp_parser_t p;
      vtx_tramp_parser_reset(&p);
      uint8_t f[TRAMP_FRAME];
      vtx_tramp_reply_t r;
      for (size_t j = skip; j < got && !ok; j++)
        if (vtx_tramp_parser_feed(&p, rx[j], f) && memcmp(f, tx, TRAMP_FRAME) && vtx_tramp_decode(f, &r) == 0 && r.cmd == 'v') {
          ok = true;
          fmt(what, sizeof what, "Tramp: %u MHz, %u mW, pit mode %s", r.freq, r.power, r.pit ? "on" : "off");
        }
    } else {
      vtx_sa_parser_t p;
      vtx_sa_parser_reset(&p);
      vtx_sa_frame_t f;
      vtx_sa_settings_t s;
      for (size_t j = skip; j < got && !ok; j++)
        if (vtx_sa_parser_feed(&p, rx[j], &f) && !f.host && vtx_sa_decode_settings(&f, &s) == 0) {
          ok = true;
          fmt(what, sizeof what, "SmartAudio %s: channel %u, %u MHz, power %u%s", s.version == 1 ? "1.0" : s.version == 2 ? "2.0" : "2.1",
              s.channel, s.freq, s.version == 3 ? (unsigned)s.dbm : s.power, s.version == 3 ? " dBm" : "");
        }
    }
    answered += ok;
    fprintf(out, "frame %d: %s (%u bytes back%s)\n", i + 1, what, (unsigned)got, skip ? ", echo first" : "");
    plat_sleep_us(500000);
  }
  char st[200] = "";
  if (io->stats) io->stats(io, st, sizeof st);
  fprintf(out, "%d of %d frames answered%s%s\n", answered, frames, st[0] ? "\nline: " : "", st);
  io->close(io);
  vtx_io_free(io);
  return answered == frames ? 0 : 1;
}

int vtx_loopback_test(const char *tx_spec, const char *rx_spec, int frames, FILE *out) {
  int bad_total = 0, sent_total = 0;
  for (int pass = 0; pass < 2; pass++) {                  /* SmartAudio's line, then Tramp's */
    bool tramp = pass == 1;
    vtx_io_t *tx = test_open(tx_spec, tramp, out);
    if (!tx) return 2;
    vtx_io_t *rx = test_open(rx_spec, tramp, out);
    if (!rx) { tx->close(tx); vtx_io_free(tx); return 2; }
    int bad = 0, lost = 0;
    for (int i = 0; i < frames; i++) {
      uint8_t pat[20], got[512];
      static const uint8_t fixed[8] = { 0x00, 0xFF, 0x55, 0xAA, 0x0F, 0xF0, 0x01, 0x80 };
      for (int j = 0; j < 20; j++) pat[j] = j < 8 ? fixed[j] : (uint8_t)(i * 37 + j * 101);
      rx->flush(rx);
      if (tx->write(tx, pat, sizeof pat) != 0) { fprintf(out, "the TX line failed\n"); break; }
      size_t n = test_collect(rx, got, sizeof got, 60);
      int diff = n == sizeof pat ? 0 : 1;
      for (size_t j = 0; j < n && j < sizeof pat; j++) diff += got[j] != pat[j];
      if (n < sizeof pat) lost++;
      bad += diff;
      if (diff) fprintf(out, "%s frame %d: %u of 20 bytes back, %d wrong\n", tramp ? "Tramp" : "SmartAudio", i + 1, (unsigned)n, diff);
      plat_sleep_us(20000);
    }
    char ts[200] = "", rs[200] = "";
    if (tx->stats) tx->stats(tx, ts, sizeof ts);
    if (rx->stats) rx->stats(rx, rs, sizeof rs);
    fprintf(out, "%s (%d baud): %d frames, %d with lost bytes, %d byte errors\n  TX %s: %s\n  RX %s: %s\n", tramp ? "Tramp" : "SmartAudio",
            tramp ? TRAMP_BAUD : SA_BAUD, frames, lost, bad, tx->desc, ts, rx->desc, rs);
    bad_total += bad;
    sent_total += frames;
    tx->close(tx);
    rx->close(rx);
    vtx_io_free(tx);
    vtx_io_free(rx);
  }
  fprintf(out, "%s\n", bad_total ? "ERRORS: see above" : "all frames came back right");
  return bad_total || !sent_total ? 1 : 0;
}
