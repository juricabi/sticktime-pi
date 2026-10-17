/* test_sfx: the motor synth and the beeps (sfx.c) rendered offline, checked against the spec
 * (notes audio-motor.md §10, §13) and, when available, against reference renders of the browser's own
 * sound code in headless Chromium (the harness: ref_render.py driving app.js's sound section).
 *
 *   test_sfx [refdir [outdir]]
 *   env STP_SFX_REF=<dir>   reference WAVs + scenario JSONs: spool_q1, spool_q1_raw, spool_q4,
 *                           manoeuvres (make_scenarios.py), beeps, crash, classes (the sc_* below)
 *   env STP_SFX_OUT=<dir>   also write the C renders there as float32 WAVs (<name>_c.wav)
 *   env STP_SFX_VERBOSE=1   print the per-window level tables
 *
 * Without references only the self-contained checks run (levels and peaks from the spec, beep timing
 * and envelope, 50 Hz vs 60 Hz frames, production vs reference mode, volume, determinism, reset).
 * The scenarios are generated here like make_scenarios.py does; with references, the JSON next to
 * each WAV is compared with them (within 1e-15: libm sin() may differ in the last bit) and its
 * values are what gets rendered, so both sides see the very same inputs.
 *
 * Reference mode (SFX_DBG_REF | SFX_DBG_LITERAL: per-sample filter coefficients, app.js's per-call
 * EMA constants) is expected to match Chromium sample by sample; production mode (coefficients every
 * 16 samples, dt-based EMAs) within the spec's 1 dB. */
#include "t.h"
#include "audio_internal.h"

#include <math.h>
#include <stdint.h>

/* the scenarios must come out like make_scenarios.py's (no fused multiply-adds) */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

/* sfx.c's test hooks (not in audio_internal.h) */
#define SFX_DBG_REF     1
#define SFX_DBG_LITERAL 2
#define SFX_DBG_RAW     4
#define REFMODE (SFX_DBG_REF | SFX_DBG_LITERAL)
void sfx_debug_config(sfx_t *s, int flags);
unsigned sfx_debug_dropped(sfx_t *s);

static int verbose = 0;
static const char *refdir = NULL, *outdir = NULL;

/* A beep's |x| crosses half the square's plateau (0.045*0.848/2 = 0.01908 of gain 1) when the
 * envelope 0.0001 -> 0.045 (6 ms exp) passes 0.01908 (the Gibbs peaks reach 1.0): t0 + 5.16 ms; it
 * falls below it 1.69 ms into the 12 ms exp release (t1 - 10.3 ms). Square edges every half period. */
#define BEEP_ON  0.00516
#define BEEP_OFF 0.01031

/* ------------------------------------------------------------------------------------ scenarios */
#define NV 14
typedef struct { double t; int f, d, p, flags; } tone_ev_t;
typedef struct {
  const char *name;
  int fs, frame, nframes;
  uint32_t seed;
  float vol;
  double (*seq)[NV];
  tone_ev_t tones[64];
  int ntones;
  int nvol, vol_k[8];                /* volume changes: vol_at[i] from frame vol_k[i] on */
  float vol_at[8];
} scen_t;

static const double I0[5] = {0.0315, 0.026, 0.0221, 0.0059, 0.0198};
static const double TWR[5] = {7.67, 6.88, 5.35, 2.58, 3.54};
static const double PF[5] = {26, 24, 35, 45, 40};

static void a_def(double *x, int quad) {
  double d[NV] = {0, quad, 4, 0.03, 0, 0, 0, 0, 0, 0, 0, PF[quad - 1], 0, 0};
  memcpy(x, d, sizeof d);
}

static void scen_alloc(scen_t *s, const char *name, int fs, int frame, double seconds, uint32_t seed) {
  memset(s, 0, sizeof *s);
  s->name = name; s->fs = fs; s->frame = frame; s->seed = seed; s->vol = 0.8f;
  s->nframes = (int)(seconds * fs / frame);              /* python int(): truncation */
  s->seq = calloc((size_t)s->nframes, sizeof *s->seq);
}

static void tone_add(scen_t *s, double t, int f, int d, int p, int flags) {
  tone_ev_t e = {t, f, d, p, flags};
  if (s->ntones < 64) s->tones[s->ntones++] = e;
}

/* make_scenarios.py spool(quad), at any fs/frame (the harness: 46080/768 = 60 Hz): COUNT with the
 * countdown beeps, idle, ramp to hover, to full, chop, a prop strike with its PLAY_NOW beep, PAUSED */
static void sc_spool(scen_t *s, int quad, int fs, int frame) {
  scen_alloc(s, quad == 1 ? "spool_q1" : "spool_q4", fs, frame, 8, 1);
  double hover = 1 / TWR[quad - 1], i0 = I0[quad - 1];
  for (int k = 0; k < s->nframes; k++) {
    double t = (double)k * frame / fs;
    double st = 4, frac = i0, crashes = 0, hv = 0;
    if (t < 0.5) st = 3;
    else if (t < 2) {}
    else if (t < 3) frac = i0 + (hover - i0) * (t - 2);
    else if (t < 4) frac = hover + (1 - hover) * (t - 3);
    else if (t < 5) frac = 1.0;
    else if (t < 5.1) frac = 1.0 + (i0 - 1.0) * (t - 5) / 0.1;
    if (t >= 6.5) { crashes = 1; hv = 6.0; }
    if (t >= 7.2) st = 7;
    a_def(s->seq[k], quad);
    s->seq[k][2] = st; s->seq[k][3] = frac; s->seq[k][12] = crashes; s->seq[k][13] = hv;
  }
  tone_add(s, 0.0, 1000, 120, 0, 0);
  tone_add(s, 0.25, 2000, 400, 0, 0);
  tone_add(s, 6.5, (int)(160 + 6 * 8), 90, 0, 16);
}

/* make_scenarios.py manoeuvres(): class 1 at 50 %: roll sweep, edgewise speed ramp 0 -> 40 m/s,
 * then sinking 3.5 m/s with prop wash 1 and 0.3 deg of shake (dirty air, chatter); speed = 0 cuts
 * the speed ramp (for the wind check) */
static void sc_manoeuvres_v(scen_t *s, int speed) {
  scen_alloc(s, "manoeuvres", 46080, 768, 9, 2);
  for (int k = 0; k < s->nframes; k++) {
    double t = (double)k * 768 / 46080;
    double *x = s->seq[k];
    a_def(x, 1);
    x[3] = 0.5;
    if (t < 3) x[7] = sin(2 * M_PI * 0.5 * t);
    else if (t < 6) x[5] = speed ? 40 * (t - 3) / 3 : 0;
    else { x[4] = -3.5; x[6] = 1.0; x[10] = 0.3; }
  }
}
static void sc_manoeuvres(scen_t *s) { sc_manoeuvres_v(s, 1); }

/* The extra scenarios (sfx_scenarios.py writes the same as JSON for ref_render.py):
 * beeps: menu state (motors silent): the battery double beep (pause), the race finish pair cut by a
 * crash beep (PLAY_NOW), gates back to back, a beep shorter than 20 ms, a frequency below 50 Hz, a
 * PLAY_NOW in the same frame as a queued beep */
