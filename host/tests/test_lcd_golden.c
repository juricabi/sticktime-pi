/* Golden test: replays the call files in host/tests/golden/calls through lcd.c with the stub font, the way the
 * Lua API would call it, and compares every frame byte for byte with what engine.js drew
 * (build/golden/, written by host/tests/golden/ref.js; see host/tests/golden/README.md).
 * Also checks sizeText results, and that drawNumber / drawTimer print the strings engine.js printed
 * (rendered with Roboto: the same string gives the same pixels). Skips when there are no references. */
#include "t.h"
#include "lcd.h"

#include <stdint.h>

#define GOLD "build/golden"
#define CALLS "host/tests/golden/calls"
#define MAXARGS 12

typedef struct {
  int n;
  bool is_str[MAXARGS];
  int64_t v[MAXARGS];
  char *s[MAXARGS];
  int slen[MAXARGS];
} args_t;

static char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  char *buf = NULL;
  long n = -1;
  if (fseek(f, 0, SEEK_END) == 0) n = ftell(f);
  if (n >= 0 && fseek(f, 0, SEEK_SET) == 0 && (buf = malloc((size_t)n + 1)) != NULL) {
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    else buf[n] = 0;
  }
  fclose(f);
  if (buf && len) *len = (size_t)n;
  return buf;
}

static uint32_t fnv1a(const unsigned char *p, size_t n) {
  uint32_t h = 0x811c9dc5u;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
  return h;
}

static int hexval(int c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* the format ref.js reads; strings are decoded in place into the line. Returns the name, NULL for
 * blank / comment lines, "" on a syntax error (message in err). */
static const char *parse_line(char *line, args_t *a, char *err, size_t errlen) {
  char *p = line, *name = NULL;
  a->n = 0;
  for (;;) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) break;
    if (*p == '#' && !name) return NULL;
    if (*p == '"') {
      if (!name) { snprintf(err, errlen, "a call starts with a name"); return ""; }
      if (a->n >= MAXARGS) { snprintf(err, errlen, "too many arguments"); return ""; }
      char *out = ++p, *start = out;
      for (;;) {
        if (!*p) { snprintf(err, errlen, "unterminated string"); return ""; }
        if (*p == '"') { p++; break; }
        if (*p != '\\') { *out++ = *p++; continue; }
        p++;
        if (*p == 'x' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) { *out++ = (char)(hexval(p[1]) * 16 + hexval(p[2])); p += 3; }
        else if (*p == 'n') { *out++ = '\n'; p++; }
        else if (*p == 'r') { *out++ = '\r'; p++; }
        else if (*p == 't') { *out++ = '\t'; p++; }
        else if (*p == '\\' || *p == '"') *out++ = *p++;
        else { snprintf(err, errlen, "bad escape"); return ""; }
      }
      a->is_str[a->n] = true;
      a->s[a->n] = start;
      a->slen[a->n] = (int)(out - start);
      a->n++;
      continue;
    }
    char *tok = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = 0;
    if (!name) {
      name = tok;
      if (!strcmp(name, "adv")) return name;   /* its float is read by main() */
      continue;
    }
    if (a->n >= MAXARGS) { snprintf(err, errlen, "too many arguments"); return ""; }
    bool neg = *tok == '-';
    const char *d = tok + neg;
    char *end;
    unsigned long long u = (d[0] == '0' && d[1] == 'x') ? strtoull(d + 2, &end, 16) : strtoull(d, &end, 10);
    if (end == d || *end || (d[0] == '0' && d[1] == 'x' && end == d + 2)) { snprintf(err, errlen, "bad number %s", tok); return ""; }
    int64_t v = neg ? -(int64_t)u : (int64_t)u;
    if (u > 0xFFFFFFFFull || v < -2147483648LL) { snprintf(err, errlen, "%s does not fit 32 bits", tok); return ""; }
    a->is_str[a->n] = false;
    a->v[a->n] = v;
    a->n++;
  }
  return name;
}

