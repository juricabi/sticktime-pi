/* Unit tests for the portable parts of the video module: the layout (render size and view
 * rectangle for every mode, aspect and margin), the CPU scaling helpers, the PNG encoder (checked
 * with the small PNG decoder below, not by trusting the encoder), the png and none backends
 * (frame files, pacing by the 60 Hz software clock) and the dispatch. The real display backends
 * are tested on Linux by tests/linux/test_video_drm.c. */
#include "t.h"
#include "video.h"
#include "plat.h"

#include <math.h>
#include <stdint.h>

/* ================================================================ a small PNG decoder
 * Enough for 8-bit RGB / RGBA non-interlaced PNGs with any filter and any deflate block type,
 * with CRC and Adler-32 checks: so a broken encoder can't pass by accident. */

typedef struct {
  const uint8_t *in;
  size_t inlen, inpos;
  uint32_t bitbuf;
  int bitcnt;
  uint8_t *out;
  size_t outlen, outpos;
  int err;
} inf_t;

typedef struct { short count[16]; short symbol[320]; } huff_t;

static int inf_bits(inf_t *s, int need) {
  uint32_t v = s->bitbuf;
  while (s->bitcnt < need) {
    if (s->inpos >= s->inlen) { s->err = 1; return 0; }
    v |= (uint32_t)s->in[s->inpos++] << s->bitcnt;
    s->bitcnt += 8;
  }
  s->bitbuf = need < 32 ? v >> need : 0;
  s->bitcnt -= need;
  return (int)(v & ((1u << need) - 1));
}

static int inf_decode(inf_t *s, const huff_t *h) {
  int code = 0, first = 0, index = 0;
  for (int len = 1; len < 16; len++) {
    code |= inf_bits(s, 1);
    int count = h->count[len];
    if (code - count < first) return h->symbol[index + (code - first)];
    index += count;
    first += count;
    first <<= 1;
    code <<= 1;
  }
  s->err = 1;
  return -1;
}

/* canonical Huffman code from lengths; < 0 over-subscribed, > 0 incomplete, 0 complete */
static int inf_build(huff_t *h, const short *length, int n) {
  memset(h->count, 0, sizeof h->count);
  for (int i = 0; i < n; i++) h->count[length[i]]++;
  if (h->count[0] == n) return 0;
  int left = 1;
  for (int len = 1; len < 16; len++) {
    left <<= 1;
    left -= h->count[len];
    if (left < 0) return left;
  }
  short offs[16];
  offs[1] = 0;
  for (int len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
  for (int i = 0; i < n; i++)
    if (length[i]) h->symbol[offs[length[i]]++] = (short)i;
  return left;
}

static void inf_codes(inf_t *s, const huff_t *lc, const huff_t *dc) {
  static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                   67, 83, 99, 115, 131, 163, 195, 227, 258 };
  static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
  static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                   1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
  static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
  for (;;) {
    int sym = inf_decode(s, lc);
    if (s->err) return;
    if (sym < 256) {
      if (s->outpos >= s->outlen) { s->err = 2; return; }
      s->out[s->outpos++] = (uint8_t)sym;
    } else if (sym == 256) {
      return;
    } else {
      sym -= 257;
      if (sym >= 29) { s->err = 3; return; }
      int len = lbase[sym] + inf_bits(s, lext[sym]);
      int ds = inf_decode(s, dc);
      if (s->err || ds < 0 || ds >= 30) { s->err = 3; return; }
      size_t dist = (size_t)dbase[ds] + (size_t)inf_bits(s, dext[ds]);
      if (dist > s->outpos || s->outpos + (size_t)len > s->outlen) { s->err = 4; return; }
      for (int i = 0; i < len; i++, s->outpos++) s->out[s->outpos] = s->out[s->outpos - dist];
    }
  }
}

/* zlib stream -> out; returns bytes written or -1 */
static long zlib_inflate(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen) {
  if (inlen < 6) return -1;
  if ((in[0] & 15) != 8 || ((in[0] << 8) | in[1]) % 31 || (in[1] & 32)) return -1;
  inf_t s = { .in = in, .inlen = inlen - 4, .inpos = 2, .out = out, .outlen = outlen };
  int last;
  do {
    last = inf_bits(&s, 1);
    int type = inf_bits(&s, 2);
    if (s.err) return -1;
    if (type == 0) {
      s.bitbuf = 0;
      s.bitcnt = 0;
      if (s.inpos + 4 > s.inlen) return -1;
      unsigned len = s.in[s.inpos] | (s.in[s.inpos + 1] << 8), nlen = s.in[s.inpos + 2] | (s.in[s.inpos + 3] << 8);
      s.inpos += 4;
      if ((len ^ 0xffffu) != nlen || s.inpos + len > s.inlen || s.outpos + len > s.outlen) return -1;
      memcpy(s.out + s.outpos, s.in + s.inpos, len);
      s.inpos += len;
      s.outpos += len;
    } else if (type == 1) {
      static huff_t lc, dc;
      static int built;
      if (!built) {
        short l[288];
        int i = 0;
        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        inf_build(&lc, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        inf_build(&dc, l, 30);
        built = 1;
      }
      inf_codes(&s, &lc, &dc);
    } else if (type == 2) {
      static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
      int nlen = inf_bits(&s, 5) + 257, ndist = inf_bits(&s, 5) + 1, ncode = inf_bits(&s, 4) + 4;
      if (s.err || nlen > 286 || ndist > 30) return -1;
      short lengths[320];
      int i;
      for (i = 0; i < ncode; i++) lengths[order[i]] = (short)inf_bits(&s, 3);
      for (; i < 19; i++) lengths[order[i]] = 0;
      huff_t lc, dc;
      if (inf_build(&lc, lengths, 19) != 0) return -1;
      for (i = 0; i < nlen + ndist;) {
        int sym = inf_decode(&s, &lc);
        if (s.err || sym < 0) return -1;
        if (sym < 16) { lengths[i++] = (short)sym; continue; }
        short v = 0;
        int rep;
        if (sym == 16) {
          if (!i) return -1;
          v = lengths[i - 1];
          rep = 3 + inf_bits(&s, 2);
        } else if (sym == 17) {
          rep = 3 + inf_bits(&s, 3);
        } else {
          rep = 11 + inf_bits(&s, 7);
        }
        if (i + rep > nlen + ndist) return -1;
        while (rep--) lengths[i++] = v;
      }
      if (lengths[256] == 0) return -1;
      int el = inf_build(&lc, lengths, nlen);
      if (el < 0 || (el > 0 && nlen - lc.count[0] != 1)) return -1;
      int ed = inf_build(&dc, lengths + nlen, ndist);
      if (ed < 0 || (ed > 0 && ndist - dc.count[0] != 1)) return -1;
      inf_codes(&s, &lc, &dc);
    } else {
      return -1;
    }
    if (s.err) return -1;
  } while (!last);
  /* Adler-32 of the output, big-endian after the deflate data */
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < s.outpos; i++) {
    a = (a + s.out[i]) % 65521;
    b = (b + a) % 65521;
  }
  const uint8_t *t = in + inlen - 4;
  uint32_t want = ((uint32_t)t[0] << 24) | ((uint32_t)t[1] << 16) | ((uint32_t)t[2] << 8) | t[3];
  if (((b << 16) | a) != want) return -1;
  return (long)s.outpos;
}

static uint32_t crc32_png(const uint8_t *p, size_t n) {
  static uint32_t tab[256];
  if (!tab[1])
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      tab[i] = c;
    }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 255] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

/* decode to malloc'd RGB888 (alpha dropped); NULL with *why on failure */
static uint8_t *png_decode(const uint8_t *png, size_t len, int *w, int *h, const char **why) {
  static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
  *why = "signature";
  if (len < 8 || memcmp(png, sig, 8)) return NULL;
  size_t pos = 8, idat_len = 0;
  uint8_t *idat = NULL, *rgb = NULL, *raw = NULL;
  int W = 0, H = 0, ch = 0;
  bool ihdr = false, iend = false;
  while (pos + 12 <= len && !iend) {
    uint32_t n = be32(png + pos);
    const uint8_t *type = png + pos + 4, *data = png + pos + 8;
    if (n > len || pos + 12 + n > len) { *why = "chunk length"; goto fail; }
    if (crc32_png(type, n + 4) != be32(data + n)) { *why = "chunk CRC"; goto fail; }
    if (!memcmp(type, "IHDR", 4)) {
      if (n != 13 || ihdr) { *why = "IHDR"; goto fail; }
      W = (int)be32(data);
      H = (int)be32(data + 4);
      if (data[8] != 8 || (data[9] != 2 && data[9] != 6) || data[10] || data[11] || data[12]) { *why = "IHDR format"; goto fail; }
      ch = data[9] == 2 ? 3 : 4;
      if (W <= 0 || H <= 0 || W > 20000 || H > 20000) { *why = "IHDR size"; goto fail; }
      ihdr = true;
    } else if (!memcmp(type, "IDAT", 4)) {
      if (!ihdr) { *why = "IDAT before IHDR"; goto fail; }
      uint8_t *p = realloc(idat, idat_len + n + 1);
      if (!p) { *why = "memory"; goto fail; }
      idat = p;
      memcpy(idat + idat_len, data, n);
      idat_len += n;
    } else if (!memcmp(type, "IEND", 4)) {
      iend = true;
    } else if (!(type[0] & 32)) {
      *why = "unknown critical chunk";
      goto fail;
    }
    pos += 12 + n;
  }
  if (!ihdr || !iend || !idat) { *why = "missing chunk"; goto fail; }
  size_t stride = (size_t)W * (size_t)ch, rawlen = (stride + 1) * (size_t)H;
  raw = malloc(rawlen);
  rgb = malloc((size_t)W * (size_t)H * 3);
  if (!raw || !rgb) { *why = "memory"; goto fail; }
  if (zlib_inflate(idat, idat_len, raw, rawlen) != (long)rawlen) { *why = "zlib"; goto fail; }
  /* unfilter in place */
  for (int y = 0; y < H; y++) {
    uint8_t *row = raw + (size_t)y * (stride + 1), f = row[0], *cur = row + 1;
    const uint8_t *up = y ? raw + (size_t)(y - 1) * (stride + 1) + 1 : NULL;
    if (f > 4) { *why = "filter type"; goto fail; }
    for (size_t x = 0; x < stride; x++) {
      int a = x >= (size_t)ch ? cur[x - (size_t)ch] : 0, b = up ? up[x] : 0, c = up && x >= (size_t)ch ? up[x - (size_t)ch] : 0;
      int p;
      switch (f) {
      case 0: p = 0; break;
      case 1: p = a; break;
      case 2: p = b; break;
      case 3: p = (a + b) / 2; break;
      default: {
        int pp = a + b - c, pa = abs(pp - a), pb = abs(pp - b), pc = abs(pp - c);
        p = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      }
      }
      cur[x] = (uint8_t)(cur[x] + p);
    }
    for (int x = 0; x < W; x++) memcpy(rgb + ((size_t)y * W + x) * 3, cur + (size_t)x * ch, 3);
  }
  free(idat);
  free(raw);
  *w = W;
  *h = H;
  *why = "";
  return rgb;
fail:
  free(idat);
  free(raw);
  free(rgb);
  return NULL;
}

