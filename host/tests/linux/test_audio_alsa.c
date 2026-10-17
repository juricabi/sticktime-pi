/* Linux: audio.c's raw ALSA path (kernel PCM ioctls, no alsa-lib) on a real PCM device.
 *
 *   test_audio_alsa [--hotplug] [out]
 *     out        an audio.h output: "card:Loopback" (default when snd-aloop is loaded), "hw:1,0", "jack" ...
 *                default without a Loopback card: the first card with a playback PCM
 *     --hotplug  (root, snd-aloop loaded and unused) unload snd-aloop, open the output (missing: silent,
 *                an error, retried), load it again: the output must come back by itself
 *
 * Best on snd-aloop (modprobe snd-aloop): whatever audio.c plays on Loopback device 0 subdevice N comes
 * back on capture device 1 subdevice N, so the test checks the samples themselves:
 *   - open/hw_params/sw_params/prepare: S16_LE stereo 48 kHz in 256-frame periods x 4, the status string
 *   - the write loop: a 1 kHz test tone arrives at the right level and frequency without a single gap,
 *     frames go out at the sample rate
 *   - the thread: SCHED_FIFO one below a real-time caller and pinned next to its core; normal otherwise
 *   - underrun recovery: the render thread is stalled for 150 ms -> -EPIPE -> PREPARE, counted, the
 *     tone comes back clean
 *   - mono, a beep through sfx, a quick close
 *   - S32_LE, and the IEC958 path end to end (the subframes are sent as S32_LE, which the loopback
 *     carries back unchanged): preambles, parity, 192-frame blocks, channel status, samples, and a new
 *     block (Z) at the restart after an underrun
 *   - errors: missing jack/HDMI cards, a missing hw device
 * On any other card it can only check that the loop runs (frames out, no errors, underrun recovery).
 * Without a sound card it says so and passes. */
#ifndef __linux__
#include <stdio.h>
int main(void) { puts("test_audio_alsa: Linux only (skipped)"); return 0; }
#else

#define _GNU_SOURCE
#include "../t.h"
#include "audio.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <sound/asound.h>

/* audio.c's test helpers (not in a header: only tests use them) */
typedef struct { uint64_t frames; int period, buffer, format, card, dev; } audio_test_info_t;
void audio_test_info(audio_t *a, audio_test_info_t *ti);
int audio_test_subdevice(audio_t *a);
void audio_test_force_format(int fmt, bool iec_as_s32);
void audio_test_source(audio_t *a, void (*fn)(void *ud, float *L, float *R, int n), void *ud);
enum { F_S16 = 0, F_S32 = 1, F_IEC958 = 2 };

#define RATE 48000
#define FREQ 1000.0
#define AL 0.5f
#define AR 0.25f

static int64_t now_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000); }

/* ---------------------------------------------------------------- the test tone (render thread) */

typedef struct {
  double ph;
  int on;                      /* atomic */
  uint64_t frames;             /* atomic: frames rendered */
  uint64_t stall_at;           /* atomic: stall once when frames reach this (0 never) */
  int stall_ms;
  int stalls;                  /* atomic */
} sine_t;

static void sine_fn(void *ud, float *L, float *R, int n) {
  sine_t *s = ud;
  uint64_t at = __atomic_load_n(&s->stall_at, __ATOMIC_ACQUIRE), fr = __atomic_load_n(&s->frames, __ATOMIC_RELAXED);
  if (at && fr >= at) {        /* the render thread gets stuck: the device runs dry */
    __atomic_store_n(&s->stall_at, 0, __ATOMIC_RELEASE);
    usleep((useconds_t)s->stall_ms * 1000);
    __atomic_add_fetch(&s->stalls, 1, __ATOMIC_RELEASE);
  }
  if (__atomic_load_n(&s->on, __ATOMIC_RELAXED)) {
    const double w = 2 * M_PI * FREQ / RATE;
    for (int i = 0; i < n; i++) {
      float x = (float)sin(s->ph);
      L[i] += AL * x;
      R[i] += AR * x;
      s->ph += w;
      if (s->ph > 2 * M_PI) s->ph -= 2 * M_PI;
    }
  }
  __atomic_store_n(&s->frames, fr + (uint64_t)n, __ATOMIC_RELEASE);
}

/* ---------------------------------------------------------------- cards */

static void card_id(int card, char *out, size_t n) {
  char p[64];
  snprintf(p, sizeof p, "/proc/asound/card%d/id", card);
  out[0] = 0;
  FILE *f = fopen(p, "r");
  if (!f) return;
  if (fgets(out, (int)n, f)) out[strcspn(out, "\r\n")] = 0;
  fclose(f);
}
static int find_card(const char *id) {
  char b[64];
  for (int c = 0; c < 32; c++) { card_id(c, b, sizeof b); if (b[0] && !strcmp(b, id)) return c; }
  return -1;
}
static int first_playback_card(void) {
  for (int c = 0; c < 32; c++) {
    char p[64];
    snprintf(p, sizeof p, "/dev/snd/pcmC%dD0p", c);
    if (access(p, F_OK) == 0) return c;
  }
  return -1;
}

