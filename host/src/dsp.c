/* dsp.c: WebAudio-like building blocks with Chromium's semantics (see dsp.h). */
#include "dsp.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>

/* Chromium's x86 build (the reference renders) has no fused multiply-adds in these loops */
#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

/* ---------------------------------------------------------------------------------------- random */
void dsp_noise_fill(float *d, size_t n, dsp_rng_t *r) {
  for (size_t i = 0; i < n; i++) d[i] = (float)(dsp_rng(r) * 2.0 - 1.0);
}

/* ------------------------------------------------------------------------------------ smoothing */
double dsp_tau_coef(double tau_s, double rate) {
  if (!(tau_s > 0) || !(rate > 0)) return 1.0;
  return 1.0 - exp(-1.0 / (rate * tau_s));
}

void dsp_smooth_coef(dsp_smooth_t *s, float c) {
  s->c = c;
  /* tau in recursion steps: c = 1 - exp(-1/steps) */
  s->conv = c > 0 && c < 1 ? (float)(10.0 * (-1.0 / log1p(-(double)c))) : 0.0f;
}

/* one ProcessSetTarget call: out[0..n-1] for target T from the value v carried in; hold: the event
 * starts here (exp(0)), else the first sample steps */
static void smooth_seg(dsp_smooth_t *s, float v, float T, float *out, int n, int hold) {
  const float c = s->c;
  if (hold) s->age = 0;
  else v += (T - v) * c;
  /* HasSetTargetConverged (kSetTargetThreshold = exp(-10)) */
  if ((float)s->age > s->conv || (T == 0.0f ? fabsf(v) < 4.539992976248485e-05f
                                            : fabsf(T - v) < 4.539992976248485e-05f * fabsf(v))) {
    for (int i = 0; i < n; i++) out[i] = T;
  } else {
    const float c0 = c, c1 = c0 * (2 - c0), c2 = c0 * ((c0 - 3) * c0 + 3),
                c3 = c0 * (c0 * ((4 - c0) * c0 - 6) + 4);
    int i = 0;
    for (; i + 4 <= n; i += 4) {                       /* Chromium's SSE loop */
      const float d = T - v;
      out[i] = v;
      out[i + 1] = v + d * c0;
      out[i + 2] = v + d * c1;
      out[i + 3] = v + d * c2;
      v += d * c3;
    }
    for (; i < n; i++) { out[i] = v; v += (T - v) * c; }
  }
  if (s->age < INT32_MAX - n) s->age += n;
}

void dsp_smooth_fill(dsp_smooth_t *s, float *out, int n) {
  if (n <= 0) return;
  float v = s->v;
  if (s->pend) {                     /* zero frames of the old event step v, the new one holds it */
    s->pend = 0;
    v += (s->old_target - v) * s->c;
    v = (float)((double)s->target + (double)(v - s->target));      /* T + (v - T) exp(0) */
    smooth_seg(s, v, s->target, out, n, 1);
  } else smooth_seg(s, v, s->target, out, n, 0);
  s->v = out[n - 1];                 /* the param's value for the next quantum */
}

/* ------------------------------------------------------------------------------- param timeline */
void dsp_param_init(dsp_param_t *p, float value, double rate) {
  memset(p, 0, sizeof *p);
  p->rate = rate;
  p->value = value;
  p->base_kind = 0;
  p->base_t = 0;
  p->base_v = value;
}

static double seg_value(int kind, double t0, double v0, float target, float tau, double t) {
  if (kind == 1 && tau > 0) return target + (v0 - target) * exp(-(t - t0) / tau);
  if (kind == 1) return target;
  return v0;
}

static double ramp_value(int type, double t0, double v0, double t1, double v1, double t) {
  if (t1 <= t0) return v1;
  double x = (t - t0) / (t1 - t0);
  if (x < 0) x = 0;
  if (type == DSP_EV_LIN) return v0 + (v1 - v0) * x;
  if (v0 == 0 || v1 == 0 || (v0 < 0) != (v1 < 0)) return v0;   /* spec: hold v0 until t1 */
  return v0 * pow(v1 / v0, x);
}

float dsp_param_at(const dsp_param_t *p, double t) {
  int kind = p->base_kind;
  double bt = p->base_t, bv = p->base_v;
  float tg = p->base_target, tau = p->base_tau;
  for (int i = 0; i < p->n; i++) {
    const dsp_event_t *e = &p->ev[i];
    if (e->type == DSP_EV_LIN || e->type == DSP_EV_EXP) {
      double v0 = seg_value(kind, bt, bv, tg, tau, bt);
      if (t < e->t) return (float)ramp_value(e->type, bt, v0, e->t, e->v, t);
      kind = 0; bt = e->t; bv = e->v;
      continue;
    }
    if (t < e->t) break;
    if (e->type == DSP_EV_SET) { kind = 0; bt = e->t; bv = e->v; }
    else {
      double vs = seg_value(kind, bt, bv, tg, tau, e->t);
      kind = 1; bt = e->t; bv = vs; tg = e->v; tau = e->tau;
    }
  }
  return (float)seg_value(kind, bt, bv, tg, tau, t);
}

