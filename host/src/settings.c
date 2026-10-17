/* sticktime.txt: parsing that keeps every line, edits that touch only what changed, and the
 * known keys' defaults and validation. The format is described in settings.h.
 * image/rootfs/usr/lib/sticktime/settings.py implements the same rules; tests/settings_vectors.txt
 * holds both to them, byte for byte (warnings included).
 *
 * Beyond settings.h:
 *  - A lone CR also ends a line (old Mac files). Each line keeps its own line end; new lines use the
 *    file's first one. A BOM is kept. NUL bytes are dropped (a FAT power loss can leave zeros).
 *  - settings_get() returns the effective value of a known key: the file's value normalized
 *    (enums lower case, bools "on"/"off", ints canonical, wifi_country and channel_order upper case),
 *    or the default when the value is empty or invalid (an invalid one also gives a warning).
 *    hotspot_password falls back to its default when empty while hotspot is not off.
 *  - settings_set() stores valid values of known keys normalized the same way, and refuses values
 *    with a line break (they can't be stored). settings_validate() refuses those for every key. */
#include "settings.h"
#include "input.h"
#include "plat.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define X(key, kind, def, choices, live, reboot, help) { key, kind, def, choices, live, reboot, help },
#include "settings_keys.h"
const settings_key_t SETTINGS_KEYS[] = { SETTINGS_KEY_LIST };
#undef X
const int SETTINGS_NKEYS = (int)(sizeof SETTINGS_KEYS / sizeof SETTINGS_KEYS[0]);
const char SETTINGS_TEMPLATE[] = SETTINGS_TEMPLATE_TEXT;

#define KEY_MAX 64
#define FILE_MAX (1 << 20)
#define BOM "\xEF\xBB\xBF"

enum { L_BLANK, L_COMMENT, L_KV, L_BAD };
enum { W_NOEQ = 1, W_BADKEY = 2, W_UNTERM = 4, W_AFTERQ = 8, W_UTF8 = 16 };

typedef struct {
  char *text;               /* the line without its end; never contains NUL, CR or LF */
  size_t len;
  char term[3];             /* "\n", "\r\n", "\r", or "" on a last line without one */
  unsigned char kind, flags;
  char *key, *value;        /* L_KV: the lower-cased key, the value as parsed */
  char *eff;                /* known key: the normalized value, NULL when empty-invalid or invalid */
  int ki;                   /* index in SETTINGS_KEYS, -1 */
  size_t pre_len;           /* text through the '=' */
  size_t val_off, val_len;  /* the value as written, quotes included */
  size_t post_off;          /* after the value: spaces and the comment */
  bool comment;
} line_t;

struct settings {
  line_t *lines;
  int n, cap;
  bool bom, had_nul;
  char nl[3];
  int *last;                /* per known key: its effective line, -1 */
  bool hp_default;
  char *note;               /* a load problem, shown first in the warnings */
  char *warn;
};

/* ---------------------------------------------------------------- small helpers */
static bool is_ws(char c) { return c == ' ' || c == '\t'; }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static char upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alpha(char c) { c = lower(c); return c >= 'a' && c <= 'z'; }
static bool is_hex(char c) { c = lower(c); return is_digit(c) || (c >= 'a' && c <= 'f'); }

/* ASCII case-insensitive equality (settings.py lowers bytes the same way, never Unicode) */
static bool ieq(const char *a, const char *b) {
  while (*a && lower(*a) == lower(*b)) a++, b++;
  return lower(*a) == lower(*b);
}

static char *dupn(const char *s, size_t n) {
  char *d = malloc(n + 1);
  if (d) { memcpy(d, s, n); d[n] = 0; }
  return d;
}
static char *dup(const char *s) { return dupn(s, strlen(s)); }

/* strict UTF-8, as Python's decoder: no overlongs, surrogates or code points past U+10FFFF */
static bool utf8_ok(const char *str, size_t n) {
  const unsigned char *s = (const unsigned char *)str;
  size_t i = 0;
  while (i < n) {
    unsigned c = s[i];
    if (c < 0x80) { i++; continue; }
    size_t k;
    unsigned lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) k = 1;
    else if (c >= 0xE0 && c <= 0xEF) { k = 2; if (c == 0xE0) lo = 0xA0; if (c == 0xED) hi = 0x9F; }
    else if (c >= 0xF0 && c <= 0xF4) { k = 3; if (c == 0xF0) lo = 0x90; if (c == 0xF4) hi = 0x8F; }
    else return false;
    if (n - i <= k || s[i + 1] < lo || s[i + 1] > hi) return false;
    for (size_t j = 2; j <= k; j++) if (s[i + j] < 0x80 || s[i + j] > 0xBF) return false;
    i += k + 1;
  }
  return true;
}