/* ---------------------------------------------------------------- capture (snd-aloop's other end) */

typedef struct snd_pcm_hw_params hwp_t;
static struct snd_mask *hp_mask(hwp_t *p, int n) { return &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK]; }
static struct snd_interval *hp_iv(hwp_t *p, int n) { return &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL]; }
static void hp_any(hwp_t *p) {
  memset(p, 0, sizeof *p);
  for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++)
    memset(hp_mask(p, n)->bits, 0xff, sizeof hp_mask(p, n)->bits);
  for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
    hp_iv(p, n)->min = 0;
    hp_iv(p, n)->max = UINT_MAX;
  }
  p->rmask = ~0u;
  p->info = ~0u;
}
static void hp_set_mask(hwp_t *p, int n, unsigned v) {
  memset(hp_mask(p, n), 0, sizeof(struct snd_mask));
  hp_mask(p, n)->bits[v >> 5] |= 1u << (v & 31);
}
static void hp_set_int(hwp_t *p, int n, unsigned v) {
  struct snd_interval *i = hp_iv(p, n);
  i->min = i->max = v;
  i->openmin = i->openmax = 0;
  i->integer = 1;
}

/* frames of interleaved stereo from capture device 1 subdevice sub of card, as int32 (S16 sign-extended,
 * S32 as is); returns 0, else why says why. *xruns: capture overruns (gaps that are the test's own) */
static int capture(int card, int sub, int fmt, int frames, int32_t *out, int *xruns, char *why, size_t wl) {
  char p[64];
  *xruns = 0;
  int ctl = -1;
  snprintf(p, sizeof p, "/dev/snd/controlC%d", card);
  if (sub >= 0 && (ctl = open(p, O_RDWR | O_CLOEXEC)) >= 0) ioctl(ctl, SNDRV_CTL_IOCTL_PCM_PREFER_SUBDEVICE, &sub);
  snprintf(p, sizeof p, "/dev/snd/pcmC%dD1c", card);
  int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (ctl >= 0) close(ctl);
  if (fd < 0) { snprintf(why, wl, "%s: %s", p, strerror(errno)); return -1; }
  struct snd_pcm_info info;
  memset(&info, 0, sizeof info);
  if (ioctl(fd, SNDRV_PCM_IOCTL_INFO, &info) == 0 && sub >= 0 && (int)info.subdevice != sub) {
    snprintf(why, wl, "capture got subdevice %u, not %d", info.subdevice, sub);
    close(fd);
    return -1;
  }
  hwp_t base, t;
  hp_any(&base);
  hp_set_mask(&base, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
  hp_set_mask(&base, SNDRV_PCM_HW_PARAM_FORMAT, fmt == F_S16 ? SNDRV_PCM_FORMAT_S16_LE : SNDRV_PCM_FORMAT_S32_LE);
  hp_set_mask(&base, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
  hp_set_int(&base, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
  hp_set_int(&base, SNDRV_PCM_HW_PARAM_RATE, RATE);
  static const unsigned tries[][2] = { { 1024, 8 }, { 512, 16 }, { 256, 32 }, { 256, 16 }, { 256, 4 } };
  bool ok = false;
  for (size_t i = 0; i < sizeof tries / sizeof tries[0] && !ok; i++) {
    t = base;
    hp_set_int(&t, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, tries[i][0]);
    hp_set_int(&t, SNDRV_PCM_HW_PARAM_PERIODS, tries[i][1]);
    t.rmask = ~0u;
    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, &t) == 0 && ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &t) == 0) ok = true;
  }
  if (!ok) { t = base; ok = ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &t) == 0; }
  if (!ok || ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0 || ioctl(fd, SNDRV_PCM_IOCTL_START) < 0) {
    snprintf(why, wl, "capture setup: %s", strerror(errno));
    close(fd);
    return -1;
  }
  static int32_t tmp[2 * 8192];
  int got = 0, idle = 0;
  while (got < frames) {
    struct snd_xferi x;
    memset(&x, 0, sizeof x);
    int n = frames - got > 8192 ? 8192 : frames - got;
    x.buf = tmp;
    x.frames = (snd_pcm_uframes_t)n;
    if (ioctl(fd, SNDRV_PCM_IOCTL_READI_FRAMES, &x) == 0 && x.result > 0) {
      for (long i = 0; i < 2 * (long)x.result; i++)
        out[2 * got + i] = fmt == F_S16 ? ((int16_t *)tmp)[i] : tmp[i];
      got += (int)x.result;
      idle = 0;
      continue;
    }
    if (errno == EPIPE) {
      (*xruns)++;
      ioctl(fd, SNDRV_PCM_IOCTL_PREPARE);
      ioctl(fd, SNDRV_PCM_IOCTL_START);
      continue;
    }
    if (errno != EAGAIN && errno != EINTR) { snprintf(why, wl, "capture read: %s", strerror(errno)); close(fd); return -1; }
    struct pollfd pf = { fd, POLLIN, 0 };
    if (poll(&pf, 1, 100) == 0 && (idle += 100) > 3000) { snprintf(why, wl, "capture: no data for 3 s"); close(fd); return -1; }
  }
  ioctl(fd, SNDRV_PCM_IOCTL_DROP);
  close(fd);
  return 0;
}

