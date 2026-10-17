/* The music: the browser's generated music (notes audio-music.md, app.js:374-734) and the player's
 * own songs (songs.c), on one bus with the browser's level (0.6 * vol^2) and compressor.
 *
 * Generated music, as the browser does it, re-expressed on the audio sample clock:
 *  - the composer newSong() with every random draw in app.js's order (mulberry32, seeded), so a
 *    seed gives the browser's songs and the browser's event log (notes 3, 12);
 *  - the 72-bar arrangement (FORM) and genStep(), run by musicTick() with the same 0.2 s lookahead
 *    before each 128-frame quantum (now = frames / rate), so the swing, the future starts (risers)
 *    and "next" behave as in the browser;
 *  - genStart's mixer: duck (pumped by the kick), the dotted-eighth delay (feedback 0.35 through a
 *    2400 Hz low-pass, one quantum of extra lag per trip as Chromium), the 2.6 s convolution reverb
 *    (music_fx.c; why convolution: see there), the master low-pass with its section sweeps, the
 *    out gain (genStop: setTargetAtTime(0, now, 0.05), discarded 0.4 s later);
 *  - the bus gain 0.6 * vol^2 (setTargetAtTime tau 0.03 on changes) and the compressor
 *    (-16 dB, knee 10, ratio 3, 10 ms, 200 ms: dsp.h's port of Chromium's), shared with the songs.
 * Deviations, on purpose:
 *  - the master filter sweeps are the intended ones: the browser cancels a sweep that ends exactly
 *    at the next section's start in some tempos (notes 8.2, a 0.2 s jump of the filter);
 *    music_test_exact() restores the browser's behaviour for parity tests;
 *  - the reverb's IR is drawn once (music_new, from the seeded stream, exactly where the first
 *    genStart draws it); later starts reuse it and skip the same number of draws, so the random
 *    stream stays the browser's and the audio thread never rebuilds 3 MB of spectra.
 * Threads: music_set/next/songs_changed/now_playing from any thread (atomics); music_render in the
 * audio thread only: no allocation, no locks, no file I/O (songs come through songs.c's ring). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "audio_internal.h"
#include "songs.h"
#include "plat.h"

#define NVOICES 96
#define LOOKAHEAD 0.2
#define BARS 72
#define IDLE_FRAMES_S 2.0                      /* silent this long: stop running the bus */

/* ================================================================ tables (app.js:382-415) */

static const int AEOLIAN[7] = { 0, 2, 3, 5, 7, 8, 10 };
static const int PROGS[8][4] = { { 0, 5, 2, 6 }, { 0, 6, 5, 6 }, { 5, 6, 0, 0 }, { 0, 3, 5, 4 },
                                 { 0, 5, 3, 4 }, { 5, 3, 0, 6 }, { 3, 5, 0, 6 }, { 0, 2, 6, 5 } };
const int MUSIC_BASSES[3][16] = {
  { 1, 0, 1, 0, 1, 0, 2, 0, 1, 0, 1, 0, 1, 0, 2, 0 },   /* driving eighths, octave jumps */
  { 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1 },   /* offbeat */
  { 1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 2, 0 } }; /* rolling */
const int MUSIC_ARPS[3][16] = {
  { 0, 1, 2, 3, 1, 2, 3, 4, 2, 3, 4, 5, 3, 2, 1, 2 },
  { 0, 2, 1, 3, 2, 4, 3, 5, 4, 3, 2, 4, 1, 3, 0, 2 },
  { 0, 1, 2, 1, 3, 2, 4, 3, 5, 4, 3, 2, 4, 3, 2, 1 } };
typedef struct { int n; int r[10][2]; } rhy_t;
static const rhy_t RHY[5] = {
  { 10, { { 0, 3 }, { 3, 3 }, { 6, 2 }, { 8, 4 }, { 12, 2 }, { 14, 2 }, { 16, 3 }, { 19, 3 }, { 22, 2 }, { 24, 8 } } },
  { 9, { { 0, 2 }, { 2, 2 }, { 4, 4 }, { 10, 2 }, { 12, 4 }, { 16, 2 }, { 18, 2 }, { 20, 4 }, { 26, 6 } } },
  { 9, { { 2, 2 }, { 4, 2 }, { 6, 4 }, { 12, 2 }, { 14, 2 }, { 16, 6 }, { 24, 2 }, { 26, 2 }, { 28, 4 } } },
  { 7, { { 0, 6 }, { 6, 6 }, { 12, 4 }, { 16, 3 }, { 19, 3 }, { 22, 2 }, { 24, 8 } } },
  { 9, { { 0, 3 }, { 3, 3 }, { 6, 4 }, { 10, 2 }, { 12, 4 }, { 16, 3 }, { 19, 3 }, { 22, 4 }, { 28, 4 } } } };
