/* test_dsp: the WebAudio building blocks (dsp.h) against analytic values and the constants of the
 * notes (audio-motor.md §4, §13.1): random stream, setTargetAtTime smoothing (Chromium's discrete
 * form), the AudioParam timeline, biquad responses, band-limited wavetable oscillators and their
 * normalisation, the stereo panner, the wave shaper (curve, 2x latency), Chromium's compressor
 * (static curve, makeup, attack/release, look-ahead), buffer readers, the delay line. */
#include "t.h"
#include "dsp.h"

#include <math.h>
#include <stdint.h>

static double db20(double x) { return 20 * log10(x > 1e-30 ? x : 1e-30); }

/* ------------------------------------------------------------------------------------ random */
static void test_rng(void) {
  /* the harness's Math.random replacement in node: seeds 1 and 0x12345678, draws 1, 2, 3, 1001 */
  static const double want[2][4] = {
    {0.62707394058816135, 0.0027357211802154779, 0.52744703995995224, 0.22846644930541515},
    {0.10615200875326991, 0.94127628416754305, 0.93987061525695026, 0.34271810506470501}};
  static const uint32_t seeds[2] = {1, 0x12345678};
  for (int k = 0; k < 2; k++) {
    dsp_rng_t r;
    dsp_rng_seed(&r, seeds[k]);
    double v[4];
    v[0] = dsp_rng(&r); v[1] = dsp_rng(&r); v[2] = dsp_rng(&r);
    for (int i = 0; i < 997; i++) dsp_rng(&r);
    v[3] = dsp_rng(&r);
    for (int i = 0; i < 4; i++) CHECK(v[i] == want[k][i]);
  }
  /* noise buffer: uniform [-1, 1), mean ~0, rms 1/sqrt(3) */
  static float nz[96000];
  dsp_rng_t r;
  dsp_rng_seed(&r, 5);
  dsp_noise_fill(nz, 96000, &r);
  double m = 0, e = 0, lo = 1, hi = -1;
  for (int i = 0; i < 96000; i++) { m += nz[i]; e += nz[i] * (double)nz[i]; lo = fmin(lo, nz[i]); hi = fmax(hi, nz[i]); }
  CHECK_NEAR(m / 96000, 0, 0.01);
  CHECK_NEAR(sqrt(e / 96000), 0.57735, 0.003);
  CHECK(lo >= -1 && hi < 1);
}

