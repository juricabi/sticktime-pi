/* The player's own songs (notes audio-music.md 10.2, emulator-features.md 2.20, 3.6): the files in
 * the songs folder (mp3, ogg/oga Vorbis, wav, flac) in natural, case-insensitive name order, looped.
 *
 *  - A decoder thread (normal priority) opens, decodes (minimp3, stb_vorbis, dr_wav, dr_flac: all
 *    vendored in third_party/ and compiled here, once), down-mixes to stereo (Web Audio's rules for
 *    quad and 5.1), resamples to the output rate when the file's differs (a 48-tap Kaiser-windowed
 *    sinc, interpolated polyphase) and fills a ~4 s float ring. It never runs in the audio thread:
 *    FAT reads can stall for hundreds of ms.
 *  - The audio thread reads the ring (songs_read): lock-free, no allocation. "Next" fades the old
 *    song out over 20 ms, then the new one comes in with a 5 ms fade-in (as after any gap).
 *  - The end of a song runs straight into the next file (wrapping); a boundary that starts or ends
 *    "hot" (not near silence) gets a 20 ms fade so it doesn't click.
 *  - A file that can't be opened or decodes to nothing is skipped with a log line; when every file
 *    fails, songs_playable() turns false (the music falls back to the generated songs).
 *  - songs_rescan() (an upload or delete): rescanned in the decoder thread; the song playing keeps
 *    playing; if it was removed, the file after it (by name) follows.
 * Locks: a mutex guards the control state and the list; the decoder never holds it during file I/O,
 * and the audio thread never takes it. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "songs.h"
#include "plat.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- the decoders (unmodified single files; their warnings are theirs) */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wunknown-warning-option"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wuninitialized"
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wold-style-declaration"
#pragma GCC diagnostic ignored "-Wbitwise-instead-of-logical"
#pragma GCC diagnostic ignored "-Wtautological-compare"
#endif
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_NO_STDIO
#include "minimp3_ex.h"
#define DR_WAV_IMPLEMENTATION
#define DR_WAV_NO_WCHAR
#include "dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_WCHAR
#include "dr_flac.h"
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_INTEGER_CONVERSION
#define get_bits stbv_get_bits                 /* minimp3 has a static get_bits too */
#include "stb_vorbis.c"
#undef get_bits
#undef L
#undef C
#undef R
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#define RING_SECONDS 4
#define NSEG 16                                /* song starts in the ring (for "now playing") */
#define CHUNK 2048                             /* frames decoded per step (file rate) */
#define EDGE_MS 20                             /* boundary fades */
#define FADE_NEXT_MS 20
#define FADE_IN_MS 5
#define HOT 0.003f                             /* a boundary louder than this (-50 dBFS) is faded */
#define RS_HALF 24                             /* resampler: 48 taps */
#define RS_TAPS (2 * RS_HALF)
#define RS_PH 256                              /* phases (interpolated) */
#define NAME_MAX_ 768                           /* a FAT long name: 255 UTF-16 units as UTF-8 */

enum { F_NONE = 0, F_WAV, F_FLAC, F_OGG, F_MP3 };
enum { RD_PLAY = 0, RD_FADE, RD_WAIT };

typedef struct { char *name; bool failed; } entry_t;
typedef struct { int64_t pos; int idx; char name[NAME_MAX_]; } seg_t;   /* name: the file's */

/* the windowed-sinc resampler (decoder thread) */
typedef struct {
  bool on;
  double step, pos;                            /* input frames per output frame; next output position */
  float *tab;                                  /* (RS_PH + 1) x RS_TAPS */
  float *in;                                   /* stereo input frames not yet consumed */
  int nin, cap;
} rs_t;

typedef struct {
  int fmt, channels, rate;
  FILE *fp;                                    /* mp3 (callbacks) */
  mp3dec_io_t io;
  mp3dec_ex_t *mp3;
  drwav wav;
  drflac *flac;
  stb_vorbis *ogg;
  int64_t frames_out;                          /* frames of this song pushed */
  bool start_hot_known, start_hot;
  float *raw, *st, *out;                       /* decode buffer (file channels), stereo, resampled */
  int out_cap;
  rs_t rs;
  float hold[2 * 48000 * EDGE_MS / 1000 * 4];  /* holdback for the end fade (up to 192 kHz) */
  int nhold, hold_len;
} dec_t;

struct songs {
  int rate;
  char dir[1024];
  plat_thread th;
  bool th_ok;
  plat_mutex mu;
  plat_cond cv;
  /* control, under mu */
  bool quit, active, rescan_req;
  int req;                                     /* (sequence << 1) | fade: next / restart requests */
  char req_heard[NAME_MAX_];                   /* the file heard when the request was made */
  int req_nexts;                               /* nexts asked since the decoder last looked */
  entry_t *list;
  int count, nfailed;
  int cur;                                     /* the entry being decoded (-1: none) */
  int after;                                   /* cur == -1 after a rescan removed it: play this next */
  char cur_name[NAME_MAX_];
  /* atomics */
  int a_req, a_skip_seq, a_count, a_playable, a_failures, a_heard, a_heard_slot, a_seg_w, a_seg_r;
  int64_t a_w, a_r, a_skip_to;
  /* the ring */
  float *ring;
  int cap;
  seg_t seg[NSEG];
  /* reader (audio thread) */
  int rd_req, rd_state, rd_fade, rd_fade_len, rd_fadein, rd_fadein_len, rd_seg_r;
  int rd_heard_seq;                            /* the segment heard (its slot stays reserved) */
  bool rd_gap;
  /* decoder thread */
  dec_t d;
  int handled;                                 /* the request handled last (under mu) */
  bool seg_pending;
};

