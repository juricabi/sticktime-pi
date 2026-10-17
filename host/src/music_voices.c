/* The generated music's instruments (notes audio-music.md section 7, app.js:519-624), re-expressed
 * as voices rendered per sample: AudioParam timelines with Web Audio semantics, Chromium's
 * band-limited oscillators (dsp.h PeriodicWave tables), biquads as Chromium computes them (dsp.h),
 * the noise buffer source (non-looping hits stop at the buffer's end), equal-power panners (dsp.h).
 * Every instrument draws its random numbers in the browser's order (the event log and the songs
 * of a seed match app.js exactly). Voices keep absolute start/stop times; the pool is fixed. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "songs.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ================================================================ AudioParam timeline */

void mparam_init(mparam_t *p, double def) { p->def = def; p->n = 0; }

static void mp_insert(mparam_t *p, int type, double v, double t) {
  int i;
  for (i = 0; i < p->n; i++) {
    if (p->ev[i].t == t && p->ev[i].type == type) { p->ev[i].v = v; return; }  /* Chromium: replace */
    if (p->ev[i].t > t) break;
  }
  if (p->n == MP_MAXEV) {                     /* never with the voices here; keep the newest */
    if (i == 0) return;
    memmove(&p->ev[0], &p->ev[1], (size_t)(p->n - 1) * sizeof p->ev[0]);
    p->n--; i--;
  }
  memmove(&p->ev[i + 1], &p->ev[i], (size_t)(p->n - i) * sizeof p->ev[0]);
  p->ev[i].t = t; p->ev[i].v = v; p->ev[i].type = type;
  p->n++;
}
void mparam_set(mparam_t *p, double v, double t) { mp_insert(p, MP_SET, v, t); }
void mparam_lin(mparam_t *p, double v, double t) { mp_insert(p, MP_LIN, v, t); }
void mparam_exp(mparam_t *p, double v, double t) { mp_insert(p, MP_EXP, v, t); }

void mparam_cancel(mparam_t *p, double t) {
  int n = 0;
  for (int i = 0; i < p->n; i++) if (p->ev[i].t < t) p->ev[n++] = p->ev[i];
  p->n = n;
}

void mparam_cancel_keep_ending(mparam_t *p, double t) {
  int n = 0;
  for (int i = 0; i < p->n; i++) {
    mpev_t e = p->ev[i];
    if (e.type != MP_SET && fabs(e.t - t) < 1e-6) { e.t = t; p->ev[n++] = e; }
    else if (e.t < t) p->ev[n++] = e;
  }
  p->n = n;
}

/* value at tau given j = index of the last event at or before tau (-1: none) */
static double mp_seg_value(const mparam_t *p, int j, double tau) {
  if (j < 0) return p->def;
  if (j + 1 < p->n && p->ev[j + 1].type != MP_SET) {
    double t0 = p->ev[j].t, v0 = p->ev[j].v, t1 = p->ev[j + 1].t, v1 = p->ev[j + 1].v;
    double x = (tau - t0) / (t1 - t0);
    if (p->ev[j + 1].type == MP_LIN) return v0 + (v1 - v0) * x;
    if (v0 * v1 <= 0) return v0;              /* the spec: an invalid exponential ramp holds */
    return v0 * pow(v1 / v0, x);
  }
  return p->ev[j].v;
}

static int mp_find(const mparam_t *p, double tau) {
  int j = -1;
  for (int i = 0; i < p->n && p->ev[i].t <= tau; i++) j = i;
  return j;
}

double mparam_value(const mparam_t *p, double tau) { return mp_seg_value(p, mp_find(p, tau), tau); }

/* the first frame whose time k / sr is >= t */
int64_t mframe_at(double t, double sr) {
  int64_t k = (int64_t)ceil(t * sr);
  while ((double)(k - 1) / sr >= t) k--;
  while ((double)k / sr < t) k++;
  return k;
}

static void mp_prune(mparam_t *p, double tau0) {
  int k = mp_find(p, tau0);
  if (k < 0) return;
  if (k > 0) { memmove(&p->ev[0], &p->ev[k], (size_t)(p->n - k) * sizeof p->ev[0]); p->n -= k; }
  p->ev[0].type = MP_SET;                     /* a finished ramp holds its end value */
}