/* ----------------------------------------------------------------------------- smoothing */
static void test_smooth(void) {
  /* coefficients of the notes' table (§4.1) */
  CHECK_NEAR(dsp_tau_coef(0.008, 48000), 2.6008e-3, 1e-7);
  CHECK_NEAR(dsp_tau_coef(0.03, 48000), 6.9420e-4, 1e-8);
  CHECK_NEAR(dsp_tau_coef(0.1, 48000), 2.0831e-4, 1e-8);
  CHECK_NEAR(dsp_tau_coef(0.06, 44100), 3.7786e-4, 1e-8);
  CHECK_NEAR(dsp_tau_coef(0, 48000), 1.0, 0);

  /* step response from rest: the first sample holds, then v = T + (V0 - T) e^(-n / (fs tau)) */
  const double fs = 48000, tau = 0.01;
  dsp_smooth_t s;
  dsp_smooth_init(&s, 0, (float)dsp_tau_coef(tau, fs));
  dsp_smooth_set(&s, 1);
  float buf[DSP_RENDER_QUANTUM * 8];
  for (int q = 0; q < 8; q++) dsp_smooth_fill(&s, buf + q * DSP_RENDER_QUANTUM, DSP_RENDER_QUANTUM);
  CHECK(buf[0] == 0.0f);
  double worst = 0;
  for (int n = 1; n < 8 * DSP_RENDER_QUANTUM; n++) worst = fmax(worst, fabs(buf[n] - (1 - exp(-n / (fs * tau)))));
  printf("smooth: step response max error vs 1 - e^(-t/tau): %.2g\n", worst);
  CHECK(worst < 2e-6);

  /* re-issued target (each frame): the first sample is one more step towards the OLD target */
  dsp_smooth_init(&s, 100, (float)dsp_tau_coef(0.02, fs));
  dsp_smooth_set(&s, 200);
  for (int q = 0; q < 3; q++) dsp_smooth_fill(&s, buf, DSP_RENDER_QUANTUM);
  float v = s.v, c = s.c;
  dsp_smooth_set(&s, 50);
  dsp_smooth_fill(&s, buf, DSP_RENDER_QUANTUM);
  float first = v + (200 - v) * c;
  CHECK_NEAR(buf[0], first, 1e-4);
  CHECK(buf[1] < buf[0]);                            /* then towards 50 */
  CHECK_NEAR(buf[1], first + (50 - first) * c, 2e-4);

  /* convergence: Chromium's snap lands exactly on the target. In float, a plain one-pole stalls
   * short of it (ulp/(2c) away): 281 Hz with tau 20 ms would end 0.014 Hz off */
  dsp_smooth_t a, b;
  float ca = (float)dsp_tau_coef(0.02, 46080);
  dsp_smooth_init(&a, 200, ca); dsp_smooth_init(&b, 200, ca);
  dsp_smooth_set(&a, 281.12f); dsp_smooth_set(&b, 281.12f);
  for (int q = 0; q < 46080 / DSP_RENDER_QUANTUM; q++) {
    dsp_smooth_fill(&a, buf, DSP_RENDER_QUANTUM);
    for (int i = 0; i < DSP_RENDER_QUANTUM; i++) dsp_smooth_next(&b);
  }
  printf("smooth: after 1 s: Chromium form %.6f Hz, plain recursion %.6f Hz (target 281.12)\n", a.v, b.v);
  CHECK(a.v == 281.12f);
  CHECK(buf[DSP_RENDER_QUANTUM - 1] == 281.12f);
  CHECK(fabsf(b.v - 281.12f) > 4.54e-5f * 281.12f);  /* (why dsp_smooth_fill unrolls like Chromium) */
  /* to zero: |v| < 4.54e-5 snaps to 0 */
  dsp_smooth_init(&a, 1, (float)dsp_tau_coef(0.01, 48000));
  dsp_smooth_set(&a, 0);
  for (int q = 0; q < 60; q++) dsp_smooth_fill(&a, buf, DSP_RENDER_QUANTUM);
  CHECK(a.v == 0.0f && buf[DSP_RENDER_QUANTUM - 1] == 0.0f);

  /* segment splits inside a quantum do not change the curve much (production renders) */
  dsp_smooth_init(&a, 0, (float)dsp_tau_coef(0.03, 48000)); dsp_smooth_set(&a, 1);
  dsp_smooth_init(&b, 0, (float)dsp_tau_coef(0.03, 48000)); dsp_smooth_set(&b, 1);
  float x[DSP_RENDER_QUANTUM], y[DSP_RENDER_QUANTUM];
  dsp_smooth_fill(&a, x, DSP_RENDER_QUANTUM);
  dsp_smooth_fill(&b, y, 37); dsp_smooth_fill(&b, y + 37, DSP_RENDER_QUANTUM - 37);
  double dmax = 0;
  for (int i = 0; i < DSP_RENDER_QUANTUM; i++) dmax = fmax(dmax, fabs(x[i] - y[i]));
  CHECK(dmax < 1e-6);

  /* k-rate: one value per quantum with c = 1 - e^(-128/(fs tau)) */
  dsp_smooth_init(&a, 1, (float)dsp_tau_coef(0.1, 48000.0 / DSP_RENDER_QUANTUM));
  CHECK_NEAR(a.c, 1 - exp(-128 / (48000 * 0.1)), 1e-7);
  dsp_smooth_set(&a, 2);
  float kv[40];
  for (int q = 0; q < 40; q++) dsp_smooth_fill(&a, &kv[q], 1);
  CHECK(kv[0] == 1.0f);
  CHECK_NEAR(kv[39], 2 - exp(-39 * 128 / (48000 * 0.1)), 1e-5);

  /* more than 10 tau after the event start the target is exact, whatever the distance */
  dsp_smooth_init(&a, 0, (float)dsp_tau_coef(0.001, 48000));
  dsp_smooth_set(&a, 1e6f);
  for (int q = 0; q < 5; q++) dsp_smooth_fill(&a, buf, DSP_RENDER_QUANTUM);
  CHECK(a.v == 1e6f);
}

