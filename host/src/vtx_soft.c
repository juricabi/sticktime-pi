/* Single-pin soft serial: see vtx_soft.h. */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "vtx_soft.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

/* ================================================================ the bit schedule */
int vtx_soft_schedule(const uint8_t *p, size_t n, int baud, int stop_bits, int from, vtx_edge_t *out, int max, int64_t *end_ns) {
  double bit = 1e9 / (baud > 0 ? baud : 4800);
  int level = from ? 1 : 0, k = 0;
  long slot = 0;
  for (size_t i = 0; i < n; i++) {
    int bits[12], nb = 0;
    bits[nb++] = 0;                                        /* start */
    for (int b = 0; b < 8; b++) bits[nb++] = (p[i] >> b) & 1;
    for (int s = 0; s < (stop_bits == 2 ? 2 : 1); s++) bits[nb++] = 1;
    for (int b = 0; b < nb; b++, slot++) {
      if (bits[b] == level) continue;
      if (k < max) { out[k].t_ns = (int64_t)llround(slot * bit); out[k].level = (uint8_t)bits[b]; }
      k++;
      level = bits[b];
    }
  }
  if (end_ns) *end_ns = (int64_t)llround(slot * bit);
  return k < max ? k : max;
}

/* ================================================================ the decoder */
void vtx_soft_rx_init(vtx_soft_rx_t *r, int baud, int stop_bits, int level) {
  memset(r, 0, sizeof *r);
  r->bit_ns = (int64_t)llround(1e9 / (baud > 0 ? baud : 4800));
  r->stop_bits = stop_bits == 2 ? 2 : 1;
  r->level = level ? 1 : 0;
}

static int level_at(const vtx_soft_rx_t *r, int64_t t) {
  int lv = 0;                                              /* low from the start edge */
  for (int i = 0; i < r->ne && r->e[i].t_ns <= t; i++) lv = r->e[i].level;
  return lv;
}

/* the byte in progress, sampled; 1 when it gave a byte */
static int finish(vtx_soft_rx_t *r, uint8_t *out, int max) {
  int64_t bit = r->bit_ns;
  /* re-measure the bit time on the latest clean edge: near a bit boundary, no other edge within half a bit */
  for (int i = 0; i < r->ne; i++) {
    int64_t rel = r->e[i].t_ns - r->start_ns;
    double x = (double)rel / (double)r->bit_ns;
    long nb = lround(x);
    if (nb < 1 || nb > 9 || fabs(x - (double)nb) > 0.3) continue;
    int64_t prev = i ? r->e[i - 1].t_ns : r->start_ns;
    int64_t next = i + 1 < r->ne ? r->e[i + 1].t_ns : INT64_MAX;
    if (r->e[i].t_ns - prev < r->bit_ns / 2 || next - r->e[i].t_ns < r->bit_ns / 2) continue;
    bit = rel / nb;
  }
  if (bit < r->bit_ns * 9 / 10) bit = r->bit_ns * 9 / 10;
  if (bit > r->bit_ns * 11 / 10) bit = r->bit_ns * 11 / 10;
  unsigned v = 0;
  for (int k = 0; k < 8; k++)
    if (level_at(r, r->start_ns + (int64_t)((k + 1.5) * (double)bit))) v |= 1u << k;
  int stop = level_at(r, r->start_ns + (int64_t)(9.5 * (double)bit));
  r->in_byte = false;
  r->ne = 0;
  if (!stop) {
    if (v == 0) { r->breaks++; return 0; }               /* the line went low and stayed low: it rests there */
    r->framing++;
  }
  r->bytes++;
  if (max > 0) out[0] = (uint8_t)v;
  return max > 0;
}

int64_t vtx_soft_rx_due(const vtx_soft_rx_t *r) { return r->in_byte ? r->start_ns + r->bit_ns * 21 / 2 : 0; }

int vtx_soft_rx_idle(vtx_soft_rx_t *r, int64_t now_ns, uint8_t *out, int max) {
  if (r->in_byte && now_ns >= vtx_soft_rx_due(r)) return finish(r, out, max);
  return 0;
}