/* ---------------------------------------------------------------- analysis */

typedef struct {
  double rms[2], freq;
  int jumps;                   /* samples that break the sine recursion (gaps, clicks) */
  int silent;                  /* frames in runs of >= 4 where both channels are 0 (gaps) */
} tone_t;

/* x: interleaved int32 at full scale fs_ (32767 or 2^31-1); skip the first `skip` frames */
static tone_t analyse(const int32_t *x, int frames, int skip, double fs_) {
  tone_t r;
  memset(&r, 0, sizeof r);
  const double c2 = 2 * cos(2 * M_PI * FREQ / RATE);
  double s[2] = { 0, 0 };
  int cross = 0, first = -1, last = -1, zrun = 0;
  for (int i = skip; i < frames; i++) {
    double l = x[2 * i] / fs_, rr = x[2 * i + 1] / fs_;
    s[0] += l * l;
    s[1] += rr * rr;
    if (x[2 * i] == 0 && x[2 * i + 1] == 0) { if (++zrun == 4) r.silent += 4; else if (zrun > 4) r.silent++; }
    else zrun = 0;
    if (i > skip) {
      double l0 = x[2 * (i - 1)] / fs_;
      if (l0 < 0 && l >= 0) { cross++; if (first < 0) first = i; last = i; }
    }
    if (i >= skip + 2) {
      for (int ch = 0; ch < 2; ch++) {
        double e = x[2 * i + ch] / fs_ - c2 * x[2 * (i - 1) + ch] / fs_ + x[2 * (i - 2) + ch] / fs_;
        if (fabs(e) > 0.01) { r.jumps++; break; }
      }
    }
  }
  int n = frames - skip;
  r.rms[0] = n > 0 ? sqrt(s[0] / n) : 0;
  r.rms[1] = n > 0 ? sqrt(s[1] / n) : 0;
  r.freq = cross > 1 ? (double)(cross - 1) * RATE / (double)(last - first) : 0;
  return r;
}

/* IEC958 subframes (as captured S32): structure checks; samples -> pcm (16-bit as int32) */
typedef struct {
  int words, gaps, bad_parity, bad_pre, bad_vu, z, bad_block, restarts, bad_restart, cs_blocks, bad_cs;
  int gap_at, gap_end, bad_block_at, bad_block_len;
} iec_t;
static iec_t iec_check(const int32_t *x, int frames, int32_t *pcm) {
  static const uint8_t want_cs[24] = { 0x04, 0x82, 0x00, 0x02, 0x02 };
  iec_t r;
  memset(&r, 0, sizeof r);
  r.gap_at = r.gap_end = r.bad_block_at = -1;
  int last_z = -1;
  bool gap = false, gap_since_z = false;
  uint8_t cs[24];
  int cs_k = -1;
  for (int i = 0; i < frames; i++) {
    uint32_t wl = (uint32_t)x[2 * i], wr = (uint32_t)x[2 * i + 1];
    if (wl == 0 && wr == 0) {  /* the loopback's silence: the playback was stopped (underrun) */
      if (!gap) { r.gaps++; if (r.gap_at < 0) r.gap_at = i; }
      gap = gap_since_z = true;
      cs_k = -1;
      pcm[2 * i] = pcm[2 * i + 1] = 0;
      continue;
    }
    r.words += 2;
    if (__builtin_parity(wl & 0xfffffff0u)) r.bad_parity++;
    if (__builtin_parity(wr & 0xfffffff0u)) r.bad_parity++;
    if ((wr & 0xf) != 0x4) r.bad_pre++;
    if ((wl & 0xf) != 0x8 && (wl & 0xf) != 0x2) r.bad_pre++;
    if ((wl | wr) & 0x30000000u) r.bad_vu++;
    if (((wl ^ wr) & 0x40000000u)) r.bad_cs++;         /* both subframes carry the same status bit */
    bool z = (wl & 0xf) == 0x8;
    if (gap) {                                          /* the first frame after a restart opens a block */
      if (i > 0) { r.restarts++; if (!z) r.bad_restart++; if (r.gap_end < 0) r.gap_end = i; }
      gap = false;
    }
    if (z) {
      if (last_z >= 0 && !gap_since_z && i - last_z != 192) {
        r.bad_block++;
        if (r.bad_block_at < 0) { r.bad_block_at = i; r.bad_block_len = i - last_z; }
      }
      if (cs_k == 192 && memcmp(cs, want_cs, 24)) r.bad_cs++;
      if (cs_k == 192) r.cs_blocks++;
      r.z++;
      last_z = i;
      gap_since_z = false;
      memset(cs, 0, sizeof cs);
      cs_k = 0;
    }
    if (cs_k >= 0 && cs_k < 192) {
      if (wl & 0x40000000u) cs[cs_k >> 3] |= (uint8_t)(1u << (cs_k & 7));
      cs_k++;
    }
    pcm[2 * i] = (int16_t)((wl >> 12) & 0xffff);
    pcm[2 * i + 1] = (int16_t)((wr >> 12) & 0xffff);
  }
  return r;
}

