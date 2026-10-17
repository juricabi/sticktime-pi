/* settings.c: the shared vectors (tests/settings_vectors.txt, also run against settings.py),
 * the template, load/save, and randomized round trips. */
#include "t.h"
#include "settings.h"
#include "plat.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#define VECTORS "host/tests/settings_vectors.txt"
#define TMPDIR "build/test_settings_tmp"

/* ------------------------------------------------------------------ growable bytes */
typedef struct { char *p; size_t n, cap; } buf_t;

static void buf_add(buf_t *b, const char *s, size_t n) {
  if (b->n + n + 1 > b->cap) {
    b->cap = (b->n + n + 1) * 2;
    b->p = realloc(b->p, b->cap);
    if (!b->p) { fprintf(stderr, "out of memory\n"); exit(2); }
  }
  memcpy(b->p + b->n, s, n);
  b->n += n;
  b->p[b->n] = 0;
}
static void buf_str(buf_t *b, const char *s) { buf_add(b, s, strlen(s)); }
static void buf_clear(buf_t *b) { b->n = 0; if (b->p) b->p[0] = 0; }

/* printable form of bytes for failure messages */
static const char *show(const char *s, size_t n) {
  static char out[4][700];
  static int k;
  char *o = out[k++ & 3];
  size_t j = 0;
  for (size_t i = 0; i < n && j < sizeof out[0] - 8; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == '\n') j += (size_t)sprintf(o + j, "\\n");
    else if (c == '\r') j += (size_t)sprintf(o + j, "\\r");
    else if (c == '\t') j += (size_t)sprintf(o + j, "\\t");
    else if (c == '\\') j += (size_t)sprintf(o + j, "\\\\");
    else if (c < 0x20 || c >= 0x7f) j += (size_t)sprintf(o + j, "\\x%02X", c);
    else o[j++] = (char)c;
  }
  if (j >= sizeof out[0] - 8) j += (size_t)sprintf(o + j, "...");
  o[j] = 0;
  return o;
}

/* ------------------------------------------------------------------ the vector runner */
static const char *case_name = "";
static int vline;

#define VFAIL(...) do { t_fail++; fprintf(stderr, "%s:%d [%s] ", VECTORS, vline, case_name); \
  fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define VCHECK(cond, ...) do { t_count++; if (!(cond)) VFAIL(__VA_ARGS__); } while (0)

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* decode the vector escapes into b (appends) */
static void unesc(const char *s, size_t n, buf_t *b) {
  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    if (c != '\\') { buf_add(b, &c, 1); continue; }
    if (++i >= n) { VFAIL("dangling backslash"); return; }
    char o;
    switch (s[i]) {
      case '\\': o = '\\'; break;
      case 'n': o = '\n'; break;
      case 'r': o = '\r'; break;
      case 't': o = '\t'; break;
      case '0': o = 0; break;
      case 's': o = ' '; break;
      case 'x': {
        int h = i + 2 < n ? hexval(s[i + 1]) : -1, l = i + 2 < n ? hexval(s[i + 2]) : -1;
        if (h < 0 || l < 0) { VFAIL("bad \\x escape"); return; }
        o = (char)(h * 16 + l);
        i += 2;
        break;
      }
      default: VFAIL("unknown escape \\%c", s[i]); return;
    }
    buf_add(b, &o, 1);
  }
}

/* "KEY = VALUE" / "KEY =" / "KEY": key and value decoded; returns 0 no value, 1 value, -1 bad */
static int key_value(const char *p, buf_t *key, buf_t *val) {
  buf_clear(key);
  buf_clear(val);
  const char *sp = strchr(p, ' ');
  unesc(p, sp ? (size_t)(sp - p) : strlen(p), key);
  if (!sp) return 0;
  if (!strcmp(sp, " =")) return 1;
  if (strncmp(sp, " = ", 3)) { VFAIL("expected ' = ' in '%s'", p); return -1; }
  unesc(sp + 3, strlen(sp + 3), val);
  return 1;
}