/* -------------------------------------------------------------------------- param timeline */
static void test_param(void) {
  const double fs = 46080;
  dsp_param_t p;
  float g[4096];
  /* the beep envelope: 0.0001 at t0 -> exp -> 0.045 at t0 + 6 ms, hold, 0.045 at t1 - 12 ms -> exp
   * -> 0.0001 at t1 */
  double t0 = 0.25, t1 = t0 + 0.06;
  dsp_param_init(&p, 1, fs);
  CHECK(dsp_param_set(&p, 0.0001f, t0));
  CHECK(dsp_param_exp(&p, 0.045f, t0 + 0.006));
  CHECK(dsp_param_set(&p, 0.045f, t1 - 0.012));
  CHECK(dsp_param_exp(&p, 0.0001f, t1));
  int64_t f0 = (int64_t)(t0 * fs) - 100;
  for (int k = 0; k < 4; k++) dsp_param_render(&p, f0 + k * 1024, g + k * 1024, 1024);
  double worst = 0;
  for (int i = 0; i < 4096; i++) {
    double t = (double)(f0 + i) / fs, want;
    if ((double)(f0 + i) < t0 * fs) want = 1;
    else if (t < t0 + 0.006) want = 0.0001 * pow(450.0, (t - t0) / 0.006);
    else if ((double)(f0 + i) < (t1 - 0.012) * fs) want = 0.045;
    else if (t < t1) want = 0.045 * pow(0.0001 / 0.045, (t - (t1 - 0.012)) / 0.012);
    else want = 0.0001;
    worst = fmax(worst, fabs(g[i] - want) / want);
  }
  printf("param: beep envelope max relative error %.2g\n", worst);
  CHECK(worst < 1e-5);
  /* an event at time t takes effect at frame ceil(t * fs) */
  dsp_param_init(&p, 0, 48000);
  dsp_param_set(&p, 1, 100.5 / 48000);
  dsp_param_render(&p, 0, g, 200);
  CHECK(g[100] == 0.0f && g[101] == 1.0f);
  /* linear ramp, then setTarget, then cancel */
  dsp_param_init(&p, 0, 1000);
  dsp_param_set(&p, 0, 0);
  dsp_param_lin(&p, 1, 0.1);
  dsp_param_target(&p, 0, 0.2, 0.05f);
  dsp_param_render(&p, 0, g, 400);
  CHECK_NEAR(g[50], 0.5, 1e-6);
  CHECK_NEAR(g[150], 1, 1e-6);
  CHECK_NEAR(g[250], exp(-1.0), 1e-5);
  CHECK(!dsp_param_idle(&p, 0.3));
  dsp_param_cancel(&p, 0.35);
  dsp_param_set(&p, 0.25f, 0.35);
  dsp_param_render(&p, 400, g, 10);
  CHECK_NEAR(g[0], 0.25, 1e-6);
  CHECK(dsp_param_idle(&p, 0.41));
  CHECK_NEAR(dsp_param_at(&p, 0.5), 0.25, 1e-6);
}

/* ----------------------------------------------------------------------------------- biquads */
static double run_sine(dsp_biquad_t *b, double f, double fs) {    /* steady-state gain */
  double e_in = 0, e_out = 0;
  for (int n = 0; n < 48000; n++) {
    double x = sin(2 * DSP_PI * f * n / fs);
    double y = dsp_biquad_run(b, (float)x);
    if (n >= 24000) { e_in += x * x; e_out += y * y; }
  }
  return sqrt(e_out / e_in);
}