static void sc_beeps(scen_t *s) {
  scen_alloc(s, "beeps", 46080, 768, 4, 3);
  for (int k = 0; k < s->nframes; k++) { a_def(s->seq[k], 1); s->seq[k][2] = 1; }
  tone_add(s, 0.1, 660, 150, 100, 0);
  tone_add(s, 0.1, 660, 150, 0, 0);
  tone_add(s, 0.8, 1800, 120, 0, 0);
  tone_add(s, 0.8, 2400, 300, 0, 0);
  tone_add(s, 0.95, 260, 400, 0, 16);
  tone_add(s, 1.6, 1300, 60, 0, 0);
  tone_add(s, 1.6, 1360, 60, 0, 0);
  tone_add(s, 1.6, 1420, 60, 0, 0);
  tone_add(s, 2.0, 1400, 10, 0, 0);
  tone_add(s, 2.3, 30, 200, 0, 0);
  tone_add(s, 2.7, 2200, 160, 0, 0);
  tone_add(s, 2.7, 1000, 120, 0, 16);
  tone_add(s, 3.2, 900, 300, 0, 0);
}

/* crash: freestyle hovering; a prop strike at 1 s (crashes 0 -> 1, hv 3: thud 0.495, rpm x 0.725),
 * a real crash at 2.5 s (4 -> 5 with crashes 1 -> 2: the stale hv), respawn READY at 3.5 s, FLY at
 * 4 s, a battery-EMPTY crash at 5.5 s (4 -> 5, no increment: thud 0.9, rpm x 0.3) */
static void sc_crash(scen_t *s) {
  scen_alloc(s, "crash", 46080, 768, 7, 4);
  for (int k = 0; k < s->nframes; k++) {
    double t = (double)k * 768 / 46080;
    double *x = s->seq[k];
    a_def(x, 2);
    x[3] = 1 / TWR[1];
    x[12] = t >= 1.0 ? 1 : 0; x[13] = t >= 1.0 ? 3 : 0;
    if (t >= 2.5) x[12] = 2;
    if (t >= 2.5 && t < 3.5) x[2] = 5;
    if (t >= 3.5 && t < 4.0) { x[2] = 6; x[3] = 0; }
    if (t >= 5.5) x[2] = 5;
  }
  tone_add(s, 1.0, (int)(160 + 3 * 8), 90, 0, 16);
  tone_add(s, 2.5, 260, 400, 0, 16);
  tone_add(s, 5.5, 260, 400, 0, 16);
}

/* classes: each quad class 1..5 for 1.5 s (hover, then full for the last 0.5 s), pitch/yaw stick
 * wiggles, edgewise speed rising: the wave switch (W0 <-> W1) and the whoops' high whine */
static void sc_classes(scen_t *s) {
  scen_alloc(s, "classes", 46080, 768, 7.5, 5);
  for (int k = 0; k < s->nframes; k++) {
    double t = (double)k * 768 / 46080;
    int q = 1 + (int)(t / 1.5);
    if (q > 5) q = 5;
    double u = t - (q - 1) * 1.5;
    double *x = s->seq[k];
    a_def(x, q);
    x[3] = u < 1.0 ? 1 / TWR[q - 1] : 1.0;
    x[8] = 0.5 * sin(2 * M_PI * 1.3 * t);
    x[9] = 0.3 * sin(2 * M_PI * 0.7 * t);
    x[5] = 8 * u;
  }
}

/* ------------------------------------------------------------------------------- render, files */
typedef struct { int fs; long n; float *L, *R; } wav_t;

static void wav_free(wav_t *w) { free(w->L); free(w->R); w->L = w->R = NULL; w->n = 0; }

static wav_t render(const scen_t *sc, int flags) {
  wav_t w = {sc->fs, (long)sc->nframes * sc->frame, NULL, NULL};
  w.L = calloc((size_t)w.n, sizeof(float));
  w.R = calloc((size_t)w.n, sizeof(float));
  sfx_t *s = sfx_new(sc->fs, sc->seed);
  if (!s || !w.L || !w.R) { fprintf(stderr, "out of memory\n"); exit(2); }
  sfx_debug_config(s, flags);
  sfx_set_volume(s, sc->vol);
  const double dt = 1000.0 * sc->frame / sc->fs;
  for (int k = 0; k < sc->nframes; k++) {
    for (int i = 0; i < sc->nvol; i++) if (sc->vol_k[i] == k) sfx_set_volume(s, sc->vol_at[i]);
    for (int i = 0; i < sc->ntones; i++) {           /* the harness: frame = round(t * fs / frame) */
      const tone_ev_t *e = &sc->tones[i];
      if ((int)lround(e->t * sc->fs / sc->frame) == k) sfx_tone(s, e->f, e->d, e->p, e->flags, 0);
    }
    sfx_frame(s, sc->seq[k], NV, dt);
    /* reference mode: the frame in one call (targets at its first sample, as the harness); else in
     * uneven chunks (the device renders 128-frame blocks; any split must work) */
    long off = (long)k * sc->frame;
    int left = sc->frame, c = 0;
    while (left > 0) {
      static const int chunk[] = {128, 128, 300, 51, 128, 1, 200, 256};
      int m = (flags & SFX_DBG_REF) ? left : chunk[c++ & 7];
      if (m > left) m = left;
      sfx_render(s, w.L + off, w.R + off, m);
      off += m; left -= m;
    }
  }
  CHECK_INT(sfx_debug_dropped(s), 0);
  sfx_free(s);
  return w;
}

static int wav_read(const char *path, wav_t *w) {
  memset(w, 0, sizeof *w);
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  unsigned char h[12];
  if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { fclose(f); return -1; }
  int fmt = 0, ch = 0, bits = 0;
  for (;;) {
    unsigned char c[8];
    if (fread(c, 1, 8, f) != 8) { fclose(f); return -1; }
    uint32_t len = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
    if (!memcmp(c, "fmt ", 4)) {
      unsigned char b[16];
      if (len < 16 || fread(b, 1, 16, f) != 16) { fclose(f); return -1; }
      fmt = b[0] | b[1] << 8; ch = b[2] | b[3] << 8;
      w->fs = (int)(b[4] | b[5] << 8 | b[6] << 16 | (uint32_t)b[7] << 24);
      bits = b[14] | b[15] << 8;
      if (len > 16) fseek(f, (long)(len - 16), SEEK_CUR);
    } else if (!memcmp(c, "data", 4)) {
      if (fmt != 3 || ch != 2 || bits != 32) { fclose(f); return -1; }
      w->n = (long)(len / 8);
      float *buf = malloc((size_t)len);
      w->L = malloc(sizeof(float) * (size_t)w->n);
      w->R = malloc(sizeof(float) * (size_t)w->n);
      if (!buf || !w->L || !w->R || fread(buf, 8, (size_t)w->n, f) != (size_t)w->n) {
        free(buf); wav_free(w); fclose(f); return -1;
      }
      for (long i = 0; i < w->n; i++) { w->L[i] = buf[2 * i]; w->R[i] = buf[2 * i + 1]; }
      free(buf);
      fclose(f);
      return 0;
    } else fseek(f, (long)len, SEEK_CUR);
  }
}

static void wav_write(const char *name, const wav_t *w) {
  if (!outdir) return;
  char path[1024];
  snprintf(path, sizeof path, "%s/%s_c.wav", outdir, name);
  FILE *f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
  uint32_t nb = (uint32_t)(w->n * 8), fs = (uint32_t)w->fs;
  uint32_t hdr[11] = {0x46464952u, 36 + nb, 0x45564157u, 0x20746d66u, 16, 3u | 2u << 16, fs, fs * 8,
                      8u | 32u << 16, 0x61746164u, nb};
  fwrite(hdr, 4, 11, f);
  for (long i = 0; i < w->n; i++) { float p[2] = {w->L[i], w->R[i]}; fwrite(p, 4, 2, f); }
  fclose(f);
}

/* The scenario JSON next to a reference: "seq" must match what we generated (within 1e-15); its
 * values replace ours. Returns the number of mismatches, or -1 without a JSON file. */