/* fold every event that has fully started by frame f (SET/TARGET, or ramps that ended): as in
 * Chromium, an event at time t takes effect at frame ceil(t * rate), i.e. at frames f >= t * rate */
static void param_fold(dsp_param_t *p, double f) {
  int k = 0;
  while (k < p->n && p->ev[k].t * p->rate <= f) {
    const dsp_event_t *e = &p->ev[k];
    if (e->type == DSP_EV_TARGET) {
      double vs = seg_value(p->base_kind, p->base_t, p->base_v, p->base_target, p->base_tau, e->t);
      p->base_kind = 1; p->base_t = e->t; p->base_v = vs; p->base_target = e->v; p->base_tau = e->tau;
    } else {
      p->base_kind = 0; p->base_t = e->t; p->base_v = e->v;
    }
    k++;
  }
  if (k) {
    memmove(p->ev, p->ev + k, (size_t)(p->n - k) * sizeof p->ev[0]);
    p->n -= k;
  }
}

static bool param_insert(dsp_param_t *p, int type, float v, double t, float tau) {
  if (!(t >= 0)) t = 0;
  if (p->n >= DSP_PARAM_EVENTS) param_fold(p, p->now * p->rate);
  int i = 0;
  while (i < p->n && p->ev[i].t <= t) {
    if (p->ev[i].t == t && p->ev[i].type == type) {     /* same type at the same time: replace */
      p->ev[i].v = v; p->ev[i].tau = tau;
      return true;
    }
    i++;
  }
  if (p->n >= DSP_PARAM_EVENTS) return false;
  memmove(p->ev + i + 1, p->ev + i, (size_t)(p->n - i) * sizeof p->ev[0]);
  p->ev[i].type = (uint8_t)type; p->ev[i].t = t; p->ev[i].v = v; p->ev[i].tau = tau;
  p->n++;
  return true;
}

bool dsp_param_set(dsp_param_t *p, float v, double t) { return param_insert(p, DSP_EV_SET, v, t, 0); }
bool dsp_param_target(dsp_param_t *p, float v, double t, float tau) {
  return param_insert(p, DSP_EV_TARGET, v, t, tau);
}

/* a ramp after a TARGET: start it from that curve's value at max(its start, now) (Chromium) */
static bool param_ramp(dsp_param_t *p, int type, float v, double t) {
  int prev = -1;
  for (int i = 0; i < p->n && p->ev[i].t <= t; i++) prev = i;
  int prev_target = prev >= 0 ? p->ev[prev].type == DSP_EV_TARGET : p->base_kind == 1;
  if (prev_target) {
    double ts = prev >= 0 ? p->ev[prev].t : p->base_t;
    if (p->now > ts) ts = p->now;
    if (ts < t && !param_insert(p, DSP_EV_SET, dsp_param_at(p, ts), ts, 0)) return false;
  }
  return param_insert(p, type, v, t, 0);
}
bool dsp_param_lin(dsp_param_t *p, float v, double t) { return param_ramp(p, DSP_EV_LIN, v, t); }
bool dsp_param_exp(dsp_param_t *p, float v, double t) { return param_ramp(p, DSP_EV_EXP, v, t); }

void dsp_param_cancel(dsp_param_t *p, double t) {
  int k = 0;
  while (k < p->n && p->ev[k].t < t) k++;
  p->n = k;
}

bool dsp_param_idle(const dsp_param_t *p, double t) {
  int kind = p->base_kind;
  for (int i = 0; i < p->n; i++) {
    if (p->ev[i].t > t) return false;
    kind = p->ev[i].type == DSP_EV_TARGET ? 1 : 0;
  }
  if (kind == 0) return true;
  float v = dsp_param_at(p, t), tg = p->n ? p->ev[p->n - 1].v : p->base_target;
  return fabsf(v - tg) <= 1e-6f * (fabsf(tg) > 1 ? fabsf(tg) : 1);
}

/* the frame an event at time t takes effect (Chromium: fill_to_end_frame = ceil(t * rate)) */
static int64_t first_frame_at(double t, double rate) {
  double x = ceil(t * rate);
  if (x > 9.0e18) return INT64_MAX;
  return (int64_t)x;
}

