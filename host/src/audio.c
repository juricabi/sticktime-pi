/* Sound output: see audio.h and audio_internal.h.
 *
 * One mix path for every output: blocks of 128 frames, zeroed float L/R -> sfx_render (motors, beeps)
 * -> music_render -> mono option -> clip (NaN -> 0) -> the output format.
 *
 * Outputs:
 *   "jack" | "hdmi" | "auto"   the Pi's ALSA cards through raw kernel PCM ioctls (no alsa-lib: the binary
 *                              is static musl). Cards by /proc/asound/card<N>/id: "Headphones" = the 3.5 mm
 *                              jack (snd_bcm2835), "vc4hdmi0"/"vc4hdmi1" = HDMI. auto: HDMI when the video
 *                              goes to HDMI, else the jack (then the other one).
 *   "hw:<card>[,<dev>]"        any ALSA PCM by number, "card:<id>[,<dev>]" by card id (USB sound cards, tests)
 *   "wav:<path>"               float32 stereo WAV (the reference harness' layout), "wav16:<path>" 16-bit PCM
 *                              (what the jack gets); offline: audio_render_offline() renders, no thread
 *   "null"                     offline rendering that is discarded; "off" nothing at all
 *
 * ALSA: S16_LE stereo at the configured rate (48 kHz), 256-frame periods x 4 (else the nearest the
 * driver accepts), start at 2 periods, non-blocking writes paced by poll(): the thread waits for room
 * for a period, then renders it (rendered as late as possible: about one buffer of latency). Underruns
 * (-EPIPE) are recovered with PREPARE and counted; 50 within 2 s count as a broken device. Errors,
 * unplugging or a stalled device close it: the mix keeps running silently in real time (so the game's
 * sounds don't pile up) and the device is retried every 3 s. The kernel's silence fill keeps the unwritten
 * part of the ring silent, so an underrun is a gap, not a replay of old audio. Only the configured rate
 * is offered (the mix isn't resampled: the Pi's jack and HDMI take 48 kHz). vc4 HDMI only takes
 * IEC958_SUBFRAME_LE: the 16-bit samples are packed into IEC 60958 subframes exactly as alsa-lib's
 * pcm_iec958.c does for an S16 client of Pi OS's vc4-hdmi.conf (preambles Z/X/Y = 0x08/0x02/0x04,
 * channel status AES0..4 = 04 82 00 02 02: consumer, not copyright, original, PCM coder, 48 kHz,
 * 16-bit; parity over bits 4-30; the 192-frame block counter restarts at every PREPARE). The
 * "IEC958 Playback Default" control that vc4-hdmi.conf's hook writes is left alone: vc4 only reads it
 * to tell non-audio (HBR) streams from PCM, and the channel status travels in the subframes.
 *
 * The audio thread is created SCHED_FIFO one step below the frame loop when the frame loop (the
 * thread calling audio_open) is real-time and the system allows it (else normal), and pinned to the
 * core before the frame loop's (with no pin: the second to last core). It never allocates and does no
 * file I/O except the device itself (the ALSA write, and /proc + open when it retries). */
#ifdef __linux__
#define _GNU_SOURCE
#endif
#include "audio.h"
#include "audio_internal.h"
#include "plat.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <sound/asound.h>
#endif

#define BLOCK      128           /* frames per mix block */
#define CHUNK_MAX  4096          /* frames per ALSA write at most (buffers preallocated) */
#define RETRY_US   3000000       /* retry a missing/failed device every 3 s */
#define STALL_MS   2000          /* no room in the device buffer for this long: treat it as gone */
#define XRUN_STORM 50            /* this many underruns within XRUN_WIN_US: the device is broken, reopen */
#define XRUN_WIN_US 2000000

enum { OUT_OFF, OUT_NULL, OUT_WAV, OUT_ALSA };
enum { FMT_S16, FMT_S32, FMT_IEC958, FMT_F32 };

struct audio {
  int kind;
  int rate;
  sfx_t *sfx;
  music_t *music;

  /* levels (any thread) */
  int mono;                          /* atomic */
  bool lv_applied;
  float lv_sfx_vol, lv_music_vol;
  bool lv_music_on;
  int lv_source;

  uint32_t underruns;                /* atomic */
  uint64_t frames_out;               /* atomic: frames handed to the output */
  void (*test_fn)(void *ud, float *L, float *R, int n);
  void *test_ud;

  /* status: the strings change only when the device opens or fails */
  plat_mutex mu;
  bool ok;
  char device[48];
  char error[128];

  /* wav */
  FILE *wav;
  int wav_fmt;                       /* FMT_F32 | FMT_S16 */
  uint64_t wav_frames;
  bool wav_failed;

  /* alsa */
  char target[96];                   /* what out asked for: auto, jack, hdmi, hw:1,0, card:Loopback */
  bool video_hdmi;
  bool thread_started;
  plat_thread th;
  int quit;                          /* atomic */
  int fd;
  int fmt, period, buffer, card, dev, subdev;
  unsigned iec_counter;
  uint8_t iec_status[24];
  int rt_prio, cpu;
  int64_t xrun_t0;                   /* underrun storm detection: count since xrun_t0 */
  int xrun_n;

  float L[BLOCK], R[BLOCK];
  int16_t stage[CHUNK_MAX * 2];      /* IEC958: the 16-bit samples of the chunk being written */
  union { int16_t s16[CHUNK_MAX * 2]; int32_t s32[CHUNK_MAX * 2]; uint32_t u32[CHUNK_MAX * 2]; float f32[CHUNK_MAX * 2]; } out;
};

/* ======================================================================= helpers (also used by tests) */

/* IEC 60958 subframe of one 16-bit sample, as alsa-lib's iec958_subframe() builds it from an S16 source:
 * the sample as S32 (s16 << 16) shifted right by 4 (bits 4-27; 12-27 for 16-bit), V = U = 0, the channel
 * status bit for this frame of the 192-frame block in bit 30, even parity over bits 4-31 in bit 31, the
 * preamble in bits 0-3: Z (block start, left), X (other left), Y (right). */