int vtx_soft_rx_edge(vtx_soft_rx_t *r, int64_t t_ns, int level, uint8_t *out, int max) {
  int got = 0;
  level = level ? 1 : 0;
  if (r->in_byte) {
    int64_t rel = t_ns - r->start_ns;
    /* past the byte, or a falling edge where only the next start bit can be (9 bits: +-5 % either way) */
    if (rel >= r->bit_ns * 21 / 2 || (level == 0 && rel >= r->bit_ns * 9)) {
      got = finish(r, out, max);
      out += got;
      max -= got;
    } else {
      if (level == 1 && rel < r->bit_ns / 2) {             /* a start bit shorter than half a bit: a glitch */
        r->in_byte = false;
        r->ne = 0;
        r->glitches++;
      } else if (r->ne < VTX_SOFT_EDGES) {
        r->e[r->ne].t_ns = t_ns;
        r->e[r->ne].level = (uint8_t)level;
        r->ne++;
      }
      r->level = level;
      return got;
    }
  }
  if (level == 0) {                                        /* a start bit (also after a missed rising edge) */
    r->in_byte = true;
    r->start_ns = t_ns;
    r->ne = 0;
  }
  r->level = level;
  return got;
}

/* ================================================================ ports */
static const struct { int gpio, pin; } PINS[] = { { 4, 7 }, { 17, 11 }, { 22, 15 }, { 23, 16 }, { 27, 13 } };

int vtx_soft_header_pin(int gpio) {
  for (size_t i = 0; i < sizeof PINS / sizeof PINS[0]; i++)
    if (PINS[i].gpio == gpio) return PINS[i].pin;
  return 0;
}

/* "gpioN" with N one of ours -> N; else -1 */
static int gpio_of(const char *spec) {
  if (strncmp(spec, "gpio", 4) || !spec[4]) return -1;
  char *end;
  long n = strtol(spec + 4, &end, 10);
  if (*end || n < 0 || n > 63 || !vtx_soft_header_pin((int)n)) return -1;
  return (int)n;
}

bool vtx_soft_is_spec(const char *spec) { return gpio_of(spec) >= 0 || !strncmp(spec, "gpio:", 5); }

static void sfmt(char *out, size_t n, const char *f, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 3, 4)))
#endif
  ;
static void sfmt(char *out, size_t n, const char *f, ...) {
  va_list ap;
  va_start(ap, f);
  vsnprintf(out, n, f, ap);
  va_end(ap);
}

#define SOFT_RT_PRIO_DEFAULT 40
static int soft_rt_prio = SOFT_RT_PRIO_DEFAULT;
void vtx_soft_set_rt_prio(int prio) { soft_rt_prio = prio > 0 && prio < 100 ? prio : SOFT_RT_PRIO_DEFAULT; }

#ifdef __linux__
/* ================================================================ the GPIO line (Linux character device, v2) */
#define SOFT_QUEUE 1024

typedef struct {
  int gpio;                             /* BCM number, -1 for a "gpio:" spec */
  char chip[96];                        /* /dev/gpiochipN */
  unsigned offset;
  int chip_fd, line_fd;
  volatile uint32_t *regs;              /* the GPIO block (/dev/gpiomem) on a BCM283x/2711; NULL = the ioctls */
  int64_t tx_end_ns;                    /* events up to here are the sender's own edges */
  bool irq_prio_set;                    /* the edge interrupt's thread runs above the sender */
  int baud, stop;
  vtx_proto_t proto;
  vtx_soft_rx_t rx;
  uint8_t q[SOFT_QUEUE];
  int qn;
  /* statistics */
  uint32_t frames, resent, late_frames, events, overflow;   /* late_frames: still late after the retries */
  uint32_t own_seen, own_expect;        /* the sender's own edges that came back as events, and all it drove */
  int64_t late_sum_ns, late_max_ns, late_last_max_ns;
  uint32_t late_n, late_q, late_h;      /* edges late by more than a quarter bit / half a bit */
  int64_t sw_out_max_ns, sw_in_max_ns;  /* the longest switch of the pin to output / back to input */
  bool rt_ok;
} soft_t;

