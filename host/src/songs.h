/* Internals of the music module (audio_internal.h has its public face, music_*):
 *   songs.c         the player's own songs: folder scan, a decoder thread, a ~4 s float ring
 *   music.c         the generated music: composer, arrangement, sequencer, mixer, bus, compressor
 *   music_voices.c  AudioParam timelines and the instruments (voices) on dsp.h's oscillators/biquads
 *   music_fx.c      FFT and the partitioned convolution reverb
 * (The declarations shared by the music_*.c files live here because this is the module's one header.)
 * Spec: notes audio-music.md (the browser's app.js music, re-expressed per sample). Shared building
 * blocks (PRNG, wave tables, biquads, panner, compressor, delay line, one-pole) come from dsp.h. */
#ifndef SONGS_H
#define SONGS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "dsp.h"

/* ================================================================ the player's songs (songs.c) */
typedef struct songs songs_t;

/* Scans dir once (synchronously) and starts the decoder thread (normal priority, idle until active).
 * rate: the output rate; files at other rates are resampled (Kaiser-windowed sinc, 48 taps).
 * NULL only without memory. */
songs_t *songs_new(int rate, const char *dir);
void songs_free(songs_t *s);
/* control threads (never the audio thread): */
void songs_set_active(songs_t *s, bool on);   /* off -> on restarts the current song from its start */
void songs_next(songs_t *s);                  /* the next file, now (wraps); 20 ms fade-out of the old one */
void songs_rescan(songs_t *s);                /* the folder changed: rescan in the decoder thread */
/* any thread: */
int  songs_count(songs_t *s);                 /* playable files found by the last scan */
bool songs_playable(songs_t *s);              /* count > 0 and not every file failed to decode */
void songs_title(songs_t *s, char *out, size_t n);  /* the file being heard, without extension ("" none) */
/* audio thread only: overwrite L/R with n frames (silence where none are decoded yet). Handles the
 * 20 ms fade-out on next and a 5 ms fade-in after any gap. Never blocks or allocates. */
void songs_read(songs_t *s, float *L, float *R, int n);
/* tests: frames waiting in the ring, the file index heard (-1 none), files that failed so far */
int  songs_buffered(songs_t *s);
int  songs_index(songs_t *s);
int  songs_failures(songs_t *s);
/* natural, case-insensitive name order ("a2" < "A10"); exposed for tests */
int  songs_name_cmp(const char *a, const char *b);

/* ================================================================ shared DSP pieces (music_*.c) */
#define MQ 128                                 /* render quantum, frames (as WebAudio) */

/* ---- AudioParam timeline (Web Audio semantics: SET, LIN and EXP ramps ending at t, cancel(t)).
 * dsp_param_t has the same semantics; this one keeps double values and adds the "intended" cancel
 * that the master filter sweeps need (notes 8.2). */
enum { MP_SET = 0, MP_LIN = 1, MP_EXP = 2 };
#define MP_MAXEV 8
typedef struct { double t, v; int type; } mpev_t;
typedef struct { double def; int n; mpev_t ev[MP_MAXEV]; } mparam_t;
void   mparam_init(mparam_t *p, double def);
void   mparam_set(mparam_t *p, double v, double t);
void   mparam_lin(mparam_t *p, double v, double t);
void   mparam_exp(mparam_t *p, double v, double t);
void   mparam_cancel(mparam_t *p, double t);  /* drop every event with time >= t (a running ramp too) */
/* the "intended" cancel: like cancel(t), but a ramp ending within 1 us of t is kept and ends at t */
void   mparam_cancel_keep_ending(mparam_t *p, double t);
double mparam_value(const mparam_t *p, double tau);
/* out[i] = value at (k0 + i) / sr, i < n; also drops events that ended before k0 (keeps the list short) */
void   mparam_fill(mparam_t *p, int64_t k0, int n, double sr, double *out);
bool   mparam_constant(const mparam_t *p, int64_t k0, int n, double sr, double *v); /* no change in the block */
int64_t mframe_at(double t, double sr);        /* the first frame whose time k / sr is >= t */

/* ---- the three oscillator waves (dsp.h tables: Chromium's PeriodicWave, 36 band-limited tables) */
enum { MW_SINE = 0, MW_SAW = 1, MW_SQUARE = 2 };
typedef struct { dsp_wave_t *w[3]; } mwaves_t;
int  mwaves_build(mwaves_t *w, double sr);     /* 0 ok (allocates) */
void mwaves_free(mwaves_t *w);

/* ---- FFT (music_fx.c): complex radix-2, split re/im, run pass by pass so work can be spread */
typedef struct { int n, log2n; float *cs, *sn; int *rev; } mfft_t;
int  mfft_init(mfft_t *f, int n);              /* n a power of two */
void mfft_free(mfft_t *f);
/* pass 0 = bit-reverse permutation, passes 1..log2n = butterflies; inverse: conjugate twiddles, unscaled */
void mfft_pass(const mfft_t *f, float *re, float *im, int pass, bool inverse);
void mfft_run(const mfft_t *f, float *re, float *im, bool inverse);