void mparam_fill(mparam_t *p, int64_t k0, int n, double sr, double *out) {
  mp_prune(p, (double)k0 / sr);
  int i = 0;
  while (i < n) {
    double tau = (double)(k0 + i) / sr;
    int j = mp_find(p, tau);
    int m = n - i;
    if (j + 1 < p->n) {
      int64_t kb = mframe_at(p->ev[j + 1].t, sr);
      if (kb - (k0 + i) < m) m = (int)(kb - (k0 + i));
      if (m < 1) m = 1;
    }
    if (j >= 0 && j + 1 < p->n && p->ev[j + 1].type == MP_EXP) {
      double t0 = p->ev[j].t, v0 = p->ev[j].v, t1 = p->ev[j + 1].t, v1 = p->ev[j + 1].v;
      if (v0 * v1 > 0) {
        double r = v1 / v0, v = v0 * pow(r, (tau - t0) / (t1 - t0)), st = pow(r, 1.0 / ((t1 - t0) * sr));
        for (int q = 0; q < m; q++) { out[i + q] = v; v *= st; }
      } else {
        for (int q = 0; q < m; q++) out[i + q] = v0;
      }
    } else if (j >= 0 && j + 1 < p->n && p->ev[j + 1].type == MP_LIN) {
      for (int q = 0; q < m; q++) out[i + q] = mp_seg_value(p, j, (double)(k0 + i + q) / sr);
    } else {
      double v = j < 0 ? p->def : p->ev[j].v;
      for (int q = 0; q < m; q++) out[i + q] = v;
    }
    i += m;
  }
}

bool mparam_constant(const mparam_t *p, int64_t k0, int n, double sr, double *v) {
  double tau = (double)k0 / sr, tend = (double)(k0 + n - 1) / sr;
  int j = mp_find(p, tau);
  if (j + 1 < p->n) {
    if (p->ev[j + 1].type != MP_SET) return false;   /* inside (or before) a ramp */
    if (p->ev[j + 1].t <= tend) return false;        /* a jump inside the block */
  }
  *v = j < 0 ? p->def : p->ev[j].v;
  return true;
}

/* ================================================================ voices */

enum { V_FREE = 0, V_KICK, V_IMPACT, V_NOISE, V_RISER, V_BASS, V_PAD, V_PLUCK, V_LEAD };
enum { D_LP = 1, D_DUCK = 2, D_VS = 4, D_DS = 8 };

#define COEF_STRIDE 4                         /* automated filters: coefficients every 4 frames */

typedef struct {
  double phase;                                /* table index [0, size) */
  double hz;                                   /* frequency * 2^(detune/1200) (fixed part) */
  float gain;
  uint8_t wave, note;
} mosc_t;

typedef struct {
  uint8_t kind, dest, nosc, ftype;             /* ftype: DSP_LOWPASS / DSP_HIGHPASS / DSP_BANDPASS */
  bool pan, loop;
  int64_t k0, k1;                              /* first frame, end frame (exclusive) */
  double t0;
  mparam_t gain, freq, vib;                    /* envelope; osc or filter frequency; lead vibrato depth */
  double fq, qlin;                             /* filter Q (dB for LP/HP, linear for BP); LP/HP: 10^(Q/20) */
  dsp_biquad_t fs[2];
  double npos;                                 /* noise read position (buffer frames) at k0 */
  double vibph;                                /* vibrato phase (sine table index) at k0 */
  float pl, pr;                                /* panner gains (mono in) */
  float padL[4], padR[4];
  mosc_t osc[12];
} voice_t;

struct mvoices {
  int max;
  uint32_t stolen;
  voice_t *v;
};