/* ================================================================ helpers */

static uint32_t rng_state = 12345;
static uint32_t rnd(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return rng_state;
}

/* the expected RGB888 for an RGB565 pixel, written out independently of video.h */
static void ref_rgb(uint16_t p, uint8_t *o) {
  int r = p >> 11, g = (p >> 5) & 63, b = p & 31;
  o[0] = (uint8_t)(r * 8 + r / 4);
  o[1] = (uint8_t)(g * 4 + g / 16);
  o[2] = (uint8_t)(b * 8 + b / 4);
}

static char tmpdir[512];

static void make_tmpdir(void) {
  const char *base = getenv("TMPDIR");
  if (!base || !*base) base = getenv("TEMP");
  if (!base || !*base) base = getenv("TMP");
  if (!base || !*base) base = "/tmp";
  snprintf(tmpdir, sizeof tmpdir, "%s/test_video_%lld", base, (long long)(plat_wall_ms() % 100000000));
  for (char *p = tmpdir; *p; p++)
    if (*p == '\\') *p = '/';
  CHECK(plat_mkdirs(tmpdir) == 0);
}

static int rm_cb(void *ud, const char *name, bool is_dir, int64_t size, int64_t mtime) {
  (void)size; (void)mtime;
  char p[700];
  snprintf(p, sizeof p, "%s/%s", (const char *)ud, name);
  if (!is_dir) plat_remove(p);
  return 0;
}

static void rm_tree(const char *dir) {
  plat_list_dir(dir, rm_cb, (void *)dir);
  plat_remove(dir);
}

/* wait until t without relying on the OS sleep granularity (Windows Sleep() overshoots by up to
 * 15.6 ms without timeBeginPeriod): the timing tests measure the video module, not the OS */
static void spin_until(int64_t t) {
  for (int64_t now = plat_now_us(); now < t; now = plat_now_us()) plat_sleep_us(t - now > 1500 ? 1500 : t - now);
}

static void quiet_log(void *ud, const char *line) {
  (void)ud;
  (void)line;
}

/* ================================================================ layout */

typedef struct { int mw, mh; bool comp; const char *aspect; int margin; int crw, crh;
                 int rw, rh, vx, vy, vw, vh; } lay_case_t;

static void test_layout_table(void) {
  static const lay_case_t cases[] = {
    /* the PLAN 3.3 table and the spec's 240p/288p sizes */
    { 720, 480, true, "4:3", 0, 0, 0, 640, 480, 0, 0, 720, 480 },
    { 720, 480, true, "16:9", 0, 0, 0, 854, 480, 0, 0, 720, 480 },
    { 720, 576, true, "4:3", 0, 0, 0, 768, 576, 0, 0, 720, 576 },
    { 720, 576, true, "16:9", 0, 0, 0, 1024, 576, 0, 0, 720, 576 },
    { 720, 240, true, "4:3", 0, 0, 0, 320, 240, 0, 0, 720, 240 },
    { 720, 240, true, "16:9", 0, 0, 0, 427, 240, 0, 0, 720, 240 },
    { 720, 288, true, "4:3", 0, 0, 0, 384, 288, 0, 0, 720, 288 },
    { 720, 288, true, "16:9", 0, 0, 0, 512, 288, 0, 0, 720, 288 },
    /* the header's margin example */
    { 720, 480, true, "4:3", 10, 0, 0, 512, 384, 72, 48, 576, 384 },
    { 720, 480, true, "16:9", 10, 0, 0, 683, 384, 72, 48, 576, 384 },
    { 720, 576, true, "4:3", 20, 0, 0, 461, 346, 144, 115, 432, 346 },
    { 720, 240, true, "4:3", 5, 0, 0, 288, 216, 36, 12, 648, 216 },
    /* HDMI: square pixels, the mode itself, at most 1920x1200 */
    { 1280, 720, false, "4:3", 0, 0, 0, 1280, 720, 0, 0, 1280, 720 },
    { 1920, 1080, false, "16:9", 0, 0, 0, 1920, 1080, 0, 0, 1920, 1080 },
    { 1920, 1200, false, NULL, 0, 0, 0, 1920, 1200, 0, 0, 1920, 1200 },
    { 3840, 2160, false, NULL, 0, 0, 0, 1920, 1080, 0, 0, 3840, 2160 },
    { 4096, 2160, false, NULL, 0, 0, 0, 1920, 1013, 0, 0, 4096, 2160 },
    { 2560, 1600, false, NULL, 0, 0, 0, 1920, 1200, 0, 0, 2560, 1600 },
    { 1024, 768, false, NULL, 0, 0, 0, 1024, 768, 0, 0, 1024, 768 },
    { 640, 480, false, NULL, 0, 0, 0, 640, 480, 0, 0, 640, 480 },
    { 1280, 720, false, NULL, 10, 0, 0, 1024, 576, 128, 72, 1024, 576 },
    /* the aspect setting means nothing for square-pixel outputs */
    { 1280, 720, false, "16:9", 5, 0, 0, 1152, 648, 64, 36, 1152, 648 },
    /* explicit render sizes are fitted, centered */
    { 1280, 720, false, NULL, 0, 640, 480, 640, 480, 160, 0, 960, 720 },
    { 1024, 768, false, NULL, 0, 1280, 720, 1280, 720, 0, 96, 1024, 576 },
    { 720, 480, true, "4:3", 0, 1280, 720, 1280, 720, 0, 60, 720, 360 },
    { 720, 480, true, "16:9", 0, 640, 480, 640, 480, 90, 0, 540, 480 },
    { 720, 576, true, "4:3", 0, 640, 480, 640, 480, 0, 0, 720, 576 },   /* 4:3 into 4:3 */
    { 720, 480, true, "4:3", 0, 640, 480, 640, 480, 0, 0, 720, 480 },
    { 720, 480, true, "4:3", 0, 768, 576, 768, 576, 0, 0, 720, 480 },
    /* the computed size given back explicitly (drm re-fits it): the same full area */
    { 720, 480, true, "16:9", 0, 854, 480, 854, 480, 0, 0, 720, 480 },
    { 720, 240, true, "16:9", 0, 427, 240, 427, 240, 0, 0, 720, 240 },
    { 720, 480, true, "4:3", 10, 512, 384, 512, 384, 72, 48, 576, 384 },
    /* margins out of range are clamped */
    { 720, 480, true, "4:3", -5, 0, 0, 640, 480, 0, 0, 720, 480 },
    { 720, 480, true, "4:3", 99, 0, 0, 384, 288, 144, 96, 432, 288 },
    /* a bad aspect string falls back to 4:3 */
    { 720, 480, true, "wide", 0, 0, 0, 640, 480, 0, 0, 720, 480 },
    { 720, 480, true, "0:9", 0, 0, 0, 640, 480, 0, 0, 720, 480 },
    { 720, 480, true, "16:9x", 0, 0, 0, 640, 480, 0, 0, 720, 480 },
    { 720, 480, true, " 16 : 9 ", 0, 0, 0, 854, 480, 0, 0, 720, 480 },
    { 720, 480, true, "21:9", 0, 0, 0, 1120, 480, 0, 0, 720, 480 },
    /* nonsense explicit sizes are ignored */
    { 720, 480, true, "4:3", 0, -1, 480, 640, 480, 0, 0, 720, 480 },
    { 720, 480, true, "4:3", 0, 640, 0, 640, 480, 0, 0, 720, 480 },
    { 720, 480, true, "4:3", 0, 70000, 480, 640, 480, 0, 0, 720, 480 },
    /* odd modes */
    { 721, 481, false, NULL, 0, 0, 0, 721, 481, 0, 0, 721, 481 },
    { 1, 1, false, NULL, 0, 0, 0, 1, 1, 0, 0, 1, 1 },
    { 1, 1, true, "16:9", 20, 0, 0, 2, 1, 0, 0, 1, 1 },
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    const lay_case_t *c = &cases[i];
    video_layout_t l;
    memset(&l, 0x55, sizeof l);
    int r = video_layout(c->mw, c->mh, c->comp, c->aspect, c->margin, c->crw, c->crh, &l);
    char what[160];
    snprintf(what, sizeof what, "case %u: %dx%d %s %s m%d r%dx%d", (unsigned)i, c->mw, c->mh, c->comp ? "comp" : "sq",
             c->aspect ? c->aspect : "-", c->margin, c->crw, c->crh);
    bool ok = r == 0 && l.render_w == c->rw && l.render_h == c->rh && l.view_x == c->vx && l.view_y == c->vy &&
              l.view_w == c->vw && l.view_h == c->vh;
    t_count++;
    if (!ok) {
      t_fail++;
      fprintf(stderr, "%s: got r=%d render %dx%d view %d,%d %dx%d, want render %dx%d view %d,%d %dx%d\n", what, r,
              l.render_w, l.render_h, l.view_x, l.view_y, l.view_w, l.view_h, c->rw, c->rh, c->vx, c->vy, c->vw, c->vh);
    }
  }
}