static void test_biquad(void) {
  const double fs = 48000;
  dsp_biquad_t b;
  /* lowpass, Q in dB: |H(fc)| = 10^(Q/20), DC 1, Nyquist 0 */
  dsp_biquad_reset(&b);
  dsp_biquad_set(&b, DSP_LOWPASS, 1000, 0.8, 0, fs);
  CHECK_NEAR(db20(dsp_biquad_mag(&b, 1000, fs)), 0.8, 1e-6);
  CHECK_NEAR(dsp_biquad_mag(&b, 0, fs), 1, 1e-9);
  CHECK(dsp_biquad_mag(&b, fs / 2, fs) < 1e-9);
  CHECK(db20(dsp_biquad_mag(&b, 8000, fs)) < -30);           /* 12 dB/octave above fc */
  CHECK_NEAR(run_sine(&b, 1000, fs), pow(10, 0.8 / 20), 2e-3);
  dsp_biquad_set(&b, DSP_LOWPASS, 1000, 0, 0, fs);
  CHECK_NEAR(db20(dsp_biquad_mag(&b, 1000, fs)), 0.0, 1e-6);
  /* the same via the precomputed-Qlin setter */
  dsp_biquad_t b2;
  dsp_biquad_reset(&b2);
  dsp_biquad_set_lphp(&b2, 0, 1000, 1.0, fs);
  CHECK(b2.b0 == b.b0 && b2.a1 == b.a1 && b2.a2 == b.a2);
  /* edges: f >= nyquist passes, f <= 0 mutes */
  dsp_biquad_set(&b, DSP_LOWPASS, 30000, 0, 0, fs);
  CHECK(b.b0 == 1 && b.b1 == 0 && b.a1 == 0);
  dsp_biquad_set(&b, DSP_LOWPASS, 0, 0, 0, fs);
  CHECK(b.b0 == 0);
  /* highpass: |H(fc)| = 10^(Q/20), DC 0, Nyquist 1 */
  dsp_biquad_set(&b, DSP_HIGHPASS, 300, 0.5, 0, fs);
  CHECK_NEAR(db20(dsp_biquad_mag(&b, 300, fs)), 0.5, 1e-6);
  CHECK(dsp_biquad_mag(&b, 0, fs) < 1e-9);
  CHECK_NEAR(dsp_biquad_mag(&b, fs / 2, fs), 1, 1e-9);
  /* bandpass, linear Q: 0 dB at f0, -3 dB at the band edges f0 (sqrt(1 + 1/4Q^2) +- 1/2Q) */
  double q = 4, f0 = 2000;
  dsp_biquad_set(&b, DSP_BANDPASS, f0, q, 0, fs);
  CHECK_NEAR(dsp_biquad_mag(&b, f0, fs), 1, 1e-9);
  CHECK(dsp_biquad_mag(&b, 0, fs) < 1e-9 && dsp_biquad_mag(&b, fs / 2, fs) < 1e-9);
  /* (bilinear: the -3 dB points are exact in the pre-warped domain) */
  double w0 = tan(DSP_PI * f0 / fs), r = sqrt(1 + 1 / (4 * q * q));
  double fl = atan(w0 * (r - 1 / (2 * q))) * fs / DSP_PI, fh = atan(w0 * (r + 1 / (2 * q))) * fs / DSP_PI;
  CHECK_NEAR(db20(dsp_biquad_mag(&b, fl, fs)), -3.0103, 1e-3);
  CHECK_NEAR(db20(dsp_biquad_mag(&b, fh, fs)), -3.0103, 1e-3);
  /* highshelf 3800 Hz -6 dB (the motor's): 0 dB at DC, -6.02 at Nyquist, -3 at f0 */
  dsp_biquad_set(&b, DSP_HIGHSHELF, 3800, 1, -6, fs);
  CHECK_NEAR(db20(dsp_biquad_mag(&b, 0, fs)), 0, 1e-6);
  CHECK_NEAR(db20(dsp_biquad_mag(&b, fs / 2, fs)), -6, 1e-6);
  CHECK_NEAR(db20(dsp_biquad_mag(&b, 3800, fs)), -3, 1e-6);
  CHECK_NEAR(db20(run_sine(&b, 12000, fs)), db20(dsp_biquad_mag(&b, 12000, fs)), 0.01);
  /* lowshelf, peaking, notch, allpass */
  dsp_biquad_set(&b, DSP_LOWSHELF, 200, 1, 9, fs);
  CHECK_NEAR(db20(dsp_biquad_mag(&b, 0, fs)), 9, 1e-6);
  CHECK_NEAR(db20(dsp_biquad_mag(&b, fs / 2, fs)), 0, 1e-6);
  dsp_biquad_set(&b, DSP_PEAKING, 1000, 2, -12, fs);
  CHECK_NEAR(db20(dsp_biquad_mag(&b, 1000, fs)), -12, 1e-6);
  CHECK_NEAR(db20(dsp_biquad_mag(&b, 0, fs)), 0, 1e-6);
  dsp_biquad_set(&b, DSP_NOTCH, 1000, 2, 0, fs);
  CHECK(dsp_biquad_mag(&b, 1000, fs) < 1e-9);
  dsp_biquad_set(&b, DSP_ALLPASS, 1000, 2, 0, fs);
  for (double f = 50; f < 23000; f *= 1.7) CHECK_NEAR(dsp_biquad_mag(&b, f, fs), 1, 1e-9);
  /* filter state flush and NaN-free silence */
  dsp_biquad_reset(&b);
  dsp_biquad_set(&b, DSP_LOWPASS, 100, 0, 0, fs);
  dsp_biquad_run(&b, 1);
  for (int i = 0; i < 200000; i++) dsp_biquad_run(&b, 0);
  dsp_biquad_flush(&b);
  CHECK(b.y1 == 0 && b.y2 == 0);
}

/* ------------------------------------------------------------------------------- oscillators */
static double peak_of(const float *t, int n) {
  double m = 0;
  for (int i = 0; i < n; i++) m = fmax(m, fabs(t[i]));
  return m;
}
static void fft(double *re, double *im, int n) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
  }
  for (int len = 2; len <= n; len <<= 1)
    for (int i = 0; i < n; i += len)
      for (int k = 0; k < len / 2; k++) {
        double a = -2 * DSP_PI * k / len, wr = cos(a), wi = sin(a);
        double xr = re[i + k + len / 2], xi = im[i + k + len / 2];
        double vr = xr * wr - xi * wi, vi = xr * wi + xi * wr;
        re[i + k + len / 2] = re[i + k] - vr; im[i + k + len / 2] = im[i + k] - vi;
        re[i + k] += vr; im[i + k] += vi;
      }
}
/* |DFT| of a table at harmonic k, scaled to the sine amplitude */
static double harm(const float *t, int n, int k) {
  double re = 0, im = 0;
  for (int i = 0; i < n; i++) { re += t[i] * cos(2 * DSP_PI * k * i / n); im += t[i] * sin(2 * DSP_PI * k * i / n); }
  return 2 * sqrt(re * re + im * im) / n;
}