/* lower-cased key into out (KEY_MAX + 1); false unless it is [a-z0-9_][a-z0-9_.-]*, 1..64 chars */
static bool make_key(const char *s, size_t n, char *out) {
  if (n == 0 || n > KEY_MAX) return false;
  for (size_t i = 0; i < n; i++) {
    char c = lower(s[i]);
    if (!((c >= 'a' && c <= 'z') || is_digit(c) || c == '_' || (i > 0 && (c == '.' || c == '-')))) return false;
    out[i] = c;
  }
  out[n] = 0;
  return true;
}

static int key_index(const char *key) {
  for (int i = 0; i < SETTINGS_NKEYS; i++) if (!strcmp(SETTINGS_KEYS[i].key, key)) return i;
  return -1;
}

const settings_key_t *settings_key(const char *key) {
  if (!key) return NULL;
  for (int i = 0; i < SETTINGS_NKEYS; i++) if (ieq(SETTINGS_KEYS[i].key, key)) return &SETTINGS_KEYS[i];
  return NULL;
}

/* ---------------------------------------------------------------- values */
/* [+-]digits with |v| <= INT32_MAX */
static bool parse_int(const char *s, int *out) {
  const char *p = s;
  bool neg = false;
  if (*p == '+' || *p == '-') neg = *p++ == '-';
  if (!is_digit(*p)) return false;
  int64_t v = 0;
  for (; is_digit(*p); p++) if ((v = v * 10 + (*p - '0')) > INT32_MAX) return false;
  if (*p) return false;
  *out = (int)(neg ? -v : v);
  return true;
}

/* [+-](digits[.[digits]] | .digits)[(e|E)[+-]digits]: no inf, nan, hex or spaces */
static bool parse_num(const char *s, double *out) {
  const char *p = s;
  int nd = 0;
  if (*p == '+' || *p == '-') p++;
  while (is_digit(*p)) p++, nd++;
  if (*p == '.') for (p++; is_digit(*p); p++) nd++;
  if (!nd) return false;
  if (*p == 'e' || *p == 'E') {
    p++;
    if (*p == '+' || *p == '-') p++;
    if (!is_digit(*p)) return false;
    while (is_digit(*p)) p++;
  }
  if (*p) return false;
  *out = strtod(s, NULL);
  return true;
}

static int parse_bool(const char *v) {      /* 1, 0, -1 */
  static const char *const yes[] = { "on", "yes", "true", "1" }, *const no[] = { "off", "no", "false", "0" };
  for (int i = 0; i < 4; i++) {
    if (ieq(v, yes[i])) return 1;
    if (ieq(v, no[i])) return 0;
  }
  return -1;
}

static bool in_choices(const char *choices, const char *v) {
  size_t n = strlen(v);
  for (const char *p = choices; ; ) {
    const char *bar = strchr(p, '|');
    size_t len = bar ? (size_t)(bar - p) : strlen(p);
    if (len == n && n) {
      size_t i = 0;
      while (i < n && lower(p[i]) == lower(v[i])) i++;
      if (i == n) return true;
    }
    if (!bar) return false;
    p = bar + 1;
  }
}

/* "min|max" of an SK_INT / SK_NUM key, as text */
static void range_text(const char *choices, char *lo, char *hi, size_t n) {
  const char *bar = strchr(choices, '|');
  size_t l = bar ? (size_t)(bar - choices) : strlen(choices);
  if (l >= n) l = n - 1;
  memcpy(lo, choices, l);
  lo[l] = 0;
  plat_strlcpy(hi, bar ? bar + 1 : "", n);
}

/* invert: words of letters; anything else separates them. Every word must name a stick
 * (mix_parse_invert decides what does) or be "none". */
static bool invert_ok(const char *v) {
  for (const char *p = v; *p; ) {
    if (!is_alpha(*p)) { p++; continue; }
    const char *q = p;
    while (is_alpha(*q)) q++;
    char tok[16];
    size_t len = (size_t)(q - p);
    if (len >= sizeof tok) return false;
    memcpy(tok, p, len);
    tok[len] = 0;
    if (!ieq(tok, "none")) {
      bool inv[4];
      mix_parse_invert(tok, inv);
      if (!inv[0] && !inv[1] && !inv[2] && !inv[3]) return false;
    }
    p = q;
  }
  return true;
}