/* ---- the reverb: zero-latency partitioned convolution with a 2-channel IR (music_fx.c)
 * Stage 0: 128-frame partitions computed in the quantum itself (taps 0..2047). Stage 1: 1024-frame
 * partitions from tap 2048, stage 2: 8192-frame partitions from tap 16384; their work for a block is
 * spread evenly over the next block's quanta, so the CPU load per quantum is flat. */
typedef struct mconv_ir mconv_ir_t;           /* the IR's partition spectra (shared, read-only) */
typedef struct mconv mconv_t;                 /* one convolver's state (input history, accumulators) */
/* ir[c][i], i < n; normalize like ConvolverNode (normalize = true) at sr */
mconv_ir_t *mconv_ir_new(const float *irL, const float *irR, int n, double sr);
void        mconv_ir_free(mconv_ir_t *h);
double      mconv_ir_scale(const mconv_ir_t *h);   /* the normalization applied */
mconv_t    *mconv_new(const mconv_ir_t *h);
void        mconv_free(mconv_t *c);
void        mconv_reset(mconv_t *c);           /* silence (zero history) */
/* one quantum: inL/inR -> outL/outR (overwritten), MQ frames each */
void        mconv_process(mconv_t *c, const float *inL, const float *inR, float *outL, float *outR);

/* ---- the instruments (music_voices.c) */
typedef struct {
  float lpL[MQ], lpR[MQ];                      /* straight into the master low-pass */
  float duckL[MQ], duckR[MQ];                  /* the pumped bus (bass, pad, pluck) */
  float vsL[MQ], vsR[MQ];                      /* reverb send */
  float ds[MQ];                                /* delay send (mono) */
} mbus_t;
typedef struct mvoices mvoices_t;
typedef struct {
  mvoices_t *v;
  const mwaves_t *waves;
  const float *noise; int noise_len;          /* 1 s of white noise (genStart) */
  dsp_rng_t *rs;                               /* the random stream (jitter, offsets) */
  mparam_t *duck;                              /* the kick pumps it */
  double sr;
  /* tests: every instrument call, as the browser harness logs it (NULL = off) */
  void (*log)(void *ud, const char *name, const double *a, int na);
  void *log_ud;
} minst_t;
mvoices_t *mvoices_new(int max_voices);
void       mvoices_free(mvoices_t *v);
void       mvoices_clear(mvoices_t *v);
int        mvoices_active(const mvoices_t *v);       /* voices allocated (incl. future starts) */
uint32_t   mvoices_stolen(const mvoices_t *v);       /* pool overflows so far */
/* render every voice overlapping frames [k0, k0 + MQ) into the bus (added) */
void       mvoices_render(mvoices_t *v, const mwaves_t *w, const float *noise, int noise_len,
                          int64_t k0, double sr, mbus_t *bus);
void mi_kick(minst_t *I, double t);
void mi_clap(minst_t *I, double t, double v);
void mi_hat(minst_t *I, double t, double v, bool open);
void mi_crash(minst_t *I, double t);
void mi_impact(minst_t *I, double t);
void mi_bass(minst_t *I, double t, int m, double dur);
void mi_pad(minst_t *I, double t, const int *notes, double dur, double cut);
void mi_pluck(minst_t *I, double t, int m, double v, double pan, double bright);
void mi_lead(minst_t *I, double t, int m, double dur, bool soft);
void mi_riser(minst_t *I, double t, double dur);

/* ---- tests (music.c): the composer and an offline generator without threads or a songs folder */
typedef struct { int d, t[5], v[4], bass; } mchord_t;
typedef struct { int st, len, x, m; } mnote_t;
typedef struct {
  int tonic, bpm, nhook, calls;                /* calls: rnd() draws this song took */
  mchord_t ch[4];
  mnote_t hook[40];
  int bass_i, arp_i;                           /* BASSES[bass_i], ARPS[arp_i] */
  double swing, pad_cut;
} msong_t;
void music_compose(msong_t *s, const msong_t *prev, dsp_rng_t *rs);
extern const int MUSIC_BASSES[3][16], MUSIC_ARPS[3][16];
struct music;
/* the instrument log hook (applies to generators started afterwards) */
void music_test_log(struct music *m, void (*fn)(void *, const char *, const double *, int), void *ud);
/* true: cancel the master filter sweeps exactly as the browser does (notes 8.2), for parity tests */
void music_test_exact(struct music *m, bool exact);
/* frames rendered by the music clock; the generator's current song (false when none) */
int64_t music_test_frames(struct music *m);
bool    music_test_song(struct music *m, msong_t *out, int *bar, int *step);
/* random draws taken so far (the stream position, mulberry32 counts) */
uint64_t music_test_draws(struct music *m);
/* the master low-pass frequency (Hz) at a time, the duck gain, the voices in use */
double  music_test_lp_freq(struct music *m, double t);
int     music_test_voices(struct music *m);

#endif
