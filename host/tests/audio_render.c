/* audio_render: renders a scripted game sequence through the whole sound path (sfx + music + output
 * stage, via audio_render_offline) into a WAV, to compare with the browser's reference renders
 * (made by a headless-Chromium harness that renders the same scenario files as float32 WAVs).
 *
 *   audio_render [options] scenario.json|scenario.txt out.wav
 *     --s16                 16-bit PCM WAV (default: float32, the reference harness' layout)
 *     --null                render, write nothing (speed check)
 *     --seconds S           length (default: the scenario's "seconds", else its frames)
 *     --rate N              sample rate (default: the scenario's "fs", else 48000)
 *     --frame N             samples per game frame (default: the scenario's "frame", else rate/60)
 *     --seed N              (default: the scenario's "seed", else 1)
 *     --vol V               Sound level 0..1 (default: the scenario's "vol", else 0.8)
 *     --music on|off  --music-vol V  --music-source generated|songs  --songs DIR  --mono
 *   Without arguments it runs a self-test (it lives in tests/, so the test runner runs it).
 *
 * JSON scenario (ref_render.py's format, plus optional extras):
 *   {"fs": 46080, "frame": 768, "seconds": 8, "seed": 1, "vol": 0.8,
 *    "seq": [[sT, quad, state, aTf, aVu, aE2, aW, sA, sE, sR, aS, aF, crashes, hv], ...],  one per frame
 *                                                    (the last repeats; [] = no audio_game calls)
 *    "tones": [{"t": 1.0, "f": 1000, "d": 120, "p": 0, "flags": 0, "incr": 0}, ...],     playTone at frame round(t*fs/frame)
 *    extras: "sound": true, "music": false, "music_vol": 0.6, "music_source": "generated", "songs": "dir",
 *            "mono": false, "haptics": [{"t", "ms"}], "resets": [t, ...], "next": [t, ...],
 *            "levels": [{"t": 3.0, "vol": 0.5, "sound": .., "music": .., "music_vol": .., "music_source": .., "mono": ..}]}
 *   "patches" (the reference's raw mode) can't be applied here and is ignored with a warning.
 * Text scenario (one command per line, # comments):
 *   fs N | frame N | seconds S | seed N | vol V | sound on|off | music on|off | music_vol V |
 *   music_source generated|songs | songs DIR | mono on|off       (after the first frame: a change at the next frame)
 *   a v0 .. v13        one frame of STICKTIME_TEST.audio() values      repeat N   the last frame N more times
 *   tone f d [p [flags [incr]]] | haptic ms | reset | next          at the next frame */
#include "audio.h"
#include "audio_internal.h"
#include "plat.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t frames; int period, buffer, format, card, dev; } audio_test_info_t;
void audio_test_info(audio_t *a, audio_test_info_t *ti);

/* ================================================================ a small JSON reader */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype_t;
typedef struct jv {
  jtype_t t;
  double n;
  char *s;
  struct jv *kid;          /* array items / object values */
  char **key;              /* object keys */
  int len, cap;
} jv_t;

static const char *jp;
static char jerr[160];

static void jws(void) { while (*jp && isspace((unsigned char)*jp)) jp++; }
static int jfail(const char *m) { if (!jerr[0]) snprintf(jerr, sizeof jerr, "%s near \"%.20s\"", m, jp); return -1; }
static int jvalue(jv_t *v, int depth);

static int jstring(char **out) {
  if (*jp != '"') return jfail("string expected");
  jp++;
  size_t cap = 16, n = 0;
  char *s = malloc(cap);
  if (!s) return jfail("out of memory");
  while (*jp && *jp != '"') {
    unsigned c = (unsigned char)*jp++;
    if (c == '\\') {
      char e = *jp++;
      switch (e) {
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'u': {
          unsigned u = 0;
          for (int i = 0; i < 4; i++) {
            char h = *jp++;
            u = u * 16 + (unsigned)(isdigit((unsigned char)h) ? h - '0' : (tolower((unsigned char)h) - 'a' + 10) & 15);
          }
          c = u;
          break;
        }
        case 0: free(s); return jfail("bad escape");
        default: c = (unsigned char)e; break;
      }
    }
    if (n + 4 >= cap) {
      char *t = realloc(s, cap *= 2);
      if (!t) { free(s); return jfail("out of memory"); }
      s = t;
    }
    if (c < 0x80) s[n++] = (char)c;
    else if (c < 0x800) { s[n++] = (char)(0xc0 | c >> 6); s[n++] = (char)(0x80 | (c & 63)); }
    else { s[n++] = (char)(0xe0 | c >> 12); s[n++] = (char)(0x80 | ((c >> 6) & 63)); s[n++] = (char)(0x80 | (c & 63)); }
  }
  if (*jp != '"') { free(s); return jfail("unterminated string"); }
  jp++;
  s[n] = 0;
  *out = s;
  return 0;
}