/* every AV mode x aspect x margin, and a set of HDMI modes x margin: the invariants */
static void test_layout_sweep(void) {
  static const int av[4][2] = { { 720, 480 }, { 720, 576 }, { 720, 240 }, { 720, 288 } };
  static const char *aspects[] = { "4:3", "16:9" };
  for (int m = 0; m < 4; m++)
    for (int a = 0; a < 2; a++)
      for (int mg = -2; mg <= 25; mg++) {
        int mw = av[m][0], mh = av[m][1], aw = a ? 16 : 4, ah = a ? 9 : 3;
        video_layout_t l;
        CHECK(video_layout(mw, mh, true, aspects[a], mg, 0, 0, &l) == 0);
        int cm = mg < 0 ? 0 : mg > 20 ? 20 : mg;
        int mx = (2 * mw * cm + 100) / 200, my = (2 * mh * cm + 100) / 200;
        int area_w = mw - 2 * mx, area_h = mh - 2 * my;
        /* the view is exactly the inset area: no vertical scaling, all lines used */
        CHECK_INT(l.view_x, mx);
        CHECK_INT(l.view_y, my);
        CHECK_INT(l.view_w, area_w);
        CHECK_INT(l.view_h, area_h);
        CHECK_INT(l.render_h, area_h);
        /* width: square pixels on a screen of shape aw:ah, rounded up (never narrower) */
        double exact = (double)area_w * aw * mh / ((double)ah * mw);
        CHECK(l.render_w >= exact - 1e-9 && l.render_w < exact + 1);
        /* the picture's shape on the goggles matches the render shape within half a pixel */
        double shown = (double)l.view_w / l.view_h * ((double)aw * mh / ((double)ah * mw));
        CHECK(fabs(shown * l.render_h - l.render_w) < 1.0);
        /* video_render_size is the no-margin case */
        if (mg == 0) {
          int rw = 0, rh = 0;
          video_render_size(mw, mh, true, aspects[a], &rw, &rh);
          CHECK_INT(rw, l.render_w);
          CHECK_INT(rh, l.render_h);
        }
        /* giving the computed size back explicitly changes nothing (drm does this) */
        video_layout_t l2;
        CHECK(video_layout(mw, mh, true, aspects[a], mg, l.render_w, l.render_h, &l2) == 0);
        CHECK(!memcmp(&l, &l2, sizeof l));
      }
  static const int sq[][2] = { { 1280, 720 }, { 1920, 1080 }, { 1024, 768 }, { 800, 600 }, { 640, 480 },
                               { 3840, 2160 }, { 1366, 768 }, { 1280, 1024 }, { 720, 480 }, { 7680, 4320 } };
  for (size_t m = 0; m < sizeof sq / sizeof sq[0]; m++)
    for (int mg = 0; mg <= 20; mg += 3) {
      video_layout_t l;
      CHECK(video_layout(sq[m][0], sq[m][1], false, "16:9", mg, 0, 0, &l) == 0);
      CHECK(l.render_w <= 1920 && l.render_h <= 1200 && l.render_w > 0 && l.render_h > 0);
      CHECK(l.view_x >= 0 && l.view_y >= 0 && l.view_x + l.view_w <= sq[m][0] && l.view_y + l.view_h <= sq[m][1]);
      /* centered */
      CHECK(abs((sq[m][0] - l.view_w) - 2 * l.view_x) <= 1);
      CHECK(abs((sq[m][1] - l.view_h) - 2 * l.view_y) <= 1);
      /* square pixels: same shape (within a pixel of rounding) */
      CHECK(fabs((double)l.render_w / l.render_h * l.view_h - l.view_w) <= 2.0 * l.view_w / l.render_w + 0.5);
      if (sq[m][0] <= 1920 && sq[m][1] <= 1200) {
        CHECK_INT(l.render_w, l.view_w);   /* 1:1 */
        CHECK_INT(l.render_h, l.view_h);
      }
    }
  /* explicit sizes: always inside the area, centered, shape kept */
  static const int rs[][2] = { { 640, 480 }, { 1280, 720 }, { 320, 240 }, { 480, 272 }, { 800, 480 }, { 100, 1000 }, { 1000, 100 }, { 1, 1 } };
  for (size_t m = 0; m < 4; m++)
    for (size_t r = 0; r < sizeof rs / sizeof rs[0]; r++)
      for (int mg = 0; mg <= 20; mg += 5) {
        video_layout_t l;
        CHECK(video_layout(av[m][0], av[m][1], true, "4:3", mg, rs[r][0], rs[r][1], &l) == 0);
        CHECK_INT(l.render_w, rs[r][0]);
        CHECK_INT(l.render_h, rs[r][1]);
        int mx = (2 * av[m][0] * mg + 100) / 200, my = (2 * av[m][1] * mg + 100) / 200;
        CHECK(l.view_x >= mx && l.view_y >= my && l.view_w >= 1 && l.view_h >= 1);
        CHECK(l.view_x + l.view_w <= av[m][0] - mx && l.view_y + l.view_h <= av[m][1] - my);
        CHECK(l.view_w == av[m][0] - 2 * mx || l.view_h == av[m][1] - 2 * my);   /* touches two sides */
        double par = (4.0 * av[m][1]) / (3.0 * av[m][0]);
        double shape = (double)l.view_w * par / l.view_h, want = (double)rs[r][0] / rs[r][1];
        if (l.view_w > 20 && l.view_h > 20) CHECK(fabs(shape - want) / want < 0.03);
      }
}

static void test_layout_bad(void) {
  static const int bad[][2] = { { 0, 480 }, { 720, 0 }, { -720, 480 }, { 720, -1 }, { 65536, 480 }, { 720, 70000 } };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    video_layout_t l;
    CHECK_INT(video_layout(bad[i][0], bad[i][1], true, "4:3", 0, 0, 0, &l), -1);
    CHECK(l.render_w == 640 && l.render_h == 480 && l.view_x == 0 && l.view_y == 0 && l.view_w == 640 && l.view_h == 480);
    int rw = -1, rh = -1;
    video_render_size(bad[i][0], bad[i][1], false, NULL, &rw, &rh);
    CHECK(rw == 640 && rh == 480);
  }
  video_render_size(720, 480, true, "4:3", NULL, NULL);   /* NULL outputs are allowed */
  CHECK(1);
}

static void test_parse_mode(void) {
  struct { const char *s; int r, w, h; double hz; bool il; } c[] = {
    { "auto", 0, 0, 0, 0, false },      { "AUTO", 0, 0, 0, 0, false },     { "  auto  ", 0, 0, 0, 0, false },
    { "", 0, 0, 0, 0, false },          { "   ", 0, 0, 0, 0, false },      { NULL, 0, 0, 0, 0, false },
    { "1280x720", 1, 1280, 720, 0, false }, { "1920X1080", 1, 1920, 1080, 0, false },
    { "1280x720@60", 1, 1280, 720, 60, false }, { "1280x720@59.94", 1, 1280, 720, 59.94, false },
    { "1920x1080i", 1, 1920, 1080, 0, true }, { "1920x1080i@60", 1, 1920, 1080, 60, true },
    { "1920x1080@60i", 1, 1920, 1080, 60, true }, { "1920x1080p@50", 1, 1920, 1080, 50, false },
    { "1280x720@60Hz", 1, 1280, 720, 60, false }, { "1280x720@60hz ", 1, 1280, 720, 60, false },
    { " 800x600", 1, 800, 600, 0, false },
    { "autox", -1, 0, 0, 0, false },    { "1280", -1, 0, 0, 0, false },    { "1280x", -1, 0, 0, 0, false },
    { "x720", -1, 0, 0, 0, false },     { "1280x720@", -1, 0, 0, 0, false }, { "1280x720@abc", -1, 0, 0, 0, false },
    { "1280x720 junk", -1, 0, 0, 0, false }, { "1280 x 720", -1, 0, 0, 0, false }, { "-1280x720", -1, 0, 0, 0, false },
    { "32x32", -1, 0, 0, 0, false },    { "9000x720", -1, 0, 0, 0, false }, { "1280x720@5", -1, 0, 0, 0, false },
    { "1280x720@500", -1, 0, 0, 0, false }, { "1280x720ii", -1, 0, 0, 0, false }, { "hdmi", -1, 0, 0, 0, false },
  };
  for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
    int w = -1, h = -1;
    double hz = -1;
    bool il = true;
    int r = video_parse_mode(c[i].s, &w, &h, &hz, &il);
    t_count++;
    if (r != c[i].r || (r == 1 && (w != c[i].w || h != c[i].h || fabs(hz - c[i].hz) > 1e-9 || il != c[i].il)) ||
        (r != 1 && (w || h || hz != 0 || il))) {
      t_fail++;
      fprintf(stderr, "parse_mode(\"%s\") = %d %dx%d@%g%s\n", c[i].s ? c[i].s : "(null)", r, w, h, hz, il ? "i" : "");
    }
  }
}

/* ================================================================ mode selection */

#define IL VIDEO_MODE_FLAG_INTERLACE
#define PREF VIDEO_MODE_TYPE_PREFERRED
#define DRV VIDEO_MODE_TYPE_DRIVER

static video_mode_t mk(uint32_t clk, int hd, int ht, int vd, int vt, uint32_t flags, uint32_t type) {
  video_mode_t m;
  memset(&m, 0, sizeof m);
  m.clock = clk;
  m.hdisplay = (uint16_t)hd;
  m.hsync_start = (uint16_t)(hd + 8);
  m.hsync_end = (uint16_t)(hd + 16);
  m.htotal = (uint16_t)ht;
  m.vdisplay = (uint16_t)vd;
  m.vsync_start = (uint16_t)(vd + 2);
  m.vsync_end = (uint16_t)(vd + 5);
  m.vtotal = (uint16_t)vt;
  m.flags = flags;
  m.type = type;
  m.vrefresh = (uint32_t)lround(clk * 1000.0 / ((double)ht * vt) * (flags & IL ? 2 : 1));
  snprintf(m.name, sizeof m.name, "%dx%d%s", hd, vd, flags & IL ? "i" : "");
  return m;
}

