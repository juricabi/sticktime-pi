/* dsp.h: WebAudio-like building blocks with Chromium's exact semantics, used by the motor synth
 * (sfx.c) and available to the music. Notes: emulator-features.md §3.3, audio-motor.md §4,
 * audio-music.md §6. Arithmetic follows Chromium's (float vs double, operation order, its x86
 * loops), so renders can match the browser's sample by sample; dsp.c is compiled without fused
 * multiply-adds (clang: FP_CONTRACT OFF) for the same reason.
 *
 * Everything here is float/double DSP on plain structs. Rules:
 * - Functions that allocate say so (dsp_wave_new, dsp_delay_init): call them at init, never in the
 *   audio thread. Everything else is allocation-free, lock-free and does no I/O.
 * - Deterministic: the only randomness is dsp_rng_t (mulberry32, the reference harness's stream).
 * - Times are in samples or seconds as noted; `rate` is the output sample rate (e.g. 48000).
 *
 * Contents:
 *   dsp_rng_t        mulberry32 PRNG (== the harness's Math.random), noise fill
 *   dsp_smooth_t     AudioParam.setTargetAtTime re-issued per frame (Chromium's per-quantum form)
 *   dsp_param_t      AudioParam timeline: setValue / linearRamp / exponentialRamp / setTarget / cancel
 *   dsp_wave_t       PeriodicWave / OscillatorNode tables (sine, square, sawtooth, triangle, custom):
 *   dsp_osc_t          Chromium's 36 band-limited tables per wave, peak-normalised, crossfaded
 *   dsp_biquad_t     BiquadFilterNode (all 8 types; Q in dB for lowpass/highpass, linear for the rest)
 *   dsp_loop_t       AudioBufferSourceNode read head (loop or one-shot, playbackRate, linear interp)
 *   dsp_pan_*        StereoPannerNode (mono and stereo input)
 *   dsp_shaper_t     WaveShaperNode (curve lookup, 'none' or Chromium's '2x' up/down sampler kernels)
 *   dsp_comp_t       DynamicsCompressorNode (literal port of Chromium's dynamics_compressor.cc)
 *   dsp_delay_t      DelayNode line (fractional delay, linear interpolation)
 */
#ifndef DSP_H
#define DSP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <math.h>

#define DSP_PI 3.14159265358979323846
#define DSP_RENDER_QUANTUM 128          /* WebAudio's block size (k-rate params update per block) */

static inline float dsp_db2lin(float db) { return powf(10.0f, 0.05f * db); }
static inline float dsp_lin2db(float x) { return 20.0f * log10f(x); }

/* ------------------------------------------------------------------------------------------------
 * Random: mulberry32, bit-identical to the reference harness's Math.random replacement:
 *   s = (s + 0x6D2B79F5)|0; t = imul(s ^ s>>>15, 1|s); t = (t + imul(t ^ t>>>7, 61|t)) ^ t;
 *   return ((t ^ t>>>14) >>> 0) / 2^32
 */
typedef struct { uint32_t s; } dsp_rng_t;
static inline void dsp_rng_seed(dsp_rng_t *r, uint32_t seed) { r->s = seed; }
static inline double dsp_rng(dsp_rng_t *r) {             /* uniform [0, 1) */
  uint32_t t = (r->s += 0x6D2B79F5u);
  t = (t ^ (t >> 15)) * (t | 1u);
  t ^= t + (t ^ (t >> 7)) * (t | 61u);
  return (double)(t ^ (t >> 14)) / 4294967296.0;
}
/* d[i] = (float)(rnd*2 - 1) for i in 0..n-1: the browser's white-noise buffers, same draw order */
void dsp_noise_fill(float *d, size_t n, dsp_rng_t *r);