static jv_t *jpush(jv_t *v) {
  if (v->len == v->cap) {
    int cap = v->cap ? v->cap * 2 : 8;
    jv_t *k = realloc(v->kid, sizeof *k * (size_t)cap);
    if (!k) return NULL;
    v->kid = k;
    if (v->t == J_OBJ) {
      char **ks = realloc(v->key, sizeof *ks * (size_t)cap);
      if (!ks) return NULL;
      v->key = ks;
    }
    v->cap = cap;
  }
  jv_t *k = &v->kid[v->len++];
  memset(k, 0, sizeof *k);
  return k;
}

static int jvalue(jv_t *v, int depth) {
  memset(v, 0, sizeof *v);
  if (depth > 64) return jfail("too deep");
  jws();
  if (*jp == '{' || *jp == '[') {
    char close = *jp == '{' ? '}' : ']';
    v->t = *jp == '{' ? J_OBJ : J_ARR;
    jp++;
    jws();
    if (*jp == close) { jp++; return 0; }
    for (;;) {
      char *key = NULL;
      if (v->t == J_OBJ) {
        jws();
        if (jstring(&key)) return -1;
        jws();
        if (*jp != ':') { free(key); return jfail("':' expected"); }
        jp++;
      }
      jv_t *k = jpush(v);
      if (!k) { free(key); return jfail("out of memory"); }
      if (v->t == J_OBJ) v->key[v->len - 1] = key;
      if (jvalue(k, depth + 1)) return -1;
      jws();
      if (*jp == ',') { jp++; continue; }
      if (*jp == close) { jp++; return 0; }
      return jfail("',' or end expected");
    }
  }
  if (*jp == '"') { v->t = J_STR; return jstring(&v->s); }
  if (!strncmp(jp, "true", 4)) { v->t = J_BOOL; v->n = 1; jp += 4; return 0; }
  if (!strncmp(jp, "false", 5)) { v->t = J_BOOL; jp += 5; return 0; }
  if (!strncmp(jp, "null", 4)) { v->t = J_NULL; jp += 4; return 0; }
  char *end;
  v->n = strtod(jp, &end);
  if (end == jp) return jfail("value expected");
  v->t = J_NUM;
  jp = end;
  return 0;
}

static void jfree(jv_t *v) {
  for (int i = 0; i < v->len; i++) {
    jfree(&v->kid[i]);
    if (v->t == J_OBJ) free(v->key[i]);
  }
  free(v->kid);
  free(v->key);
  free(v->s);
}

static jv_t *jget(jv_t *o, const char *k) {
  if (!o || o->t != J_OBJ) return NULL;
  for (int i = 0; i < o->len; i++) if (!strcmp(o->key[i], k)) return &o->kid[i];
  return NULL;
}
static double jnum(jv_t *o, const char *k, double def) {
  jv_t *v = jget(o, k);
  return v && (v->t == J_NUM || v->t == J_BOOL) ? v->n : def;
}
static const char *jstr(jv_t *o, const char *k, const char *def) {
  jv_t *v = jget(o, k);
  return v && v->t == J_STR ? v->s : def;
}

/* ================================================================ the scenario */

enum { EV_TONE, EV_HAPTIC, EV_RESET, EV_NEXT, EV_LEVELS };
typedef struct { long frame; int kind; int arg[5]; audio_levels_t lv; } ev_t;

