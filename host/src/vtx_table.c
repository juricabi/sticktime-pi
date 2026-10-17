/* VTX tables: the built-in defaults and Betaflight Configurator JSON files. See vtx_table.h.
 * The JSON reader is a small one for this format only (strict JSON, unknown keys skipped). */
#include "vtx_table.h"
#include "vtx_proto.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================ defaults */
/* the five common 5.8 GHz bands, in the order of the TBS Unify's 40-channel table (A1 = 0 .. R8 = 39) */
static const struct { const char *name; char letter; uint16_t f[8]; } BANDS[5] = {
  { "BOSCAM_A", 'A', { 5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725 } },
  { "BOSCAM_B", 'B', { 5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866 } },
  { "BOSCAM_E", 'E', { 5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945 } },
  { "FATSHARK", 'F', { 5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880 } },
  { "RACEBAND", 'R', { 5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917 } },
};

static void add_power(vtx_table_t *t, int value, int mw) {
  if (t->npowers >= VTX_MAX_POWERS) return;
  vtx_power_t *p = &t->power[t->npowers++];
  p->value = (uint16_t)value;
  vtx_mw_label(p->label, sizeof p->label, mw);
}

void vtx_table_default(vtx_table_t *t, vtx_table_kind_t kind, const uint8_t *dbm, int ndbm) {
  memset(t, 0, sizeof *t);
  static const char *const desc[] = { "StickTime Pi default: SmartAudio 1.0", "StickTime Pi default: SmartAudio 2.0",
                                      "StickTime Pi default: SmartAudio 2.1", "StickTime Pi default: Tramp" };
  snprintf(t->description, sizeof t->description, "%s", desc[kind]);
  t->nbands = 5;
  t->nchannels = 8;
  for (int b = 0; b < 5; b++) {
    snprintf(t->band[b].name, sizeof t->band[b].name, "%s", BANDS[b].name);
    t->band[b].letter = BANDS[b].letter;
    t->band[b].factory = kind != VTX_TABLE_TRAMP;
    memcpy(t->band[b].freq, BANDS[b].f, sizeof BANDS[b].f);
  }
  static const int mw4[4] = { 25, 200, 500, 800 };
  switch (kind) {
    case VTX_TABLE_SA1: {                    /* the spec's recommended DAC values for 25/200/500/800 mW */
      static const int dac[4] = { 7, 16, 25, 40 };
      for (int i = 0; i < 4; i++) add_power(t, dac[i], mw4[i]);
      break;
    }
    case VTX_TABLE_SA2:
      for (int i = 0; i < 4; i++) add_power(t, i, mw4[i]);
      break;
    case VTX_TABLE_SA21: {
      static const uint8_t fallback[3] = { 14, 20, 26 };
      if (!dbm || ndbm <= 0) { dbm = fallback; ndbm = 3; }
      for (int i = 0; i < ndbm; i++) add_power(t, dbm[i], vtx_dbm_to_mw(dbm[i]));
      break;
    }
    case VTX_TABLE_TRAMP: {
      static const int mw[5] = { 25, 100, 200, 400, 600 };
      for (int i = 0; i < 5; i++) add_power(t, mw[i], mw[i]);
      break;
    }
  }
}

/* ================================================================ lookups */
int vtx_table_band(const vtx_table_t *t, char letter) {
  char l = (char)(letter >= 'a' && letter <= 'z' ? letter - 32 : letter);
  for (int i = 0; i < t->nbands; i++) {
    char b = t->band[i].letter;
    if (b >= 'a' && b <= 'z') b = (char)(b - 32);
    if (b == l) return i;
  }
  return -1;
}

uint16_t vtx_table_freq(const vtx_table_t *t, int band, int channel) {
  if (band < 0 || band >= t->nbands || channel < 1 || channel > t->nchannels || channel > VTX_MAX_CHANNELS) return 0;
  return t->band[band].freq[channel - 1];
}

bool vtx_table_find_freq(const vtx_table_t *t, uint16_t freq, int *band, int *channel) {
  if (!freq) return false;
  for (int pass = 0; pass < 2; pass++)
    for (int b = 0; b < t->nbands; b++) {
      if (t->band[b].factory != (pass == 0)) continue;
      for (int c = 0; c < t->nchannels && c < VTX_MAX_CHANNELS; c++)
        if (t->band[b].freq[c] == freq) {
          if (band) *band = b;
          if (channel) *channel = c + 1;
          return true;
        }
    }
  return false;
}

int vtx_table_power_level(const vtx_table_t *t, int value) {
  for (int i = 0; i < t->npowers; i++)
    if (t->power[i].value == value) return i + 1;
  return 0;
}