mvoices_t *mvoices_new(int max_voices) {
  mvoices_t *p = calloc(1, sizeof *p);
  if (!p) return NULL;
  p->max = max_voices;
  p->v = calloc((size_t)max_voices, sizeof *p->v);
  if (!p->v) { free(p); return NULL; }
  return p;
}
void mvoices_free(mvoices_t *p) { if (p) { free(p->v); free(p); } }
void mvoices_clear(mvoices_t *p) { for (int i = 0; i < p->max; i++) p->v[i].kind = V_FREE; }
int mvoices_active(const mvoices_t *p) {
  int n = 0;
  for (int i = 0; i < p->max; i++) n += p->v[i].kind != V_FREE;
  return n;
}
uint32_t mvoices_stolen(const mvoices_t *p) { return p->stolen; }

static voice_t *v_alloc(minst_t *I, int kind, double t, double stop) {
  mvoices_t *p = I->v;
  voice_t *v = NULL;
  for (int i = 0; i < p->max; i++) if (p->v[i].kind == V_FREE) { v = &p->v[i]; break; }
  if (!v) {                                    /* full (never seen): steal the voice that ends first */
    v = &p->v[0];
    for (int i = 1; i < p->max; i++) if (p->v[i].k1 < v->k1) v = &p->v[i];
    p->stolen++;
  }
  memset(v, 0, offsetof(voice_t, osc));
  v->kind = (uint8_t)kind;
  v->t0 = t;
  v->k0 = mframe_at(t, I->sr);
  v->k1 = mframe_at(stop, I->sr);
  mparam_init(&v->gain, 1);
  mparam_init(&v->freq, 0);
  mparam_init(&v->vib, 0);
  return v;
}

static void v_filter_type(voice_t *v, int type, double q) {
  v->ftype = (uint8_t)type;
  v->fq = q;
  v->qlin = pow(10.0, q / 20.0);
}

static double midi_hz(double m) { return 440.0 * pow(2.0, (m - 69.0) / 12.0); }

/* an oscillator starting at phase 0 at time t0 (sub-sample: the phase at the first frame is advanced) */
static void v_osc(minst_t *I, voice_t *v, int wave, double f, double detune, float gain, int note) {
  if (v->nosc >= 12) return;
  const dsp_wave_t *w = I->waves->w[wave];
  mosc_t *o = &v->osc[v->nosc++];
  o->wave = (uint8_t)wave;
  o->hz = f * pow(2.0, detune / 1200.0);
  o->gain = gain;
  o->note = (uint8_t)note;
  double dt = (double)v->k0 - v->t0 * I->sr;   /* frames between the start time and the first frame */
  o->phase = fmod(dt * o->hz * w->size / I->sr, (double)w->size);
}

static void env(mparam_t *g, double t, double v, double a, double d) {
  mparam_set(g, 0.0001, t);
  mparam_exp(g, v, t + a);
  mparam_exp(g, 0.0001, t + a + d);
}

static void pan_gains(double pan, float *l, float *r) {
  double gl, gr;
  dsp_pan_mono_gains((float)pan, &gl, &gr);
  *l = (float)gl; *r = (float)gr;
}

static void logev(minst_t *I, const char *name, const double *a, int na) { if (I->log) I->log(I->log_ud, name, a, na); }

/* noiseHit: noise from offset rnd*0.5 s (not looped) -> biquad -> env(v, 0.002, dur) -> pan -> dest */
static void noise_hit(minst_t *I, double t, double dur, int type, double f, double q, double v, int dest, double pan) {
  voice_t *x = v_alloc(I, V_NOISE, t, t + dur + 0.05);
  x->dest = (uint8_t)dest;
  v_filter_type(x, type, q);
  x->freq.def = f;
  dsp_biquad_set(&x->fs[0], type, f, q, 0, I->sr);   /* constant: the coefficients once */
  env(&x->gain, t, v, 0.002, dur);
  if (pan != 0) { x->pan = true; pan_gains(pan, &x->pl, &x->pr); }
  /* s.start(t, Math.random() * 0.5): Chromium rounds the offset to a whole frame, then starts
   * sub-sample accurately (the read position moves by the start's fraction; interpolated) */
  double off = dsp_rng(I->rs) * 0.5;
  x->npos = floor(off * I->sr + 0.5) + ((double)x->k0 - t * I->sr);
}

