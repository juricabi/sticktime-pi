/* audio.c: WAV output (header, length), offline rendering, mono, clipping, level mapping, IEC958
 * packing (against a literal transcription of alsa-lib's pcm_iec958.c), status strings, determinism.
 * Works with the real sfx/music or stand-ins: the signal checks use audio.c's test source, and the
 * sfx/music checks only rely on the contract (vol^2 law, silence when off, same seed = same output).
 * Temporary files go to $TEST_TMPDIR (default "build"). */
#include "t.h"
#include "audio.h"
#include "audio_internal.h"

#include <math.h>
#include <stdint.h>

/* audio.c's helpers for tests (not in a header: only tests use them) */
uint32_t audio_iec958_subframe(int16_t s, unsigned counter, int channel, const uint8_t status[24]);
void audio_iec958_pack(uint32_t *dst, const int16_t *src, int frames, unsigned *counter, const uint8_t status[24]);
void audio_iec958_status(uint8_t status[24], int rate);
void audio_levels_map(const audio_levels_t *lv, float *sfx_vol, bool *music_on, float *music_vol, int *source);
void audio_test_source(audio_t *a, void (*fn)(void *ud, float *L, float *R, int n), void *ud);
typedef struct { uint64_t frames; int period, buffer, format, card, dev; } audio_test_info_t;
void audio_test_info(audio_t *a, audio_test_info_t *ti);

static char tmpdir[400];

static const char *tmp(const char *name) {
  static char p[4][512];
  static int k;
  k = (k + 1) & 3;
  snprintf(p[k], sizeof p[k], "%s/%s", tmpdir, name);
  return p[k];
}