/* a mode with exact timings (the kernel's tables) */
static video_mode_t mkx(uint32_t clk, int hd, int hss, int hse, int ht, int vd, int vss, int vse, int vt, uint32_t flags, uint32_t type) {
  video_mode_t m = mk(clk, hd, ht, vd, vt, flags, type);
  m.hsync_start = (uint16_t)hss;
  m.hsync_end = (uint16_t)hse;
  m.vsync_start = (uint16_t)vss;
  m.vsync_end = (uint16_t)vse;
  return m;
}

static bool is_mode(const video_mode_t *m, int w, int h, bool il, double hz) {
  return m->hdisplay == w && m->vdisplay == h && ((m->flags & IL) != 0) == il && fabs(video_mode_hz(m) - hz) < 0.02;
}

static bool no_dups(const video_mode_t *c, int n) {
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++)
      if (video_same_timing(&c[i], &c[j])) return false;
  for (int i = 0; i < n; i++)
    if (c[i].flags & VIDEO_MODE_FLAG_PIC_AR_MASK) return false;
  return n >= 1 && n <= VIDEO_MAX_CANDIDATES;
}

static void test_mode_helpers(void) {
  /* the four VEC modes of rpi-6.12.y (spec 4.6 table) */
  video_mode_t i480 = mk(13500, 720, 858, 480, 525, IL, PREF), i576 = mk(13500, 720, 864, 576, 625, IL, 0);
  video_mode_t p240 = mk(13500, 720, 858, 240, 262, 0, DRV), p288 = mk(13500, 720, 864, 288, 312, 0, DRV);
  CHECK_NEAR(video_mode_hz(&i480), 59.94, 0.005);   /* field rate */
  CHECK_NEAR(video_mode_hz(&i576), 50.0, 0.005);
  CHECK_NEAR(video_mode_hz(&p240), 60.05, 0.005);
  CHECK_NEAR(video_mode_hz(&p288), 50.08, 0.005);
  video_mode_t m = mk(74250, 1280, 1650, 720, 750, 0, 0);
  CHECK_NEAR(video_mode_hz(&m), 60.0, 1e-9);
  m.flags = VIDEO_MODE_FLAG_DBLSCAN;
  CHECK_NEAR(video_mode_hz(&m), 30.0, 1e-9);
  m.flags = 0;
  m.vscan = 2;
  CHECK_NEAR(video_mode_hz(&m), 30.0, 1e-9);
  m.clock = 0;
  m.vrefresh = 75;
  CHECK_NEAR(video_mode_hz(&m), 75.0, 1e-9);        /* no timings: the vrefresh field */
  m.vrefresh = 0;
  CHECK_NEAR(video_mode_hz(&m), 60.0, 1e-9);
  /* same_timing ignores AR bits, type and name, nothing else */
  video_mode_t a = mk(74250, 1280, 1650, 720, 750, VIDEO_MODE_FLAG_PHSYNC, PREF), b = a;
  b.flags |= 3u << 19;
  b.type = 0;
  strcpy(b.name, "other");
  CHECK(video_same_timing(&a, &b));
  b = a; b.clock++; CHECK(!video_same_timing(&a, &b));
  b = a; b.hsync_start++; CHECK(!video_same_timing(&a, &b));
  b = a; b.hsync_end++; CHECK(!video_same_timing(&a, &b));
  b = a; b.htotal++; CHECK(!video_same_timing(&a, &b));
  b = a; b.vdisplay++; CHECK(!video_same_timing(&a, &b));
  b = a; b.vsync_start++; CHECK(!video_same_timing(&a, &b));
  b = a; b.vsync_end++; CHECK(!video_same_timing(&a, &b));
  b = a; b.vtotal++; CHECK(!video_same_timing(&a, &b));
  b = a; b.vscan = 2; CHECK(!video_same_timing(&a, &b));
  b = a; b.flags |= IL; CHECK(!video_same_timing(&a, &b));
  /* TV norms: the "TV mode" names; pal60 is PAL on the 525-line mode (the PAL60 trap) */
  static const char *tv[][2] = { { "ntsc", "NTSC" }, { "ntsc-j", "NTSC-J" }, { "ntsc-443", "NTSC-443" },
                                 { "pal-m", "PAL-M" }, { "pal60", "PAL" }, { "pal", "PAL" }, { "pal-n", "PAL-N" },
                                 { "secam", "SECAM" }, { "PAL60", "PAL" }, { "Pal", "PAL" }, { "mono", "NTSC" },
                                 { "", "NTSC" }, { "palx", "NTSC" } };
  for (size_t i = 0; i < sizeof tv / sizeof tv[0]; i++) CHECK_STR(video_tv_norm(tv[i][0]), tv[i][1]);
  CHECK_STR(video_tv_norm(NULL), "NTSC");
  CHECK(video_tv_625("pal") && video_tv_625("PAL") && video_tv_625("pal-n") && video_tv_625("secam"));
  CHECK(!video_tv_625("pal60") && !video_tv_625("pal-m") && !video_tv_625("ntsc") && !video_tv_625("ntsc-j") &&
        !video_tv_625(NULL) && !video_tv_625(""));
  /* the VEC's (norm, htotal) table */
  CHECK(video_vec_valid("PAL", 858) && video_vec_valid("PAL", 864) && !video_vec_valid("PAL", 860));
  CHECK(video_vec_valid("Mono", 858) && video_vec_valid("Mono", 864));
  CHECK(!video_vec_valid("PAL-N", 858) && video_vec_valid("PAL-N", 864));
  CHECK(!video_vec_valid("SECAM", 858) && video_vec_valid("SECAM", 864));
  CHECK(video_vec_valid("NTSC", 858) && !video_vec_valid("NTSC", 864));
  CHECK(video_vec_valid("NTSC-J", 858) && !video_vec_valid("NTSC-J", 864));
  CHECK(video_vec_valid("NTSC-443", 858) && !video_vec_valid("NTSC-443", 864));
  CHECK(video_vec_valid("PAL-M", 858) && !video_vec_valid("PAL-M", 864));
  CHECK(!video_vec_valid(NULL, 858));
  /* every tv setting's first mode passes the VEC check with its own norm */
  static const char *sets[] = { "ntsc", "ntsc-j", "ntsc-443", "pal-m", "pal60", "pal", "pal-n", "secam" };
  for (size_t i = 0; i < sizeof sets / sizeof sets[0]; i++)
    for (int prog = 0; prog < 2; prog++) {
      video_mode_t c[VIDEO_MAX_CANDIDATES];
      int n = video_composite_modes(NULL, 0, sets[i], prog, c);
      CHECK(n >= 1 && video_vec_valid(video_tv_norm(sets[i]), c[0].htotal));
    }
}

static void test_composite_modes(void) {
  /* the list a Pi 4 composite connector reports (spec 4.6; NTSC default norm: 480i preferred) */
  const uint32_t nhnv = VIDEO_MODE_FLAG_NHSYNC | VIDEO_MODE_FLAG_NVSYNC;
  video_mode_t i480 = mkx(13500, 720, 736, 800, 858, 480, 486, 492, 525, IL | nhnv, PREF);
  video_mode_t i576 = mkx(13500, 720, 732, 796, 864, 576, 581, 587, 625, IL | nhnv, 0);
  video_mode_t p288 = mkx(13500, 720, 740, 804, 864, 288, 290, 293, 312, 0, DRV);
  video_mode_t p240 = mkx(13500, 720, 734, 798, 858, 240, 243, 246, 262, 0, DRV);
  video_mode_t pi[4] = { i480, i576, p288, p240 };
  struct { const char *tv; bool prog; int h; bool il; double hz; } want[] = {
    { "ntsc", false, 480, true, 59.94 },   { "ntsc", true, 240, false, 60.05 },  { "ntsc-j", false, 480, true, 59.94 },
    { "ntsc-443", false, 480, true, 59.94 }, { "pal-m", false, 480, true, 59.94 }, { "pal-m", true, 240, false, 60.05 },
    { "pal60", false, 480, true, 59.94 },  { "pal60", true, 240, false, 60.05 }, { "pal", false, 576, true, 50.0 },
    { "pal", true, 288, false, 50.08 },    { "pal-n", false, 576, true, 50.0 },  { "secam", false, 576, true, 50.0 },
    { "secam", true, 288, false, 50.08 },  { "PAL", false, 576, true, 50.0 },    { "bogus", false, 480, true, 59.94 },
    { NULL, false, 480, true, 59.94 },
  };
  for (size_t i = 0; i < sizeof want / sizeof want[0]; i++) {
    video_mode_t c[VIDEO_MAX_CANDIDATES];
    int n = video_composite_modes(pi, 4, want[i].tv, want[i].prog, c);
    t_count++;
    if (!no_dups(c, n) || !is_mode(&c[0], 720, want[i].h, want[i].il, want[i].hz)) {
      t_fail++;
      fprintf(stderr, "composite %s %s: first %ux%u%s@%.2f (n=%d)\n", want[i].tv ? want[i].tv : "(null)",
              want[i].prog ? "p" : "i", c[0].hdisplay, c[0].vdisplay, c[0].flags & IL ? "i" : "", video_mode_hz(&c[0]), n);
    }
    /* the list's own copy is used (keeps the kernel's type bits), and the built-in duplicate dropped */
    CHECK(c[0].type == (want[i].h == 480 ? PREF : want[i].h == 576 ? 0u : DRV));
  }
  /* after the target: the list's preferred mode, the other scan of the norm; the built-in copies
   * of modes already there (same timings) are dropped */
  video_mode_t c[VIDEO_MAX_CANDIDATES];
  int n = video_composite_modes(pi, 4, "pal", false, c);
  CHECK(no_dups(c, n) && n == 3);
  CHECK(is_mode(&c[0], 720, 576, true, 50.0) && is_mode(&c[1], 720, 480, true, 59.94) && is_mode(&c[2], 720, 288, false, 50.08));
  CHECK(c[1].type == PREF);
  n = video_composite_modes(pi, 4, "ntsc", true, c);
  CHECK(no_dups(c, n) && n == 3 && c[0].type == DRV && is_mode(&c[1], 720, 480, true, 59.94) && is_mode(&c[2], 720, 576, true, 50.0));
  /* a list without the progressive modes (older kernels): the built-in 240p is still tried first */
  video_mode_t old[2] = { i480, i576 };
  n = video_composite_modes(old, 2, "ntsc", true, c);
  CHECK(no_dups(c, n) && is_mode(&c[0], 720, 240, false, 60.05) && c[0].type == VIDEO_MODE_TYPE_USERDEF);
  CHECK(c[0].htotal == 858 && c[0].vtotal == 262 && c[0].vsync_end - c[0].vsync_start == 3);   /* VEC rules */
  n = video_composite_modes(old, 2, "pal", true, c);
  CHECK(is_mode(&c[0], 720, 288, false, 50.08) && c[0].htotal == 864 && c[0].vtotal == 312);
  /* no list (status unknown, nothing probed): the built-in modes, target first */
  n = video_composite_modes(NULL, 0, "pal", false, c);
  CHECK(no_dups(c, n) && n == 3 && is_mode(&c[0], 720, 576, true, 50.0) && is_mode(&c[1], 720, 288, false, 50.08) &&
        is_mode(&c[2], 720, 480, true, 59.94));
  n = video_composite_modes(pi, -3, "ntsc", false, c);   /* a negative count is an empty list */
  CHECK(n == 3 && is_mode(&c[0], 720, 480, true, 59.94));
  /* two copies of the target: the PREFERRED one wins even when it comes later */
  video_mode_t alt = i480;
  alt.type = 0;
  alt.hsync_start += 2;
  video_mode_t two[2] = { alt, i480 };
  n = video_composite_modes(two, 2, "ntsc", false, c);
  CHECK(video_same_timing(&c[0], &i480));
  /* a doublescan mode is never the target; AR bits are cleared */
  video_mode_t dbl = i480;
  dbl.flags |= VIDEO_MODE_FLAG_DBLSCAN;
  dbl.type = PREF;
  video_mode_t ar = i576;
  ar.flags |= 2u << 19;
  video_mode_t mix[2] = { dbl, ar };
  n = video_composite_modes(mix, 2, "ntsc", false, c);
  CHECK(no_dups(c, n) && !(c[0].flags & VIDEO_MODE_FLAG_DBLSCAN) && is_mode(&c[0], 720, 480, true, 59.94));
  n = video_composite_modes(mix, 2, "pal", false, c);
  CHECK(is_mode(&c[0], 720, 576, true, 50.0) && !(c[0].flags & VIDEO_MODE_FLAG_PIC_AR_MASK));
  /* a huge list of preferred modes: capped */
  static video_mode_t many[300];
  for (int i = 0; i < 300; i++) many[i] = mk(13500 + (uint32_t)i, 720, 858, 480, 525, IL, PREF);
  n = video_composite_modes(many, 300, "ntsc", false, c);
  CHECK(n == VIDEO_MAX_CANDIDATES && no_dups(c, n));
}