/* ------------------------------------------------------------------------------------------------
 * AudioParam.setTargetAtTime(T, now, tau), re-issued every control frame, rendered the way Chromium
 * does (audio_param_handler.cc ProcessSetTarget, x86 build), block by block:
 *   c = 1 - exp(-1 / (rate * tau))   (float)
 *   first sample of a new event (dsp_smooth_set): Chromium still processes the previous event for
 *   zero frames there, which steps the carried value once towards the OLD target, and the new
 *   event's start (exp(0)) holds that value: out = v + (T_old - v) c, then on towards T;
 *   first sample of any other render-quantum segment: v += (T - v) * c;
 *   then, if converged (|T - v| < 4.54e-5 |v|, or |v| < 4.54e-5 for T = 0, or more than 10 tau
 *   since the event start) the segment is T; else the recursion, unrolled by 4 as Chromium's SSE
 *   loop: out = v, v + d c0, v + d c1, v + d c2 with d = T - v, c1 = c(2-c), c2 = c((c-3)c+3),
 *   then v += d c3 (c3 = c(c((4-c)c-6)+4)); a remainder of < 4 samples steps one by one.
 * Both details matter for sample-exact renders: in float a plain v += (T - v)c stalls about
 * ulp(v)/(2c) short of T, outside the snap threshold for audio-rate frequencies, and a hold instead
 * of the old-target step shifts every frequency trajectory: either way the oscillators drift in
 * phase against the browser (verified against Chromium renders: residual -98 dB vs -30 dB).
 * dsp_smooth_fill() renders one segment (a whole 128-frame quantum, or the part of one before/after
 * a new event); k-rate params: rate = fs/128 and n = 1 per quantum.
 * dsp_smooth_next() is a plain per-sample recursion (with a hold) for uses that need no fidelity.
 * Usage: dsp_smooth_init(&p, initial, dsp_tau_coef(0.05, fs)); per frame dsp_smooth_set(&p, T);
 *        per segment dsp_smooth_fill(&p, buf, n).
 */
typedef struct {
  float v, target, c, conv, old_target;
  int pend;                             /* a new event starts at the next sample */
  int32_t age;                          /* steps since the current event started */
} dsp_smooth_t;
double dsp_tau_coef(double tau_s, double rate);          /* 1 - exp(-1/(rate*tau)); tau <= 0 -> 1 */
/* c changes the time constant (keeps value and target); conv = 10 tau in steps of the recursion */
void dsp_smooth_coef(dsp_smooth_t *s, float c);
static inline void dsp_smooth_init(dsp_smooth_t *s, float v, float c) {
  s->v = v; s->target = v; s->old_target = v; s->pend = 0; s->age = 0;
  dsp_smooth_coef(s, c);
}
/* setTargetAtTime(target, now): starts at the next rendered sample (a second call before that
 * replaces the first, as an event at the same time does) */
static inline void dsp_smooth_set(dsp_smooth_t *s, float target) {
  if (!s->pend) s->old_target = s->target;
  s->target = target; s->pend = 1;
}
static inline void dsp_smooth_jump(dsp_smooth_t *s, float v) {
  s->v = v; s->target = v; s->old_target = v; s->pend = 0; s->age = 0;
}
void dsp_smooth_fill(dsp_smooth_t *s, float *out, int n);
static inline float dsp_smooth_next(dsp_smooth_t *s) {
  if (s->pend) s->pend = 0;
  else s->v += (s->target - s->v) * s->c;
  return s->v;
}

/* ------------------------------------------------------------------------------------------------
 * AudioParam timeline (for envelopes and one-shot automation; music voices, beeps, thuds).
 * Events are kept sorted by time (seconds). Semantics follow the WebAudio spec / Chromium:
 *   SET(v, t)      value jumps to v at t
 *   LIN(v, t)      linear ramp from the previous event (time, value) to (t, v)
 *   EXP(v, t)      exponential ramp likewise (v0*(v1/v0)^x; holds v0 if v0, v1 differ in sign or are 0)
 *   TARGET(v,t,tau) from t: v + (v(t) - v) * exp(-(t' - t)/tau), forever (until the next event)
 *   cancel(t)      removes every event with time >= t (a ramp's time is its end time)
 * A ramp scheduled after a running TARGET starts from the TARGET curve's value at max(its start,
 * the param's current time) (Chromium inserts a SET there). Before the first event: `value`.
 * A sample n has time n / rate; as in Chromium an event at time t takes effect at frame
 * ceil(t * rate) (samples with n >= t * rate). Exponential ramps restart per rendered block from
 * v1 (v2/v1)^((t - t1)/(t2 - t1)) and multiply per sample, like Chromium's per-quantum loop.
 * Evaluation: dsp_param_render() fills per-sample values for consecutive blocks (incremental,
 * exact at block starts); dsp_param_at() evaluates one time (exact, slower). Past events are
 * folded into a base segment as rendering advances, so the list stays short.
 */