/* ================================================================ names */

int songs_name_cmp(const char *a, const char *b) {
  const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
  while (*p && *q) {
    if (isdigit(*p) && isdigit(*q)) {
      while (*p == '0' && isdigit(p[1])) p++;
      while (*q == '0' && isdigit(q[1])) q++;
      const unsigned char *p2 = p, *q2 = q;
      while (isdigit(*p2)) p2++;
      while (isdigit(*q2)) q2++;
      if (p2 - p != q2 - q) return (p2 - p) < (q2 - q) ? -1 : 1;
      for (; p < p2; p++, q++) if (*p != *q) return *p < *q ? -1 : 1;
      continue;
    }
    int x = tolower(*p), y = tolower(*q);
    if (x != y) return x < y ? -1 : 1;
    p++; q++;
  }
  if (*p || *q) return *p ? 1 : -1;
  return strcmp(a, b);                         /* equal but for case or zeros: a stable order */
}

static int cmp_entries(const void *x, const void *y) {
  return songs_name_cmp(((const entry_t *)x)->name, ((const entry_t *)y)->name);
}

static bool playable_ext(const char *name) {
  const char *e = strrchr(name, '.');
  if (!e || e == name) return false;
  static const char *const EXT[] = { ".mp3", ".ogg", ".oga", ".wav", ".flac" };
  for (size_t i = 0; i < sizeof EXT / sizeof EXT[0]; i++) {
    const char *a = e, *b = EXT[i];
    while (*a && *b && tolower((unsigned char)*a) == *b) { a++; b++; }
    if (!*a && !*b) return true;
  }
  return false;
}

static void strip_ext(char *out, size_t n, const char *name) {
  plat_strlcpy(out, name, n);
  char *e = strrchr(out, '.');
  if (e && e != out) *e = 0;
}

typedef struct { entry_t *v; int n, cap; } elist_t;

static int scan_cb(void *ud, const char *name, bool is_dir, int64_t size, int64_t mtime) {
  elist_t *l = ud;
  (void)size; (void)mtime;
  if (is_dir || name[0] == '.' || !playable_ext(name)) return 0;
  if (l->n == l->cap) {
    int nc = l->cap ? 2 * l->cap : 64;
    entry_t *v = realloc(l->v, sizeof(entry_t) * (size_t)nc);
    if (!v) return 0;
    l->v = v; l->cap = nc;
  }
  char *s = strdup(name);
  if (!s) return 0;
  l->v[l->n].name = s;
  l->v[l->n].failed = false;
  l->n++;
  return 0;
}

static elist_t scan_dir(const char *dir) {
  elist_t l = { NULL, 0, 0 };
  if (dir[0]) plat_list_dir(dir, scan_cb, &l);
  if (l.n > 1) qsort(l.v, (size_t)l.n, sizeof(entry_t), cmp_entries);
  return l;
}

static void free_list(entry_t *v, int n) {
  for (int i = 0; i < n; i++) free(v[i].name);
  free(v);
}

/* ================================================================ resampler */

static double bessel_i0(double x) {
  double s = 1, t = 1;
  for (int k = 1; k < 40; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; if (t < 1e-12 * s) break; }
  return s;
}

static int rs_init(rs_t *r, int in_rate, int out_rate) {
  r->on = in_rate != out_rate;
  r->nin = 0;
  if (!r->on) return 0;
  r->step = (double)in_rate / out_rate;
  if (!r->tab) {
    r->tab = malloc(sizeof(float) * (RS_PH + 1) * RS_TAPS);
    if (!r->tab) return -1;
  }
  double fc = 0.9 * (out_rate < in_rate ? (double)out_rate / in_rate : 1.0), beta = 8.0, i0b = bessel_i0(beta);
  for (int ph = 0; ph <= RS_PH; ph++) {
    double frac = (double)ph / RS_PH, sum = 0;
    float *t = r->tab + (size_t)ph * RS_TAPS;
    for (int j = 0; j < RS_TAPS; j++) {
      double x = frac + RS_HALF - 1 - j;       /* distance from the output position, input frames */
      double u = x / RS_HALF, w = fabs(u) < 1 ? bessel_i0(beta * sqrt(1 - u * u)) / i0b : 0;
      double s = x == 0 ? 1 : sin(M_PI * fc * x) / (M_PI * fc * x);
      t[j] = (float)(fc * s * w);
      sum += t[j];
    }
    for (int j = 0; j < RS_TAPS; j++) t[j] = (float)(t[j] / sum);   /* unity gain at DC */
  }
  /* history: RS_HALF - 1 frames of silence before the song */
  int need = RS_HALF + CHUNK + RS_TAPS;
  if (r->cap < need) {
    float *in = realloc(r->in, sizeof(float) * 2 * (size_t)need);
    if (!in) return -1;
    r->in = in; r->cap = need;
  }
  memset(r->in, 0, sizeof(float) * 2 * (RS_HALF - 1));
  r->nin = RS_HALF - 1;
  r->pos = RS_HALF - 1;
  return 0;
}

