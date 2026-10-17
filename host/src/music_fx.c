/* Effects and tables for the generated music (notes audio-music.md 6.2, 6.7):
 *  - a radix-2 complex FFT (split re/im) that can run one pass at a time;
 *  - the three oscillator waves (dsp.h's Chromium PeriodicWave tables);
 *  - the reverb: the browser's ConvolverNode with its 2.6 s noise IR, as a zero-latency
 *    partitioned convolution. Why convolution and not an algorithmic reverb: the IR is the
 *    browser's own (same random numbers, same normalization), so the wet signal matches by
 *    construction, and on a Pi 4 it costs about 65 MFLOP/s (a few % of one A72 core).
 *    Stage 0 uses 128-frame partitions in the quantum itself (no latency, taps 0..2047);
 *    stage 1 (1024-frame partitions, taps from 2048) and stage 2 (8192-frame partitions, taps
 *    from 16384) process a finished block during the next block, one small step per quantum,
 *    so no quantum carries a big FFT. Both channels go through one complex FFT (z = L + iR). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "songs.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ================================================================ FFT */

int mfft_init(mfft_t *f, int n) {
  memset(f, 0, sizeof *f);
  int lg = 0;
  while ((1 << lg) < n) lg++;
  if ((1 << lg) != n) return -1;
  f->n = n; f->log2n = lg;
  f->cs = malloc(sizeof(float) * (size_t)(n / 2 + 1));
  f->sn = malloc(sizeof(float) * (size_t)(n / 2 + 1));
  f->rev = malloc(sizeof(int) * (size_t)n);
  if (!f->cs || !f->sn || !f->rev) { mfft_free(f); return -1; }
  for (int k = 0; k <= n / 2; k++) { f->cs[k] = (float)cos(2 * M_PI * k / n); f->sn[k] = (float)sin(2 * M_PI * k / n); }
  for (int i = 0; i < n; i++) {
    int r = 0;
    for (int b = 0; b < lg; b++) if (i & (1 << b)) r |= 1 << (lg - 1 - b);
    f->rev[i] = r;
  }
  return 0;
}

void mfft_free(mfft_t *f) { free(f->cs); free(f->sn); free(f->rev); f->cs = f->sn = NULL; f->rev = NULL; }

void mfft_pass(const mfft_t *f, float *re, float *im, int pass, bool inverse) {
  int n = f->n;
  if (pass == 0) {
    for (int i = 0; i < n; i++) {
      int r = f->rev[i];
      if (r > i) { float t = re[i]; re[i] = re[r]; re[r] = t; t = im[i]; im[i] = im[r]; im[r] = t; }
    }
    return;
  }
  int half = 1 << (pass - 1), size = half << 1, ts = n / size;
  float sg = inverse ? 1.0f : -1.0f;
  if (half == 1) {
    for (int s = 0; s < n; s += 2) {
      float ar = re[s], ai = im[s], br = re[s + 1], bi = im[s + 1];
      re[s] = ar + br; im[s] = ai + bi; re[s + 1] = ar - br; im[s + 1] = ai - bi;
    }
    return;
  }
  for (int s = 0; s < n; s += size) {
    float *r0 = re + s, *i0 = im + s, *r1 = re + s + half, *i1 = im + s + half;
    for (int j = 0; j < half; j++) {
      float wr = f->cs[j * ts], wi = sg * f->sn[j * ts];
      float br = r1[j] * wr - i1[j] * wi, bi = r1[j] * wi + i1[j] * wr;
      float ar = r0[j], ai = i0[j];
      r0[j] = ar + br; i0[j] = ai + bi; r1[j] = ar - br; i1[j] = ai - bi;
    }
  }
}

void mfft_run(const mfft_t *f, float *re, float *im, bool inverse) {
  for (int p = 0; p <= f->log2n; p++) mfft_pass(f, re, im, p, inverse);
}

/* ================================================================ the oscillator waves (dsp.h) */

int mwaves_build(mwaves_t *w, double sr) {
  w->w[MW_SINE] = dsp_wave_new_basic(DSP_WAVE_SINE, sr);
  w->w[MW_SAW] = dsp_wave_new_basic(DSP_WAVE_SAWTOOTH, sr);
  w->w[MW_SQUARE] = dsp_wave_new_basic(DSP_WAVE_SQUARE, sr);
  if (!w->w[0] || !w->w[1] || !w->w[2]) { mwaves_free(w); return -1; }
  return 0;
}

void mwaves_free(mwaves_t *w) {
  for (int i = 0; i < 3; i++) { dsp_wave_free(w->w[i]); w->w[i] = NULL; }
}