#define NV 16
typedef struct {
  int fs, frame, seed;
  double seconds;                  /* <= 0: the frames given */
  audio_levels_t lv;               /* at the start */
  char songs[512];
  double (*seq)[NV];
  int nvals;
  long nseq, capseq;
  ev_t *ev;
  long nev, capev;
  bool patches;
} scen_t;

static void scen_init(scen_t *s) {
  memset(s, 0, sizeof *s);
  s->seed = 1;
  s->nvals = 14;
  s->lv = (audio_levels_t){ true, 80, false, 60, AUDIO_MUSIC_GENERATED, false };
}
static void scen_free(scen_t *s) { free(s->seq); free(s->ev); }

static int add_frame(scen_t *s, const double *v, int n) {
  if (s->nseq == s->capseq) {
    long cap = s->capseq ? s->capseq * 2 : 256;
    double (*q)[NV] = realloc(s->seq, sizeof *q * (size_t)cap);
    if (!q) return -1;
    s->seq = q;
    s->capseq = cap;
  }
  memset(s->seq[s->nseq], 0, sizeof s->seq[0]);
  for (int i = 0; i < n && i < NV; i++) s->seq[s->nseq][i] = v[i];
  if (s->nseq == 0 || n < s->nvals) s->nvals = n < NV ? n : NV;
  s->nseq++;
  return 0;
}
static ev_t *add_ev(scen_t *s, long frame, int kind) {
  if (s->nev == s->capev) {
    long cap = s->capev ? s->capev * 2 : 64;
    ev_t *e = realloc(s->ev, sizeof *e * (size_t)cap);
    if (!e) return NULL;
    s->ev = e;
    s->capev = cap;
  }
  ev_t *e = &s->ev[s->nev++];
  memset(e, 0, sizeof *e);
  e->frame = frame;
  e->kind = kind;
  return e;
}
static long frame_at(const scen_t *s, double t) { return (long)floor(t * s->fs / s->frame + 0.5); }
static int vol100(double v) { return (int)floor((v < 0 ? 0 : v > 1 ? 1 : v) * 100 + 0.5); }
static int source_of(const char *s) { return s && !strcmp(s, "songs") ? AUDIO_MUSIC_SONGS : AUDIO_MUSIC_GENERATED; }

/* level keys present in o change lv */
static void json_levels(jv_t *o, audio_levels_t *lv) {
  jv_t *v;
  if ((v = jget(o, "vol"))) lv->volume = vol100(v->n);
  if ((v = jget(o, "sound"))) lv->sound = v->n != 0;
  if ((v = jget(o, "music"))) lv->music = v->n != 0;
  if ((v = jget(o, "music_vol"))) lv->music_volume = vol100(v->n);
  if ((v = jget(o, "music_source")) && v->t == J_STR) lv->music_source = source_of(v->s);
  if ((v = jget(o, "mono"))) lv->mono = v->n != 0;
}