void dsp_param_render(dsp_param_t *p, int64_t frame0, float *out, int n) {
  const double rate = p->rate;
  int i = 0;
  while (i < n) {
    int64_t f = frame0 + i;
    double t = (double)f / rate;
    param_fold(p, (double)f);
    int64_t fend = frame0 + n;
    if (p->n) {
      int64_t fe = first_frame_at(p->ev[0].t, rate);
      if (fe < fend) fend = fe;
    }
    int m = (int)(fend - f);
    if (m < 1) m = 1;                       /* (cannot happen: ev[0].t > t) */
    if (p->n && (p->ev[0].type == DSP_EV_LIN || p->ev[0].type == DSP_EV_EXP)) {
      const dsp_event_t *e = &p->ev[0];
      double v0 = seg_value(p->base_kind, p->base_t, p->base_v, p->base_target, p->base_tau, p->base_t);
      if (e->type == DSP_EV_LIN || v0 == 0 || e->v == 0 || (v0 < 0) != (e->v < 0) || e->t <= p->base_t) {
        for (int j = 0; j < m; j++)
          out[i + j] = (float)ramp_value(e->type, p->base_t, v0, e->t, e->v, (double)(f + j) / rate);
      } else {
        /* Chromium: V = v1 (v2/v1)^((f/rate - t1)/(t2 - t1)), then x m per sample with
         * m = ((float)(v2/v1))^(1/((t2 - t1) rate)), restarted at every segment */
        double v = ramp_value(e->type, p->base_t, v0, e->t, e->v, t);
        double k = pow((double)((float)v0 > 0 || (float)v0 < 0 ? e->v / (float)v0 : 0.0f),
                       1.0 / ((e->t - p->base_t) * rate));
        for (int j = 0; j < m; j++) { out[i + j] = (float)v; v *= k; }
      }
    } else if (p->base_kind == 1 && p->base_tau > 0) {
      double tg = p->base_target;
      double d = seg_value(1, p->base_t, p->base_v, p->base_target, p->base_tau, t) - tg;
      double k = exp(-1.0 / (rate * p->base_tau));
      for (int j = 0; j < m; j++) { out[i + j] = (float)(tg + d); d *= k; }
    } else {
      float v = (float)seg_value(p->base_kind, p->base_t, p->base_v, p->base_target, p->base_tau, t);
      for (int j = 0; j < m; j++) out[i + j] = v;
    }
    i += m;
  }
  p->now = (double)(frame0 + n - 1) / rate;
  /* keep the base anchored near now so exp() arguments stay small */
  if (p->base_kind == 1 && p->base_tau > 0 && p->now - p->base_t > 64.0 * p->base_tau) {
    p->base_v = p->base_target; p->base_t = p->now; p->base_kind = 0;
  }
}

/* ------------------------------------------------------------------------------------ wavetables */
static void fft_inplace(double *re, double *im, int n) {      /* unscaled, e^{+j} (inverse) */
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
  }
  for (int len = 2; len <= n; len <<= 1) {
    double ang = 2 * DSP_PI / len;
    for (int i = 0; i < n; i += len) {
      for (int k = 0; k < len / 2; k++) {
        double wr = cos(ang * k), wi = sin(ang * k);
        double ur = re[i + k], ui = im[i + k];
        double xr = re[i + k + len / 2], xi = im[i + k + len / 2];
        double vr = xr * wr - xi * wi, vi = xr * wi + xi * wr;
        re[i + k] = ur + vr; im[i + k] = ui + vi;
        re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
      }
    }
  }
}

static int wave_size_for_rate(double rate) {
  if (rate <= 24000) return 2048;
  if (rate <= 88200) return 4096;
  return 16384;
}

/* x[n] = sum_{k=1..kmax} (a_k cos(2 pi k n/S) + b_k sin(2 pi k n/S)) */
static void wave_build_table(float *dst, int S, const float *real, const float *imag, int kmax,
                             double *re, double *im) {
  if (kmax <= 32) {
    for (int i = 0; i < S; i++) {
      double x = 0, w = 2 * DSP_PI * i / S;
      for (int k = 1; k <= kmax; k++) {
        double a = real ? real[k] : 0, b = imag ? imag[k] : 0;
        if (a != 0) x += a * cos(w * k);
        if (b != 0) x += b * sin(w * k);
      }
      dst[i] = (float)x;
    }
    return;
  }
  for (int i = 0; i < S; i++) { re[i] = 0; im[i] = 0; }
  for (int k = 1; k <= kmax; k++) {          /* C_k = a_k - j b_k: Re(C e^{j}) = a cos + b sin */
    re[k] = real ? real[k] : 0;
    im[k] = imag ? -imag[k] : 0;
  }
  fft_inplace(re, im, S);
  for (int i = 0; i < S; i++) dst[i] = (float)re[i];
}