static void test_osc(void) {
  const double fs = 48000;
  static const float W0[7] = {0, 1, 0.2f, 0.08f, 0.06f, 0.04f, 0.02f}, W1[7] = {0, 1, 0.8f, 0.6f, 0.35f, 0.3f, 0.15f};
  dsp_wave_t *w0 = dsp_wave_new(NULL, W0, 7, true, fs), *w1 = dsp_wave_new(NULL, W1, 7, true, fs);
  dsp_wave_t *sq = dsp_wave_new_basic(DSP_WAVE_SQUARE, fs), *tri = dsp_wave_new_basic(DSP_WAVE_TRIANGLE, fs);
  dsp_wave_t *saw = dsp_wave_new_basic(DSP_WAVE_SAWTOOTH, fs), *sine = dsp_wave_new_basic(DSP_WAVE_SINE, fs);
  CHECK(w0 && w1 && sq && tri && saw && sine);
  if (!(w0 && w1 && sq && tri && saw && sine)) return;
  /* Chromium's peak normalisation (notes §4.3) */
  printf("osc: norm W0 %.6f W1 %.6f square %.6f triangle %.6f sawtooth %.6f sine %.6f\n", dsp_wave_norm(w0),
         dsp_wave_norm(w1), dsp_wave_norm(sq), dsp_wave_norm(tri), dsp_wave_norm(saw), dsp_wave_norm(sine));
  CHECK_NEAR(dsp_wave_norm(w0), 1.010911, 2e-6);
  CHECK_NEAR(dsp_wave_norm(w1), 0.437348, 2e-6);
  CHECK_NEAR(dsp_wave_norm(sq), 0.848191, 2e-6);
  CHECK_NEAR(dsp_wave_norm(tri), 1.000198, 2e-6);
  CHECK_NEAR(dsp_wave_norm(sine), 1, 1e-6);
  CHECK(w0->size == 4096 && w0->nranges == 36);
  CHECK_NEAR(peak_of(sq->tab[0], 4096), 1, 1e-6);
  CHECK_NEAR(peak_of(w1->tab[0], 4096), 1, 1e-6);
  /* the partials: the square's k-th = N 4/(pi k) for odd k, 0 for even */
  CHECK_NEAR(harm(sq->tab[0], 4096, 1), 0.848191 * 4 / DSP_PI, 1e-5);
  CHECK_NEAR(harm(sq->tab[0], 4096, 7), 0.848191 * 4 / (7 * DSP_PI), 1e-5);
  CHECK(harm(sq->tab[0], 4096, 8) < 1e-5);
  /* band limiting: range r keeps partials k <= 2048 * 2^(-r/3); W0's 6th partial fades out between
   * pitch range 25 and 26: f = (fs/4096) 2^(24.5/3) is half way */
  double f = fs / 4096 * pow(2, 24.5 / 3);
  const float *hi, *lo;
  float tf;
  dsp_wave_select(w0, (float)(f * w0->rate_scale), &hi, &lo, &tf);
  CHECK(hi == w0->tab[25] && lo == w0->tab[26]);
  CHECK_NEAR(tf, 0.5, 1e-4);
  CHECK_NEAR(harm(hi, 4096, 6), 1.010911 * 0.02, 1e-5);
  CHECK(harm(lo, 4096, 6) < 1e-6);
  CHECK_NEAR(harm(lo, 4096, 5), 1.010911 * 0.04, 1e-5);
  /* a square at 3 kHz: pitch range 25 keeps partials k <= 6 (Chromium culls a range early, so the
   * 7th at 21 kHz is already gone), the next range k <= 5 */
  dsp_wave_select(sq, (float)(3000 * sq->rate_scale), &hi, &lo, &tf);
  CHECK(hi == sq->tab[25]);
  CHECK(harm(hi, 4096, 7) < 1e-6 && harm(lo, 4096, 7) < 1e-6);
  CHECK_NEAR(harm(hi, 4096, 5), 0.848191 * 4 / (5 * DSP_PI), 1e-5);
  /* the oscillator: sine at 1234.5 Hz. Its phase step is f * size / fs in float (Chromium's), so
   * the frequency is right to ~1e-7; against that step the table interpolation errs ~1e-6 */
  dsp_osc_t o = {0};
  float incr = 1234.5f * sine->rate_scale;
  CHECK_NEAR(incr * 48000.0 / 4096, 1234.5, 1234.5 * 2e-7);
  double e = 0;
  for (int n = 0; n < 48000; n++) {
    float x = dsp_osc_next(&o, sine, 1234.5f);
    e = fmax(e, fabs(x - sin(2 * DSP_PI * fmod(n * (double)incr, 4096.0) / 4096)));
  }
  printf("osc: sine max error %.2g (against its float phase step)\n", e);
  CHECK(e < 3e-6);
  /* a square: its spectrum has only harmonics (aliases would land between them) */
  static float buf[1 << 15];
  memset(&o, 0, sizeof o);
  double fq = 1500.0 * 32768 / 48000 / 32;          /* a whole number of cycles in 32768 samples */
  fq = round(fq) * 32 * 48000 / 32768;
  for (int n = 0; n < 32768; n++) buf[n] = dsp_osc_next(&o, sq, (float)fq);
  static double re[1 << 15], im[1 << 15];
  for (int n = 0; n < 32768; n++) { re[n] = buf[n]; im[n] = 0; }
  fft(re, im, 32768);
  double eh = 0, en = 0;
  for (int k = 1; k < 16384; k++) {
    double p = re[k] * re[k] + im[k] * im[k];
    double hk = k * 48000.0 / 32768 / fq;
    if (fabs(hk - round(hk)) < 1e-6) eh += p; else en += p;
  }
  printf("osc: square %.1f Hz: energy off the harmonics %.1f dB\n", fq, 10 * log10(en / eh + 1e-30));
  CHECK(10 * log10(en / eh + 1e-30) < -80);
  /* negative and NaN frequencies are clamped (Chromium: NaN -> Nyquist) */
  memset(&o, 0, sizeof o);
  for (int n = 0; n < 100; n++) CHECK(isfinite(dsp_osc_next(&o, sq, n & 1 ? NAN : -1e9f)));
  dsp_wave_free(w0); dsp_wave_free(w1); dsp_wave_free(sq); dsp_wave_free(tri); dsp_wave_free(saw);
  dsp_wave_free(sine);
  dsp_wave_free(NULL);
}