typedef struct { buf_t keys; buf_t vals; int n; } list_t;   /* NUL-separated pairs */

static void each_cb(void *ud, const char *k, const char *v) {
  list_t *l = ud;
  buf_add(&l->keys, k, strlen(k) + 1);
  buf_add(&l->vals, v, strlen(v) + 1);
  l->n++;
}

static void each_list(const settings_t *s, list_t *l) {
  buf_clear(&l->keys);
  buf_clear(&l->vals);
  l->n = 0;
  settings_each(s, each_cb, l);
}

static bool lists_equal(const list_t *a, const list_t *b) {
  return a->n == b->n && a->keys.n == b->keys.n && a->vals.n == b->vals.n &&
         (!a->keys.n || !memcmp(a->keys.p, b->keys.p, a->keys.n)) && (!a->vals.n || !memcmp(a->vals.p, b->vals.p, a->vals.n));
}

typedef struct {
  buf_t in;
  settings_t *s;
  bool parsed, check_parse_warn, modified;
  char block[8];             /* "warn", "out", "each" or "" */
  buf_t expect;              /* warn / out text */
  list_t each_expect;
} vcase_t;

static void close_block(vcase_t *c) {
  if (!strcmp(c->block, "warn")) {
    const char *w = settings_warnings(c->s);
    VCHECK(strlen(w) == c->expect.n && !memcmp(w, c->expect.p, c->expect.n),
           "warnings \"%s\", expected \"%s\"", show(w, strlen(w)), show(c->expect.p, c->expect.n));
  } else if (!strcmp(c->block, "out")) {
    size_t n;
    char *o = settings_serialize(c->s, &n);
    VCHECK(o && n == c->expect.n && !memcmp(o, c->expect.p, n),
           "serialized \"%s\", expected \"%s\"", show(o, n), show(c->expect.p, c->expect.n));
    free(o);
  } else if (!strcmp(c->block, "each")) {
    list_t got = { 0 };
    each_list(c->s, &got);
    VCHECK(lists_equal(&got, &c->each_expect), "settings_each differs (%d entries, expected %d)", got.n, c->each_expect.n);
    free(got.keys.p);
    free(got.vals.p);
  }
  c->block[0] = 0;
  buf_clear(&c->expect);
  buf_clear(&c->each_expect.keys);
  buf_clear(&c->each_expect.vals);
  c->each_expect.n = 0;
}

static void ensure_parsed(vcase_t *c) {
  if (c->parsed) return;
  c->s = settings_parse(c->in.p ? c->in.p : "", c->in.n);
  if (!c->s) { fprintf(stderr, "out of memory\n"); exit(2); }
  c->parsed = true;
  c->check_parse_warn = true;
}

/* the checks every case gets at its end */
static void finish_case(vcase_t *c) {
  ensure_parsed(c);
  if (c->check_parse_warn) {
    const char *w = settings_warnings(c->s);
    VCHECK(!*w, "unexpected warnings \"%s\"", show(w, strlen(w)));
    c->check_parse_warn = false;
  }
  close_block(c);
  size_t n1, n2;
  char *t1 = settings_serialize(c->s, &n1);
  settings_t *s2 = settings_parse(t1, n1);
  char *t2 = settings_serialize(s2, &n2);
  VCHECK(n1 == n2 && !memcmp(t1, t2, n1), "serialize(parse(serialize)) differs");
  list_t a = { 0 }, b = { 0 };
  each_list(c->s, &a);
  each_list(s2, &b);
  VCHECK(lists_equal(&a, &b), "the reparsed settings differ");
  const char *w1 = settings_warnings(c->s), *w2 = settings_warnings(s2);
  if (!strncmp(w1, "NUL bytes removed\n", 18)) w1 += 18;
  VCHECK(!strcmp(w1, w2), "reparsed warnings \"%s\" vs \"%s\"", show(w2, strlen(w2)), show(w1, strlen(w1)));
  for (const settings_key_t *k = SETTINGS_KEYS; k < SETTINGS_KEYS + SETTINGS_NKEYS; k++)
    VCHECK(!strcmp(settings_get(c->s, k->key), settings_get(s2, k->key)), "reparsed %s differs", k->key);
  if (!c->modified) {
    buf_t clean = { 0 };
    for (size_t i = 0; i < c->in.n; i++) if (c->in.p[i]) buf_add(&clean, c->in.p + i, 1);
    VCHECK(n1 == clean.n && (!n1 || !memcmp(t1, clean.p, n1)), "round trip \"%s\", input \"%s\"", show(t1, n1), show(clean.p, clean.n));
    free(clean.p);
  }
  free(a.keys.p); free(a.vals.p); free(b.keys.p); free(b.vals.p);
  free(t1);
  free(t2);
  settings_free(s2);
  settings_free(c->s);
  c->s = NULL;
}