static int64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void wait_until(int64_t t) {
  int64_t now = mono_ns();
  if (t - now > 80000) {                /* sleep until ~60 us before, then spin */
    int64_t w = t - 60000;
    struct timespec ts = { (time_t)(w / 1000000000), (long)(w % 1000000000) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
  }
  while (mono_ns() < t) {}
}

static uint64_t bias(const soft_t *s) {
  return s->proto == VTX_PROTO_TRAMP ? GPIO_V2_LINE_FLAG_BIAS_PULL_UP : GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
}

static void cfg_input(const soft_t *s, struct gpio_v2_line_config *c, bool with_bias) {
  memset(c, 0, sizeof *c);
  c->flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING | (with_bias ? bias(s) : 0);
}

static void cfg_output(struct gpio_v2_line_config *c, int value) {
  memset(c, 0, sizeof *c);
  c->flags = GPIO_V2_LINE_FLAG_OUTPUT;
  c->num_attrs = 1;
  c->attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
  c->attrs[0].attr.values = value ? 1 : 0;
  c->attrs[0].mask = 1;
}

/* The BCM283x/2711 GPIO block (BCM2835 ARM Peripherals, section 6; the same on the 2711): GPFSELn at 0x00
 * (3 bits per pin, 000 input, 001 output), GPSET0 at 0x1C, GPCLR0 at 0x28, GPLEV0 at 0x34 (one bit per
 * pin, GPIO 0-31). Function select is a read-modify-write of the register that holds ten pins. */
#define REG_FSEL(n) ((n) / 10)
#define REG_SET0 (0x1C / 4)
#define REG_CLR0 (0x28 / 4)
#define REG_LEV0 (0x34 / 4)

static void reg_fsel(soft_t *s, unsigned f) {
  volatile uint32_t *r = s->regs + REG_FSEL(s->offset);
  unsigned sh = (s->offset % 10) * 3;
  *r = (*r & ~(7u << sh)) | ((f & 7u) << sh);
}
static inline void reg_drive(soft_t *s, int level) { s->regs[level ? REG_SET0 : REG_CLR0] = 1u << s->offset; }

static int get_level(soft_t *s) {
  if (s->regs) return (int)(s->regs[REG_LEV0] >> s->offset & 1);
  struct gpio_v2_line_values v = { 0, 1 };
  if (ioctl(s->line_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) != 0) return s->proto == VTX_PROTO_TRAMP;
  return (int)(v.bits & 1);
}

/* /dev/gpiomem: the GPIO block of a BCM283x/2711 (the Pi 1-4; the Pi 5's RP1 has another layout and its
 * own device). Only for a pin of GPIO 0-31 that the character device has just set up as an input. */
static void map_regs(soft_t *s, const char *label) {
  if (s->offset >= 32 || !(!strcmp(label, "pinctrl-bcm2711") || !strcmp(label, "pinctrl-bcm2835"))) return;
  int fd = open("/dev/gpiomem", O_RDWR | O_SYNC | O_CLOEXEC);
  if (fd < 0) return;
  void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (m == MAP_FAILED) return;
  volatile uint32_t *r = m;
  if ((r[REG_FSEL(s->offset)] >> ((s->offset % 10) * 3) & 7u) != 0) {   /* not the input the kernel just made: not our block */
    munmap(m, 4096);
    return;
  }
  s->regs = r;
}

/* The kernel reads our line's edges in a thread ("irq/<n>-sticktime-vtx", SCHED_FIFO 50 like every
 * interrupt thread) and keeps the interrupt masked until it has run: late, and an edge's timestamp is
 * wrong or the edge is lost. Above the sender and the frame loop it runs at once. (The name may be cut
 * to 15 characters.) */
static void raise_irq_thread(soft_t *s) {
  DIR *d = opendir("/proc");
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] < '1' || e->d_name[0] > '9') continue;
    char p[64], comm[40] = "";
    sfmt(p, sizeof p, "/proc/%s/comm", e->d_name);
    FILE *f = fopen(p, "r");
    if (!f) continue;
    if (!fgets(comm, sizeof comm, f)) comm[0] = 0;
    fclose(f);
    if (strncmp(comm, "irq/", 4) || !strstr(comm, "-sticktim")) continue;
    struct sched_param sp = { .sched_priority = soft_rt_prio + 1 < 99 ? soft_rt_prio + 1 : 99 };
    /* the raw system call: musl's sched_setscheduler only fails */
    if (syscall(SYS_sched_setscheduler, (pid_t)atoi(e->d_name), SCHED_FIFO, &sp) == 0) s->irq_prio_set = true;
  }
  closedir(d);
}