static unsigned char *slurp(const char *path, long *len) {
  FILE *f = fopen(path, "rb");
  if (!f) { *len = -1; return NULL; }
  fseek(f, 0, SEEK_END);
  *len = ftell(f);
  fseek(f, 0, SEEK_SET);
  unsigned char *b = malloc((size_t)*len + 1);
  if (b && fread(b, 1, (size_t)*len, f) != (size_t)*len) { free(b); b = NULL; }
  fclose(f);
  return b;
}
static unsigned u16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t u32(const unsigned char *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static float f32(const unsigned char *p) { uint32_t v = u32(p); float f; memcpy(&f, &v, 4); return f; }
static int s16(const unsigned char *p) { return (int16_t)u16(p); }

static audio_t *open_out(const char *out, int rate, uint32_t seed) {
  audio_config_t c;
  memset(&c, 0, sizeof c);
  c.out = out;
  c.rate = rate;
  c.seed = seed;
  c.music_dir = tmpdir;
  char err[200];
  return audio_open(&c, err, sizeof err);
}

static void levels(audio_t *a, bool sound, int vol, bool music, int mvol, bool mono) {
  audio_levels_t lv = { sound, vol, music, mvol, AUDIO_MUSIC_GENERATED, mono };
  audio_set_levels(a, &lv);
}

/* ---------------------------------------------------------------- WAV */

static void check_header(const unsigned char *b, long len, int fmt, int rate, long frames) {
  int bytes = fmt == 3 ? 4 : 2;
  CHECK_INT(len, 44 + frames * 2 * bytes);
  if (len < 44) return;
  CHECK(!memcmp(b, "RIFF", 4));
  CHECK_INT(u32(b + 4), 36 + frames * 2 * bytes);
  CHECK(!memcmp(b + 8, "WAVEfmt ", 8));
  CHECK_INT(u32(b + 16), 16);
  CHECK_INT(u16(b + 20), fmt);
  CHECK_INT(u16(b + 22), 2);
  CHECK_INT(u32(b + 24), rate);
  CHECK_INT(u32(b + 28), rate * 2 * bytes);
  CHECK_INT(u16(b + 32), 2 * bytes);
  CHECK_INT(u16(b + 34), 8 * bytes);
  CHECK(!memcmp(b + 36, "data", 4));
  CHECK_INT(u32(b + 40), frames * 2 * bytes);
}

static void test_wav_header_and_length(void) {
  const char *p = tmp("test_audio_f32.wav");
  char out[600];
  snprintf(out, sizeof out, "wav:%s", p);
  audio_t *a = open_out(out, 46080, 1);
  CHECK(a != NULL);
  if (!a) return;
  /* odd chunk sizes add up; 0 and negative counts render nothing */
  static const int chunks[] = { 100, 128, 129, 1, 0, -5, 1000, 768 };
  long total = 0;
  for (size_t i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
    audio_render_offline(a, chunks[i]);
    if (chunks[i] > 0) total += chunks[i];
  }
  /* the header is valid while the file is still open */
  long len;
  unsigned char *b = slurp(p, &len);
  CHECK(b != NULL);
  if (b) check_header(b, len, 3, 46080, total);
  free(b);
  audio_render_offline(a, 3);
  total += 3;
  audio_close(a);
  b = slurp(p, &len);
  CHECK(b != NULL);
  if (b) check_header(b, len, 3, 46080, total);
  free(b);
  remove(p);

  /* 16-bit */
  p = tmp("test_audio_s16.wav");
  snprintf(out, sizeof out, "wav16:%s", p);
  a = open_out(out, 48000, 1);
  audio_render_offline(a, 4800);
  audio_close(a);
  b = slurp(p, &len);
  CHECK(b != NULL);
  if (b) check_header(b, len, 1, 48000, 4800);
  free(b);
  remove(p);

  /* nothing rendered: a valid empty WAV */
  p = tmp("test_audio_empty.wav");
  snprintf(out, sizeof out, "wav:%s", p);
  a = open_out(out, 48000, 1);
  audio_close(a);
  b = slurp(p, &len);
  if (b) check_header(b, len, 3, 48000, 0);
  free(b);
  remove(p);
}

/* ---------------------------------------------------------------- test source: mono and clipping */

typedef struct { float l, r; int n; } src_t;
static void const_src(void *ud, float *L, float *R, int n) {
  src_t *s = ud;
  for (int i = 0; i < n; i++) { L[i] += s->l; R[i] += s->r; }
  s->n += n;
}

/* render n frames of a constant source with the given levels into a WAV; returns the file */
static unsigned char *render_const(const char *kind, float l, float r, bool mono, int n, long *len) {
  const char *p = tmp("test_audio_src.wav");
  char out[600];
  snprintf(out, sizeof out, "%s:%s", kind, p);
  audio_t *a = open_out(out, 48000, 3);
  levels(a, true, 80, false, 60, mono);
  src_t s = { l, r, 0 };
  audio_test_source(a, const_src, &s);
  audio_render_offline(a, n);
  CHECK_INT(s.n, n);
  audio_close(a);
  unsigned char *b = slurp(p, len);
  remove(p);
  return b;
}

static void test_mono_clip(void) {
  long len;
  /* stereo passes through, mono = (L + R) / 2 on both */
  unsigned char *b = render_const("wav", 0.5f, -0.1f, false, 300, &len);
  if (b && len >= 44 + 8 * 300) {
    CHECK_NEAR(f32(b + 44), 0.5, 1e-7);
    CHECK_NEAR(f32(b + 48), -0.1, 1e-7);
    CHECK_NEAR(f32(b + 44 + 8 * 299), 0.5, 1e-7);
  }
  free(b);
  b = render_const("wav", 0.5f, -0.1f, true, 300, &len);
  if (b && len >= 44 + 8 * 300) {
    CHECK_NEAR(f32(b + 44), 0.2, 1e-7);
    CHECK_NEAR(f32(b + 48), 0.2, 1e-7);
    CHECK_NEAR(f32(b + 44 + 8 * 299 + 4), 0.2, 1e-7);
  }
  free(b);
  /* hard clip at +-1 */
  b = render_const("wav", 2.0f, -3.0f, false, 200, &len);
  if (b && len >= 44 + 8 * 200) {
    CHECK(f32(b + 44) == 1.0f);
    CHECK(f32(b + 48) == -1.0f);
  }
  free(b);
  /* mono is summed before the clip: (1.5 + (-0.5)) / 2 = 0.5 */
  b = render_const("wav", 1.5f, -0.5f, true, 200, &len);
  if (b && len >= 44 + 8 * 200) CHECK_NEAR(f32(b + 44), 0.5, 1e-7);
  free(b);
  /* NaN becomes silence, not full scale */
  b = render_const("wav", NAN, 0.25f, false, 130, &len);
  if (b && len >= 44 + 8 * 130) {
    CHECK(f32(b + 44) == 0.0f);
    CHECK_NEAR(f32(b + 48), 0.25, 1e-7);
  }
  free(b);
  /* 16-bit: x * 32767 rounded, clipped */
  b = render_const("wav16", 0.25f, -2.0f, false, 200, &len);
  if (b && len >= 44 + 4 * 200) {
    CHECK_INT(s16(b + 44), 8192);              /* 8191.75 */
    CHECK_INT(s16(b + 46), -32767);
  }
  free(b);
  b = render_const("wav16", 1.0f, 0.5f, false, 10, &len);
  if (b && len >= 44 + 40) {
    CHECK_INT(s16(b + 44), 32767);
    CHECK_INT(s16(b + 46), 16384);             /* 16383.5, to even */
  }
  free(b);
}

/* ---------------------------------------------------------------- levels */

static void test_level_map(void) {
  float sv, mv;
  bool mon;
  int src;
  audio_levels_t lv = { true, 80, false, 60, AUDIO_MUSIC_GENERATED, false };
  audio_levels_map(&lv, &sv, &mon, &mv, &src);
  CHECK_NEAR(sv, 0.8, 1e-6);
  CHECK(!mon);
  CHECK_NEAR(mv, 0.6, 1e-6);
  CHECK_INT(src, AUDIO_MUSIC_GENERATED);
  lv.sound = false;                            /* sound off = volume 0 */
  audio_levels_map(&lv, &sv, &mon, &mv, &src);
  CHECK(sv == 0.0f);
  lv = (audio_levels_t){ true, 150, true, -20, AUDIO_MUSIC_SONGS, true };
  audio_levels_map(&lv, &sv, &mon, &mv, &src);
  CHECK(sv == 1.0f);
  CHECK(mon);
  CHECK(mv == 0.0f);
  CHECK_INT(src, AUDIO_MUSIC_SONGS);
  lv = (audio_levels_t){ true, -3, true, 100, 7, false };
  audio_levels_map(&lv, &sv, &mon, &mv, &src);
  CHECK(sv == 0.0f);
  CHECK(mv == 1.0f);
  CHECK_INT(src, AUDIO_MUSIC_GENERATED);
  lv = (audio_levels_t){ true, 55, true, 35, AUDIO_MUSIC_GENERATED, false };
  audio_levels_map(&lv, &sv, &mon, &mv, &src);
  CHECK_NEAR(sv, 0.55, 1e-6);
  CHECK_NEAR(mv, 0.35, 1e-6);
}

/* rms of the left channel of a float WAV between t0 and t1 seconds */
static double rms(const unsigned char *b, long len, int rate, double t0, double t1) {
  long i0 = (long)(t0 * rate), i1 = (long)(t1 * rate);
  double s = 0;
  long n = 0;
  for (long i = i0; i < i1 && 44 + 8 * i + 4 <= len; i++, n++) {
    double x = f32(b + 44 + 8 * i);
    s += x * x;
  }
  return n ? sqrt(s / (double)n) : 0;
}

/* a 1 kHz beep through the real path (sfx) at a sound level; returns the file */
static unsigned char *render_beep(bool sound, int vol, long *len) {
  const char *p = tmp("test_audio_beep.wav");
  char out[600];
  snprintf(out, sizeof out, "wav:%s", p);
  audio_t *a = open_out(out, 48000, 5);
  levels(a, sound, vol, false, 60, false);
  audio_render_offline(a, 12000);              /* 0.25 s: the level glide settles */
  audio_tone(a, 1000, 300, 0, 0, 0);
  audio_render_offline(a, 24000);
  audio_close(a);
  unsigned char *b = slurp(p, len);
  remove(p);
  return b;
}

static void test_level_law(void) {
  long l1, l2, l0;
  unsigned char *full = render_beep(true, 100, &l1), *half = render_beep(true, 50, &l2), *off = render_beep(false, 100, &l0);
  if (full && half && off) {
    double r1 = rms(full, l1, 48000, 0.30, 0.50), r2 = rms(half, l2, 48000, 0.30, 0.50);
    double r0 = rms(off, l0, 48000, 0.0, 0.75), q = rms(full, l1, 48000, 0.0, 0.24);
    CHECK(r1 > 0.01);                          /* the beep plays: plateau 0.045 x 0.848 at full level */
    CHECK_NEAR(r2 > 0 ? r1 / r2 : 0, 4.0, 0.12);   /* half the volume = a quarter of the gain */
    CHECK(r0 < 1e-5);                          /* sound off: the beep is dropped */
    CHECK(q < 1e-5);                           /* nothing before the beep */
    printf("beep rms full %.4f half %.4f ratio %.3f\n", r1, r2, r2 > 0 ? r1 / r2 : 0);
  } else CHECK(0);
  free(full);
  free(half);
  free(off);
}

static void test_music_levels(void) {
  /* music off: silence; music on: something plays and the title says so */
  const char *p = tmp("test_audio_music.wav");
  char out[600];
  snprintf(out, sizeof out, "wav:%s", p);
  audio_t *a = open_out(out, 48000, 1);
  levels(a, true, 80, false, 60, false);
  audio_render_offline(a, 48000);
  levels(a, true, 80, true, 60, false);
  for (int i = 0; i < 180; i++) audio_render_offline(a, 800);   /* 3 s, in frame-sized pieces */
  audio_status_t st;
  audio_status(a, &st);
  CHECK(strlen(st.now_playing) < sizeof st.now_playing);
  CHECK(st.now_playing[0] != 0);
  printf("now playing: \"%s\"\n", st.now_playing);
  audio_close(a);
  long len;
  unsigned char *b = slurp(p, &len);
  if (b) {
    CHECK(rms(b, len, 48000, 0.0, 0.99) < 1e-5);
    CHECK(rms(b, len, 48000, 1.0, 4.0) > 1e-4);
  } else CHECK(0);
  free(b);
  remove(p);
}

/* ---------------------------------------------------------------- determinism */

static unsigned char *render_script(uint32_t seed, long *len) {
  const char *p = tmp("test_audio_det.wav");
  char out[600];
  snprintf(out, sizeof out, "wav:%s", p);
  audio_t *a = open_out(out, 48000, seed);
  levels(a, true, 80, true, 60, false);
  double v[14] = { 0, 1, 3, 0.0315, 0, 0, 0, 0, 0, 0, 0, 26, 0, 0 };
  for (int k = 0; k < 150; k++) {
    if (k == 0) audio_tone(a, 1000, 120, 0, 0, 0);
    if (k == 30) v[2] = 4;
    if (k >= 60) v[3] = 0.0315 + (k - 60) * 0.01;
    if (k == 100) audio_tone(a, 208, 90, 0, 16, 0);
    if (k == 100) { v[12] = 1; v[13] = 6; }
    audio_game(a, v, 14, 1000.0 / 60);
    audio_render_offline(a, 800);
  }
  audio_close(a);
  unsigned char *b = slurp(p, len);
  remove(p);
  return b;
}

static void test_determinism(void) {
  long l1, l2;
  unsigned char *x = render_script(7, &l1), *y = render_script(7, &l2);
  CHECK(x && y && l1 == l2 && l1 == 44 + 150 * 800 * 8);
  if (x && y && l1 == l2) CHECK(memcmp(x, y, (size_t)l1) == 0);
  free(x);
  free(y);
}

/* ---------------------------------------------------------------- IEC958 */

/* alsa-lib src/pcm/pcm_iec958.c, transcribed literally (iec958_parity, iec958_subframe, the encode
 * loop for an interleaved S16 source: get32 = sample << 16, one channel after the other) */
typedef struct { unsigned int counter; unsigned char status[24]; unsigned char preamble[3]; } ref_iec_t;
enum { PREAMBLE_Z, PREAMBLE_X, PREAMBLE_Y };
static unsigned int ref_parity(unsigned int data) {
  unsigned int parity;
  int bit;
  data >>= 4;
  parity = 0;
  for (bit = 4; bit <= 30; bit++) {
    if (data & 1) parity++;
    data >>= 1;
  }
  return (parity & 1);
}
static uint32_t ref_subframe(ref_iec_t *iec, uint32_t data, int channel) {
  unsigned int byte = iec->counter >> 3;
  unsigned int mask = 1 << (iec->counter - (byte << 3));
  data >>= 4;
  data &= ~0xf;
  if (iec->status[byte] & mask) data |= 0x40000000;
  if (ref_parity(data)) data |= 0x80000000;
  if (channel) data |= iec->preamble[PREAMBLE_Y];
  else if (!iec->counter) data |= iec->preamble[PREAMBLE_Z];
  else data |= iec->preamble[PREAMBLE_X];
  return data;
}
static void ref_encode(ref_iec_t *iec, uint32_t *dst, const int16_t *src, unsigned channels, int frames) {
  int counter = iec->counter;
  for (unsigned channel = 0; channel < channels; ++channel) {
    iec->counter = counter;
    for (int f = 0; f < frames; f++) {
      int32_t sample = (int32_t)((uint32_t)(uint16_t)src[f * channels + channel] << 16);
      dst[f * channels + channel] = ref_subframe(iec, (uint32_t)sample, (int)channel);
      iec->counter += 1;
      iec->counter %= 192;
    }
  }
}
/* the status bytes of vc4-hdmi.conf (AES0-3 = 0x04 0x82 0x00 0x01) after the plugin's hw_params at
 * 48 kHz with a 16-bit format (snd_pcm_iec958_hw_params) */
static void ref_status(unsigned char st[24], unsigned rate) {
  memset(st, 0, 24);
  st[0] = 0x04; st[1] = 0x82; st[2] = 0x00; st[3] = 0x01;
  if ((st[0] & 0x01) == 0) {                   /* IEC958_AES0_PROFESSIONAL */
    if ((st[3] & 0x0f) == 0x01) {              /* IEC958_AES3_CON_FS == NOTID */
      unsigned char fs = rate == 48000 ? 0x02 : rate == 44100 ? 0x00 : 0x01;
      st[3] &= ~0x0f;
      st[3] |= fs;
    }
    if ((st[4] & 0x0e) == 0x00) {              /* IEC958_AES4_CON_WORDLEN == NOTID; width 16 -> WORDLEN_20_16 */
      st[4] &= ~(0x01 | 0x0e);
      st[4] |= 0x02;
    }
  }
}

static uint32_t lcg = 12345;
static int16_t rnd16(void) { lcg = lcg * 1103515245u + 12345u; return (int16_t)(lcg >> 8); }

static void test_iec958(void) {
  uint8_t st[24];
  unsigned char rs[24];
  audio_iec958_status(st, 48000);
  ref_status(rs, 48000);
  CHECK(memcmp(st, rs, 24) == 0);
  CHECK_INT(st[0], 0x04); CHECK_INT(st[1], 0x82); CHECK_INT(st[2], 0x00); CHECK_INT(st[3], 0x02); CHECK_INT(st[4], 0x02);
  audio_iec958_status(st, 44100);
  CHECK_INT(st[3], 0x00);
  audio_iec958_status(st, 48000);

  /* hand-worked vectors (status 04 82 00 02 02: C bits at frames 2, 9, 15, 25, 33) */
  CHECK_INT(audio_iec958_subframe(0, 0, 0, st), 0x00000008u);          /* block start Z */
  CHECK_INT(audio_iec958_subframe(0, 0, 1, st), 0x00000004u);          /* right Y */
  CHECK_INT(audio_iec958_subframe(0, 1, 0, st), 0x00000002u);          /* left X */
  CHECK_INT(audio_iec958_subframe(0, 2, 0, st), 0xC0000002u);          /* C bit + parity */
  CHECK_INT(audio_iec958_subframe(0, 2, 1, st), 0xC0000004u);
  CHECK_INT(audio_iec958_subframe(32767, 1, 0, st), 0x87FFF002u);      /* 15 ones: parity set */
  CHECK_INT(audio_iec958_subframe(-1, 3, 0, st), 0x0FFFF002u);         /* 16 ones: even */
  CHECK_INT(audio_iec958_subframe(-32768, 8, 0, st), 0x88000002u);
  CHECK_INT(audio_iec958_subframe(0, 9, 0, st), 0xC0000002u);          /* AES1 bit 1 (PCM coder) */
  CHECK_INT(audio_iec958_subframe(0, 15, 1, st), 0xC0000004u);         /* AES1 bit 7 (original) */
  CHECK_INT(audio_iec958_subframe(0, 24, 0, st), 0x00000002u);
  CHECK_INT(audio_iec958_subframe(0, 25, 0, st), 0xC0000002u);         /* AES3 = 48 kHz */
  CHECK_INT(audio_iec958_subframe(0, 33, 0, st), 0xC0000002u);         /* AES4 = 16-bit */
  CHECK_INT(audio_iec958_subframe(0, 191, 0, st), 0x00000002u);
  CHECK_INT(audio_iec958_subframe(0x1234, 2, 0, st), 0x41234002u);     /* 5 ones + C = 6: even */

  /* against the transcription: 3 blocks + in odd chunks, counter kept across calls */
  enum { N = 3 * 192 + 77 };
  static int16_t src[N * 2];
  static uint32_t got[N * 2], want[N * 2];
  for (int i = 0; i < N * 2; i++) src[i] = rnd16();
  src[0] = 32767; src[1] = -32768; src[2] = 0; src[3] = -1;
  ref_iec_t ref;
  memset(&ref, 0, sizeof ref);
  memcpy(ref.status, rs, 24);
  ref.preamble[PREAMBLE_Z] = 0x08; ref.preamble[PREAMBLE_X] = 0x02; ref.preamble[PREAMBLE_Y] = 0x04;
  static const int chunks[] = { 100, 1, 91, 192, 200, 69 };   /* = N */
  unsigned counter = 0;
  int off = 0;
  for (size_t i = 0; i < sizeof chunks / sizeof chunks[0]; i++) {
    ref_encode(&ref, want + 2 * off, src + 2 * off, 2, chunks[i]);
    audio_iec958_pack(got + 2 * off, src + 2 * off, chunks[i], &counter, st);
    CHECK_INT(counter, ref.counter);
    off += chunks[i];
  }
  CHECK_INT(off, N);
  int bad = 0;
  for (int i = 0; i < N * 2; i++) if (got[i] != want[i]) { if (!bad) fprintf(stderr, "iec958 word %d: %08x != %08x\n", i, got[i], want[i]); bad++; }
  CHECK_INT(bad, 0);
  /* every Z sits at a block start, and bits 4..31 always have even parity */
  int z = 0, odd = 0;
  for (int i = 0; i < N; i++) {
    if ((got[2 * i] & 0xf) == 0x08) { z++; CHECK_INT(i % 192, 0); }
    if (__builtin_parity(got[2 * i] & 0xfffffff0u) || __builtin_parity(got[2 * i + 1] & 0xfffffff0u)) odd++;
  }
  CHECK_INT(z, 4);
  CHECK_INT(odd, 0);
}

/* ---------------------------------------------------------------- status */

static void test_status(void) {
  audio_status_t st;
  char err[200];
  audio_config_t c;
  memset(&c, 0, sizeof c);
  c.rate = 48000;

  audio_status(NULL, &st);
  CHECK_STR(st.device, "off");
  CHECK(!st.ok);

  c.out = "off";
  audio_t *a = audio_open(&c, err, sizeof err);
  CHECK(a != NULL);
  CHECK_STR(err, "");
  audio_status(a, &st);
  CHECK_STR(st.device, "off");
  CHECK(!st.ok);
  CHECK_STR(st.error, "");
  CHECK_STR(st.now_playing, "");
  CHECK_INT(st.underruns, 0);
  /* everything is harmless on an "off" output */
  double v[14] = { 0 };
  audio_game(a, v, 14, 16.7);
  audio_tone(a, 1000, 100, 0, 0, 0);
  audio_haptic(a, 30);
  audio_game_reset(a);
  audio_music_next(a);
  audio_songs_changed(a);
  audio_render_offline(a, 100);
  levels(a, true, 50, true, 50, true);
  audio_close(a);

  c.out = "null";
  a = audio_open(&c, err, sizeof err);
  CHECK_STR(err, "");
  audio_status(a, &st);
  CHECK_STR(st.device, "null");
  CHECK(st.ok);
  audio_render_offline(a, 1000);
  audio_test_info_t ti;
  audio_test_info(a, &ti);
  CHECK_INT(ti.frames, 1000);
  audio_close(a);

  char out[600];
  snprintf(out, sizeof out, "wav:%s", tmp("test_audio_st.wav"));
  c.out = out;
  a = audio_open(&c, err, sizeof err);
  audio_status(a, &st);
  CHECK_STR(st.device, "wav");
  CHECK(st.ok);
  CHECK_STR(st.error, "");
  audio_close(a);
  remove(tmp("test_audio_st.wav"));

  snprintf(out, sizeof out, "wav:%s", tmp("no/such/dir/x.wav"));
  a = audio_open(&c, err, sizeof err);
  CHECK(a != NULL);
  audio_status(a, &st);
  CHECK_STR(st.device, "wav");
  CHECK(!st.ok);
  CHECK(!strncmp(st.error, "can't write ", 12));
  CHECK_STR(err, st.error);
  audio_render_offline(a, 500);                /* silent, no crash */
  audio_close(a);

  c.out = "bogus";
  a = audio_open(&c, err, sizeof err);
  audio_status(a, &st);
  CHECK_STR(st.device, "off");
  CHECK_STR(st.error, "unknown audio output 'bogus'");
  CHECK_STR(err, st.error);
  audio_close(a);

#ifdef __linux__
  c.out = "card:NoSuchCard";
  a = audio_open(&c, err, sizeof err);
  audio_status(a, &st);
  CHECK(!st.ok);
  CHECK_STR(st.device, "card:NoSuchCard");
  CHECK_STR(st.error, "card: no 'NoSuchCard' sound card");
  audio_close(a);
#else
  static const char *outs[] = { "jack", "hdmi", "auto" };
  for (int i = 0; i < 3; i++) {
    c.out = outs[i];
    a = audio_open(&c, err, sizeof err);
    audio_status(a, &st);
    CHECK(!st.ok);
    CHECK_STR(st.device, outs[i]);
    CHECK_STR(st.error, "ALSA output needs Linux");
    audio_close(a);
  }
#endif
  c.out = NULL;                                /* default: auto */
  a = audio_open(&c, err, sizeof err);
  audio_status(a, &st);
  CHECK_STR(st.device, "auto");
  audio_close(a);
}

int main(void) {
  const char *t = getenv("TEST_TMPDIR");
  snprintf(tmpdir, sizeof tmpdir, "%s", t && *t ? t : "build");
  test_wav_header_and_length();
  test_mono_clip();
  test_level_map();
  test_level_law();
  test_music_levels();
  test_determinism();
  test_iec958();
  test_status();
  T_DONE();
}