enum { DSP_EV_SET, DSP_EV_LIN, DSP_EV_EXP, DSP_EV_TARGET };
#define DSP_PARAM_EVENTS 16
typedef struct { uint8_t type; double t; float v; float tau; } dsp_event_t;
typedef struct {
  double rate;
  float value;                          /* intrinsic value (before any event) */
  /* base segment: what the events already passed produce */
  int base_kind;                        /* 0 hold, 1 target */
  double base_t;                        /* start time of the base segment */
  double base_v;                        /* value at base_t */
  float base_target, base_tau;
  double now;                           /* time of the last rendered sample (for ramp-after-target) */
  int n;
  dsp_event_t ev[DSP_PARAM_EVENTS];
} dsp_param_t;
void  dsp_param_init(dsp_param_t *p, float value, double rate);
bool  dsp_param_set(dsp_param_t *p, float v, double t);                 /* setValueAtTime */
bool  dsp_param_lin(dsp_param_t *p, float v, double t);                 /* linearRampToValueAtTime */
bool  dsp_param_exp(dsp_param_t *p, float v, double t);                 /* exponentialRampToValueAtTime */
bool  dsp_param_target(dsp_param_t *p, float v, double t, float tau);   /* setTargetAtTime */
void  dsp_param_cancel(dsp_param_t *p, double t);                       /* cancelScheduledValues */
float dsp_param_at(const dsp_param_t *p, double t);                     /* value at time t */
/* values for samples frame0 .. frame0+n-1 (call with increasing frame0). The schedulers return
 * false when the event list is full (the event is dropped). */
void  dsp_param_render(dsp_param_t *p, int64_t frame0, float *out, int n);
/* true when no event lies at or after t and the value is constant from t on (voice can stop) */
bool  dsp_param_idle(const dsp_param_t *p, double t);

/* ------------------------------------------------------------------------------------------------
 * Oscillators: Chromium builds every OscillatorNode type (and createPeriodicWave) as a set of
 * band-limited wavetables: table size S = 2048 (rate <= 24 kHz), 4096 (<= 88.2 kHz) or 16384,
 * R = round(3*log2(S)) ranges (36 for 4096); range r keeps partials k <= P(r) = (unsigned)(S/2 *
 * 2^(-r/3)); all tables are scaled by 1/peak(table 0) (normalisation on). For a fundamental f:
 *   pr = clamp(1 + 3*log2(f / (rate/S)), 0, R-1); r1 = floor(pr); r2 = min(r1+1, R-1); t = pr - r1
 *   out = (1-t)*lerp(tab[r1], idx) + t*lerp(tab[r2], idx)
 * Phase idx is a double in table samples [0, S), +f*S/rate per sample, starting at 0.
 * Normalisation constants: square 0.848191, sawtooth 0.848, triangle 1.000198, sine 1,
 * the motor's W0 1.010911, W1 0.437348 (dsp_wave_norm()). Saw: 0 at phase 0, rising; square: +
 * on the first half; triangle: 0 rising to 1 at a quarter. Tables with identical partial content
 * are shared (a 7-term wave costs ~7 tables, sine 1).
 */
enum { DSP_WAVE_SINE, DSP_WAVE_SQUARE, DSP_WAVE_SAWTOOTH, DSP_WAVE_TRIANGLE };
#define DSP_WAVE_MAX_RANGES 44
typedef struct dsp_wave {
  int size, mask, nranges;
  float rate_scale;                      /* size / rate (float, as Chromium) */
  float inv_rate_scale;
  float lowest;                          /* lowest fundamental: rate / size */
  float nyquist;
  float simple_incr;                     /* |incr| below this reads tab[0] only (r1, r2 both full) */
  float norm;                            /* normalisation scale applied (1/peak of table 0) */
  double inv_size;                       /* 1 / size */
  const float *tab[DSP_WAVE_MAX_RANGES]; /* each has size + 1 floats (tab[size] = tab[0]) */
  float *mem;
} dsp_wave_t;
/* allocate + build (init only; ~1 ms per wave). NULL on out of memory. */
dsp_wave_t *dsp_wave_new_basic(int type, double rate);
/* createPeriodicWave(real, imag) with n coefficients (index 0 = DC, ignored) */
dsp_wave_t *dsp_wave_new(const float *real, const float *imag, int n, bool normalize, double rate);
void dsp_wave_free(dsp_wave_t *w);
static inline float dsp_wave_norm(const dsp_wave_t *w) { return w->norm; }