static void run_directive(vcase_t *c, const char *d, const char *p) {
  static buf_t key, val;
  if (!strcmp(d, "in") || !strcmp(d, "inrep")) {
    VCHECK(!c->parsed, "in after the parse");
    if (!strcmp(d, "in")) { unesc(p, strlen(p), &c->in); return; }
    char *end;
    long n = strtol(p, &end, 10);
    buf_clear(&val);
    unesc(end + 1, strlen(end + 1), &val);
    for (long i = 0; i < n; i++) buf_add(&c->in, val.p ? val.p : "", val.n);
    return;
  }
  ensure_parsed(c);
  const char *blk = !strcmp(d, "outrep") ? "out" : d;
  if (c->block[0] && strcmp(c->block, blk)) close_block(c);
  if (c->check_parse_warn) {
    c->check_parse_warn = false;
    if (strcmp(d, "warn")) {
      const char *w = settings_warnings(c->s);
      VCHECK(!*w, "unexpected warnings \"%s\"", show(w, strlen(w)));
    }
  }
  settings_t *s = c->s;
  if (!strcmp(d, "warn")) {
    strcpy(c->block, "warn");
    unesc(p, strlen(p), &c->expect);
    buf_str(&c->expect, "\n");
  } else if (!strcmp(d, "nowarn")) {
    const char *w = settings_warnings(s);
    VCHECK(!*w, "unexpected warnings \"%s\"", show(w, strlen(w)));
  } else if (!strcmp(d, "out")) {
    strcpy(c->block, "out");
    unesc(p, strlen(p), &c->expect);
  } else if (!strcmp(d, "outrep")) {
    strcpy(c->block, "out");
    char *end;
    long n = strtol(p, &end, 10);
    buf_clear(&val);
    unesc(end + 1, strlen(end + 1), &val);
    for (long i = 0; i < n; i++) buf_add(&c->expect, val.p ? val.p : "", val.n);
  } else if (!strcmp(d, "each")) {
    strcpy(c->block, "each");
    if (key_value(p, &key, &val) != 1) { VFAIL("each needs KEY = VALUE"); return; }
    each_cb(&c->each_expect, key.p, val.p ? val.p : "");
  } else if (!strcmp(d, "noeach")) {
    list_t got = { 0 };
    each_list(s, &got);
    VCHECK(got.n == 0, "settings_each gave %d entries", got.n);
    free(got.keys.p);
    free(got.vals.p);
  } else if (!strcmp(d, "raw") || !strcmp(d, "get")) {
    int r = key_value(p, &key, &val);
    if (r < 0) return;
    const char *v = !strcmp(d, "raw") ? settings_raw(s, key.p) : settings_get(s, key.p);
    if (r == 0) VCHECK(!v, "%s(%s) = \"%s\", expected not set", d, key.p, v ? show(v, strlen(v)) : "");
    else VCHECK(v && strlen(v) == val.n && !memcmp(v, val.p ? val.p : "", val.n), "%s(%s) = \"%s\", expected \"%s\"", d,
                show(key.p, key.n), v ? show(v, strlen(v)) : "(null)", show(val.p, val.n));
  } else if (!strcmp(d, "rawlen")) {
    if (key_value(p, &key, &val) != 1) return;
    const char *v = settings_raw(s, key.p);
    VCHECK(v && (long)strlen(v) == atol(val.p), "rawlen(%s) = %ld", key.p, v ? (long)strlen(v) : -1L);
  } else if (!strcmp(d, "int") || !strcmp(d, "num") || !strcmp(d, "bool")) {
    char k[128], def[64], mn[64], mx[64], eq[4], exp[64];
    if (!strcmp(d, "bool")) {
      if (sscanf(p, "%127s %63s %3s %63s", k, def, eq, exp) != 4) { VFAIL("bad bool line"); return; }
      bool b = settings_get_bool(s, k, atoi(def) != 0);
      VCHECK(b == (atoi(exp) != 0), "get_bool(%s, %s) = %d", k, def, b);
    } else if (sscanf(p, "%127s %63s %63s %63s %3s %63s", k, def, mn, mx, eq, exp) != 6) {
      VFAIL("bad %s line", d);
    } else if (!strcmp(d, "int")) {
      int v = settings_get_int(s, k, atoi(def), atoi(mn), atoi(mx));
      VCHECK(v == atoi(exp), "get_int(%s, %s, %s, %s) = %d, expected %s", k, def, mn, mx, v, exp);
    } else {
      double v = settings_get_num(s, k, atof(def), atof(mn), atof(mx)), e = atof(exp);
      VCHECK(v - e < 1e-9 && e - v < 1e-9, "get_num(%s, %s, %s, %s) = %g, expected %s", k, def, mn, mx, v, exp);
    }
  } else if (!strcmp(d, "set") || !strcmp(d, "set!")) {
    if (key_value(p, &key, &val) != 1) { VFAIL("set needs KEY = VALUE"); return; }
    bool want = !strcmp(d, "set");
    bool ok = settings_set(s, key.p, val.p ? val.p : "");
    VCHECK(ok == want, "settings_set(%s, \"%s\") = %d", show(key.p, key.n), show(val.p, val.n), ok);
    c->modified = true;
  } else if (!strcmp(d, "del")) {
    key_value(p, &key, &val);
    VCHECK(settings_set(s, key.p, NULL), "settings_set(%s, NULL) failed", key.p);
    c->modified = true;
  } else if (!strcmp(d, "valid") || !strcmp(d, "invalid")) {
    if (key_value(p, &key, &val) != 1) { VFAIL("%s needs KEY = VALUE", d); return; }
    const char *m = settings_validate(key.p, val.p ? val.p : "");
    if (!strcmp(d, "valid")) VCHECK(!m, "validate(%s, \"%s\"): %s", key.p, show(val.p, val.n), m);
    else VCHECK(m && *m, "validate(%s, \"%s\") passed", key.p, show(val.p, val.n));
  } else {
    VFAIL("unknown directive %s", d);
  }
}