static int json_seq_use(scen_t *sc, const char *name) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s.json", refdir, name);
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *b = malloc((size_t)n + 1);
  if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return -1; }
  b[n] = 0;
  fclose(f);
  char *p = strstr(b, "\"seq\"");
  int bad = 0, k = 0, exact = 0;
  if (!p || !(p = strchr(p, '['))) { free(b); return -1; }
  p++;
  while (*p) {
    while (*p == ' ' || *p == ',') p++;
    if (*p != '[') break;
    p++;
    for (int i = 0; i < NV; i++) {
      char *e;
      double v = strtod(p, &e);
      if (e == p) { bad++; break; }
      if (k < sc->nframes) {
        double g = sc->seq[k][i];
        if (v != g) {
          exact++;
          if (fabs(v - g) > 1e-15 * fmax(1, fabs(v))) {
            if (bad < 3) fprintf(stderr, "%s: seq[%d][%d] = %.17g, json %.17g\n", name, k, i, g, v);
            bad++;
          }
        }
        sc->seq[k][i] = v;
      }
      p = e;
      while (*p == ' ' || *p == ',') p++;
    }
    while (*p && *p != ']') p++;
    if (*p) p++;
    k++;
  }
  free(b);
  if (k != sc->nframes) { fprintf(stderr, "%s: json has %d frames, generated %d\n", name, k, sc->nframes); bad++; }
  if (exact) printf("  (%d scenario values differ from the JSON in the last bit: libm)\n", exact);
  return bad;
}

/* ------------------------------------------------------------------------------------- metrics */
static double db10(double p) { return 10 * log10(p > 1e-30 ? p : 1e-30); }

static double pow_ch(const float *x, long i0, long i1) {
  double s = 0;
  for (long i = i0; i < i1; i++) s += (double)x[i] * x[i];
  return i1 > i0 ? s / (double)(i1 - i0) : 0;
}
static long clampn(const wav_t *w, double t) {
  long i = (long)(t * w->fs);
  return i < 0 ? 0 : i > w->n ? w->n : i;
}
static double rms_db(const wav_t *w, double t0, double t1) {          /* both channels */
  long i0 = clampn(w, t0), i1 = clampn(w, t1);
  return db10(0.5 * (pow_ch(w->L, i0, i1) + pow_ch(w->R, i0, i1)));
}
static double peak_db(const wav_t *w, double t0, double t1) {
  long i0 = clampn(w, t0), i1 = clampn(w, t1);
  double m = 0;
  for (long i = i0; i < i1; i++) { m = fmax(m, fabs(w->L[i])); m = fmax(m, fabs(w->R[i])); }
  return 20 * log10(m > 1e-15 ? m : 1e-15);
}
/* clicks: the largest sample step relative to the rms step within +-10 ms (both channels), among
 * steps above 1e-3. A tone scores ~1.4, noise ~4-5; a discontinuity much more (at most ~43). */
static double click_score(const wav_t *w, double t0, double t1) {
  long i0 = clampn(w, t0), i1 = clampn(w, t1), h = w->fs / 100;
  if (i0 < h + 1) i0 = h + 1;
  if (i1 > w->n - h) i1 = w->n - h;
  double m = 0;
  for (int ch = 0; ch < 2; ch++) {
    const float *x = ch ? w->R : w->L;
    double e = 0;
    for (long j = i0 - h; j < i0 + h; j++) e += ((double)x[j] - x[j - 1]) * ((double)x[j] - x[j - 1]);
    for (long i = i0; i < i1; i++) {
      double rms = sqrt(e / (2 * h)), st = fabs((double)x[i] - x[i - 1]);
      if (st > 1e-3 && st / rms > m) m = st / rms;     /* (steps below -60 dBFS are inaudible) */
      double a = (double)x[i + h] - x[i + h - 1], b = (double)x[i - h] - x[i - h - 1];
      e += a * a - b * b;
      if (e < 0) e = 0;
    }
  }
  return m;
}

typedef struct { double max, mean, p95; int n; } stat_t;
static int cmp_d(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}
static void stat_end(stat_t *st, double *d) {
  if (!st->n) return;
  st->mean /= st->n;
  qsort(d, (size_t)st->n, sizeof(double), cmp_d);
  st->p95 = d[(int)(0.95 * (st->n - 1))];
}
/* per-window level difference (dB) where the reference is above floor_db; per channel */
static stat_t env_diff(const wav_t *c, const wav_t *r, double win_s, double t0, double t1, double floor_db) {
  stat_t st = {0, 0, 0, 0};
  long win = (long)(win_s * r->fs), i0 = clampn(r, t0), i1 = clampn(r, t1);
  if (i1 > c->n) i1 = c->n;
  long nw = (i1 - i0) / win;
  double *d = malloc(sizeof(double) * (size_t)(2 * nw + 1));
  for (long k = 0; k < nw; k++) {
    long a = i0 + k * win, b = a + win;
    for (int ch = 0; ch < 2; ch++) {
      double pr = db10(pow_ch(ch ? r->R : r->L, a, b)), pc = db10(pow_ch(ch ? c->R : c->L, a, b));
      if (pr < floor_db) continue;
      double x = fabs(pc - pr);
      d[st.n++] = x;
      st.mean += x;
      if (x > st.max) st.max = x;
    }
  }
  stat_end(&st, d);
  free(d);
  return st;
}

/* residual energy of c - r relative to r over [t0, t1) (dB) */
static double null_db(const wav_t *c, const wav_t *r, double t0, double t1) {
  long i0 = clampn(r, t0), i1 = clampn(r, t1);
  if (i1 > c->n) i1 = c->n;
  double e = 0, s = 0;
  for (long i = i0; i < i1; i++) {
    double dl = (double)c->L[i] - r->L[i], dr = (double)c->R[i] - r->R[i];
    e += dl * dl + dr * dr;
    s += (double)r->L[i] * r->L[i] + (double)r->R[i] * r->R[i];
  }
  return db10(e / (s > 0 ? s : 1e-30));
}

static void fft(double *re, double *im, int n) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
  }
  for (int len = 2; len <= n; len <<= 1) {
    for (int i = 0; i < n; i += len) {
      for (int k = 0; k < len / 2; k++) {
        double ang = -2 * M_PI * k / len, wr = cos(ang), wi = sin(ang);
        double xr = re[i + k + len / 2], xi = im[i + k + len / 2];
        double vr = xr * wr - xi * wi, vi = xr * wi + xi * wr;
        re[i + k + len / 2] = re[i + k] - vr; im[i + k + len / 2] = im[i + k] - vi;
        re[i + k] += vr; im[i + k] += vi;
      }
    }
  }
}