/* append n stereo input frames (NULL: n frames of silence), produce outputs into out; returns count */
static int rs_run(rs_t *r, const float *x, int n, float *out, int out_cap) {
  if (r->nin + n > r->cap) {
    int nc = r->nin + n + RS_TAPS;
    float *in = realloc(r->in, sizeof(float) * 2 * (size_t)nc);
    if (!in) return 0;
    r->in = in; r->cap = nc;
  }
  if (x) memcpy(r->in + 2 * r->nin, x, sizeof(float) * 2 * (size_t)n);
  else memset(r->in + 2 * r->nin, 0, sizeof(float) * 2 * (size_t)n);
  r->nin += n;
  int k = 0;
  while (k < out_cap) {
    int base = (int)r->pos;
    if (base + RS_HALF >= r->nin) break;
    double fp = (r->pos - base) * RS_PH;
    int ph = (int)fp;
    float a = (float)(fp - ph);
    const float *t0 = r->tab + (size_t)ph * RS_TAPS, *t1 = t0 + RS_TAPS;
    const float *s = r->in + 2 * (base - RS_HALF + 1);
    float l = 0, rr = 0;
    for (int j = 0; j < RS_TAPS; j++) {
      float h = t0[j] + a * (t1[j] - t0[j]);
      l += h * s[2 * j];
      rr += h * s[2 * j + 1];
    }
    out[2 * k] = l; out[2 * k + 1] = rr;
    k++;
    r->pos += r->step;
  }
  int drop = (int)r->pos - RS_HALF + 1;        /* keep the history the next outputs need */
  if (drop > 0) {
    if (drop > r->nin) drop = r->nin;
    memmove(r->in, r->in + 2 * drop, sizeof(float) * 2 * (size_t)(r->nin - drop));
    r->nin -= drop;
    r->pos -= drop;
  }
  return k;
}

/* ================================================================ decoders */

static size_t mp3_read(void *buf, size_t size, void *ud) { return fread(buf, 1, size, (FILE *)ud); }
static int mp3_seek(uint64_t pos, void *ud) { return fseek((FILE *)ud, (long)pos, SEEK_SET) ? -1 : 0; }

static void dec_close(dec_t *d) {
  switch (d->fmt) {
    case F_WAV: drwav_uninit(&d->wav); break;
    case F_FLAC: drflac_close(d->flac); d->flac = NULL; break;
    case F_OGG: stb_vorbis_close(d->ogg); d->ogg = NULL; break;
    case F_MP3: mp3dec_ex_close(d->mp3); break;
    default: break;
  }
  if (d->fp) { fclose(d->fp); d->fp = NULL; }
  d->fmt = F_NONE;
}

/* open path; returns NULL or the reason it can't be played */
static const char *dec_open(dec_t *d, const char *path, int out_rate) {
  unsigned char magic[12] = { 0 };
  FILE *f = fopen(path, "rb");
  if (!f) return "can't open the file";
  size_t got = fread(magic, 1, sizeof magic, f);
  fclose(f);
  if (got < 4) return "empty or too short";
  d->fmt = F_NONE;
  if (!memcmp(magic, "RIFF", 4) || !memcmp(magic, "RIFX", 4) || !memcmp(magic, "RF64", 4) || !memcmp(magic, "FORM", 4)) {
    if (!drwav_init_file(&d->wav, path, NULL)) return "not a WAV file dr_wav can read";
    d->fmt = F_WAV; d->channels = (int)d->wav.channels; d->rate = (int)d->wav.sampleRate;
  } else if (!memcmp(magic, "fLaC", 4)) {
    d->flac = drflac_open_file(path, NULL);
    if (!d->flac) return "broken FLAC file";
    d->fmt = F_FLAC; d->channels = (int)d->flac->channels; d->rate = (int)d->flac->sampleRate;
  } else if (!memcmp(magic, "OggS", 4)) {
    int err = 0;
    d->ogg = stb_vorbis_open_filename(path, &err, NULL);
    if (!d->ogg) return "not an Ogg Vorbis file (Opus isn't supported)";
    stb_vorbis_info vi = stb_vorbis_get_info(d->ogg);
    d->fmt = F_OGG; d->channels = vi.channels; d->rate = (int)vi.sample_rate;
  } else {
    d->fp = fopen(path, "rb");
    if (!d->fp) return "can't open the file";
    if (!d->mp3) d->mp3 = malloc(sizeof *d->mp3);
    if (!d->mp3) { fclose(d->fp); d->fp = NULL; return "out of memory"; }
    d->io.read = mp3_read; d->io.read_data = d->fp;
    d->io.seek = mp3_seek; d->io.seek_data = d->fp;
    if (mp3dec_ex_open_cb(d->mp3, &d->io, MP3D_SEEK_TO_SAMPLE | MP3D_DO_NOT_SCAN) != 0) {
      mp3dec_ex_close(d->mp3);
      fclose(d->fp); d->fp = NULL;
      return "not an audio file (or a broken MP3)";
    }
    d->fmt = F_MP3; d->channels = d->mp3->info.channels; d->rate = d->mp3->info.hz;
  }
  if (d->channels < 1 || d->channels > 8 || d->rate < 4000 || d->rate > 384000) {
    dec_close(d);
    return "unsupported channel count or sample rate";
  }
  if (!d->raw) d->raw = malloc(sizeof(float) * CHUNK * 8);
  if (!d->st) d->st = malloc(sizeof(float) * CHUNK * 2);
  int oc = (int)((double)CHUNK * out_rate / d->rate) + 2 * RS_TAPS + 16;
  if (oc > d->out_cap) {
    free(d->out);
    d->out = malloc(sizeof(float) * 2 * (size_t)oc);
    d->out_cap = d->out ? oc : 0;
  }
  if (!d->raw || !d->st || !d->out || rs_init(&d->rs, d->rate, out_rate) != 0) { dec_close(d); return "out of memory"; }
  d->frames_out = 0;
  d->start_hot_known = false;
  d->nhold = 0;
  d->hold_len = out_rate * EDGE_MS / 1000;
  if (d->hold_len * 2 > (int)(sizeof d->hold / sizeof d->hold[0])) d->hold_len = (int)(sizeof d->hold / sizeof d->hold[0]) / 2;
  return NULL;
}