uint32_t audio_iec958_subframe(int16_t s, unsigned counter, int channel, const uint8_t status[24]) {
  uint32_t d = (uint32_t)(uint16_t)s << 12;
  if (status[(counter >> 3) % 24] & (1u << (counter & 7))) d |= 0x40000000u;
  if (__builtin_parity(d & 0x7ffffff0u)) d |= 0x80000000u;
  d |= channel ? 0x04u : counter == 0 ? 0x08u : 0x02u;
  return d;
}

/* interleaved stereo S16 -> IEC958_SUBFRAME_LE words; *counter = frame in the 192-frame block (kept across calls) */
void audio_iec958_pack(uint32_t *dst, const int16_t *src, int frames, unsigned *counter, const uint8_t status[24]) {
  unsigned c = *counter % 192;
  for (int i = 0; i < frames; i++) {
    uint32_t l = audio_iec958_subframe(src[2 * i], c, 0, status);
    uint32_t r = audio_iec958_subframe(src[2 * i + 1], c, 1, status);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    l = __builtin_bswap32(l);
    r = __builtin_bswap32(r);
#endif
    dst[2 * i] = l;
    dst[2 * i + 1] = r;
    if (++c == 192) c = 0;
  }
  *counter = c;
}

/* the channel status alsa-lib sends through Pi OS's vc4-hdmi.conf (AES0-3 = 0x04 0x82 0x00 0x01, the rate
 * filled in by the iec958 plugin's hw_params) for a 16-bit client: AES4 = 20/16-bit word length */
void audio_iec958_status(uint8_t status[24], int rate) {
  memset(status, 0, 24);
  status[0] = 0x04;                  /* consumer, PCM, not copyright, no emphasis */
  status[1] = 0x82;                  /* original, PCM coder */
  status[2] = 0x00;                  /* source and channel unspecified */
  switch (rate) {
    case 22050: status[3] = 0x04; break;
    case 24000: status[3] = 0x06; break;
    case 32000: status[3] = 0x03; break;
    case 44100: status[3] = 0x00; break;
    case 48000: status[3] = 0x02; break;
    case 88200: status[3] = 0x08; break;
    case 96000: status[3] = 0x0a; break;
    case 176400: status[3] = 0x0c; break;
    case 192000: status[3] = 0x0e; break;
    case 768000: status[3] = 0x09; break;
    default: status[3] = 0x01; break;   /* not indicated */
  }
  status[4] = 0x02;                  /* word length 20/16 bits (max 20): an S16 source */
}

/* audio_levels_t -> what sfx and music get */
void audio_levels_map(const audio_levels_t *lv, float *sfx_vol, bool *music_on, float *music_vol, int *source) {
  int v = lv->volume < 0 ? 0 : lv->volume > 100 ? 100 : lv->volume;
  int mv = lv->music_volume < 0 ? 0 : lv->music_volume > 100 ? 100 : lv->music_volume;
  *sfx_vol = lv->sound ? (float)v / 100.0f : 0.0f;      /* sfx squares it (the browser's volGain) */
  *music_on = lv->music;
  *music_vol = (float)mv / 100.0f;                      /* music: bus 0.6 * vol^2 */
  *source = lv->music_source == AUDIO_MUSIC_SONGS ? AUDIO_MUSIC_SONGS : AUDIO_MUSIC_GENERATED;
}

/* tests: a signal added to the mix before sfx and music (called in the rendering thread) */
void audio_test_source(audio_t *a, void (*fn)(void *ud, float *L, float *R, int n), void *ud) {
  if (!a) return;
  a->test_ud = ud;
  __atomic_store_n(&a->test_fn, fn, __ATOMIC_RELEASE);
}

/* tests: what the output is doing (format: 0 S16_LE, 1 S32_LE, 2 IEC958_SUBFRAME_LE) */
typedef struct { uint64_t frames; int period, buffer, format, card, dev; } audio_test_info_t;
void audio_test_info(audio_t *a, audio_test_info_t *ti) {
  memset(ti, 0, sizeof *ti);
  if (!a) return;
  ti->frames = __atomic_load_n(&a->frames_out, __ATOMIC_ACQUIRE);
  ti->period = a->period;
  ti->buffer = a->buffer;
  ti->format = a->fmt;
  ti->card = a->card;
  ti->dev = a->dev;
}
/* tests: the ALSA subdevice the output opened (-1 none), to find its snd-aloop capture twin */
int audio_test_subdevice(audio_t *a) { return a ? a->subdev : -1; }

/* tests: devices opened from now on take only this format (-1 = normal choice); iec_as_s32: the
 * IEC958 words go to the device as S32_LE (same 32-bit container) so a loopback card that has no
 * IEC958 format can carry them back to the test unchanged */
static int test_fmt = -1;
static bool test_iec_as_s32;
void audio_test_force_format(int fmt, bool iec_as_s32) {
  test_fmt = fmt;
  test_iec_as_s32 = iec_as_s32;
}

static void set_status(audio_t *a, bool ok, const char *device, const char *error) {
  plat_mutex_lock(&a->mu);
  a->ok = ok;
  if (device) plat_strlcpy(a->device, device, sizeof a->device);
  plat_strlcpy(a->error, error ? error : "", sizeof a->error);
  plat_mutex_unlock(&a->mu);
}

static inline float clip1(float x) {
  if (x != x) return 0.0f;           /* NaN from a broken voice: silence, not full scale */
  return x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
}
static inline int16_t to_s16(float x) { return (int16_t)lrintf(x * 32767.0f); }

/* ======================================================================= the mix */

static void mix_block(audio_t *a, int n) {
  float *L = a->L, *R = a->R;
  memset(L, 0, sizeof(float) * (size_t)n);
  memset(R, 0, sizeof(float) * (size_t)n);
  void (*fn)(void *, float *, float *, int) = __atomic_load_n(&a->test_fn, __ATOMIC_ACQUIRE);
  if (fn) fn(a->test_ud, L, R, n);
  if (a->sfx) sfx_render(a->sfx, L, R, n);
  if (a->music) music_render(a->music, L, R, n);
  if (__atomic_load_n(&a->mono, __ATOMIC_RELAXED)) {
    for (int i = 0; i < n; i++) {
      float m = clip1(0.5f * (L[i] + R[i]));
      L[i] = R[i] = m;
    }
  } else {
    for (int i = 0; i < n; i++) {
      L[i] = clip1(L[i]);
      R[i] = clip1(R[i]);
    }
  }
}