/* the arrangement: bars, kick, clap, hats 1/2, bass, pad, arp, lead, riser, crash(+impact), the
 * snare roll, soft, hats from bar, filter ('i' in, 'o' out, 'b' build) */
typedef struct { int n, k, c, h, b, p, a, l, r, x, kick_stop, soft, h_from; char f; } sec_t;
static const sec_t FORM[7] = {
  { 8, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 4, 'i' },
  { 8, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0 },
  { 8, 1, 1, 2, 1, 1, 1, 0, 1, 0, 1, 0, 0, 0 },
  { 16, 1, 1, 2, 1, 1, 1, 1, 0, 2, 0, 0, 0, 0 },
  { 8, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 'b' },
  { 16, 1, 1, 2, 1, 1, 1, 1, 0, 2, 0, 0, 0, 0 },
  { 8, 1, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 'o' } };
static const char *const KEYS[12] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };

/* ================================================================ the composer (app.js:427-483) */

#define MULBERRY_INC 0x6D2B79F5u
static uint32_t inc_inverse(void) {           /* MULBERRY_INC^-1 mod 2^32 (Newton) */
  uint32_t a = MULBERRY_INC, x = a;
  for (int i = 0; i < 5; i++) x *= 2u - a * x;
  return x;
}
static uint32_t draws_between(uint32_t s0, uint32_t s1) { return (s1 - s0) * inc_inverse(); }

static int fdiv7(int k) { return k >= 0 ? k / 7 : -((-k + 6) / 7); }
static int deg(int d, int i) { int k = d + i; return AEOLIAN[((k % 7) + 7) % 7] + 12 * fdiv7(k); }
static int pick_i(dsp_rng_t *rs, int n) { return (int)floor(dsp_rng(rs) * n); }
static int mod12(int x) { return ((x % 12) + 12) % 12; }

/* the chord tone nearest to x (scale steps, kept in -3..11): set order, then octaves ascending */
static int nearest(int x, const mchord_t *c, const int *set, int nset) {
  int best = x, bd = 99;
  for (int k = 0; k < nset; k++) {
    int sv = c->d + 2 * set[k];
    for (int o = -14; o <= 14; o += 7) {
      int v = sv + o;
      if (v >= -3 && v <= 11 && abs(v - x) < bd) { bd = abs(v - x); best = v; }
    }
  }
  return best;
}

static const int SET012[3] = { 0, 1, 2 }, SET0[1] = { 0 };
static const int STEPS6[6] = { -1, 1, 1, 2, -2, -1 };

static int phrase(const msong_t *s, const rhy_t *rhy, int b0, int start, mnote_t *out, dsp_rng_t *rs) {
  int x = start;
  for (int k = 0; k < rhy->n; k++) {
    int st = rhy->r[k][0], len = rhy->r[k][1];
    const mchord_t *c = &s->ch[(b0 + (st >> 4)) % 4];
    if (st % 8 == 0 || len >= 4) x = nearest(x + (dsp_rng(rs) < 0.5 ? 1 : -1), c, SET012, 3);
    else {
      x += STEPS6[pick_i(rs, 6)];
      x = x < -3 ? -3 : x > 11 ? 11 : x;
    }
    out[k].st = st + 16 * b0; out[k].len = len; out[k].x = x;
  }
  return rhy->n;
}

static int fit(const msong_t *s, const mnote_t *ph, int n, int b0, bool end_root, mnote_t *out) {
  for (int k = 0; k < n; k++) {
    int st = ph[k].st % 32;
    const mchord_t *c = &s->ch[(b0 + (st >> 4)) % 4];
    int x;
    if (k == n - 1 && end_root) x = nearest(ph[k].x, c, SET0, 1);
    else if (st % 8 == 0 || ph[k].len >= 4) x = nearest(ph[k].x, c, SET012, 3);
    else x = ph[k].x;
    out[k].st = st + 16 * b0; out[k].len = ph[k].len; out[k].x = x;
  }
  return n;
}