void mi_kick(minst_t *I, double t) {
  double a[1] = { t };
  logev(I, "kick", a, 1);
  voice_t *v = v_alloc(I, V_KICK, t, t + 0.45);
  v->dest = D_LP;
  mparam_init(&v->freq, 440);                  /* OscillatorNode.frequency's default */
  mparam_set(&v->freq, 160, t);
  mparam_exp(&v->freq, 48, t + 0.1);
  env(&v->gain, t, 0.85, 0.003, 0.38);
  v->osc[0].phase = 0;
  v->nosc = 1;
  noise_hit(I, t, 0.01, DSP_HIGHPASS, 2500, 0.7, 0.1, D_LP, 0);
  mparam_t *d = I->duck;                       /* side-chain pump */
  mparam_cancel(d, t);
  mparam_set(d, 0.35, t);
  mparam_lin(d, 1, t + 0.24);
}

void mi_clap(minst_t *I, double t, double v) {
  double a[2] = { t, v };
  logev(I, "clap", a, 2);
  for (int k = 0; k < 3; k++) noise_hit(I, t + k * 0.011, k < 2 ? 0.02 : 0.18, DSP_BANDPASS, 1500, 1.1, 0.6 * v, D_LP, 0);
  noise_hit(I, t, 0.3, DSP_BANDPASS, 1500, 1, 0.2 * v, D_VS, 0);
}

void mi_hat(minst_t *I, double t, double v, bool open) {
  double a[3] = { t, v, open ? 1 : 0 };
  logev(I, "hat", a, 3);
  double jit = (dsp_rng(I->rs) - 0.5) * 0.004;  /* JS evaluates the arguments left to right */
  double lvl = (open ? 0.75 : 0.7) * v * (0.9 + dsp_rng(I->rs) * 0.2);
  noise_hit(I, t + jit, open ? 0.3 : 0.05, DSP_HIGHPASS, 6500, 0.7, lvl, D_LP, open ? -0.2 : 0.25);
}

void mi_crash(minst_t *I, double t) {
  double a[1] = { t };
  logev(I, "crash", a, 1);
  noise_hit(I, t, 1.8, DSP_HIGHPASS, 4500, 0.5, 0.35, D_LP, 0.15);
  noise_hit(I, t, 2.2, DSP_HIGHPASS, 4500, 0.5, 0.15, D_VS, 0);
}

void mi_impact(minst_t *I, double t) {
  double a[1] = { t };
  logev(I, "impact", a, 1);
  voice_t *v = v_alloc(I, V_IMPACT, t, t + 1.2);
  v->dest = D_LP;
  mparam_init(&v->freq, 440);
  mparam_set(&v->freq, 70, t);
  mparam_exp(&v->freq, 38, t + 0.9);
  env(&v->gain, t, 0.5, 0.005, 1.1);
  v->osc[0].phase = 0;
  v->nosc = 1;
}

void mi_bass(minst_t *I, double t, int m, double dur) {
  double a[3] = { t, m, dur };
  logev(I, "bassNote", a, 3);
  voice_t *v = v_alloc(I, V_BASS, t, t + dur + 0.06);
  v->dest = D_DUCK;
  v_filter_type(v, DSP_LOWPASS, 5);
  mparam_set(&v->freq, 200, t);
  mparam_exp(&v->freq, 900, t + 0.012);
  mparam_exp(&v->freq, 280, t + dur);
  mparam_set(&v->gain, 0.0001, t);
  mparam_exp(&v->gain, 0.17, t + 0.006);
  mparam_set(&v->gain, 0.17, t + dur * 0.8);
  mparam_exp(&v->gain, 0.0001, t + dur + 0.04);
  v_osc(I, v, MW_SAW, midi_hz(m), -6, 1, 0);
  v_osc(I, v, MW_SAW, midi_hz(m), 6, 1, 0);
  v_osc(I, v, MW_SINE, midi_hz(m - 12), 0, 0.9f, 1);   /* the sub skips the filter (note = 1) */
}