/* ---------------------------------------------------------------- threads */

/* thread tid: policy, priority, the one CPU it is pinned to (-1 none); 0 ok. Raw syscalls: musl's
 * sched_getscheduler/sched_getparam always fail (ENOSYS) by design */
static int thread_info(int tid, int *pol, int *prio, int *cpu) {
  struct sched_param sp;
  int p = (int)syscall(SYS_sched_getscheduler, tid);
  if (p < 0 || syscall(SYS_sched_getparam, tid, &sp) != 0) return -1;
  *pol = p & ~SCHED_RESET_ON_FORK;
  *prio = sp.sched_priority;
  cpu_set_t set;
  CPU_ZERO(&set);
  *cpu = -1;
  if (sched_getaffinity(tid, sizeof set, &set) == 0 && CPU_COUNT(&set) == 1)
    for (int i = 0; i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &set)) { *cpu = i; break; }
  return 0;
}

/* a thread of this process other than the caller with this policy (and priority), pinned to cpu
 * (cpu < 0: any affinity); returns its tid or -1. list: print every thread */
static int find_thread(int pol, int prio, int want_cpu, bool list) {
  DIR *d = opendir("/proc/self/task");
  if (!d) return -1;
  int self = (int)syscall(SYS_gettid), found = -1;
  struct dirent *e;
  while ((e = readdir(d))) {
    int tid = atoi(e->d_name), p, q, c;
    if (tid <= 0 || thread_info(tid, &p, &q, &c) != 0) continue;
    if (list) printf("  thread %d%s: policy %s prio %d cpu %d\n", tid, tid == self ? " (main)" : "",
                     p == SCHED_FIFO ? "FIFO" : p == SCHED_OTHER ? "OTHER" : "other", q, c);
    if (tid == self || found > 0) continue;
    if (p == pol && (pol == SCHED_OTHER || q == prio) && (want_cpu < 0 || c == want_cpu)) found = tid;
  }
  closedir(d);
  return found;
}

/* ---------------------------------------------------------------- tests */

static const char *out_spec;
static int card = -1;
static bool loopback;
static long ncpu_all;            /* CPUs, counted before the main thread is pinned (musl: sysconf counts the mask) */
static char report[2048];
static void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void note(const char *fmt, ...) {
  size_t n = strlen(report);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(report + n, sizeof report - n, fmt, ap);
  va_end(ap);
}

static audio_t *open_out(const char *out, sine_t *src) {
  audio_config_t c;
  memset(&c, 0, sizeof c);
  c.out = out;
  c.rate = RATE;
  c.seed = 11;
  c.music_dir = "/nonexistent";
  char err[200];
  audio_t *a = audio_open(&c, err, sizeof err);
  if (a && src) audio_test_source(a, sine_fn, src);
  audio_levels_t lv = { true, 80, false, 60, AUDIO_MUSIC_GENERATED, false };
  if (a) audio_set_levels(a, &lv);
  return a;
}

static bool wait_ok(audio_t *a, int ms) {
  audio_status_t st;
  for (int t = 0; t <= ms; t += 50) {
    audio_status(a, &st);
    if (st.ok) return true;
    sleep_ms(50);
  }
  return false;
}

static int32_t cap_buf[2 * RATE * 2];
static int32_t pcm_buf[2 * RATE * 2];