static bool hostname_ok(const char *v) {      /* RFC 1123 label */
  size_t n = strlen(v);
  if (n < 1 || n > 63) return false;
  for (size_t i = 0; i < n; i++) {
    char c = v[i];
    if (!(is_alpha(c) || is_digit(c) || (c == '-' && i > 0 && i < n - 1))) return false;
  }
  return true;
}

static bool ipv4_ok(const char *v) {
  const char *p = v;
  for (int part = 0; part < 4; part++) {
    if (part && *p++ != '.') return false;
    if (!is_digit(*p)) return false;
    if (*p == '0' && is_digit(p[1])) return false;   /* leading zeros read as octal elsewhere */
    int x = 0, nd = 0;
    while (is_digit(*p)) {
      x = x * 10 + (*p++ - '0');
      if (++nd > 3) return false;
    }
    if (x > 255) return false;
  }
  return *p == 0;
}

static bool mode_dim(const char **pp) {      /* 1..9999, no leading zero */
  const char *p = *pp;
  int nd = 0;
  if (*p < '1' || *p > '9') return false;
  while (is_digit(*p)) { p++; if (++nd > 4) return false; }
  *pp = p;
  return true;
}

static bool hdmi_mode_ok(const char *v) {     /* WxH or WxH@Hz (Hz: 1-3 digits [.1-3 digits], > 0) */
  const char *p = v;
  if (!mode_dim(&p) || lower(*p) != 'x') return false;
  p++;
  if (!mode_dim(&p)) return false;
  if (!*p) return true;
  if (*p++ != '@') return false;
  const char *hz = p;
  int nd = 0;
  while (is_digit(*p)) { p++; if (++nd > 3) return false; }
  if (!nd) return false;
  if (*p == '.') {
    int nf = 0;
    for (p++; is_digit(*p); p++) if (++nf > 3) return false;
    if (!nf) return false;
  }
  return !*p && strtod(hz, NULL) > 0;
}

static bool script_ok(const char *v) {
  size_t n = strlen(v);
  if (n < 6 || n > 255 || v[0] != '/' || strstr(v, "..") || strchr(v, '\\')) return false;
  for (size_t i = 0; i < n; i++) if ((unsigned char)v[i] < 0x20 || v[i] == 0x7f) return false;
  return ieq(v + n - 4, ".lua");
}

/* NULL when v is fine for SETTINGS_KEYS[ki], else why (built in buf when it names choices) */
static const char *check(int ki, const char *v, char *buf, size_t bufsz) {
  const settings_key_t *k = &SETTINGS_KEYS[ki];
  const char *name = k->key;
  size_t n = strlen(v);
  if (strpbrk(v, "\r\n")) return "must not contain line breaks";
  if (!strcmp(name, "channel_order")) {
    char o[5];
    return mix_parse_order(v, o) ? NULL : "must be the letters A, E, T and R in any order";
  }
  if (!strcmp(name, "invert")) return invert_ok(v) ? NULL : "must be a list of ail, ele, thr, rud";
  if (!strcmp(name, "hostname"))
    return hostname_ok(v) ? NULL : "must be 1-63 letters, digits or hyphens, not starting or ending with a hyphen";
  if (!strcmp(name, "wifi_country")) return n == 2 && is_alpha(v[0]) && is_alpha(v[1]) ? NULL : "must be a two-letter country code";
  if (!strcmp(name, "hotspot_password")) return n == 0 || (n >= 8 && n <= 63) ? NULL : "must be 8-63 characters";
  if (!strcmp(name, "wifi_password")) {
    bool hex = n == 64;
    for (size_t i = 0; hex && i < n; i++) hex = is_hex(v[i]);
    return n == 0 || (n >= 8 && n <= 63) || hex ? NULL : "must be empty, 8-63 characters or 64 hex digits";
  }
  if (!strcmp(name, "wifi_module_ip")) return n == 0 || ipv4_ok(v) ? NULL : "must be an IPv4 address (like 192.168.1.50) or empty";
  if (!strcmp(name, "hdmi_mode")) return ieq(v, "auto") || hdmi_mode_ok(v) ? NULL : "must be auto, WxH or WxH@Hz (like 1280x720@60)";
  if (!strcmp(name, "latency")) {
    double d;
    return ieq(v, "auto") || ieq(v, "game") || (parse_num(v, &d) && d >= 0 && d <= 0.2) ? NULL
         : "must be auto, game or a number from 0 to 0.2";
  }
  if (!strcmp(name, "script")) return script_ok(v) ? NULL : "must be an SD path ending in .lua (like /SCRIPTS/TOOLS/StickTime.lua)";
  if (!strcmp(name, "wifi_ssid")) return n <= 32 ? NULL : "must be at most 32 bytes";
  if (!strcmp(name, "hotspot_ssid")) return n >= 1 && n <= 32 ? NULL : "must be 1-32 bytes";
  if (!strcmp(name, "vtx_band"))
    return n == 0 || (n == 1 && (is_alpha(v[0]) || is_digit(v[0]))) ? NULL : "must be one letter or digit (a band of the VTX table), or empty";
  char lo[24], hi[24];
  switch (k->kind) {
    case SK_ENUM: {
      if (in_choices(k->choices, v)) return NULL;
      size_t o = (size_t)snprintf(buf, bufsz, "must be one of ");
      for (const char *p = k->choices; *p && o + 3 < bufsz; p++) {
        if (*p == '|') { buf[o++] = ','; buf[o++] = ' '; }
        else buf[o++] = *p;
      }
      buf[o < bufsz ? o : bufsz - 1] = 0;
      return buf;
    }
    case SK_INT: {
      int x, mn, mx;
      range_text(k->choices, lo, hi, sizeof lo);
      if (parse_int(v, &x) && parse_int(lo, &mn) && parse_int(hi, &mx) && x >= mn && x <= mx) return NULL;
      snprintf(buf, bufsz, "must be a whole number from %s to %s", lo, hi);
      return buf;
    }
    case SK_NUM: {
      double x, mn, mx;
      range_text(k->choices, lo, hi, sizeof lo);
      if (parse_num(v, &x) && parse_num(lo, &mn) && parse_num(hi, &mx) && x >= mn && x <= mx) return NULL;
      snprintf(buf, bufsz, "must be a number from %s to %s", lo, hi);
      return buf;
    }
    case SK_BOOL:
      return parse_bool(v) >= 0 ? NULL : "must be on or off";
    default:
      return NULL;
  }
}