/* the block in a->L/R -> interleaved fmt at frame offset off of a->out */
static void convert_block(audio_t *a, int fmt, int off, int n) {
  const float *L = a->L, *R = a->R;
  switch (fmt) {
    case FMT_S16: {
      int16_t *d = a->out.s16 + 2 * off;
      for (int i = 0; i < n; i++) { d[2 * i] = to_s16(L[i]); d[2 * i + 1] = to_s16(R[i]); }
      break;
    }
    case FMT_S32: {                  /* 24 significant bits */
      int32_t *d = a->out.s32 + 2 * off;
      for (int i = 0; i < n; i++) {
        d[2 * i] = (int32_t)lrintf(L[i] * 8388607.0f) * 256;
        d[2 * i + 1] = (int32_t)lrintf(R[i] * 8388607.0f) * 256;
      }
      break;
    }
    case FMT_IEC958: {
      int16_t *s = a->stage + 2 * off;
      for (int i = 0; i < n; i++) { s[2 * i] = to_s16(L[i]); s[2 * i + 1] = to_s16(R[i]); }
      audio_iec958_pack(a->out.u32 + 2 * off, s, n, &a->iec_counter, a->iec_status);
      break;
    }
    case FMT_F32: {
      float *d = a->out.f32 + 2 * off;
      for (int i = 0; i < n; i++) { d[2 * i] = L[i]; d[2 * i + 1] = R[i]; }
      break;
    }
  }
}

/* ======================================================================= levels and the game */

void audio_set_levels(audio_t *a, const audio_levels_t *lv) {
  if (!a || !lv) return;
  float sv, mv;
  bool mon;
  int src;
  audio_levels_map(lv, &sv, &mon, &mv, &src);
  __atomic_store_n(&a->mono, lv->mono ? 1 : 0, __ATOMIC_RELAXED);
  /* only changes reach sfx and music: a settings reload must not restart a song */
  if (a->sfx && (!a->lv_applied || sv != a->lv_sfx_vol)) sfx_set_volume(a->sfx, sv);
  if (a->music && (!a->lv_applied || mon != a->lv_music_on || mv != a->lv_music_vol || src != a->lv_source))
    music_set(a->music, mon, mv, src);
  a->lv_sfx_vol = sv;
  a->lv_music_on = mon;
  a->lv_music_vol = mv;
  a->lv_source = src;
  a->lv_applied = true;
}

void audio_game(audio_t *a, const double *vals, int n, double dt_ms) {
  if (a && a->sfx) sfx_frame(a->sfx, vals, n, dt_ms);
}
void audio_tone(audio_t *a, int freq, int len_ms, int pause_ms, int flags, int freq_incr) {
  if (a && a->sfx) sfx_tone(a->sfx, freq, len_ms, pause_ms, flags, freq_incr);
}
/* playHaptic makes no sound (the browser shakes the radio picture and vibrates a phone) */
void audio_haptic(audio_t *a, int ms) { (void)a; (void)ms; }
void audio_game_reset(audio_t *a) {
  if (a && a->sfx) sfx_reset(a->sfx);
}
void audio_music_next(audio_t *a) {
  if (a && a->music) music_next(a->music);
}
void audio_songs_changed(audio_t *a) {
  if (a && a->music) music_songs_changed(a->music);
}

void audio_status(audio_t *a, audio_status_t *st) {
  memset(st, 0, sizeof *st);
  if (!a) { plat_strlcpy(st->device, "off", sizeof st->device); return; }
  plat_mutex_lock(&a->mu);
  st->ok = a->ok;
  plat_strlcpy(st->device, a->device, sizeof st->device);
  plat_strlcpy(st->error, a->error, sizeof st->error);
  plat_mutex_unlock(&a->mu);
  if (a->music) music_now_playing(a->music, st->now_playing, sizeof st->now_playing);
  st->now_playing[sizeof st->now_playing - 1] = 0;
  st->underruns = __atomic_load_n(&a->underruns, __ATOMIC_RELAXED);
}

/* ======================================================================= WAV file */

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* the 44-byte header (same layout as the reference harness' write_wav_f32) */
static int wav_header(audio_t *a) {
  int bytes = a->wav_fmt == FMT_F32 ? 4 : 2;
  uint64_t data = a->wav_frames * 2u * (unsigned)bytes;
  if (data > 0xffffffffull - 36) data = 0xffffffffull - 36;
  uint8_t h[44];
  memcpy(h, "RIFF", 4);
  put32(h + 4, (uint32_t)(36 + data));
  memcpy(h + 8, "WAVEfmt ", 8);
  put32(h + 16, 16);
  put16(h + 20, a->wav_fmt == FMT_F32 ? 3 : 1);          /* IEEE float | PCM */
  put16(h + 22, 2);
  put32(h + 24, (uint32_t)a->rate);
  put32(h + 28, (uint32_t)a->rate * 2u * (unsigned)bytes);
  put16(h + 32, 2u * (unsigned)bytes);
  put16(h + 34, 8u * (unsigned)bytes);
  memcpy(h + 36, "data", 4);
  put32(h + 40, (uint32_t)data);
  if (fseek(a->wav, 0, SEEK_SET) != 0) return -1;
  if (fwrite(h, sizeof h, 1, a->wav) != 1) return -1;
  if (fseek(a->wav, 0, SEEK_END) != 0) return -1;
  return fflush(a->wav) == 0 ? 0 : -1;
}

static void wav_fail(audio_t *a) {
  char e[128];
  snprintf(e, sizeof e, "writing the WAV file: %s", strerror(errno));
  a->wav_failed = true;
  set_status(a, false, NULL, e);
}

static void wav_write(audio_t *a, int n) {
  if (!a->wav || a->wav_failed) return;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  if (a->wav_fmt == FMT_F32) for (int i = 0; i < 2 * n; i++) a->out.u32[i] = __builtin_bswap32(a->out.u32[i]);
  else for (int i = 0; i < 2 * n; i++) a->out.s16[i] = (int16_t)__builtin_bswap16((uint16_t)a->out.s16[i]);
#endif
  size_t bytes = (size_t)n * 2u * (a->wav_fmt == FMT_F32 ? 4u : 2u);
  if (fwrite(&a->out, bytes, 1, a->wav) != 1) { wav_fail(a); return; }
  a->wav_frames += (uint64_t)n;
}

void audio_render_offline(struct audio *a, int nsamples) {
  if (!a || nsamples <= 0 || (a->kind != OUT_WAV && a->kind != OUT_NULL)) return;
  for (int done = 0; done < nsamples;) {
    int n = nsamples - done < BLOCK ? nsamples - done : BLOCK;
    mix_block(a, n);
    if (a->kind == OUT_WAV) {
      convert_block(a, a->wav_fmt, 0, n);
      wav_write(a, n);
    }
    done += n;
  }
  __atomic_add_fetch(&a->frames_out, (uint64_t)nsamples, __ATOMIC_RELEASE);
  if (a->wav && !a->wav_failed && wav_header(a) != 0) wav_fail(a);
}

/* ======================================================================= ALSA (Linux) */
#ifdef __linux__

/* frames (<= CHUNK_MAX) of the mix into a->out as fmt */
static void render_out(audio_t *a, int fmt, int frames) {
  for (int done = 0; done < frames;) {
    int n = frames - done < BLOCK ? frames - done : BLOCK;
    mix_block(a, n);
    convert_block(a, fmt, done, n);
    done += n;
  }
}

typedef struct snd_pcm_hw_params hwp_t;

static struct snd_mask *hp_mask(hwp_t *p, int n) { return &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK]; }
static struct snd_interval *hp_iv(hwp_t *p, int n) { return &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL]; }