/* ================================================================ partitioned convolution */

#define NSTAGE 3

typedef struct {
  int B, M, P, off, K;                         /* block, quanta per block, partitions, first tap, job steps */
  mfft_t fft;                                  /* 2B points */
  float *Hr[2], *Hi[2];                        /* P * (B + 1) partition spectra, scaled by 1/(2B) */
} stage_ir_t;

struct mconv_ir { stage_ir_t st[NSTAGE]; int nst; double scale; };

typedef struct {
  float *in[2];                                /* 3 blocks of input per channel (ring) */
  float *Xr[2], *Xi[2];                        /* frequency-domain delay line: P * (B + 1) */
  float *Yr[2], *Yi[2];                        /* accumulators: B + 1 */
  float *wr, *wi;                              /* work: 2B */
  float *out[2][2];                            /* [buffer][channel] output blocks of B */
} stage_t;

struct mconv {
  const mconv_ir_t *h;
  stage_t st[NSTAGE];
  int64_t q;                                   /* quanta since reset */
  void *mem; size_t memsz;                     /* every state array, one block (reset = memset) */
};

/* split Z = FFT(xL + i xR) into XL, XR for k = 0..B (N = 2B) */
static void split2(const float *zr, const float *zi, int N, float *Lr, float *Li, float *Rr, float *Ri) {
  int B = N / 2;
  for (int k = 0; k <= B; k++) {
    int k2 = (N - k) & (N - 1);
    float a = zr[k], b = zi[k], c = zr[k2], d = zi[k2];
    Lr[k] = 0.5f * (a + c); Li[k] = 0.5f * (b - d);
    Rr[k] = 0.5f * (b + d); Ri[k] = 0.5f * (c - a);
  }
}

/* Z = YL + i YR over the full N from the half spectra */
static void combine2(const float *Lr, const float *Li, const float *Rr, const float *Ri, int N, float *zr, float *zi) {
  int B = N / 2;
  for (int k = 0; k <= B; k++) { zr[k] = Lr[k] - Ri[k]; zi[k] = Li[k] + Rr[k]; }
  for (int k = B + 1; k < N; k++) { int j = N - k; zr[k] = Lr[j] + Ri[j]; zi[k] = -Li[j] + Rr[j]; }
}

static void cmac(float *yr, float *yi, const float *xr, const float *xi, const float *hr, const float *hi, int n) {
  for (int k = 0; k < n; k++) {
    yr[k] += xr[k] * hr[k] - xi[k] * hi[k];
    yi[k] += xr[k] * hi[k] + xi[k] * hr[k];
  }
}

mconv_ir_t *mconv_ir_new(const float *irL, const float *irR, int n, double sr) {
  mconv_ir_t *h = calloc(1, sizeof *h);
  if (!h) return NULL;
  /* ConvolverNode normalize = true (Chromium Reverb::CalculateNormalizationScale) */
  double pw = 0;
  for (int i = 0; i < n; i++) pw += (double)irL[i] * irL[i] + (double)irR[i] * irR[i];
  double rms = sqrt(pw / (2.0 * n));
  if (rms < 0.000125) rms = 0.000125;
  double scale = (1.0 / rms) * pow(10.0, -58.0 / 20.0) * (44100.0 / sr);
  h->scale = scale;
  static const int BS[NSTAGE] = { 128, 1024, 8192 };
  int off = 0;
  for (int s = 0; s < NSTAGE && off < n; s++) {
    stage_ir_t *S = &h->st[s];
    S->B = BS[s]; S->M = S->B / MQ; S->off = off;
    int lim = s + 1 < NSTAGE ? 2 * BS[s + 1] : n;   /* the next stage starts at 2 x its block */
    if (lim > n) lim = n;
    S->P = (lim - off + S->B - 1) / S->B;
    if (mfft_init(&S->fft, 2 * S->B)) goto fail;
    S->K = 2 * S->fft.log2n + S->P + 4;
    size_t hs = (size_t)S->P * (S->B + 1);
    for (int c = 0; c < 2; c++) {
      S->Hr[c] = calloc(hs, sizeof(float));
      S->Hi[c] = calloc(hs, sizeof(float));
      if (!S->Hr[c] || !S->Hi[c]) goto fail;
    }
    int N2 = 2 * S->B;
    float *zr = calloc((size_t)N2, sizeof(float)), *zi = calloc((size_t)N2, sizeof(float));
    if (!zr || !zi) { free(zr); free(zi); goto fail; }
    float sc = (float)(scale / N2);
    for (int p = 0; p < S->P; p++) {
      memset(zr, 0, sizeof(float) * N2);
      memset(zi, 0, sizeof(float) * N2);
      for (int j = 0; j < S->B; j++) {
        int k = off + p * S->B + j;
        if (k < n) { zr[j] = irL[k] * sc; zi[j] = irR[k] * sc; }
      }
      mfft_run(&S->fft, zr, zi, false);
      size_t o = (size_t)p * (S->B + 1);
      split2(zr, zi, N2, S->Hr[0] + o, S->Hi[0] + o, S->Hr[1] + o, S->Hi[1] + o);
    }
    free(zr); free(zi);
    off += S->P * S->B;
    h->nst = s + 1;
  }
  return h;
fail:
  mconv_ir_free(h);
  return NULL;
}