dsp_wave_t *dsp_wave_new(const float *real, const float *imag, int ncomp, bool normalize, double rate) {
  dsp_wave_t *w = calloc(1, sizeof *w);
  if (!w) return NULL;
  int S = wave_size_for_rate(rate);
  w->size = S; w->mask = S - 1; w->inv_size = 1.0 / S;
  w->nranges = (int)lroundf(3.0f * log2f((float)S));
  if (w->nranges > DSP_WAVE_MAX_RANGES) w->nranges = DSP_WAVE_MAX_RANGES;
  w->rate_scale = (float)S / (float)rate;
  w->inv_rate_scale = 1.0f / w->rate_scale;
  w->lowest = (float)(0.5 * rate) / (float)(S / 2);
  w->nyquist = (float)(0.5 * rate);
  if (ncomp > S / 2) ncomp = S / 2;
  int kfull = ncomp - 1;                       /* highest partial index given */
  int keff[DSP_WAVE_MAX_RANGES], ndist = 0, dist_of[DSP_WAVE_MAX_RANGES];
  int dist_k[DSP_WAVE_MAX_RANGES];
  for (int r = 0; r < w->nranges; r++) {
    float scale = powf(2.0f, -(float)r * 400.0f / 1200.0f);
    unsigned np = (unsigned)(scale * (float)(S / 2));
    int k = (int)np < kfull ? (int)np : kfull;
    if (k < 0) k = 0;
    keff[r] = k;
    int d = -1;
    for (int j = 0; j < ndist; j++) if (dist_k[j] == k) { d = j; break; }
    if (d < 0) { d = ndist; dist_k[ndist++] = k; }
    dist_of[r] = d;
  }
  w->mem = calloc((size_t)ndist * (size_t)(S + 1), sizeof(float));
  double *re = malloc(sizeof(double) * (size_t)S), *im = malloc(sizeof(double) * (size_t)S);
  if (!w->mem || !re || !im) { free(re); free(im); dsp_wave_free(w); return NULL; }
  for (int d = 0; d < ndist; d++)
    wave_build_table(w->mem + (size_t)d * (size_t)(S + 1), S, real, imag, dist_k[d], re, im);
  free(re); free(im);
  /* normalisation: 1 / peak of table 0 (the full-partial table), applied to all tables */
  float norm = 1.0f;
  if (normalize) {
    const float *t0 = w->mem + (size_t)dist_of[0] * (size_t)(S + 1);
    float mx = 0;
    for (int i = 0; i < S; i++) if (fabsf(t0[i]) > mx) mx = fabsf(t0[i]);
    if (mx > 0) norm = 1.0f / mx;
  }
  w->norm = norm;
  for (int d = 0; d < ndist; d++) {
    float *t = w->mem + (size_t)d * (size_t)(S + 1);
    for (int i = 0; i < S; i++) t[i] *= norm;
    t[S] = t[0];
  }
  for (int r = 0; r < w->nranges; r++) w->tab[r] = w->mem + (size_t)dist_of[r] * (size_t)(S + 1);
  /* below simple_incr, r1 and r2 both read table 0's data: skip the log2 and the crossfade */
  int rd = w->nranges;
  for (int r = 1; r < w->nranges; r++) if (w->tab[r] != w->tab[0]) { rd = r; break; }
  if (rd >= w->nranges) w->simple_incr = FLT_MAX;
  else {
    double flim = (double)w->lowest * pow(2.0, (rd - 2) / 3.0) * 0.9999;
    w->simple_incr = (float)(flim * (double)w->rate_scale);
  }
  (void)keff;
  return w;
}

dsp_wave_t *dsp_wave_new_basic(int type, double rate) {
  int S = wave_size_for_rate(rate), half = S / 2;
  float *imag = calloc((size_t)half, sizeof(float));
  if (!imag) return NULL;
  for (int n = 1; n < half; n++) {
    float pi_factor = 2.0f / ((float)n * 3.14159265358979f);
    float b = 0;
    switch (type) {
      case DSP_WAVE_SINE: b = n == 1 ? 1.0f : 0.0f; break;
      case DSP_WAVE_SQUARE: b = pi_factor * ((n & 1) ? 2.0f : 0.0f); break;
      case DSP_WAVE_SAWTOOTH: b = pi_factor * ((n & 1) ? 1.0f : -1.0f); break;
      case DSP_WAVE_TRIANGLE:
        b = (n & 1) ? 2.0f * (pi_factor * pi_factor) * ((((n - 1) >> 1) & 1) ? -1.0f : 1.0f) : 0.0f;
        break;
      default: break;
    }
    imag[n] = b;
  }
  int ncomp = type == DSP_WAVE_SINE ? 2 : half;
  dsp_wave_t *w = dsp_wave_new(NULL, imag, ncomp, true, rate);
  free(imag);
  return w;
}

void dsp_wave_free(dsp_wave_t *w) {
  if (!w) return;
  free(w->mem);
  free(w);
}

/* --------------------------------------------------------------------------------------- biquad */
static void bq_norm(dsp_biquad_t *b, double b0, double b1, double b2, double a0, double a1, double a2) {
  double s = 1.0 / a0;
  b->b0 = b0 * s; b->b1 = b1 * s; b->b2 = b2 * s; b->a1 = a1 * s; b->a2 = a2 * s;
}

void dsp_biquad_reset(dsp_biquad_t *b) {
  memset(b, 0, sizeof *b);
  b->b0 = 1;
}

void dsp_biquad_set_lphp(dsp_biquad_t *b, int highpass, double freq, double qlin, double rate) {
  double w = freq / (0.5 * rate);
  if (w < 0) w = 0;
  if (w > 1) w = 1;
  if (!highpass) {
    if (w == 1) bq_norm(b, 1, 0, 0, 1, 0, 0);
    else if (w > 0) {
      double th = DSP_PI * w, alpha = sin(th) / (2 * qlin), cw = cos(th), beta = (1 - cw) / 2;
      bq_norm(b, beta, 2 * beta, beta, 1 + alpha, -2 * cw, 1 - alpha);
    } else bq_norm(b, 0, 0, 0, 1, 0, 0);
  } else {
    if (w == 1) bq_norm(b, 0, 0, 0, 1, 0, 0);
    else if (w > 0) {
      double th = DSP_PI * w, alpha = sin(th) / (2 * qlin), cw = cos(th), beta = (1 + cw) / 2;
      bq_norm(b, beta, -2 * beta, beta, 1 + alpha, -2 * cw, 1 - alpha);
    } else bq_norm(b, 1, 0, 0, 1, 0, 0);
  }
}