/* up to CHUNK frames into d->raw (file channels); 0 at the end (or an error) */
static int dec_read(dec_t *d) {
  switch (d->fmt) {
    case F_WAV: return (int)drwav_read_pcm_frames_f32(&d->wav, CHUNK, d->raw);
    case F_FLAC: return (int)drflac_read_pcm_frames_f32(d->flac, CHUNK, d->raw);
    case F_OGG: return stb_vorbis_get_samples_float_interleaved(d->ogg, d->channels, d->raw, CHUNK * d->channels);
    case F_MP3: {
      int ch = d->channels;
      size_t n = mp3dec_ex_read(d->mp3, d->raw, (size_t)CHUNK * (size_t)ch);
      return (int)(n / (size_t)ch);
    }
    default: return 0;
  }
}

/* file channels -> stereo (Web Audio's down-mix for quad and 5.1; Vorbis orders 5.1 differently) */
static void to_stereo(const dec_t *d, int n) {
  const float *x = d->raw;
  float *y = d->st;
  int ch = d->channels;
  const float k = 0.70710678f;
  for (int i = 0; i < n; i++, x += ch) {
    float l, r;
    switch (ch) {
      case 1: l = r = x[0]; break;
      case 2: l = x[0]; r = x[1]; break;
      case 3: if (d->fmt == F_OGG) { l = x[0] + k * x[1]; r = x[2] + k * x[1]; } else { l = x[0] + k * x[2]; r = x[1] + k * x[2]; } break;
      case 4: l = 0.5f * (x[0] + x[2]); r = 0.5f * (x[1] + x[3]); break;
      case 6: if (d->fmt == F_OGG) { l = x[0] + k * (x[1] + x[3]); r = x[2] + k * (x[1] + x[4]); }
              else { l = x[0] + k * (x[2] + x[4]); r = x[1] + k * (x[2] + x[5]); } break;
      default: l = x[0]; r = x[1]; break;
    }
    y[2 * i] = l; y[2 * i + 1] = r;
  }
}

/* ================================================================ the ring (decoder side) */

static void do_rescan(songs_t *s);

/* wait until n frames (and a segment, if one is pending) fit; false: something else needs doing.
 * A rescan is done here too (it doesn't touch the ring): the folder's count stays right even
 * while nobody reads the ring. */
static bool wait_space(songs_t *s, int n) {
  for (;;) {
    int64_t w = __atomic_load_n(&s->a_w, __ATOMIC_ACQUIRE), r = __atomic_load_n(&s->a_r, __ATOMIC_ACQUIRE);
    int sw = __atomic_load_n(&s->a_seg_w, __ATOMIC_ACQUIRE), sr = __atomic_load_n(&s->a_seg_r, __ATOMIC_ACQUIRE);
    bool seg_ok = !s->seg_pending || sw - sr < NSEG - 1;
    if (s->cap - (w - r) >= n && seg_ok) return true;
    plat_mutex_lock(&s->mu);
    bool stop = s->quit || !s->active || s->req != s->handled, rescan = s->rescan_req;
    s->rescan_req = false;
    if (!stop && !rescan) plat_cond_wait(&s->cv, &s->mu, 10);
    plat_mutex_unlock(&s->mu);
    if (stop) { if (rescan) { plat_mutex_lock(&s->mu); s->rescan_req = true; plat_mutex_unlock(&s->mu); } return false; }
    if (rescan) do_rescan(s);
  }
}