/* everything allowed (alsa-lib's _snd_pcm_hw_params_any) */
static void hp_any(hwp_t *p) {
  memset(p, 0, sizeof *p);
  for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++)
    memset(hp_mask(p, n)->bits, 0xff, sizeof hp_mask(p, n)->bits);
  for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
    struct snd_interval *i = hp_iv(p, n);
    i->min = 0;
    i->max = UINT_MAX;
  }
  p->rmask = ~0u;
  p->cmask = 0;
  p->info = ~0u;
}
static void hp_set_mask(hwp_t *p, int n, unsigned v) {
  struct snd_mask *m = hp_mask(p, n);
  memset(m, 0, sizeof *m);
  m->bits[v >> 5] |= 1u << (v & 31);
}
static void hp_set_int(hwp_t *p, int n, unsigned v) {
  struct snd_interval *i = hp_iv(p, n);
  i->min = i->max = v;
  i->openmin = i->openmax = 0;
  i->integer = 1;
  i->empty = 0;
}
static int hp_refine(int fd, hwp_t *p) {
  p->rmask = ~0u;
  p->cmask = 0;
  return ioctl(fd, SNDRV_PCM_IOCTL_HW_REFINE, p);
}
static unsigned iv_lo(struct snd_interval *i) { return i->min + (i->openmin ? 1 : 0); }
static unsigned iv_hi(struct snd_interval *i) { return i->max - (i->openmax && i->max ? 1 : 0); }
static unsigned clampu(unsigned v, unsigned lo, unsigned hi) { return v < lo ? lo : v > hi ? hi : v; }

static unsigned alsa_format(int fmt) {
  switch (fmt) {
    case FMT_S32: return (unsigned)SNDRV_PCM_FORMAT_S32_LE;
    case FMT_IEC958: return test_iec_as_s32 ? (unsigned)SNDRV_PCM_FORMAT_S32_LE : (unsigned)SNDRV_PCM_FORMAT_IEC958_SUBFRAME_LE;
    default: return (unsigned)SNDRV_PCM_FORMAT_S16_LE;
  }
}
static const char *fmt_name(int fmt) {
  return fmt == FMT_S32 ? "S32_LE" : fmt == FMT_IEC958 ? "IEC958_SUBFRAME_LE" : "S16_LE";
}

/* hw params: the first format of fmts the device takes, 2 channels at a->rate, periods near 256 x 4 */
static int pcm_hw(audio_t *a, int fd, const int *fmts, int nfmts, char *why, size_t whylen) {
  static const unsigned psz[] = { 256, 512, 128, 1024, 2048, 4096 };
  static const unsigned pcnt[] = { 4, 3, 5, 6, 8, 2 };
  hwp_t base, t;
  for (int fi = 0; fi < nfmts; fi++) {
    hp_any(&base);
    hp_set_mask(&base, SNDRV_PCM_HW_PARAM_ACCESS, (unsigned)SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    hp_set_mask(&base, SNDRV_PCM_HW_PARAM_FORMAT, alsa_format(fmts[fi]));
    hp_set_mask(&base, SNDRV_PCM_HW_PARAM_SUBFORMAT, (unsigned)SNDRV_PCM_SUBFORMAT_STD);
    hp_set_int(&base, SNDRV_PCM_HW_PARAM_CHANNELS, 2);
    hp_set_int(&base, SNDRV_PCM_HW_PARAM_RATE, (unsigned)a->rate);
    if (hp_refine(fd, &base) < 0) {
      snprintf(why, whylen, "no %s stereo at %d Hz", fmt_name(fmts[fi]), a->rate);
      continue;
    }
    bool done = false;
    for (size_t i = 0; i < sizeof psz / sizeof psz[0] && !done; i++)
      for (size_t j = 0; j < sizeof pcnt / sizeof pcnt[0] && !done; j++) {
        t = base;
        hp_set_int(&t, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, psz[i]);
        hp_set_int(&t, SNDRV_PCM_HW_PARAM_PERIODS, pcnt[j]);
        if (hp_refine(fd, &t) == 0 && ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &t) == 0) done = true;
      }
    if (!done) {                     /* what the driver allows, nearest to 256 x 4 */
      t = base;
      struct snd_interval *ps = hp_iv(&t, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
      hp_set_int(&t, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, clampu(256, iv_lo(ps), iv_hi(ps)));
      if (hp_refine(fd, &t) == 0) {
        struct snd_interval *pc = hp_iv(&t, SNDRV_PCM_HW_PARAM_PERIODS);
        hp_set_int(&t, SNDRV_PCM_HW_PARAM_PERIODS, clampu(4, iv_lo(pc) < 2 ? 2 : iv_lo(pc), iv_hi(pc)));
        if (hp_refine(fd, &t) == 0 && ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &t) == 0) done = true;
      }
    }
    if (!done) {                     /* the driver's own choice */
      t = base;
      if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &t) == 0) done = true;
    }
    if (!done) {
      snprintf(why, whylen, "hw_params (%s): %s", fmt_name(fmts[fi]), strerror(errno));
      continue;
    }
    a->fmt = fmts[fi];
    a->period = (int)hp_iv(&t, SNDRV_PCM_HW_PARAM_PERIOD_SIZE)->min;
    a->buffer = (int)hp_iv(&t, SNDRV_PCM_HW_PARAM_BUFFER_SIZE)->min;
    if (a->period <= 0 || a->buffer < a->period) {
      snprintf(why, whylen, "odd period %d / buffer %d", a->period, a->buffer);
      return -1;
    }
    return 0;
  }
  return -1;
}

