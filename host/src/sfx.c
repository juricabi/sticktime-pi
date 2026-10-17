/* sfx.c: the web emulator's sound effects (notes: audio-motor.md): the motor synth and the beeps.
 *
 * Frame thread (the game loop):
 *   sfx_frame()  literal port of app.js motorSound(): per-motor rpm from the stick mix, the crash
 *                thud trigger and ~70 parameter targets, published through a wait-free triple buffer
 *   sfx_tone()   app.js tone(): beeps go through a single-producer/single-consumer event ring and are
 *                queued (pause, PLAY_NOW flush, PLAY_BACKGROUND) in audio sample time
 *   sfx_reset()  the game was reloaded: motors off now (10 ms fade), beeps cut, control state fresh
 * Audio thread:
 *   sfx_render() the WebAudio graph of app.js startSynth() written out per sample: 4 motors x (2
 *                blade-pass PeriodicWave oscillators + shaft + 2 whines + band-passed noise) with
 *                level/pitch modulation, panning, dirty-air rumble, wind, tanh saturation (2x),
 *                master, -6 dB high shelf, Chromium's compressor; crash/hit thuds; beeps; the
 *                output gain vol^2 (tau 0.03 s)
 * Any thread: sfx_set_volume() (atomic). At volume 0 the synth fades (tau 0.04 s) and is torn down
 * after 0.3 s; the next sfx_frame with volume > 0 builds it fresh (all DSP and control state reset;
 * the random noise/modulator buffers, generated once at sfx_new, are reused). A restart that
 * arrives while the old synth still fades waits for the end of the fade (the browser lets the old
 * graph fade while a new one starts; with one synth that would be a click).
 *
 * Rendering follows Chromium's: 128-frame render quanta from sample 0 (k-rate params, compressor
 * divisions), parameter automation per quantum segment (dsp_smooth_fill), parameters re-targeted at
 * the first sample of the sfx_render() call that sees a new frame. Filter coefficients are
 * recomputed every 16 samples while their frequency moves (Chromium: every sample; SFX_DBG_REF).
 *
 * Control smoothing is time-based so 50 Hz (PAL) frames sound like the browser's 60 Hz:
 * rpm EMA 1-0.88^(dt/16.667), not-live decay 0.92^(dt/16.667), slope x 16.667/dt (identical to
 * app.js at dt = 16.667 ms; SFX_DBG_LITERAL uses app.js's per-call constants).
 *
 * The audio thread never allocates, locks or does I/O; everything it touches is preallocated.
 * Verified sample by sample against the browser's own code rendered by Chromium (tests/test_sfx.c:
 * residual -67 .. -105 dB in reference mode, envelopes within 0.1 dB in production mode).
 */
#include "audio_internal.h"
#include "dsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* The reference renders come from Chromium's x86 build, which has no fused multiply-adds here */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

#define SFX_PLAY_NOW        0x10
#define SFX_PLAY_BACKGROUND 0x20

/* test/debug configuration (not part of audio_internal.h; tests declare it themselves) */
#define SFX_DBG_REF     1    /* per-sample filter coefficients, as Chromium (default: every 16) */
#define SFX_DBG_LITERAL 2    /* app.js's per-call EMA constants (60 Hz browser) instead of dt-based */
#define SFX_DBG_RAW     4    /* bypass shaper, shelf and compressor (the harness's "raw" patch) */
void sfx_debug_config(sfx_t *s, int flags);
unsigned sfx_debug_dropped(sfx_t *s);

/* ---------------------------------------------------------------------------- app.js constants */
typedef struct { double rpm; int blades, poles; double buzz, whine, shaft, noise; int wave; } aclass_t;
static const aclass_t ACLASS[5] = {
  {31679, 3, 7, 1.0, 0.14, 0.10, 0.45, 0},   /* 5" racer */
  {31679, 3, 7, 0.9, 0.16, 0.12, 0.50, 0},   /* 5" freestyle */
  {36301, 3, 7, 0.8, 0.18, 0.10, 0.50, 0},   /* 3" toothpick */
  {56381, 3, 7, 0.4, 0.40, 0.05, 0.40, 1},   /* 65 mm whoop */
  {46605, 4, 7, 0.5, 0.32, 0.06, 0.40, 1},   /* 75 mm whoop (2S) */
};
static const float WAVES[2][7] = {{0, 1, 0.2f, 0.08f, 0.06f, 0.04f, 0.02f},
                                  {0, 1, 0.8f, 0.6f, 0.35f, 0.3f, 0.15f}};
static const int MIX[4][3] = {{-1, 1, -1}, {1, 1, 1}, {1, -1, 1}, {-1, -1, -1}};
static const float PAN[4] = {-0.55f, 0.55f, -0.3f, 0.3f};
static const double TIMBRE[4][2] = {{1, 1}, {0.9, 0.85}, {1.08, 1.12}, {0.94, 0.88}};
#define CENTS 1731.0
#define MOD_RATE 8000
#define MOD_N (MOD_RATE * 8)
#define FRAME60_MS (1000.0 / 60.0)
#define QN DSP_RENDER_QUANTUM

/* the automated parameters: per motor and global, with their initial values (the node defaults of
 * startSynth) and setTargetAtTime time constants (motorSound) */
enum { MP_BUZZ_F, MP_BUZZ2_F, MP_SHAFT_F, MP_WHINE_F, MP_WHINE2_F, MP_WAND, MP_LP_F, MP_NLP_F, MP_NHP_F,
       MP_BG, MP_B2G, MP_SG, MP_WG, MP_W2G, MP_NG, MP_N };
static const float MP_INIT[MP_N] = {200, 201.4f, 70, 1400, 2830, 0, 1500, 350, 350, 0, 0, 0, 0, 0, 0};
static const double MP_TAU[MP_N] = {0.02, 0.02, 0.02, 0.03, 0.03, 0.1, 0.05, 0.05, 0.05,
                                    0.04, 0.04, 0.04, 0.04, 0.04, 0.07};
enum { GP_MASTER, GP_FLUT, GP_CHAT_FM, GP_CHAT_AM, GP_DVF, GP_DVG, GP_DVCH, GP_WGN, GP_WF, GP_N };
static const float GP_INIT[GP_N] = {0, 0, 0, 0, 400, 0, 0, 0, 200};
static const double GP_TAU[GP_N] = {0.06, 0.1, 0.05, 0.05, 0.1, 0.08, 0.05, 0.1, 0.1};