const char *settings_validate(const char *key, const char *value) {
  static __thread char buf[256];
  if (!value) return NULL;
  if (strpbrk(value, "\r\n")) return "must not contain line breaks";
  const settings_key_t *k = settings_key(key);
  return k ? check((int)(k - SETTINGS_KEYS), value, buf, sizeof buf) : NULL;
}

/* the stored form of a valid value of a known key */
static char *normalize(int ki, const char *v) {
  const settings_key_t *k = &SETTINGS_KEYS[ki];
  if (k->kind == SK_BOOL) return dup(parse_bool(v) ? "on" : "off");
  if (k->kind == SK_INT) {
    int x = 0;
    char buf[16];
    parse_int(v, &x);
    snprintf(buf, sizeof buf, "%d", x);
    return dup(buf);
  }
  char *d = dup(v);
  if (!d) return NULL;
  if (k->kind == SK_ENUM || !strcmp(k->key, "hdmi_mode") || !strcmp(k->key, "latency"))
    for (char *p = d; *p; p++) *p = lower(*p);
  else if (!strcmp(k->key, "wifi_country") || !strcmp(k->key, "channel_order") || !strcmp(k->key, "vtx_band"))
    for (char *p = d; *p; p++) *p = upper(*p);
  return d;
}

/* ---------------------------------------------------------------- lines */
static void line_clear(line_t *l) {
  free(l->key);
  free(l->value);
  free(l->eff);
  l->key = l->value = l->eff = NULL;
  l->ki = -1;
  l->comment = false;
}

static void line_free(line_t *l) {
  line_clear(l);
  free(l->text);
  l->text = NULL;
}

/* classify l->text and take it apart; false only on OOM (the line is then L_BAD) */
static bool parse_kv(line_t *l, size_t i, size_t e);

static bool parse_line(line_t *l) {
  line_clear(l);
  l->kind = L_BAD;
  l->flags = utf8_ok(l->text, l->len) ? 0 : W_UTF8;
  const char *t = l->text;
  size_t n = l->len, i = 0;
  while (i < n && is_ws(t[i])) i++;
  if (i == n) { l->kind = L_BLANK; return true; }
  if (t[i] == '#') { l->kind = L_COMMENT; return true; }
  const char *eq = memchr(t, '=', n);
  if (!eq) { l->flags |= W_NOEQ; return true; }
  if (parse_kv(l, i, (size_t)(eq - t))) return true;
  bool oom = l->kind == L_KV;
  line_clear(l);
  l->kind = L_BAD;
  return !oom;
}