/* the main run: S16 on the chosen output, with the caller real-time (rt) or not */
static void test_s16(bool rt) {
  sine_t src;
  memset(&src, 0, sizeof src);
  src.on = 1;
  audio_t *a = open_out(out_spec, &src);
  CHECK(a != NULL);
  if (!a) return;
  audio_status_t st;
  audio_status(a, &st);
  CHECK(st.ok);
  if (!st.ok) { fprintf(stderr, "open %s: %s\n", out_spec, st.error); audio_close(a); return; }
  audio_test_info_t ti;
  audio_test_info(a, &ti);
  int sub = audio_test_subdevice(a);
  printf("%s: device \"%s\", card %d dev %d sub %d, format %d, period %d, buffer %d\n", rt ? "rt" : "normal", st.device,
         ti.card, ti.dev, sub, ti.format, ti.period, ti.buffer);
  char cid[64] = "";
  if (ti.card >= 0) card_id(ti.card, cid, sizeof cid);
  CHECK_INT(ti.format, !strncmp(cid, "vc4hdmi", 7) ? F_IEC958 : F_S16);   /* vc4 HDMI: IEC958 only */
  CHECK_STR(st.error, "");
  CHECK(!strncmp(st.device, "hw:", 3) || !strncmp(st.device, "jack (", 6) || !strncmp(st.device, "hdmi (", 6));
  if (loopback) {
    char want[48];
    snprintf(want, sizeof want, "hw:%d,0 (Loopback)", card);
    if (!strncmp(out_spec, "card:", 5)) CHECK_STR(st.device, want);
    CHECK_INT(ti.period, 256);                 /* snd-aloop takes the preferred geometry */
    CHECK_INT(ti.buffer, 1024);
    CHECK(sub >= 0);
  }
  if (!rt) note("S16 %dx%d; ", ti.period, ti.buffer / (ti.period ? ti.period : 1));

  /* the thread */
  long ncpu = ncpu_all;
  if (rt) {
    /* the caller: FIFO 10 on cpu 0 -> the audio thread: FIFO 9 on the core before (the last one) */
    int want = ncpu >= 2 ? (int)(ncpu - 1) : -1;
    int tid = find_thread(SCHED_FIFO, 9, want, true);
    printf("audio thread (FIFO 9 on cpu %d): %d (%ld cpus)\n", want, tid, ncpu);
    CHECK(tid > 0);
    /* helper threads sfx/music start (the songs decoder) don't inherit the caller's FIFO 10 */
    int inh = find_thread(SCHED_FIFO, 10, -1, false);
    if (inh > 0) printf("thread %d inherited the caller's SCHED_FIFO 10\n", inh);
    CHECK(inh < 0);
    note("thread SCHED_FIFO 9 on cpu %d below a FIFO 10 caller on cpu 0: %s; ", want, tid > 0 ? "yes" : "NO");
  } else {
    /* a normal caller -> a normal thread on the second to last core; nothing real-time */
    int want = ncpu >= 3 ? (int)(ncpu - 2) : -1;
    int tid = find_thread(SCHED_OTHER, 0, want, true);
    printf("audio thread (OTHER on cpu %d): %d\n", want, tid);
    CHECK(tid > 0);
    CHECK(find_thread(SCHED_FIFO, 9, -1, false) < 0);
  }

  /* frames go out at the sample rate */
  sleep_ms(300);
  audio_test_info(a, &ti);
  uint64_t f0 = ti.frames;
  int64_t t0 = now_us();
  sleep_ms(2000);
  audio_test_info(a, &ti);
  double rate = (double)(ti.frames - f0) * 1e6 / (double)(now_us() - t0);
  printf("frames out: %.0f/s\n", rate);
  CHECK_NEAR(rate, RATE, RATE * 0.05);
  audio_status(a, &st);
  uint32_t u0 = st.underruns;
  if (u0) printf("note: %u underruns while idle (a busy VM?)\n", u0);

  int xr;
  char why[200];
  if (loopback) {
    /* the tone arrives whole */
    int n = RATE / 2;
    if (capture(card, sub, F_S16, n, cap_buf, &xr, why, sizeof why) == 0) {
      audio_status(a, &st);
      tone_t r = analyse(cap_buf, n, 1024, 32767.0);
      printf("tone: rms %.4f %.4f (want %.4f %.4f), %.1f Hz, %d jumps, %d silent, capture xruns %d, underruns %u\n", r.rms[0],
             r.rms[1], AL / sqrt(2), AR / sqrt(2), r.freq, r.jumps, r.silent, xr, st.underruns - u0);
      CHECK_NEAR(r.rms[0], AL / sqrt(2), 0.005);
      CHECK_NEAR(r.rms[1], AR / sqrt(2), 0.005);
      CHECK_NEAR(r.freq, FREQ, 2.0);
      if (!xr && st.underruns == u0) CHECK_INT(r.jumps, 0);
      if (!rt) note("tone ok (%.4f/%.4f rms, %.1f Hz, %d gaps); ", r.rms[0], r.rms[1], r.freq, r.jumps);
    } else { fprintf(stderr, "capture: %s\n", why); CHECK(0); }
  }

  /* underrun: the render thread stalls 150 ms */
  audio_status(a, &st);
  u0 = st.underruns;
  src.stall_ms = 150;
  __atomic_store_n(&src.stall_at, __atomic_load_n(&src.frames, __ATOMIC_ACQUIRE) + 1, __ATOMIC_RELEASE);
  for (int t = 0; t < 2000 && !__atomic_load_n(&src.stalls, __ATOMIC_ACQUIRE); t += 10) sleep_ms(10);
  sleep_ms(300);
  audio_status(a, &st);
  audio_test_info(a, &ti);
  printf("after a 150 ms stall: %u underrun(s), ok %d, error \"%s\"\n", st.underruns - u0, st.ok, st.error);
  CHECK_INT(src.stalls, 1);
  CHECK(st.underruns >= u0 + 1);
  CHECK(st.ok);
  CHECK_STR(st.error, "");
  f0 = ti.frames;
  sleep_ms(500);
  audio_test_info(a, &ti);
  CHECK(ti.frames - f0 > RATE / 4);           /* still playing */
  if (!rt) note("underrun recovery ok (%u counted); ", st.underruns - u0);
  if (loopback) {
    u0 = st.underruns;
    int n = RATE / 2;
    if (capture(card, sub, F_S16, n, cap_buf, &xr, why, sizeof why) == 0) {
      audio_status(a, &st);
      tone_t r = analyse(cap_buf, n, 1024, 32767.0);
      printf("tone after the recovery: rms %.4f %.4f, %.1f Hz, %d jumps\n", r.rms[0], r.rms[1], r.freq, r.jumps);
      CHECK_NEAR(r.rms[0], AL / sqrt(2), 0.005);
      CHECK_NEAR(r.freq, FREQ, 2.0);
      if (!xr && st.underruns == u0) CHECK_INT(r.jumps, 0);
    } else { fprintf(stderr, "capture: %s\n", why); CHECK(0); }

    /* mono: (L + R) / 2 on both */
    audio_levels_t lv = { true, 80, false, 60, AUDIO_MUSIC_GENERATED, true };
    audio_set_levels(a, &lv);
    sleep_ms(100);
    if (capture(card, sub, F_S16, RATE / 4, cap_buf, &xr, why, sizeof why) == 0) {
      tone_t r = analyse(cap_buf, RATE / 4, 1024, 32767.0);
      int diff = 0;
      for (int i = 1024; i < RATE / 4; i++) if (cap_buf[2 * i] != cap_buf[2 * i + 1]) diff++;
      printf("mono: rms %.4f %.4f, %d frames L != R\n", r.rms[0], r.rms[1], diff);
      CHECK_INT(diff, 0);
      CHECK_NEAR(r.rms[0], (AL + AR) / 2 / sqrt(2), 0.005);
    } else { fprintf(stderr, "capture: %s\n", why); CHECK(0); }
    lv.mono = false;
    audio_set_levels(a, &lv);

    /* a beep through sfx: silence, then the beep */
    __atomic_store_n(&src.on, 0, __ATOMIC_RELEASE);
    sleep_ms(100);
    if (capture(card, sub, F_S16, RATE / 4, cap_buf, &xr, why, sizeof why) == 0) {
      tone_t q = analyse(cap_buf, RATE / 4, 1024, 32767.0);
      CHECK(q.rms[0] < 1e-4);
      audio_tone(a, 1000, 300, 0, 0, 0);
      if (capture(card, sub, F_S16, RATE / 2, cap_buf, &xr, why, sizeof why) == 0) {
        tone_t b = analyse(cap_buf, RATE / 2, 0, 32767.0);
        printf("beep through sfx: rms %.4f (silence before %.6f)\n", b.rms[0], q.rms[0]);
        CHECK(b.rms[0] > 0.004);
        if (!rt) note("mono ok, a beep through sfx arrives (%.4f rms); ", b.rms[0]);
      } else CHECK(0);
    } else { fprintf(stderr, "capture: %s\n", why); CHECK(0); }
  }

  int64_t c0 = now_us();
  audio_close(a);
  int64_t cms = (now_us() - c0) / 1000;
  printf("close: %lld ms\n", (long long)cms);
  CHECK(cms < 500);
}