/* ------------------------------------------------------------------------- frame -> audio data */
typedef struct {
  uint32_t gen;
  int wave;
  float chat_rate;
  float g[GP_N];
  float m[4][MP_N];
} targets_t;

enum { EV_TONE = 1, EV_THUD, EV_START, EV_RESET };
typedef struct { int type; int f, dur, pause, flags; float gain; uint32_t gen; } sfx_event_t;
#define RING_N 64

/* ----------------------------------------------------------------------------- audio-side state */
typedef struct {
  dsp_osc_t buzz, buzz2, shaft, whine, whine2;
  dsp_smooth_t p[MP_N];
  float buf[MP_N][QN];                  /* this segment's parameter values */
  dsp_biquad_t lp, nlp, nhp;
  float lp_at, nlp_at, nhp_at;          /* frequency of the current coefficients */
  dsp_loop_t nhead, whead;
  double nstep, wstep;                  /* noise rate 0.9 + 0.05m; wander rate 0.8 + 0.17m */
  float gl, gr;                         /* panner gains */
  float gax[2];                         /* MIX[m][ax] / sqrt 2 */
} motor_t;

#define MAX_TONES 16
#define MAX_THUDS 8
typedef struct { int on; int64_t start, stop; float freq; dsp_osc_t osc; dsp_param_t gain; } tone_t;
typedef struct {
  int on; int64_t start, stop, end;
  dsp_loop_t head; dsp_biquad_t lp; dsp_param_t gain;
} thud_t;

struct sfx {
  double fs;
  int dbg;
  uint32_t vol_bits;                   /* atomic: the volume 0..1 as float bits */

  /* frame thread -> audio thread */
  sfx_event_t ring[RING_N];
  unsigned ring_head, ring_tail;       /* atomic */
  unsigned dropped;                    /* atomic: events lost to a full ring */
  targets_t tb[3];
  unsigned tb_mid;                     /* atomic: index of the middle buffer, | 4 when fresh */
  int tb_back, tb_front;

  /* frame thread: control state (app.js synth.rpmS, prev, state, hits, vol, dirty) */
  int c_alive;
  uint32_t c_gen;
  double c_rpm[4], c_prev, c_state, c_hits, c_venv, c_dirty;

  /* tables (built by sfx_new, read-only afterwards) */
  float *noise; int noise_n;
  float *slow, *chat[2];
  dsp_wave_t *wave[2], *sine, *square, *triangle;
  float curve[257];
  double q08, q00, q05, q10;           /* Qlin of 0.8, 0, 0.5, 1 dB */
  float c_mp[MP_N], c_gp[GP_N], c_chat_rate, c_master_stop, c_master_fast, c_out, c_bgf;

  /* audio thread */
  int64_t now;                         /* sample clock: frames rendered */
  float vol_seen;                      /* -1: no volume applied yet (the first one jumps) */
  int active, stopping, restart;       /* restart: a new synth waits for the fade-out */
  uint32_t restart_gen;
  int64_t stop_at, fast_until;
  uint32_t gen;
  int wave_idx;
  dsp_smooth_t gp[GP_N];
  float gbuf[GP_N][QN];
  dsp_smooth_t chat_rate;              /* k-rate: one value per render quantum */
  int cr_pend;                         /* a new target: 1 before the next quantum, 2 after this one */
  float cr_next;
  double chat_step;
  dsp_loop_t flut_head, dvfl_head, chat_head[2], dv_head, ws_head;
  double step_slow1, step_slow33, step_dv, step_ws;
  dsp_biquad_t dvf_bq, wf_bq, shelf[2];
  float dvf_at, wf_at;
  dsp_shaper_t shaper[2];
  dsp_comp_t comp;
  motor_t m[4];
  thud_t thud[MAX_THUDS];
  tone_t tone[MAX_TONES];
  double tone_end;
  int bg_on;
  dsp_osc_t bg_osc;
  dsp_smooth_t bg_freq;
  dsp_param_t bg_gain;
  float bg_last;
  dsp_smooth_t out;
  float obuf[QN];
};

/* --------------------------------------------------------------------------------------- helpers */
static inline float load_vol(const sfx_t *s) {
  uint32_t b = __atomic_load_n(&s->vol_bits, __ATOMIC_ACQUIRE);
  float v;
  memcpy(&v, &b, sizeof v);
  return v;
}
static inline double clampd(double x, double lo, double hi) { return x < lo ? lo : x > hi ? hi : x; }
static inline double fin(double x) { return isfinite(x) ? x : 0; }
static inline float sane(float x) { return !(x == x) ? 0.0f : x > 1e7f ? 1e7f : x < -1e7f ? -1e7f : x; }
static int to_int32(double x) {                         /* JS x | 0 (for the small values here) */
  if (!isfinite(x)) return 0;
  if (x >= 2147483647.0) return 2147483647;
  if (x <= -2147483648.0) return (-2147483647 - 1);
  return (int)x;
}
/* Chromium's TimeToSampleFrame(t, fs, kRoundUp): the frame a source starts / stops at */
static int64_t frame_up(double t, double fs) {
  double f = round(t * fs * 1024.0) / 1024.0;
  return (int64_t)ceil(f);
}

static int ring_push(sfx_t *s, const sfx_event_t *e) {
  unsigned h = __atomic_load_n(&s->ring_head, __ATOMIC_RELAXED);
  unsigned t = __atomic_load_n(&s->ring_tail, __ATOMIC_ACQUIRE);
  if (h - t >= RING_N) { __atomic_add_fetch(&s->dropped, 1, __ATOMIC_RELAXED); return 0; }
  s->ring[h & (RING_N - 1)] = *e;
  __atomic_store_n(&s->ring_head, h + 1, __ATOMIC_RELEASE);
  return 1;
}
static int ring_pop(sfx_t *s, sfx_event_t *e) {
  unsigned t = __atomic_load_n(&s->ring_tail, __ATOMIC_RELAXED);
  unsigned h = __atomic_load_n(&s->ring_head, __ATOMIC_ACQUIRE);
  if (t == h) return 0;
  *e = s->ring[t & (RING_N - 1)];
  __atomic_store_n(&s->ring_tail, t + 1, __ATOMIC_RELEASE);
  return 1;
}