void dsp_biquad_set(dsp_biquad_t *b, int type, double freq, double q, double gain_db, double rate) {
  double w = freq / (0.5 * rate);
  double a = pow(10.0, gain_db / 40);
  switch (type) {
    case DSP_LOWPASS: case DSP_HIGHPASS:
      dsp_biquad_set_lphp(b, type == DSP_HIGHPASS, freq, pow(10.0, q / 20), rate);
      return;
    case DSP_BANDPASS:
      if (w < 0) w = 0;
      if (q < 0) q = 0;
      if (w > 0 && w < 1) {
        double w0 = DSP_PI * w;
        if (q > 0) {
          double alpha = sin(w0) / (2 * q), k = cos(w0);
          bq_norm(b, alpha, 0, -alpha, 1 + alpha, -2 * k, 1 - alpha);
        } else bq_norm(b, 1, 0, 0, 1, 0, 0);
      } else bq_norm(b, 0, 0, 0, 1, 0, 0);
      return;
    case DSP_LOWSHELF:
      if (w < 0) w = 0;
      if (w > 1) w = 1;
      if (w == 1) bq_norm(b, a * a, 0, 0, 1, 0, 0);
      else if (w > 0) {
        double w0 = DSP_PI * w, s = 1, alpha = 0.5 * sin(w0) * sqrt((a + 1 / a) * (1 / s - 1) + 2);
        double k = cos(w0), k2 = 2 * sqrt(a) * alpha, ap = a + 1, am = a - 1;
        bq_norm(b, a * (ap - am * k + k2), 2 * a * (am - ap * k), a * (ap - am * k - k2),
                ap + am * k + k2, -2 * (am + ap * k), ap + am * k - k2);
      } else bq_norm(b, 1, 0, 0, 1, 0, 0);
      return;
    case DSP_HIGHSHELF:
      if (w < 0) w = 0;
      if (w > 1) w = 1;
      if (w == 1) bq_norm(b, 1, 0, 0, 1, 0, 0);
      else if (w > 0) {
        double w0 = DSP_PI * w, s = 1, alpha = 0.5 * sin(w0) * sqrt((a + 1 / a) * (1 / s - 1) + 2);
        double k = cos(w0), k2 = 2 * sqrt(a) * alpha, ap = a + 1, am = a - 1;
        bq_norm(b, a * (ap + am * k + k2), -2 * a * (am + ap * k), a * (ap + am * k - k2),
                ap - am * k + k2, 2 * (am - ap * k), ap - am * k - k2);
      } else bq_norm(b, a * a, 0, 0, 1, 0, 0);
      return;
    case DSP_PEAKING:
      if (w < 0) w = 0;
      if (w > 1) w = 1;
      if (q < 0) q = 0;
      if (w > 0 && w < 1) {
        if (q > 0) {
          double w0 = DSP_PI * w, alpha = sin(w0) / (2 * q), k = cos(w0);
          bq_norm(b, 1 + alpha * a, -2 * k, 1 - alpha * a, 1 + alpha / a, -2 * k, 1 - alpha / a);
        } else bq_norm(b, a * a, 0, 0, 1, 0, 0);
      } else bq_norm(b, 1, 0, 0, 1, 0, 0);
      return;
    case DSP_NOTCH:
      if (w < 0) w = 0;
      if (w > 1) w = 1;
      if (q < 0) q = 0;
      if (w > 0 && w < 1) {
        if (q > 0) {
          double w0 = DSP_PI * w, alpha = sin(w0) / (2 * q), k = cos(w0);
          bq_norm(b, 1, -2 * k, 1, 1 + alpha, -2 * k, 1 - alpha);
        } else bq_norm(b, 0, 0, 0, 1, 0, 0);
      } else bq_norm(b, 1, 0, 0, 1, 0, 0);
      return;
    case DSP_ALLPASS:
      if (w < 0) w = 0;
      if (w > 1) w = 1;
      if (q < 0) q = 0;
      if (w > 0 && w < 1) {
        if (q > 0) {
          double w0 = DSP_PI * w, alpha = sin(w0) / (2 * q), k = cos(w0);
          bq_norm(b, 1 - alpha, -2 * k, 1 + alpha, 1 + alpha, -2 * k, 1 - alpha);
        } else bq_norm(b, -1, 0, 0, 1, 0, 0);
      } else bq_norm(b, 1, 0, 0, 1, 0, 0);
      return;
    default:
      bq_norm(b, 1, 0, 0, 1, 0, 0);
  }
}

void dsp_biquad_flush(dsp_biquad_t *b) {
  if (fabs(b->x1) < 1e-30) b->x1 = 0;
  if (fabs(b->x2) < 1e-30) b->x2 = 0;
  if (fabs(b->y1) < 1e-30) b->y1 = 0;
  if (fabs(b->y2) < 1e-30) b->y2 = 0;
}