static int run_vectors(void) {
  size_t len;
  char *text = plat_read_file(VECTORS, &len, 1 << 22);
  if (!text) { fprintf(stderr, "can't read %s (run from the repository root)\n", VECTORS); t_fail++; return 0; }
  vcase_t c;
  memset(&c, 0, sizeof c);
  bool in_case = false;
  int ncases = 0;
  char *line = text;
  vline = 0;
  while (line && *line) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0;
    size_t ll = strlen(line);
    if (ll && line[ll - 1] == '\r') line[--ll] = 0;
    vline++;
    if (ll && line[0] != '#') {
      char *colon = strchr(line, ':');
      if (!colon) { VFAIL("no ':' in \"%s\"", line); }
      else {
        *colon = 0;
        char *p = colon + 1;
        if (*p == ' ') p++;
        if (!strcmp(line, "case")) {
          if (in_case) finish_case(&c);
          buf_clear(&c.in);
          c.parsed = c.check_parse_warn = c.modified = false;
          c.block[0] = 0;
          static char name[128];
          plat_strlcpy(name, p, sizeof name);
          case_name = name;
          in_case = true;
          ncases++;
        } else if (in_case) {
          run_directive(&c, line, p);
        } else {
          VFAIL("directive outside a case");
        }
      }
    }
    line = nl ? nl + 1 : NULL;
  }
  if (in_case) finish_case(&c);
  free(c.in.p);
  free(c.expect.p);
  free(c.each_expect.keys.p);
  free(c.each_expect.vals.p);
  free(text);
  return ncases;
}