void mi_pad(minst_t *I, double t, const int *notes, double dur, double cut) {
  double a[7] = { t, notes[0], notes[1], notes[2], notes[3], dur, cut };
  logev(I, "padChord", a, 7);
  voice_t *v = v_alloc(I, V_PAD, t, t + dur + 1);
  v->dest = D_DUCK | D_VS;
  v_filter_type(v, DSP_LOWPASS, 1);
  mparam_set(&v->freq, cut * 0.6, t);
  mparam_lin(&v->freq, cut, t + dur * 0.6);
  mparam_set(&v->gain, 0.0001, t);
  mparam_exp(&v->gain, 0.045, t + 0.4);
  mparam_set(&v->gain, 0.045, t + dur - 0.05);
  mparam_exp(&v->gain, 0.0001, t + dur + 0.9);
  static const double DT[3] = { -11, 0, 11 };
  for (int i = 0; i < 4; i++) {
    pan_gains(i % 2 ? 0.3 : -0.3, &v->padL[i], &v->padR[i]);
    for (int k = 0; k < 3; k++) v_osc(I, v, MW_SAW, midi_hz(notes[i]), DT[k] + (dsp_rng(I->rs) - 0.5) * 4, 1, i);
  }
}

void mi_pluck(minst_t *I, double t, int m, double vel, double pan, double bright) {
  double a[5] = { t, m, vel, pan, bright };
  logev(I, "pluck", a, 5);
  voice_t *v = v_alloc(I, V_PLUCK, t, t + 0.25);
  v->dest = D_DUCK | D_DS;
  v_filter_type(v, DSP_LOWPASS, 2);
  mparam_set(&v->freq, 1200 + 1800 * bright, t);
  mparam_exp(&v->freq, 500, t + 0.15);
  env(&v->gain, t, 0.075 * vel, 0.004, 0.2);
  if (pan != 0) { v->pan = true; pan_gains(pan, &v->pl, &v->pr); }
  v_osc(I, v, MW_SAW, midi_hz(m), -5, 1, 0);
  v_osc(I, v, MW_SQUARE, midi_hz(m), 5, 1, 0);
}

void mi_lead(minst_t *I, double t, int m, double dur, bool soft) {
  double a[4] = { t, m, dur, soft ? 1 : 0 };
  logev(I, "leadNote", a, 4);
  voice_t *v = v_alloc(I, V_LEAD, t, t + dur + 0.15);
  v->dest = D_LP | D_DS | D_VS;
  v_filter_type(v, DSP_LOWPASS, 1.5);
  mparam_set(&v->freq, soft ? 1600 : 3800, t);
  mparam_exp(&v->freq, soft ? 1000 : 2300, t + dur);
  double lv = soft ? 0.06 : 0.1;
  mparam_set(&v->gain, 0.0001, t);
  mparam_exp(&v->gain, lv, t + 0.012);
  mparam_set(&v->gain, lv * 0.85, t + fmax(0.02, dur - 0.03));
  mparam_exp(&v->gain, 0.0001, t + dur + 0.12);
  mparam_set(&v->vib, 0, t);
  mparam_lin(&v->vib, dur > 0.3 ? 14 : 0, t + fmin(dur, 0.5));
  const dsp_wave_t *sw = I->waves->w[MW_SINE];
  double dt = (double)v->k0 - t * I->sr;
  v->vibph = fmod(dt * 5.5 * sw->size / I->sr, (double)sw->size);
  v_osc(I, v, MW_SAW, midi_hz(m), -8, 1, 0);
  v_osc(I, v, MW_SAW, midi_hz(m), 8, 1, 0);
  v_osc(I, v, MW_SQUARE, midi_hz(m - 12), 0, 0.4f, 0);
  /* the vibrato drives their detune (a connection): Chromium's sample-accurate oscillator path,
   * which starts at phase 0 on the first frame without the sub-sample advance (measured) */
  for (int k = 0; k < v->nosc; k++) v->osc[k].phase = 0;
}

