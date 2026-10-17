/* The single-pin VTX line (host/src/vtx_soft.c) through the real GPIO character device, against a
 * simulated line (gpio-sim): finding GPIO4 by its line name, the request with edge events and bias,
 * decoding a frame a "VTX" drives on the line, sending a frame (the line an output for it, an input again
 * right after) that a "VTX" samples, both protocols' biases, and the timing statistics.
 *
 * Needs root and the gpio-sim module (Debian's kernel doesn't have it: build drivers/gpio/gpio-sim.c with
 * kernel/irq/irq_sim.c out of tree). The fake VTX drives the line through gpio-sim's sysfs "pull" file
 * from a thread, which an emulated VM can't time finely: the frames go at a slow baud (default 300;
 * --baud N to try others). On a real Pi use sticktime-pi --vtx-test / --vtx-loopback-test instead.
 *
 *   gcc -std=gnu99 -O2 -Wall -Wextra -Wno-unused-parameter -Ihost/src -Ihost/third_party \
 *       host/tests/linux/test_vtx_gpio.c host/src/vtx_soft.c host/src/vtx_proto.c host/src/plat.c -lm -lpthread -o test_vtx_gpio
 *   sudo ./test_vtx_gpio [--baud N] */
#define _GNU_SOURCE
#include "../t.h"
#include "vtx_soft.h"
#include "vtx_proto.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CFG "/sys/kernel/config/gpio-sim/stpvtx"
static char chip[64], value_path[256], pull_path[256];
static int baud = 300;

static int wr(const char *path, const char *s) {
  int fd = open(path, O_WRONLY);
  if (fd < 0) return -1;
  ssize_t n = write(fd, s, strlen(s));
  close(fd);
  return n == (ssize_t)strlen(s) ? 0 : -1;
}

static int rd(const char *path, char *out, size_t n) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  ssize_t k = read(fd, out, n - 1);
  close(fd);
  if (k <= 0) return -1;
  out[k] = 0;
  while (k && (out[k - 1] == '\n' || out[k - 1] == ' ')) out[--k] = 0;
  return 0;
}

static void sim_down(void) {
  wr(CFG "/live", "0");
  rmdir(CFG "/bank0/line4");
  rmdir(CFG "/bank0");
  rmdir(CFG);
}

static int sim_up(void) {
  sim_down();
  char dev[64];
  if (mkdir(CFG, 0755) || mkdir(CFG "/bank0", 0755) || wr(CFG "/bank0/num_lines", "8") || mkdir(CFG "/bank0/line4", 0755) ||
      wr(CFG "/bank0/line4/name", "GPIO4") || wr(CFG "/live", "1") || rd(CFG "/bank0/chip_name", chip, sizeof chip) ||
      rd(CFG "/dev_name", dev, sizeof dev)) {
    fprintf(stderr, "gpio-sim: can't set up %s (%s): load gpio-sim, run as root\n", CFG, strerror(errno));
    return -1;
  }
  snprintf(value_path, sizeof value_path, "/sys/devices/platform/%s/%s/sim_gpio4/value", dev, chip);
  snprintf(pull_path, sizeof pull_path, "/sys/devices/platform/%s/%s/sim_gpio4/pull", dev, chip);
  printf("gpio-sim: /dev/%s line 4 \"GPIO4\" (%s)\n", chip, value_path);
  return 0;
}