/* "key = value" with the key from i and the '=' at e; false for a bad key (or OOM, kind L_KV) */
static bool parse_kv(line_t *l, size_t i, size_t e) {
  const char *t = l->text;
  size_t n = l->len, ke = e;
  while (ke > i && is_ws(t[ke - 1])) ke--;
  char key[KEY_MAX + 1];
  if (!make_key(t + i, ke - i, key)) { l->flags |= W_BADKEY; return false; }
  l->kind = L_KV;
  if (!(l->key = dup(key))) return false;
  l->pre_len = e + 1;

  size_t vs = e + 1;
  while (vs < n && is_ws(t[vs])) vs++;
  if (vs < n && t[vs] == '"') {
    char *buf = malloc(n - vs + 1);
    if (!buf) return false;
    size_t bl = 0, j = vs + 1;
    bool closed = false;
    while (j < n) {
      if (t[j] == '\\' && j + 1 < n && (t[j + 1] == '"' || t[j + 1] == '\\')) { buf[bl++] = t[j + 1]; j += 2; }
      else if (t[j] == '"') { closed = true; break; }
      else buf[bl++] = t[j++];
    }
    if (closed) {
      size_t k = j + 1;
      while (k < n && is_ws(t[k])) k++;
      if (k == n || t[k] == '#') {
        buf[bl] = 0;
        l->value = buf;
        l->val_off = vs;
        l->val_len = j + 1 - vs;
        l->post_off = j + 1;
        l->comment = k < n;
        return true;
      }
      l->flags |= W_AFTERQ;
    } else {
      l->flags |= W_UNTERM;
    }
    free(buf);
  }
  /* unquoted: a '#' after a space or tab starts the comment */
  size_t c = n;
  for (size_t j = e + 1; j < n; j++) if (t[j] == '#' && is_ws(t[j - 1])) { c = j; break; }
  size_t ve = c;
  while (ve > vs && is_ws(t[ve - 1])) ve--;
  if (ve <= vs) vs = ve = e + 1;          /* empty: everything after the '=' is the tail */
  l->val_off = vs;
  l->val_len = ve - vs;
  l->post_off = ve;
  l->comment = c < n;
  return (l->value = dupn(t + vs, ve - vs)) != NULL;
}

static bool needs_quotes(const char *v) {
  size_t n = strlen(v);
  if (!n) return false;
  if (is_ws(v[0]) || v[0] == '"' || v[0] == '#' || is_ws(v[n - 1])) return true;
  for (size_t i = 1; i < n; i++) if (v[i] == '#' && is_ws(v[i - 1])) return true;
  return false;
}

/* the value as it is written in the file */
static char *encode(const char *v) {
  if (!needs_quotes(v)) return dup(v);
  size_t n = strlen(v), o = 0;
  char *d = malloc(n * 2 + 3);
  if (!d) return NULL;
  d[o++] = '"';
  for (size_t i = 0; i < n; i++) {
    if (v[i] == '"' || v[i] == '\\') d[o++] = '\\';
    d[o++] = v[i];
  }
  d[o++] = '"';
  d[o] = 0;
  return d;
}

/* new value into an existing line, keeping the key as written, the spacing and the comment */
static bool rewrite(line_t *l, const char *v) {
  char *enc = encode(v);
  if (!enc) return false;
  size_t en = strlen(enc);
  size_t ws1 = l->val_len && en ? l->val_off - l->pre_len : 0;
  bool add_sp = en && !l->val_len;
  const char *post = l->comment ? l->text + l->post_off : "";
  size_t pn = l->comment ? l->len - l->post_off : 0;
  bool post_sp = pn && !is_ws(post[0]);
  size_t len = l->pre_len + (add_sp ? 1 : ws1) + en + (post_sp ? 1 : 0) + pn;
  char *t = malloc(len + 1);
  if (!t) { free(enc); return false; }
  size_t o = 0;
  memcpy(t, l->text, l->pre_len);
  o = l->pre_len;
  if (add_sp) t[o++] = ' ';
  else { memcpy(t + o, l->text + l->pre_len, ws1); o += ws1; }
  memcpy(t + o, enc, en);
  o += en;
  if (post_sp) t[o++] = ' ';
  memcpy(t + o, post, pn);
  o += pn;
  t[o] = 0;
  free(enc);
  free(l->text);
  l->text = t;
  l->len = o;
  return parse_line(l);
}