/* Hann-windowed power spectrum of the mono mix (or of a - b) over [t0, t1), zero-padded to nfft */
typedef struct { int nfft; double fs; double *p; } spec_t;
static spec_t spectrum2(const wav_t *w, const wav_t *minus, double t0, double t1, int nfft) {
  spec_t s = {nfft, w->fs, malloc(sizeof(double) * (size_t)(nfft / 2 + 1))};
  double *re = calloc((size_t)nfft, sizeof(double)), *im = calloc((size_t)nfft, sizeof(double));
  long i0 = clampn(w, t0), n = clampn(w, t1) - i0;
  if (n > nfft) n = nfft;
  for (long i = 0; i < n; i++) {
    double hw = 0.5 - 0.5 * cos(2 * M_PI * i / (n - 1));
    double x = 0.5 * ((double)w->L[i0 + i] + w->R[i0 + i]);
    if (minus) x -= 0.5 * ((double)minus->L[i0 + i] + minus->R[i0 + i]);
    re[i] = x * hw;
  }
  fft(re, im, nfft);
  for (int k = 0; k <= nfft / 2; k++) s.p[k] = re[k] * re[k] + im[k] * im[k];
  free(re); free(im);
  return s;
}
static spec_t spectrum(const wav_t *w, double t0, double t1, int nfft) { return spectrum2(w, NULL, t0, t1, nfft); }
/* strongest peak in [f0, f1] (parabolic interpolation on dB) */
static double spec_peak(const spec_t *s, double f0, double f1) {
  double bin = s->fs / s->nfft;
  int k0 = (int)(f0 / bin), k1 = (int)(f1 / bin) + 1;
  if (k0 < 1) k0 = 1;
  if (k1 > s->nfft / 2 - 1) k1 = s->nfft / 2 - 1;
  int best = k0;
  for (int k = k0; k <= k1; k++) if (s->p[k] > s->p[best]) best = k;
  double a = db10(s->p[best - 1]), b = db10(s->p[best]), c = db10(s->p[best + 1]);
  double den = a - 2 * b + c, off = den != 0 ? 0.5 * (a - c) / den : 0;
  return (best + off) * bin;
}
/* power-weighted mean frequency in [f0, f1] */
static double spec_centroid(const spec_t *s, double f0, double f1) {
  double bin = s->fs / s->nfft, e = 0, ef = 0;
  for (int k = (int)ceil(f0 / bin); k <= f1 / bin && k <= s->nfft / 2; k++) { e += s->p[k]; ef += s->p[k] * k * bin; }
  return e > 0 ? ef / e : 0;
}
/* band power (dB) of [f0, f1) */
static double band_db(const spec_t *s, double f0, double f1) {
  double bin = s->fs / s->nfft, e = 0;
  for (int k = (int)ceil(f0 / bin); k < f1 / bin && k <= s->nfft / 2; k++) e += s->p[k];
  return db10(e);
}
/* the strongest component in [f0, f1] over a 40 ms window at t */
static double track(const wav_t *w, double t, double f0, double f1) {
  spec_t s = spectrum(w, t, t + 0.04, 16384);
  double f = spec_peak(&s, f0, f1);
  free(s.p);
  return f;
}

/* 1/3-octave band levels (31.5 Hz .. 16 kHz) of c vs r over 0.5 s windows, in bands within 40 dB
 * of each window's loudest band */
static stat_t third_octave_diff(const wav_t *c, const wav_t *r, double t0, double t1) {
  stat_t st = {0, 0, 0, 0};
  double *d = malloc(sizeof(double) * 4096);
  for (double t = t0; t + 0.5 <= t1 + 1e-9; t += 0.5) {
    spec_t sc = spectrum(c, t, t + 0.5, 32768), sr = spectrum(r, t, t + 0.5, 32768);
    double lc[28], lr[28], top = -1e9;
    for (int b = 0; b < 28; b++) {
      double fc = 1000 * pow(2, (b - 15) / 3.0), lo = fc * pow(2, -1 / 6.0), hi = fc * pow(2, 1 / 6.0);
      lc[b] = band_db(&sc, lo, hi); lr[b] = band_db(&sr, lo, hi);
      if (lr[b] > top) top = lr[b];
    }
    for (int b = 0; b < 28; b++) {
      if (lr[b] < top - 40 || st.n >= 4096) continue;
      double x = fabs(lc[b] - lr[b]);
      d[st.n++] = x; st.mean += x;
      if (x > st.max) st.max = x;
    }
    free(sc.p); free(sr.p);
  }
  stat_end(&st, d);
  free(d);
  return st;
}

/* first / last time |L| exceeds thr within [t0, t1) */
static void onset_offset(const wav_t *w, double t0, double t1, double thr, double *on, double *off) {
  long i0 = clampn(w, t0), i1 = clampn(w, t1);
  *on = *off = -1;
  for (long i = i0; i < i1; i++)
    if (fabsf(w->L[i]) > thr) { if (*on < 0) *on = (double)i / w->fs; *off = (double)i / w->fs; }
}

static void print_levels(const char *tag, const wav_t *c, const wav_t *r) {
  if (!verbose) return;
  printf("  %s: t(s)  C rmsL rmsR peak | Chromium rmsL rmsR peak (dBFS, 250 ms)\n", tag);
  for (double t = 0; t + 0.25 <= (double)c->n / c->fs + 1e-9; t += 0.25) {
    long i0 = clampn(c, t), i1 = clampn(c, t + 0.25);
    printf("   %5.2f  %6.1f %6.1f %6.1f", t, db10(pow_ch(c->L, i0, i1)), db10(pow_ch(c->R, i0, i1)), peak_db(c, t, t + 0.25));
    if (r) printf(" | %6.1f %6.1f %6.1f", db10(pow_ch(r->L, i0, i1)), db10(pow_ch(r->R, i0, i1)), peak_db(r, t, t + 0.25));
    printf("\n");
  }
}

/* the reference WAV (and JSON) of a scenario; with the JSON its inputs are taken over */
static int load_ref(scen_t *sc, const char *name, wav_t *r) {
  if (!refdir) return -1;
  char path[1024];
  snprintf(path, sizeof path, "%s/%s.wav", refdir, name);
  if (wav_read(path, r)) { printf("  (no reference %s: skipped)\n", path); return -1; }
  int bad = json_seq_use(sc, name);
  if (bad >= 0) CHECK_INT(bad, 0);
  return 0;
}

/* a reference-mode render vs Chromium: sample-exact */
static void compare_ref(const char *name, const wav_t *c, const wav_t *r) {
  double t1 = (double)r->n / r->fs;
  stat_t e10 = env_diff(c, r, 0.010, 0, t1, -60), e250 = env_diff(c, r, 0.250, 0, t1, -60);
  stat_t b3 = third_octave_diff(c, r, 0, t1);
  double nl = null_db(c, r, 0, t1);
  printf("  %s vs Chromium: sample null %.1f dB; 10 ms env |diff| p95 %.4f max %.4f dB; 250 ms max %.4f dB; "
         "1/3-oct p95 %.4f max %.4f dB\n", name, nl, e10.p95, e10.max, e250.max, b3.p95, b3.max);
  CHECK_INT(r->n, c->n);
  CHECK(e10.n > 0);
  CHECK(nl < -60);                    /* spec §13.4: <= -60 dB */
  CHECK(e10.p95 <= 0.05);
  CHECK(e250.max <= 0.05);
  CHECK(b3.max <= 0.1);
}
/* a production-mode render vs Chromium: the spec's 1 dB */
static void compare_prod(const char *name, const wav_t *c, const wav_t *r) {
  double t1 = (double)r->n / r->fs;
  stat_t e10 = env_diff(c, r, 0.010, 0, t1, -60), e250 = env_diff(c, r, 0.250, 0, t1, -60);
  stat_t b3 = third_octave_diff(c, r, 0, t1);
  printf("  %s (production mode) vs Chromium: 10 ms env |diff| p95 %.3f max %.3f dB; 250 ms max %.3f dB; "
         "1/3-oct p95 %.3f max %.3f dB\n", name, e10.p95, e10.max, e250.max, b3.p95, b3.max);
  CHECK(e10.p95 <= 0.5);
  CHECK(e250.max <= 0.5);
  CHECK(b3.p95 <= 1.0);
}

/* ------------------------------------------------------------------------------------- the tests */
static const double RPM[5] = {31679, 31679, 36301, 56381, 46605};
static const int BLADES[5] = {3, 3, 3, 3, 4};
static double bpf_of(int q, double frac) {         /* blade pass for a thrust fraction, no stick mix */
  return RPM[q - 1] * fmax(0.1, sqrt(fmax(0.008, frac))) / 60 * BLADES[q - 1];
}