static void test_hdmi_modes(void) {
  /* a typical TV: 1080p60 preferred, the 59.94 twins, 1080i, 720p50, SD */
  video_mode_t tv[] = {
    mk(148500, 1920, 2200, 1080, 1125, 0, PREF), mk(148352, 1920, 2200, 1080, 1125, 0, 0),
    mk(74250, 1920, 2200, 1080, 1125, IL, 0),    mk(74176, 1280, 1650, 720, 750, 0, 0),
    mk(74250, 1280, 1650, 720, 750, 0, 0),       mk(74250, 1280, 1980, 720, 750, 0, 0),
    mk(148500, 1920, 2640, 1080, 1125, 0, 0),    mk(27000, 720, 858, 480, 525, 0, 0),
    mk(25175, 640, 800, 480, 525, 0, 0),         mk(297000, 3840, 4400, 2160, 2250, 0, 0),
  };
  int nt = (int)(sizeof tv / sizeof tv[0]);
  video_mode_t c[VIDEO_MAX_CANDIDATES];
  int note = -1;
  int n = video_hdmi_modes(tv, nt, "auto", c, &note);
  CHECK(note == 0 && no_dups(c, n));
  CHECK(is_mode(&c[0], 1280, 720, false, 60.0) && c[0].clock == 74250);   /* 60.00 over 59.94 */
  CHECK(is_mode(&c[1], 1920, 1080, false, 60.0) && c[1].clock == 148500);
  n = video_hdmi_modes(tv, nt, NULL, c, NULL);                            /* NULL = auto, note optional */
  CHECK(c[0].clock == 74250 && c[0].hdisplay == 1280);
  /* explicit sizes */
  n = video_hdmi_modes(tv, nt, "1920x1080", c, &note);
  CHECK(note == 0 && is_mode(&c[0], 1920, 1080, false, 60.0) && c[0].clock == 148500);
  n = video_hdmi_modes(tv, nt, "1920x1080@59.94", c, &note);
  CHECK(note == 0 && c[0].clock == 148352);
  n = video_hdmi_modes(tv, nt, "1920x1080@50", c, &note);
  CHECK(note == 0 && is_mode(&c[0], 1920, 1080, false, 50.0));
  n = video_hdmi_modes(tv, nt, "1280x720@50Hz", c, &note);
  CHECK(note == 0 && is_mode(&c[0], 1280, 720, false, 50.0));
  n = video_hdmi_modes(tv, nt, "1920x1080i", c, &note);
  CHECK(note == 0 && is_mode(&c[0], 1920, 1080, true, 60.0));
  n = video_hdmi_modes(tv, nt, "3840x2160", c, &note);                    /* only 30 Hz there: the fastest */
  CHECK(note == 0 && is_mode(&c[0], 3840, 2160, false, 30.0));
  /* not offered: noted; a built-in copy when there is one, else auto */
  n = video_hdmi_modes(tv, nt, "1024x768", c, &note);
  CHECK(note == 2 && is_mode(&c[0], 1024, 768, false, 60.0) && c[0].type == VIDEO_MODE_TYPE_USERDEF);
  CHECK(is_mode(&c[1], 1280, 720, false, 60.0));
  n = video_hdmi_modes(tv, nt, "1366x768", c, &note);
  CHECK(note == 2 && c[0].hdisplay == 1280 && c[0].clock == 74250);
  n = video_hdmi_modes(tv, nt, "720x576i", c, &note);
  CHECK(note == 2 && c[0].hdisplay == 1280);                              /* no built-in interlaced modes */
  n = video_hdmi_modes(tv, nt, "1920x1080@75", c, &note);
  CHECK(note == 2 && c[0].hdisplay == 1280);
  n = video_hdmi_modes(tv, nt, "big", c, &note);
  CHECK(note == 1 && c[0].hdisplay == 1280 && no_dups(c, n));
  /* no display (disconnected: no modes): the built-in 1280x720@60, blind mode */
  n = video_hdmi_modes(NULL, 0, "auto", c, &note);
  CHECK(n == 1 && note == 0 && is_mode(&c[0], 1280, 720, false, 60.0) && c[0].clock == 74250 && c[0].htotal == 1650 &&
        c[0].hsync_start == 1390 && c[0].vsync_start == 725 && c[0].vtotal == 750 &&
        c[0].flags == (VIDEO_MODE_FLAG_PHSYNC | VIDEO_MODE_FLAG_PVSYNC));
  n = video_hdmi_modes(NULL, 0, "1920x1080@60", c, &note);                /* blind + explicit: no note */
  CHECK(n == 2 && note == 0 && is_mode(&c[0], 1920, 1080, false, 60.0) && c[0].clock == 148500 &&
        is_mode(&c[1], 1280, 720, false, 60.0));
  n = video_hdmi_modes(NULL, 0, "1280x720@50", c, &note);
  CHECK(is_mode(&c[0], 1280, 720, false, 50.0) && c[0].htotal == 1980);
  n = video_hdmi_modes(NULL, 0, "640x480@60", c, &note);
  CHECK(is_mode(&c[0], 640, 480, false, 59.94) && c[0].clock == 25175);
  /* connected without EDID: the kernel's no-EDID list up to 1024x768 (that one preferred) */
  video_mode_t noedid[] = { mk(65000, 1024, 1344, 768, 806, 0, PREF), mk(40000, 800, 1056, 600, 628, 0, 0),
                            mk(25175, 640, 800, 480, 525, 0, 0) };
  n = video_hdmi_modes(noedid, 3, "auto", c, &note);
  CHECK(no_dups(c, n) && is_mode(&c[0], 1024, 768, false, 60.0));
  /* a 4K screen preferring 2160p30, no 720p/1080p60: not the slow preferred mode, the nearest to 720 lines */
  video_mode_t k4[] = { mk(297000, 3840, 4400, 2160, 2250, 0, PREF), mk(594000, 3840, 4400, 2160, 2250, 0, 0),
                        mk(74250, 1920, 2200, 1080, 1125, 0, 0), mk(241500, 2560, 2720, 1440, 1481, 0, 0) };
  n = video_hdmi_modes(k4, 4, "auto", c, &note);
  CHECK(no_dups(c, n) && is_mode(&c[0], 2560, 1440, false, 59.95));
  CHECK(is_mode(&c[1], 1280, 720, false, 60.0) && c[1].type == VIDEO_MODE_TYPE_USERDEF);   /* then blind 720p */
  /* a preferred mode within limits comes right after 720p/1080p */
  video_mode_t mon[] = { mk(108000, 1280, 1688, 1024, 1066, 0, PREF), mk(74250, 1280, 1650, 720, 750, 0, 0) };
  n = video_hdmi_modes(mon, 2, "auto", c, &note);
  CHECK(is_mode(&c[0], 1280, 720, false, 60.0) && is_mode(&c[1], 1280, 1024, false, 60.02));
  /* 720p60 twice: the preferred one, whatever the order */
  video_mode_t twin[] = { mk(74250, 1280, 1650, 720, 750, 0, 0), mk(74250, 1280, 1650, 720, 750, VIDEO_MODE_FLAG_NHSYNC, PREF) };
  n = video_hdmi_modes(twin, 2, "auto", c, &note);
  CHECK(c[0].type == PREF);
  /* interlaced or doublescan modes are never picked by auto */
  video_mode_t odd[] = { mk(74250, 1920, 2200, 1080, 1125, IL, PREF), mk(74250, 1280, 1650, 720, 750, VIDEO_MODE_FLAG_DBLSCAN, 0),
                         mk(65000, 1024, 1344, 768, 806, 0, 0) };
  n = video_hdmi_modes(odd, 3, "auto", c, &note);
  CHECK(is_mode(&c[0], 1024, 768, false, 60.0));
  /* AR bits from the list are cleared */
  video_mode_t arm = mk(74250, 1280, 1650, 720, 750, 3u << 19, 0);
  n = video_hdmi_modes(&arm, 1, "auto", c, &note);
  CHECK(c[0].clock == 74250 && !(c[0].flags & VIDEO_MODE_FLAG_PIC_AR_MASK) && no_dups(c, n));
}