double mconv_ir_scale(const mconv_ir_t *h) { return h->scale; }

void mconv_ir_free(mconv_ir_t *h) {
  if (!h) return;
  for (int s = 0; s < NSTAGE; s++) {
    mfft_free(&h->st[s].fft);
    for (int c = 0; c < 2; c++) { free(h->st[s].Hr[c]); free(h->st[s].Hi[c]); }
  }
  free(h);
}

mconv_t *mconv_new(const mconv_ir_t *h) {
  mconv_t *c = calloc(1, sizeof *c);
  if (!c) return NULL;
  c->h = h;
  size_t total = 0;
  for (int s = 0; s < h->nst; s++) {
    const stage_ir_t *S = &h->st[s];
    size_t B = (size_t)S->B, P = (size_t)S->P;
    total += 2 * (3 * B) + 4 * P * (B + 1) + 4 * (B + 1) + 2 * (2 * B) + 4 * B;
  }
  c->memsz = total * sizeof(float);
  float *m = calloc(total, sizeof(float));
  if (!m) { free(c); return NULL; }
  c->mem = m;
  for (int s = 0; s < h->nst; s++) {
    const stage_ir_t *S = &h->st[s];
    size_t B = (size_t)S->B, P = (size_t)S->P;
    stage_t *T = &c->st[s];
    for (int ch = 0; ch < 2; ch++) { T->in[ch] = m; m += 3 * B; }
    for (int ch = 0; ch < 2; ch++) { T->Xr[ch] = m; m += P * (B + 1); T->Xi[ch] = m; m += P * (B + 1); }
    for (int ch = 0; ch < 2; ch++) { T->Yr[ch] = m; m += B + 1; T->Yi[ch] = m; m += B + 1; }
    T->wr = m; m += 2 * B;
    T->wi = m; m += 2 * B;
    for (int b = 0; b < 2; b++) for (int ch = 0; ch < 2; ch++) { T->out[b][ch] = m; m += B; }
  }
  return c;
}

void mconv_free(mconv_t *c) { if (c) { free(c->mem); free(c); } }

void mconv_reset(mconv_t *c) { memset(c->mem, 0, c->memsz); c->q = 0; }

/* stage 0: the whole job in the quantum (window = previous + current quantum) */
static void stage0(mconv_t *c, const float *inL, const float *inR, float *outL, float *outR) {
  const stage_ir_t *S = &c->h->st[0];
  stage_t *T = &c->st[0];
  int B = S->B, N = 2 * B;
  int cur = (int)(c->q & 1), prev = cur ^ 1;   /* 2 of the 3 history slots suffice here */
  memcpy(T->in[0] + cur * B, inL, sizeof(float) * B);
  memcpy(T->in[1] + cur * B, inR, sizeof(float) * B);
  memcpy(T->wr, T->in[0] + prev * B, sizeof(float) * B);
  memcpy(T->wr + B, inL, sizeof(float) * B);
  memcpy(T->wi, T->in[1] + prev * B, sizeof(float) * B);
  memcpy(T->wi + B, inR, sizeof(float) * B);
  mfft_run(&S->fft, T->wr, T->wi, false);
  size_t B1 = (size_t)B + 1;
  int slot = (int)(c->q % S->P);
  split2(T->wr, T->wi, N, T->Xr[0] + slot * B1, T->Xi[0] + slot * B1, T->Xr[1] + slot * B1, T->Xi[1] + slot * B1);
  for (int ch = 0; ch < 2; ch++) { memset(T->Yr[ch], 0, sizeof(float) * B1); memset(T->Yi[ch], 0, sizeof(float) * B1); }
  for (int p = 0; p < S->P; p++) {
    int xs = (int)((c->q - p) % S->P);
    if (xs < 0) xs += S->P;
    for (int ch = 0; ch < 2; ch++)
      cmac(T->Yr[ch], T->Yi[ch], T->Xr[ch] + xs * B1, T->Xi[ch] + xs * B1, S->Hr[ch] + p * B1, S->Hi[ch] + p * B1, (int)B1);
  }
  combine2(T->Yr[0], T->Yi[0], T->Yr[1], T->Yi[1], N, T->wr, T->wi);
  mfft_run(&S->fft, T->wr, T->wi, true);
  for (int i = 0; i < B; i++) { outL[i] = T->wr[B + i]; outR[i] = T->wi[B + i]; }
}