static void test_spool(int quad) {
  scen_t sc;
  sc_spool(&sc, quad, 46080, 768);
  printf("%s (60 Hz frames, fs 46080)\n", sc.name);
  wav_t r = {0};
  int have = load_ref(&sc, sc.name, &r) == 0;
  wav_t c = render(&sc, REFMODE), cp = render(&sc, 0);
  wav_write(sc.name, &c);
  /* spec checks (§10.1, §10.3); the pitch wanders by +-3.5 cents (and more in the 0.7 s windows'
   * mean), so the spectral peaks get 0.6 % */
  spec_t sp = spectrum(&c, 1.0, 2.0, 65536);
  double b_idle = bpf_of(quad, I0[quad - 1]), f = spec_peak(&sp, b_idle * 0.97, b_idle * 1.03);
  printf("  idle blade pass %.2f Hz (expected %.2f)\n", f, b_idle);
  CHECK_NEAR(f, b_idle, b_idle * 0.006);
  free(sp.p);
  sp = spectrum(&c, 4.2, 4.9, 65536);
  double b_full = bpf_of(quad, 1.0), sh = b_full / BLADES[quad - 1], wh = sh * 7;
  double fb = spec_peak(&sp, b_full * 0.98, b_full * 1.02), fsh = spec_peak(&sp, sh * 0.98, sh * 1.02),
         fw = spec_peak(&sp, wh * 0.98, wh * 1.02);
  printf("  full: blade pass %.2f Hz (expected %.2f), shaft %.2f (%.2f), whine %.2f (%.2f)\n", fb, b_full, fsh, sh, fw, wh);
  CHECK_NEAR(fb, b_full, b_full * 0.006);
  CHECK_NEAR(fsh, sh, sh * 0.006);
  CHECK_NEAR(fw, wh, wh * 0.006);
  free(sp.p);
  /* the countdown beeps alone: 1000 Hz at 0 s, 2000 Hz at 0.25 s; plateau 0.045 * 0.848 * 0.8^2 */
  double on, off, plateau = 0.045 * 0.848191 * 0.64;
  onset_offset(&c, 0, 0.2, plateau / 2, &on, &off);
  printf("  beep 1 sounds %.4f .. %.4f s (expected %.4f .. %.4f)\n", on, off, BEEP_ON, 0.12 - BEEP_OFF);
  CHECK_NEAR(on, BEEP_ON, 0.0006);
  CHECK_NEAR(off, 0.12 - BEEP_OFF, 0.0006);
  double bp = peak_db(&c, 0.02, 0.1), br = rms_db(&c, 0.3, 0.45);
  printf("  beep peak %.2f dBFS (Gibbs peak = 0.045*0.64: %.2f), 2000 Hz beep rms %.2f dBFS (spec -32.5)\n", bp,
         20 * log10(0.045 * 0.64), br);
  CHECK_NEAR(bp, 20 * log10(0.045 * 0.64), 0.2);
  CHECK_NEAR(br, -32.5, 0.3);
  CHECK(rms_db(&c, 0.15, 0.24) < -100);              /* between the beeps: silence (motors off) */
  double pk = peak_db(&c, 4.2, 4.9), ri = rms_db(&c, 1, 2), rf = rms_db(&c, 4.2, 4.9);
  printf("  motors: idle rms %.1f, full rms %.1f / peak %.1f dBFS; PAUSED at 7.2 s: %.1f, %.1f, %.1f dBFS\n", ri, rf,
         pk, rms_db(&c, 7.25, 7.5), rms_db(&c, 7.5, 7.75), rms_db(&c, 7.75, 8.0));
  CHECK(pk > -12 && pk < -9);                        /* the compressor holds peaks near -10 dBFS */
  CHECK(ri > -24 && ri < -14);
  CHECK(rms_db(&c, 7.5, 7.75) < -80);
  CHECK(rms_db(&c, 7.75, 8.0) < -120);
  stat_t pr = env_diff(&cp, &c, 0.010, 0, 8, -60);
  printf("  production vs reference mode: 10 ms env |diff| p95 %.3f max %.3f dB\n", pr.p95, pr.max);
  CHECK(pr.p95 < 0.2);
  if (have) {
    print_levels(sc.name, &c, &r);
    compare_ref(sc.name, &c, &r);
    compare_prod(sc.name, &cp, &r);
    spec_t a = spectrum(&c, 4.2, 4.9, 65536), b = spectrum(&r, 4.2, 4.9, 65536);
    double fa = spec_peak(&a, b_full * 0.98, b_full * 1.02), fr = spec_peak(&b, b_full * 0.98, b_full * 1.02);
    printf("  full blade pass peak: C %.3f Hz, Chromium %.3f Hz\n", fa, fr);
    CHECK_NEAR(fa, fr, 0.5);
    free(a.p); free(b.p);
    double onr, offr;
    onset_offset(&r, 0, 0.2, plateau / 2, &onr, &offr);
    printf("  beep 1 timing: C %.5f..%.5f s, Chromium %.5f..%.5f s; beeps (0-0.45 s) null %.1f dB\n", on, off, onr,
           offr, null_db(&c, &r, 0, 0.45));
    CHECK_NEAR(on, onr, 0.5 / sc.fs);
    CHECK_NEAR(off, offr, 0.5 / sc.fs);
    CHECK(null_db(&c, &r, 0, 0.45) < -90);
    wav_free(&r);
  }
  wav_free(&c); wav_free(&cp);
  free(sc.seq);
}

static void test_raw(void) {
  scen_t sc;
  sc_spool(&sc, 1, 46080, 768);
  printf("spool_q1_raw (shaper, shelf, compressor bypassed: sample-wise null test)\n");
  wav_t r = {0};
  int have = load_ref(&sc, "spool_q1_raw", &r) == 0;
  wav_t c = render(&sc, REFMODE | SFX_DBG_RAW);
  wav_write("spool_q1_raw", &c);
  printf("  idle %.1f dBFS rms (spec -23..-31), full %.1f (spec -18..-21), full peak %.1f (spec -6.3..-7.5)\n",
         rms_db(&c, 1, 2), rms_db(&c, 4.2, 4.9), peak_db(&c, 4.2, 4.9));
  CHECK(rms_db(&c, 1, 2) > -32 && rms_db(&c, 1, 2) < -22);
  CHECK(rms_db(&c, 4.2, 4.9) > -22 && rms_db(&c, 4.2, 4.9) < -17);
  if (have) {
    print_levels("spool_q1_raw", &c, &r);
    double n1 = null_db(&c, &r, 0, 2), n2 = null_db(&c, &r, 0, 5), n3 = null_db(&c, &r, 0, 8);
    printf("  residual vs Chromium: 0-2 s %.1f dB, 0-5 s %.1f dB, 0-8 s %.1f dB (spec: <= -60)\n", n1, n2, n3);
    if (verbose) for (double t = 0; t < 8; t += 1) printf("    %.0f-%.0f s: %.1f dB\n", t, t + 1, null_db(&c, &r, t, t + 1));
    CHECK(n1 < -90);
    CHECK(n3 < -75);
    compare_ref("spool_q1_raw", &c, &r);
    wav_free(&r);
  }
  wav_free(&c);
  free(sc.seq);
}