/* the decoder from the line's level now; its counters stay (it is reset after every frame) */
static void rx_reset(soft_t *s, int level) {
  uint32_t bytes = s->rx.bytes, framing = s->rx.framing, glitches = s->rx.glitches, breaks = s->rx.breaks;
  vtx_soft_rx_init(&s->rx, s->baud, s->stop, level);
  s->rx.bytes = bytes;
  s->rx.framing = framing;
  s->rx.glitches = glitches;
  s->rx.breaks = breaks;
}

/* back to listening: input, both edges, the bias; the decoder from the line's level now */
static int to_input(soft_t *s) {
  struct gpio_v2_line_config c;
  cfg_input(s, &c, true);
  if (ioctl(s->line_fd, GPIO_V2_LINE_SET_CONFIG_IOCTL, &c) != 0) {
    cfg_input(s, &c, false);                       /* a chip without bias control */
    if (ioctl(s->line_fd, GPIO_V2_LINE_SET_CONFIG_IOCTL, &c) != 0) return -1;
  }
  rx_reset(s, get_level(s));
  return 0;
}

static bool chip_label_ok(const char *label) {
  return !strcmp(label, "pinctrl-bcm2711") || !strcmp(label, "pinctrl-bcm2835") || !strcmp(label, "pinctrl-rp1");
}

/* the chip and line of BCM GPIO n: the Pi's pinctrl chip by its label, else a line named "GPIO<n>" */
static int find_gpio(int n, char *chip, size_t cn, unsigned *offset) {
  char want[16];
  sfmt(want, sizeof want, "GPIO%d", n);
  for (int pass = 0; pass < 2; pass++) {
    DIR *d = opendir("/dev");
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while (found < 0 && (e = readdir(d))) {
      if (strncmp(e->d_name, "gpiochip", 8)) continue;
      char p[300];
      sfmt(p, sizeof p, "/dev/%s", e->d_name);
      int fd = open(p, O_RDWR | O_CLOEXEC);
      if (fd < 0) continue;
      struct gpiochip_info ci;
      memset(&ci, 0, sizeof ci);
      if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &ci) == 0) {
        if (pass == 0 && chip_label_ok(ci.label) && (unsigned)n < ci.lines) {
          plat_strlcpy(chip, p, cn);
          *offset = (unsigned)n;
          found = 0;
        } else if (pass == 1) {
          for (unsigned i = 0; i < ci.lines && found < 0; i++) {
            struct gpio_v2_line_info li;
            memset(&li, 0, sizeof li);
            li.offset = i;
            if (ioctl(fd, GPIO_V2_GET_LINEINFO_IOCTL, &li) == 0 && !strcmp(li.name, want)) {
              plat_strlcpy(chip, p, cn);
              *offset = i;
              found = 0;
            }
          }
        }
      }
      close(fd);
    }
    closedir(d);
    if (found == 0) return 0;
  }
  return -1;
}

static int soft_open(vtx_io_t *io, char *err, size_t errlen) {
  soft_t *s = io->ud;
  if (s->gpio >= 0 && find_gpio(s->gpio, s->chip, sizeof s->chip, &s->offset) != 0) {
    sfmt(err, errlen, "No GPIO chip has GPIO%d (is this a Raspberry Pi?).", s->gpio);
    return -2;
  }
  s->chip_fd = open(s->chip, O_RDWR | O_CLOEXEC);
  if (s->chip_fd < 0) {
    int e = errno;
    sfmt(err, errlen, "Can't open %s: %s.", s->chip, strerror(e));
    return e == ENOENT ? -2 : -1;
  }
  struct gpio_v2_line_request req;
  memset(&req, 0, sizeof req);
  req.offsets[0] = s->offset;
  req.num_lines = 1;
  req.event_buffer_size = 1024;
  plat_strlcpy(req.consumer, "sticktime-vtx", sizeof req.consumer);
  cfg_input(s, &req.config, true);
  int r = ioctl(s->chip_fd, GPIO_V2_GET_LINE_IOCTL, &req);
  if (r != 0 && errno == EINVAL) {                 /* no bias on this chip */
    cfg_input(s, &req.config, false);
    r = ioctl(s->chip_fd, GPIO_V2_GET_LINE_IOCTL, &req);
  }
  if (r != 0) {
    int e = errno;
    struct gpio_v2_line_info li;
    memset(&li, 0, sizeof li);
    li.offset = s->offset;
    if (e == EBUSY && ioctl(s->chip_fd, GPIO_V2_GET_LINEINFO_IOCTL, &li) == 0 && li.consumer[0])
      sfmt(err, errlen, "%s line %u is in use (by \"%s\"): pick another vtx_port.", s->chip, s->offset, li.consumer);
    else sfmt(err, errlen, "Can't take %s line %u: %s.", s->chip, s->offset, strerror(e));
    close(s->chip_fd);
    s->chip_fd = -1;
    return -1;
  }
  s->line_fd = req.fd;
  fcntl(s->line_fd, F_SETFL, fcntl(s->line_fd, F_GETFL) | O_NONBLOCK);
  if (s->gpio >= 0) sfmt(io->desc, sizeof io->desc, "GPIO%d, pin %d (single pin)", s->gpio, vtx_soft_header_pin(s->gpio));
  else sfmt(io->desc, sizeof io->desc, "%s line %u (single pin)", s->chip, s->offset);
  struct gpiochip_info ci;
  memset(&ci, 0, sizeof ci);
  if (ioctl(s->chip_fd, GPIO_GET_CHIPINFO_IOCTL, &ci) == 0) map_regs(s, ci.label);
  s->irq_prio_set = false;
  raise_irq_thread(s);
  s->tx_end_ns = 0;
  rx_reset(s, get_level(s));
  s->qn = 0;
  return 0;
}