/* sw params: start at 2 periods, wake at 1 period of room, stop on underrun, and silence fill in
 * "top-up" mode (silence_size = boundary): the kernel keeps the part of the ring the thread hasn't
 * written yet silent, so an underrun is a short gap instead of the hardware replaying the last
 * buffer's worth of old audio until the next period interrupt notices */
static int pcm_sw(audio_t *a, int fd) {
  struct snd_pcm_sw_params sp;
  memset(&sp, 0, sizeof sp);
  snd_pcm_uframes_t period = (snd_pcm_uframes_t)a->period, buffer = (snd_pcm_uframes_t)a->buffer;
  sp.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
  sp.period_step = 1;
  sp.avail_min = period;
  sp.xfer_align = 1;
  sp.start_threshold = 2 * period < buffer ? 2 * period : buffer;
  sp.stop_threshold = buffer;
  snd_pcm_uframes_t boundary = buffer;           /* the kernel's own formula (snd_pcm_hw_params) */
  while (boundary * 2 <= (snd_pcm_uframes_t)LONG_MAX - buffer) boundary *= 2;
  sp.boundary = boundary;
  sp.silence_threshold = 0;
  sp.silence_size = boundary;
  if (ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sp) == 0) return 0;
  sp.silence_size = 0;                           /* an old kernel: no silence fill */
  return ioctl(fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sp);
}

/* /proc/asound/card<N>/id -> out ("" if there is no such card); plain read(): no stdio buffers, so the
 * audio thread allocates nothing even when it retries a device */
static void card_id(int card, char *out, size_t n) {
  char p[64];
  snprintf(p, sizeof p, "/proc/asound/card%d/id", card);
  out[0] = 0;
  int fd = open(p, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  ssize_t r = read(fd, out, n - 1);
  close(fd);
  out[r > 0 ? r : 0] = 0;
  out[strcspn(out, "\r\n")] = 0;
}
static int find_card(const char *id) {
  for (int c = 0; c < 32; c++) {
    char buf[64];
    card_id(c, buf, sizeof buf);
    if (buf[0] && !strcmp(buf, id)) return c;
  }
  return -1;
}

typedef struct { const char *kind; const char *id; int card, dev; } cand_t;

static void add_reason(char *why, size_t len, const char *r) {
  size_t n = strlen(why);
  if (n && n + 2 < len) { plat_strlcpy(why + n, "; ", len - n); n += 2; }
  if (n < len) plat_strlcpy(why + n, r, len - n);
}

/* open and configure the device out asked for; 0 = a->fd ready (prepared), else why says why not */
static int alsa_try_open(audio_t *a, char *why, size_t whylen) {
  cand_t c[4];
  int nc = 0;
  char idbuf[64] = "";
  const cand_t JACK = { "jack", "Headphones", -1, 0 }, HD0 = { "hdmi", "vc4hdmi0", -1, 0 }, HD1 = { "hdmi", "vc4hdmi1", -1, 0 };
  const char *t = a->target;
  why[0] = 0;
  if (!strcmp(t, "jack")) c[nc++] = JACK;
  else if (!strcmp(t, "hdmi")) { c[nc++] = HD0; c[nc++] = HD1; }
  else if (!strcmp(t, "auto")) {
    if (a->video_hdmi) { c[nc++] = HD0; c[nc++] = HD1; c[nc++] = JACK; }
    else { c[nc++] = JACK; c[nc++] = HD0; c[nc++] = HD1; }
  } else if (!strncmp(t, "hw:", 3)) {
    cand_t h = { "hw", NULL, 0, 0 };
    if (sscanf(t + 3, "%d,%d", &h.card, &h.dev) < 1) { snprintf(why, whylen, "bad output '%s'", t); return -1; }
    c[nc++] = h;
  } else if (!strncmp(t, "card:", 5)) {
    cand_t h = { "card", idbuf, -1, 0 };
    plat_strlcpy(idbuf, t + 5, sizeof idbuf);
    char *comma = strchr(idbuf, ',');
    if (comma) { *comma = 0; h.dev = atoi(comma + 1); }
    c[nc++] = h;
  }
  for (int i = 0; i < nc; i++) {
    char r[160];
    int card = c[i].id ? find_card(c[i].id) : c[i].card;
    if (card < 0) {
      if (!strcmp(c[i].kind, "jack")) snprintf(r, sizeof r, "jack: no 'Headphones' sound card (dtparam=audio=on in config.txt?)");
      else snprintf(r, sizeof r, "%s: no '%s' sound card", c[i].kind, c[i].id);
      add_reason(why, whylen, r);
      continue;
    }
    char path[64], id[64];
    snprintf(path, sizeof path, "/dev/snd/pcmC%dD%dp", card, c[i].dev);
    card_id(card, id, sizeof id);
    int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
      if (!strcmp(c[i].kind, "hdmi") && errno == ENODEV) snprintf(r, sizeof r, "hdmi (%s): no HDMI sink with audio", id);
      else snprintf(r, sizeof r, "%s: %s", path, strerror(errno));
      add_reason(why, whylen, r);
      continue;
    }
    int ver = 0;
    if (ioctl(fd, SNDRV_PCM_IOCTL_PVERSION, &ver) < 0 || SNDRV_PROTOCOL_MAJOR(ver) != 2) {
      snprintf(r, sizeof r, "%s: not an ALSA PCM", path);
      add_reason(why, whylen, r);
      close(fd);
      continue;
    }
    static const int F_JACK[] = { FMT_S16, FMT_S32, FMT_IEC958 }, F_HDMI[] = { FMT_IEC958, FMT_S16, FMT_S32 };
    bool hdmi = !strcmp(c[i].kind, "hdmi") || !strncmp(id, "vc4hdmi", 7);
    const int *fl = hdmi ? F_HDMI : F_JACK;
    int nf = 3;
    if (test_fmt >= FMT_S16 && test_fmt <= FMT_IEC958) { fl = &test_fmt; nf = 1; }
    char hw[96] = "";
    if (pcm_hw(a, fd, fl, nf, hw, sizeof hw) != 0) {
      snprintf(r, sizeof r, "%s (%s): %s", c[i].kind, id, hw);
      add_reason(why, whylen, r);
      close(fd);
      continue;
    }
    if (pcm_sw(a, fd) < 0 || ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
      snprintf(r, sizeof r, "%s (%s): sw_params/prepare: %s", c[i].kind, id, strerror(errno));
      add_reason(why, whylen, r);
      close(fd);
      continue;
    }
    struct snd_pcm_info info;
    memset(&info, 0, sizeof info);
    a->subdev = ioctl(fd, SNDRV_PCM_IOCTL_INFO, &info) == 0 ? (int)info.subdevice : -1;
    a->fd = fd;
    a->card = card;
    a->dev = c[i].dev;
    a->iec_counter = 0;
    a->xrun_n = 0;
    a->xrun_t0 = plat_now_us();
    audio_iec958_status(a->iec_status, a->rate);
    char dev[48];
    if (!strcmp(c[i].kind, "jack") || !strcmp(c[i].kind, "hdmi")) snprintf(dev, sizeof dev, "%s (%s)", c[i].kind, id);
    else snprintf(dev, sizeof dev, "hw:%d,%d (%s)", card, c[i].dev, id);
    set_status(a, true, dev, "");
    return 0;
  }
  if (!nc) snprintf(why, whylen, "unknown output '%s'", t);
  return -1;
}