/* forced formats on the loopback: S32_LE, IEC958 (natively if the card has it, else carried as S32_LE) */
static void test_formats(void) {
  sine_t src;
  memset(&src, 0, sizeof src);
  src.on = 1;
  audio_status_t st;
  audio_test_info_t ti;
  int xr;
  char why[200];

  audio_test_force_format(F_S32, false);
  audio_t *a = open_out(out_spec, &src);
  audio_status(a, &st);
  audio_test_info(a, &ti);
  CHECK(st.ok);
  CHECK_INT(ti.format, F_S32);
  if (st.ok) {
    sleep_ms(200);
    int n = RATE / 2;
    uint32_t u0 = st.underruns;
    if (capture(card, audio_test_subdevice(a), F_S32, n, cap_buf, &xr, why, sizeof why) == 0) {
      audio_status(a, &st);
      tone_t r = analyse(cap_buf, n, 1024, 2147483647.0);
      int low = 0;
      for (int i = 1024; i < n; i++) if (cap_buf[2 * i] & 0xff) low++;   /* 24 significant bits */
      printf("S32: rms %.4f %.4f, %.1f Hz, %d jumps, %d samples with the low byte set\n", r.rms[0], r.rms[1], r.freq, r.jumps, low);
      CHECK_NEAR(r.rms[0], AL / sqrt(2), 0.005);
      CHECK_NEAR(r.rms[1], AR / sqrt(2), 0.005);
      CHECK_NEAR(r.freq, FREQ, 2.0);
      CHECK_INT(low, 0);
      if (!xr && st.underruns == u0) CHECK_INT(r.jumps, 0);
      note("S32_LE ok; ");
    } else { fprintf(stderr, "capture: %s\n", why); CHECK(0); }
  } else fprintf(stderr, "S32: %s\n", st.error);
  audio_close(a);

  /* IEC958 natively: snd-aloop has no IEC958 format, a real HDMI card does */
  audio_test_force_format(F_IEC958, false);
  a = open_out(out_spec, &src);
  audio_status(a, &st);
  printf("IEC958_SUBFRAME_LE natively: %s%s\n", st.ok ? "accepted" : "refused: ", st.ok ? "" : st.error);
  if (!st.ok) {
    CHECK(strstr(st.error, "IEC958_SUBFRAME_LE") != NULL);
    note("native IEC958 not offered by the card (expected for snd-aloop); ");
  }
  audio_close(a);

  /* IEC958 words carried as S32_LE: check the framing end to end, including the restart after an underrun */
  audio_test_force_format(F_IEC958, true);
  memset(&src, 0, sizeof src);
  src.on = 1;
  a = open_out(out_spec, &src);
  audio_status(a, &st);
  audio_test_info(a, &ti);
  CHECK(st.ok);
  CHECK_INT(ti.format, F_IEC958);
  if (st.ok) {
    sleep_ms(200);
    int n = RATE;                              /* 1 s; the render thread stalls 300 ms in */
    src.stall_ms = 150;
    __atomic_store_n(&src.stall_at, __atomic_load_n(&src.frames, __ATOMIC_ACQUIRE) + RATE * 3 / 10, __ATOMIC_RELEASE);
    uint32_t u0 = st.underruns;
    if (capture(card, audio_test_subdevice(a), F_S32, n, cap_buf, &xr, why, sizeof why) == 0) {
      audio_status(a, &st);
      iec_t r = iec_check(cap_buf, n, pcm_buf);
      printf("IEC958 via S32: %d words, %d Z, %d full status blocks, gaps %d, restarts %d (bad %d), bad parity %d, "
             "preamble %d, V/U %d, block length %d, status %d; underruns %u, stalls %d, capture xruns %d\n",
             r.words, r.z, r.cs_blocks, r.gaps, r.restarts, r.bad_restart, r.bad_parity, r.bad_pre, r.bad_vu, r.bad_block,
             r.bad_cs, st.underruns - u0, src.stalls, xr);
      printf("  gap %d..%d, first bad block ends at %d (length %d)\n", r.gap_at, r.gap_end, r.bad_block_at, r.bad_block_len);
      CHECK(r.words > RATE);
      CHECK_INT(r.bad_parity, 0);
      CHECK_INT(r.bad_pre, 0);
      CHECK_INT(r.bad_vu, 0);
      CHECK_INT(r.bad_cs, 0);
      CHECK(r.cs_blocks >= 100);
      CHECK_INT(src.stalls, 1);
      CHECK(st.underruns >= u0 + 1);
      if (!xr) {
        CHECK_INT(r.bad_block, 0);
        CHECK_INT(r.bad_restart, 0);
        CHECK(r.restarts >= 1);                /* the underrun's gap, then a fresh block */
      }
      /* the samples inside: the tone (frames before the stall) */
      tone_t t = analyse(pcm_buf, RATE / 4, 1024, 32767.0);
      printf("IEC958 samples: rms %.4f %.4f, %.1f Hz, %d jumps\n", t.rms[0], t.rms[1], t.freq, t.jumps);
      CHECK_NEAR(t.rms[0], AL / sqrt(2), 0.005);
      CHECK_NEAR(t.rms[1], AR / sqrt(2), 0.005);
      CHECK_NEAR(t.freq, FREQ, 2.0);
      note("IEC958 framing ok end to end (%d blocks, restart with Z after an underrun: %s); ", r.cs_blocks,
           !xr && r.restarts && !r.bad_restart ? "yes" : "not seen");
    } else { fprintf(stderr, "capture: %s\n", why); CHECK(0); }
  } else fprintf(stderr, "IEC958 as S32: %s\n", st.error);
  audio_close(a);
  audio_test_force_format(-1, false);
}