/* ------------------------------------------------------------------ the rest of the API */
static void test_keys(void) {
  CHECK(SETTINGS_NKEYS >= 38);
  CHECK(settings_key("video") == &SETTINGS_KEYS[0]);
  CHECK(settings_key("VIDEO") == &SETTINGS_KEYS[0]);
  CHECK(settings_key("nope") == NULL);
  CHECK(settings_key("") == NULL);
  CHECK(settings_key(NULL) == NULL);
  for (int i = 0; i < SETTINGS_NKEYS; i++) {
    const settings_key_t *k = &SETTINGS_KEYS[i];
    CHECK(settings_key(k->key) == k);
    CHECK(k->help && *k->help);
    CHECK(!(k->live && k->reboot));
    if (settings_validate(k->key, k->def)) fprintf(stderr, "default of %s: %s\n", k->key, settings_validate(k->key, k->def));
    CHECK(settings_validate(k->key, k->def) == NULL);
    if (k->kind == SK_ENUM) CHECK(strchr(k->choices, '|') != NULL);
    if (k->kind == SK_INT) {
      int mn = 0, mx = 0;
      CHECK(sscanf(k->choices, "%d|%d", &mn, &mx) == 2 && mn <= atoi(k->def) && atoi(k->def) <= mx);
    }
    for (int j = 0; j < i; j++) CHECK(strcmp(SETTINGS_KEYS[j].key, k->key) != 0);
  }
  CHECK(settings_validate("video", NULL) == NULL);
  CHECK(settings_validate("not_a_key", "anything") == NULL);
  CHECK(settings_validate(NULL, "x") == NULL);
  CHECK_STR(settings_validate("tv", "x"), "must be one of ntsc, pal, pal60, ntsc-j, pal-m, pal-n, ntsc-443, secam");
  CHECK_STR(settings_validate("margin", "x"), "must be a whole number from 0 to 20");
}

static void template_key_cb(void *ud, const char *k, const char *v) {
  int *i = ud;
  if (*i < SETTINGS_NKEYS) {
    CHECK_STR(k, SETTINGS_KEYS[*i].key);
    CHECK_STR(v, SETTINGS_KEYS[*i].def);
  }
  (*i)++;
}

static void test_template(void) {
  settings_t *s = settings_parse(SETTINGS_TEMPLATE, strlen(SETTINGS_TEMPLATE));
  CHECK(s != NULL);
  CHECK_STR(settings_warnings(s), "");
  int i = 0;
  settings_each(s, template_key_cb, &i);
  CHECK_INT(i, SETTINGS_NKEYS);          /* every key, once, in table order, with its default */
  size_t n;
  char *t = settings_serialize(s, &n);
  CHECK(n == strlen(SETTINGS_TEMPLATE) && !strcmp(t, SETTINGS_TEMPLATE));
  free(t);
  for (int k = 0; k < SETTINGS_NKEYS; k++) CHECK_STR(settings_get(s, SETTINGS_KEYS[k].key), SETTINGS_KEYS[k].def);
  /* an edit of the template touches one line */
  CHECK(settings_set(s, "video", "hdmi"));
  t = settings_serialize(s, &n);
  const char *a = strstr(SETTINGS_TEMPLATE, "\nvideo = av\n");
  CHECK(a && n == strlen(SETTINGS_TEMPLATE) + 2);
  CHECK(a && !memcmp(t, SETTINGS_TEMPLATE, (size_t)(a - SETTINGS_TEMPLATE)) && !strncmp(t + (a - SETTINGS_TEMPLATE), "\nvideo = hdmi\n", 14));
  CHECK(a && !strcmp(t + (a - SETTINGS_TEMPLATE) + 14, a + 12));
  free(t);
  settings_free(s);
}