double dsp_biquad_mag(const dsp_biquad_t *b, double freq, double rate) {
  double w = 2 * DSP_PI * freq / rate;
  double c1 = cos(w), s1 = sin(w), c2 = cos(2 * w), s2 = sin(2 * w);
  double nr = b->b0 + b->b1 * c1 + b->b2 * c2, ni = -(b->b1 * s1 + b->b2 * s2);
  double dr = 1 + b->a1 * c1 + b->a2 * c2, di = -(b->a1 * s1 + b->a2 * s2);
  return sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

/* ------------------------------------------------------------------------------------------ pan */
void dsp_pan_mono_gains(float pan, double *gl, double *gr) {
  double p = pan < -1 ? -1 : pan > 1 ? 1 : pan;
  double a = (p * 0.5 + 0.5) * (DSP_PI / 2);
  *gl = cos(a); *gr = sin(a);
}

void dsp_pan_stereo(float pan, float inl, float inr, float *l, float *r) {
  double p = pan < -1 ? -1 : pan > 1 ? 1 : pan;
  if (p <= 0) {
    double a = (p + 1) * (DSP_PI / 2), gl = cos(a), gr = sin(a);
    *l = (float)(inl + inr * gl); *r = (float)(inr * gr);
  } else {
    double a = p * (DSP_PI / 2), gl = cos(a), gr = sin(a);
    *l = (float)(inl * gl); *r = (float)(inr + inl * gr);
  }
}

/* --------------------------------------------------------------------------------------- shaper */
void dsp_shaper_tanh_curve(float *curve, int n, double drive) {
  double d = tanh(drive);
  for (int i = 0; i < n; i++) {
    double x = 2.0 * i / (n - 1) - 1.0;
    curve[i] = (float)(tanh(drive * x) / d);
  }
}

float dsp_shaper_curve(const float *curve, int n, float x) {
  double v = 0.5 * ((double)x + 1) * (n - 1);
  if (v < 0) return curve[0];
  if (v >= n - 1) return curve[n - 1];
  unsigned i1 = (unsigned)v;
  double f = v - i1;
  return (float)((1.0 - f) * curve[i1] + f * curve[i1 + 1]);
}

static void shaper_kernels(dsp_shaper_t *s) {
  const double alpha = 0.16, a0 = 0.5 * (1.0 - alpha), a1 = 0.5, a2 = 0.5 * alpha;
  /* UpSampler: sinc with a -0.5 sub-sample offset, 128 taps */
  int n = DSP_UP_TAPS, half = n / 2;
  double off = -0.5;
  for (int i = 0; i < n; i++) {
    double sx = DSP_PI * (i - half - off);
    double sinc = sx == 0 ? 1.0 : sin(sx) / sx;
    double x = (i - off) / n;
    double win = a0 - a1 * cos(2 * DSP_PI * x) + a2 * cos(2 * DSP_PI * 2.0 * x);
    s->up[i] = (float)(sinc * win);
  }
  /* DownSampler: 256-tap half-band (0.5 sinc(pi m / 2)), only the odd taps are stored */
  int size = 2 * DSP_DOWN_TAPS, hs = size / 2;
  for (int i = 1; i < size; i += 2) {
    double sx = 0.5 * DSP_PI * (i - hs);
    double sinc = sx == 0 ? 1.0 : sin(sx) / sx;
    sinc *= 0.5;
    double x = (double)i / size;
    double win = a0 - a1 * cos(2 * DSP_PI * x) + a2 * cos(2 * DSP_PI * 2.0 * x);
    s->down[(i - 1) / 2] = (float)(sinc * win);
  }
}

void dsp_shaper_init(dsp_shaper_t *s, const float *curve, int n, int oversample) {
  memset(s, 0, sizeof *s);
  if (n > DSP_SHAPER_CURVE_MAX) n = DSP_SHAPER_CURVE_MAX;
  if (n < 2) { s->curve[0] = -1; s->curve[1] = 1; n = 2; }
  else memcpy(s->curve, curve, sizeof(float) * (size_t)n);
  s->ncurve = n;
  s->oversample = oversample >= 2 ? 2 : 1;
  shaper_kernels(s);
  for (int j = 0; j < DSP_UP_TAPS; j++) s->upr[j] = s->up[DSP_UP_TAPS - 1 - j];
  for (int j = 0; j < DSP_DOWN_TAPS; j++) s->downr[j] = s->down[DSP_DOWN_TAPS - 1 - j];
}

/* sum a[i] b[i], i < n (n % 8 == 0), in 8 independent partial sums: vectorizable without
 * reassociation (Chromium's convolver is SIMD too; its rounding order is not ours either way) */
static inline float dot8(const float *a, const float *b, int n) {
  float t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0, t5 = 0, t6 = 0, t7 = 0;
  for (int i = 0; i < n; i += 8) {
    t0 += a[i] * b[i]; t1 += a[i + 1] * b[i + 1]; t2 += a[i + 2] * b[i + 2]; t3 += a[i + 3] * b[i + 3];
    t4 += a[i + 4] * b[i + 4]; t5 += a[i + 5] * b[i + 5]; t6 += a[i + 6] * b[i + 6]; t7 += a[i + 7] * b[i + 7];
  }
  return ((t0 + t1) + (t2 + t3)) + ((t4 + t5) + (t6 + t7));
}

void dsp_shaper_reset(dsp_shaper_t *s) {
  memset(s->xh, 0, sizeof s->xh);
  memset(s->oh, 0, sizeof s->oh);
  s->pos = 0;
}

float dsp_shaper_run(dsp_shaper_t *s, float x) {
  if (s->oversample < 2) return dsp_shaper_curve(s->curve, s->ncurve, x);
  int p = s->pos;
  s->xh[p] = x; s->xh[p + 256] = x;
  const float *xn = &s->xh[p + 256];            /* xn[-j] = x[n - j] */
  /* up: odd[n] = sum_j up[j] x[n-j] = sum_m upr[m] x[n-127+m] */
  float odd = dot8(s->upr, xn - (DSP_UP_TAPS - 1), DSP_UP_TAPS);
  float so = dsp_shaper_curve(s->curve, s->ncurve, odd);
  /* down: d[n] = sum_j down[j] * so[n-1-j] + 0.5 * shape(x[n-128]) (the even 2x sample) */
  const float *on = &s->oh[p + 256];            /* on[-j] = so[n - j] (so[n] written below) */
  float d = dot8(s->downr, on - DSP_DOWN_TAPS, DSP_DOWN_TAPS);
  d += 0.5f * dsp_shaper_curve(s->curve, s->ncurve, xn[-128]);
  s->oh[p] = so; s->oh[p + 256] = so;
  s->pos = (p + 1) & 255;
  return d;
}

/* ----------------------------------------------------------------------------------- compressor */
/* (Chromium dynamics_compressor.cc, float arithmetic as there; KneeCurve's exp in double) */
static float comp_knee_curve(const dsp_comp_t *c, float x, float k) {
  if (x < c->lin_thr) return x;
  return c->lin_thr + (1 - (float)exp((double)(-k * (x - c->lin_thr)))) / k;
}

static float comp_saturate(const dsp_comp_t *c, float x, float k) {
  if (x < c->knee_thr) return comp_knee_curve(c, x, k);
  float x_db = dsp_lin2db(x);
  float y_db = c->y_knee_db + c->slope * (x_db - c->knee_thr_db);
  return dsp_db2lin(y_db);
}

static float comp_k_at_slope(const dsp_comp_t *c, float desired) {
  const float x_db = c->thr + c->knee;
  const float x = dsp_db2lin(x_db);
  float x2 = 1, x2_db = 0;
  if (!(x < c->lin_thr)) { x2 = (float)(x * 1.001); x2_db = dsp_lin2db(x2); }
  float min_k = 0.1f, max_k = 10000, k = 5, slope = 1;
  for (int i = 0; i < 15; ++i) {
    if (!(x < c->lin_thr)) {
      float y_db = dsp_lin2db(comp_knee_curve(c, x, k)), y2_db = dsp_lin2db(comp_knee_curve(c, x2, k));
      slope = (y2_db - y_db) / (x2_db - x_db);
    }
    if (slope < desired) max_k = k; else min_k = k;
    k = sqrtf(min_k * max_k);
  }
  return k;
}

void dsp_comp_reset(dsp_comp_t *c) {
  c->det = 0; c->gain = 1; c->max_att_db = -1; c->env_rate = 1; c->scaled_desired = 1;
  c->phase = 0;
  memset(c->buf, 0, sizeof c->buf);
  c->ri = 0; c->wi = c->pre;
}

void dsp_comp_init(dsp_comp_t *c, double rate, float thr_db, float knee_db, float ratio,
                   float attack_s, float release_s) {
  memset(c, 0, sizeof *c);
  c->rate = (float)rate; c->thr = thr_db; c->knee = knee_db; c->ratio = ratio;
  c->attack = attack_s; c->release = release_s;
  c->lin_thr = dsp_db2lin(thr_db);
  c->slope = 1 / ratio;
  c->k = comp_k_at_slope(c, 1 / ratio);
  c->knee_thr_db = thr_db + knee_db;
  c->knee_thr = dsp_db2lin(c->knee_thr_db);
  c->y_knee_db = dsp_lin2db(comp_knee_curve(c, c->knee_thr, c->k));
  float full = comp_saturate(c, 1, c->k);
  c->makeup = powf(1 / full, 0.6f);                          /* post gain 0 dB */
  float att = attack_s > 0.001f ? attack_s : 0.001f;
  c->attack_frames = att * c->rate;
  float release_frames = c->rate * release_s;
  c->sat_release_frames = 0.0025f * c->rate;
  /* the release polynomial through (0, z1) .. (3, z4) x release_frames, Chromium's float bases */
  const float z1 = 0.09f, z2 = 0.16f, z3 = 0.42f, z4 = 0.98f;
  const float ka = 0.9999999999999998f * z1 + 1.8432219684323923e-16f * z2 - 1.9373394351676423e-16f * z3 +
                   8.824516011816245e-18f * z4;
  const float kb = -1.5788320352845888f * z1 + 2.3305837032074286f * z2 - 0.9141194204840429f * z3 +
                   0.1623677525612032f * z4;
  const float kc = 0.5334142869106424f * z1 - 1.272736789213631f * z2 + 0.9258856042207512f * z3 -
                   0.18656310191776226f * z4;
  const float kd = 0.08783463138207234f * z1 - 0.1694162967925622f * z2 + 0.08588057951595272f * z3 -
                   0.00429891410546283f * z4;
  const float ke = -0.042416883008123074f * z1 + 0.1115693827987602f * z2 - 0.09764676325265872f * z3 +
                   0.028494263462021576f * z4;
  c->ka = release_frames * ka; c->kb = release_frames * kb; c->kc = release_frames * kc;
  c->kd = release_frames * kd; c->ke = release_frames * ke;
  unsigned pre = (unsigned)(0.006f * c->rate);
  if (pre > DSP_COMP_MAX_PREDELAY - 1) pre = DSP_COMP_MAX_PREDELAY - 1;
  c->pre = (int)pre;
  dsp_comp_reset(c);
}

float dsp_comp_curve(const dsp_comp_t *c, float x) { return comp_saturate(c, fabsf(x), c->k); }

static void comp_division(dsp_comp_t *c) {
  if (isnan(c->det) || isinf(c->det)) c->det = 1;
  float desired = c->det;
  float sdg = asinf(desired) / 1.5707963267948966f;
  c->scaled_desired = sdg;
  int releasing = sdg > c->gain;
  float diff_db = dsp_lin2db(c->gain / sdg);
  if (releasing) {
    c->max_att_db = -1;
    if (isnan(diff_db) || isinf(diff_db)) diff_db = -1;
    float x = diff_db;
    if (x < -12) x = -12;
    if (x > 0) x = 0;
    x = 0.25f * (x + 12);
    float x2 = x * x, x3 = x2 * x, x4 = x2 * x2;
    float release_frames = c->ka + c->kb * x + c->kc * x2 + c->kd * x3 + c->ke * x4;
    c->env_rate = dsp_db2lin(5.0f / release_frames);
  } else {
    if (isnan(diff_db) || isinf(diff_db)) diff_db = 1;
    if (c->max_att_db == -1 || c->max_att_db < diff_db) c->max_att_db = diff_db;
    float eff = c->max_att_db > 0.5f ? c->max_att_db : 0.5f;
    c->env_rate = 1 - powf(0.25f / eff, 1 / c->attack_frames);
  }
}

void dsp_comp_process(dsp_comp_t *c, float *l, float *r, int n) {
  const float k = c->k, sat_frames = c->sat_release_frames;
  for (int i = 0; i < n; i++) {
    if (c->phase == 0) comp_division(c);
    float xl = l[i], xr = r[i];
    c->buf[0][c->wi] = xl; c->buf[1][c->wi] = xr;
    float al = fabsf(xl), ar = fabsf(xr);
    float in = al > ar ? al : ar;
    float shaped = comp_saturate(c, in, k);
    float att = in <= 0.0001f ? 1 : shaped / in;
    float att_db = -dsp_lin2db(att);
    if (att_db < 2.0f) att_db = 2.0f;
    float sat_rate = dsp_db2lin(att_db / sat_frames) - 1;
    float rate = att > c->det ? sat_rate : 1;
    c->det += (att - c->det) * rate;
    if (c->det > 1.0f) c->det = 1.0f;
    if (isnan(c->det) || isinf(c->det)) c->det = 1;
    if (c->env_rate < 1) c->gain += (c->scaled_desired - c->gain) * c->env_rate;
    else { c->gain *= c->env_rate; if (c->gain > 1.0f) c->gain = 1.0f; }
    float g = c->makeup * (float)sin((double)(1.5707963267948966f * c->gain));
    l[i] = c->buf[0][c->ri] * g;
    r[i] = c->buf[1][c->ri] * g;
    c->ri = (c->ri + 1) & (DSP_COMP_MAX_PREDELAY - 1);
    c->wi = (c->wi + 1) & (DSP_COMP_MAX_PREDELAY - 1);
    if (++c->phase == 32) {
      c->phase = 0;
      if (fabsf(c->det) < FLT_MIN) c->det = 0;
      if (fabsf(c->gain) < FLT_MIN) c->gain = 0;
    }
  }
}

/* ---------------------------------------------------------------------------------------- delay */
int dsp_delay_init(dsp_delay_t *d, int max_frames) {
  int size = 4;
  while (size < max_frames + 2) size <<= 1;
  d->buf = calloc((size_t)size, sizeof(float));
  if (!d->buf) { d->size = d->mask = d->w = 0; return -1; }
  d->size = size; d->mask = size - 1; d->w = 0;
  return 0;
}

void dsp_delay_free(dsp_delay_t *d) {
  free(d->buf);
  d->buf = NULL; d->size = d->mask = d->w = 0;
}

void dsp_delay_clear(dsp_delay_t *d) {
  if (d->buf) memset(d->buf, 0, sizeof(float) * (size_t)d->size);
  d->w = 0;
}