void mi_riser(minst_t *I, double t, double dur) {
  double a[2] = { t, dur };
  logev(I, "riser", a, 2);
  voice_t *v = v_alloc(I, V_RISER, t, t + dur + 0.25);
  v->dest = D_LP | D_VS;
  v_filter_type(v, DSP_BANDPASS, 2);
  v->loop = true;
  mparam_set(&v->freq, 300, t);
  mparam_exp(&v->freq, 8000, t + dur);
  mparam_set(&v->gain, 0.0001, t);
  mparam_exp(&v->gain, 0.12, t + dur);
  mparam_exp(&v->gain, 0.0001, t + dur + 0.2);
  v->npos = (double)v->k0 - t * I->sr;
}

/* ---------------------------------------------------------------- rendering */

/* one table read: two band-limited tables (picked by pitch) crossfaded, linear interpolation inside
 * (Chromium's OscillatorNode, as dsp_osc_next; tables carry a guard sample at [size]) */
static inline float tab_read(const float *hi, const float *lo, float tf, double ph) {
  unsigned i = (unsigned)ph;
  float fr = (float)(ph - (double)i);
  float sh = (1.0f - fr) * hi[i] + fr * hi[i + 1];
  if (lo == hi) return sh;
  float sl = (1.0f - fr) * lo[i] + fr * lo[i + 1];
  return (1.0f - tf) * sh + tf * sl;
}

/* a constant-frequency oscillator over n frames, added into dst at gain g */
static void osc_block(const dsp_wave_t *w, mosc_t *o, float g, float *dst, int n) {
  float f = (float)o->hz;
  if (f > w->nyquist) f = w->nyquist;
  float incr = f * w->rate_scale;
  const float *hi, *lo; float tf;
  dsp_wave_select(w, incr, &hi, &lo, &tf);
  double ph = o->phase, S = (double)w->size;
  for (int i = 0; i < n; i++) {
    dst[i] += g * tab_read(hi, lo, tf, ph);
    ph += incr;
    if (ph >= S) ph -= S;
  }
  o->phase = ph;
}

/* the voice filter's coefficients for frequency f (both channels: the pad is stereo) */
static void v_coefs(voice_t *v, double f, double sr) {
  dsp_biquad_t *b = &v->fs[0], *c = &v->fs[1];
  if (v->ftype == DSP_BANDPASS) dsp_biquad_set(b, v->ftype, f, v->fq, 0, sr);
  else dsp_biquad_set_lphp(b, v->ftype == DSP_HIGHPASS, f, v->qlin, sr);
  c->b0 = b->b0; c->b1 = b->b1; c->b2 = b->b2; c->a1 = b->a1; c->a2 = b->a2;
}

/* filter a block in place (xr NULL: mono); the frequency param automated (fr[]) or constant (fr[0]) */
static void v_filter(voice_t *v, float *xl, float *xr, int n, double sr, const double *fr, bool fconst) {
  if (fconst) v_coefs(v, fr[0], sr);
  for (int i = 0; i < n; i++) {
    if (!fconst && i % COEF_STRIDE == 0) v_coefs(v, fr[i], sr);
    xl[i] = dsp_biquad_run(&v->fs[0], xl[i]);
    if (xr) xr[i] = dsp_biquad_run(&v->fs[1], xr[i]);
  }
  dsp_biquad_flush(&v->fs[0]);
  if (xr) dsp_biquad_flush(&v->fs[1]);
}

static void v_out_mono(const voice_t *v, const float *x, int off, int n, mbus_t *b) {
  float gl = v->pan ? v->pl : 1.0f, gr = v->pan ? v->pr : 1.0f;
  if (v->dest & D_LP) for (int i = 0; i < n; i++) { b->lpL[off + i] += gl * x[i]; b->lpR[off + i] += gr * x[i]; }
  if (v->dest & D_DUCK) for (int i = 0; i < n; i++) { b->duckL[off + i] += gl * x[i]; b->duckR[off + i] += gr * x[i]; }
  /* sends are unpanned (the clap and crash rooms have no pan; the lead and riser are mono) */
  if (v->dest & D_VS) for (int i = 0; i < n; i++) { b->vsL[off + i] += x[i]; b->vsR[off + i] += x[i]; }
  if (v->dest & D_DS) for (int i = 0; i < n; i++) b->ds[off + i] += x[i];
}