static bool add_line(settings_t *s, const char *text, size_t len, const char *term) {
  if (s->n == s->cap) {
    int cap = s->cap ? s->cap * 2 : 64;
    line_t *nl = realloc(s->lines, sizeof *nl * (size_t)cap);
    if (!nl) return false;
    s->lines = nl;
    s->cap = cap;
  }
  line_t *l = &s->lines[s->n];
  memset(l, 0, sizeof *l);
  l->ki = -1;
  plat_strlcpy(l->term, term, sizeof l->term);
  if (!(l->text = dupn(text, len))) return false;
  l->len = len;
  s->n++;
  if (!parse_line(l)) { line_free(l); s->n--; return false; }
  return true;
}

/* ---------------------------------------------------------------- effective values, warnings */
static const char *known_get(const settings_t *s, int ki) {
  int li = s->last[ki];
  const char *def = SETTINGS_KEYS[ki].def;
  if (li < 0 || (s->hp_default && !strcmp(SETTINGS_KEYS[ki].key, "hotspot_password"))) return def;
  return s->lines[li].eff ? s->lines[li].eff : def;
}

typedef struct { char *p; size_t n, cap; bool oom; } sbuf_t;

static void sb_add(sbuf_t *b, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
  ;
static void sb_add(sbuf_t *b, const char *fmt, ...) {
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (len < 0 || b->oom) return;
  size_t l = (size_t)len < sizeof line ? (size_t)len : sizeof line - 1;
  if (b->n + l + 2 > b->cap) {
    size_t cap = (b->cap ? b->cap * 2 : 256) + l;
    char *np = realloc(b->p, cap);
    if (!np) { b->oom = true; return; }
    b->p = np;
    b->cap = cap;
  }
  memcpy(b->p + b->n, line, l);
  b->n += l;
  b->p[b->n++] = '\n';
  b->p[b->n] = 0;
}

/* recompute the effective values and the warnings after a parse or an edit */
static void refresh(settings_t *s) {
  char buf[256];
  for (int i = 0; i < SETTINGS_NKEYS; i++) s->last[i] = -1;
  for (int i = 0; i < s->n; i++) {
    line_t *l = &s->lines[i];
    free(l->eff);
    l->eff = NULL;
    l->ki = -1;
    if (l->kind != L_KV || (l->ki = key_index(l->key)) < 0) continue;
    s->last[l->ki] = i;
    if (!check(l->ki, l->value, buf, sizeof buf)) l->eff = normalize(l->ki, l->value);
  }
  int hp = key_index("hotspot_password"), hs = key_index("hotspot");
  s->hp_default = false;
  if (hp >= 0 && hs >= 0 && s->last[hp] >= 0 && !s->lines[s->last[hp]].value[0] && strcmp(known_get(s, hs), "off"))
    s->hp_default = true;

  sbuf_t b = { 0 };
  if (s->note) sb_add(&b, "%s", s->note);
  if (s->had_nul) sb_add(&b, "NUL bytes removed");
  for (int i = 0; i < s->n; i++) {
    const line_t *l = &s->lines[i];
    if (l->flags & W_NOEQ) sb_add(&b, "line %d: no '='", i + 1);
    if (l->flags & W_BADKEY) sb_add(&b, "line %d: bad key", i + 1);
    if (l->flags & W_UNTERM) sb_add(&b, "line %d: unterminated quote", i + 1);
    if (l->flags & W_AFTERQ) sb_add(&b, "line %d: text after the closing quote", i + 1);
    if (l->flags & W_UTF8) sb_add(&b, "line %d: not UTF-8", i + 1);
    if (l->kind != L_KV || l->ki < 0 || s->last[l->ki] != i) continue;
    const char *msg = l->value[0] ? check(l->ki, l->value, buf, sizeof buf) : NULL;
    if (msg) sb_add(&b, "line %d: %s %s (using the default)", i + 1, l->key, msg);
    else if (l->ki == hp && s->hp_default)
      sb_add(&b, "line %d: hotspot_password is empty but hotspot is not off (using the default)", i + 1);
  }
  free(s->warn);
  s->warn = b.p ? b.p : dup("");
}

/* ---------------------------------------------------------------- API */
settings_t *settings_new(void) {
  settings_t *s = calloc(1, sizeof *s);
  if (!s) return NULL;
  s->last = malloc(sizeof(int) * (size_t)SETTINGS_NKEYS);
  if (!s->last) { free(s); return NULL; }
  strcpy(s->nl, "\n");
  refresh(s);
  if (!s->warn) { settings_free(s); return NULL; }
  return s;
}

settings_t *settings_parse(const char *text, size_t len) {
  settings_t *s = settings_new();
  if (!s) return NULL;
  if (!text) len = 0;
  char *buf = malloc(len + 1);
  if (!buf) { settings_free(s); return NULL; }
  size_t n = 0;
  for (size_t i = 0; i < len; i++) {
    if (text[i]) buf[n++] = text[i];
    else s->had_nul = true;
  }
  size_t p = 0;
  bool nl_known = false;
  if (n >= 3 && !memcmp(buf, BOM, 3)) { s->bom = true; p = 3; }
  while (p < n) {
    size_t e = p, next;
    while (e < n && buf[e] != '\n' && buf[e] != '\r') e++;
    const char *term = "";
    if (e == n) next = n;
    else if (buf[e] == '\r' && e + 1 < n && buf[e + 1] == '\n') { term = "\r\n"; next = e + 2; }
    else { term = buf[e] == '\r' ? "\r" : "\n"; next = e + 1; }
    if (*term && !nl_known) { strcpy(s->nl, term); nl_known = true; }
    if (!add_line(s, buf + p, e - p, term)) { free(buf); settings_free(s); return NULL; }
    p = next;
  }
  free(buf);
  refresh(s);
  return s;
}

settings_t *settings_load(const char *path, bool *exists) {
  plat_stat_t st;
  bool ex = plat_stat(path, &st) != 0 || st.exists;    /* a stat error: something is there */
  size_t len = 0;
  char *text = ex ? plat_read_file(path, &len, FILE_MAX) : NULL;
  int err = errno;
  if (ex && !text && err == ENOENT) ex = false;         /* removed in between */
  if (exists) *exists = ex;
  if (text) {
    settings_t *s = settings_parse(text, len);
    free(text);
    return s;
  }
  settings_t *s = settings_parse(SETTINGS_TEMPLATE, strlen(SETTINGS_TEMPLATE));
  if (s && ex) {
    char note[700];
    snprintf(note, sizeof note, "cannot read %s: %s (using the defaults)", path, strerror(err));
    s->note = dup(note);
    refresh(s);
  }
  return s;
}

void settings_free(settings_t *s) {
  if (!s) return;
  for (int i = 0; i < s->n; i++) line_free(&s->lines[i]);
  free(s->lines);
  free(s->last);
  free(s->note);
  free(s->warn);
  free(s);
}

static int find_last(const settings_t *s, const char *key) {
  for (int i = s->n - 1; i >= 0; i--)
    if (s->lines[i].kind == L_KV && !strcmp(s->lines[i].key, key)) return i;
  return -1;
}

/* lookups take any case; keys longer than KEY_MAX never match */
static bool lookup_key(const char *key, char *out) {
  size_t n = key ? strlen(key) : 0;
  if (n == 0 || n > KEY_MAX) return false;
  for (size_t i = 0; i < n; i++) out[i] = lower(key[i]);
  out[n] = 0;
  return true;
}

const char *settings_raw(const settings_t *s, const char *key) {
  char k[KEY_MAX + 1];
  if (!s || !lookup_key(key, k)) return NULL;
  int i = find_last(s, k);
  return i < 0 ? NULL : s->lines[i].value;
}

const char *settings_get(const settings_t *s, const char *key) {
  char k[KEY_MAX + 1];
  if (!lookup_key(key, k)) return "";
  int ki = key_index(k);
  if (ki >= 0) return s ? known_get(s, ki) : SETTINGS_KEYS[ki].def;
  const char *v = settings_raw(s, k);
  return v ? v : "";
}

int settings_get_int(const settings_t *s, const char *key, int def, int min, int max) {
  int x;
  if (!parse_int(settings_get(s, key), &x)) return def;
  return x < min ? min : x > max ? max : x;
}

double settings_get_num(const settings_t *s, const char *key, double def, double min, double max) {
  double x;
  if (!parse_num(settings_get(s, key), &x) || x != x) return def;
  return x < min ? min : x > max ? max : x;
}

bool settings_get_bool(const settings_t *s, const char *key, bool def) {
  int b = parse_bool(settings_get(s, key));
  return b < 0 ? def : b == 1;
}

bool settings_set(settings_t *s, const char *key, const char *value) {
  char k[KEY_MAX + 1];
  size_t a = 0, b = key ? strlen(key) : 0;
  while (a < b && is_ws(key[a])) a++;
  while (b > a && is_ws(key[b - 1])) b--;
  if (!s || !key || !make_key(key + a, b - a, k)) return false;
  if (!value) {
    int o = 0;
    for (int i = 0; i < s->n; i++) {
      if (s->lines[i].kind == L_KV && !strcmp(s->lines[i].key, k)) line_free(&s->lines[i]);
      else s->lines[o++] = s->lines[i];
    }
    s->n = o;
    refresh(s);
    return true;
  }
  if (strpbrk(value, "\r\n")) return false;
  int ki = key_index(k);
  char buf[256];
  char *v = ki >= 0 && !check(ki, value, buf, sizeof buf) ? normalize(ki, value) : dup(value);
  if (!v) return false;
  bool ok = true;
  int i = find_last(s, k);
  if (i >= 0) {
    if (strcmp(s->lines[i].value, v)) ok = rewrite(&s->lines[i], v);
  } else {
    char *enc = encode(v);
    size_t len = enc ? strlen(k) + 3 + strlen(enc) : 0;
    char *t = enc ? malloc(len + 1) : NULL;
    if (t) {
      snprintf(t, len + 1, *enc ? "%s = %s" : "%s =", k, enc);
      if (s->n && !s->lines[s->n - 1].term[0]) strcpy(s->lines[s->n - 1].term, s->nl);
      ok = add_line(s, t, strlen(t), s->nl);
    } else {
      ok = false;
    }
    free(t);
    free(enc);
  }
  free(v);
  refresh(s);
  return ok;
}

char *settings_serialize(const settings_t *s, size_t *len) {
  if (!s) { errno = EINVAL; return NULL; }
  size_t total = s->bom ? 3 : 0;
  for (int i = 0; i < s->n; i++) total += s->lines[i].len + strlen(s->lines[i].term);
  char *out = malloc(total + 1);
  if (!out) return NULL;
  size_t o = 0;
  if (s->bom) { memcpy(out, BOM, 3); o = 3; }
  for (int i = 0; i < s->n; i++) {
    size_t tl = strlen(s->lines[i].term);
    memcpy(out + o, s->lines[i].text, s->lines[i].len);
    o += s->lines[i].len;
    memcpy(out + o, s->lines[i].term, tl);
    o += tl;
  }
  out[o] = 0;
  if (len) *len = o;
  return out;
}

int settings_save(const settings_t *s, const char *path) {
  size_t n;
  if (!s || !path) { errno = EINVAL; return -1; }
  char *text = settings_serialize(s, &n);
  if (!text) { errno = ENOMEM; return -1; }
  int r = plat_write_file_atomic(path, text, n);
  int e = errno;
  free(text);
  errno = e;
  return r == 0 ? 0 : -1;
}

typedef struct { const char *key; int line; } kref_t;

static int kref_cmp(const void *a, const void *b) {
  const kref_t *x = a, *y = b;
  int c = strcmp(x->key, y->key);
  return c ? c : (x->line > y->line) - (x->line < y->line);
}

void settings_each(const settings_t *s, settings_cb cb, void *ud) {
  if (!s || !cb) return;
  int m = 0;
  for (int i = 0; i < s->n; i++) m += s->lines[i].kind == L_KV;
  if (!m) return;
  /* group the lines by key (a sort: a big pasted file must not take quadratic time); first[i] is
   * the key's last line when line i is its first one, else -1 */
  kref_t *r = malloc(sizeof *r * (size_t)m);
  int *first = malloc(sizeof *first * (size_t)s->n);
  if (!r || !first) {
    free(r);
    free(first);
    for (int i = 0; i < s->n; i++) {               /* out of memory: the slow way */
      const line_t *l = &s->lines[i];
      if (l->kind != L_KV) continue;
      bool is_first = true;
      for (int j = 0; j < i && is_first; j++) is_first = !(s->lines[j].kind == L_KV && !strcmp(s->lines[j].key, l->key));
      if (is_first) cb(ud, l->key, s->lines[find_last(s, l->key)].value);
    }
    return;
  }
  for (int i = 0, k = 0; i < s->n; i++) {
    first[i] = -1;
    if (s->lines[i].kind == L_KV) r[k++] = (kref_t){ s->lines[i].key, i };
  }
  qsort(r, (size_t)m, sizeof *r, kref_cmp);
  for (int a = 0, b; a < m; a = b + 1) {
    for (b = a; b + 1 < m && !strcmp(r[b + 1].key, r[a].key); b++) {}
    first[r[a].line] = r[b].line;
  }
  for (int i = 0; i < s->n; i++)
    if (first[i] >= 0) cb(ud, s->lines[i].key, s->lines[first[i]].value);
  free(r);
  free(first);
}

const char *settings_warnings(const settings_t *s) {
  return s && s->warn ? s->warn : "";
}