static void test_new_and_null(void) {
  settings_t *s = settings_new();
  CHECK_STR(settings_warnings(s), "");
  CHECK_STR(settings_get(s, "video"), "av");
  CHECK(settings_raw(s, "video") == NULL);
  CHECK(settings_set(s, "video", "hdmi"));
  CHECK(!settings_set(s, "", "x"));
  CHECK(!settings_set(s, "   ", "x"));
  CHECK(!settings_set(s, NULL, "x"));
  CHECK(!settings_set(NULL, "video", "x"));
  char long_key[80];
  memset(long_key, 'k', 64);
  long_key[64] = 0;
  CHECK(settings_set(s, long_key, "x"));
  CHECK_STR(settings_raw(s, long_key), "x");
  long_key[64] = 'k';
  long_key[65] = 0;
  CHECK(!settings_set(s, long_key, "x"));
  CHECK(settings_raw(s, long_key) == NULL);
  CHECK_STR(settings_get(s, long_key), "");
  CHECK(settings_raw(s, "") == NULL);
  CHECK(settings_raw(s, NULL) == NULL);
  CHECK_STR(settings_get(s, NULL), "");
  CHECK_STR(settings_get(NULL, "tv"), "ntsc");
  CHECK_STR(settings_get(NULL, "nokey"), "");
  CHECK_INT(settings_get_int(NULL, "volume", 1, 0, 100), 70);
  CHECK(settings_get_bool(NULL, "sound", false));
  CHECK_STR(settings_warnings(NULL), "");
  size_t n;
  char *t = settings_serialize(s, &n);
  memset(long_key, 'k', 64);
  long_key[64] = 0;
  char want[200];
  snprintf(want, sizeof want, "video = hdmi\n%s = x\n", long_key);
  CHECK_STR(t, want);
  free(t);
  settings_free(s);
  settings_free(NULL);
  /* empty input, NULL text (also with a length) */
  s = settings_parse(NULL, 0);
  CHECK(s != NULL);
  t = settings_serialize(s, &n);
  CHECK_INT(n, 0);
  free(t);
  settings_each(s, NULL, NULL);
  settings_free(s);
  s = settings_parse(NULL, 99);
  CHECK(s != NULL);
  CHECK_STR(settings_warnings(s), "");
  settings_free(s);
  /* NULL settings */
  settings_each(NULL, NULL, NULL);
  errno = 0;
  CHECK(settings_serialize(NULL, &n) == NULL);
  CHECK_INT(errno, EINVAL);
  errno = 0;
  CHECK_INT(settings_save(NULL, TMPDIR "/never.txt"), -1);
  CHECK_INT(errno, EINVAL);
}

static int each_count;
static const char *each_last_key, *each_last_val;
static void count_each_cb(void *ud, const char *k, const char *v) {
  each_count++;
  each_last_key = k;
  each_last_val = v;
}

/* a big file (a paste into the web page, a corrupted card): everything stays linear */
static void test_big(void) {
  const int lines = 120000;                   /* ~1 MiB */
  size_t cap = (size_t)lines * 24;
  char *t = malloc(cap), *p = t;
  for (int i = 0; i < lines; i++) p += sprintf(p, "k%d = %d\n", i % 40000, i);
  p += sprintf(p, "video = hdmi\n");
  int64_t t0 = plat_now_us();
  settings_t *s = settings_parse(t, (size_t)(p - t));
  each_count = 0;
  settings_each(s, count_each_cb, NULL);
  int64_t t1 = plat_now_us();
  CHECK_INT(each_count, 40001);
  CHECK_STR(each_last_key, "video");
  CHECK_STR(each_last_val, "hdmi");
  CHECK_STR(settings_raw(s, "k0"), "80000");  /* the last of three */
  CHECK_STR(settings_raw(s, "k39999"), "119999");
  CHECK_STR(settings_get(s, "video"), "hdmi");
  CHECK(settings_set(s, "k1", "x"));
  CHECK_STR(settings_raw(s, "k1"), "x");
  int64_t t2 = plat_now_us();
  if (t1 - t0 > 2000000 || t2 - t1 > 2000000)
    fprintf(stderr, "big file: parse + each %lld ms, set %lld ms\n", (long long)(t1 - t0) / 1000, (long long)(t2 - t1) / 1000);
  CHECK(t1 - t0 < 2000000);                   /* quadratic would be minutes */
  CHECK(t2 - t1 < 2000000);
  settings_free(s);
  free(t);
}