static int parse_json(scen_t *s, const char *text, char *err, size_t errlen) {
  jv_t root;
  jp = text;
  jerr[0] = 0;
  if (jvalue(&root, 0) || root.t != J_OBJ) {
    snprintf(err, errlen, "JSON: %s", jerr[0] ? jerr : "an object expected");
    jfree(&root);
    return -1;
  }
  s->fs = (int)jnum(&root, "fs", 48000);
  s->frame = (int)jnum(&root, "frame", 0);
  if (s->frame <= 0) s->frame = s->fs / 60;
  s->seconds = jnum(&root, "seconds", 0);
  s->seed = (int)jnum(&root, "seed", 1);
  json_levels(&root, &s->lv);
  plat_strlcpy(s->songs, jstr(&root, "songs", ""), sizeof s->songs);
  s->patches = jget(&root, "patches") != NULL;
  jv_t *seq = jget(&root, "seq");
  if (seq && seq->t == J_ARR)
    for (int k = 0; k < seq->len; k++) {
      jv_t *f = &seq->kid[k];
      double v[NV] = { 0 };
      int n = 0;
      if (f->t == J_ARR) for (int i = 0; i < f->len && i < NV; i++, n++) v[i] = f->kid[i].n;
      add_frame(s, v, n);
    }
  jv_t *a;
  if ((a = jget(&root, "tones")) && a->t == J_ARR)
    for (int i = 0; i < a->len; i++) {
      jv_t *t = &a->kid[i];
      ev_t *e = add_ev(s, frame_at(s, jnum(t, "t", 0)), EV_TONE);
      if (!e) break;
      e->arg[0] = (int)jnum(t, "f", 1000);
      e->arg[1] = (int)jnum(t, "d", 100);
      e->arg[2] = (int)jnum(t, "p", 0);
      e->arg[3] = (int)jnum(t, "flags", 0);
      e->arg[4] = (int)jnum(t, "incr", 0);
    }
  if ((a = jget(&root, "haptics")) && a->t == J_ARR)
    for (int i = 0; i < a->len; i++) {
      ev_t *e = add_ev(s, frame_at(s, jnum(&a->kid[i], "t", 0)), EV_HAPTIC);
      if (e) e->arg[0] = (int)jnum(&a->kid[i], "ms", 30);
    }
  if ((a = jget(&root, "resets")) && a->t == J_ARR)
    for (int i = 0; i < a->len; i++) add_ev(s, frame_at(s, a->kid[i].n), EV_RESET);
  if ((a = jget(&root, "next")) && a->t == J_ARR)
    for (int i = 0; i < a->len; i++) add_ev(s, frame_at(s, a->kid[i].n), EV_NEXT);
  if ((a = jget(&root, "levels")) && a->t == J_ARR) {
    /* each change builds on the previous one in time order */
    audio_levels_t lv = s->lv;
    for (int i = 0; i < a->len; i++) {
      ev_t *e = add_ev(s, frame_at(s, jnum(&a->kid[i], "t", 0)), EV_LEVELS);
      if (!e) break;
      json_levels(&a->kid[i], &lv);
      e->lv = lv;
    }
  }
  jfree(&root);
  return 0;
}

static bool on_word(const char *w) { return w && (!strcmp(w, "on") || !strcmp(w, "1") || !strcmp(w, "true") || !strcmp(w, "yes")); }