static void test_manoeuvres(void) {
  scen_t sc, sn;
  sc_manoeuvres(&sc);
  sc_manoeuvres_v(&sn, 0);
  printf("manoeuvres\n");
  wav_t r = {0};
  int have = load_ref(&sc, "manoeuvres", &r) == 0;
  wav_t c = render(&sc, REFMODE), cn = render(&sn, REFMODE), cp = render(&sc, 0);
  wav_write("manoeuvres", &c);
  /* roll right at 0.5 s: motors 1, 2 at 0.5 x 1.3, motors 0, 3 at 0.5 x 0.7 (spec: 953..1268 Hz) */
  spec_t sp = spectrum(&c, 0.4, 0.6, 65536);
  double lo = spec_peak(&sp, 850, 1060), hi = spec_peak(&sp, 1150, 1350);
  printf("  roll right: blade pass split %.0f / %.0f Hz (expected %.0f / %.0f before the EMA)\n", lo, hi,
         bpf_of(1, 0.35), bpf_of(1, 0.65));
  CHECK_NEAR(lo, bpf_of(1, 0.35), 60);
  CHECK_NEAR(hi, bpf_of(1, 0.65), 60);
  free(sp.p);
  /* wind: the difference to the same run without the speed ramp; below the motors' air noise
   * (high-passed at 0.3 x blade pass = 336 Hz) it is the wind: gain 0.1 (v/35)^2, lowpass 150 + 15 v */
  spec_t d1 = spectrum2(&c, &cn, 3.4, 3.9, 32768), d2 = spectrum2(&c, &cn, 5.4, 5.9, 32768);
  spec_t s2 = spectrum(&c, 5.4, 5.9, 32768);
  double w1 = band_db(&d1, 30, 250), w2 = band_db(&d2, 30, 250), share = w2 - band_db(&s2, 30, 250);
  printf("  wind (difference to no speed, 30-250 Hz): %.1f dB at ~5 m/s, %.1f dB at ~37 m/s (expected ~+30); "
         "%.1f dB of the mix there\n", w1, w2, share);
  CHECK(w2 > w1 + 15);
  CHECK(share > -10);
  free(d1.p); free(d2.p); free(s2.p);
  /* dirty air (sinking 3.5 m/s, wash 1): rpm -6 %, rumble below the blade pass, chatter sidebands
   * (62 cents FM at the wash frequency): the blade pass band's centroid */
  sp = spectrum(&c, 7.0, 8.5, 65536);
  spec_t sq = spectrum(&c, 4.0, 5.5, 65536);
  double fd = spec_centroid(&sp, 950, 1150), fe = bpf_of(1, 0.5) * 0.94;
  double rum = band_db(&sp, 40, 250) - band_db(&sq, 40, 250);
  printf("  dirty air: blade pass centroid %.1f Hz (expected %.1f), rumble 40-250 Hz %+.1f dB vs clean air\n", fd,
         fe, rum);
  CHECK_NEAR(fd, fe, fe * 0.01);
  CHECK(rum > 6);
  free(sp.p); free(sq.p);
  if (have) {
    print_levels("manoeuvres", &c, &r);
    compare_ref("manoeuvres", &c, &r);
    compare_prod("manoeuvres", &cp, &r);
    wav_free(&r);
  }
  wav_free(&c); wav_free(&cn); wav_free(&cp);
  free(sc.seq); free(sn.seq);
}

static void test_beeps(void) {
  scen_t sc;
  sc_beeps(&sc);
  printf("beeps (queue, pause, PLAY_NOW, minimum length, 50 Hz floor)\n");
  wav_t r = {0};
  int have = load_ref(&sc, "beeps", &r) == 0;
  wav_t c = render(&sc, REFMODE), cp = render(&sc, 0);
  wav_write("beeps", &c);
  double thr = 0.045 * 0.848191 * 0.64 / 2, on, off;
  onset_offset(&c, 0.05, 0.3, thr, &on, &off);       /* battery: 0.1..0.25 s, 100 ms pause */
  CHECK_NEAR(on, 0.1 + BEEP_ON, 0.0008); CHECK_NEAR(off, 0.25 - BEEP_OFF, 0.0008);
  onset_offset(&c, 0.3, 0.6, thr, &on, &off);        /* the second: 0.35..0.5 s */
  printf("  battery double beep: second at %.4f..%.4f s (expected %.4f..%.4f)\n", on, off, 0.35 + BEEP_ON, 0.5 - BEEP_OFF);
  CHECK_NEAR(on, 0.35 + BEEP_ON, 0.0008); CHECK_NEAR(off, 0.5 - BEEP_OFF, 0.0008);
  /* finish pair 1800 Hz 0.8..0.92 + 2400 Hz from 0.92, cut at 0.95 by the crash beep (PLAY_NOW):
   * 260 Hz 0.95..1.35, the oscillator stops 20 ms after */
  spec_t s = spectrum(&c, 0.96, 1.3, 32768);
  double l24 = band_db(&s, 2380, 2420), l26 = band_db(&s, 250, 270);
  printf("  PLAY_NOW: after the cut 2400 Hz %.1f dB vs 260 Hz %.1f dB\n", l24, l26);
  CHECK(l24 < l26 - 60);
  free(s.p);
  s = spectrum(&c, 0.925, 0.948, 32768);
  CHECK(band_db(&s, 2380, 2420) > band_db(&s, 1780, 1820));      /* 2400 Hz did start */
  free(s.p);
  CHECK(rms_db(&c, 1.375, 1.55) < -100);
  onset_offset(&c, 1.55, 1.9, thr, &on, &off);       /* gates back to back 1.6..1.78 */
  CHECK_NEAR(on, 1.6 + BEEP_ON, 0.0008); CHECK_NEAR(off, 1.78 - BEEP_OFF, 0.0008);
  onset_offset(&c, 1.95, 2.2, thr, &on, &off);       /* 10 ms beep: 20 ms (hold from 8 ms) */
  printf("  10 ms beep sounds %.4f..%.4f s (20 ms minimum)\n", on, off);
  CHECK_NEAR(on, 2.0 + BEEP_ON, 0.0008);
  CHECK_NEAR(off, 2.0 + 0.008 + (0.012 - BEEP_OFF), 0.0008);
  s = spectrum(&c, 2.32, 2.48, 32768);
  double f50 = spec_peak(&s, 30, 70);
  printf("  30 Hz request plays at %.2f Hz\n", f50);
  CHECK_NEAR(f50, 50, 1);
  free(s.p);
  s = spectrum(&c, 2.7, 2.83, 32768);                /* PLAY_NOW in the same frame: no 2200 Hz */
  CHECK(band_db(&s, 2180, 2220) < band_db(&s, 990, 1010) - 60);
  free(s.p);
  stat_t pr = env_diff(&cp, &c, 0.010, 0, 4, -80);
  CHECK(pr.max < 0.01);                              /* beeps do not depend on the mode */
  if (have) {
    compare_ref("beeps", &c, &r);
    printf("  beeps null vs Chromium (production mode) %.1f dB\n", null_db(&cp, &r, 0, 4));
    CHECK(null_db(&c, &r, 0, 4) < -90);
    wav_free(&r);
  }
  wav_free(&c); wav_free(&cp);
  free(sc.seq);
}