static bool file_exists(const char *p) {
  plat_stat_t st;
  return plat_stat(p, &st) == 0 && st.exists;
}

static int count_cb(void *ud, const char *name, bool is_dir, int64_t size, int64_t mtime) {
  (*(int *)ud)++;
  return 0;
}

static void test_load_save(void) {
  plat_mkdirs(TMPDIR);
  const char *path = TMPDIR "/sticktime.txt";
  plat_remove(path);
  bool exists = true;
  settings_t *s = settings_load(path, &exists);
  CHECK(s != NULL);
  CHECK(!exists);
  CHECK_STR(settings_warnings(s), "");
  CHECK_STR(settings_raw(s, "video"), "av");   /* the template's lines */
  CHECK(settings_set(s, "wifi_ssid", " My \"Net\" # 1 "));
  CHECK(settings_set(s, "wifi_password", "p\xC3\xA4ss \\ #word"));
  CHECK_INT(settings_save(s, path), 0);
  CHECK(file_exists(path));
  int nfiles = 0;
  plat_list_dir(TMPDIR, count_cb, &nfiles);
  CHECK_INT(nfiles, 1);                        /* no temp file left behind */
  size_t n, m;
  char *disk = plat_read_file(path, &n, 1 << 20);
  char *mem = settings_serialize(s, &m);
  CHECK(disk && n == m && !memcmp(disk, mem, n));
  free(disk);
  free(mem);
  settings_free(s);

  s = settings_load(path, &exists);
  CHECK(exists);
  CHECK_STR(settings_raw(s, "wifi_ssid"), " My \"Net\" # 1 ");
  CHECK_STR(settings_raw(s, "wifi_password"), "p\xC3\xA4ss \\ #word");
  CHECK_STR(settings_warnings(s), "");
  settings_free(s);

  /* NULL exists pointer is fine */
  s = settings_load(path, NULL);
  CHECK(s != NULL);
  /* saving where the folder is missing fails with errno set, and leaves nothing */
  errno = 0;
  CHECK_INT(settings_save(s, TMPDIR "/missing/dir/sticktime.txt"), -1);
  CHECK(errno != 0);
  CHECK(!file_exists(TMPDIR "/missing"));
  settings_free(s);

  /* a directory where the file should be: it "exists" but can't be read -> template + a note */
  const char *dirpath = TMPDIR "/adir.txt";
  plat_mkdirs(dirpath);
  s = settings_load(dirpath, &exists);
  CHECK(s != NULL);
  CHECK(exists);
  CHECK(!strncmp(settings_warnings(s), "cannot read " TMPDIR "/adir.txt: ", 12 + strlen(TMPDIR "/adir.txt: ")));
  CHECK_STR(settings_get(s, "video"), "av");
  settings_free(s);
  plat_remove(dirpath);

  /* a file with CRLF, BOM and NULs from a PC */
  const char pc[] = "\xEF\xBB\xBFvideo = hdmi\r\nwifi_ssid = \"Caf\xC3\xA9 #1\"\r\n\0\0";
  CHECK_INT(plat_write_file_atomic(path, pc, sizeof pc - 1), 0);
  s = settings_load(path, &exists);
  CHECK_STR(settings_get(s, "video"), "hdmi");
  CHECK_STR(settings_raw(s, "wifi_ssid"), "Caf\xC3\xA9 #1");
  CHECK_STR(settings_warnings(s), "NUL bytes removed\n");
  CHECK(settings_set(s, "tv", "pal"));
  CHECK_INT(settings_save(s, path), 0);
  disk = plat_read_file(path, &n, 1 << 20);
  CHECK_STR(disk, "\xEF\xBB\xBFvideo = hdmi\r\nwifi_ssid = \"Caf\xC3\xA9 #1\"\r\ntv = pal\r\n");
  free(disk);
  settings_free(s);
  plat_remove(path);
  plat_remove(TMPDIR);
}