/* publish n stereo frames to the ring (space was checked) */
static void ring_put(songs_t *s, const float *x, int n) {
  if (n <= 0) return;
  int64_t w = __atomic_load_n(&s->a_w, __ATOMIC_RELAXED);
  if (s->seg_pending) {                        /* this song's first frame: a segment for the title */
    int sw = __atomic_load_n(&s->a_seg_w, __ATOMIC_RELAXED);
    seg_t *g = &s->seg[sw % NSEG];
    g->pos = w;
    plat_mutex_lock(&s->mu);
    g->idx = s->cur;
    plat_strlcpy(g->name, s->cur_name, sizeof g->name);
    plat_mutex_unlock(&s->mu);
    __atomic_store_n(&s->a_seg_w, sw + 1, __ATOMIC_RELEASE);
    s->seg_pending = false;
  }
  for (int i = 0; i < n; i++) {
    int64_t p = (w + i) % s->cap;
    s->ring[2 * p] = x[2 * i];
    s->ring[2 * p + 1] = x[2 * i + 1];
  }
  __atomic_store_n(&s->a_w, w + n, __ATOMIC_RELEASE);
}

static float peak(const float *x, int n) {
  float m = 0;
  for (int i = 0; i < 2 * n; i++) { float a = fabsf(x[i]); if (a > m) m = a; }
  return m;
}

/* output-rate frames of the current song: start fade, holdback, ring. false: interrupted */
static bool emit(songs_t *s, float *x, int n) {
  dec_t *d = &s->d;
  int edge = d->hold_len;
  if (!d->start_hot_known && n > 0) {
    int probe = n < edge / 10 + 1 ? n : edge / 10 + 1;     /* the first 2 ms */
    d->start_hot = peak(x, probe) > HOT;
    d->start_hot_known = true;
  }
  if (d->start_hot && d->frames_out < edge) {
    for (int i = 0; i < n && d->frames_out + i < edge; i++) {
      float g = (float)(d->frames_out + i) / (float)edge;
      x[2 * i] *= g; x[2 * i + 1] *= g;
    }
  }
  d->frames_out += n;
  /* holdback: the last EDGE_MS stay here until the next frames (or the end) arrive */
  int total = d->nhold + n, out = total - edge;
  if (out > 0) {
    if (!wait_space(s, out)) return false;
    int from_hold = out < d->nhold ? out : d->nhold;
    ring_put(s, d->hold, from_hold);
    ring_put(s, x, out - from_hold);
    memmove(d->hold, d->hold + 2 * from_hold, sizeof(float) * 2 * (size_t)(d->nhold - from_hold));
    d->nhold -= from_hold;
    int rest = n - (out - from_hold);
    memcpy(d->hold + 2 * d->nhold, x + 2 * (out - from_hold), sizeof(float) * 2 * (size_t)rest);
    d->nhold += rest;
  } else {
    memcpy(d->hold + 2 * d->nhold, x, sizeof(float) * 2 * (size_t)n);
    d->nhold += n;
  }
  return true;
}

/* the song ended: flush the resampler, fade a hot end, publish the holdback */
static bool finish(songs_t *s) {
  dec_t *d = &s->d;
  if (d->rs.on) {
    int k = rs_run(&d->rs, NULL, RS_HALF, d->out, d->out_cap);
    if (k > 0 && !emit(s, d->out, k)) return false;
  }
  int n = d->nhold, probe = d->hold_len / 10 + 1;
  if (n > 0 && peak(d->hold + 2 * (n > probe ? n - probe : 0), n > probe ? probe : n) > HOT) {
    for (int i = 0; i < n; i++) {
      float g = (float)(n - i) / (float)(n + 1);
      d->hold[2 * i] *= g; d->hold[2 * i + 1] *= g;
    }
  }
  if (n > 0) {
    if (!wait_space(s, n)) return false;
    ring_put(s, d->hold, n);
  }
  d->nhold = 0;
  return true;
}

/* ================================================================ the decoder thread */

static void publish_counts(songs_t *s) {      /* with mu held */
  __atomic_store_n(&s->a_count, s->count, __ATOMIC_RELEASE);
  __atomic_store_n(&s->a_playable, s->count > 0 && s->nfailed < s->count ? 1 : 0, __ATOMIC_RELEASE);
}

static int next_ok(songs_t *s, int from) {    /* with mu held: the next entry after from not failed */
  for (int k = 1; k <= s->count; k++) {
    int i = ((from < 0 ? -1 : from) + k) % s->count;
    if (i < 0) i += s->count;
    if (!s->list[i].failed) return i;
  }
  return -1;
}