/* stages >= 1: during block blk, process block b = blk - 1 one step at a time (output -> block blk + 1) */
static void stage_job_step(mconv_t *c, int s, int64_t blk, int step) {
  const stage_ir_t *S = &c->h->st[s];
  stage_t *T = &c->st[s];
  int B = S->B, N = 2 * B, L = S->fft.log2n;
  int64_t b = blk - 1;
  size_t B1 = (size_t)B + 1;
  if (step == 0) {
    int s0 = (int)((b - 1) % 3), s1 = (int)(b % 3);
    if (b - 1 < 0) memset(T->wr, 0, sizeof(float) * B), memset(T->wi, 0, sizeof(float) * B);
    else { memcpy(T->wr, T->in[0] + s0 * B, sizeof(float) * B); memcpy(T->wi, T->in[1] + s0 * B, sizeof(float) * B); }
    memcpy(T->wr + B, T->in[0] + s1 * B, sizeof(float) * B);
    memcpy(T->wi + B, T->in[1] + s1 * B, sizeof(float) * B);
    mfft_pass(&S->fft, T->wr, T->wi, 0, false);
  } else if (step <= L) {
    mfft_pass(&S->fft, T->wr, T->wi, step, false);
  } else if (step == L + 1) {
    int slot = (int)(b % S->P);
    split2(T->wr, T->wi, N, T->Xr[0] + slot * B1, T->Xi[0] + slot * B1, T->Xr[1] + slot * B1, T->Xi[1] + slot * B1);
    for (int ch = 0; ch < 2; ch++) { memset(T->Yr[ch], 0, sizeof(float) * B1); memset(T->Yi[ch], 0, sizeof(float) * B1); }
  } else if (step <= L + 1 + S->P) {
    int p = step - (L + 2);
    int64_t xb = b - p;
    if (xb >= 0) {
      int xs = (int)(xb % S->P);
      for (int ch = 0; ch < 2; ch++)
        cmac(T->Yr[ch], T->Yi[ch], T->Xr[ch] + xs * B1, T->Xi[ch] + xs * B1, S->Hr[ch] + p * B1, S->Hi[ch] + p * B1, (int)B1);
    }
  } else if (step == L + S->P + 2) {
    combine2(T->Yr[0], T->Yi[0], T->Yr[1], T->Yi[1], N, T->wr, T->wi);
    mfft_pass(&S->fft, T->wr, T->wi, 0, true);
  } else if (step <= 2 * L + S->P + 2) {
    mfft_pass(&S->fft, T->wr, T->wi, step - (L + S->P + 2), true);
  } else {
    int ob = (int)((blk + 1) & 1);
    memcpy(T->out[ob][0], T->wr + B, sizeof(float) * B);
    memcpy(T->out[ob][1], T->wi + B, sizeof(float) * B);
  }
}

void mconv_process(mconv_t *c, const float *inL, const float *inR, float *outL, float *outR) {
  stage0(c, inL, inR, outL, outR);
  for (int s = 1; s < c->h->nst; s++) {
    const stage_ir_t *S = &c->h->st[s];
    stage_t *T = &c->st[s];
    int64_t blk = c->q / S->M;
    int qi = (int)(c->q % S->M), B = S->B;
    float *dl = T->in[0] + (blk % 3) * B + qi * MQ, *dr = T->in[1] + (blk % 3) * B + qi * MQ;
    memcpy(dl, inL, sizeof(float) * MQ);
    memcpy(dr, inR, sizeof(float) * MQ);
    const float *ol = T->out[blk & 1][0] + qi * MQ, *orr = T->out[blk & 1][1] + qi * MQ;
    for (int i = 0; i < MQ; i++) { outL[i] += ol[i]; outR[i] += orr[i]; }
    if (blk >= 1) {
      int a = (int)((int64_t)qi * S->K / S->M), e = (int)((int64_t)(qi + 1) * S->K / S->M);
      for (int st = a; st < e; st++) stage_job_step(c, s, blk, st);
    }
  }
  c->q++;
}