static void render_voice(voice_t *v, const mwaves_t *W, const float *noise, int nlen, int64_t q0, double sr, mbus_t *b) {
  int64_t a = v->k0 > q0 ? v->k0 : q0, e = v->k1 < q0 + MQ ? v->k1 : q0 + MQ;
  if (e <= a) return;
  int off = (int)(a - q0), n = (int)(e - a);
  double g[MQ], fr[MQ];
  float x[MQ], y[MQ];
  mparam_fill(&v->gain, a, n, sr, g);
  double fc = 0;
  bool fconst = true;
  if (v->kind != V_KICK && v->kind != V_IMPACT && v->kind != V_NOISE) {
    fconst = mparam_constant(&v->freq, a, n, sr, &fc);
    if (fconst) fr[0] = fc; else mparam_fill(&v->freq, a, n, sr, fr);
  }
  switch (v->kind) {
  case V_KICK: case V_IMPACT: {                /* a sine with an exponential pitch drop */
    const dsp_wave_t *w = W->w[MW_SINE];
    double S = (double)w->size;
    /* Chromium (measured): in the render quantum where an oscillator starts, output frame k0 + j
     * reads the frequency computed for frame q0 + j (the quantum's start, not the oscillator's),
     * and before the first event that is the intrinsic value (440 Hz). From the next quantum on
     * the automation is aligned. So a kick at frame 4800 plays 64 frames at 440 Hz, one at frame
     * 27658 plays 10 frames at 440 Hz and then the ramp 10 frames early until frame 27776. */
    if (a == v->k0 && v->k0 % MQ != 0) mparam_fill(&v->freq, q0, n, sr, fr);
    else mparam_fill(&v->freq, a, n, sr, fr);
    mosc_t *o = &v->osc[0];
    if (a == v->k0) o->phase = 0;              /* automated frequency: no sub-sample advance (measured) */
    const float *t0 = w->tab[0];
    for (int i = 0; i < n; i++) {
      float incr = (float)fr[i] * w->rate_scale;
      const float *hi = t0, *lo = t0; float tf = 0;
      if (incr >= w->simple_incr) dsp_wave_select(w, incr, &hi, &lo, &tf);
      x[i] = (float)g[i] * tab_read(hi, lo, tf, o->phase);
      o->phase += incr;
      if (o->phase >= S) o->phase -= S;
    }
    v_out_mono(v, x, off, n, b);
  } break;
  case V_NOISE: {                              /* not looped: the hit stops at the buffer's end (6.5) */
    int m = 0;
    for (int i = 0; i < n; i++) {
      double p = v->npos + (double)(a + i - v->k0);
      int i0 = (int)p;
      if (i0 + 1 >= nlen) break;
      float fr0 = (float)(p - i0);
      float s = noise[i0] + fr0 * (noise[i0 + 1] - noise[i0]);
      x[i] = dsp_biquad_run(&v->fs[0], s) * (float)g[i];
      m = i + 1;
    }
    dsp_biquad_flush(&v->fs[0]);
    if (m < n) v->k1 = a + m;                  /* finished */
    v_out_mono(v, x, off, m, b);
  } break;
  case V_RISER: {                              /* looped from offset 0 */
    for (int i = 0; i < n; i++) {
      double p = fmod(v->npos + (double)(a + i - v->k0), (double)nlen);
      int i0 = (int)p, i1 = i0 + 1 < nlen ? i0 + 1 : 0;
      float fr0 = (float)(p - i0);
      y[i] = noise[i0] + fr0 * (noise[i1] - noise[i0]);
    }
    v_filter(v, y, NULL, n, sr, fr, fconst);
    for (int i = 0; i < n; i++) x[i] = y[i] * (float)g[i];
    v_out_mono(v, x, off, n, b);
  } break;
  case V_BASS: {
    float sub[MQ];
    for (int i = 0; i < n; i++) { y[i] = 0; sub[i] = 0; }
    for (int k = 0; k < v->nosc; k++) {
      mosc_t *o = &v->osc[k];
      osc_block(W->w[o->wave], o, o->gain, o->note ? sub : y, n);
    }
    v_filter(v, y, NULL, n, sr, fr, fconst);
    for (int i = 0; i < n; i++) x[i] = (y[i] + sub[i]) * (float)g[i];
    v_out_mono(v, x, off, n, b);
  } break;
  case V_PLUCK: {
    for (int i = 0; i < n; i++) y[i] = 0;
    for (int k = 0; k < v->nosc; k++) osc_block(W->w[v->osc[k].wave], &v->osc[k], 1.0f, y, n);
    v_filter(v, y, NULL, n, sr, fr, fconst);
    for (int i = 0; i < n; i++) x[i] = y[i] * (float)g[i];
    /* panned into duck, unpanned into the delay send */
    float gl = v->pan ? v->pl : 1.0f, gr = v->pan ? v->pr : 1.0f;
    for (int i = 0; i < n; i++) { b->duckL[off + i] += gl * x[i]; b->duckR[off + i] += gr * x[i]; b->ds[off + i] += x[i]; }
  } break;
  case V_PAD: {                                /* 4 notes x 3 saws, each note panned, one stereo filter */
    float yl[MQ], yr[MQ], s1[MQ];
    for (int i = 0; i < n; i++) { yl[i] = 0; yr[i] = 0; }
    for (int note = 0; note < 4; note++) {
      for (int i = 0; i < n; i++) s1[i] = 0;
      for (int k = 0; k < v->nosc; k++) if (v->osc[k].note == note) osc_block(W->w[v->osc[k].wave], &v->osc[k], 1.0f, s1, n);
      float pl = v->padL[note], pr = v->padR[note];
      for (int i = 0; i < n; i++) { yl[i] += pl * s1[i]; yr[i] += pr * s1[i]; }
    }
    v_filter(v, yl, yr, n, sr, fr, fconst);
    for (int i = 0; i < n; i++) {
      float l = yl[i] * (float)g[i], r = yr[i] * (float)g[i];
      b->duckL[off + i] += l; b->duckR[off + i] += r;
      b->vsL[off + i] += l; b->vsR[off + i] += r;
    }
  } break;
  case V_LEAD: {                               /* vibrato on the detune: per-sample pitch and tables */
    double vd[MQ];
    mparam_fill(&v->vib, a, n, sr, vd);
    const dsp_wave_t *sw = W->w[MW_SINE];
    const float *S0 = sw->tab[0];
    double SS = (double)sw->size, vinc = 5.5 * SS / sr;
    float cents[MQ];
    for (int i = 0; i < n; i++) {
      cents[i] = (float)vd[i] * tab_read(S0, S0, 0, v->vibph);
      v->vibph += vinc;
      if (v->vibph >= SS) v->vibph -= SS;
    }
    for (int i = 0; i < n; i++) y[i] = 0;
    for (int k = 0; k < v->nosc; k++) {
      mosc_t *o = &v->osc[k];
      const dsp_wave_t *w = W->w[o->wave];
      double S = (double)w->size;
      for (int i = 0; i < n; i++) {
        float f = (float)(o->hz * exp2((double)cents[i] / 1200.0));
        if (f > w->nyquist) f = w->nyquist;
        float incr = f * w->rate_scale;
        const float *hi, *lo; float tf;
        dsp_wave_select(w, incr, &hi, &lo, &tf);
        y[i] += o->gain * tab_read(hi, lo, tf, o->phase);
        o->phase += incr;
        if (o->phase >= S) o->phase -= S;
      }
    }
    v_filter(v, y, NULL, n, sr, fr, fconst);
    for (int i = 0; i < n; i++) x[i] = y[i] * (float)g[i];
    v_out_mono(v, x, off, n, b);
  } break;
  }
}

void mvoices_render(mvoices_t *p, const mwaves_t *w, const float *noise, int nlen, int64_t q0, double sr, mbus_t *bus) {
  for (int i = 0; i < p->max; i++) {
    voice_t *v = &p->v[i];
    if (v->kind == V_FREE) continue;
    render_voice(v, w, noise, nlen, q0, sr, bus);
    if (v->k1 <= q0 + MQ) v->kind = V_FREE;
  }
}