/* ------------------------------------------------------------------ randomized */
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static const char ALPHA[] = { ' ', '\t', '#', '"', '\\', '=', 'a', 'b', 'Z', '0', '\r', '\n', 0, (char)0xEF, (char)0xBB,
                              (char)0xBF, (char)0xC3, (char)0xA9, (char)0x80, (char)0xFF, '_', '.', '-', 'x' };

static void test_random_roundtrip(void) {
  /* any bytes: serialize(parse(x)) == x without its NUL bytes */
  char text[200], clean[200];
  int bad = 0;
  for (int it = 0; it < 20000; it++) {
    size_t n = rnd() % sizeof text, m = 0;
    for (size_t i = 0; i < n; i++) {
      text[i] = (rnd() & 3) ? ALPHA[rnd() % sizeof ALPHA] : (char)(rnd() & 0xff);
      if (text[i]) clean[m++] = text[i];
    }
    settings_t *s = settings_parse(text, n);
    size_t on;
    char *o = settings_serialize(s, &on);
    if (on != m || memcmp(o, clean, m)) bad++;
    free(o);
    settings_free(s);
  }
  CHECK_INT(bad, 0);

  /* any value without line breaks survives set -> serialize -> parse, next to random lines */
  static const char *keys[] = { "password", "wifi_ssid", "x", "github_token", "zz.y-1" };
  bad = 0;
  for (int it = 0; it < 20000; it++) {
    size_t n = rnd() % 60, m = 0;
    for (size_t i = 0; i < n; i++) text[i] = ALPHA[rnd() % sizeof ALPHA];
    settings_t *s = settings_parse(text, n);
    char v[40];
    size_t vn = rnd() % sizeof v;
    for (size_t i = 0; i < vn; i++) {
      char ch = (rnd() & 1) ? ALPHA[rnd() % sizeof ALPHA] : (char)(rnd() & 0xff);
      if (ch && ch != '\r' && ch != '\n') v[m++] = ch;
    }
    v[m] = 0;
    const char *k = keys[rnd() % 5];
    if (!settings_set(s, k, v)) { bad++; settings_free(s); continue; }
    size_t on;
    char *o = settings_serialize(s, &on);
    settings_t *s2 = settings_parse(o, on);
    const char *r = settings_raw(s2, k);
    if (!r || strcmp(r, v)) {
      if (bad < 5) fprintf(stderr, "round trip of \"%s\" in \"%s\" gave \"%s\"\n", show(v, m), show(o, on), r ? show(r, strlen(r)) : "(null)");
      bad++;
    }
    /* and a second set to another random value rewrites in place */
    if (settings_set(s2, k, "second")) {
      char *o2 = settings_serialize(s2, &on);
      settings_t *s3 = settings_parse(o2, on);
      if (strcmp(settings_raw(s3, k) ? settings_raw(s3, k) : "", "second")) bad++;
      settings_free(s3);
      free(o2);
    }
    free(o);
    settings_free(s2);
    settings_free(s);
  }
  CHECK_INT(bad, 0);
}

int main(void) {
  int n = run_vectors();
  CHECK(n >= 50);
  printf("%d vector cases\n", n);
  test_keys();
  test_template();
  test_new_and_null();
  test_load_save();
  test_random_roundtrip();
  test_big();
  T_DONE();
}