static int64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void sleep_until(int64_t t) {
  struct timespec ts = { (time_t)(t / 1000000000), (long)(t % 1000000000) };
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

/* the fake VTX drives bytes onto the line (gpio-sim's pull = what an input sees) */
typedef struct { const uint8_t *p; size_t n; int from, stop; int64_t start; int64_t late_max; } drive_t;

static void *drive_main(void *arg) {
  drive_t *d = arg;
  vtx_edge_t e[2048];
  int64_t end;
  int ne = vtx_soft_schedule(d->p, d->n, baud, d->stop, 1, e, 2048, &end);
  int fd = open(pull_path, O_WRONLY);
  if (d->from == 0) {                              /* a line resting low: the VTX raises it first (idle mark) */
    sleep_until(d->start - 2 * 1000000000LL / baud);
    if (pwrite(fd, "pull-up", 7, 0) < 0) perror("pull");
  }
  for (int i = 0; i < ne; i++) {
    sleep_until(d->start + e[i].t_ns);
    const char *s = e[i].level ? "pull-up" : "pull-down";
    if (pwrite(fd, s, strlen(s), 0) < 0) perror("pull");
    int64_t late = mono_ns() - (d->start + e[i].t_ns);
    if (late > d->late_max) d->late_max = late;
  }
  close(fd);
  return NULL;
}

/* the fake VTX samples what the Pi drives: waits for a start bit, samples each bit in its middle */
typedef struct { int stop; size_t want; uint8_t got[64]; size_t n; } sample_t;

static int value_now(int fd) {
  char c = '?';
  if (pread(fd, &c, 1, 0) != 1) return -1;
  return c == '1';
}

static void *sample_main(void *arg) {
  sample_t *s = arg;
  int fd = open(value_path, O_RDONLY);
  int64_t bit = 1000000000LL / baud, give_up = mono_ns() + 5000000000LL + 20 * 11 * bit;
  int prev = value_now(fd);
  while (s->n < s->want && mono_ns() < give_up) {
    int v = value_now(fd);
    if (prev == 1 && v == 0) {                     /* a start bit */
      int64_t t0 = mono_ns();
      unsigned b = 0;
      for (int k = 0; k < 8; k++) {
        sleep_until(t0 + (int64_t)((k + 1.5) * (double)bit));
        if (value_now(fd) == 1) b |= 1u << k;
      }
      sleep_until(t0 + (int64_t)(9.5 * (double)bit));
      s->got[s->n++] = (uint8_t)b;
      v = value_now(fd);
    }
    prev = v;
    usleep(bit / 40000 > 0 ? (useconds_t)(bit / 40000) : 1);   /* ~1/40 of a bit */
  }
  close(fd);
  return NULL;
}

static unsigned line_flags(vtx_io_t *io) {
  (void)io;
  char p[80];
  snprintf(p, sizeof p, "/dev/%s", chip);
  int fd = open(p, O_RDONLY);
  struct gpio_v2_line_info li;
  memset(&li, 0, sizeof li);
  li.offset = 4;
  unsigned f = 0;
  if (fd >= 0 && ioctl(fd, GPIO_V2_GET_LINEINFO_IOCTL, &li) == 0) f = (unsigned)li.flags;
  if (fd >= 0) close(fd);
  return f;
}

int main(int argc, char **argv) {
  for (int i = 1; i + 1 < argc; i++)
    if (!strcmp(argv[i], "--baud")) baud = atoi(argv[++i]);
  if (sim_up() != 0) return 77;
  char err[200] = "";
  vtx_io_t *io = vtx_soft_io_new("gpio4", err, sizeof err);   /* found by its line name "GPIO4" */
  CHECK(io != NULL);
  if (!io) { sim_down(); T_DONE(); }
  CHECK_INT(io->open(io, err, sizeof err), 0);
  if (err[0]) fprintf(stderr, "open: %s\n", err);
  printf("line: %s, %d baud\n", io->desc, baud);
  unsigned f = line_flags(io);
  CHECK(f & GPIO_V2_LINE_FLAG_USED);
  CHECK(f & GPIO_V2_LINE_FLAG_INPUT);
  CHECK(f & GPIO_V2_LINE_FLAG_EDGE_RISING);
  CHECK(f & GPIO_V2_LINE_FLAG_EDGE_FALLING);
  CHECK(f & GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN);                /* SmartAudio: idle low */
  char v[8] = "";
  rd(value_path, v, sizeof v);
  CHECK_STR(v, "0");

  /* receive: SmartAudio, the "VTX" raises the line, sends a V1 reply, lets go */
  io->set_proto(io, VTX_PROTO_SA);
  CHECK_INT(io->set_line(io, baud, 2), 0);
  io->flush(io);
  uint8_t reply[10] = { 0xAA, 0x55, 0x01, 0x06, 0x00, 0x00, 0x01, 0x16, 0xE9, 0x4D };
  drive_t d = { reply, 10, 0, 2, mono_ns() + 20000000, 0 };
  pthread_t th;
  pthread_create(&th, NULL, drive_main, &d);
  uint8_t got[64];
  size_t n = 0;
  int64_t until = mono_ns() + 2000000000LL + (int64_t)12 * 11 * 1000000000LL / baud;
  while (n < 10 && mono_ns() < until) {
    int k = io->read(io, got + n, sizeof got - n, 200);
    if (k > 0) n += (size_t)k;
  }
  pthread_join(th, NULL);
  wr(pull_path, "pull-down");
  printf("RX SmartAudio: %zu bytes (the driver's edges up to %lld us late)\n", n, (long long)(d.late_max / 1000));
  CHECK_INT(n, 10);
  CHECK(n == 10 && !memcmp(got, reply, 10));

  /* send: Tramp (idle high, pull-up), a 'v' query; the "VTX" samples it */
  io->set_proto(io, VTX_PROTO_TRAMP);
  CHECK(line_flags(io) & GPIO_V2_LINE_FLAG_BIAS_PULL_UP);
  CHECK_INT(io->set_line(io, baud, 1), 0);
  usleep(20000);
  uint8_t q[TRAMP_FRAME];
  vtx_tramp_frame(q, 'v', 0);
  sample_t s = { 1, TRAMP_FRAME, { 0 }, 0 };
  pthread_create(&th, NULL, sample_main, &s);
  usleep(50000);
  CHECK_INT(io->write(io, q, TRAMP_FRAME), 0);
  pthread_join(th, NULL);
  printf("TX Tramp: the sampler read %zu bytes\n", s.n);
  CHECK_INT(s.n, TRAMP_FRAME);
  CHECK(s.n == TRAMP_FRAME && !memcmp(s.got, q, TRAMP_FRAME));
  f = line_flags(io);
  CHECK(f & GPIO_V2_LINE_FLAG_INPUT);                         /* listening again */
  CHECK(!(f & GPIO_V2_LINE_FLAG_OUTPUT));
  n = (size_t)io->read(io, got, sizeof got, 10);              /* the frame as the line's echo */
  CHECK_INT(n, TRAMP_FRAME);
  CHECK(n == TRAMP_FRAME && !memcmp(got, q, TRAMP_FRAME));

  /* send then answer, like a VTX: SmartAudio GET_SETTINGS out, a reply back on the same line */
  io->set_proto(io, VTX_PROTO_SA);
  CHECK_INT(io->set_line(io, baud, 2), 0);
  uint8_t gs[6];
  gs[0] = 0x00;
  vtx_sa_get_settings(gs + 1);
  sample_t s2 = { 2, 5, { 0 }, 0 };                           /* the frame; the lead-in has no start edge */
  pthread_create(&th, NULL, sample_main, &s2);
  usleep(50000);
  io->flush(io);
  CHECK_INT(io->write(io, gs, 6), 0);
  pthread_join(th, NULL);
  printf("TX SmartAudio: the sampler read %zu bytes:", s2.n);
  for (size_t i = 0; i < s2.n; i++) printf(" %02X", s2.got[i]);
  printf("\n");
  CHECK(s2.n >= 5 && !memcmp(s2.got + s2.n - 5, gs + 1, 5));  /* (the 0x00 lead-in on a low line has no start edge) */
  n = (size_t)io->read(io, got, sizeof got, 10);
  CHECK_INT(n, 6);                                           /* the echo */
  d = (drive_t){ reply, 10, 0, 2, mono_ns() + 30000000, 0 };
  pthread_create(&th, NULL, drive_main, &d);
  n = 0;
  until = mono_ns() + 2000000000LL + (int64_t)12 * 11 * 1000000000LL / baud;
  while (n < 10 && mono_ns() < until) {
    int k = io->read(io, got + n, sizeof got - n, 200);
    if (k > 0) n += (size_t)k;
  }
  pthread_join(th, NULL);
  wr(pull_path, "pull-down");
  CHECK(n == 10 && !memcmp(got, reply, 10));
  vtx_sa_parser_t p;
  vtx_sa_parser_reset(&p);
  vtx_sa_frame_t fr;
  int frames = 0;
  for (size_t i = 0; i < n; i++) frames += vtx_sa_parser_feed(&p, got[i], &fr);
  CHECK_INT(frames, 1);
  char st[200] = "";
  io->stats(io, st, sizeof st);
  printf("stats: %s\n", st);
  CHECK(strstr(st, "frames sent") != NULL);

  /* busy: a second user of the line is told who has it */
  vtx_io_t *io2 = vtx_soft_io_new("gpio4", err, sizeof err);
  CHECK_INT(io2->open(io2, err, sizeof err), -1);
  CHECK(strstr(err, "sticktime-vtx") != NULL);
  printf("second open: %s\n", err);
  io2->destroy(io2);
  io->close(io);
  io->destroy(io);
  sim_down();
  T_DONE();
}