/* ================================================================ CPU scaling */

static void test_luts(void) {
  static const int sizes[] = { 1, 2, 3, 7, 239, 240, 288, 320, 384, 427, 432, 480, 512, 576, 640, 683,
                               704, 720, 768, 853, 854, 1024, 1080, 1280, 1920, 4097 };
  static uint16_t lut[8192];
  int n = (int)(sizeof sizes / sizeof sizes[0]);
  for (int a = 0; a < n; a++)
    for (int b = 0; b < n; b++) {
      int s = sizes[a], d = sizes[b];
      lut[d] = 0xBEEF;
      video_scale_luts(s, s, d, d, lut, NULL);
      CHECK(lut[d] == 0xBEEF);                     /* exactly d entries */
      bool ok = true;
      for (int i = 0; i < d; i++) {
        int want = (int)floor((i + 0.5) * s / d);   /* nearest at pixel centres */
        if (want > s - 1) want = s - 1;
        if (lut[i] != want || (i && lut[i] < lut[i - 1])) ok = false;
      }
      if (s == d) for (int i = 0; i < d; i++) ok = ok && lut[i] == i;
      if (d >= s) ok = ok && lut[0] == 0 && lut[d - 1] == s - 1;   /* upscaling uses every pixel */
      if (d >= s) for (int i = 1; i < d; i++) ok = ok && lut[i] - lut[i - 1] <= 1;
      t_count++;
      if (!ok) {
        t_fail++;
        fprintf(stderr, "lut %d -> %d wrong\n", s, d);
      }
    }
  /* x and y tables are independent; NULL tables are skipped */
  uint16_t x[5], y[3];
  video_scale_luts(10, 3, 5, 3, x, y);
  CHECK(x[0] == 1 && x[1] == 3 && x[2] == 5 && x[3] == 7 && x[4] == 9);
  CHECK(y[0] == 0 && y[1] == 1 && y[2] == 2);
  video_scale_luts(10, 3, 5, 3, NULL, NULL);
  video_scale_luts(0, 0, 5, 3, x, y);              /* no source: nothing written, no crash */
  CHECK(x[0] == 1 && y[2] == 2);
  /* the largest sizes the uint16 tables allow */
  static uint16_t big[65535];
  video_scale_luts(65535, 1, 65535, 1, big, NULL);
  CHECK(big[0] == 0 && big[65534] == 65534);
  video_scale_luts(1, 1, 65535, 1, big, NULL);
  CHECK(big[0] == 0 && big[65534] == 0);
}

/* scale a random picture into a buffer with a sentinel border and compare every pixel */
static void check_scale(int sw, int sh, int dx, int dy, int dw, int dh, int bpp, bool use_pixlut) {
  int W = dx + dw + 3, H = dy + dh + 2;
  int pitch = W * (bpp / 8) + 8;                  /* a padded pitch */
  uint16_t *src = malloc(sizeof(uint16_t) * (size_t)sw * (size_t)sh);
  uint8_t *dst = malloc((size_t)pitch * (size_t)H);
  uint16_t *xl = malloc(sizeof(uint16_t) * (size_t)dw), *yl = malloc(sizeof(uint16_t) * (size_t)dh);
  static uint32_t pixlut[65536];
  if (!src || !dst || !xl || !yl) { CHECK(!"memory"); goto out; }
  for (int i = 0; i < sw * sh; i++) src[i] = (uint16_t)rnd();
  memset(dst, 0xA5, (size_t)pitch * (size_t)H);
  if (use_pixlut)
    for (uint32_t p = 0; p < 65536; p++) pixlut[p] = bpp == 16 ? (uint32_t)(((p & 31) << 11) | (p & 0x7E0) | (p >> 11)) : (p * 2654435761u);
  bool one = dw == sw && dh == sh && (rnd() & 1);
  if (!one) video_scale_luts(sw, sh, dw, dh, xl, yl);
  video_scale_copy_lut(src, sw, dst, pitch, bpp, dx, dy, dw, dh, one ? NULL : xl, one ? NULL : yl, use_pixlut ? pixlut : NULL);
  bool ok = true;
  for (int y = 0; y < H && ok; y++)
    for (int x = 0; x < pitch && ok; x++) {
      int px = x / (bpp / 8);
      bool inside = y >= dy && y < dy + dh && x < W * (bpp / 8) && px >= dx && px < dx + dw;
      if (!inside) { ok = dst[(size_t)y * pitch + x] == 0xA5; continue; }
      if ((x % (bpp / 8)) != 0) continue;
      int sx = one ? px - dx : xl[px - dx], sy = one ? y - dy : yl[y - dy];
      uint16_t s = src[(size_t)sy * sw + sx];
      if (bpp == 16) {
        uint16_t got;
        memcpy(&got, dst + (size_t)y * pitch + x, 2);
        ok = got == (use_pixlut ? (uint16_t)pixlut[s] : s);
      } else {
        uint32_t got, want;
        memcpy(&got, dst + (size_t)y * pitch + x, 4);
        if (use_pixlut) want = pixlut[s];
        else {
          uint8_t c[3];
          ref_rgb(s, c);
          want = 0xFF000000u | ((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2];
        }
        ok = got == want;
      }
    }
  t_count++;
  if (!ok) {
    t_fail++;
    fprintf(stderr, "scale %dx%d -> %d,%d %dx%d bpp %d%s wrong\n", sw, sh, dx, dy, dw, dh, bpp, use_pixlut ? " pixlut" : "");
  }
out:
  free(src);
  free(dst);
  free(xl);
  free(yl);
}

static void test_scale_copy(void) {
  /* the real cases (render -> view of the mode) and odd ones */
  static const int c[][6] = {
    { 640, 480, 0, 0, 720, 480 },   { 854, 480, 0, 0, 720, 480 },  { 768, 576, 0, 0, 720, 576 },
    { 1024, 576, 0, 0, 720, 576 },  { 320, 240, 0, 0, 720, 240 },  { 427, 240, 0, 0, 720, 240 },
    { 512, 384, 72, 48, 576, 384 }, { 640, 480, 160, 0, 960, 720 }, { 1280, 720, 0, 60, 720, 360 },
    { 7, 5, 3, 2, 13, 11 },         { 13, 11, 1, 1, 7, 5 },         { 1, 1, 0, 0, 9, 4 },
    { 9, 4, 5, 5, 1, 1 },           { 33, 17, 0, 0, 33, 17 },       { 3, 640, 2, 1, 5, 7 },
  };
  for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
    check_scale(c[i][0], c[i][1], c[i][2], c[i][3], c[i][4], c[i][5], 16, false);
    check_scale(c[i][0], c[i][1], c[i][2], c[i][3], c[i][4], c[i][5], 32, false);
    check_scale(c[i][0], c[i][1], c[i][2], c[i][3], c[i][4], c[i][5], 16, true);
    check_scale(c[i][0], c[i][1], c[i][2], c[i][3], c[i][4], c[i][5], 32, true);
  }
  for (int k = 0; k < 40; k++) {
    int sw = 1 + (int)(rnd() % 50), sh = 1 + (int)(rnd() % 50), dw = 1 + (int)(rnd() % 70), dh = 1 + (int)(rnd() % 70);
    check_scale(sw, sh, (int)(rnd() % 5), (int)(rnd() % 5), dw, dh, rnd() & 1 ? 16 : 32, (rnd() & 3) == 0);
  }
  /* 1:1 copies (no tables) of the same size */
  check_scale(640, 480, 0, 0, 640, 480, 16, false);
  check_scale(31, 7, 4, 3, 31, 7, 32, false);
  /* bad arguments write nothing */
  uint16_t s[4] = { 1, 2, 3, 4 }, d[16];
  memset(d, 0x77, sizeof d);
  video_scale_copy(NULL, 2, d, 8, 16, 0, 0, 2, 2, NULL, NULL);
  video_scale_copy(s, 2, NULL, 8, 16, 0, 0, 2, 2, NULL, NULL);
  video_scale_copy(s, 0, d, 8, 16, 0, 0, 2, 2, NULL, NULL);
  video_scale_copy(s, 2, d, 8, 24, 0, 0, 2, 2, NULL, NULL);
  video_scale_copy(s, 2, d, 8, 16, 0, 0, 0, 2, NULL, NULL);
  video_scale_copy(s, 2, d, 8, 16, 0, 0, 2, -1, NULL, NULL);
  video_scale_copy(s, 2, d, 8, 16, 0, 0, 3, 2, NULL, NULL);   /* 1:1 can't widen */
  bool untouched = true;
  for (int i = 0; i < 16; i++) untouched = untouched && d[i] == 0x7777;
  CHECK(untouched);
}