typedef struct { double idx; } dsp_osc_t;                   /* reset = {0}: phase 0 */
/* the two tables and crossfade for a phase increment (table samples per output sample) */
static inline void dsp_wave_select(const dsp_wave_t *w, float incr, const float **hi,
                                   const float **lo, float *t) {
  incr = fabsf(incr);
  if (incr < w->simple_incr) { *hi = *lo = w->tab[0]; *t = 0.0f; return; }
  float f = w->inv_rate_scale * incr;
  float ratio = f > 0.0f ? f / w->lowest : 0.5f;
  float pr = 1.0f + log2f(ratio) * 1200.0f / 400.0f;
  if (pr < 0.0f) pr = 0.0f;
  if (pr > (float)(w->nranges - 1)) pr = (float)(w->nranges - 1);
  unsigned r1 = (unsigned)pr, r2 = r1 < (unsigned)w->nranges - 1 ? r1 + 1 : r1;
  *hi = w->tab[r1]; *lo = w->tab[r2]; *t = pr - (float)r1;
}
/* one sample at frequency f (Hz, detune already applied: f * exp2f(cents/1200)) */
static inline float dsp_osc_next(dsp_osc_t *o, const dsp_wave_t *w, float f) {
  if (!(f <= w->nyquist)) f = w->nyquist;             /* (NaN too, as Chromium's ClampFrequency) */
  else if (f < -w->nyquist) f = -w->nyquist;
  float incr = f * w->rate_scale;
  const float *hi, *lo; float tf;
  dsp_wave_select(w, incr, &hi, &lo, &tf);
  double vi = o->idx;
  unsigned i1 = (unsigned)vi;
  float fr = (float)vi - (float)i1;                   /* (Chromium's arithmetic, float index) */
  i1 &= (unsigned)w->mask;
  unsigned i2 = i1 + 1;                               /* tables have a guard sample at [size] */
  float sh = hi[i1] + fr * (hi[i2] - hi[i1]);
  float s = sh;
  if (lo != hi) { float sl = lo[i1] + fr * (lo[i2] - lo[i1]); s = sh + tf * (sl - sh); }
  vi += incr;
  vi -= floor(vi * w->inv_size) * (double)w->size;   /* (1/size: exact for a power of two) */
  o->idx = vi;
  return s;
}

/* ------------------------------------------------------------------------------------------------
 * BiquadFilterNode (Chromium biquad.cc = RBJ cookbook). freq in Hz (normalised by rate/2 and clamped
 * to [0, 1] as Chromium does, including its edge cases), q: in dB for LOWPASS/HIGHPASS
 * (Qlin = 10^(q/20)), linear for BANDPASS/PEAKING/NOTCH/ALLPASS, ignored by the shelves (slope 1);
 * gain_db for the shelves and PEAKING. Direct Form I, double coefficients and state, float out.
 * Recompute with dsp_biquad_set whenever a parameter moves (Chromium: per sample while automated).
 */
enum { DSP_LOWPASS, DSP_HIGHPASS, DSP_BANDPASS, DSP_LOWSHELF, DSP_HIGHSHELF, DSP_PEAKING, DSP_NOTCH,
       DSP_ALLPASS };