static void do_rescan(songs_t *s) {
  elist_t l = scan_dir(s->dir);
  plat_mutex_lock(&s->mu);
  char was[NAME_MAX_];
  bool had = s->cur >= 0 && s->cur < s->count;
  plat_strlcpy(was, had ? s->list[s->cur].name : s->cur_name, sizeof was);
  free_list(s->list, s->count);
  s->list = l.v; s->count = l.n; s->nfailed = 0;
  s->cur = -1; s->after = 0;
  if (was[0]) {
    for (int i = 0; i < s->count; i++) if (!strcmp(s->list[i].name, was)) { s->cur = i; break; }
    if (s->cur < 0) {                          /* removed: the next one by name follows */
      s->after = 0;
      for (int i = 0; i < s->count; i++) if (songs_name_cmp(s->list[i].name, was) > 0) { s->after = i; break; }
    }
  }
  publish_counts(s);
  bool empty = s->count == 0;
  plat_mutex_unlock(&s->mu);
  if (empty && s->d.fmt != F_NONE) { dec_close(&s->d); s->d.nhold = 0; }
}

/* the next song to open: after cur (or the remembered one after a removal); -1 none */
static int pick_after(songs_t *s, bool same) {   /* with mu held */
  if (s->count == 0) return -1;
  if (s->cur < 0) {
    int a = s->after % s->count;
    return s->list[a].failed ? next_ok(s, a) : a;
  }
  if (same) return s->list[s->cur].failed ? next_ok(s, s->cur) : s->cur;
  return next_ok(s, s->cur);
}

/* open the song at index i, skipping (and logging) broken files; true when one is open */
static bool open_from(songs_t *s, int i) {
  dec_t *d = &s->d;
  for (int tries = 0; i >= 0 && tries <= s->count; tries++) {
    char path[sizeof s->dir + NAME_MAX_ + 2], name[NAME_MAX_];
    plat_mutex_lock(&s->mu);
    if (i >= s->count) { plat_mutex_unlock(&s->mu); return false; }
    plat_strlcpy(name, s->list[i].name, sizeof name);
    s->cur = i;
    plat_strlcpy(s->cur_name, name, sizeof s->cur_name);
    plat_mutex_unlock(&s->mu);
    snprintf(path, sizeof path, "%s/%s", s->dir, name);
    const char *why = dec_open(d, path, s->rate);
    if (!why) { s->seg_pending = true; return true; }
    fprintf(stderr, "songs: skipping %s: %s\n", name, why);
    plat_mutex_lock(&s->mu);
    if (i < s->count && !s->list[i].failed) { s->list[i].failed = true; s->nfailed++; }
    __atomic_add_fetch(&s->a_failures, 1, __ATOMIC_ACQ_REL);
    publish_counts(s);
    i = next_ok(s, i);
    plat_mutex_unlock(&s->mu);
  }
  return false;
}