static void test_clear(void) {
  uint8_t b[10 * 7];
  memset(b, 0xEE, sizeof b);
  video_clear_buffer(b, 10, 16, 3, 7);             /* 6 bytes of each 10-byte row */
  bool ok = true;
  for (int y = 0; y < 7; y++)
    for (int x = 0; x < 10; x++) ok = ok && b[y * 10 + x] == (x < 6 ? 0 : 0xEE);
  CHECK(ok);
  memset(b, 0xEE, sizeof b);
  video_clear_buffer(b, 10, 32, 5, 7);             /* exactly the pitch: one memset */
  ok = true;
  for (size_t i = 0; i < sizeof b; i++) ok = ok && b[i] == 0;
  CHECK(ok);
  memset(b, 0xEE, sizeof b);
  video_clear_buffer(b, 10, 32, 50, 7);            /* wider than the pitch: clamped */
  ok = true;
  for (size_t i = 0; i < sizeof b; i++) ok = ok && b[i] == 0;
  CHECK(ok);
  memset(b, 0xEE, sizeof b);
  video_clear_buffer(b, 10, 16, 0, 7);
  video_clear_buffer(b, 0, 16, 3, 7);
  video_clear_buffer(NULL, 10, 16, 3, 7);
  video_clear_buffer(b, 10, 16, 3, 0);
  ok = true;
  for (size_t i = 0; i < sizeof b; i++) ok = ok && b[i] == 0xEE;
  CHECK(ok);
}

static void test_rgb888(void) {
  bool ok = true;
  for (uint32_t p = 0; p < 65536; p++) {
    uint8_t a[3], b[3];
    video_rgb565_to_rgb888((uint16_t)p, a);
    ref_rgb((uint16_t)p, b);
    ok = ok && !memcmp(a, b, 3);
  }
  CHECK(ok);
  uint8_t c[3];
  video_rgb565_to_rgb888(0xFFFF, c);
  CHECK(c[0] == 255 && c[1] == 255 && c[2] == 255);
  video_rgb565_to_rgb888(0x0000, c);
  CHECK(c[0] == 0 && c[1] == 0 && c[2] == 0);
  video_rgb565_to_rgb888(0xF800, c);
  CHECK(c[0] == 255 && c[1] == 0 && c[2] == 0);
  video_rgb565_to_rgb888(0x07E0, c);
  CHECK(c[0] == 0 && c[1] == 255 && c[2] == 0);
  video_rgb565_to_rgb888(0x001F, c);
  CHECK(c[0] == 0 && c[1] == 0 && c[2] == 255);
  video_rgb565_to_rgb888(0x0821, c);               /* the lowest step of each channel */
  CHECK(c[0] == 8 && c[1] == 4 && c[2] == 8);
  video_rgb565_to_rgb888(0x0841, c);               /* green 2 */
  CHECK(c[0] == 8 && c[1] == 8 && c[2] == 8);
  video_rgb565_to_rgb888(0x8410, c);               /* mid grey: 16,32,16 -> 132,130,132 */
  CHECK(c[0] == 132 && c[1] == 130 && c[2] == 132);
}

/* ================================================================ PNG */

static void fill_pattern(uint16_t *fb, int w, int h, int kind) {
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
      uint16_t v;
      switch (kind) {
      case 0: v = (uint16_t)rnd(); break;                                   /* noise: dynamic/stored blocks */
      case 1: v = (uint16_t)(((x * 31 / (w > 1 ? w - 1 : 1)) << 11) | ((y * 63 / (h > 1 ? h - 1 : 1)) << 5) | ((x + y) & 31)); break;
      case 2: v = 0; break;
      case 3: v = 0xFFFF; break;
      default: v = (uint16_t)(((x / 8 + y / 8) & 1) ? 0xF81F : 0x07E0); break;   /* checkerboard */
      }
      fb[(size_t)y * w + x] = v;
    }
}

static bool png_matches(const unsigned char *png, int len, const uint16_t *fb, int w, int h) {
  int dw = 0, dh = 0;
  const char *why = "";
  uint8_t *rgb = png_decode(png, (size_t)len, &dw, &dh, &why);
  if (!rgb) {
    fprintf(stderr, "png decode failed: %s\n", why);
    return false;
  }
  bool ok = dw == w && dh == h;
  for (int i = 0; ok && i < w * h; i++) {
    uint8_t c[3];
    ref_rgb(fb[i], c);
    ok = !memcmp(c, rgb + (size_t)i * 3, 3);
  }
  free(rgb);
  return ok;
}