/* --------------------------------------------------------------------------- panner, shaper */
static void test_pan_shaper(void) {
  double gl, gr;
  static const float pan[4] = {-0.55f, 0.55f, -0.3f, 0.3f};
  static const double want[4][2] = {{0.93819, 0.34612}, {0.34612, 0.93819}, {0.85264, 0.52250}, {0.52250, 0.85264}};
  for (int i = 0; i < 4; i++) {
    dsp_pan_mono_gains(pan[i], &gl, &gr);
    CHECK_NEAR(gl, want[i][0], 1e-5);
    CHECK_NEAR(gr, want[i][1], 1e-5);
  }
  dsp_pan_mono_gains(0, &gl, &gr);
  CHECK_NEAR(gl, sqrt(0.5), 1e-12);
  float l, r;
  dsp_pan_stereo(-0.5f, 1, 1, &l, &r);               /* p <= 0: L = l + r cos, R = r sin (x = 0.5) */
  CHECK_NEAR(l, 1 + cos(DSP_PI / 4), 1e-6);
  CHECK_NEAR(r, sin(DSP_PI / 4), 1e-6);
  dsp_pan_stereo(1, 0.5f, 0.25f, &l, &r);
  CHECK_NEAR(l, 0, 1e-6);
  CHECK_NEAR(r, 0.75, 1e-6);

  /* the motor's curve: tanh(1.2x)/tanh(1.2), 257 points; clipped outside [-1, 1] */
  float curve[257];
  dsp_shaper_tanh_curve(curve, 257, 1.2);
  CHECK_NEAR(curve[0], -1, 1e-7);
  CHECK_NEAR(curve[256], 1, 1e-7);
  CHECK_NEAR(curve[192], tanh(0.6) / tanh(1.2), 1e-7);
  CHECK_NEAR(dsp_shaper_curve(curve, 257, 0.3f), tanh(0.36) / tanh(1.2), 1e-5);
  CHECK(dsp_shaper_curve(curve, 257, 5) == curve[256]);
  CHECK(dsp_shaper_curve(curve, 257, -5) == curve[0]);
  /* 2x oversampling (Chromium's kernels): 128 samples of latency; a low tone comes out as the curve
   * of the delayed input */
  static dsp_shaper_t sh;
  dsp_shaper_init(&sh, curve, 257, 2);
  double sum = 0, wsum = 0;
  for (int n = 0; n < 400; n++) {                    /* impulse response centre of mass */
    float y = dsp_shaper_run(&sh, n == 0 ? 1e-3f : 0);
    sum += n * fabs(y); wsum += fabs(y);
  }
  printf("shaper: 2x latency %.2f samples\n", sum / wsum);
  CHECK_NEAR(sum / wsum, 128, 1.0);
  dsp_shaper_reset(&sh);
  double worst = 0;
  for (int n = 0; n < 4800; n++) {
    double x = 0.9 * sin(2 * DSP_PI * 200 * n / 48000.0);
    float y = dsp_shaper_run(&sh, (float)x);
    if (n >= 1000) {
      double xd = 0.9 * sin(2 * DSP_PI * 200 * (n - 128) / 48000.0);
      worst = fmax(worst, fabs(y - tanh(1.2 * xd) / tanh(1.2)));
    }
  }
  printf("shaper: 200 Hz through 2x vs the curve: max error %.2g\n", worst);
  CHECK(worst < 2e-3);
  static dsp_shaper_t s1;
  dsp_shaper_init(&s1, curve, 257, 1);
  CHECK_NEAR(dsp_shaper_run(&s1, 0.5f), tanh(0.6) / tanh(1.2), 1e-5);
}