/* modBuffer(lo, hi, centre) (app.js:143-156): 8 s at 8 kHz, random-phase sines on the 1/8 Hz grid,
 * by the same two-term recurrence, accumulated in float, normalised to rms 1 */
static void mod_buffer(float *d, double lo, double hi, double centre, dsp_rng_t *r) {
  const int sr = MOD_RATE, n = MOD_N;
  memset(d, 0, sizeof(float) * (size_t)n);
  for (int k = (int)ceil(lo * 8); k <= hi * 8; k++) {
    double f = k / 8.0, a;
    if (centre != 0) { double z = (f - centre) / (centre / 6); a = exp(-0.5 * (z * z)); }
    else a = 1 / sqrt(f);
    double ph = dsp_rng(r) * 2 * DSP_PI, w = 2 * DSP_PI * f / sr, c2 = 2 * cos(w);
    double y1 = a * sin(ph - w), y2 = a * sin(ph - 2 * w);
    for (int i = 0; i < n; i++) {
      double y = c2 * y1 - y2;
      d[i] = (float)((double)d[i] + y);
      y2 = y1; y1 = y;
    }
  }
  double sum = 0;
  for (int i = 0; i < n; i++) sum += (double)d[i] * (double)d[i];
  double g = 1 / sqrt(sum / n);
  for (int i = 0; i < n; i++) d[i] = (float)((double)d[i] * g);
}

/* ------------------------------------------------------------------------------------ lifecycle */
sfx_t *sfx_new(int rate, uint32_t seed) {
  if (rate < 8000) rate = 48000;
  sfx_t *s = calloc(1, sizeof *s);
  if (!s) return NULL;
  s->fs = rate;
  float v = 0.8f;                                    /* the browser's default level */
  memcpy(&s->vol_bits, &v, sizeof v);
  s->vol_seen = -1;
  s->tb_back = 0; s->tb_mid = 1; s->tb_front = 2;
  s->noise_n = 2 * rate;
  s->noise = malloc(sizeof(float) * (size_t)s->noise_n);
  s->slow = malloc(sizeof(float) * MOD_N);
  s->chat[0] = malloc(sizeof(float) * MOD_N);
  s->chat[1] = malloc(sizeof(float) * MOD_N);
  for (int w = 0; w < 2; w++) s->wave[w] = dsp_wave_new(NULL, WAVES[w], 7, true, s->fs);
  s->sine = dsp_wave_new_basic(DSP_WAVE_SINE, s->fs);
  s->square = dsp_wave_new_basic(DSP_WAVE_SQUARE, s->fs);
  s->triangle = dsp_wave_new_basic(DSP_WAVE_TRIANGLE, s->fs);
  if (!s->noise || !s->slow || !s->chat[0] || !s->chat[1] || !s->wave[0] || !s->wave[1] ||
      !s->sine || !s->square || !s->triangle) { sfx_free(s); return NULL; }
  /* random draws in the browser's order (startSynth): noise (2 s), slow, chat[0], chat[1] */
  dsp_rng_t rng;
  dsp_rng_seed(&rng, seed);
  dsp_noise_fill(s->noise, (size_t)s->noise_n, &rng);
  mod_buffer(s->slow, 0.5, 6, 0, &rng);
  mod_buffer(s->chat[0], 15, 60, 30, &rng);
  mod_buffer(s->chat[1], 15, 60, 30, &rng);
  /* the shaper curve: tanh(1.2x)/tanh(1.2), 257 points (computed in double, stored float) */
  for (int i = 0; i < 257; i++) {
    double x = i / 128.0 - 1;
    s->curve[i] = (float)(tanh(1.2 * x) / tanh(1.2));
  }
  s->q08 = pow(10.0, (double)0.8f / 20); s->q00 = 1.0;
  s->q05 = pow(10.0, (double)0.5f / 20); s->q10 = pow(10.0, 1.0 / 20);
  const double fs = s->fs;
  for (int p = 0; p < MP_N; p++) s->c_mp[p] = (float)dsp_tau_coef(MP_TAU[p], fs);
  for (int p = 0; p < GP_N; p++) s->c_gp[p] = (float)dsp_tau_coef(GP_TAU[p], fs);
  s->c_chat_rate = (float)dsp_tau_coef(0.1, fs / QN);
  s->c_master_stop = (float)dsp_tau_coef(0.04, fs);
  s->c_master_fast = (float)dsp_tau_coef(0.01, fs);
  s->c_out = (float)dsp_tau_coef(0.03, fs);
  s->c_bgf = (float)dsp_tau_coef(0.012, fs);
  dsp_smooth_init(&s->out, 0.64f, s->c_out);
  s->step_slow1 = dsp_loop_step(1.0f, MOD_RATE, fs);
  s->step_slow33 = dsp_loop_step(3.3f, MOD_RATE, fs);
  s->step_dv = dsp_loop_step(0.71f, fs, fs);
  s->step_ws = dsp_loop_step(0.8f, fs, fs);
  for (int m = 0; m < 4; m++) {
    motor_t *M = &s->m[m];
    M->nstep = dsp_loop_step((float)(0.9 + 0.05 * m), fs, fs);
    M->wstep = dsp_loop_step((float)(0.8 + 0.17 * m), MOD_RATE, fs);
    double gl, gr;
    dsp_pan_mono_gains(PAN[m], &gl, &gr);
    M->gl = (float)gl; M->gr = (float)gr;            /* StereoPanner without automation: float gains */
    for (int ax = 0; ax < 2; ax++) M->gax[ax] = (float)(MIX[m][ax] / sqrt(2.0));
  }
  s->c_hits = -1;
  return s;
}

void sfx_free(sfx_t *s) {
  if (!s) return;
  free(s->noise); free(s->slow); free(s->chat[0]); free(s->chat[1]);
  dsp_wave_free(s->wave[0]); dsp_wave_free(s->wave[1]);
  dsp_wave_free(s->sine); dsp_wave_free(s->square); dsp_wave_free(s->triangle);
  free(s);
}

void sfx_debug_config(sfx_t *s, int flags) { s->dbg = flags; }
unsigned sfx_debug_dropped(sfx_t *s) { return __atomic_load_n(&s->dropped, __ATOMIC_RELAXED); }