static void test_png_encode(void) {
  static const int sz[][2] = { { 1, 1 }, { 2, 3 }, { 3, 2 }, { 17, 5 }, { 1, 200 }, { 200, 1 }, { 320, 240 }, { 640, 480 }, { 854, 480 } };
  for (size_t i = 0; i < sizeof sz / sizeof sz[0]; i++)
    for (int kind = 0; kind < 5; kind++) {
      int w = sz[i][0], h = sz[i][1];
      uint16_t *fb = malloc(sizeof(uint16_t) * (size_t)w * (size_t)h);
      if (!fb) { CHECK(!"memory"); continue; }
      fill_pattern(fb, w, h, kind);
      int len = -1;
      unsigned char *png = video_png_encode(fb, w, h, &len);
      CHECK(png && len > 57);                       /* signature + IHDR + IDAT + IEND */
      if (png) {
        t_count++;
        if (!png_matches(png, len, fb, w, h)) {
          t_fail++;
          fprintf(stderr, "png %dx%d kind %d doesn't round-trip\n", w, h, kind);
        }
      }
      free(png);
      free(fb);
    }
  /* len may be NULL */
  uint16_t px[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
  unsigned char *p = video_png_encode(px, 2, 2, NULL);
  CHECK(p != NULL);
  free(p);
  /* bad arguments: NULL and *len = 0 */
  int len = 99;
  CHECK(video_png_encode(NULL, 2, 2, &len) == NULL && len == 0);
  len = 99;
  CHECK(video_png_encode(px, 0, 2, &len) == NULL && len == 0);
  CHECK(video_png_encode(px, 2, -1, &len) == NULL);
  CHECK(video_png_encode(px, 70000, 1, &len) == NULL);
  CHECK(video_png_encode(px, 60000, 60000, &len) == NULL);   /* w*h*3 overflows an int */
}

static unsigned char *read_all(const char *path, int *len) {
  size_t n = 0;
  char *d = plat_read_file(path, &n, 64u << 20);
  *len = (int)n;
  return (unsigned char *)d;
}

static void test_snapshot(void) {
  int w = 31, h = 9;
  uint16_t fb[31 * 9];
  fill_pattern(fb, w, h, 1);
  char path[700];
  snprintf(path, sizeof path, "%s/snap.png", tmpdir);
  CHECK_INT(video_snapshot_png(fb, w, h, path), 0);
  int len = 0;
  unsigned char *png = read_all(path, &len);
  CHECK(png && png_matches(png, len, fb, w, h));
  free(png);
  /* overwrite an existing file (rename over it) */
  fill_pattern(fb, w, h, 4);
  CHECK_INT(video_snapshot_png(fb, w, h, path), 0);
  png = read_all(path, &len);
  CHECK(png && png_matches(png, len, fb, w, h));
  free(png);
  /* errors */
  char bad[700];
  snprintf(bad, sizeof bad, "%s/no/such/dir/snap.png", tmpdir);
  CHECK_INT(video_snapshot_png(fb, w, h, bad), -1);
  CHECK_INT(video_snapshot_png(fb, w, h, NULL), -1);
  CHECK_INT(video_snapshot_png(fb, w, h, ""), -1);
  CHECK_INT(video_snapshot_png(NULL, w, h, path), -1);
  CHECK_INT(video_snapshot_png(fb, 0, h, path), -1);
  plat_remove(path);
}

/* ================================================================ backends */

static void test_png_backend(void) {
  char dir[700];
  snprintf(dir, sizeof dir, "%s/frames/sub", tmpdir);   /* created on open (mkdir -p) */
  video_config_t c;
  memset(&c, 0, sizeof c);
  c.backend = "png";
  c.render_w = 64;
  c.render_h = 48;
  c.png_dir = dir;
  c.png_every = 2;
  c.log = quiet_log;
  video_info_t info;
  memset(&info, 0x5A, sizeof info);
  char err[160] = "x";
  video_t *v = video_open(&c, &info, err, sizeof err);
  CHECK(v != NULL);
  if (!v) return;
  CHECK_STR(err, "");
  CHECK_STR(info.backend, "png");
  CHECK(info.render_w == 64 && info.render_h == 48 && info.mode_w == 64 && info.mode_h == 48);
  CHECK(info.view_x == 0 && info.view_y == 0 && info.view_w == 64 && info.view_h == 48);
  CHECK(!info.interlaced && !info.composite && !info.hw_scaling);
  CHECK(strstr(info.desc, "every 2 frames") != NULL);
  CHECK(video_get_info(v) && video_get_info(v)->render_w == 64);
  CHECK_INT(video_fd(v), -1);
  CHECK(!video_pending(v));
  static uint16_t frames[5][64 * 48];
  for (int i = 0; i < 5; i++) {
    fill_pattern(frames[i], 64, 48, i);
    CHECK_INT(video_present(v, frames[i]), 0);
    CHECK(!video_pending(v));                       /* no display: shown at once */
    int64_t t = -1;
    CHECK_INT(video_wait(v, 0, &t), 0);
    CHECK(t > 0);
  }
  video_stats_t st;
  video_get_stats(v, &st);
  CHECK_INT(st.flips, 5);
  CHECK_INT(st.missed, 0);
  CHECK(!st.down);
  CHECK_INT(video_present(v, NULL), -1);
  CHECK_INT(video_debug(v, "reinit"), -1);          /* no test hooks here */
  video_close(v);
  for (int i = 0; i < 5; i++) {
    char p[800];
    snprintf(p, sizeof p, "%s/frame-%06d.png", dir, i);
    int len = 0;
    unsigned char *png = read_all(p, &len);
    if (i % 2 == 0) {
      CHECK(png != NULL);
      if (png) {
        t_count++;
        if (!png_matches(png, len, frames[i], 64, 48)) {
          t_fail++;
          fprintf(stderr, "frame %d file doesn't match\n", i);
        }
      }
    } else {
      CHECK(png == NULL);                           /* every 2nd frame only */
    }
    free(png);
  }
  rm_tree(dir);
  char up[700];
  snprintf(up, sizeof up, "%s/frames", tmpdir);
  plat_remove(up);

  /* png_every = 0: no files, default size 640x480 */
  memset(&c, 0, sizeof c);
  c.backend = "png";
  c.png_dir = dir;
  c.log = quiet_log;
  v = video_open(&c, &info, err, sizeof err);
  CHECK(v != NULL);
  if (v) {
    CHECK(info.render_w == 640 && info.render_h == 480);
    CHECK(strstr(info.desc, "on request") != NULL);
    static uint16_t fb[640 * 480];
    CHECK_INT(video_present(v, fb), 0);
    video_close(v);
    plat_stat_t s;
    CHECK(plat_stat(dir, &s) == 0 && !s.exists);    /* not even the directory */
  }
  /* bad sizes and directories */
  c.render_w = 100;
  c.render_h = 0;
  CHECK(video_open(&c, &info, err, sizeof err) == NULL && strstr(err, "bad render size"));
  c.render_w = 9000;
  c.render_h = 10;
  CHECK(video_open(&c, &info, err, sizeof err) == NULL);
  c.render_w = -5;
  c.render_h = -5;
  CHECK(video_open(&c, &info, err, sizeof err) == NULL);
  char longdir[1200];
  memset(longdir, 'a', sizeof longdir - 1);
  longdir[sizeof longdir - 1] = 0;
  c.render_w = c.render_h = 0;
  c.png_dir = longdir;
  CHECK(video_open(&c, &info, err, sizeof err) == NULL && strstr(err, "too long"));
  /* a file where the directory should be */
  char file[700];
  snprintf(file, sizeof file, "%s/afile", tmpdir);
  CHECK(plat_write_file_atomic(file, "x", 1) == 0);
  c.png_dir = file;
  c.png_every = 1;
  CHECK(video_open(&c, &info, err, sizeof err) == NULL && strstr(err, "can't create"));
  plat_remove(file);
}

static void test_none_backend(void) {
  video_config_t c;
  memset(&c, 0, sizeof c);
  c.backend = "none";
  c.render_w = 320;
  c.render_h = 240;
  video_info_t info;
  char err[64];
  video_t *v = video_open(&c, &info, err, sizeof err);
  CHECK(v != NULL);
  if (!v) return;
  CHECK_STR(info.backend, "none");
  CHECK(info.render_w == 320 && info.render_h == 240);
  CHECK_NEAR(info.hz, 60.0, 1e-9);
  CHECK_INT(video_period_us(v), 16667);
  CHECK_INT(video_fd(v), -1);
  CHECK(!video_pending(v));
  int64_t t = 0;
  CHECK_INT(video_wait(v, 0, &t), 0);               /* nothing queued: at once */
  static uint16_t fb[320 * 240];
  const int64_t P = 16667;

  /* a loop like main.c's: present, wait for the "vblank", sleep to the deadline */
  int64_t t0 = plat_now_us(), first_vb = 0, prev_vb = 0;
  bool grid_ok = true, deadline_ok = true, pend_ok = true, step_ok = true;
  const int N = 30;
  for (int i = 0; i < N; i++) {
    CHECK_INT(video_present(v, fb), 0);
    if (!video_pending(v)) pend_ok = false;
    int64_t vb = 0;
    int r = video_wait(v, 100, &vb);
    if (r != 0 || video_pending(v)) pend_ok = false;
    if (!first_vb) first_vb = vb;
    if ((vb - first_vb) % P) grid_ok = false;      /* every frame on the 60 Hz grid */
    if (prev_vb && vb - prev_vb != P) step_ok = false;
    prev_vb = vb;
    int64_t now = plat_now_us(), dl = video_deadline_us(v, now);
    if (dl <= now || dl > now + P || (dl - first_vb) % P) deadline_ok = false;
    spin_until(dl - 4000);                          /* "render" until 4 ms before the deadline */
  }
  int64_t el = plat_now_us() - t0;
  CHECK(pend_ok);
  CHECK(grid_ok);
  CHECK(deadline_ok);
  CHECK(step_ok);                                   /* one frame per refresh: none missed */
  if (!step_ok) fprintf(stderr, "none: frames were not one refresh apart\n");
  /* paced: N frames take N-1..N+ periods, not as fast as possible */
  t_count++;
  if (el < (N - 1) * P - 2000 || el > (N + 6) * P + 200000) {
    t_fail++;
    fprintf(stderr, "none: %d frames took %lld us (want about %lld)\n", N, (long long)el, (long long)(N * P));
  }
  video_stats_t st;
  video_get_stats(v, &st);
  CHECK_INT(st.flips, N);
  CHECK_INT(st.missed, 0);
  CHECK_INT(st.period_us, P);
  CHECK(!st.down && st.reinits == 0);

  /* a late frame: 3 refreshes without a present count as missed */
  CHECK_INT(video_present(v, fb), 0);
  CHECK_INT(video_wait(v, 100, &t), 0);
  int64_t before = t;
  uint32_t missed0 = st.missed;
  spin_until(before + 3 * P + P / 2);               /* 3.5 refreshes after the last shown frame */
  CHECK_INT(video_present(v, fb), 0);
  CHECK_INT(video_wait(v, 100, &t), 0);
  video_get_stats(v, &st);
  CHECK_INT(st.missed - missed0, 3);                /* shown at the 4th refresh: 3 repeats */
  CHECK_INT(t - before, 4 * P);

  /* wait with timeout 0 / short timeouts: "not yet" */
  CHECK_INT(video_present(v, fb), 0);
  CHECK(video_pending(v));
  CHECK_INT(video_wait(v, 0, &t), 1);
  CHECK(video_pending(v));
  int64_t w0 = plat_now_us();
  int r = video_wait(v, 1, &t);
  int64_t w1 = plat_now_us();
  CHECK(r == 1 || r == 0);                          /* 0 if the tick was within the 1 ms */
  CHECK(w1 - w0 < 8000);
  CHECK_INT(video_wait(v, 100, &t), 0);
  CHECK(!video_pending(v));
  /* two presents in a row: the second waits for the first frame's tick (bounded, <= 1 period) */
  CHECK_INT(video_present(v, fb), 0);
  int64_t p0 = plat_now_us();
  CHECK_INT(video_present(v, fb), 0);
  int64_t p1 = plat_now_us();
#ifdef _WIN32
  CHECK(p1 - p0 <= P + 17000);                      /* the wait inside uses Sleep(): +15.6 ms */
#else
  CHECK(p1 - p0 <= P + 3000);
#endif
  CHECK_INT(video_wait(v, 100, &t), 0);
  CHECK(video_wait(v, 100, NULL) == 0);             /* t_us may be NULL */
  video_close(v);

  /* default size; nonsense sizes fall back to 640x480 */
  c.render_w = 0;
  c.render_h = 0;
  v = video_open(&c, &info, NULL, 0);
  CHECK(v && info.render_w == 640 && info.render_h == 480);
  video_close(v);
  c.render_w = 100000;
  c.render_h = 5;
  v = video_open(&c, &info, NULL, 0);
  CHECK(v && info.render_w == 640 && info.render_h == 480);
  video_close(v);
  /* info may be NULL */
  v = video_open(&c, NULL, NULL, 0);
  CHECK(v != NULL);
  video_close(v);
}

static void test_dispatch(void) {
  video_config_t c;
  memset(&c, 0, sizeof c);
  c.log = quiet_log;
  char err[200];
  video_info_t info;
  c.backend = "vga";
  CHECK(video_open(&c, &info, err, sizeof err) == NULL && strstr(err, "unknown video backend 'vga'"));
  /* the null-safe API */
  CHECK_INT(video_present(NULL, NULL), -1);
  CHECK_INT(video_wait(NULL, 0, NULL), -1);
  CHECK(!video_pending(NULL));
  CHECK_INT(video_fd(NULL), -1);
  CHECK(video_get_info(NULL) == NULL);
  CHECK_INT(video_period_us(NULL), 16667);
  CHECK_INT(video_deadline_us(NULL, 1000), 1000 + 16667);
  CHECK_INT(video_debug(NULL, "reinit"), -1);
  video_stats_t st;
  memset(&st, 0x33, sizeof st);
  video_get_stats(NULL, &st);
  CHECK(st.flips == 0 && st.period_us == 0 && !st.down);
  video_close(NULL);
  /* device backends that can't open give an error, never a crash */
  c.card_wait_ms = -1;
  c.backend = "drm";
  c.device = "/dev/dri/card99";
  err[0] = 0;
  CHECK(video_open(&c, &info, err, sizeof err) == NULL && err[0]);
  c.backend = "fb";
  c.device = "/dev/fb99";
  err[0] = 0;
  CHECK(video_open(&c, &info, err, sizeof err) == NULL && err[0]);
  c.backend = "fbdev";
  err[0] = 0;
  CHECK(video_open(&c, &info, err, sizeof err) == NULL && err[0]);
#ifndef __linux__
  /* no Linux: auto ends at "none" (with the reasons), so the loop still runs */
  c.backend = NULL;
  c.device = NULL;
  err[0] = 0;
  video_t *v = video_open(&c, &info, err, sizeof err);
  CHECK(v != NULL);
  CHECK_STR(info.backend, "none");
  CHECK(strstr(info.desc, "no display (drm:") != NULL);
  CHECK(strstr(err, "drm: DRM/KMS needs Linux") != NULL && strstr(err, "fb: fbdev needs Linux") != NULL);
  video_close(v);
  v = video_open(NULL, &info, NULL, 0);            /* no config at all */
  CHECK(v && !strcmp(info.backend, "none"));
  video_close(v);
#endif
}

int main(void) {
  make_tmpdir();
  test_layout_table();
  test_layout_sweep();
  test_layout_bad();
  test_parse_mode();
  test_mode_helpers();
  test_composite_modes();
  test_hdmi_modes();
  test_luts();
  test_scale_copy();
  test_clear();
  test_rgb888();
  test_png_encode();
  test_snapshot();
  test_png_backend();
  test_none_backend();
  test_dispatch();
  rm_tree(tmpdir);
  T_DONE();
}