/* ------------------------------------------------------------------------------- compressor */
static void test_comp(void) {
  static dsp_comp_t c;
  dsp_comp_init(&c, 48000, -20, 18, 5, 0.004f, 0.18f);
  printf("comp: motor k %.4f, saturate(1) %.5f, makeup %.5f (%+.2f dB), look-ahead %d frames\n", c.k,
         dsp_comp_curve(&c, 1), c.makeup, db20(c.makeup), c.pre);
  CHECK_NEAR(c.k, 3.4406, 2e-4);
  CHECK_NEAR(c.y_knee_db, -8.778, 2e-3);
  CHECK_NEAR(dsp_comp_curve(&c, 1), 0.38114, 2e-5);
  CHECK_NEAR(c.makeup, 1.78381, 2e-5);
  CHECK_INT(c.pre, 288);
  /* the static curve of the notes (§4.8): in -> out dB before makeup */
  static const double in_db[7] = {-20, -16, -12, -8, -4, 0, 6}, out_db[7] = {-20.00, -16.31, -13.24, -10.86, -9.27, -8.38, -7.18};
  for (int i = 0; i < 7; i++) CHECK_NEAR(db20(dsp_comp_curve(&c, (float)pow(10, in_db[i] / 20))), out_db[i], 0.01);
  /* dynamically: a 1 kHz sine at each level settles at the curve + makeup (peak detector) */
  static float L[48000], R[48000];
  for (int i = 0; i < 7; i++) {
    dsp_comp_reset(&c);
    double a = pow(10, in_db[i] / 20);
    for (int n = 0; n < 48000; n++) L[n] = R[n] = (float)(a * sin(2 * DSP_PI * 1000 * n / 48000.0));
    dsp_comp_process(&c, L, R, 48000);
    double pk = 0;
    for (int n = 43200; n < 48000; n++) pk = fmax(pk, fabs(L[n]));
    double got = db20(pk), want = out_db[i] + db20(c.makeup);
    if (i == 0 || i == 5) printf("comp: sine at %+.0f dBFS -> %.2f dBFS (static %.2f)\n", in_db[i], got, want);
    CHECK_NEAR(got, want, 0.5);                      /* (the detector releases between the peaks) */
  }
  /* attack: a -30 -> 0 dBFS step reaches the static reduction within tens of ms; release: back to
   * -30 dBFS, the gain recovers over a few hundred ms (adaptive release) */
  dsp_comp_reset(&c);
  float gtr[48000];
  for (int blk = 0; blk < 48000 / 128; blk++) {
    float bl[128], br[128];
    for (int n = 0; n < 128; n++) {
      int k = blk * 128 + n;
      double a = k < 4800 ? pow(10, -30 / 20.0) : k < 24000 ? 1 : pow(10, -30 / 20.0);
      bl[n] = br[n] = (float)(a * sin(2 * DSP_PI * 1000 * k / 48000.0));
    }
    dsp_comp_process(&c, bl, br, 128);
    gtr[blk] = dsp_comp_total_gain(&c);
  }
  double g0 = db20(gtr[4800 / 128 - 2]), g_static = db20(c.makeup) + out_db[5];   /* 0 dBFS in */
  int att = -1, rel = -1;
  for (int b = 4800 / 128; b < 24000 / 128; b++) if (att < 0 && db20(gtr[b]) < g_static + 1) att = b;
  for (int b = 24000 / 128; b < 48000 / 128; b++) if (rel < 0 && db20(gtr[b]) > g0 - 1) rel = b;
  printf("comp: step -30 -> 0 dBFS: within 1 dB of the static gain after %.1f ms; back to -30: within 1 dB after %.0f ms\n",
         att < 0 ? -1.0 : (att * 128 - 4800) / 48.0, rel < 0 ? -1.0 : (rel * 128 - 24000) / 48.0);
  CHECK(att > 0 && (att * 128 - 4800) / 48.0 < 40);
  CHECK(rel > 0 && (rel * 128 - 24000) / 48.0 > 30 && (rel * 128 - 24000) / 48.0 < 600);
  /* look-ahead: the signal is delayed by 288 frames; block splits change nothing */
  dsp_comp_reset(&c);
  memset(L, 0, sizeof L); memset(R, 0, sizeof R);
  L[10] = R[10] = 0.01f;
  dsp_comp_process(&c, L, R, 1000);
  CHECK(L[298] != 0 && L[297] == 0 && L[10] == 0);
  static dsp_comp_t c2;
  dsp_comp_init(&c2, 48000, -20, 18, 5, 0.004f, 0.18f);
  dsp_comp_reset(&c);
  static float A[9600], B[9600], A2[9600], B2[9600];
  for (int n = 0; n < 9600; n++) A[n] = B[n] = A2[n] = B2[n] = (float)(0.8 * sin(n * 0.05) * (n > 4000));
  dsp_comp_process(&c, A, B, 9600);
  for (int off = 0, k = 0; off < 9600; k++) {
    int m = (k * 37 + 11) % 200 + 1;
    if (m > 9600 - off) m = 9600 - off;
    dsp_comp_process(&c2, A2 + off, B2 + off, m);
    off += m;
  }
  CHECK(memcmp(A, A2, sizeof A) == 0);
  /* the music bus compressor (-16, 10, 3) */
  dsp_comp_init(&c, 48000, -16, 10, 3, 0.01f, 0.2f);
  printf("comp: music k %.4f, makeup %.5f (%+.2f dB)\n", c.k, c.makeup, db20(c.makeup));
  /* (the notes' 4.3685 / 1.67518 come from double math; Chromium's float bisection gives these) */
  CHECK_NEAR(c.k, 4.36951, 2e-5);
  CHECK_NEAR(c.makeup, 1.67525, 2e-5);
  dsp_comp_init(&c, 44100, -20, 18, 5, 0.004f, 0.18f);
  CHECK_INT(c.pre, 264);
}