static int parse_text(scen_t *s, char *text, char *err, size_t errlen) {
  s->fs = 48000;
  int line = 0;
  bool fs_set = false;
  char *save = NULL;
  for (char *ln = strtok_r(text, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
    line++;
    char *hash = strchr(ln, '#');
    if (hash) *hash = 0;
    char *tok[NV + 2];
    int nt = 0;
    char *sv = NULL;
    for (char *t = strtok_r(ln, " \t\r", &sv); t && nt < NV + 2; t = strtok_r(NULL, " \t\r", &sv)) tok[nt++] = t;
    if (!nt) continue;
    const char *c = tok[0], *x = nt > 1 ? tok[1] : NULL;
    long at = s->nseq;
    audio_levels_t lv = s->lv;
    if (s->nev) for (long i = s->nev - 1; i >= 0; i--) if (s->ev[i].kind == EV_LEVELS) { lv = s->ev[i].lv; break; }
    bool lchange = false;
    if (!strcmp(c, "a")) {
      double v[NV] = { 0 };
      for (int i = 1; i < nt; i++) v[i - 1] = atof(tok[i]);
      if (add_frame(s, v, nt - 1)) { snprintf(err, errlen, "out of memory"); return -1; }
    } else if (!strcmp(c, "repeat") && x) {
      if (!s->nseq) { snprintf(err, errlen, "line %d: repeat before any frame", line); return -1; }
      double v[NV];
      memcpy(v, s->seq[s->nseq - 1], sizeof v);
      for (long n = atol(x); n > 0; n--) add_frame(s, v, s->nvals);
    } else if (!strcmp(c, "tone") && x) {
      ev_t *e = add_ev(s, at, EV_TONE);
      if (!e) return -1;
      for (int i = 0; i < 5; i++) e->arg[i] = nt > i + 1 ? atoi(tok[i + 1]) : 0;
    } else if (!strcmp(c, "haptic")) {
      ev_t *e = add_ev(s, at, EV_HAPTIC);
      if (e) e->arg[0] = x ? atoi(x) : 30;
    } else if (!strcmp(c, "reset")) add_ev(s, at, EV_RESET);
    else if (!strcmp(c, "next")) add_ev(s, at, EV_NEXT);
    else if (!strcmp(c, "fs") || !strcmp(c, "rate")) { s->fs = x ? atoi(x) : 48000; fs_set = true; }
    else if (!strcmp(c, "frame")) s->frame = x ? atoi(x) : 0;
    else if (!strcmp(c, "seconds")) s->seconds = x ? atof(x) : 0;
    else if (!strcmp(c, "seed")) s->seed = x ? atoi(x) : 1;
    else if (!strcmp(c, "songs")) plat_strlcpy(s->songs, x ? x : "", sizeof s->songs);
    else if (!strcmp(c, "vol")) { lv.volume = vol100(x ? atof(x) : 0.8); lchange = true; }
    else if (!strcmp(c, "volume")) { lv.volume = x ? atoi(x) : 80; lchange = true; }
    else if (!strcmp(c, "sound")) { lv.sound = on_word(x); lchange = true; }
    else if (!strcmp(c, "music")) { lv.music = on_word(x); lchange = true; }
    else if (!strcmp(c, "music_vol")) { lv.music_volume = vol100(x ? atof(x) : 0.6); lchange = true; }
    else if (!strcmp(c, "music_source")) { lv.music_source = source_of(x); lchange = true; }
    else if (!strcmp(c, "mono")) { lv.mono = on_word(x); lchange = true; }
    else { snprintf(err, errlen, "line %d: unknown command '%s'", line, c); return -1; }
    if (lchange) {
      if (!s->nseq) s->lv = lv;
      else {
        ev_t *e = add_ev(s, at, EV_LEVELS);
        if (e) e->lv = lv;
      }
    }
  }
  if (s->frame <= 0) s->frame = s->fs / 60;
  (void)fs_set;
  return 0;
}

/* ================================================================ render */

typedef struct {
  const char *out;                 /* "wav:<path>" | "wav16:<path>" | "null" */
  bool quiet;
} render_opt_t;

/* returns frames rendered (samples), -1 on error */
static long render(scen_t *s, const render_opt_t *o, char *err, size_t errlen) {
  if (s->fs < 8000 || s->fs > 384000 || s->frame <= 0) { snprintf(err, errlen, "bad fs %d / frame %d", s->fs, s->frame); return -1; }
  /* events by frame, keeping the file order within a frame (insertion sort: few, nearly sorted) */
  for (long i = 1; i < s->nev; i++) {
    ev_t e = s->ev[i];
    long j = i - 1;
    while (j >= 0 && s->ev[j].frame > e.frame) { s->ev[j + 1] = s->ev[j]; j--; }
    s->ev[j + 1] = e;
  }
  long nframes = s->seconds > 0 ? (long)floor(s->seconds * s->fs / s->frame + 0.5) : s->nseq;
  if (nframes <= 0) { snprintf(err, errlen, "nothing to render (no frames, no seconds)"); return -1; }
  audio_config_t c;
  memset(&c, 0, sizeof c);
  c.out = o->out;
  c.rate = s->fs;
  c.seed = (uint32_t)s->seed;
  c.music_dir = s->songs;
  char e[200];
  audio_t *a = audio_open(&c, e, sizeof e);
  if (!a) { snprintf(err, errlen, "audio_open: %s", e); return -1; }
  audio_status_t st;
  audio_status(a, &st);
  if (!st.ok) { snprintf(err, errlen, "%s", st.error); audio_close(a); return -1; }
  audio_set_levels(a, &s->lv);
  double dt = 1000.0 * s->frame / s->fs;
  long ei = 0;
  int64_t t0 = plat_now_us();
  for (long k = 0; k < nframes; k++) {
    while (ei < s->nev && s->ev[ei].frame <= k) {
      ev_t *v = &s->ev[ei++];
      switch (v->kind) {
        case EV_TONE: audio_tone(a, v->arg[0], v->arg[1], v->arg[2], v->arg[3], v->arg[4]); break;
        case EV_HAPTIC: audio_haptic(a, v->arg[0]); break;
        case EV_RESET: audio_game_reset(a); break;
        case EV_NEXT: audio_music_next(a); break;
        case EV_LEVELS: audio_set_levels(a, &v->lv); break;
      }
    }
    if (s->nseq) audio_game(a, s->seq[k < s->nseq ? k : s->nseq - 1], s->nvals, dt);
    audio_render_offline(a, s->frame);
  }
  int64_t us = plat_now_us() - t0;
  audio_status(a, &st);
  audio_test_info_t ti;
  audio_test_info(a, &ti);
  audio_close(a);
  if (!o->quiet) {
    double secs = (double)nframes * s->frame / s->fs;
    printf("rendered %ld frames x %d = %.3f s at %d Hz in %.1f ms (%.0fx real time)%s%s\n", nframes, s->frame, secs, s->fs,
           us / 1000.0, us > 0 ? secs * 1e6 / (double)us : 0.0, st.now_playing[0] ? ", music: " : "", st.now_playing);
  }
  if (st.error[0]) { snprintf(err, errlen, "%s", st.error); return -1; }
  return (long)ti.frames;
}

static int load(scen_t *s, const char *path, char *err, size_t errlen) {
  size_t len;
  char *text = plat_read_file(path, &len, 256u << 20);
  if (!text) { snprintf(err, errlen, "can't read %s", path); return -1; }
  const char *p = text;
  while (*p && isspace((unsigned char)*p)) p++;
  int r = *p == '{' ? parse_json(s, text, err, errlen) : parse_text(s, text, err, errlen);
  free(text);
  return r;
}

/* ================================================================ self-test (no arguments) */

static int selftest(void) {
  static const char *J =
    "{\"fs\": 46080, \"frame\": 768, \"seconds\": 0.5, \"seed\": 3, \"vol\": 0.75, \"note\": \"a \\\"q\\\" \\u00e9\",\n"
    " \"seq\": [[0, 1, 3, 0.0315, 0, 0, 0, 0, 0, 0, 0, 26, 0, 0], [0, 1, 4, 0.1, -1.5e0, 2, 0, 0.5, 0, 0, 0, 26, 1, 6.0]],\n"
    " \"tones\": [{\"t\": 0.0, \"f\": 1000, \"d\": 120}, {\"t\": 0.25, \"f\": 2000, \"d\": 100, \"flags\": 16}],\n"
    " \"levels\": [{\"t\": 0.2, \"mono\": true}, {\"t\": 0.3, \"vol\": 0.5}], \"resets\": [0.4], \"patches\": [] }";
  static const char *T =
    "# text form\nfs 48000\nframe 800\nseed 2\nvol 0.6\nmusic on\n"
    "a 0 1 4 0.2 0 0 0 0 0 0 0 26 0 0\nrepeat 9\ntone 1300 60\nmono on\na 0 1 4 0.3 0 0 0 0 0 0 0 26 0 0\nrepeat 4\n";
  int fail = 0;
  char err[200];
  render_opt_t o = { "null", true };
  scen_t s;
  scen_init(&s);
  if (parse_json(&s, J, err, sizeof err)) { fprintf(stderr, "self-test JSON: %s\n", err); return 1; }
  fail |= !(s.fs == 46080 && s.frame == 768 && s.seed == 3 && s.nseq == 2 && s.nvals == 14 && s.lv.volume == 75 && s.patches);
  fail |= !(s.nev == 5 && s.ev[0].kind == EV_TONE && s.ev[0].arg[0] == 1000 && s.ev[1].frame == 15 && s.ev[1].arg[3] == 16);
  fail |= !(s.seq[1][4] == -1.5 && s.seq[1][13] == 6.0);
  long n = render(&s, &o, err, sizeof err);
  fail |= n != 30 * 768;           /* 0.5 s at 60 Hz */
  if (n < 0) fprintf(stderr, "self-test render: %s\n", err);
  /* levels events build on each other: the second keeps mono */
  for (long i = 0; i < s.nev; i++) if (s.ev[i].kind == EV_LEVELS && s.ev[i].lv.volume == 50) fail |= !s.ev[i].lv.mono;
  scen_free(&s);

  scen_init(&s);
  char *t = strdup(T);
  if (!t || parse_text(&s, t, err, sizeof err)) { fprintf(stderr, "self-test text: %s\n", err); free(t); return 1; }
  free(t);
  fail |= !(s.fs == 48000 && s.frame == 800 && s.seed == 2 && s.nseq == 15 && s.lv.volume == 60 && s.lv.music);
  fail |= !(s.nev == 2 && s.ev[0].kind == EV_TONE && s.ev[0].frame == 10 && s.ev[1].kind == EV_LEVELS && s.ev[1].lv.mono);
  n = render(&s, &o, err, sizeof err);
  fail |= n != 15 * 800;
  scen_free(&s);
  if (fail) { fprintf(stderr, "audio_render self-test FAILED\n"); return 1; }
  printf("audio_render self-test ok (usage: audio_render [options] scenario.json|txt out.wav)\n");
  return 0;
}

/* ================================================================ main */

static void usage(void) {
  fputs("usage: audio_render [--s16] [--null] [--seconds S] [--rate N] [--frame N] [--seed N] [--vol V]\n"
        "                    [--music on|off] [--music-vol V] [--music-source generated|songs] [--songs DIR]\n"
        "                    [--mono] [--quiet] scenario.json|scenario.txt out.wav\n", stderr);
}

int main(int argc, char **argv) {
  if (argc == 1) return selftest();
  bool s16 = false, null = false, quiet = false;
  const char *in = NULL, *outp = NULL;
  double seconds = -1, vol = -1, mvol = -1;
  int rate = 0, frame = 0, seed = -1, music = -1, mono = -1, source = -1;
  const char *songs = NULL;
  for (int i = 1; i < argc; i++) {
    const char *o = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define VAL() (v ? (i++, v) : (fprintf(stderr, "%s needs a value\n", o), exit(2), ""))
    if (!strcmp(o, "--s16")) s16 = true;
    else if (!strcmp(o, "--null")) null = true;
    else if (!strcmp(o, "--quiet")) quiet = true;
    else if (!strcmp(o, "--mono")) mono = 1;
    else if (!strcmp(o, "--seconds")) seconds = atof(VAL());
    else if (!strcmp(o, "--rate")) rate = atoi(VAL());
    else if (!strcmp(o, "--frame")) frame = atoi(VAL());
    else if (!strcmp(o, "--seed")) seed = atoi(VAL());
    else if (!strcmp(o, "--vol")) vol = atof(VAL());
    else if (!strcmp(o, "--music")) music = on_word(VAL());
    else if (!strcmp(o, "--music-vol")) mvol = atof(VAL());
    else if (!strcmp(o, "--music-source")) source = source_of(VAL());
    else if (!strcmp(o, "--songs")) songs = VAL();
    else if (!strcmp(o, "-h") || !strcmp(o, "--help")) { usage(); return 0; }
    else if (o[0] == '-' && o[1]) { fprintf(stderr, "unknown option %s\n", o); usage(); return 2; }
    else if (!in) in = o;
    else if (!outp) outp = o;
    else { usage(); return 2; }
#undef VAL
  }
  if (!in || (!outp && !null)) { usage(); return 2; }
  scen_t s;
  scen_init(&s);
  char err[300];
  if (load(&s, in, err, sizeof err)) { fprintf(stderr, "%s: %s\n", in, err); scen_free(&s); return 1; }
  if (s.patches) fprintf(stderr, "note: \"patches\" ignored (the reference's raw mode has no switch in the C path)\n");
  if (seconds >= 0) s.seconds = seconds;
  if (rate > 0) { if (frame <= 0) frame = (int)((long long)s.frame * rate / s.fs); s.fs = rate; }
  if (frame > 0) s.frame = frame;
  if (seed >= 0) s.seed = seed;
  if (vol >= 0) s.lv.volume = vol100(vol);
  if (music >= 0) s.lv.music = music;
  if (mvol >= 0) s.lv.music_volume = vol100(mvol);
  if (source >= 0) s.lv.music_source = source;
  if (mono >= 0) s.lv.mono = true;
  if (songs) plat_strlcpy(s.songs, songs, sizeof s.songs);
  char out[1100];
  if (null) snprintf(out, sizeof out, "null");
  else snprintf(out, sizeof out, "%s:%s", s16 ? "wav16" : "wav", outp);
  render_opt_t ro = { out, quiet };
  long n = render(&s, &ro, err, sizeof err);
  scen_free(&s);
  if (n < 0) { fprintf(stderr, "%s\n", err); return 1; }
  return 0;
}