static void soft_close(vtx_io_t *io) {
  soft_t *s = io->ud;
  if (s->regs) {
    reg_fsel(s, 0);                                /* an input, as the kernel believes */
    munmap((void *)s->regs, 4096);
    s->regs = NULL;
  }
  if (s->line_fd >= 0) close(s->line_fd);
  if (s->chip_fd >= 0) close(s->chip_fd);
  s->line_fd = s->chip_fd = -1;
}

static int soft_set_line(vtx_io_t *io, int baud, int stop) {
  soft_t *s = io->ud;
  s->baud = baud;
  s->stop = stop;
  rx_reset(s, s->line_fd >= 0 ? get_level(s) : s->rx.level);
  return 0;
}

static void soft_set_proto(vtx_io_t *io, vtx_proto_t p) {
  soft_t *s = io->ud;
  if (p == s->proto) return;
  s->proto = p;
  if (s->line_fd >= 0) to_input(s);                /* the other bias */
}

static void push(soft_t *s, const uint8_t *p, int n) {
  for (int i = 0; i < n && s->qn < SOFT_QUEUE; i++) s->q[s->qn++] = p[i];
}

/* the thread, for a frame: SCHED_FIFO (when allowed) at soft_rt_prio, above whatever else of ours shares
 * the core (the drawing threads) */
static bool rt_on(void) {
  struct sched_param sp = { .sched_priority = soft_rt_prio };
  return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
}
static void rt_off(void) {
  struct sched_param sp = { .sched_priority = 0 };
  pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
}