static void alsa_close(audio_t *a) {
  if (a->fd < 0) return;
  ioctl(a->fd, SNDRV_PCM_IOCTL_DROP);
  close(a->fd);
  a->fd = -1;
  a->subdev = -1;
}

#define E_XRUN_STORM 100000      /* alsa_write's own "error": underruns without end */

/* wait until the device has room for a period (so the block is rendered as late as possible: less
 * latency than rendering first and then waiting); 0 ready (or an error state the write will report),
 * 1 quitting, -ETIMEDOUT the device stopped taking audio */
static int alsa_wait_room(audio_t *a) {
  for (int stall_ms = 0;;) {
    if (__atomic_load_n(&a->quit, __ATOMIC_ACQUIRE)) return 1;
    struct pollfd pf = { a->fd, POLLOUT, 0 };
    int r = poll(&pf, 1, 100);
    if (r > 0) return 0;
    if (r < 0 && errno != EINTR) return 0;   /* let the write say what is wrong */
    if (r == 0 && (stall_ms += 100) >= STALL_MS) return -ETIMEDOUT;
  }
}

/* after a PREPARE alsa-lib's iec958 plugin restarts its block counter: pack the rest again from 0 */
static void after_prepare(audio_t *a, int off, int left) {
  if (a->fmt != FMT_IEC958) return;
  a->iec_counter = 0;
  audio_iec958_pack(a->out.u32 + 2 * off, a->stage + 2 * off, left, &a->iec_counter, a->iec_status);
}

/* write frames of a->out; 0 ok (or quitting), else -errno: the device is unusable */
static int alsa_write(audio_t *a, int frames) {
  const size_t bpf = a->fmt == FMT_S16 ? 4 : 8;
  int off = 0, stall_ms = 0, spins = 0;
  while (off < frames) {
    if (__atomic_load_n(&a->quit, __ATOMIC_ACQUIRE)) return 0;
    struct snd_xferi x;
    memset(&x, 0, sizeof x);
    x.buf = (char *)&a->out + (size_t)off * bpf;
    x.frames = (snd_pcm_uframes_t)(frames - off);
    int e;
    if (ioctl(a->fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) == 0) {
      if (x.result > 0) { off += (int)x.result; stall_ms = 0; spins = 0; continue; }
      e = EAGAIN;
    } else e = errno;
    switch (e) {
      case EINTR: break;
      case EAGAIN: {
        struct pollfd pf = { a->fd, POLLOUT, 0 };
        int r = poll(&pf, 1, 100);
        if (r == 0) {
          stall_ms += 100;
          if (stall_ms >= STALL_MS) return -ETIMEDOUT;
        } else if (r > 0 && (pf.revents & (POLLERR | POLLHUP | POLLNVAL))) {
          if (++spins > 10) plat_sleep_us(10000);
          if (spins > 200) return -EIO;
        }
        break;
      }
      case EPIPE: {                    /* underrun: start again */
        __atomic_add_fetch(&a->underruns, 1, __ATOMIC_RELAXED);
        int64_t now = plat_now_us();
        if (now - a->xrun_t0 > XRUN_WIN_US) { a->xrun_t0 = now; a->xrun_n = 0; }
        if (++a->xrun_n >= XRUN_STORM) return -E_XRUN_STORM;
        if (ioctl(a->fd, SNDRV_PCM_IOCTL_PREPARE) < 0) return -errno;
        after_prepare(a, off, frames - off);
        break;
      }
      case ESTRPIPE: {                 /* suspended */
        int k = 0, r;
        while ((r = ioctl(a->fd, SNDRV_PCM_IOCTL_RESUME)) < 0 && errno == EAGAIN && k++ < 100) plat_sleep_us(10000);
        if (r < 0) {
          if (ioctl(a->fd, SNDRV_PCM_IOCTL_PREPARE) < 0) return -errno;
          after_prepare(a, off, frames - off);
        }
        break;
      }
      default:
        return -e;
    }
  }
  return 0;
}

/* the CPUs online; not sysconf(_SC_NPROCESSORS_ONLN): musl counts only the calling thread's affinity
 * mask, so a frame loop pinned to one core would see a single CPU */