typedef struct { double b0, b1, b2, a1, a2; double x1, x2, y1, y2; } dsp_biquad_t;
void dsp_biquad_reset(dsp_biquad_t *b);                     /* coefficients pass-through, state 0 */
void dsp_biquad_set(dsp_biquad_t *b, int type, double freq, double q, double gain_db, double rate);
/* the same with precomputed Qlin = 10^(q/20) (LOWPASS/HIGHPASS only; skips a pow per update) */
void dsp_biquad_set_lphp(dsp_biquad_t *b, int highpass, double freq, double qlin, double rate);
static inline float dsp_biquad_run(dsp_biquad_t *b, float x) {
  /* as Chromium: double coefficients and state, but y is rounded to float before it is stored */
  float y = (float)(b->b0 * x + b->b1 * b->x1 + b->b2 * b->x2 - b->a1 * b->y1 - b->a2 * b->y2);
  b->x2 = b->x1; b->x1 = x; b->y2 = b->y1; b->y1 = y;
  return y;
}
/* flush tiny state (call now and then on idle filters; Chromium does this per quantum) */
void dsp_biquad_flush(dsp_biquad_t *b);
/* |H(e^jw)| at freq (tests) */
double dsp_biquad_mag(const dsp_biquad_t *b, double freq, double rate);

/* ------------------------------------------------------------------------------------------------
 * AudioBufferSourceNode read head: pos (double, in buffer samples) starts at the start offset;
 * per output sample: out = lerp(buf[i], buf[i+1]) with i+1 wrapping to 0 when looping, then
 * pos += step, step = playbackRate * bufferRate / rate (playbackRate as float, like Chromium).
 */
typedef struct { double pos; } dsp_loop_t;
static inline double dsp_loop_step(float playback_rate, double buf_rate, double rate) {
  return (double)playback_rate * (buf_rate / rate);
}
static inline float dsp_loop_next(dsp_loop_t *r, const float *buf, int n, double step) {
  double p = r->pos;
  if (!(p >= 0.0 && p < (double)n)) p = 0.0;          /* NaN or a step >= n: restart, never overrun */
  unsigned i = (unsigned)p;
  double f = p - (double)i;
  unsigned i2 = i + 1 >= (unsigned)n ? 0 : i + 1;
  float y = (float)((1.0 - f) * buf[i] + f * buf[i2]);
  p += step;
  if (p >= (double)n) p -= (double)n;
  r->pos = p;
  return y;
}
/* not looping: 0 once pos reaches n (*done set); the last sample interpolates towards itself */
static inline float dsp_oneshot_next(dsp_loop_t *r, const float *buf, int n, double step, bool *done) {
  if (!(r->pos >= 0 && r->pos < (double)n)) { *done = true; return 0.0f; }
  unsigned i = (unsigned)r->pos;
  double f = r->pos - (double)i;
  unsigned i2 = i + 1 >= (unsigned)n ? i : i + 1;
  float y = (float)((1.0 - f) * buf[i] + f * buf[i2]);
  r->pos += step;
  return y;
}

/* ------------------------------------------------------------------------------------------------
 * StereoPannerNode (Chromium stereo_panner.cc), pan clamped to [-1, 1].
 * mono in:   a = (pan*0.5 + 0.5)*pi/2; L = x cos a, R = x sin a   (pan 0: 0.7071 each)
 * stereo in: pan <= 0: a = (pan+1)*pi/2; L = l + r cos a, R = r sin a
 *            pan >  0: a = pan*pi/2;     L = l cos a,     R = r + l sin a
 */
void dsp_pan_mono_gains(float pan, double *gl, double *gr);
static inline void dsp_pan_mono(float pan, float x, float *l, float *r) {
  double gl, gr; dsp_pan_mono_gains(pan, &gl, &gr);
  *l = (float)(x * gl); *r = (float)(x * gr);
}
void dsp_pan_stereo(float pan, float inl, float inr, float *l, float *r);

/* ------------------------------------------------------------------------------------------------
 * WaveShaperNode. Curve lookup (Chromium): v = 0.5*(x+1)*(n-1); v < 0 -> curve[0]; v >= n-1 ->
 * curve[n-1]; else linear interpolation (double). oversample 1 = 'none'; 2 = '2x' with Chromium's
 * UpSampler (128-tap Blackman(0.16)-windowed sinc, -0.5 offset) and DownSampler (256-tap half-band,
 * 128 odd taps + the 0.5 centre tap): 128 samples of latency, like the browser (not compensated).
 * dsp_shaper_tanh_curve builds the motor's curve: c[i] = tanh(drive*x)/tanh(drive), x = 2i/(n-1)-1.
 */