static void test_errors(void) {
  audio_t *a;
  audio_status_t st;
  if (find_card("Headphones") < 0) {
    a = open_out("jack", NULL);
    audio_status(a, &st);
    CHECK(!st.ok);
    CHECK_STR(st.device, "jack");
    CHECK_STR(st.error, "jack: no 'Headphones' sound card (dtparam=audio=on in config.txt?)");
    audio_close(a);
  }
  if (find_card("vc4hdmi0") < 0 && find_card("vc4hdmi1") < 0) {
    a = open_out("hdmi", NULL);
    audio_status(a, &st);
    CHECK(!st.ok);
    CHECK_STR(st.device, "hdmi");
    CHECK_STR(st.error, "hdmi: no 'vc4hdmi0' sound card; hdmi: no 'vc4hdmi1' sound card");
    audio_close(a);
    if (find_card("Headphones") < 0) {
      a = open_out("auto", NULL);
      audio_status(a, &st);
      CHECK(!st.ok);
      CHECK_STR(st.device, "auto");
      CHECK(!strncmp(st.error, "jack: no 'Headphones'", 21));
      audio_close(a);
    }
  }
  a = open_out("hw:31,0", NULL);
  audio_status(a, &st);
  CHECK(!st.ok);
  CHECK_STR(st.error, "/dev/snd/pcmC31D0p: No such file or directory");
  audio_close(a);
  /* silent while the device is missing, but the mix keeps time (a beep doesn't wait for the device) */
  a = open_out("card:NoSuchCard", NULL);
  audio_tone(a, 1000, 100, 0, 0, 0);
  sleep_ms(300);
  audio_test_info_t ti;
  audio_test_info(a, &ti);
  audio_status(a, &st);
  CHECK(!st.ok);
  CHECK_INT(ti.frames, 0);
  audio_close(a);
  note("error strings ok; ");
}