static long cpus_online(void) {
  char b[64];
  int fd = open("/sys/devices/system/cpu/online", O_RDONLY | O_CLOEXEC);
  ssize_t r = fd >= 0 ? read(fd, b, sizeof b - 1) : -1;
  if (fd >= 0) close(fd);
  long n = 0;
  if (r > 0) {                                   /* "0-3", "0,2-3" */
    b[r] = 0;
    for (char *p = b; *p && *p != '\n';) {
      char *e;
      long lo = strtol(p, &e, 10), hi = lo;
      if (e == p) break;
      if (*e == '-') hi = strtol(e + 1, &e, 10);
      n = hi + 1 > n ? hi + 1 : n;               /* the highest CPU number + 1 */
      p = *e == ',' ? e + 1 : e;
    }
  }
  if (n <= 0) n = sysconf(_SC_NPROCESSORS_CONF);
  return n > 0 ? (n < CPU_SETSIZE ? n : CPU_SETSIZE) : 1;
}

static void thread_sched(audio_t *a) {
  struct sched_param sp;
  memset(&sp, 0, sizeof sp);
  if (a->rt_prio > 0) {
    sp.sched_priority = a->rt_prio;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
      sp.sched_priority = 0;
      pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
    }
  } else {
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);   /* never above the frame loop */
  }
  long ncpu = cpus_online();
  cpu_set_t set;
  CPU_ZERO(&set);
  if (a->cpu >= 0) CPU_SET(a->cpu, &set);
  else for (long i = 0; i < ncpu && i < CPU_SETSIZE; i++) CPU_SET((int)i, &set);   /* drop an inherited pin */
  sched_setaffinity(0, sizeof set, &set);
}

/* the frame loop's thread decides: one step below its SCHED_FIFO priority, on the core before its own */
static void pick_sched(audio_t *a) {
  int pol = SCHED_OTHER;
  struct sched_param sp;
  memset(&sp, 0, sizeof sp);
  a->rt_prio = 0;
  if (pthread_getschedparam(pthread_self(), &pol, &sp) == 0 && (pol == SCHED_FIFO || pol == SCHED_RR) && sp.sched_priority > 1)
    a->rt_prio = sp.sched_priority - 1;
  long ncpu = cpus_online();
  int pinned = -1;
  cpu_set_t cur;
  CPU_ZERO(&cur);
  if (sched_getaffinity(0, sizeof cur, &cur) == 0 && CPU_COUNT(&cur) == 1)
    for (int i = 0; i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &cur)) { pinned = i; break; }
  if (ncpu >= 2 && pinned >= 0) a->cpu = (int)((pinned + ncpu - 1) % ncpu);
  else if (ncpu >= 3) a->cpu = (int)(ncpu - 2);
  else a->cpu = -1;
}

static void *alsa_thread(void *p);

/* the thread starts with its policy and priority (not the frame loop's, inherited: a SCHED_FIFO frame
 * loop pinned to one core would keep it from running its own setup until the loop blocks) and is moved
 * to its core by the creator at once; thread_sched() in the thread covers what this couldn't do */
static int start_thread(audio_t *a) {
  pthread_attr_t at;
  struct sched_param sp;
  memset(&sp, 0, sizeof sp);
  sp.sched_priority = a->rt_prio > 0 ? a->rt_prio : 0;
  pthread_attr_init(&at);
  pthread_attr_setstacksize(&at, 256 * 1024);
  pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
  pthread_attr_setschedpolicy(&at, a->rt_prio > 0 ? SCHED_FIFO : SCHED_OTHER);
  pthread_attr_setschedparam(&at, &sp);
  int r = pthread_create(&a->th, &at, alsa_thread, a);
  if (r == EPERM || r == EINVAL) {               /* not allowed: as the caller, thread_sched() adjusts */
    pthread_attr_setinheritsched(&at, PTHREAD_INHERIT_SCHED);
    r = pthread_create(&a->th, &at, alsa_thread, a);
  }
  pthread_attr_destroy(&at);
  if (r) return -1;
  if (a->cpu >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(a->cpu, &set);
    pthread_setaffinity_np(a->th, sizeof set, &set);
  }
  return 0;
}

static void *alsa_thread(void *p) {
  audio_t *a = p;
  thread_sched(a);
  int64_t next_try = plat_now_us() + RETRY_US;
  int64_t idle_t0 = plat_now_us(), idle_done = 0;
  char label[48];
  plat_strlcpy(label, a->target, sizeof label);
  while (!__atomic_load_n(&a->quit, __ATOMIC_ACQUIRE)) {
    if (a->fd < 0) {
      int64_t now = plat_now_us();
      if (now >= next_try) {
        char why[128];
        if (alsa_try_open(a, why, sizeof why) == 0) continue;
        set_status(a, false, label, why);
        next_try = now + RETRY_US;
      }
      /* silent: the mix keeps time so tones and music don't pile up for the device's return */
      int64_t due = (now - idle_t0) * a->rate / 1000000 - idle_done;
      if (due > a->rate / 5) { idle_done += due - a->rate / 5; due = a->rate / 5; }
      while (due > 0) {
        int n = due > CHUNK_MAX ? CHUNK_MAX : (int)due;
        render_out(a, FMT_S16, n);
        due -= n;
        idle_done += n;
      }
      plat_sleep_us(20000);
      continue;
    }
    int chunk = a->period < BLOCK ? BLOCK : a->period > CHUNK_MAX ? CHUNK_MAX : a->period;
    int r = alsa_wait_room(a);
    if (r > 0) break;
    if (r == 0) {
      render_out(a, a->fmt, chunk);
      r = alsa_write(a, chunk);
    }
    if (r < 0) {
      char e[128], dev[48];
      plat_mutex_lock(&a->mu);
      plat_strlcpy(dev, a->device, sizeof dev);
      plat_mutex_unlock(&a->mu);
      snprintf(e, sizeof e, "%s: %s, retrying", dev,
               r == -ETIMEDOUT ? "the device stopped taking audio" :
               r == -E_XRUN_STORM ? "underruns without end" : strerror(-r));
      alsa_close(a);
      set_status(a, false, label, e);
      next_try = plat_now_us() + RETRY_US;
      idle_t0 = plat_now_us();
      idle_done = 0;
      continue;
    }
    __atomic_add_fetch(&a->frames_out, (uint64_t)chunk, __ATOMIC_RELEASE);
  }
  alsa_close(a);
  return NULL;
}