/* ================================================================ JSON reading */
typedef struct {
  const char *s, *p, *e;
  char *err;
  size_t errlen;
  bool failed;
} jp_t;

static bool jfail(jp_t *j, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
  ;
static bool jfail(jp_t *j, const char *fmt, ...) {
  if (j->failed) return false;
  j->failed = true;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(j->err, j->errlen, fmt, ap);
  va_end(ap);
  return false;
}

static bool jsyntax(jp_t *j) {
  return jfail(j, "This isn't valid JSON (at byte %d).", (int)(j->p - j->s) + 1);
}

static void jws(jp_t *j) {
  while (j->p < j->e && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static bool jeat(jp_t *j, char c) {
  jws(j);
  if (j->p < j->e && *j->p == c) { j->p++; return true; }
  return false;
}

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool hex4(jp_t *j, unsigned *v) {
  if (j->e - j->p < 4) return false;
  *v = 0;
  for (int i = 0; i < 4; i++) {
    int h = hexv(j->p[i]);
    if (h < 0) return false;
    *v = *v << 4 | (unsigned)h;
  }
  j->p += 4;
  return true;
}

static void put(char *out, size_t n, size_t *k, unsigned char c) {
  if (*k + 1 < n) out[*k] = (char)c;
  (*k)++;
}

/* a string into out (truncated, NUL-terminated); *len = its full length in bytes (UTF-8) */
static bool jstr(jp_t *j, char *out, size_t n, size_t *len) {
  jws(j);
  if (j->p >= j->e || *j->p != '"') return jsyntax(j);
  j->p++;
  size_t k = 0;
  while (j->p < j->e && *j->p != '"') {
    unsigned char c = (unsigned char)*j->p++;
    if (c < 0x20) return jsyntax(j);
    if (c != '\\') { put(out, n, &k, c); continue; }
    if (j->p >= j->e) return jsyntax(j);
    char x = *j->p++;
    unsigned cp;
    switch (x) {
      case '"': case '\\': case '/': put(out, n, &k, (unsigned char)x); continue;
      case 'b': put(out, n, &k, '\b'); continue;
      case 'f': put(out, n, &k, '\f'); continue;
      case 'n': put(out, n, &k, '\n'); continue;
      case 'r': put(out, n, &k, '\r'); continue;
      case 't': put(out, n, &k, '\t'); continue;
      case 'u':
        if (!hex4(j, &cp)) return jsyntax(j);
        if (cp >= 0xD800 && cp < 0xDC00) {             /* a surrogate pair */
          unsigned lo;
          if (j->e - j->p < 6 || j->p[0] != '\\' || j->p[1] != 'u') return jsyntax(j);
          j->p += 2;
          if (!hex4(j, &lo) || lo < 0xDC00 || lo > 0xDFFF) return jsyntax(j);
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        } else if (cp >= 0xDC00 && cp < 0xE000) return jsyntax(j);
        if (cp < 0x80) put(out, n, &k, (unsigned char)cp);
        else if (cp < 0x800) { put(out, n, &k, (unsigned char)(0xC0 | cp >> 6)); put(out, n, &k, (unsigned char)(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) {
          put(out, n, &k, (unsigned char)(0xE0 | cp >> 12)); put(out, n, &k, (unsigned char)(0x80 | ((cp >> 6) & 0x3F)));
          put(out, n, &k, (unsigned char)(0x80 | (cp & 0x3F)));
        } else {
          put(out, n, &k, (unsigned char)(0xF0 | cp >> 18)); put(out, n, &k, (unsigned char)(0x80 | ((cp >> 12) & 0x3F)));
          put(out, n, &k, (unsigned char)(0x80 | ((cp >> 6) & 0x3F))); put(out, n, &k, (unsigned char)(0x80 | (cp & 0x3F)));
        }
        continue;
      default: return jsyntax(j);
    }
  }
  if (j->p >= j->e) return jsyntax(j);
  j->p++;
  if (n) out[k < n ? k : n - 1] = 0;
  if (len) *len = k;
  /* a truncated multi-byte character at the end would be invalid UTF-8: cut it off */
  if (n && k >= n) {
    size_t m = n - 1;
    while (m > 0 && ((unsigned char)out[m - 1] & 0xC0) == 0x80) m--;
    if (m > 0 && ((unsigned char)out[m - 1] & 0xC0) == 0xC0) out[m - 1] = 0;
  }
  return true;
}

static bool jnum(jp_t *j, double *v) {
  jws(j);
  const char *q = j->p;
  if (q < j->e && *q == '-') q++;
  if (q >= j->e || *q < '0' || *q > '9') return jsyntax(j);
  if (*q == '0') q++;
  else while (q < j->e && *q >= '0' && *q <= '9') q++;
  if (q < j->e && *q == '.') {
    q++;
    if (q >= j->e || *q < '0' || *q > '9') return jsyntax(j);
    while (q < j->e && *q >= '0' && *q <= '9') q++;
  }
  if (q < j->e && (*q == 'e' || *q == 'E')) {
    q++;
    if (q < j->e && (*q == '+' || *q == '-')) q++;
    if (q >= j->e || *q < '0' || *q > '9') return jsyntax(j);
    while (q < j->e && *q >= '0' && *q <= '9') q++;
  }
  char buf[64];
  size_t n = (size_t)(q - j->p);
  if (n >= sizeof buf) return jfail(j, "A number is too long (at byte %d).", (int)(j->p - j->s) + 1);
  memcpy(buf, j->p, n);
  buf[n] = 0;
  *v = strtod(buf, NULL);
  j->p = q;
  return true;
}

static bool jword(jp_t *j, const char *w) {
  size_t n = strlen(w);
  jws(j);
  if ((size_t)(j->e - j->p) >= n && !memcmp(j->p, w, n)) { j->p += n; return true; }
  return false;
}

static bool jbool(jp_t *j, bool *v) {
  if (jword(j, "true")) { *v = true; return true; }
  if (jword(j, "false")) { *v = false; return true; }
  return jsyntax(j);
}

static bool jskip(jp_t *j, int depth) {
  if (depth > 32) return jfail(j, "The JSON is nested too deeply.");
  jws(j);
  if (j->p >= j->e) return jsyntax(j);
  char c = *j->p;
  if (c == '"') return jstr(j, NULL, 0, NULL);
  if (c == '{' || c == '[') {
    char close = c == '{' ? '}' : ']';
    j->p++;
    if (jeat(j, close)) return true;
    for (;;) {
      if (c == '{') {
        if (!jstr(j, NULL, 0, NULL) || !jeat(j, ':')) return jsyntax(j);
      }
      if (!jskip(j, depth + 1)) return false;
      if (jeat(j, ',')) continue;
      if (jeat(j, close)) return true;
      return jsyntax(j);
    }
  }
  if (jword(j, "true") || jword(j, "false") || jword(j, "null")) return true;
  double d;
  return jnum(j, &d);
}

/* iterate an object's members: call with *first = true; returns false at the end (or on an error) */
static bool jmember(jp_t *j, bool *first, char *key, size_t kn) {
  if (j->failed) return false;
  if (*first) {
    *first = false;
    if (jeat(j, '}')) return false;
  } else {
    if (jeat(j, '}')) return false;
    if (!jeat(j, ',')) { jsyntax(j); return false; }
  }
  size_t len;
  if (!jstr(j, key, kn, &len)) return false;
  if (len >= kn) key[0] = 0;                 /* a long key is none of ours */
  if (!jeat(j, ':')) { jsyntax(j); return false; }
  return true;
}

/* the same for arrays: true when an element follows */
static bool jelem(jp_t *j, bool *first) {
  if (j->failed) return false;
  if (*first) {
    *first = false;
    return !jeat(j, ']');
  }
  if (jeat(j, ']')) return false;
  if (!jeat(j, ',')) { jsyntax(j); return false; }
  return true;
}

static bool is_int(double v, double lo, double hi) { return v == (double)(long)v && v >= lo && v <= hi; }

static void rtrim(char *s) {
  size_t n = strlen(s);
  while (n && s[n - 1] == ' ') s[--n] = 0;
}

static bool parse_band(jp_t *j, vtx_table_t *t) {
  int bi = t->nbands;
  if (bi >= VTX_MAX_BANDS) return jfail(j, "The table has more than %d bands.", VTX_MAX_BANDS);
  vtx_band_t *b = &t->band[bi];
  memset(b, 0, sizeof *b);
  if (!jeat(j, '{')) return jfail(j, "Band %d isn't an object.", bi + 1);
  bool first = true, have_letter = false, have_freq = false;
  char key[32];
  int nf = 0;
  while (jmember(j, &first, key, sizeof key)) {
    if (!strcmp(key, "name")) {
      size_t len;
      if (!jstr(j, b->name, sizeof b->name, &len)) return false;
      if (len >= sizeof b->name) return jfail(j, "Band %d: the name is longer than %d characters.", bi + 1, (int)sizeof b->name - 1);
      rtrim(b->name);
    } else if (!strcmp(key, "letter")) {
      char l[8];
      size_t len;
      if (!jstr(j, l, sizeof l, &len)) return false;
      if (len != 1 || l[0] <= ' ' || (unsigned char)l[0] >= 0x7F)
        return jfail(j, "Band %d: the letter must be one character (A-Z, 0-9).", bi + 1);
      b->letter = (char)(l[0] >= 'a' && l[0] <= 'z' ? l[0] - 32 : l[0]);   /* (the settings store it upper-case) */
      have_letter = true;
    } else if (!strcmp(key, "is_factory_band") || !strcmp(key, "factory_flag")) {
      if (!jbool(j, &b->factory)) return false;
    } else if (!strcmp(key, "frequencies")) {
      if (!jeat(j, '[')) return jfail(j, "Band %d: frequencies must be a list.", bi + 1);
      bool f1 = true;
      while (jelem(j, &f1)) {
        double v;
        if (!jnum(j, &v)) return false;
        if (nf >= VTX_MAX_CHANNELS) return jfail(j, "Band %d has more than %d channels.", bi + 1, VTX_MAX_CHANNELS);
        if (!is_int(v, 0, VTX_FREQ_MAX) || (v != 0 && v < VTX_FREQ_MIN))
          return jfail(j, "Band %d, channel %d: %g isn't a frequency in MHz (0 or %d to %d).", bi + 1, nf + 1, v, VTX_FREQ_MIN, VTX_FREQ_MAX);
        b->freq[nf++] = (uint16_t)v;
      }
      if (j->failed) return false;
      have_freq = true;
    } else if (!jskip(j, 1)) return false;
  }
  if (j->failed) return false;
  if (!have_letter) return jfail(j, "Band %d has no letter.", bi + 1);
  if (!have_freq || nf == 0) return jfail(j, "Band %d has no frequencies.", bi + 1);
  int dup = vtx_table_band(t, b->letter);           /* (searches the bands before this one) */
  if (dup >= 0) return jfail(j, "Bands %d and %d both use the letter %c.", dup + 1, bi + 1, b->letter);
  if (!b->name[0]) snprintf(b->name, sizeof b->name, "BAND_%c", b->letter);
  if (nf > t->nchannels) t->nchannels = nf;
  t->nbands++;
  return true;
}

static bool parse_power(jp_t *j, vtx_table_t *t) {
  int pi = t->npowers;
  if (pi >= VTX_MAX_POWERS) return jfail(j, "The table has more than %d power levels.", VTX_MAX_POWERS);
  vtx_power_t *p = &t->power[pi];
  memset(p, 0, sizeof *p);
  if (!jeat(j, '{')) return jfail(j, "Power level %d isn't an object.", pi + 1);
  bool first = true, have_value = false;
  char key[32];
  while (jmember(j, &first, key, sizeof key)) {
    if (!strcmp(key, "value")) {
      double v;
      if (!jnum(j, &v)) return false;
      if (!is_int(v, 0, 65535)) return jfail(j, "Power level %d: the value must be a whole number from 0 to 65535.", pi + 1);
      p->value = (uint16_t)v;
      have_value = true;
    } else if (!strcmp(key, "label")) {
      size_t len;
      if (!jstr(j, p->label, sizeof p->label, &len)) return false;
      if (len >= sizeof p->label) return jfail(j, "Power level %d: the label is longer than %d characters.", pi + 1, (int)sizeof p->label - 1);
      rtrim(p->label);
    } else if (!jskip(j, 1)) return false;
  }
  if (j->failed) return false;
  if (!have_value) return jfail(j, "Power level %d has no value.", pi + 1);
  if (!p->label[0]) snprintf(p->label, sizeof p->label, "%u", p->value);
  t->npowers++;
  return true;
}

static bool parse_vtx_table(jp_t *j, vtx_table_t *t, bool *have_bands, bool *have_powers) {
  if (!jeat(j, '{')) return jfail(j, "vtx_table must be an object.");
  bool first = true;
  char key[32];
  while (jmember(j, &first, key, sizeof key)) {
    bool bands = !strcmp(key, "bands_list"), powers = !strcmp(key, "powerlevels_list");
    if (bands || powers) {
      if (!jeat(j, '[')) return jfail(j, "%s must be a list.", key);
      bool f1 = true;
      while (jelem(j, &f1))
        if (!(bands ? parse_band(j, t) : parse_power(j, t))) return false;
      if (j->failed) return false;
      if (bands) *have_bands = true;
      else *have_powers = true;
    } else if (!jskip(j, 1)) return false;
  }
  return !j->failed;
}

int vtx_table_parse_json(const char *json, size_t len, vtx_table_t *t, char *err, size_t errlen) {
  char dummy[8];
  if (!err || !errlen) { err = dummy; errlen = sizeof dummy; }
  err[0] = 0;
  memset(t, 0, sizeof *t);
  jp_t j = { json, json, json + len, err, errlen, false };
  if (len >= 3 && !memcmp(json, "\xEF\xBB\xBF", 3)) j.p += 3;     /* a BOM from a Windows editor */
  bool have_table = false, have_bands = false, have_powers = false;
  if (!jeat(&j, '{')) {
    jfail(&j, "This isn't a VTX table: a Betaflight VTX table file is a JSON object.");
  } else {
    bool first = true;
    char key[32];
    while (jmember(&j, &first, key, sizeof key)) {
      if (!strcmp(key, "description")) {
        size_t n;
        if (!jstr(&j, t->description, sizeof t->description, &n)) break;
      } else if (!strcmp(key, "version")) {
        char v[32];
        size_t n;
        if (!jstr(&j, v, sizeof v, &n)) break;
        if (v[0] != '1' || (v[1] && v[1] != '.')) { jfail(&j, "Version %s of the VTX table format isn't supported (1.0 is).", v); break; }
      } else if (!strcmp(key, "vtx_table")) {
        if (!parse_vtx_table(&j, t, &have_bands, &have_powers)) break;
        have_table = true;
      } else if (!jskip(&j, 1)) break;
    }
    if (!j.failed) {
      jws(&j);
      if (j.p != j.e) jsyntax(&j);
    }
  }
  if (!j.failed && !have_table) jfail(&j, "This isn't a VTX table: it has no \"vtx_table\".");
  if (!j.failed && (!have_bands || t->nbands == 0)) jfail(&j, "The VTX table has no bands.");
  if (!j.failed && (!have_powers || t->npowers == 0)) jfail(&j, "The VTX table has no power levels.");
  if (j.failed) {
    memset(t, 0, sizeof *t);
    return -1;
  }
  return 0;
}

/* ================================================================ JSON writing */
typedef struct { char *p; size_t len, cap; bool oom; } buf_t;

static void bput(buf_t *b, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
  ;
static void bput(buf_t *b, const char *fmt, ...) {
  if (b->oom) return;
  for (;;) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0) { b->oom = true; return; }
    if ((size_t)n < b->cap - b->len) { b->len += (size_t)n; return; }
    size_t nc = b->cap * 2 + (size_t)n;
    char *np = realloc(b->p, nc);
    if (!np) { b->oom = true; return; }
    b->p = np;
    b->cap = nc;
  }
}

static void bstr(buf_t *b, const char *s) {
  bput(b, "\"");
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') bput(b, "\\%c", c);
    else if (c < 0x20) bput(b, "\\u%04x", c);
    else bput(b, "%c", c);
  }
  bput(b, "\"");
}

char *vtx_table_to_json(const vtx_table_t *t, size_t *len) {
  buf_t b = { malloc(4096), 0, 4096, false };
  if (!b.p) return NULL;
  b.p[0] = 0;
  bput(&b, "{\n  \"description\": ");
  bstr(&b, t->description);
  bput(&b, ",\n  \"version\": \"1.0\",\n  \"vtx_table\": {\n    \"bands_list\": [");
  for (int i = 0; i < t->nbands; i++) {
    const vtx_band_t *bd = &t->band[i];
    char letter[2] = { bd->letter, 0 };
    bput(&b, "%s\n      {\n        \"name\": ", i ? "," : "");
    bstr(&b, bd->name);
    bput(&b, ",\n        \"letter\": ");
    bstr(&b, letter);
    bput(&b, ",\n        \"is_factory_band\": %s,\n        \"frequencies\": [", bd->factory ? "true" : "false");
    for (int c = 0; c < t->nchannels; c++) bput(&b, "%s%u", c ? ", " : "", bd->freq[c]);
    bput(&b, "]\n      }");
  }
  bput(&b, "\n    ],\n    \"powerlevels_list\": [");
  for (int i = 0; i < t->npowers; i++) {
    bput(&b, "%s\n      {\n        \"value\": %u,\n        \"label\": ", i ? "," : "", t->power[i].value);
    bstr(&b, t->power[i].label);
    bput(&b, "\n      }");
  }
  bput(&b, "\n    ]\n  }\n}\n");
  if (b.oom) { free(b.p); return NULL; }
  if (len) *len = b.len;
  return b.p;
}