/* ------------------------------------------------------------------------- buffers, delay */
static void test_buffers(void) {
  float buf[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  dsp_loop_t r = {0};
  double step = dsp_loop_step(0.75f, 8000, 48000);
  CHECK_NEAR(step, 0.125, 1e-12);
  CHECK(dsp_loop_step(0.9f, 48000, 48000) == (double)0.9f);
  r.pos = 6.5;
  CHECK_NEAR(dsp_loop_next(&r, buf, 8, 1.0), 6.5, 1e-7);
  CHECK_NEAR(dsp_loop_next(&r, buf, 8, 1.0), 3.5, 1e-7);   /* 7.5: between buf[7] and buf[0] */
  CHECK_NEAR(r.pos, 0.5, 1e-12);
  r.pos = NAN;                                        /* never reads outside the buffer */
  CHECK(dsp_loop_next(&r, buf, 8, 1e9) == 0);
  CHECK(dsp_loop_next(&r, buf, 8, 0.5) == 0);
  bool done = false;
  r.pos = 6;
  CHECK_NEAR(dsp_oneshot_next(&r, buf, 8, 1.5, &done), 6, 1e-7);
  CHECK_NEAR(dsp_oneshot_next(&r, buf, 8, 1.5, &done), 7, 1e-7);   /* 7.5: the last sample holds */
  CHECK(!done);
  CHECK(dsp_oneshot_next(&r, buf, 8, 1.5, &done) == 0 && done);

  dsp_delay_t d;
  CHECK_INT(dsp_delay_init(&d, 1000), 0);
  CHECK(d.size >= 1002);
  for (int i = 0; i < 2000; i++) dsp_delay_write(&d, (float)i);
  CHECK_NEAR(dsp_delay_read(&d, 0), 1999, 1e-4);
  CHECK_NEAR(dsp_delay_read(&d, 10), 1989, 1e-4);
  CHECK_NEAR(dsp_delay_read(&d, 10.25), 1988.75, 1e-3);
  CHECK_NEAR(dsp_delay_read(&d, 1e9), 1999 - (d.size - 2), 1e-3);
  dsp_delay_clear(&d);
  CHECK(dsp_delay_read(&d, 5) == 0);
  dsp_delay_free(&d);
  CHECK(d.buf == NULL);
}

int main(void) {
  test_rng();
  test_smooth();
  test_param();
  test_biquad();
  test_osc();
  test_pan_shaper();
  test_comp();
  test_buffers();
  T_DONE();
}