/* sfx_new/music_new may start threads (the songs decoder): Linux threads inherit the creator's policy
 * and CPU mask, so with a SCHED_FIFO frame loop pinned to its core calling audio_open, a decoder would
 * run at the frame loop's priority on the frame loop's core. The caller is made normal and unpinned
 * around those calls (once, at startup) and restored after. */
typedef struct { int pol; struct sched_param sp; cpu_set_t cpus; bool sched_ok, aff_ok; } sched_save_t;
static void caller_normal(sched_save_t *s) {
  memset(s, 0, sizeof *s);
  s->sched_ok = pthread_getschedparam(pthread_self(), &s->pol, &s->sp) == 0 && s->pol != SCHED_OTHER;
  if (s->sched_ok) {
    struct sched_param z;
    memset(&z, 0, sizeof z);
    s->sched_ok = pthread_setschedparam(pthread_self(), SCHED_OTHER, &z) == 0;
  }
  s->aff_ok = sched_getaffinity(0, sizeof s->cpus, &s->cpus) == 0;
  if (s->aff_ok) {
    cpu_set_t all;
    CPU_ZERO(&all);
    for (long i = 0, n = cpus_online(); i < n; i++) CPU_SET((int)i, &all);
    s->aff_ok = sched_setaffinity(0, sizeof all, &all) == 0;
  }
}
static void caller_restore(sched_save_t *s) {
  if (s->sched_ok) pthread_setschedparam(pthread_self(), s->pol, &s->sp);
  if (s->aff_ok) sched_setaffinity(0, sizeof s->cpus, &s->cpus);
}

#else
typedef int sched_save_t;
static void caller_normal(sched_save_t *s) { *s = 0; }
static void caller_restore(sched_save_t *s) { (void)s; }
#endif /* __linux__ */

/* ======================================================================= open / close */

static uint32_t clock_seed(void) {
  uint64_t x = (uint64_t)plat_now_us() * 0x9E3779B97F4A7C15ull ^ (uint64_t)plat_wall_ms() * 0xD1B54A32D192ED03ull;
  uint32_t s = (uint32_t)(x ^ (x >> 32));
  return s ? s : 0x5EED1234u;
}

static void say(char *err, int errlen, const char *msg) {
  if (err && errlen > 0) plat_strlcpy(err, msg ? msg : "", (size_t)errlen);
}

audio_t *audio_open(const audio_config_t *c, char *err, int errlen) {
  say(err, errlen, "");
  audio_t *a = calloc(1, sizeof *a);
  if (!a) { say(err, errlen, "out of memory"); return NULL; }
  plat_mutex_init(&a->mu);
  a->fd = -1;
  a->card = a->dev = a->subdev = -1;
  a->cpu = -1;
  a->rate = c && c->rate > 0 ? c->rate : 48000;
  const char *out = c && c->out && c->out[0] ? c->out : "auto";
  uint32_t seed = c && c->seed ? c->seed : clock_seed();
  a->video_hdmi = c ? c->video_hdmi : false;

  bool can_render = true;
  if (!strcmp(out, "off")) {
    a->kind = OUT_OFF;
    set_status(a, false, "off", "");
    can_render = false;
  } else if (!strcmp(out, "null")) {
    a->kind = OUT_NULL;
    set_status(a, true, "null", "");
  } else if (!strncmp(out, "wav:", 4) || !strncmp(out, "wav16:", 6)) {
    a->kind = OUT_WAV;
    a->wav_fmt = out[3] == ':' ? FMT_F32 : FMT_S16;
    const char *path = out + (out[3] == ':' ? 4 : 6);
    a->wav = fopen(path, "wb");
    if (!a->wav || wav_header(a) != 0) {
      char e[128];
      snprintf(e, sizeof e, "can't write %s: %s", path, strerror(errno));
      if (a->wav) { fclose(a->wav); a->wav = NULL; }
      set_status(a, false, "wav", e);
    } else {
      set_status(a, true, "wav", "");
    }
  } else if (!strcmp(out, "auto") || !strcmp(out, "jack") || !strcmp(out, "hdmi") || !strncmp(out, "hw:", 3) ||
             !strncmp(out, "card:", 5)) {
    a->kind = OUT_ALSA;
    plat_strlcpy(a->target, out, sizeof a->target);
#ifdef __linux__
    set_status(a, false, out, "");
#else
    set_status(a, false, out, "ALSA output needs Linux");
    can_render = false;
#endif
  } else {
    char e[128];
    snprintf(e, sizeof e, "unknown audio output '%s'", out);
    a->kind = OUT_OFF;
    set_status(a, false, "off", e);
    can_render = false;
  }

  if (can_render) {
    sched_save_t sv;
    caller_normal(&sv);              /* helper threads they start (the songs decoder) inherit no real-time */
    a->sfx = sfx_new(a->rate, seed);
    a->music = music_new(a->rate, seed, c && c->music_dir ? c->music_dir : "");
    caller_restore(&sv);
    if (!a->sfx || !a->music) set_status(a, a->ok, NULL, "out of memory (sound effects or music missing)");
  }
  /* the browser's defaults until the host says otherwise: Sound 80 %, music off at 60 % */
  audio_levels_t lv = { true, 80, false, 60, AUDIO_MUSIC_GENERATED, false };
  audio_set_levels(a, &lv);

#ifdef __linux__
  if (a->kind == OUT_ALSA) {
    char why[128];
    pick_sched(a);
    if (alsa_try_open(a, why, sizeof why) != 0) set_status(a, false, out, why);
    if (start_thread(a) == 0) a->thread_started = true;
    else {
      alsa_close(a);
      set_status(a, false, out, "can't start the audio thread");
    }
  }
#endif
  plat_mutex_lock(&a->mu);
  say(err, errlen, a->error);
  plat_mutex_unlock(&a->mu);
  return a;
}

void audio_close(audio_t *a) {
  if (!a) return;
#ifdef __linux__
  if (a->thread_started) {
    __atomic_store_n(&a->quit, 1, __ATOMIC_RELEASE);
    plat_thread_join(a->th);
    a->thread_started = false;
  }
  alsa_close(a);
#endif
  if (a->wav) {
    if (!a->wav_failed) wav_header(a);
    fclose(a->wav);
  }
  if (a->music) music_free(a->music);
  if (a->sfx) sfx_free(a->sfx);
  plat_mutex_destroy(&a->mu);
  free(a);
}