static int soft_write(vtx_io_t *io, const uint8_t *p, size_t n) {
  soft_t *s = io->ud;
  if (s->line_fd < 0) return -1;
  static vtx_edge_t ed[2048];                      /* (the VTX thread only) */
  int64_t end;
  int from = s->proto == VTX_PROTO_TRAMP ? 1 : 0;  /* the line's idle level: SmartAudio rests low, Tramp high */
  int ne = vtx_soft_schedule(p, n, s->baud, s->stop, from, ed, 2048, &end);
  int64_t bit = 1000000000LL / (s->baud > 0 ? s->baud : 4800);
  bool on_time = false;
  for (int attempt = 0; attempt < 3 && !on_time; attempt++) {
    s->rt_ok = rt_on();
    int64_t sw0 = mono_ns();
    if (s->regs) {                                 /* the level first, then the pin drives: no glitch */
      reg_drive(s, from);
      reg_fsel(s, 1);
    } else {
      struct gpio_v2_line_config c;
      cfg_output(&c, from);
      if (ioctl(s->line_fd, GPIO_V2_LINE_SET_CONFIG_IOCTL, &c) != 0) { rt_off(); return -1; }
    }
    int64_t sw = mono_ns() - sw0;
    if (sw > s->sw_out_max_ns) s->sw_out_max_ns = sw;
    int64_t t0 = mono_ns() + 100000, late_max = 0;
    for (int i = 0; i < ne; i++) {
      int64_t t = t0 + ed[i].t_ns;
      wait_until(t);
      if (s->regs) reg_drive(s, ed[i].level);
      else {
        struct gpio_v2_line_values v = { ed[i].level, 1 };
        ioctl(s->line_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v);
      }
      int64_t late = mono_ns() - t;
      if (late > late_max) late_max = late;
      s->late_sum_ns += late;
      s->late_n++;
      if (late > bit / 4) s->late_q++;
      if (late > bit / 2) s->late_h++;
    }
    wait_until(t0 + end);                          /* the last stop bit is out */
    sw0 = mono_ns();
    int ri = 0;
    if (s->regs) {                                 /* an input again; the edge detection never stopped */
      reg_fsel(s, 0);
      s->tx_end_ns = mono_ns();
      rx_reset(s, get_level(s));
    } else ri = to_input(s);
    sw = mono_ns() - sw0;
    if (sw > s->sw_in_max_ns) s->sw_in_max_ns = sw;
    rt_off();
    if (ri != 0) return -1;
    s->frames++;
    s->own_expect += (uint32_t)ne;
    if (late_max > s->late_max_ns) s->late_max_ns = late_max;
    s->late_last_max_ns = late_max;
    /* A UART samples each bit in its middle: an edge late by less than half a bit still reads right (the
     * VTX's own clock error eats part of that; so does the ioctl's time, which the figure includes).
     * Only a frame that is surely garbled goes again: a frame that was probably fine would get its
     * reply while the repeat is on the wire, and both would be lost (seen on a Pi 4 with a quarter-bit
     * rule: half the frames repeated, a third answered). */
    on_time = late_max <= bit / 2;
    if (!on_time) {
      s->resent++;
      plat_sleep_us(3000);
    }
  }
  if (!on_time) s->late_frames++;                  /* three times late: it went out like that */
  s->qn = 0;
  push(s, p, (int)n);                              /* what went out, as the line's echo */
  return 0;
}

static void drain(soft_t *s, bool decode) {
  struct gpio_v2_line_event ev[32];
  for (;;) {
    ssize_t k = read(s->line_fd, ev, sizeof ev);
    if (k <= 0) break;
    for (int i = 0; i < (int)(k / (ssize_t)sizeof ev[0]); i++) {
      s->events++;
      if (s->regs && (int64_t)ev[i].timestamp_ns <= s->tx_end_ns + 20000) { s->own_seen++; continue; }   /* the sender's own */
      if (!decode) continue;
      uint8_t b[2];
      int got = vtx_soft_rx_edge(&s->rx, (int64_t)ev[i].timestamp_ns, ev[i].id == GPIO_V2_LINE_EVENT_RISING_EDGE, b, 2);
      push(s, b, got);
    }
  }
}