static void test_crash(void) {
  scen_t sc;
  sc_crash(&sc);
  printf("crash (prop strike, crash with stale hv, respawn, battery-empty crash)\n");
  wav_t r = {0};
  int have = load_ref(&sc, "crash", &r) == 0;
  wav_t c = render(&sc, REFMODE), cp = render(&sc, 0);
  wav_write("crash", &c);
  /* thuds: low-passed noise bursts (420 Hz), below the crash beep's 260 Hz */
  double hover = bpf_of(2, 1 / TWR[1]);
  double fb = track(&c, 0.9, 0.7 * hover, 1.2 * hover), fd = track(&c, 1.035, 0.4 * hover, 1.2 * hover);
  spec_t a = spectrum(&c, 0.85, 0.98, 32768), b = spectrum(&c, 1.0, 1.13, 32768);
  double thud1 = band_db(&b, 30, 200) - band_db(&a, 30, 200);
  printf("  prop strike at 1 s: thud %+.1f dB (30-200 Hz), blade pass %.0f -> %.0f Hz (rpm x 0.725 then the EMA)\n",
         thud1, fb, fd);
  CHECK(thud1 > 6);
  CHECK(fd < fb * 0.9);
  free(a.p); free(b.p);
  /* battery empty (4 -> 5, no hit): thud 0.9 and rpm x 0.3 (a hit would be x 0.725) */
  double fb3 = track(&c, 5.4, 0.7 * hover, 1.2 * hover), fd3 = track(&c, 5.505, 0.15 * hover, 1.2 * hover);
  printf("  battery-empty crash at 5.5 s: blade pass %.0f -> %.0f Hz; motors fade: %.1f dBFS at 5.8-6.0 s, %.1f at 6.5-7 s\n",
         fb3, fd3, rms_db(&c, 5.8, 6.0), rms_db(&c, 6.5, 7.0));
  CHECK(fd3 < 0.6 * fb3);
  CHECK(rms_db(&c, 6.5, 7.0) < -60);
  if (have) {
    print_levels("crash", &c, &r);
    compare_ref("crash", &c, &r);
    compare_prod("crash", &cp, &r);
    wav_free(&r);
  }
  wav_free(&c); wav_free(&cp);
  free(sc.seq);
}

static void test_classes(void) {
  scen_t sc;
  sc_classes(&sc);
  printf("classes (1..5, hover then full; the whoops' wave and whine)\n");
  wav_t r = {0};
  int have = load_ref(&sc, "classes", &r) == 0;
  wav_t c = render(&sc, REFMODE), cp = render(&sc, 0);
  wav_write("classes", &c);
  for (int q = 1; q <= 5; q++) {                     /* full throttle in the last 0.5 s of each */
    double t0 = (q - 1) * 1.5 + 1.25;
    spec_t s = spectrum(&c, t0, t0 + 0.25, 32768);
    double sh = bpf_of(q, 1.0) / BLADES[q - 1], wh = sh * 7;
    double fw = spec_peak(&s, wh * 0.9, wh * 1.1);
    printf("  class %d full: whine %.0f Hz (expected ~%.0f with the stick mix), rms %.1f dBFS\n", q, fw, wh,
           rms_db(&c, t0, t0 + 0.25));
    CHECK_NEAR(fw, wh, wh * 0.08);
    CHECK(rms_db(&c, t0, t0 + 0.25) > -24);
    free(s.p);
  }
  if (have) {
    compare_ref("classes", &c, &r);
    compare_prod("classes", &cp, &r);
    wav_free(&r);
  }
  wav_free(&c); wav_free(&cp);
  free(sc.seq);
}

/* 50 Hz (PAL) frames sound like 60 Hz: the same scenario at fs 48000, frame 960 vs 800 */
static void test_frame_rate(void) {
  printf("frame rate: 50 Hz vs 60 Hz frames (fs 48000, production mode)\n");
  scen_t s60, s50;
  sc_spool(&s60, 1, 48000, 800);
  sc_spool(&s50, 1, 48000, 960);
  wav_t a = render(&s60, 0), b = render(&s50, 0), bl = render(&s50, SFX_DBG_LITERAL);
  stat_t e = env_diff(&b, &a, 0.25, 0.5, 7.2, -60), el = env_diff(&bl, &a, 0.25, 0.5, 7.2, -60);
  printf("  250 ms levels, 50 Hz vs 60 Hz: mean %.2f max %.2f dB (app.js's per-call constants at 50 Hz: mean %.2f max %.2f)\n",
         e.mean, e.max, el.mean, el.max);
  CHECK(e.max < 1.0);
  CHECK(e.mean < el.mean);
  /* the rpm dynamics: blade pass every 50 ms through the ramp to full and the chop */
  double d = 0, dl = 0;
  int n = 0;
  for (double t = 3.0; t < 5.4; t += 0.05) {
    if (t > 4.3 && t < 5.0) continue;
    double lo = 0.3 * bpf_of(1, 1), hi = 1.05 * bpf_of(1, 1);
    double fa = track(&a, t, lo, hi), fb = track(&b, t, lo, hi), fl = track(&bl, t, lo, hi);
    d += fabs(fb - fa); dl += fabs(fl - fa); n++;
  }
  printf("  blade pass during ramp/chop, mean |50 Hz - 60 Hz|: %.1f Hz (per-call constants: %.1f Hz)\n", d / n, dl / n);
  CHECK(d / n < 0.5 * (dl / n));
  CHECK(d / n < 40);
  wav_free(&a); wav_free(&b); wav_free(&bl);
  free(s60.seq); free(s50.seq);
}

/* volume: vol^2 law, smoothed; 0 = silence, synth torn down and rebuilt; beeps dropped at 0 */
static void test_volume(void) {
  printf("volume\n");
  scen_t sc;
  sc_spool(&sc, 1, 48000, 800);
  sc.nframes = 60 * 5;                               /* COUNT, idle FLY, ramp */
  wav_t a = render(&sc, 0);
  sc.vol = 0.4f;
  wav_t b = render(&sc, 0);
  double d = rms_db(&b, 0.3, 0.45) - rms_db(&a, 0.3, 0.45), dm = rms_db(&b, 1, 2) - rms_db(&a, 1, 2);
  printf("  40 %% vs 80 %%: beep %.2f dB, motors %.2f dB (expected -12.04)\n", d, dm);
  CHECK_NEAR(d, -12.04, 0.02);
  CHECK_NEAR(dm, -12.04, 0.02);
  wav_free(&b);
  /* volume to 0 at 1.5 s, back to 0.8 at 2.5 s; a beep at 2.0 s (dropped) */
  sc.vol = 0.8f;
  sc.nvol = 2; sc.vol_k[0] = 90; sc.vol_at[0] = 0; sc.vol_k[1] = 150; sc.vol_at[1] = 0.8f;
  tone_add(&sc, 2.0, 1000, 100, 0, 0);
  b = render(&sc, 0);
  printf("  vol 0 at 1.5 s: %.1f dBFS at 1.6-1.8 s, %.1f at 1.9-2.5 s; back at 2.5 s: %.1f at 3-3.5 s (always on: %.1f)\n",
         rms_db(&b, 1.6, 1.8), rms_db(&b, 1.9, 2.5), rms_db(&b, 3, 3.5), rms_db(&a, 3, 3.5));
  CHECK(rms_db(&b, 1.6, 1.8) < rms_db(&a, 1.6, 1.8) - 20);
  CHECK(rms_db(&b, 1.9, 2.5) < -200);                /* silent; the beep was dropped */
  CHECK(rms_db(&b, 3, 3.5) > -25);                   /* rebuilt */
  double s0 = click_score(&b, 1.0, 1.5), s1 = click_score(&b, 1.5, 1.8), s2 = click_score(&b, 2.5, 3.0);
  printf("  click score (largest step / local rms step): %.1f running, %.1f in the fade-out, %.1f at the restart\n",
         s0, s1, s2);
  CHECK(s1 < 8);
  CHECK(s2 < 8);
  wav_free(&b);
  /* a quick toggle (0 for 50 ms) while the motors run: no click (the new synth waits for the fade) */
  sc.nvol = 2; sc.vol_k[0] = 90; sc.vol_at[0] = 0; sc.vol_k[1] = 93; sc.vol_at[1] = 0.8f;
  b = render(&sc, 0);
  double s3 = click_score(&b, 1.5, 2.3);
  printf("  vol 0 for 50 ms: click score %.1f; restarted after the fade: %.1f dBFS at 2.0-2.3 s\n", s3,
         rms_db(&b, 2.0, 2.3));
  CHECK(s3 < 8);
  CHECK(rms_db(&b, 2.0, 2.3) > -30);
  wav_free(&a); wav_free(&b);
  free(sc.seq);
}