static int run(const char *cmd) {
  int r = system(cmd);
  return r == -1 ? -1 : WIFEXITED(r) ? WEXITSTATUS(r) : -1;
}

static void test_hotplug(void) {
  if (run("rmmod snd_aloop 2>/dev/null") != 0) {
    printf("hotplug: can't unload snd-aloop (not root, or in use): skipped\n");
    note("hotplug skipped; ");
    return;
  }
  audio_t *a = open_out("card:Loopback", NULL);
  audio_status_t st;
  audio_status(a, &st);
  printf("hotplug: without the card: ok %d, error \"%s\"\n", st.ok, st.error);
  CHECK(!st.ok);
  CHECK_STR(st.device, "card:Loopback");
  CHECK_STR(st.error, "card: no 'Loopback' sound card");
  sleep_ms(1000);
  int64_t t0 = now_us();
  CHECK_INT(run("modprobe snd-aloop"), 0);
  bool back = wait_ok(a, 5000);
  double secs = (double)(now_us() - t0) / 1e6;
  audio_status(a, &st);
  printf("hotplug: card back -> output ok %d after %.1f s (\"%s\")\n", st.ok, secs, st.device);
  CHECK(back);
  CHECK(secs < 4.0);                           /* retried every 3 s */
  if (back) {
    audio_test_info_t ti;
    audio_test_info(a, &ti);
    uint64_t f0 = ti.frames;
    sleep_ms(500);
    audio_test_info(a, &ti);
    CHECK(ti.frames - f0 > RATE / 4);
  }
  audio_close(a);
  note("hotplug: the output came back by itself %.1f s after the card; ", secs);
}

int main(int argc, char **argv) {
  bool hotplug = false;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--hotplug")) hotplug = true;
    else out_spec = argv[i];
  }
  setvbuf(stdout, NULL, _IOLBF, 0);
  char spec[64];
  if (!out_spec) {
    if ((card = find_card("Loopback")) >= 0) out_spec = "card:Loopback";
    else if ((card = find_card("Headphones")) >= 0) out_spec = "jack";          /* a Pi */
    else if ((card = first_playback_card()) >= 0) { snprintf(spec, sizeof spec, "hw:%d,0", card); out_spec = spec; }
    else {
      printf("test_audio_alsa: no ALSA playback device (try: modprobe snd-aloop): nothing to test, skipped\n");
      T_DONE();
    }
  } else if (!strncmp(out_spec, "card:", 5)) {
    char id[48];
    snprintf(id, sizeof id, "%s", out_spec + 5);
    id[strcspn(id, ",")] = 0;
    card = find_card(id);
  } else if (!strncmp(out_spec, "hw:", 3)) card = atoi(out_spec + 3);
  char id[64] = "";
  if (card >= 0) card_id(card, id, sizeof id);
  loopback = !strcmp(id, "Loopback");
  printf("output %s (card %d \"%s\")%s\n", out_spec, card, id, loopback ? ": loopback, checking the samples" : "");

  ncpu_all = sysconf(_SC_NPROCESSORS_ONLN);
  test_errors();

  /* like the frame loop: SCHED_FIFO 10, pinned to cpu 0 -> the audio thread: FIFO 9, next core */
  struct sched_param sp = { .sched_priority = 10 };
  cpu_set_t one, all;
  CPU_ZERO(&one);
  CPU_SET(0, &one);
  sched_getaffinity(0, sizeof all, &all);
  int e = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);   /* musl: sched_setscheduler is ENOSYS */
  if (e == 0) {
    sched_setaffinity(0, sizeof one, &one);
    test_s16(true);
    sp.sched_priority = 0;
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
    sched_setaffinity(0, sizeof all, &all);
  } else {
    printf("no SCHED_FIFO for the caller (%s): real-time thread test skipped\n", strerror(e));
    note("real-time test skipped (not permitted); ");
  }
  test_s16(false);
  if (loopback) test_formats();
  if (hotplug && loopback) test_hotplug();

  printf("summary: %s\n", report);
  T_DONE();
}
#endif