static int ai(const args_t *a, int i) { return (int)(int32_t)(uint32_t)a->v[i]; }
static uint32_t au(const args_t *a, int i) { return (uint32_t)a->v[i]; }
#define OPT(i, d) (a->n > (i) ? ai(a, i) : (d))
#define OPTU(i, d) (a->n > (i) ? au(a, i) : (uint32_t)(d))

static void hex(const char *s, int len, char *out) {
  if (!len) { strcpy(out, "-"); return; }
  for (int i = 0; i < len; i++) sprintf(out + 2 * i, "%02x", (unsigned char)s[i]);
}

/* strings: does lcd draw what engine.js measured? (Roboto pixels of both, compared) */
static fonts_t *real_fonts;
static lcd_t ra, rb;
static int str_checks, str_bad;

static void check_string(const char *call, const args_t *a, const char *want_hex, const char *where) {
  char want[512];
  int n = 0;
  if (strcmp(want_hex, "-")) {
    for (; want_hex[2 * n] && want_hex[2 * n + 1] && n < (int)sizeof want; n++)
      want[n] = (char)(hexval(want_hex[2 * n]) * 16 + hexval(want_hex[2 * n + 1]));
  }
  uint32_t fl = OPTU(3, 0) | (12u << 16);   /* any visible color; size and alignment as given */
  fl &= ~(uint32_t)LCDF_RGB;
  lcd_clear(&ra, 0);
  lcd_clear(&rb, 0);
  if (!strcmp(call, "drawNumber")) lcd_draw_number(&ra, 320, 200, ai(a, 2), fl);
  else lcd_draw_timer(&ra, 320, 200, ai(a, 2), fl);
  lcd_draw_text(&rb, 320, 200, want, n, fl, NULL, NULL);
  str_checks++;
  if (memcmp(ra.fb, rb.fb, (size_t)ra.w * ra.h * 2)) {
    if (str_bad++ < 10) fprintf(stderr, "%s: %s(%d) does not draw \"%.*s\"\n", where, call, ai(a, 2), n, want);
  }
}