void music_compose(msong_t *s, const msong_t *prev, dsp_rng_t *rs) {
  static const int UP[4] = { 5, 7, 3, 9 };
  uint32_t s0 = rs->s;
  int tonic = prev ? (prev->tonic + UP[pick_i(rs, 4)]) % 12 : (int)floor(dsp_rng(rs) * 12);
  memset(s, 0, sizeof *s);
  s->tonic = tonic;
  const int *prog = PROGS[pick_i(rs, 8)];
  for (int c = 0; c < 4; c++) {
    s->ch[c].d = prog[c];
    for (int j = 0; j < 5; j++) s->ch[c].t[j] = deg(prog[c], 2 * j);
  }
  int prevV[4] = { 57, 60, 64, 67 };
  for (int c = 0; c < 4; c++) {
    mchord_t *ch = &s->ch[c];
    int pcs[4] = { ch->t[1], ch->t[2], ch->t[3], dsp_rng(rs) < 0.5 ? ch->t[4] : ch->t[0] };
    int v[4];
    for (int i = 0; i < 4; i++) {
      int best = 0, bd = 99;
      for (int o = 3; o <= 6; o++) {
        int m = 12 * o + mod12(tonic + pcs[i]);
        if (m >= 52 && m <= 76 && abs(m - prevV[i]) < bd) { bd = abs(m - prevV[i]); best = m; }
      }
      v[i] = best;
    }
    for (int i = 1; i < 4; i++)                /* sort ascending */
      for (int j = i; j > 0 && v[j - 1] > v[j]; j--) { int t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    memcpy(ch->v, v, sizeof v);
    memcpy(prevV, v, sizeof v);
    ch->bass = 36 + mod12(tonic + ch->t[0]);
  }
  int lead_base = 62 + mod12(tonic - 62);
  const rhy_t *rA = &RHY[pick_i(rs, 5)], *rB = &RHY[pick_i(rs, 5)];
  mnote_t A[10], B[10];
  int nA = phrase(s, rA, 0, 4, A, rs);
  int nB = phrase(s, rB, 4, A[nA - 1].x + 2, B, rs);
  int n = 0;
  memcpy(s->hook, A, sizeof(mnote_t) * (size_t)nA); n += nA;
  n += fit(s, A, nA, 2, false, s->hook + n);
  memcpy(s->hook + n, B, sizeof(mnote_t) * (size_t)nB); n += nB;
  n += fit(s, A, nA, 6, true, s->hook + n);
  s->nhook = n;
  for (int k = 0; k < n; k++) {
    int x = s->hook[k].x;
    s->hook[k].m = lead_base + AEOLIAN[((x % 7) + 7) % 7] + 12 * fdiv7(x);
  }
  s->bpm = 120 + (int)floor(dsp_rng(rs) * 7);
  s->bass_i = pick_i(rs, 3);
  s->arp_i = pick_i(rs, 3);
  s->swing = 0.02 + dsp_rng(rs) * 0.03;
  s->pad_cut = 1300 + dsp_rng(rs) * 900;
  s->calls = (int)draws_between(s0, rs->s);
}

/* ================================================================ one generator (genStart) */

enum { G_OFF = 0, G_RUN, G_STOP };

typedef struct {
  int state;
  double t;                                    /* the next sixteenth (s, music clock) */
  int bar, step;
  msong_t song;
  mparam_t lpf, duck, dtime;                   /* master LP frequency, duck gain, delay time */
  float out_v, out_c;                          /* out gain; genStop's one-pole (tau 0.05) */
  int64_t stop_k;                              /* discarded at this frame */
  float *noise;                                /* 1 s */
  mvoices_t *voices;
  mconv_t *conv;
  dsp_delay_t delay;
  dsp_biquad_t dlp, lp[2];
  float fb[MQ];                                /* last quantum's feedback (Chromium's cycle lag) */
  minst_t I;
  mbus_t bus;
} mgen_t;

struct music {
  int rate;
  double sr;
  dsp_rng_t rs;
  uint32_t seed;
  mwaves_t waves;
  mconv_ir_t *ir;
  int ir_n;
  mgen_t gen[2];
  songs_t *songs;
  /* control (any thread, atomics) */
  int c_on, c_source, c_next;
  uint32_t c_vol;                              /* float bits */
  /* published by the audio thread */
  int p_kind, p_tonic, p_bpm;
  int64_t p_frames;
  /* audio thread */
  int next_seen;
  bool exact;                                  /* tests: the browser's filter-cancel quirk */
  float bus_v, bus_target, bus_c;              /* bus gain (setTargetAtTime tau 0.03) */
  bool bus_live;                               /* the bus runs (else idle: nothing to output) */
  dsp_comp_t comp;
  float sg_gain, sg_step;                      /* songs on the bus: 1, or a 50 ms fade-out */
  bool songs_on;
  int64_t k;                                   /* music clock: frames rendered */
  int64_t quiet;                               /* frames since anything played */
  float qL[MQ], qR[MQ];
  int qpos;
  float sL[MQ], sR[MQ];
  void (*log)(void *, const char *, const double *, int);
  void *log_ud;
};

static float vol_get(music_t *m) {
  uint32_t b = __atomic_load_n(&m->c_vol, __ATOMIC_ACQUIRE);
  float v;
  memcpy(&v, &b, sizeof v);
  return v;
}

static int gen_alloc(music_t *m, mgen_t *g) {
  g->noise = calloc((size_t)m->rate, sizeof(float));
  g->voices = mvoices_new(NVOICES);
  g->conv = mconv_new(m->ir);
  if (!g->noise || !g->voices || !g->conv || dsp_delay_init(&g->delay, 2 * m->rate + 2 * MQ) != 0) return -1;
  return 0;
}

static void gen_free(mgen_t *g) {
  free(g->noise);
  mvoices_free(g->voices);
  mconv_free(g->conv);
  dsp_delay_free(&g->delay);
}

static void gen_set_song_delay(mgen_t *g, double t) { mparam_set(&g->dtime, 0.75 * 60.0 / g->song.bpm, t); }

/* genStart (app.js:485-510): noise, IR (drawn once, see the top), a new song from a random key */
static void gen_start(music_t *m, mgen_t *g, double now) {
  dsp_noise_fill(g->noise, (size_t)m->rate, &m->rs);
  m->rs.s += (uint32_t)(2u * (uint32_t)m->ir_n) * MULBERRY_INC;   /* the IR's draws */
  music_compose(&g->song, NULL, &m->rs);
  g->state = G_RUN;
  g->t = now + 0.1;
  g->bar = g->step = 0;
  mparam_init(&g->lpf, 18000);
  mparam_init(&g->duck, 1);
  mparam_init(&g->dtime, 0.75 * 60.0 / g->song.bpm);
  g->out_v = 1;
  g->out_c = 0;
  mvoices_clear(g->voices);
  mconv_reset(g->conv);
  dsp_delay_clear(&g->delay);
  dsp_biquad_reset(&g->dlp);
  dsp_biquad_set(&g->dlp, DSP_LOWPASS, 2400, 1, 0, m->sr);
  dsp_biquad_reset(&g->lp[0]);
  dsp_biquad_reset(&g->lp[1]);
  memset(g->fb, 0, sizeof g->fb);
  g->I.v = g->voices;
  g->I.waves = &m->waves;
  g->I.noise = g->noise;
  g->I.noise_len = m->rate;
  g->I.rs = &m->rs;
  g->I.duck = &g->duck;
  g->I.sr = m->sr;
  g->I.log = m->log;
  g->I.log_ud = m->log_ud;
}

/* genStop: setTargetAtTime(0, now, 0.05), disconnected 0.4 s later */
static void gen_stop(music_t *m, mgen_t *g) {
  if (g->state != G_RUN) return;
  g->state = G_STOP;
  g->out_c = (float)dsp_tau_coef(0.05, m->sr);
  g->stop_k = m->k + (int64_t)(0.4 * m->sr);
}

/* genStep (app.js:626-662): one sixteenth at time t */
static void gen_step(music_t *m, mgen_t *g, double t) {
  const msong_t *sg = &g->song;
  minst_t *I = &g->I;
  int s = g->step, b = g->bar;
  const sec_t *sec = &FORM[0];
  for (int i = 0; i < 7; i++) {
    if (b < FORM[i].n) { sec = &FORM[i]; break; }
    b -= FORM[i].n;
  }
  const mchord_t *ch = &sg->ch[g->bar % 4];
  double bar = 16.0 * 60.0 / sg->bpm / 4.0, sec_dur = sec->n * bar;
  if (s == 0) {
    if (b == 0) {                              /* a section starts: its filter move, crash */
      mparam_t *lf = &g->lpf;
      if (m->exact) mparam_cancel(lf, t); else mparam_cancel_keep_ending(lf, t);
      if (sec->f == 'i') { mparam_set(lf, 320, t); mparam_exp(lf, 18000, t + sec_dur); }
      else if (sec->f == 'o') { mparam_set(lf, 18000, t); mparam_exp(lf, 380, t + sec_dur); }
      else if (sec->f == 'b') { mparam_set(lf, 1500, t); mparam_exp(lf, 18000, t + sec_dur); }
      else mparam_set(lf, 18000, t);
      if (sec->x) { mi_crash(I, t); if (sec->x == 2) mi_impact(I, t); }
      if (sec->r) mi_riser(I, t + (sec->n > 4 ? (sec->n - 4) * bar : 0), (sec->n < 4 ? sec->n : 4) * bar);
    }
    if (sec->p) mi_pad(I, t, ch->v, bar, sg->pad_cut);
  }
  bool last_bar = b == sec->n - 1;
  if (sec->k && s % 4 == 0 && !(sec->kick_stop && last_bar)) mi_kick(I, t);
  if (sec->c && (s == 4 || s == 12) && !(sec->kick_stop && last_bar)) mi_clap(I, t, 1);
  if (sec->kick_stop && b >= sec->n - 2 && (s % 2 == 0 || last_bar))   /* the roll */
    mi_clap(I, t, 0.25 + 0.6 * (double)((b - sec->n + 2) * 16 + s) / 32.0);
  if (sec->h && b >= sec->h_from) {
    if (sec->h == 1 && s % 2 == 0) mi_hat(I, t, s % 4 == 2 ? 1 : 0.55, false);
    if (sec->h == 2) mi_hat(I, t, s % 4 == 2 ? 1 : (s % 2 ? 0.45 : 0.65), s % 8 == 6);
  }
  const int *bass = MUSIC_BASSES[sg->bass_i], *arp = MUSIC_ARPS[sg->arp_i];
  if (sec->b && bass[s]) mi_bass(I, t, ch->bass + (bass[s] == 2 ? 12 : 0), 60.0 / sg->bpm / 4 * 1.6);
  if (sec->a && (sec->soft ? s % 2 == 0 : true)) {
    int i = arp[s] % 8, note = ch->v[i % 4] + (i >= 4 ? 12 : 0);
    double bright = sec->soft ? 0.2 : fmin(1, b / 8.0 + 0.3);
    mi_pluck(I, t, note, s % 4 == 0 ? 1 : 0.7, s % 2 ? -0.35 : 0.35, bright);
  }
  if (sec->l) {
    int pos = (g->bar % 8) * 16 + s;
    for (int k = 0; k < sg->nhook; k++)
      if (sg->hook[k].st == pos) mi_lead(I, t, sg->hook[k].m, sg->hook[k].len * 60.0 / sg->bpm / 4 * 0.92, sec->soft != 0);
  }
}

static void publish_song(music_t *m, const mgen_t *g) {
  __atomic_store_n(&m->p_tonic, g->song.tonic, __ATOMIC_RELEASE);
  __atomic_store_n(&m->p_bpm, g->song.bpm, __ATOMIC_RELEASE);
}

/* musicTick (app.js:663-679) at the music clock's now */
static void gen_tick(music_t *m, mgen_t *g, double now) {
  if (g->t < now) g->t = now + 0.05;
  while (g->t < now + LOOKAHEAD) {
    double sd = 60.0 / g->song.bpm / 4.0;
    gen_step(m, g, g->t + (g->step % 2 ? g->song.swing * sd : 0));
    g->t += sd;
    g->step = (g->step + 1) % 16;
    if (g->step == 0 && ++g->bar >= BARS) {
      msong_t prev = g->song;
      music_compose(&g->song, &prev, &m->rs);
      g->bar = 0;
      gen_set_song_delay(g, g->t);
      publish_song(m, g);
    }
  }
}

/* one quantum of a generator's mixer, added into L/R (before the bus) */
static void gen_render(music_t *m, mgen_t *g, int64_t k0, float *L, float *R) {
  const double sr = m->sr;
  mbus_t *bus = &g->bus;
  memset(bus, 0, sizeof *bus);
  mvoices_render(g->voices, &m->waves, g->noise, m->rate, k0, sr, bus);
  double dk[MQ], dt[MQ], lf[MQ];
  mparam_fill(&g->duck, k0, MQ, sr, dk);
  mparam_fill(&g->dtime, k0, MQ, sr, dt);
  float dl[MQ], vin[2][MQ], wet[2][MQ];
  for (int i = 0; i < MQ; i++) {               /* the delay loop: fb returns one quantum late */
    dsp_delay_write(&g->delay, bus->ds[i] + g->fb[i]);
    float y = dsp_biquad_run(&g->dlp, dsp_delay_read(&g->delay, dt[i] * sr));
    dl[i] = y;
    g->fb[i] = 0.35f * y;
  }
  dsp_biquad_flush(&g->dlp);
  for (int i = 0; i < MQ; i++) { vin[0][i] = bus->vsL[i] + dl[i]; vin[1][i] = bus->vsR[i] + dl[i]; }
  mconv_process(g->conv, vin[0], vin[1], wet[0], wet[1]);
  float xl[MQ], xr[MQ];
  for (int i = 0; i < MQ; i++) {
    float d = (float)dk[i];
    xl[i] = bus->lpL[i] + d * bus->duckL[i] + dl[i] + 0.3f * wet[0][i];
    xr[i] = bus->lpR[i] + d * bus->duckR[i] + dl[i] + 0.3f * wet[1][i];
  }
  /* the master low-pass, Q 0.5 dB; coefficients every 4 frames while it sweeps */
  static const double QLIN = 1.0592537251772889;   /* 10^(0.5/20) */
  double fc;
  bool fconst = mparam_constant(&g->lpf, k0, MQ, sr, &fc);
  if (!fconst) mparam_fill(&g->lpf, k0, MQ, sr, lf);
  for (int i = 0; i < MQ; i++) {
    if (fconst ? i == 0 : i % 4 == 0) {
      dsp_biquad_set_lphp(&g->lp[0], 0, fconst ? fc : lf[i], QLIN, sr);
      dsp_biquad_t *a = &g->lp[0], *b = &g->lp[1];
      b->b0 = a->b0; b->b1 = a->b1; b->b2 = a->b2; b->a1 = a->a1; b->a2 = a->a2;
    }
    xl[i] = dsp_biquad_run(&g->lp[0], xl[i]);
    xr[i] = dsp_biquad_run(&g->lp[1], xr[i]);
  }
  dsp_biquad_flush(&g->lp[0]);
  dsp_biquad_flush(&g->lp[1]);
  if (g->state == G_STOP) {
    for (int i = 0; i < MQ; i++) {
      L[i] += g->out_v * xl[i]; R[i] += g->out_v * xr[i];
      g->out_v += (0.0f - g->out_v) * g->out_c;
    }
  } else {
    for (int i = 0; i < MQ; i++) { L[i] += xl[i]; R[i] += xr[i]; }
  }
}

/* ================================================================ the music object */

static void render_quantum(music_t *m) {
  int64_t k0 = m->k;
  double now = (double)k0 / m->sr;
  memset(m->qL, 0, sizeof m->qL);
  memset(m->qR, 0, sizeof m->qR);

  bool on = __atomic_load_n(&m->c_on, __ATOMIC_ACQUIRE) != 0;
  int src = __atomic_load_n(&m->c_source, __ATOMIC_ACQUIRE);
  bool want_songs = on && src == AUDIO_MUSIC_SONGS && m->songs && songs_playable(m->songs);
  bool want_gen = on && !want_songs;
  float target = 0.6f * vol_get(m) * vol_get(m);

  /* the generator: start, stop, next */
  mgen_t *run = NULL;
  for (int i = 0; i < 2; i++) if (m->gen[i].state == G_RUN) run = &m->gen[i];
  if (run && !want_gen) { gen_stop(m, run); run = NULL; }
  if (!run && want_gen) {
    mgen_t *g = m->gen[0].state == G_OFF ? &m->gen[0] : m->gen[1].state == G_OFF ? &m->gen[1] : NULL;
    if (!g) g = m->gen[0].stop_k < m->gen[1].stop_k ? &m->gen[0] : &m->gen[1];   /* both fading */
    if (!m->bus_live) {                        /* the bus comes up at its level, not ramping */
      m->bus_v = target;
    }
    gen_start(m, g, now);
    publish_song(m, g);
    run = g;
  }
  int nx = __atomic_load_n(&m->c_next, __ATOMIC_ACQUIRE);
  if (nx != m->next_seen) {
    m->next_seen = nx;
    if (run) {                                 /* musicNext (app.js:718-727) */
      msong_t prev = run->song;
      music_compose(&run->song, &prev, &m->rs);
      run->bar = run->step = 0;
      gen_set_song_delay(run, now);
      publish_song(m, run);
    }
  }
  if (run) gen_tick(m, run, now);

  /* songs: on at once (songs.c fades in after gaps), off with a 50 ms fade-out */
  if (want_songs) {
    if (!m->songs_on && !m->bus_live) m->bus_v = target;
    m->songs_on = true;
    m->sg_gain = 1;
    m->sg_step = 0;
  } else if (m->songs_on && m->sg_step == 0) {
    m->sg_step = (float)(1.0 / (0.05 * m->sr));
  }
  __atomic_store_n(&m->p_kind, run ? 1 : want_songs ? 2 : 0, __ATOMIC_RELEASE);

  bool any = run || m->gen[0].state != G_OFF || m->gen[1].state != G_OFF || m->songs_on;
  if (any) { m->quiet = 0; m->bus_live = true; }
  else if (m->bus_live) {
    m->quiet += MQ;
    if (m->quiet > (int64_t)(IDLE_FRAMES_S * m->sr)) m->bus_live = false;
  }
  m->bus_target = target;
  if (!m->bus_live) {                          /* idle: nothing plays, the compressor sleeps */
    m->bus_v = target;
    m->k += MQ;
    return;
  }

  for (int i = 0; i < 2; i++) {
    mgen_t *g = &m->gen[i];
    if (g->state == G_OFF) continue;
    gen_render(m, g, k0, m->qL, m->qR);
    if (g->state == G_STOP && m->k + MQ >= g->stop_k) g->state = G_OFF;
  }
  if (m->songs_on) {
    songs_read(m->songs, m->sL, m->sR, MQ);
    for (int i = 0; i < MQ; i++) {
      m->qL[i] += m->sg_gain * m->sL[i];
      m->qR[i] += m->sg_gain * m->sR[i];
      if (m->sg_step > 0) {
        m->sg_gain -= m->sg_step;
        if (m->sg_gain < 0) m->sg_gain = 0;
      }
    }
    if (m->sg_step > 0 && m->sg_gain <= 0) { m->songs_on = false; m->sg_step = 0; }
  }
  /* the bus: 0.6 vol^2 (setTargetAtTime 0.03), then the compressor */
  for (int i = 0; i < MQ; i++) {
    m->bus_v += (m->bus_target - m->bus_v) * m->bus_c;
    m->qL[i] *= m->bus_v;
    m->qR[i] *= m->bus_v;
  }
  dsp_comp_process(&m->comp, m->qL, m->qR, MQ);
  m->k += MQ;
}

music_t *music_new(int rate, uint32_t seed, const char *songs_dir) {
  music_t *m = calloc(1, sizeof *m);
  if (!m) return NULL;
  if (rate < 8000) rate = 48000;
  m->rate = rate;
  m->sr = rate;
  if (!seed) seed = (uint32_t)plat_now_us() ^ 0x9E3779B9u;
  m->seed = seed;
  dsp_rng_seed(&m->rs, seed);
  if (mwaves_build(&m->waves, m->sr) != 0) goto fail;
  /* the reverb IR as the first genStart draws it: after the 1 s noise, channel 0 then 1 */
  m->ir_n = (int)floor(m->sr * 2.6);
  {
    float *ir = malloc(sizeof(float) * 2 * (size_t)m->ir_n);
    if (!ir) goto fail;
    dsp_rng_t r = m->rs;
    r.s += (uint32_t)rate * MULBERRY_INC;
    for (int c = 0; c < 2; c++)
      for (int i = 0; i < m->ir_n; i++)
        ir[(size_t)c * m->ir_n + i] = (float)((dsp_rng(&r) * 2 - 1) * pow(1 - (double)i / m->ir_n, 3.5));
    m->ir = mconv_ir_new(ir, ir + m->ir_n, m->ir_n, m->sr);
    free(ir);
    if (!m->ir) goto fail;
  }
  for (int i = 0; i < 2; i++) if (gen_alloc(m, &m->gen[i]) != 0) goto fail;
  m->bus_c = (float)dsp_tau_coef(0.03, m->sr);
  float v0 = 0.6f;
  memcpy(&m->c_vol, &v0, sizeof v0);
  m->bus_v = m->bus_target = 0.6f * v0 * v0;
  dsp_comp_init(&m->comp, m->sr, -16, 10, 3, 0.01f, 0.2f);
  m->qpos = MQ;
  m->c_source = AUDIO_MUSIC_GENERATED;
  if (songs_dir && *songs_dir) m->songs = songs_new(rate, songs_dir);
  return m;
fail:
  music_free(m);
  return NULL;
}

void music_free(music_t *m) {
  if (!m) return;
  if (m->songs) songs_free(m->songs);
  for (int i = 0; i < 2; i++) gen_free(&m->gen[i]);
  mconv_ir_free(m->ir);
  mwaves_free(&m->waves);
  free(m);
}

void music_set(music_t *m, bool on, float vol, int source) {
  if (!m) return;
  if (!(vol >= 0)) vol = 0;                    /* NaN too */
  if (vol > 1) vol = 1;
  uint32_t b;
  memcpy(&b, &vol, sizeof b);
  __atomic_store_n(&m->c_vol, b, __ATOMIC_RELEASE);
  int src = source == AUDIO_MUSIC_SONGS ? AUDIO_MUSIC_SONGS : AUDIO_MUSIC_GENERATED;
  __atomic_store_n(&m->c_source, src, __ATOMIC_RELEASE);
  __atomic_store_n(&m->c_on, on ? 1 : 0, __ATOMIC_RELEASE);
  if (m->songs) songs_set_active(m->songs, on && src == AUDIO_MUSIC_SONGS);
}

void music_next(music_t *m) {
  if (!m) return;
  int kind = __atomic_load_n(&m->p_kind, __ATOMIC_ACQUIRE);
  if (kind == 2 && m->songs) songs_next(m->songs);
  else if (__atomic_load_n(&m->c_on, __ATOMIC_ACQUIRE)) __atomic_add_fetch(&m->c_next, 1, __ATOMIC_ACQ_REL);
}

void music_songs_changed(music_t *m) {
  if (m && m->songs) songs_rescan(m->songs);
}

void music_now_playing(music_t *m, char *out, size_t n) {
  if (!out || !n) return;
  out[0] = 0;
  if (!m) return;
  int kind = __atomic_load_n(&m->p_kind, __ATOMIC_ACQUIRE);
  if (kind == 1) {
    int tonic = __atomic_load_n(&m->p_tonic, __ATOMIC_ACQUIRE), bpm = __atomic_load_n(&m->p_bpm, __ATOMIC_ACQUIRE);
    snprintf(out, n, "Generated: %s minor, %d BPM", KEYS[mod12(tonic)], bpm);
  } else if (kind == 2 && m->songs) {
    char t[200];
    songs_title(m->songs, t, sizeof t);
    if (t[0]) snprintf(out, n, "Playing %s", t);
  }
}

void music_render(music_t *m, float *L, float *R, int n) {
  if (!m) return;
  while (n > 0) {
    if (m->qpos == MQ) { render_quantum(m); m->qpos = 0; }
    int c = MQ - m->qpos < n ? MQ - m->qpos : n;
    if (m->bus_live || m->songs_on) {
      for (int i = 0; i < c; i++) { L[i] += m->qL[m->qpos + i]; R[i] += m->qR[m->qpos + i]; }
    }
    m->qpos += c;
    L += c; R += c; n -= c;
  }
  __atomic_store_n(&m->p_frames, m->k, __ATOMIC_RELEASE);
}

/* ================================================================ tests */

void music_test_log(music_t *m, void (*fn)(void *, const char *, const double *, int), void *ud) {
  m->log = fn;
  m->log_ud = ud;
  for (int i = 0; i < 2; i++) { m->gen[i].I.log = fn; m->gen[i].I.log_ud = ud; }
}
void music_test_exact(music_t *m, bool exact) { m->exact = exact; }
int64_t music_test_frames(music_t *m) { return __atomic_load_n(&m->p_frames, __ATOMIC_ACQUIRE); }
bool music_test_song(music_t *m, msong_t *out, int *bar, int *step) {
  for (int i = 0; i < 2; i++)
    if (m->gen[i].state == G_RUN) {
      if (out) *out = m->gen[i].song;
      if (bar) *bar = m->gen[i].bar;
      if (step) *step = m->gen[i].step;
      return true;
    }
  return false;
}
uint64_t music_test_draws(music_t *m) { return draws_between(m->seed, m->rs.s); }
double music_test_lp_freq(music_t *m, double t) {
  for (int i = 0; i < 2; i++) if (m->gen[i].state == G_RUN) return mparam_value(&m->gen[i].lpf, t);
  return 0;
}
int music_test_voices(music_t *m) {
  int n = 0;
  for (int i = 0; i < 2; i++) if (m->gen[i].state != G_OFF) n += mvoices_active(m->gen[i].voices);
  return n;
}