void sfx_set_volume(sfx_t *s, float vol) {
  if (!(vol > 0)) vol = 0;
  if (vol > 1) vol = 1;
  uint32_t b;
  memcpy(&b, &vol, sizeof b);
  __atomic_store_n(&s->vol_bits, b, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------- control (frame thread) */
static void publish(sfx_t *s) {
  unsigned prev = __atomic_exchange_n(&s->tb_mid, (unsigned)s->tb_back | 4u, __ATOMIC_ACQ_REL);
  s->tb_back = (int)(prev & 3u);
}

static void control_reset(sfx_t *s) {
  for (int i = 0; i < 4; i++) s->c_rpm[i] = 0;
  s->c_prev = 0; s->c_state = 0; s->c_hits = -1; s->c_venv = 0; s->c_dirty = 0;
}

void sfx_frame(sfx_t *s, const double *a, int na, double dt) {
  if (!(load_vol(s) > 0)) { s->c_alive = 0; return; }                 /* app.js:274 */
  if (!s->c_alive) {                                                  /* startSynth() */
    control_reset(s);
    s->c_alive = 1;
    sfx_event_t e = {0};
    e.type = EV_START; e.gen = ++s->c_gen;
    ring_push(s, &e);
  }
  if (!(dt >= 0)) dt = 0;
  if (dt > 250) dt = 250;
  const int literal = s->dbg & SFX_DBG_LITERAL;
  const double dts = dt < 1 ? 1 : dt;                                 /* for the per-time forms */
  const int live = a && na >= 10;
  double v[14] = {0};
  if (live) for (int i = 0; i < 14 && i < na; i++) v[i] = fin(a[i]);
  int qi = live ? to_int32(v[1]) : 1;
  if (qi < 1) qi = 1;
  if (qi > 5) qi = 5;
  const aclass_t *c = &ACLASS[qi - 1];
  double on = 0, frac = 0, speed = 0, chop, dirty = 0, shake = 0, pf = 26;
  if (live) {
    frac = clampd(v[3], 0, 1);
    speed = fabs(v[4]) + v[5];
    on = v[2] == 4 ? 1 : 0;
    dirty = clampd((-v[4] - 0.5) / 3, 0, 1) * fmax(0, 1 - v[5] / 6) * (0.5 + 0.5 * v[6]);
    shake = na >= 12 ? fmax(0, v[10]) : 0;
    pf = na >= 12 && v[11] > 0 ? v[11] : 26;
    const double k = literal ? 0.12 : 1 - pow(0.88, dt / FRAME60_MS);
    for (int i = 0; i < 4; i++) {
      double mix = MIX[i][0] * v[7] * 0.3 + MIX[i][1] * v[8] * 0.3 + MIX[i][2] * v[9] * 0.22;
      double tf = clampd(frac * (1 + mix), 0.008, 1.15);
      double rpm = c->rpm * fmax(0.1, sqrt(tf)) * (1 - 0.06 * dirty);
      s->c_rpm[i] += k * (rpm - s->c_rpm[i]);
    }
  } else {
    const double d = literal ? 0.92 : pow(0.92, dt / FRAME60_MS);
    for (int i = 0; i < 4; i++) s->c_rpm[i] *= d;
  }
  s->c_dirty += (dirty - s->c_dirty) * fmin(1, dt / 120);
  /* crash / prop strike: a thud, and the motors dip (app.js:305-322) */
  double hits = live && na >= 14 ? v[12] : -1;
  double hit = hits > s->c_hits && s->c_hits >= 0 ? fmin(1, 0.3 + v[13] / 12) : 0;
  if (live) s->c_hits = hits;
  if (live && ((s->c_state == 4 && v[2] == 5) || hit > 0)) {
    sfx_event_t e = {0};
    e.type = EV_THUD; e.gain = (float)(hit > 0 ? 0.9 * hit : 0.9);
    ring_push(s, &e);
    for (int i = 0; i < 4; i++) s->c_rpm[i] *= hit > 0 ? 1 - 0.5 * hit : 0.3;
  }
  if (live) s->c_state = v[2];
  double mean = (s->c_rpm[0] + s->c_rpm[1] + s->c_rpm[2] + s->c_rpm[3]) / 4;
  double slope = (s->c_prev - mean) / 2000;
  if (!literal) slope *= FRAME60_MS / dts;
  slope = clampd(slope, -0.5, 0.5);
  s->c_prev = mean;
  chop = fmax(0, slope);
  double swell = clampd(1 - slope, 0.6, 1.4);
  s->c_venv += (on * swell - s->c_venv) * fmin(1, dt / 90);

  targets_t *T = &s->tb[s->tb_back];
  T->gen = s->c_gen;
  T->wave = c->wave;
  T->g[GP_MASTER] = (float)(0.5 * s->c_venv);
  double dw = s->c_dirty;
  double rough = 1 + 0.8 * (1 - fmin(1, mean / c->rpm));
  T->g[GP_FLUT] = (float)(0.09 * rough * (1 + 1.6 * dw));
  double rpm_ch = fmin(0.12, 0.12 * shake);
  T->chat_rate = (float)(pf / 30);
  T->g[GP_CHAT_FM] = (float)(on * CENTS * rpm_ch);
  T->g[GP_CHAT_AM] = (float)(on * 2.5 * rpm_ch);
  double namt = c->noise * clampd(0.2 + 0.3 * frac + 0.1 * dw + speed * 0.004 + chop * 0.6, 0.08, 1.6);
  for (int i = 0; i < 4; i++) {
    float *M = T->m[i];
    double mech = s->c_rpm[i] / 60, bpf = mech * c->blades, rf = fmin(1, s->c_rpm[i] / c->rpm);
    M[MP_BUZZ_F] = (float)fmax(30, bpf);
    M[MP_BUZZ2_F] = (float)fmax(30, bpf * 1.0068);
    M[MP_SHAFT_F] = (float)fmax(20, mech);
    M[MP_WHINE_F] = (float)fmin(11000, mech * c->poles);
    M[MP_WHINE2_F] = (float)fmin(14000, mech * c->poles * 2.02);
    M[MP_WAND] = (float)(CENTS * 0.002 * (1 + 3 * dw));
    M[MP_LP_F] = (float)fmin(12000, 250 + bpf * 3);
    M[MP_NLP_F] = (float)fmin(3000, 300 + bpf * 2.2);
    M[MP_NHP_F] = (float)fmax(40, bpf * 0.3);
    M[MP_BG] = (float)(on * c->buzz * TIMBRE[i][0] * (0.12 + 0.3 * rf));
    M[MP_B2G] = (float)(on * c->buzz * TIMBRE[i][0] * 0.55 * (0.12 + 0.3 * rf));
    M[MP_SG] = (float)(on * c->shaft * (0.1 + 0.9 * rf));
    M[MP_WG] = (float)(on * c->whine * TIMBRE[i][1] * (0.2 + 0.8 * rf));
    M[MP_W2G] = (float)(on * c->whine * TIMBRE[i][1] * 0.3 * (0.2 + 0.8 * rf));
    M[MP_NG] = (float)(on * 2.5 * namt * TIMBRE[i][0]);
  }
  double dvc = 120 + 0.4 * mean / 60 * c->blades;
  T->g[GP_DVF] = (float)dvc;
  double dvn = sqrt(1.11 * dvc / (1.5 * s->fs));
  T->g[GP_DVG] = (float)(on * 0.1 * dw * (0.5 + frac) / dvn);
  T->g[GP_DVCH] = (float)fmin(0.8, 2 * shake);
  T->g[GP_WGN] = (float)(on * fmin(0.1, 0.1 * (speed / 35) * (speed / 35)));
  T->g[GP_WF] = (float)fmin(900, 150 + speed * 15);
  /* absurd game values must not reach the DSP as inf/NaN (Chromium clamps params to their ranges;
   * a buffer source's rate to +-1024) */
  for (int p = 0; p < GP_N; p++) T->g[p] = sane(T->g[p]);
  for (int i = 0; i < 4; i++) for (int p = 0; p < MP_N; p++) T->m[i][p] = sane(T->m[i][p]);
  T->chat_rate = sane(T->chat_rate);
  if (T->chat_rate > 1024) T->chat_rate = 1024;
  publish(s);
}

void sfx_tone(sfx_t *s, int freq, int len_ms, int pause_ms, int flags, int freq_incr) {
  (void)freq_incr;                                    /* the emulator ignores the sweep too */
  if (!(load_vol(s) > 0)) return;                     /* app.js:89 */
  sfx_event_t e = {0};
  e.type = EV_TONE; e.f = freq; e.dur = len_ms; e.pause = pause_ms; e.flags = flags;
  ring_push(s, &e);
}

void sfx_reset(sfx_t *s) {
  control_reset(s);
  sfx_event_t e = {0};
  e.type = EV_RESET;
  e.gen = s->c_alive ? ++s->c_gen : s->c_gen;          /* the synth goes on under a new generation */
  ring_push(s, &e);
}

/* -------------------------------------------------------------------------- audio thread: setup */
static void synth_reset(sfx_t *s) {
  const double fs = s->fs;
  for (int p = 0; p < GP_N; p++) dsp_smooth_init(&s->gp[p], GP_INIT[p], s->c_gp[p]);
  dsp_smooth_init(&s->chat_rate, 1, s->c_chat_rate);
  s->cr_pend = 0;
  s->chat_step = dsp_loop_step(1.0f, MOD_RATE, fs);
  memset(&s->flut_head, 0, sizeof s->flut_head);
  memset(&s->dvfl_head, 0, sizeof s->dvfl_head);
  memset(s->chat_head, 0, sizeof s->chat_head);
  memset(&s->dv_head, 0, sizeof s->dv_head);
  memset(&s->ws_head, 0, sizeof s->ws_head);
  dsp_biquad_reset(&s->dvf_bq);
  dsp_biquad_set_lphp(&s->dvf_bq, 0, GP_INIT[GP_DVF], s->q00, fs);
  s->dvf_at = GP_INIT[GP_DVF];
  dsp_biquad_reset(&s->wf_bq);
  dsp_biquad_set_lphp(&s->wf_bq, 0, GP_INIT[GP_WF], s->q05, fs);
  s->wf_at = GP_INIT[GP_WF];
  for (int c = 0; c < 2; c++) {
    dsp_biquad_reset(&s->shelf[c]);
    dsp_biquad_set(&s->shelf[c], DSP_HIGHSHELF, 3800, 1, -6, fs);
    dsp_shaper_init(&s->shaper[c], s->curve, 257, 2);
  }
  dsp_comp_init(&s->comp, fs, -20, 18, 5, 0.004f, 0.18f);
  for (int m = 0; m < 4; m++) {
    motor_t *M = &s->m[m];
    memset(&M->buzz, 0, sizeof M->buzz); memset(&M->buzz2, 0, sizeof M->buzz2);
    memset(&M->shaft, 0, sizeof M->shaft); memset(&M->whine, 0, sizeof M->whine);
    memset(&M->whine2, 0, sizeof M->whine2);
    for (int p = 0; p < MP_N; p++) dsp_smooth_init(&M->p[p], MP_INIT[p], s->c_mp[p]);
    dsp_biquad_reset(&M->lp); dsp_biquad_set_lphp(&M->lp, 0, MP_INIT[MP_LP_F], s->q08, fs);
    dsp_biquad_reset(&M->nlp); dsp_biquad_set_lphp(&M->nlp, 0, MP_INIT[MP_NLP_F], s->q00, fs);
    dsp_biquad_reset(&M->nhp); dsp_biquad_set_lphp(&M->nhp, 1, MP_INIT[MP_NHP_F], s->q00, fs);
    M->lp_at = MP_INIT[MP_LP_F]; M->nlp_at = MP_INIT[MP_NLP_F]; M->nhp_at = MP_INIT[MP_NHP_F];
    memset(&M->nhead, 0, sizeof M->nhead);
    memset(&M->whead, 0, sizeof M->whead);
  }
  for (int i = 0; i < MAX_THUDS; i++) s->thud[i].on = 0;
  s->wave_idx = 0;
}

/* the frame's setTargetAtTime calls, at time now/fs */
static void apply_targets(sfx_t *s, const targets_t *T) {
  if (T->wave != s->wave_idx) s->wave_idx = T->wave ? 1 : 0;   /* setPeriodicWave: phase kept */
  for (int p = 0; p < GP_N; p++) dsp_smooth_set(&s->gp[p], T->g[p]);
  for (int m = 0; m < 4; m++)
    for (int p = 0; p < MP_N; p++) dsp_smooth_set(&s->m[m].p[p], T->m[m][p]);
  /* k-rate (one value at each quantum start): an event at a quantum start makes this quantum's
   * value one more step towards the old target (zero frames of the old event, then the new one's
   * hold) and the next quantum steps towards the new one; an event inside a quantum is first seen
   * at the next quantum, past its start: a plain step towards it */
  s->cr_next = T->chat_rate;
  s->cr_pend = (s->now & (QN - 1)) ? 1 : 2;
}

/* --------------------------------------------------------------------- audio thread: voices */
static void stop_tones(sfx_t *s) {
  for (int i = 0; i < MAX_TONES; i++) s->tone[i].on = 0;
}

static void bg_tone(sfx_t *s, int freq, int dur) {               /* app.js bgTone (69-87) */
  double now = (double)s->now / s->fs;
  float f = (float)(freq > 50 ? freq : 50);
  if (!s->bg_on) {
    s->bg_on = 1;
    memset(&s->bg_osc, 0, sizeof s->bg_osc);
    dsp_smooth_init(&s->bg_freq, f, s->c_bgf);
    dsp_param_init(&s->bg_gain, 0, s->fs);
    s->bg_last = 0;
  }
  dsp_param_cancel(&s->bg_gain, now);
  dsp_param_set(&s->bg_gain, s->bg_last, now);
  if (dur <= 0) { dsp_param_target(&s->bg_gain, 0, now, 0.008f); return; }
  dsp_smooth_set(&s->bg_freq, f);
  dsp_param_target(&s->bg_gain, 0.018f, now, 0.01f);     /* [..][vol]: vol is not passed: 0.018 */
  dsp_param_target(&s->bg_gain, 0, now + dur / 1000.0, 0.01f);
}

static void start_tone(sfx_t *s, const sfx_event_t *e) {        /* app.js tone (88-106) */
  if (e->flags & SFX_PLAY_BACKGROUND) { bg_tone(s, e->f, e->dur); return; }
  const double fs = s->fs, now = (double)s->now / fs;
  if (e->flags & SFX_PLAY_NOW) { stop_tones(s); s->tone_end = now; }
  double t0 = fmax(now, s->tone_end), t1 = t0 + fmax(0.02, e->dur / 1000.0);
  s->tone_end = t1 + fmax(0, e->pause) / 1000.0;
  tone_t *v = NULL;
  for (int i = 0; i < MAX_TONES; i++) if (!s->tone[i].on) { v = &s->tone[i]; break; }
  if (!v) return;
  v->on = 1;
  v->freq = (float)(e->f > 50 ? e->f : 50);
  dsp_param_init(&v->gain, 1, fs);
  dsp_param_set(&v->gain, 0.0001f, t0);
  dsp_param_exp(&v->gain, 0.045f, t0 + 0.006);
  dsp_param_set(&v->gain, 0.045f, fmax(t0 + 0.007, t1 - 0.012));
  dsp_param_exp(&v->gain, 0.0001f, t1);
  /* AudioScheduledSourceNode: starts at the frame t0 rounds up to, its phase advanced by the
   * fraction of a frame since t0 (Chromium's sub-sample start); stops at the frame t1 + 0.02 rounds
   * up to (exclusive). Quirk kept: in the render quantum where `frequency.value` was set (now), the
   * frequency is "sample accurate" and Chromium advances the phase by 0 * fraction. */
  v->start = frame_up(t0, fs);
  v->stop = frame_up(t1 + 0.02, fs);
  double off = t0 * fs - (double)v->start;           /* start_frame_offset, usually in (-1, 0] */
  double incr = v->start / QN == s->now / QN ? 0.0 : (double)(v->freq * s->square->rate_scale);
  memset(&v->osc, 0, sizeof v->osc);
  if (off > 0) { v->start++; v->osc.idx = (1 - off) * incr; }
  else if (off < 0) v->osc.idx = -off * incr;
}

static void start_thud(sfx_t *s, float g) {                      /* app.js:310-322 */
  thud_t *v = NULL;
  for (int i = 0; i < MAX_THUDS; i++) if (!s->thud[i].on) { v = &s->thud[i]; break; }
  if (!v) {                                                      /* all busy: take the oldest */
    v = &s->thud[0];
    for (int i = 1; i < MAX_THUDS; i++) if (s->thud[i].start < v->start) v = &s->thud[i];
  }
  const double fs = s->fs, t = (double)s->now / fs;
  v->on = 1;
  v->start = s->now;
  v->stop = frame_up(t + 0.22, fs);
  v->end = v->stop + 2048;                                       /* the filter's tail */
  v->head.pos = 0;
  dsp_biquad_reset(&v->lp);
  dsp_biquad_set_lphp(&v->lp, 0, 420, s->q10, fs);
  dsp_param_init(&v->gain, 1, fs);
  dsp_param_set(&v->gain, g, t);
  dsp_param_exp(&v->gain, 0.001f, t + 0.2);
}

/* mono sum of the thud voices for frames now .. now+n-1 (n <= QN) */
static void render_thuds(sfx_t *s, float *th, int n) {
  float g[QN];
  for (int i = 0; i < n; i++) th[i] = 0;
  for (int k = 0; k < MAX_THUDS; k++) {
    thud_t *v = &s->thud[k];
    if (!v->on) continue;
    dsp_param_render(&v->gain, s->now, g, n);
    for (int i = 0; i < n; i++) {
      int64_t f = s->now + i;
      if (f < v->start) continue;
      float x = 0;
      if (f < v->stop) {
        bool done = false;
        x = dsp_oneshot_next(&v->head, s->noise, s->noise_n, 1.0, &done);
      }
      th[i] += dsp_biquad_run(&v->lp, x) * g[i];
      if (f + 1 >= v->end) { v->on = 0; break; }
    }
  }
}

/* mono sum of the beeps and the background tone (n <= QN) */
static void render_tones(sfx_t *s, float *t, int n) {
  float g[QN], fq[QN];
  for (int i = 0; i < n; i++) t[i] = 0;
  for (int k = 0; k < MAX_TONES; k++) {
    tone_t *v = &s->tone[k];
    if (!v->on) continue;
    if (v->start >= s->now + n) continue;                         /* not started yet */
    dsp_param_render(&v->gain, s->now, g, n);
    for (int i = 0; i < n; i++) {
      int64_t f = s->now + i;
      if (f < v->start) continue;
      if (f >= v->stop) { v->on = 0; break; }
      t[i] += dsp_osc_next(&v->osc, s->square, v->freq) * g[i];
    }
  }
  if (s->bg_on) {
    dsp_param_render(&s->bg_gain, s->now, g, n);
    dsp_smooth_fill(&s->bg_freq, fq, n);
    for (int i = 0; i < n; i++) t[i] += dsp_osc_next(&s->bg_osc, s->triangle, fq[i]) * g[i];
    s->bg_last = g[n - 1];
  }
}

/* ------------------------------------------------------------------ audio thread: the synth */
static inline void lphp_update(dsp_biquad_t *b, float *at, float f, int hp, double q, double fs) {
  if (f != *at) { *at = f; dsp_biquad_set_lphp(b, hp, f, q, fs); }
}

/* one segment of a render quantum: n <= QN - (now % QN) */
static void render_synth(sfx_t *s, float *ol, float *orr, int n) {
  const double fs = s->fs;
  const int ref = s->dbg & SFX_DBG_REF, raw = s->dbg & SFX_DBG_RAW;
  const dsp_wave_t *wave = s->wave[s->wave_idx], *sine = s->sine;
  const float *noise = s->noise, *slow = s->slow;
  const int nn = s->noise_n;
  float th[QN];
  if ((s->now & (QN - 1)) == 0) {                     /* k-rate: the chatter's playbackRate */
    float cr;
    if (s->cr_pend == 1) { s->chat_rate.target = s->cr_next; s->chat_rate.age = 0; s->cr_pend = 0; }
    dsp_smooth_fill(&s->chat_rate, &cr, 1);
    if (s->cr_pend == 2) { s->chat_rate.target = s->cr_next; s->chat_rate.age = 0; s->cr_pend = 0; }
    s->chat_step = dsp_loop_step(cr, MOD_RATE, fs);
  }
  for (int p = 0; p < GP_N; p++) dsp_smooth_fill(&s->gp[p], s->gbuf[p], n);
  for (int m = 0; m < 4; m++)
    for (int p = 0; p < MP_N; p++) dsp_smooth_fill(&s->m[m].p[p], s->m[m].buf[p], n);
  render_thuds(s, th, n);
  const float *g_flut = s->gbuf[GP_FLUT], *g_cfm = s->gbuf[GP_CHAT_FM], *g_cam = s->gbuf[GP_CHAT_AM];
  const float *g_dvf = s->gbuf[GP_DVF], *g_dvg = s->gbuf[GP_DVG], *g_dvch = s->gbuf[GP_DVCH];
  const float *g_wgn = s->gbuf[GP_WGN], *g_wf = s->gbuf[GP_WF], *g_master = s->gbuf[GP_MASTER];
  for (int i = 0; i < n; i++) {
    const int coef = ref || ((s->now + i) & 15) == 0;
    float slow1 = dsp_loop_next(&s->flut_head, slow, MOD_N, s->step_slow1);
    float slowdv = dsp_loop_next(&s->dvfl_head, slow, MOD_N, s->step_slow33);
    float ch0 = dsp_loop_next(&s->chat_head[0], s->chat[0], MOD_N, s->chat_step);
    float ch1 = dsp_loop_next(&s->chat_head[1], s->chat[1], MOD_N, s->chat_step);
    float fm0 = ch0 * g_cfm[i], fm1 = ch1 * g_cfm[i], am0 = ch0 * g_cam[i], am1 = ch1 * g_cam[i];
    float sl = 0, sr = 0;
    for (int m = 0; m < 4; m++) {
      motor_t *M = &s->m[m];
      float (*P)[QN] = M->buf;
      float wnd = dsp_loop_next(&M->whead, slow, MOD_N, M->wstep);
      /* detune = wander + the chatter of both axes (cents), the same for the five oscillators */
      float det = wnd * P[MP_WAND][i] + fm0 * M->gax[0] + fm1 * M->gax[1];
      float mul = exp2f(det * (1.0f / 1200.0f));
      float b = dsp_osc_next(&M->buzz, wave, P[MP_BUZZ_F][i] * mul);
      float b2 = dsp_osc_next(&M->buzz2, wave, P[MP_BUZZ2_F][i] * mul);
      float sh = dsp_osc_next(&M->shaft, sine, P[MP_SHAFT_F][i] * mul);
      float wh = dsp_osc_next(&M->whine, sine, P[MP_WHINE_F][i] * mul);
      float w2 = dsp_osc_next(&M->whine2, sine, P[MP_WHINE2_F][i] * mul);
      if (coef) {
        lphp_update(&M->lp, &M->lp_at, P[MP_LP_F][i], 0, s->q08, fs);
        lphp_update(&M->nlp, &M->nlp_at, P[MP_NLP_F][i], 0, s->q00, fs);
        lphp_update(&M->nhp, &M->nhp_at, P[MP_NHP_F][i], 1, s->q00, fs);
      }
      float ylp = dsp_biquad_run(&M->lp, b * P[MP_BG][i] + b2 * P[MP_B2G][i] + sh * P[MP_SG][i]);
      float nz = dsp_loop_next(&M->nhead, noise, nn, M->nstep);
      float yn = dsp_biquad_run(&M->nhp, dsp_biquad_run(&M->nlp, nz)) * P[MP_NG][i];
      /* amp.gain = 1 + 0.12 shaft (imbalance) + the chatter's level modulation */
      float ampg = 1.0f + sh * 0.12f + am0 * M->gax[0] + am1 * M->gax[1];
      float x = (ylp + wh * P[MP_WG][i] + w2 * P[MP_W2G][i] + yn) * ampg;
      sl += x * M->gl;
      sr += x * M->gr;
    }
    float sg = 1.0f + g_flut[i] * slow1;                       /* sum.gain = 1 + flut */
    sl *= sg; sr *= sg;
    /* dirty-air rumble into drive (mono -> both channels) */
    if (coef) lphp_update(&s->dvf_bq, &s->dvf_at, g_dvf[i], 0, s->q00, fs);
    float dv = dsp_biquad_run(&s->dvf_bq, dsp_loop_next(&s->dv_head, noise, nn, s->step_dv)) * g_dvg[i];
    dv *= 1.0f + slowdv * 0.6f + ch0 * g_dvch[i];
    float dl = sl + dv, dr = sr + dv;
    if (!raw) { dl = dsp_shaper_run(&s->shaper[0], dl); dr = dsp_shaper_run(&s->shaper[1], dr); }
    dl *= 0.62f; dr *= 0.62f;
    /* wind into master */
    if (coef) lphp_update(&s->wf_bq, &s->wf_at, g_wf[i], 0, s->q05, fs);
    float wind = dsp_biquad_run(&s->wf_bq, dsp_loop_next(&s->ws_head, noise, nn, s->step_ws)) * g_wgn[i];
    float mg = g_master[i];
    float xl = (dl + wind + th[i]) * mg, xr = (dr + wind + th[i]) * mg;
    if (!raw) { xl = dsp_biquad_run(&s->shelf[0], xl); xr = dsp_biquad_run(&s->shelf[1], xr); }
    ol[i] = xl; orr[i] = xr;
  }
  if (!raw) dsp_comp_process(&s->comp, ol, orr, n);
  if (((s->now + n) & (QN - 1)) == 0) {                    /* quantum end: flush denormal state */
    for (int m = 0; m < 4; m++) {
      dsp_biquad_flush(&s->m[m].lp); dsp_biquad_flush(&s->m[m].nlp); dsp_biquad_flush(&s->m[m].nhp);
    }
    dsp_biquad_flush(&s->dvf_bq); dsp_biquad_flush(&s->wf_bq);
    dsp_biquad_flush(&s->shelf[0]); dsp_biquad_flush(&s->shelf[1]);
  }
}

/* ---------------------------------------------------------------------- audio thread: entry */
static void synth_start(sfx_t *s, uint32_t gen) {
  synth_reset(s);
  s->active = 1; s->stopping = 0; s->restart = 0; s->gen = gen;
}

static void synth_stop(sfx_t *s) {                   /* stopSynth(): fade, gone after 0.3 s */
  s->stopping = 1;
  s->stop_at = s->now + (int64_t)(0.3 * s->fs);
  dsp_smooth_coef(&s->gp[GP_MASTER], s->c_master_stop);
  dsp_smooth_set(&s->gp[GP_MASTER], 0);
}

static void drain(sfx_t *s) {
  sfx_event_t e;
  while (ring_pop(s, &e)) {
    switch (e.type) {
      case EV_TONE: start_tone(s, &e); break;
      case EV_THUD: if (s->active && !s->stopping) start_thud(s, e.gain); break;
      case EV_START:
        if (s->active) {                               /* the old one fades out first */
          if (!s->stopping) synth_stop(s);
          s->restart = 1; s->restart_gen = e.gen;
        } else synth_start(s, e.gen);
        break;
      case EV_RESET:
        stop_tones(s);
        s->tone_end = (double)s->now / s->fs;
        if (s->bg_on) bg_tone(s, 0, 0);
        if (s->restart) s->restart_gen = e.gen;
        if (s->active && !s->stopping) {
          s->gen = e.gen;                              /* targets from before the reset: stale */
          for (int i = 0; i < MAX_THUDS; i++) s->thud[i].on = 0;
          s->fast_until = s->now + (int64_t)(0.05 * s->fs);
          dsp_smooth_coef(&s->gp[GP_MASTER], s->c_master_fast);
          dsp_smooth_set(&s->gp[GP_MASTER], 0);
        }
        break;
      default: break;
    }
  }
}

void sfx_render(sfx_t *s, float *L, float *R, int n) {
  if (n <= 0) return;
  drain(s);
  /* volume: gain vol^2, tau 0.03 s; at 0 the synth fades out (tau 0.04) and stops after 0.3 s */
  float v = load_vol(s);
  if (v != s->vol_seen) {
    if (s->vol_seen < 0) dsp_smooth_jump(&s->out, v * v);
    else dsp_smooth_set(&s->out, v * v);
    if (!(v > 0)) {
      if (s->active && !s->stopping) synth_stop(s);
      if (s->bg_on) bg_tone(s, 0, 0);
    }
    s->vol_seen = v;
  }
  /* newest frame targets */
  if (__atomic_load_n(&s->tb_mid, __ATOMIC_ACQUIRE) & 4u) {
    unsigned prev = __atomic_exchange_n(&s->tb_mid, (unsigned)s->tb_front, __ATOMIC_ACQ_REL);
    s->tb_front = (int)(prev & 3u);
    const targets_t *T = &s->tb[s->tb_front];
    if (s->active && !s->stopping && T->gen == s->gen) apply_targets(s, T);
  }
  float ml[QN], mr[QN], tl[QN];
  int off = 0;
  while (off < n) {
    int m = QN - (int)(s->now & (QN - 1));             /* to the end of this render quantum */
    if (m > n - off) m = n - off;
    if (s->stopping && s->now >= s->stop_at) {
      s->active = 0; s->stopping = 0;
      if (s->restart) synth_start(s, s->restart_gen);  /* the next frame's targets drive it */
    }
    if (s->fast_until && s->now >= s->fast_until) {
      s->fast_until = 0;
      if (!s->stopping) dsp_smooth_coef(&s->gp[GP_MASTER], s->c_gp[GP_MASTER]);
    }
    if (s->active) {
      render_synth(s, ml, mr, m);
      float chk = 0;
      for (int i = 0; i < m; i++) chk += ml[i] + mr[i];
      if (!isfinite(chk)) {                            /* never expected: start the DSP afresh */
        synth_reset(s);
        for (int i = 0; i < m; i++) ml[i] = mr[i] = 0;
      }
    } else for (int i = 0; i < m; i++) ml[i] = mr[i] = 0;
    render_tones(s, tl, m);
    dsp_smooth_fill(&s->out, s->obuf, m);
    for (int i = 0; i < m; i++) {
      float o = s->obuf[i];
      L[off + i] += (ml[i] + tl[i]) * o;
      R[off + i] += (mr[i] + tl[i]) * o;
    }
    s->now += m;
    off += m;
  }
}