/* replay one call file on one screen; returns the number of failures */
static int replay(const char *name, const char *text, size_t tlen, float adv, int W, int H, const char *cls_name) {
  char path[512], err[256];
  int fails = 0;
  snprintf(path, sizeof path, GOLD "/%s.%dx%d.bin", name, W, H);
  size_t rlen = 0;
  uint16_t *ref = (uint16_t *)read_file(path, &rlen);
  size_t fsize = (size_t)W * H * 2;
  if (!ref || rlen % fsize) {
    fprintf(stderr, "%s: missing or bad reference\n", path);
    free(ref);
    return 1;
  }
  size_t nref = rlen / fsize;
  snprintf(path, sizeof path, GOLD "/%s.%dx%d.txt", name, W, H);
  char *res = read_file(path, NULL), *res_p = res;
  if (!res) { fprintf(stderr, "%s: missing\n", path); free(ref); return 1; }

  fonts_t *f = fonts_open_stub(adv);
  font_class_t cls;
  font_class_for(&cls, W, H, NULL);
  if (strcmp(cls.name, cls_name)) { fprintf(stderr, "class %s for %dx%d, engine.js uses %s\n", cls.name, W, H, cls_name); fails++; }
  lcd_t l;
  lcd_init(&l, W, H, f, &cls);

  char *buf = malloc(tlen + 1);
  memcpy(buf, text, tlen);
  buf[tlen] = 0;
  size_t frame = 0;
  int no = 0;
  char *line = buf;
  bool done = false;
  while (!done) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = 0; else done = true;
    no++;
    size_t ll = strlen(line);
    if (ll && line[ll - 1] == '\r') line[ll - 1] = 0;
    args_t A, *a = &A;
    const char *c = parse_line(line, a, err, sizeof err);
    char where[600];
    snprintf(where, sizeof where, CALLS "/%s.txt:%d (%dx%d)", name, no, W, H);
    if (c && !*c) { fprintf(stderr, "%s: %s\n", where, err); fails++; break; }
    if (!c || !strcmp(c, "adv")) goto next;
    int need = !strcmp(c, "clear") || !strcmp(c, "snap") ? 0 : !strcmp(c, "sizeText") ? 1 : !strcmp(c, "drawPoint") ? 2
             : !strcmp(c, "drawFilledCircle") || !strcmp(c, "drawCircle") || !strcmp(c, "drawText") || !strcmp(c, "drawNumber")
               || !strcmp(c, "drawTimer") ? 3
             : !strcmp(c, "drawFilledRectangle") || !strcmp(c, "drawRectangle") || !strcmp(c, "invertRect") ? 4
             : !strcmp(c, "drawLine") ? 5 : !strcmp(c, "drawFilledTriangle") || !strcmp(c, "drawTriangle") || !strcmp(c, "drawGauge") ? 6
             : !strcmp(c, "drawLineWithClipping") ? 9 : -1;
    if (need < 0) { fprintf(stderr, "%s: unknown call %s\n", where, c); fails++; break; }
    if (a->n < need) { fprintf(stderr, "%s: %s needs %d arguments\n", where, c, need); fails++; break; }
    bool want_str = !strcmp(c, "sizeText") || !strcmp(c, "drawText");
    for (int i = 0; i < a->n; i++)
      if (a->is_str[i] != (want_str && i == (strcmp(c, "sizeText") ? 2 : 0))) {
        fprintf(stderr, "%s: argument %d has the wrong type\n", where, i + 1);
        fails++;
        goto out;
      }

    if (!strcmp(c, "snap")) {
      if (frame >= nref) { fprintf(stderr, "%s: more frames than the reference\n", where); fails++; break; }
      if (memcmp(l.fb, ref + frame * (fsize / 2), fsize)) {
        const uint16_t *r = ref + frame * (fsize / 2);
        int nd = 0, first = -1;
        for (int i = 0; i < W * H; i++) if (l.fb[i] != r[i]) { if (first < 0) first = i; nd++; }
        fprintf(stderr, "%s: frame %d differs in %d pixels, first at (%d, %d): 0x%04X, engine.js 0x%04X\n", where,
                (int)frame, nd, first % W, first / W, l.fb[first], r[first]);
        snprintf(path, sizeof path, GOLD "/%s.%dx%d.got%d.bin", name, W, H, (int)frame);
        FILE *o = fopen(path, "wb");
        if (o) { fwrite(l.fb, 1, fsize, o); fclose(o); }
        fails++;
      }
      frame++;
    } else if (!strcmp(c, "clear")) {
      lcd_clear(&l, lcd_color_of(a->n ? au(a, 0) : 5u << 16));
    } else if (!strcmp(c, "drawPoint")) {
      lcd_point(&l, ai(a, 0), ai(a, 1), lcd_color_of(OPTU(2, 0)));
    } else if (!strcmp(c, "drawLine")) {
      lcd_draw_line_api(&l, ai(a, 0), ai(a, 1), ai(a, 2), ai(a, 3), ai(a, 4), OPTU(5, 0));
    } else if (!strcmp(c, "drawLineWithClipping")) {
      lcd_line(&l, ai(a, 0), ai(a, 1), ai(a, 2), ai(a, 3), (uint8_t)(ai(a, 8) & 255), lcd_color_of(OPTU(9, 0)));
    } else if (!strcmp(c, "drawFilledRectangle")) {
      lcd_fill_rect(&l, ai(a, 0), ai(a, 1), ai(a, 2), ai(a, 3), lcd_color_of(OPTU(4, 0)), OPT(5, 0) & 15);
    } else if (!strcmp(c, "drawRectangle")) {
      lcd_draw_rectangle(&l, ai(a, 0), ai(a, 1), ai(a, 2), ai(a, 3), OPTU(4, 0), OPT(5, 1), OPT(6, 0) & 15);
    } else if (!strcmp(c, "invertRect")) {
      lcd_invert_rect(&l, ai(a, 0), ai(a, 1), ai(a, 2), ai(a, 3));
    } else if (!strcmp(c, "drawFilledTriangle")) {
      lcd_tri(&l, ai(a, 0), ai(a, 1), ai(a, 2), ai(a, 3), ai(a, 4), ai(a, 5), lcd_color_of(OPTU(6, 0)), OPT(7, 0) & 15);
    } else if (!strcmp(c, "drawTriangle")) {
      uint16_t col = lcd_color_of(OPTU(6, 0));
      lcd_line(&l, ai(a, 0), ai(a, 1), ai(a, 2), ai(a, 3), 255, col);
      lcd_line(&l, ai(a, 2), ai(a, 3), ai(a, 4), ai(a, 5), 255, col);
      lcd_line(&l, ai(a, 4), ai(a, 5), ai(a, 0), ai(a, 1), 255, col);
    } else if (!strcmp(c, "drawFilledCircle")) {
      lcd_filled_circle(&l, ai(a, 0), ai(a, 1), ai(a, 2), lcd_color_of(OPTU(3, 0)));
    } else if (!strcmp(c, "drawCircle")) {
      lcd_circle(&l, ai(a, 0), ai(a, 1), ai(a, 2), lcd_color_of(OPTU(3, 0)));
    } else if (!strcmp(c, "drawGauge")) {
      lcd_draw_gauge(&l, ai(a, 0), ai(a, 1), ai(a, 2), ai(a, 3), ai(a, 4), ai(a, 5), OPTU(6, 0));
    } else {
      /* calls with a result line: sizeText "w h", the others the hex of the string drawn */
      char got[1100] = "", exp_line[1200] = "";
      char *e = res_p ? strchr(res_p, '\n') : NULL;
      if (res_p && e) {
        size_t n = (size_t)(e - res_p) < sizeof exp_line - 1 ? (size_t)(e - res_p) : sizeof exp_line - 1;
        memcpy(exp_line, res_p, n);
        exp_line[n] = 0;
        res_p = e + 1;
      }
      char prefix[64];
      snprintf(prefix, sizeof prefix, "L%d %s ", no, c);
      const char *want = strncmp(exp_line, prefix, strlen(prefix)) ? NULL : exp_line + strlen(prefix);
      if (!want) { fprintf(stderr, "%s: no result line in the reference (got \"%s\")\n", where, exp_line); fails++; break; }
      if (!strcmp(c, "sizeText")) {
        int w, h;
        lcd_size_text(&l, a->s[0], a->slen[0], OPTU(1, 0), &w, &h);
        snprintf(got, sizeof got, "%d %d", w, h);
        if (strcmp(got, want)) { fprintf(stderr, "%s: sizeText %s, engine.js %s\n", where, got, want); fails++; }
      } else if (!strcmp(c, "drawText")) {
        lcd_draw_text(&l, ai(a, 0), ai(a, 1), a->s[2], a->slen[2], OPTU(3, 0), NULL, NULL);
        if (a->slen[2] * 2 < (int)sizeof got) hex(a->s[2], a->slen[2], got);
        if (strcmp(got, want)) { fprintf(stderr, "%s: engine.js drew another string: %s\n", where, want); fails++; }
      } else if (!strcmp(c, "drawNumber")) {
        lcd_draw_number(&l, ai(a, 0), ai(a, 1), ai(a, 2), OPTU(3, 0));
        if (real_fonts && W == 480) check_string(c, a, want, where);
      } else if (!strcmp(c, "drawTimer")) {
        lcd_draw_timer(&l, ai(a, 0), ai(a, 1), ai(a, 2), OPTU(3, 0));
        if (real_fonts && W == 480) check_string(c, a, want, where);
      }
    }
  next:
    if (nl) line = nl + 1;
  }
  if (!fails) {
    if (frame + 1 != nref) { fprintf(stderr, "%s %dx%d: %d frames, engine.js %d\n", name, W, H, (int)frame + 1, (int)nref); fails++; }
    else if (memcmp(l.fb, ref + frame * (fsize / 2), fsize)) {
      const uint16_t *r = ref + frame * (fsize / 2);
      int nd = 0, first = -1;
      for (int i = 0; i < W * H; i++) if (l.fb[i] != r[i]) { if (first < 0) first = i; nd++; }
      fprintf(stderr, "%s %dx%d: last frame differs in %d pixels, first at (%d, %d): 0x%04X, engine.js 0x%04X\n", name, W, H,
              nd, first % W, first / W, l.fb[first], r[first]);
      snprintf(path, sizeof path, GOLD "/%s.%dx%d.got%d.bin", name, W, H, (int)frame);
      FILE *o = fopen(path, "wb");
      if (o) { fwrite(l.fb, 1, fsize, o); fclose(o); }
      fails++;
    }
    if (res_p && *res_p) { fprintf(stderr, "%s %dx%d: the reference has more result lines\n", name, W, H); fails++; }
  }