/* determinism, seeds, reset, no-hook frames, short value lists, garbage, floods */
static void test_misc(void) {
  printf("misc\n");
  scen_t sc;
  sc_spool(&sc, 1, 48000, 800);
  sc.nframes = 120;
  wav_t a = render(&sc, 0), b = render(&sc, 0);
  CHECK(memcmp(a.L, b.L, sizeof(float) * (size_t)a.n) == 0);
  CHECK(memcmp(a.R, b.R, sizeof(float) * (size_t)a.n) == 0);
  sc.seed = 99;
  wav_t c = render(&sc, 0);
  CHECK(memcmp(a.L, c.L, sizeof(float) * (size_t)a.n) != 0);
  wav_free(&a); wav_free(&b); wav_free(&c);
  free(sc.seq);

  /* sfx_reset: motors off quickly, the tone queue cleared */
  sfx_t *s = sfx_new(48000, 7);
  CHECK(s != NULL);
  if (!s) return;
  sfx_set_volume(s, 1);
  double v[NV];
  a_def(v, 1);
  v[3] = 0.5;
  static float L[48000], R[48000];
  for (int k = 0; k < 60; k++) { sfx_frame(s, v, NV, 1000.0 / 60); sfx_render(s, L, R, 800); }
  memset(L, 0, sizeof L); memset(R, 0, sizeof R);
  sfx_tone(s, 1000, 500, 0, 0, 0);
  sfx_frame(s, v, NV, 1000.0 / 60);
  sfx_render(s, L, R, 800);
  double before = db10(pow_ch(L, 0, 800));
  sfx_tone(s, 1500, 2000, 0, 0, 0);                  /* queued behind the first */
  sfx_reset(s);
  v[2] = 1;                                          /* the reloaded game: menu */
  memset(L, 0, sizeof L);
  for (int k = 0; k < 30; k++) { sfx_frame(s, v, NV, 1000.0 / 60); sfx_render(s, L + 800 * k, R + 800 * k, 800); }
  double after = db10(pow_ch(L, 9600, 24000));       /* 0.2-0.5 s after the reset */
  printf("  sfx_reset: %.1f dBFS before, %.1f dBFS 0.2-0.5 s after\n", before, after);
  CHECK(after < -100);
  /* frames without the hook (n < 10): rpm decays, silent; then live again */
  memset(L, 0, sizeof L);
  for (int k = 0; k < 30; k++) { sfx_frame(s, NULL, 0, 1000.0 / 60); sfx_render(s, L + 800 * k, R + 800 * k, 800); }
  CHECK(db10(pow_ch(L, 9600, 24000)) < -100);
  /* the BW/Lite games return 10 values: still works */
  v[2] = 4;
  memset(L, 0, sizeof L);
  for (int k = 0; k < 30; k++) { sfx_frame(s, v, 10, 1000.0 / 60); sfx_render(s, L + 800 * k, R + 800 * k, 800); }
  CHECK(db10(pow_ch(L, 9600, 24000)) > -30);
  /* garbage values: no NaN in the output */
  double g[NV];
  for (int i = 0; i < NV; i++) g[i] = NAN;
  g[2] = 4;
  memset(L, 0, sizeof L);
  for (int k = 0; k < 10; k++) { sfx_frame(s, g, NV, 1000.0 / 60); sfx_render(s, L + 800 * k, R + 800 * k, 800); }
  for (int i = 0; i < NV; i++) g[i] = 1e300;
  for (int k = 10; k < 20; k++) { sfx_frame(s, g, NV, 1e9); sfx_render(s, L + 800 * k, R + 800 * k, 800); }
  int bad = 0;
  for (int i = 0; i < 16000; i++) if (!isfinite(L[i]) || !isfinite(R[i]) || fabsf(L[i]) > 4) bad++;
  CHECK_INT(bad, 0);
  sfx_free(s);

  /* PLAY_BACKGROUND (0x20): one triangle voice, 0.018 (x N 1.000198), retuned per call (tau 12 ms),
   * off tau 10 ms after its duration; duration 0 silences it at once (tau 8 ms) */
  s = sfx_new(48000, 8);
  sfx_set_volume(s, 1);
  memset(L, 0, sizeof L); memset(R, 0, sizeof R);
  sfx_tone(s, 440, 300, 0, 0x20, 0);
  for (int k = 0; k < 10; k++) sfx_render(s, L + 4800 * k, R + 4800 * k, 4800);
  wav_t bw = {48000, 48000, L, R};
  spec_t bs = spectrum(&bw, 0.1, 0.3, 32768);
  double bf = spec_peak(&bs, 300, 600), brms = rms_db(&bw, 0.1, 0.3);
  free(bs.p);
  printf("  background tone: %.2f Hz, %.2f dBFS rms (triangle 0.018: %.2f); %.1f dBFS 0.42-0.5 s\n", bf, brms,
         20 * log10(0.018 * 1.000198 / sqrt(3)), rms_db(&bw, 0.42, 0.5));
  CHECK_NEAR(bf, 440, 0.5);
  CHECK_NEAR(brms, 20 * log10(0.018 * 1.000198 / sqrt(3)), 0.1);
  CHECK(rms_db(&bw, 0.42, 0.5) < -80);
  memset(L, 0, sizeof L); memset(R, 0, sizeof R);
  sfx_tone(s, 880, 1000, 0, 0x20, 0);                /* retune and on again, then silenced by dur 0 */
  sfx_render(s, L, R, 9600);
  sfx_tone(s, 880, 0, 0, 0x20, 0);
  sfx_render(s, L + 9600, R + 9600, 9600);
  CHECK(rms_db(&bw, 0.1, 0.2) > -45);
  CHECK(rms_db(&bw, 0.3, 0.4) < -80);
  sfx_free(s);

  /* a flood of tones: the ring (64) drops the excess, counted; nothing breaks */
  s = sfx_new(48000, 9);
  sfx_set_volume(s, 1);
  for (int i = 0; i < 200; i++) sfx_tone(s, 1000 + i, 30, 0, 0, 0);
  CHECK(sfx_debug_dropped(s) == 200 - 64);
  sfx_render(s, L, R, 128);
  for (int i = 0; i < 20; i++) sfx_render(s, L, R, 128);
  sfx_free(s);
  sfx_free(NULL);
}

int main(int argc, char **argv) {
  refdir = argc > 1 ? argv[1] : getenv("STP_SFX_REF");
  outdir = argc > 2 ? argv[2] : getenv("STP_SFX_OUT");
  verbose = getenv("STP_SFX_VERBOSE") != NULL;
  if (refdir && !*refdir) refdir = NULL;
  if (outdir && !*outdir) outdir = NULL;
  if (!refdir) printf("(no STP_SFX_REF: the comparisons with the Chromium renders are skipped)\n");
  test_spool(1);
  test_spool(4);
  test_raw();
  test_manoeuvres();
  test_beeps();
  test_crash();
  test_classes();
  test_frame_rate();
  test_volume();
  test_misc();
  T_DONE();
}