/* a next/restart request: drop the current song, tell the reader where the new one starts */
static void handle_request(songs_t *s, int req) {
  dec_t *d = &s->d;
  dec_close(d);
  d->nhold = 0;
  s->seg_pending = false;
  /* relative to the song heard (the decoder may be seconds ahead of it): next = the file after
   * it, restart = it again; when it's gone from the list (or nothing was heard), the decoder's */
  char heard[NAME_MAX_];
  plat_mutex_lock(&s->mu);
  plat_strlcpy(heard, s->req_heard, sizeof heard);
  int h = -1;
  for (int k = 0; heard[0] && k < s->count; k++) if (!strcmp(s->list[k].name, heard)) { h = k; break; }
  int nexts = s->req_nexts, i;
  s->req_nexts = 0;
  if (h < 0) i = pick_after(s, (req & 1) == 0);
  else if (req & 1) i = next_ok(s, h);
  else i = s->list[h].failed ? next_ok(s, h) : h;
  for (int k = 1; k < nexts && i >= 0; k++) i = next_ok(s, i);   /* next, next: two files on */
  s->handled = req;
  plat_mutex_unlock(&s->mu);
  __atomic_store_n(&s->a_skip_to, __atomic_load_n(&s->a_w, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
  __atomic_store_n(&s->a_skip_seq, req, __ATOMIC_RELEASE);
  if (i >= 0) open_from(s, i);
}

static void *dec_thread(void *arg) {
  songs_t *s = arg;
  dec_t *d = &s->d;
  plat_thread_lower_priority();
  for (;;) {
    plat_mutex_lock(&s->mu);
    if (s->quit) { plat_mutex_unlock(&s->mu); break; }
    bool rescan = s->rescan_req, active = s->active;
    int req = s->req;
    s->rescan_req = false;
    if (!rescan && req == s->handled && (!active || (d->fmt == F_NONE && (s->count == 0 || s->nfailed >= s->count)))) {
      plat_cond_wait(&s->cv, &s->mu, 250);     /* idle */
      plat_mutex_unlock(&s->mu);
      continue;
    }
    plat_mutex_unlock(&s->mu);
    if (rescan) do_rescan(s);
    if (req != s->handled) { handle_request(s, req); continue; }
    if (!active) { if (d->fmt != F_NONE) dec_close(d); d->nhold = 0; continue; }
    if (d->fmt == F_NONE) {
      plat_mutex_lock(&s->mu);
      int i = pick_after(s, true);
      plat_mutex_unlock(&s->mu);
      if (i < 0 || !open_from(s, i)) continue;
    }
    int n = dec_read(d);
    if (n > 0) {
      to_stereo(d, n);
      int k = n;
      if (d->rs.on) k = rs_run(&d->rs, d->st, n, d->out, d->out_cap);
      else memcpy(d->out, d->st, sizeof(float) * 2 * (size_t)n);
      if (k > 0) emit(s, d->out, k);           /* false: a request or a stop; the loop sees to it */
      continue;
    }
    /* the end of the file (or a decode error) */
    bool empty = d->frames_out == 0;
    if (!empty && !finish(s)) continue;
    dec_close(d);
    plat_mutex_lock(&s->mu);
    if (empty && s->cur >= 0 && s->cur < s->count) {
      fprintf(stderr, "songs: skipping %s: no audio decoded\n", s->list[s->cur].name);
      if (!s->list[s->cur].failed) { s->list[s->cur].failed = true; s->nfailed++; }
      __atomic_add_fetch(&s->a_failures, 1, __ATOMIC_ACQ_REL);
      publish_counts(s);
    }
    int i = pick_after(s, false);
    plat_mutex_unlock(&s->mu);
    if (i >= 0) open_from(s, i);
  }
  dec_close(d);
  return NULL;
}

/* ================================================================ public */

songs_t *songs_new(int rate, const char *dir) {
  songs_t *s = calloc(1, sizeof *s);
  if (!s) return NULL;
  s->rate = rate > 0 ? rate : 48000;
  plat_strlcpy(s->dir, dir ? dir : "", sizeof s->dir);
  size_t n = strlen(s->dir);
  while (n > 1 && s->dir[n - 1] == '/') s->dir[--n] = 0;
  s->cap = s->rate * RING_SECONDS;
  s->ring = calloc((size_t)s->cap * 2, sizeof(float));
  if (!s->ring) { free(s); return NULL; }
  s->cur = -1;
  s->a_heard = -1;
  s->a_heard_slot = -1;
  s->rd_heard_seq = -1;
  s->rd_fade_len = s->rate * FADE_NEXT_MS / 1000;
  s->rd_fadein_len = s->rate * FADE_IN_MS / 1000;
  plat_mutex_init(&s->mu);
  plat_cond_init(&s->cv);
  elist_t l = scan_dir(s->dir);
  s->list = l.v; s->count = l.n;
  publish_counts(s);
  if (plat_thread_start(&s->th, dec_thread, s, false) == 0) s->th_ok = true;
  return s;
}

void songs_free(songs_t *s) {
  if (!s) return;
  plat_mutex_lock(&s->mu);
  s->quit = true;
  plat_cond_broadcast(&s->cv);
  plat_mutex_unlock(&s->mu);
  if (s->th_ok) plat_thread_join(s->th);
  dec_close(&s->d);
  free(s->d.mp3);
  free(s->d.raw); free(s->d.st); free(s->d.out);
  free(s->d.rs.tab); free(s->d.rs.in);
  free_list(s->list, s->count);
  free(s->ring);
  plat_mutex_destroy(&s->mu);
  free(s);
}

static void request(songs_t *s, bool fade) {  /* with mu held */
  /* the song heard now (its segment slot is not reused while it is the reader's current one) */
  int slot = __atomic_load_n(&s->a_heard_slot, __ATOMIC_ACQUIRE);
  if (s->req == s->handled || !fade) {         /* a new request (a next pending: count it on) */
    s->req_heard[0] = 0;
    if (slot >= 0) plat_strlcpy(s->req_heard, s->seg[slot % NSEG].name, sizeof s->req_heard);
    s->req_nexts = 0;
  }
  if (fade) s->req_nexts++;
  s->req = ((((unsigned)s->req >> 1) + 1) << 1) | (fade ? 1 : 0);
  __atomic_store_n(&s->a_req, s->req, __ATOMIC_RELEASE);
  plat_cond_broadcast(&s->cv);
}

void songs_set_active(songs_t *s, bool on) {
  if (!s) return;
  plat_mutex_lock(&s->mu);
  if (on && !s->active) request(s, false);     /* from the start of the current song, no fade */
  s->active = on;
  plat_cond_broadcast(&s->cv);
  plat_mutex_unlock(&s->mu);
}

void songs_next(songs_t *s) {
  if (!s) return;
  plat_mutex_lock(&s->mu);
  if (s->active) request(s, true);
  plat_mutex_unlock(&s->mu);
}

void songs_rescan(songs_t *s) {
  if (!s) return;
  plat_mutex_lock(&s->mu);
  s->rescan_req = true;
  plat_cond_broadcast(&s->cv);
  plat_mutex_unlock(&s->mu);
}

int songs_count(songs_t *s) { return s ? __atomic_load_n(&s->a_count, __ATOMIC_ACQUIRE) : 0; }
bool songs_playable(songs_t *s) { return s && __atomic_load_n(&s->a_playable, __ATOMIC_ACQUIRE) != 0; }
int songs_failures(songs_t *s) { return s ? __atomic_load_n(&s->a_failures, __ATOMIC_ACQUIRE) : 0; }
int songs_index(songs_t *s) { return s ? __atomic_load_n(&s->a_heard, __ATOMIC_ACQUIRE) : -1; }
int songs_buffered(songs_t *s) {
  if (!s) return 0;
  return (int)(__atomic_load_n(&s->a_w, __ATOMIC_ACQUIRE) - __atomic_load_n(&s->a_r, __ATOMIC_ACQUIRE));
}

void songs_title(songs_t *s, char *out, size_t n) {
  if (!out || !n) return;
  out[0] = 0;
  if (!s) return;
  for (int tries = 0; tries < 3; tries++) {
    int slot = __atomic_load_n(&s->a_heard_slot, __ATOMIC_ACQUIRE);
    if (slot < 0) break;
    char t[NAME_MAX_];
    memcpy(t, s->seg[slot % NSEG].name, sizeof t);
    t[sizeof t - 1] = 0;
    if (__atomic_load_n(&s->a_heard_slot, __ATOMIC_ACQUIRE) == slot) { strip_ext(out, n, t); return; }
  }
  plat_mutex_lock(&s->mu);                     /* nothing heard yet: the file being decoded */
  if (s->active && s->cur >= 0) strip_ext(out, n, s->cur_name);
  plat_mutex_unlock(&s->mu);
}

void songs_read(songs_t *s, float *L, float *R, int n) {
  int req = __atomic_load_n(&s->a_req, __ATOMIC_ACQUIRE);
  if (req != s->rd_req) {
    s->rd_req = req;
    if ((req & 1) && s->rd_state == RD_PLAY && !s->rd_gap) { s->rd_state = RD_FADE; s->rd_fade = s->rd_fade_len; }
    else s->rd_state = RD_WAIT;
  }
  int64_t r = __atomic_load_n(&s->a_r, __ATOMIC_RELAXED);
  int64_t w = __atomic_load_n(&s->a_w, __ATOMIC_ACQUIRE);
  int sw = __atomic_load_n(&s->a_seg_w, __ATOMIC_ACQUIRE);
  for (int i = 0; i < n; i++) {
    if (s->rd_state == RD_WAIT) {
      if (__atomic_load_n(&s->a_skip_seq, __ATOMIC_ACQUIRE) == s->rd_req) {
        r = __atomic_load_n(&s->a_skip_to, __ATOMIC_ACQUIRE);
        w = __atomic_load_n(&s->a_w, __ATOMIC_ACQUIRE);
        sw = __atomic_load_n(&s->a_seg_w, __ATOMIC_ACQUIRE);
        /* songs decoded ahead and skipped by the jump were never heard: drop their starts */
        while (s->rd_seg_r < sw && s->seg[s->rd_seg_r % NSEG].pos < r) s->rd_seg_r++;
        s->rd_state = RD_PLAY;
        s->rd_gap = true;
      } else { L[i] = R[i] = 0; continue; }
    }
    int64_t limit = w;
    if (s->rd_state == RD_FADE && __atomic_load_n(&s->a_skip_seq, __ATOMIC_ACQUIRE) == s->rd_req) {
      int64_t st = __atomic_load_n(&s->a_skip_to, __ATOMIC_ACQUIRE);
      if (st < limit) limit = st;
    }
    if (r >= limit) {                          /* nothing decoded (yet): silence, then a fade-in */
      L[i] = R[i] = 0;
      if (s->rd_state == RD_FADE) s->rd_state = RD_WAIT;
      else s->rd_gap = true;
      continue;
    }
    float g = 1;
    if (s->rd_state == RD_FADE) {
      g = (float)s->rd_fade / (float)(s->rd_fade_len + 1);
      if (--s->rd_fade <= 0) s->rd_state = RD_WAIT;
    } else {
      if (s->rd_gap) { s->rd_fadein = s->rd_fadein_len; s->rd_gap = false; }
      if (s->rd_fadein > 0) { g = 1.0f - (float)s->rd_fadein / (float)s->rd_fadein_len; s->rd_fadein--; }
    }
    int64_t p = r % s->cap;
    L[i] = s->ring[2 * p] * g;
    R[i] = s->ring[2 * p + 1] * g;
    while (s->rd_seg_r < sw && s->seg[s->rd_seg_r % NSEG].pos <= r) {
      __atomic_store_n(&s->a_heard, s->seg[s->rd_seg_r % NSEG].idx, __ATOMIC_RELEASE);
      __atomic_store_n(&s->a_heard_slot, s->rd_seg_r % NSEG, __ATOMIC_RELEASE);
      s->rd_heard_seq = s->rd_seg_r;
      s->rd_seg_r++;
    }
    r++;
  }
  /* the oldest segment still needed: the decoder never reuses its slot (the title reads it) */
  int keep = s->rd_heard_seq >= 0 && s->rd_heard_seq < s->rd_seg_r ? s->rd_heard_seq : s->rd_seg_r;
  __atomic_store_n(&s->a_seg_r, keep, __ATOMIC_RELEASE);
  __atomic_store_n(&s->a_r, r, __ATOMIC_RELEASE);
}