static int soft_read(vtx_io_t *io, uint8_t *p, size_t n, int timeout_ms) {
  soft_t *s = io->ud;
  if (s->line_fd < 0) return -1;
  int64_t deadline = mono_ns() + (int64_t)(timeout_ms < 0 ? 0 : timeout_ms) * 1000000;
  for (;;) {
    if (s->qn > 0) {
      int k = s->qn < (int)n ? s->qn : (int)n;
      memcpy(p, s->q, (size_t)k);
      memmove(s->q, s->q + k, (size_t)(s->qn - k));
      s->qn -= k;
      return k;
    }
    int64_t now = mono_ns(), due = vtx_soft_rx_due(&s->rx);
    if (due && now >= due) {
      uint8_t b[2];
      push(s, b, vtx_soft_rx_idle(&s->rx, now, b, 2));
      if (s->qn) continue;
    }
    if (now >= deadline) return 0;
    int64_t until = due && due < deadline ? due : deadline;
    int ms = (int)((until - now + 999999) / 1000000);
    struct pollfd pf = { s->line_fd, POLLIN, 0 };
    int r = poll(&pf, 1, ms < 1 ? 1 : ms);
    if (r < 0 && errno != EINTR) return -1;
    if (r > 0) {
      if (pf.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
      drain(s, true);
    }
  }
}

static void soft_flush(vtx_io_t *io) {
  soft_t *s = io->ud;
  if (s->line_fd < 0) return;
  drain(s, false);
  s->qn = 0;
  rx_reset(s, get_level(s));
}

static void soft_stats(vtx_io_t *io, char *out, size_t n) {
  soft_t *s = io->ud;
  if (!s->frames) { out[0] = 0; return; }
  sfmt(out, n, "%lu frames sent%s; TX edges late by %lld us on average, %lld us at most (last frame %lld us)%s, %lu of %lu edges over "
       "a quarter bit late, %lu over half a bit; %lu sent again, %lu went out late; the pin switched to output in up to %lld us, "
       "back to input in up to %lld us; own edges seen back: %lu (%lu driven)%s; RX %lu bytes, %lu framing errors, %lu glitches",
       (unsigned long)s->frames, s->regs ? "" : " (through the kernel's ioctls)", (long long)(s->late_n ? s->late_sum_ns / s->late_n / 1000 : 0),
       (long long)(s->late_max_ns / 1000), (long long)(s->late_last_max_ns / 1000), s->rt_ok ? "" : " (not real-time: run as root)",
       (unsigned long)s->late_q, (unsigned long)s->late_n, (unsigned long)s->late_h, (unsigned long)s->resent, (unsigned long)s->late_frames,
       (long long)(s->sw_out_max_ns / 1000), (long long)(s->sw_in_max_ns / 1000), (unsigned long)s->own_seen, (unsigned long)s->own_expect,
       s->irq_prio_set ? "" : " (the edge interrupt's thread could not be raised)",
       (unsigned long)s->rx.bytes, (unsigned long)s->rx.framing, (unsigned long)s->rx.glitches);
}

static uint32_t soft_late_frames(vtx_io_t *io) { return ((soft_t *)io->ud)->late_frames; }

static int64_t soft_now(vtx_io_t *io) { return mono_ns() / 1000; }

static void soft_destroy(vtx_io_t *io) {
  soft_close(io);
  free(io->ud);
  free(io);
}

vtx_io_t *vtx_soft_io_new(const char *spec, char *err, size_t errlen) {
  int g = gpio_of(spec);
  const char *colon = !strncmp(spec, "gpio:", 5) ? strrchr(spec + 5, ':') : NULL;
  if (g < 0 && !colon) { sfmt(err, errlen, "\"%s\" isn't a GPIO port.", spec); return NULL; }
  vtx_io_t *io = calloc(1, sizeof *io);
  soft_t *s = calloc(1, sizeof *s);
  if (!io || !s) { free(io); free(s); sfmt(err, errlen, "out of memory"); return NULL; }
  s->gpio = g;
  s->chip_fd = s->line_fd = -1;
  s->baud = 4800;
  s->stop = 2;
  s->proto = VTX_PROTO_SA;
  if (g < 0) {
    size_t cl = (size_t)(colon - spec - 5);
    if (cl >= sizeof s->chip) cl = sizeof s->chip - 1;
    memcpy(s->chip, spec + 5, cl);
    s->chip[cl] = 0;
    s->offset = (unsigned)atoi(colon + 1);
  }
  io->ud = s;
  io->open = soft_open; io->close = soft_close; io->set_line = soft_set_line; io->write = soft_write;
  io->read = soft_read; io->flush = soft_flush; io->set_proto = soft_set_proto; io->stats = soft_stats;
  io->late_frames = soft_late_frames; io->now_us = soft_now; io->destroy = soft_destroy;
  if (g >= 0) sfmt(io->desc, sizeof io->desc, "GPIO%d, pin %d (single pin)", g, vtx_soft_header_pin(g));
  else plat_strlcpy(io->desc, spec, sizeof io->desc);
  return io;
}

#else
/* ================================================================ elsewhere: the pins are the Pi's */
static int none_open(vtx_io_t *io, char *err, size_t errlen) {
  sfmt(err, errlen, "There is no %s here: GPIO pins work on the Pi (Linux) only.", io->desc);
  return -1;
}
static void none_destroy(vtx_io_t *io) { free(io); }

vtx_io_t *vtx_soft_io_new(const char *spec, char *err, size_t errlen) {
  if (!vtx_soft_is_spec(spec)) { sfmt(err, errlen, "\"%s\" isn't a GPIO port.", spec); return NULL; }
  vtx_io_t *io = calloc(1, sizeof *io);
  if (!io) { sfmt(err, errlen, "out of memory"); return NULL; }
  io->open = none_open;
  io->destroy = none_destroy;
  plat_strlcpy(io->desc, spec, sizeof io->desc);
  return io;
}
#endif