out:
  free(buf);
  lcd_free(&l);
  fonts_close(f);
  free(ref);
  free(res);
  return fails;
}

int main(void) {
  char *index = read_file(GOLD "/index.txt", NULL);
  if (!index) {
    printf("SKIP: no " GOLD "/index.txt; generate the references with host/tests/golden/ref.js (see host/tests/golden/README.md)\n");
    return 0;
  }
  /* a newer engine.js (the sibling repo, when it is there) may draw differently: say so */
  const char *eh = strstr(index, "(engine.js ");
  size_t elen = 0;
  char *engine = read_file("../sticktime/web/src/engine.js", &elen);
  if (eh && engine && strtoul(eh + 11, NULL, 16) != fnv1a((const unsigned char *)engine, elen))
    printf("note: ../sticktime/web/src/engine.js changed since the references were made; rerun host/tests/golden/ref.js\n");
  free(engine);
  char err[256];
  real_fonts = fonts_open("host/fonts", err, sizeof err);
  CHECK(real_fonts != NULL);
  if (real_fonts) {
    font_class_t lrg;
    font_class_for(&lrg, 640, 480, NULL);
    lcd_init(&ra, 640, 480, real_fonts, &lrg);
    lcd_init(&rb, 640, 480, real_fonts, &lrg);
  }
  int files = 0, frames_ok = 0;
  for (char *line = strtok(index, "\n"); line; line = strtok(NULL, "\n")) {
    if (*line == '#' || !*line) continue;
    char name[128];
    unsigned hash = 0;
    if (sscanf(line, "%127s %x", name, &hash) != 2) continue;
    char path[256];
    snprintf(path, sizeof path, CALLS "/%s.txt", name);
    size_t tlen = 0;
    char *text = read_file(path, &tlen);
    CHECK(text != NULL);
    if (!text) { fprintf(stderr, "%s: missing\n", path); continue; }
    if (fnv1a((const unsigned char *)text, tlen) != hash) {
      fprintf(stderr, "%s changed since the references were made: rerun host/tests/golden/ref.js\n", path);
      CHECK(0);
      free(text);
      continue;
    }
    /* the stub advance: an 'adv' line before the calls (default 7.3, like ref.js) */
    float adv = 7.3f;
    const char *p = text;
    while (*p) {
      while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
      if (*p == '#') { while (*p && *p != '\n') p++; continue; }
      if (!strncmp(p, "adv", 3) && (p[3] == ' ' || p[3] == '\t')) adv = strtof(p + 3, NULL);
      break;
    }
    int f1 = replay(name, text, tlen, adv, 480, 272, "std");
    int f2 = replay(name, text, tlen, adv, 640, 480, "lrg");
    CHECK_INT(f1, 0);
    CHECK_INT(f2, 0);
    frames_ok += !f1 + !f2;
    files++;
    free(text);
  }
  CHECK(files > 0);
  CHECK_INT(str_bad, 0);
  printf("%d call files, %d of %d screen replays identical to engine.js, %d drawNumber/drawTimer strings checked\n",
         files, frames_ok, 2 * files, str_checks);
  if (real_fonts) {
    lcd_free(&ra);
    lcd_free(&rb);
    fonts_close(real_fonts);
  }
  free(index);
  T_DONE();
}