#define DSP_SHAPER_CURVE_MAX 1025
#define DSP_UP_TAPS 128
#define DSP_DOWN_TAPS 128                   /* the reduced (odd-term) half of the 256-tap kernel */
typedef struct {
  float curve[DSP_SHAPER_CURVE_MAX];
  int ncurve, oversample;
  int pos;                                  /* history write index */
  float up[DSP_UP_TAPS], down[DSP_DOWN_TAPS];   /* Chromium's kernels (built by dsp_shaper_init) */
  float upr[DSP_UP_TAPS], downr[DSP_DOWN_TAPS]; /* the same reversed (forward dot products) */
  float xh[2 * 256];                        /* input history (doubled ring for contiguous reads) */
  float oh[2 * 256];                        /* shaped odd 2x samples history */
} dsp_shaper_t;
void  dsp_shaper_tanh_curve(float *curve, int n, double drive);
void  dsp_shaper_init(dsp_shaper_t *s, const float *curve, int n, int oversample);
void  dsp_shaper_reset(dsp_shaper_t *s);    /* history 0, keeps curve */
float dsp_shaper_curve(const float *curve, int n, float x);
float dsp_shaper_run(dsp_shaper_t *s, float x);   /* one sample in, one out (oversample 1 or 2) */

/* ------------------------------------------------------------------------------------------------
 * DynamicsCompressorNode: literal port of Chromium's dynamics_compressor.cc (stereo-linked peak
 * detector, 6 ms look-ahead pre-delay = (unsigned)(0.006f*rate) frames, soft knee curve with the
 * k found by bisection, adaptive release polynomial, 32-frame divisions aligned to the first
 * processed sample, automatic makeup gain (1/saturate(1))^0.6). State starts like Chromium:
 * detector 0, gain 1. Motor: (-20, 18, 5, 0.004, 0.18) -> makeup 1.78381 (+5.03 dB);
 * music: (-16, 10, 3, 0.01, 0.2) -> 1.67518 (+4.48 dB).
 */
#define DSP_COMP_MAX_PREDELAY 1024
typedef struct {
  float rate, thr, knee, ratio, attack, release;
  float k, lin_thr, knee_thr_db, knee_thr, y_knee_db, slope, makeup;
  float attack_frames, sat_release_frames, ka, kb, kc, kd, ke;
  float det, gain, max_att_db, env_rate, scaled_desired;
  int phase, pre, ri, wi;
  float buf[2][DSP_COMP_MAX_PREDELAY];
} dsp_comp_t;
void  dsp_comp_init(dsp_comp_t *c, double rate, float thr_db, float knee_db, float ratio,
                    float attack_s, float release_s);
void  dsp_comp_reset(dsp_comp_t *c);                      /* state as a fresh node, same settings */
float dsp_comp_curve(const dsp_comp_t *c, float x);       /* static curve saturate(|x|), no makeup */
void  dsp_comp_process(dsp_comp_t *c, float *l, float *r, int n);   /* in place, stereo */
/* current gain applied (makeup * sin(pi/2 * gain)), for metering/tests */
static inline float dsp_comp_total_gain(const dsp_comp_t *c) {
  return c->makeup * (float)sin((double)(1.5707963267948966f * c->gain));
}

/* ------------------------------------------------------------------------------------------------
 * Delay line (DelayNode): write the current input, then read y = x[n - d] with linear
 * interpolation (d in samples, 0 <= d <= max_frames). The buffer is allocated by dsp_delay_init.
 */
typedef struct { float *buf; int size, mask, w; } dsp_delay_t;
int  dsp_delay_init(dsp_delay_t *d, int max_frames);      /* 0 ok, -1 out of memory (init only) */
void dsp_delay_free(dsp_delay_t *d);
void dsp_delay_clear(dsp_delay_t *d);
static inline void dsp_delay_write(dsp_delay_t *d, float x) {
  d->w = (d->w + 1) & d->mask; d->buf[d->w] = x;
}
static inline float dsp_delay_read(const dsp_delay_t *d, double delay) {
  if (delay < 0) delay = 0;
  if (delay > (double)(d->size - 2)) delay = (double)(d->size - 2);
  int id = (int)delay;
  float f = (float)(delay - id);
  float a = d->buf[(d->w - id) & d->mask], b = d->buf[(d->w - id - 1) & d->mask];
  return a + (b - a) * f;
}

#endif
