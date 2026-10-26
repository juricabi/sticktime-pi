/* etx: the EdgeTX Lua API. Lua fixtures from host/tests/lua run in a temporary SD folder under
 * build/; when the sibling repo is checked out, the real StickTime.lua is loaded at 640x480,
 * walked through its menus, flown on every track and raced to the finish on track 1 (the
 * autopilot of sticktime/test/harness.lua). Run from the repository root. */
#include "t.h"
#include "etx.h"
#include "font.h"
#include "lcd.h"
#include "plat.h"

#include <math.h>
#include <stdint.h>

#define TMP "build/test-etx"
#define SD TMP "/sd"
#define GAME_SD TMP "/game"
#define FIX "host/tests/lua/"
#define GAME_SRC "../sticktime/sdcard/SCRIPTS/TOOLS/StickTime.lua"
#define GAME "/SCRIPTS/TOOLS/StickTime.lua"
#define TRACKS "/SCRIPTS/TOOLS/StickTimeTracks/c"

/* ------------------------------------------------------------------ log capture */
static plat_mutex log_mu;
static char logbuf[1 << 16];
static size_t loglen;

static void logcb(void *ud, const char *line) {
  (void)ud;
  size_t n = strlen(line);
  plat_mutex_lock(&log_mu);
  if (loglen + n + 2 < sizeof logbuf) {
    memcpy(logbuf + loglen, line, n);
    loglen += n;
    logbuf[loglen++] = '\n';
    logbuf[loglen] = 0;
  }
  plat_mutex_unlock(&log_mu);
  if (!strncmp(line, "lua: FAIL", 9)) fprintf(stderr, "  %s\n", line);
}

static bool log_has(const char *s) {
  plat_mutex_lock(&log_mu);
  bool r = strstr(logbuf, s) != NULL;
  plat_mutex_unlock(&log_mu);
  return r;
}

static void log_clear(void) {
  plat_mutex_lock(&log_mu);
  loglen = 0;
  logbuf[0] = 0;
  plat_mutex_unlock(&log_mu);
}

/* ------------------------------------------------------------------ files */
static void rm_rf(const char *path);
static int rm_cb(void *ud, const char *name, bool is_dir, int64_t size, int64_t mtime) {
  (void)size; (void)mtime;
  char p[1024];
  snprintf(p, sizeof p, "%s/%s", (const char *)ud, name);
  if (is_dir) rm_rf(p); else plat_remove(p);
  return 0;
}
static void rm_rf(const char *path) {
  plat_list_dir(path, rm_cb, (void *)path);
  plat_remove(path);
}

static void real_path(const char *root, const char *sdpath, char *out, size_t n) {
  snprintf(out, n, "%s%s", root, sdpath);
}

static void sd_write_in(const char *root, const char *sdpath, const char *data, size_t len) {
  char p[1024], dir[1024];
  real_path(root, sdpath, p, sizeof p);
  plat_strlcpy(dir, p, sizeof dir);
  *strrchr(dir, '/') = 0;
  plat_mkdirs(dir);
  CHECK_INT(plat_write_file_atomic(p, data, len), 0);
}
static void sd_write(const char *sdpath, const char *s) { sd_write_in(SD, sdpath, s, strlen(s)); }
static void game_write(const char *sdpath, const char *s) { sd_write_in(GAME_SD, sdpath, s, strlen(s)); }

static char *read_in(const char *root, const char *sdpath, size_t *len) {
  char p[1024];
  real_path(root, sdpath, p, sizeof p);
  size_t n = 0;
  char *s = plat_read_file(p, &n, 1 << 24);
  if (len) *len = n;
  return s;
}

static bool exists_in(const char *root, const char *sdpath) {
  char p[1024];
  plat_stat_t st;
  real_path(root, sdpath, p, sizeof p);
  return plat_stat(p, &st) == 0 && st.exists;
}

static void remove_in(const char *root, const char *sdpath) {
  char p[1024];
  real_path(root, sdpath, p, sizeof p);
  plat_remove(p);
}

static bool file_is(const char *root, const char *sdpath, const char *want) {
  size_t n;
  char *s = read_in(root, sdpath, &n);
  bool ok = s && n == strlen(want) && !memcmp(s, want, n);
  if (!ok) fprintf(stderr, "  %s%s: \"%s\", want \"%s\"\n", root, sdpath, s ? s : "(missing)", want);
  free(s);
  return ok;
}

static int copy_cb(void *ud, const char *name, bool is_dir, int64_t size, int64_t mtime) {
  (void)ud; (void)size; (void)mtime;
  size_t n = strlen(name);
  if (is_dir || n < 5 || strcmp(name + n - 4, ".lua")) return 0;
  char src[512], dst[512];
  snprintf(src, sizeof src, FIX "%s", name);
  snprintf(dst, sizeof dst, "/SCRIPTS/TESTS/%s", name);
  size_t len = 0;
  char *s = plat_read_file(src, &len, 1 << 20);
  CHECK(s != NULL);
  if (s) sd_write_in(SD, dst, s, len);
  free(s);
  return 0;
}

/* ------------------------------------------------------------------ helpers */
static fonts_t *stub_fonts;
static lcd_t L640;

static uint16_t px(int x, int y) { return L640.fb[y * L640.w + x]; }

static etx_config_t cfg_for(const char *script) {
  etx_config_t c;
  memset(&c, 0, sizeof c);
  c.sd_root = SD;
  c.script = script;
  c.test_hooks = true;
  c.lat = -1;
  c.stick_mode = 3;
  c.watchdog_ms = 300;
  return c;
}

static char script_buf[256];
static etx_config_t cfg_test(const char *name) {
  snprintf(script_buf, sizeof script_buf, "/SCRIPTS/TESTS/%s", name);
  return cfg_for(script_buf);
}

static etx_t *open_etx(lcd_t *l, const etx_config_t *c, int want) {
  etx_t *e = etx_new(l, c);
  CHECK(e != NULL);
  etx_set_log(e, logcb, NULL);
  int r = etx_load(e);
  CHECK_INT(r, want);
  if (r != want) fprintf(stderr, "  load %s: %s\n", c->script, etx_error(e));
  return e;
}

static etx_t *load_test(const char *name, bool async) {
  etx_config_t c = cfg_test(name);
  c.async_writes = async;
  /* io moves megabytes: give slow machines (an emulated arm64 VM) time; hang tests keep 300 ms */
  if (!strcmp(name, "io.lua")) c.watchdog_ms = 20000;
  return open_etx(&L640, &c, 0);
}

/* the fixture's own checks: T.result() -> fails, total */
static void lua_checks(etx_t *e, const char *fn, const char *what) {
  double r[2] = { -1, -1 };
  int n = etx_test_call(e, fn, NULL, 0, r, 2);
  CHECK_INT(n, 2);
  if (n != 2) { fprintf(stderr, "  %s: %s\n", what, etx_test_error(e)); return; }
  CHECK_INT(r[0], 0);
  CHECK(r[1] >= 10);
  printf("  %s: %d checks in Lua, %d failed\n", what, (int)r[1], (int)r[0]);
}

static double call1(etx_t *e, const char *fn, double a) {
  double out = NAN;
  etx_test_call(e, fn, &a, 1, &out, 1);
  return out;
}

static int call_str(etx_t *e, const char *fn, double a, char *buf, size_t n) {
  return etx_test_call_str(e, fn, &a, 1, 0, buf, n) == 0 ? (int)strlen(buf) : -1;
}

static bool starts(const char *s, const char *p) { return s && !strncmp(s, p, strlen(p)); }

/* ================================================================== SD paths */
static void test_paths(void) {
  etx_config_t c = cfg_for("/x.lua");
  c.sd_root = "root";
  etx_t *e = etx_new(NULL, &c);
  char out[64];
  static const struct { const char *in, *out; } ok[] = {
    { "/SCRIPTS/x.lua", "root/SCRIPTS/x.lua" }, { "SCRIPTS/x.lua", "root/SCRIPTS/x.lua" },
    { "//SCRIPTS///x.lua/", "root/SCRIPTS/x.lua" }, { "/a/./b", "root/a/b" }, { "./a", "root/a" },
    { "\\a\\b", "root/a/b" }, { "/", "root" }, { "", "root" }, { "/a/.hidden", "root/a/.hidden" },
    { "/a/b..c", "root/a/b..c" }, { "/a/..b", "root/a/..b" }, { "/\xC4\x8C.txt", "root/\xC4\x8C.txt" },
  };
  for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
    CHECK_INT(etx_sd_path(e, ok[i].in, out, sizeof out), 0);
    CHECK_STR(out, ok[i].out);
  }
  static const char *bad[] = { "/a/../b", "..", "/..", "/a/...", "/a/.. /b", "/a/ /b", "/a/. ./b", "C:/x",
    "/a:b", ("/a\x01" "b"), "/a\x7f", "\\..\\x", "/a/..\\..\\x", " ..", ".. " };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    strcpy(out, "junk");
    CHECK_INT(etx_sd_path(e, bad[i], out, sizeof out), -1);
    CHECK_STR(out, "");
  }
  CHECK_INT(etx_sd_path(e, "/SCRIPTS/x.lua", out, 8), -1);         /* too small a buffer */
  CHECK_STR(out, "");
  CHECK_INT(etx_sd_path(e, "/SCRIPTS/x.lua", out, 19), 0);         /* exactly fits */
  CHECK_INT(etx_sd_path(e, "/SCRIPTS/x.lua", out, 18), -1);
  char lng[1500];
  memset(lng, 'a', sizeof lng - 1);
  lng[sizeof lng - 1] = 0;
  char big[2048];
  CHECK_INT(etx_sd_path(e, lng, big, sizeof big), -1);
  CHECK_INT(etx_sd_path(e, NULL, out, sizeof out), -1);
  etx_free(e);

  static const struct { const char *root, *out; } roots[] = {
    { "root/", "root/a" }, { "/", "/a" }, { "C:/sd/", "C:/sd/a" }, { "", "./a" }, { "r//", "r/a" },
  };
  for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++) {
    c.sd_root = roots[i].root;
    e = etx_new(NULL, &c);
    CHECK_INT(etx_sd_path(e, "/a", out, sizeof out), 0);
    CHECK_STR(out, roots[i].out);
    etx_free(e);
  }
  c.sd_root = NULL;
  e = etx_new(NULL, &c);
  CHECK_INT(etx_sd_path(e, "/a", out, sizeof out), 0);
  CHECK_STR(out, "./a");
  etx_free(e);
  etx_free(NULL);
}

/* ================================================================== API semantics */
static void test_api(void) {
  etx_t *e = load_test("api.lua", false);
  lua_checks(e, "result", "api");

  /* getValue: by id or name, floored ids, clamped values, integers */
  double v[16];
  CHECK_INT(etx_test_call(e, "vals", NULL, 0, v, 16), 14);
  CHECK_INT(v[0], 0);
  CHECK_INT(v[2], -1024);                         /* before any input: throttle low */
  etx_set_sticks(e, 100, -200, 2000, -3000);
  CHECK_INT(etx_test_call(e, "vals", NULL, 0, v, 16), 14);
  static const int want[14] = { 100, -200, 1024, -1024, 100, -200, 1024, -1024, 0, 0, 0, 0, 0, 0 };
  for (int i = 0; i < 14; i++) CHECK_INT(v[i], want[i]);
  CHECK_INT(etx_test_call(e, "vtypes", NULL, 0, v, 2), 2);
  CHECK(v[0] == 1 && v[1] == 1);

  /* getTime: clock / 10, floored */
  static const struct { int64_t ms; int t; } times[] = { { 0, 0 }, { 9, 0 }, { 10, 1 }, { 12345, 1234 }, { -5, -1 },
    { -10, -1 }, { -11, -2 }, { 1000LL * 3600 * 24 * 30, 259200000 } };
  for (size_t i = 0; i < sizeof times / sizeof times[0]; i++) {
    etx_set_clock_ms(e, times[i].ms);
    CHECK_INT(call1(e, "time", 0), times[i].t);
  }

  /* playTone / playHaptic */
  etx_tone_t t[64];
  CHECK_INT(etx_take_tones(e, t, 64), 0);
  CHECK_INT(etx_test_call(e, "tones", NULL, 0, NULL, 0), 0);
  CHECK_INT(etx_take_tones(e, t, 64), 5);
  CHECK(!t[0].haptic && t[0].freq == 440 && t[0].len_ms == 100 && t[0].pause_ms == 20 && t[0].flags == 0x10 &&
        t[0].freq_incr == 5 && t[0].volume == 3);
  CHECK(!t[1].haptic && t[1].freq == 880 && t[1].len_ms == 50 && t[1].pause_ms == 0 && t[1].flags == 0 &&
        t[1].freq_incr == 0 && t[1].volume == 0);
  CHECK(t[2].haptic && t[2].len_ms == 30 && t[2].pause_ms == 10);
  CHECK(t[3].haptic && t[3].len_ms == 15 && t[3].pause_ms == 0);
  CHECK_INT(t[4].flags, (int)0xFFFF8000u);
  CHECK_INT(etx_take_tones(e, t, 64), 0);
  etx_test_call(e, "tones", NULL, 0, NULL, 0);
  CHECK_INT(etx_take_tones(e, t, 2), 2);             /* the rest stays queued */
  CHECK_INT(t[1].freq, 880);
  CHECK_INT(etx_take_tones(e, t, 0), 0);
  CHECK_INT(etx_take_tones(e, t, 64), 3);
  CHECK(t[0].haptic);
  double n40 = 40;
  etx_test_call(e, "flood", &n40, 1, NULL, 0);         /* more than the queue: the newest stay */
  CHECK_INT(etx_take_tones(e, t, 64), ETX_TONE_QUEUE);
  CHECK_INT(t[0].freq, 40 - ETX_TONE_QUEUE + 1);
  CHECK_INT(t[ETX_TONE_QUEUE - 1].freq, 40);

  /* print goes to the log */
  log_clear();
  etx_test_call(e, "say", NULL, 0, NULL, 0);
  CHECK(log_has("lua: a\t1\tnil\ttrue\t2.5\n"));

  /* drawing: floored coordinates, uint32 flags, opacity */
  const uint16_t red = lcd_rgb565(229, 32, 30), white = 0xFFFF;
  const double RED = 17 << 16, WHITE = 12 << 16;
  double a[8];
  etx_test_call(e, "clear0", NULL, 0, NULL, 0);
  CHECK_INT(px(0, 0), lcd_rgb565(228, 238, 242));     /* lcd.clear() = named color 5 */
  a[0] = 0;
  etx_test_call(e, "clear", a, 1, NULL, 0);
  CHECK_INT(px(0, 0), 0);
  CHECK_INT(px(639, 479), 0);
  double r1[6] = { 10.7, 20.2, 5.9, 3.5, RED, 0 };
  etx_test_call(e, "rect", r1, 6, NULL, 0);
  CHECK_INT(px(10, 20), red); CHECK_INT(px(14, 20), red); CHECK_INT(px(14, 22), red);
  CHECK_INT(px(15, 20), 0); CHECK_INT(px(9, 20), 0); CHECK_INT(px(10, 23), 0); CHECK_INT(px(10, 19), 0);
  double r2[6] = { -0.5, 30, 3, 1, RED, 0 };            /* floor(-0.5) = -1: one column off screen */
  etx_test_call(e, "rect", r2, 6, NULL, 0);
  CHECK_INT(px(0, 30), red); CHECK_INT(px(1, 30), red); CHECK_INT(px(2, 30), 0);
  /* flags with bit 31: integers keep their bits, floats beyond int32 wrap like engine.js */
  double r3[6] = { 0, 0, 1, 1, 4294934528.0, 0 };       /* 0xFFFF8000 as a float */
  etx_test_call(e, "rect", r3, 6, NULL, 0);
  CHECK_INT(px(0, 0), white);
  double r4[6] = { 1, 0, 1, 1, -32768, 0 };             /* the same bits as an int32 */
  etx_test_call(e, "rect", r4, 6, NULL, 0);
  CHECK_INT(px(1, 0), white);
  a[0] = 2; a[1] = 0;
  etx_test_call(e, "white", a, 2, NULL, 0);
  CHECK_INT(px(2, 0), white);
  a[0] = 3;
  etx_test_call(e, "shadowed", a, 2, NULL, 0);          /* SMLSIZE + RGB(0,0,255) + SHADOWED */
  CHECK_INT(px(3, 0), 0x001F);
  double r5[6] = { 4, 0, 1, 1, NAN, 0 };                /* NaN / inf flags: color 0 */
  a[0] = RED;
  etx_test_call(e, "clear", a, 1, NULL, 0);
  etx_test_call(e, "rect", r5, 6, NULL, 0);
  CHECK_INT(px(4, 0), 0);
  r5[0] = 5; r5[4] = INFINITY;
  etx_test_call(e, "rect", r5, 6, NULL, 0);
  CHECK_INT(px(5, 0), 0);
  r5[0] = 6; r5[4] = -4294934528.0 * 3;                  /* far below int32: wraps */
  etx_test_call(e, "rect", r5, 6, NULL, 0);
  CHECK_INT(px(6, 0), lcd_color_of((uint32_t)(int64_t)fmod(floor(-4294934528.0 * 3), 4294967296.0)));
  /* opacity: floored, & 15 */
  a[0] = 0;
  etx_test_call(e, "clear", a, 1, NULL, 0);
  double o1[6] = { 0, 0, 1, 1, WHITE, 15.9 };
  etx_test_call(e, "rect", o1, 6, NULL, 0);
  CHECK_INT(px(0, 0), 0);                               /* 15: invisible */
  double o2[6] = { 1, 0, 1, 1, WHITE, 16 };
  etx_test_call(e, "rect", o2, 6, NULL, 0);
  CHECK_INT(px(1, 0), white);                           /* 16 & 15 = 0: opaque */
  double o3[6] = { 2, 0, 1, 1, WHITE, 7.5 };
  etx_test_call(e, "rect", o3, 6, NULL, 0);
  CHECK_INT(px(2, 0), lcd_blend565(0, white, 136));     /* round((15 - 7) * 255 / 15) */
  /* lines: an end beyond W/H rejects the whole line; == W is drawn (clipped) */
  memset(&L640.st, 0, sizeof L640.st);
  double l1[6] = { 0, 1, 641, 10, 255, WHITE };
  etx_test_call(e, "line", l1, 6, NULL, 0);
  CHECK_INT(L640.st.rejected, 1);
  CHECK_INT(px(0, 1), 0);
  double l2[6] = { 0, 2, 640, 2, 255, WHITE };
  etx_test_call(e, "line", l2, 6, NULL, 0);
  CHECK_INT(L640.st.rejected, 1);
  CHECK_INT(px(0, 2), white); CHECK_INT(px(639, 2), white);
  double l3[6] = { 0.9, 5.9, 10.9, 5.9, 255.9, WHITE };
  etx_test_call(e, "line", l3, 6, NULL, 0);
  CHECK_INT(px(0, 5), white); CHECK_INT(px(10, 5), white); CHECK_INT(px(11, 5), 0); CHECK_INT(px(0, 6), 0);
  double l4[6] = { 0, 8, 7, 8, 0x55, WHITE };             /* DOTTED */
  etx_test_call(e, "line", l4, 6, NULL, 0);
  CHECK_INT(px(0, 8), white); CHECK_INT(px(1, 8), 0); CHECK_INT(px(2, 8), white);
  double l5[6] = { 0, 9, 7, 9, 0x155, WHITE };            /* pattern & 255 */
  etx_test_call(e, "line", l5, 6, NULL, 0);
  CHECK_INT(px(0, 9), white); CHECK_INT(px(1, 9), 0);
  double p1[3] = { 5.7, 6.2, WHITE };
  etx_test_call(e, "point", p1, 3, NULL, 0);
  CHECK_INT(px(5, 6), white);
  double p2[3] = { 640, 480, WHITE };
  etx_test_call(e, "point", p2, 3, NULL, 0);
  double p3[3] = { -1, -1, WHITE };
  etx_test_call(e, "point", p3, 3, NULL, 0);
  double t1[8] = { 0, 100, 10, 100, 0, 110, WHITE, 0 };
  etx_test_call(e, "tri", t1, 8, NULL, 0);
  CHECK_INT(px(0, 100), white); CHECK_INT(px(1, 101), white); CHECK_INT(px(10, 109), 0);
  double tx[3] = { 1.9, 200.9, (double)(0x01 | (12 << 16)) };   /* INVERS: a box behind the text */
  etx_test_call(e, "text", tx, 3, NULL, 0);
  CHECK_INT(px(0, 200), white);
  CHECK_INT(etx_test_call(e, "size", NULL, 0, v, 2), 2);
  CHECK_INT(v[0], 50); CHECK_INT(v[1], 27);
  a[0] = 0x300;
  CHECK_INT(etx_test_call(e, "size", a, 1, v, 2), 2);
  CHECK_INT(v[1], 23);
  a[0] = -31872;                                        /* SMLSIZE + white + SHADOWED as an int32 */
  CHECK_INT(etx_test_call(e, "size", a, 1, v, 2), 2);
  CHECK_INT(v[1], 23);
  double rgb[3] = { 255, 255, 255 };
  CHECK_INT(etx_test_call(e, "rgb", rgb, 3, v, 1), 1);
  CHECK_INT(v[0], -32768);
  a[0] = 0x123456;
  CHECK_INT(etx_test_call(e, "rgb1", a, 1, v, 1), 1);
  CHECK_INT(v[0], 0x11AA8000);

  /* a reload empties the tone queue */
  etx_test_call(e, "tones", NULL, 0, NULL, 0);
  CHECK_INT(etx_load(e), 0);
  CHECK_INT(etx_take_tones(e, t, 64), 0);
  etx_free(e);
}

/* ================================================================== io */
static void test_io(void) {
  /* synchronous writes (desktop, tests) */
  etx_t *e = load_test("io.lua", false);
  lua_checks(e, "selftest", "io (sync)");
  CHECK(log_has("file too large"));
  double r[3];
  CHECK_INT(etx_test_call(e, "many", NULL, 0, r, 3), 3);
  CHECK_INT(r[0], 16); CHECK_INT(r[1], 1); CHECK_INT(r[2], 1);
  etx_test_call(e, "closeall", NULL, 0, NULL, 0);
  CHECK_INT(etx_test_call(e, "many", NULL, 0, r, 3), 3);   /* the collected handles freed their slots */
  CHECK_INT(r[0], 16);
  etx_test_call(e, "closeall", NULL, 0, NULL, 0);
  double kv[2] = { 1, 5 };
  double k0[2] = { 0, 1 };                                 /* PATHS[0] is nil: io.open(nil) raises */
  CHECK_INT(etx_test_call(e, "put", k0, 2, r, 1), -1);
  CHECK(strstr(etx_test_error(e), "bad argument #1 to 'open'") != NULL);
  etx_test_call(e, "put", kv, 2, r, 1);
  CHECK_INT(r[0], 1);
  CHECK(file_is(SD, "/IO/a.txt", "v=5"));                  /* on disk at io.close */
  kv[0] = 3; kv[1] = 7;
  etx_test_call(e, "put", kv, 2, r, 1);
  CHECK(file_is(SD, "/IO/new/deep/dir/c.txt", "v=7"));     /* folders made on the way */
  kv[0] = 4; kv[1] = 1;
  etx_test_call(e, "drop", kv, 2, NULL, 0);
  CHECK(!exists_in(SD, "/IO/d.txt"));                      /* dropped without io.close */
  double k4 = 4;
  CHECK_INT(etx_test_call(e, "half", &k4, 1, NULL, 0), -1);
  CHECK(strstr(etx_test_error(e), "boom in the middle of a save") != NULL);
  etx_test_call(e, "closeall", NULL, 0, NULL, 0);
  CHECK(!exists_in(SD, "/IO/d.txt"));                      /* never half-written */
  CHECK(!exists_in(TMP, "/evil.txt"));
  CHECK(!exists_in("build", "/evil.txt"));
  CHECK(!exists_in(SD, "/evil.txt"));
  /* a file where the folder should be: the write fails and says so */
  sd_write("/IO/blocker", "file");
  kv[0] = 6; kv[1] = 1;
  log_clear();
  etx_test_call(e, "put", kv, 2, r, 1);
  CHECK(log_has("can't write"));
  CHECK_INT(call1(e, "has", 6), 0);
  etx_free(e);

  /* writes on the writer thread, held so the pending state can be seen */
  remove_in(SD, "/IO/new_a.txt");                          /* (the selftest creates it with "a") */
  e = load_test("io.lua", true);
  lua_checks(e, "selftest", "io (async)");
  etx_flush_writes(e);
  etx_writer_hold(true);
  kv[0] = 2; kv[1] = 11;
  etx_test_call(e, "put", kv, 2, r, 1);
  CHECK(!exists_in(SD, "/IO/b.txt"));                       /* still pending */
  char buf[256];
  CHECK_INT(call_str(e, "get", 2, buf, sizeof buf), 4);    /* but readable */
  CHECK_STR(buf, "v=11");
  kv[1] = 12;
  etx_test_call(e, "put", kv, 2, r, 1);                    /* coalesced: the newest wins */
  CHECK_INT(call_str(e, "get", 2, buf, sizeof buf), 4);
  CHECK_STR(buf, "v=12");
  kv[0] = 1; kv[1] = 100;
  etx_test_call(e, "put", kv, 2, r, 1);
  etx_flush_writes(e);                                     /* returns while held */
  CHECK(!exists_in(SD, "/IO/b.txt"));
  /* a reload (a new etx_t) still sees the pending writes */
  etx_free(e);
  e = load_test("io.lua", true);
  CHECK_INT(call1(e, "has", 2), 1);
  CHECK_INT(call_str(e, "get", 2, buf, sizeof buf), 4);
  CHECK_STR(buf, "v=12");
  CHECK_INT(call_str(e, "get", 1, buf, sizeof buf), 5);
  CHECK_STR(buf, "v=100");
  /* append on top of a pending write */
  kv[0] = 1; kv[1] = 7;
  CHECK_INT(etx_test_call(e, "app", kv, 2, r, 1), 1);
  CHECK_INT(r[0], 1);
  CHECK_INT(call_str(e, "get", 1, buf, sizeof buf), 7);
  CHECK_STR(buf, "v=100+7");
  CHECK(file_is(SD, "/IO/a.txt", "v=5"));                  /* the disk still has the sync phase's */
  kv[1] = 100;
  etx_test_call(e, "put", kv, 2, r, 1);
  etx_writer_hold(false);
  etx_flush_writes(e);
  CHECK(file_is(SD, "/IO/b.txt", "v=12"));
  CHECK(file_is(SD, "/IO/a.txt", "v=100"));
  kv[1] = 5;
  etx_test_call(e, "app", kv, 2, r, 1);                    /* and on top of the file on disk */
  etx_flush_writes(e);
  CHECK(file_is(SD, "/IO/a.txt", "v=100+5"));
  /* a burst while the writer runs */
  for (int i = 0; i < 200; i++) {
    double w[2] = { 1 + (i % 2), i };
    etx_test_call(e, "put", w, 2, NULL, 0);
    if (i % 2) {
      snprintf(buf, sizeof buf, "v=%d", i);
      char got[64];
      call_str(e, "get", 2, got, sizeof got);
      CHECK_STR(got, buf);                                 /* every read sees the newest */
    }
  }
  etx_flush_writes(e);
  CHECK(file_is(SD, "/IO/a.txt", "v=198"));
  CHECK(file_is(SD, "/IO/b.txt", "v=199"));
  /* missing folders and a failing write on the writer thread */
  kv[0] = 3; kv[1] = 8;
  etx_test_call(e, "put", kv, 2, r, 1);
  log_clear();
  kv[0] = 6;
  etx_test_call(e, "put", kv, 2, r, 1);
  etx_flush_writes(e);
  CHECK(file_is(SD, "/IO/new/deep/dir/c.txt", "v=8"));
  CHECK(log_has("can't write"));
  CHECK_INT(call1(e, "has", 6), 0);                        /* the failed write is not cached */
  kv[0] = 4; kv[1] = 2;
  etx_test_call(e, "drop", kv, 2, NULL, 0);
  etx_flush_writes(e);
  CHECK(!exists_in(SD, "/IO/d.txt"));
  /* no temp files left behind */
  CHECK(!exists_in(SD, "/IO/a.txt.tmp~"));
  CHECK(!exists_in(SD, "/IO/b.txt.tmp~"));
  etx_free(e);
}

/* ================================================================== loadScript */
static void test_loadscript(void) {
  etx_t *e = load_test("loadscript.lua", false);
  lua_checks(e, "result", "loadScript");
  etx_free(e);
}

/* ================================================================== errors */
static void test_errors(void) {
  static const struct { const char *file, *prefix, *contains; } cases[] = {
    { "err_syntax.lua", "Syntax error: err_syntax.lua:1:", NULL },
    { "err_load.lua", "Script error: err_load.lua:2: attempt to index a nil value", "stack traceback:" },
    { "err_table.lua", "Script error: script must return a table with init/run", NULL },
    { "err_norun.lua", "Script error: script must return a table with init/run", NULL },
    { "err_init.lua", "init() error: err_init.lua:2: init boom", "stack traceback:" },
    { "hang_load.lua", "Script error: hang_load.lua:1: script took too long", NULL },
    { "hang_init.lua", "init() error: hang_init.lua:1: script took too long", "stack traceback:" },
    { "missing.lua", "Script error: /SCRIPTS/TESTS/missing.lua: file not found", NULL },
    { "../../x.lua", "Script error: /SCRIPTS/TESTS/../../x.lua: file not found", NULL },
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    etx_config_t c = cfg_test(cases[i].file);
    int64_t t0 = plat_now_us();
    etx_t *e = open_etx(&L640, &c, -1);
    double s = (plat_now_us() - t0) / 1e6;
    const char *err = etx_error(e);
    CHECK(starts(err, cases[i].prefix));
    if (!starts(err, cases[i].prefix)) fprintf(stderr, "  %s: \"%s\"\n", cases[i].file, err);
    if (cases[i].contains) CHECK(strstr(err, cases[i].contains) != NULL);
    CHECK(s < 1.5);
    CHECK(!etx_loaded(e));
    CHECK_INT(etx_run(e, 0), -1);
    etx_free(e);
  }

  /* run() errors: the message, then nothing runs any more (but the hooks still answer) */
  etx_t *e = load_test("err_run.lua", false);
  CHECK(etx_loaded(e));
  CHECK_STR(etx_error(e), "");
  CHECK_INT(etx_run(e, 0), 0);
  CHECK_INT(etx_run(e, 0), 0);
  CHECK_INT(etx_run(e, 0), -1);
  CHECK(starts(etx_error(e), "Script error: err_run.lua:6: attempt to index a nil value (local 't')"));
  CHECK(strstr(etx_error(e), "stack traceback:") != NULL);
  CHECK_INT(etx_run(e, 0), -1);
  CHECK_INT(call1(e, "count", 0), 3);
  CHECK(!etx_loaded(e));
  CHECK_INT(etx_load(e), 0);                                /* a reload starts over */
  CHECK_STR(etx_error(e), "");
  CHECK_INT(etx_run(e, 0), 0);
  etx_free(e);

  e = load_test("err_object.lua", false);
  CHECK_INT(etx_run(e, 0), -1);
  CHECK(strstr(etx_error(e), "(error object is a table value)") != NULL);
  etx_free(e);
  e = load_test("err_tostring.lua", false);
  CHECK_INT(etx_run(e, 0), -1);
  CHECK(starts(etx_error(e), "Script error: custom error object"));
  etx_free(e);

  /* run()'s result: a nonzero number closes the script (engine.js) */
  static const struct { int k; int want; } res[] = { { 0, 0 }, { 1, 1 }, { 2, 1 }, { 3, 0 }, { 4, 0 }, { 5, 0 },
    { 6, 1 }, { 7, 1 }, { 8, 0 }, { 9, 1 }, { 10, 0 }, { 99, 0 } };
  for (size_t i = 0; i < sizeof res / sizeof res[0]; i++) {
    e = load_test("results.lua", false);
    double k = res[i].k;
    etx_test_call(e, "ret", &k, 1, NULL, 0);
    CHECK_INT(etx_run(e, 0), res[i].want);
    CHECK_INT(etx_run(e, 0), res[i].want);                  /* closed stays closed */
    if (res[i].want) CHECK(etx_loaded(e));
    CHECK_STR(etx_error(e), "");
    etx_free(e);
  }

  /* the watchdog */
  e = load_test("hang_run.lua", false);
  CHECK_INT(etx_run(e, 0), 0);
  int64_t t0 = plat_now_us();
  CHECK_INT(etx_run(e, 1), -1);
  double s = (plat_now_us() - t0) / 1e6;
  CHECK(s >= 0.25 && s < 1.0);
  CHECK(starts(etx_error(e), "Script error: hang_run.lua:2: script took too long"));
  etx_free(e);
  e = load_test("hang_pcall.lua", false);
  t0 = plat_now_us();
  CHECK_INT(etx_run(e, 0), -1);
  s = (plat_now_us() - t0) / 1e6;
  CHECK(s < 1.0);
  CHECK(strstr(etx_error(e), "script took too long") != NULL);
  etx_free(e);
  /* xpcall: the handler is skipped once the watchdog fired (Lua runs it with hooks off there) */
  e = load_test("hang_xpcall.lua", false);
  double xo[4] = { -1, -1, -1, -1 };
  t0 = plat_now_us();
  CHECK_INT(etx_test_call(e, "both", NULL, 0, xo, 4), -1);
  CHECK(strstr(etx_test_error(e), "script took too long") != NULL);
  CHECK_INT(etx_test_call(e, "handler", NULL, 0, xo, 4), -1);
  CHECK(strstr(etx_test_error(e), "script took too long") != NULL);
  CHECK((plat_now_us() - t0) < 2000000);
  char xs[128] = "";
  CHECK_INT(etx_test_call(e, "normal", NULL, 0, xo, 4), 2);
  CHECK_INT(xo[0], 0);
  CHECK_INT(etx_test_call_str(e, "normal", NULL, 0, 1, xs, sizeof xs), 0);
  CHECK(starts(xs, "handled: hang_xpcall.lua:6: e1"));
  CHECK_INT(etx_test_call(e, "args", NULL, 0, xo, 4), 3);   /* true, 5, "ok" */
  CHECK(xo[0] == 1 && xo[1] == 5 && isnan(xo[2]));
  CHECK_INT(etx_test_call(e, "yield", NULL, 0, xo, 4), 3);  /* a yield inside xpcall still works */
  CHECK(xo[0] == 5 && xo[1] == 1 && xo[2] == 7);
  CHECK_INT(etx_test_call(e, "bad", NULL, 0, xo, 4), -1);
  CHECK(strstr(etx_test_error(e), "bad argument #2 to 'xpcall'") != NULL);
  CHECK_INT(etx_run(e, 0), 0);
  t0 = plat_now_us();
  CHECK_INT(etx_run(e, 1), -1);
  CHECK((plat_now_us() - t0) < 1000000);
  /* (xpcall returns false; the next instruction of run() raises it again) */
  CHECK(starts(etx_error(e), "Script error: hang_xpcall.lua:15: script took too long"));
  etx_free(e);

  etx_config_t c = cfg_test("hang_run.lua");
  c.watchdog_ms = 0;                                        /* default: 2 s */
  e = open_etx(&L640, &c, 0);
  t0 = plat_now_us();
  CHECK_INT(etx_run(e, 1), -1);
  s = (plat_now_us() - t0) / 1e6;
  CHECK(s >= 1.95 && s < 3.5);
  printf("  watchdog: default stops a hung run() after %.2f s\n", s);
  etx_free(e);

  /* the heap ceiling */
  e = load_test("mem.lua", false);
  CHECK_INT(etx_test_call(e, "big", NULL, 0, NULL, 0), -1);
  CHECK(strstr(etx_test_error(e), "not enough memory") != NULL);
  double n = 0;
  CHECK_INT(etx_test_call(e, "ok", NULL, 0, &n, 1), 1);
  CHECK_INT(n, 1 << 20);
  CHECK_INT(etx_run(e, 0), 0);
  etx_free(e);

  /* finalizers: refused (Lua runs them with hooks off); other metamethods are watched */
  e = load_test("gcerr.lua", false);
  double k = 1;
  CHECK_INT(etx_test_call(e, "junk", &k, 1, NULL, 0), -1);
  CHECK(strstr(etx_test_error(e), "__gc metamethods are not supported") != NULL);
  k = 2;
  CHECK_INT(etx_test_call(e, "junk", &k, 1, NULL, 0), -1);
  CHECK(strstr(etx_test_error(e), "bad argument #2 to 'setmetatable'") != NULL);
  CHECK_INT(etx_test_call(e, "falsegc", NULL, 0, NULL, 0), -1);   /* any __gc value */
  t0 = plat_now_us();
  double ok1 = 0;
  CHECK_INT(etx_test_call(e, "late", NULL, 0, &ok1, 1), 1);       /* never finalized: no hang */
  CHECK_INT(ok1, 1);
  for (int i = 0; i < 3; i++) etx_gc_step(e, 1 << 20);
  CHECK((plat_now_us() - t0) < 1000000);
  CHECK_INT(etx_test_call(e, "plain", NULL, 0, &ok1, 1), 1);      /* metatables otherwise work */
  CHECK_INT(ok1, 1);
  CHECK_INT(etx_test_call(e, "badarg", NULL, 0, NULL, 0), -1);
  CHECK(strstr(etx_test_error(e), "nil or table expected") != NULL);
  char mt[32] = "";
  CHECK_INT(etx_test_call_str(e, "filemt", NULL, 0, 0, mt, sizeof mt), 0);
  CHECK_STR(mt, "FILE*");                                   /* the handle metatable is locked */
  double fm[2] = { -1, -1 };
  CHECK_INT(etx_test_call(e, "filemt", NULL, 0, fm, 2), 2);
  CHECK_INT(fm[1], 0);                                      /* setmetatable on userdata: refused */
  t0 = plat_now_us();
  CHECK_INT(etx_test_call(e, "spinidx", NULL, 0, NULL, 0), -1);
  CHECK(strstr(etx_test_error(e), "script took too long") != NULL);
  CHECK((plat_now_us() - t0) < 1000000);
  CHECK_INT(etx_run(e, 0), 0);
  /* an error object whose __tostring hangs or fails: still an error message, in time */
  k = 1;
  etx_test_call(e, "mode", &k, 1, NULL, 0);
  t0 = plat_now_us();
  CHECK_INT(etx_run(e, 0), -1);
  CHECK((plat_now_us() - t0) < 1000000);
  CHECK(starts(etx_error(e), "Script error: gcerr.lua:"));
  CHECK(strstr(etx_error(e), "script took too long") != NULL);
  etx_free(e);
  e = load_test("gcerr.lua", false);
  k = 2;
  etx_test_call(e, "mode", &k, 1, NULL, 0);
  CHECK_INT(etx_run(e, 0), -1);
  CHECK(starts(etx_error(e), "Script error: "));
  CHECK(strstr(etx_error(e), "again") != NULL);
  etx_free(e);

  /* a strict _G can't make the host's global access fail */
  e = load_test("strict.lua", false);
  etx_set_global_number(e, "STICKTIME_LAT", 0.25);
  CHECK_NEAR(call1(e, "lat", 0), 0.25, 1e-6);
  etx_set_global_number(e, "TOOL_RUN_US", 1234);
  double tv[2];
  CHECK_INT(etx_test_call(e, "tool", NULL, 0, tv, 2), 2);
  CHECK_INT(tv[0], 1234); CHECK_INT(tv[1], 1);
  etx_clear_global(e, "STICKTIME_LAT");
  CHECK(isnan(call1(e, "lat", 0)));
  CHECK_INT(etx_game_state(e), GS_UNKNOWN);
  CHECK_INT(etx_run(e, 0), 0);
  etx_free(e);
}

/* ================================================================== STICKTIME_TEST from C */
static void test_hooks(void) {
  etx_t *e = load_test("hooks.lua", false);
  double a[3] = { 1, 2.5, 3 }, out[64];
  CHECK_INT(etx_test_call(e, "sum", a, 3, out, 4), 1);
  CHECK_NEAR(out[0], 6.5, 1e-6);
  CHECK_INT(etx_test_call(e, "sum", a, 2, out, 4), 1);
  CHECK_NEAR(out[0], 3.5, 1e-6);
  CHECK_INT(etx_test_call(e, "types", a, 2, out, 3), 3);    /* integers stay integers */
  CHECK(out[0] == 1 && out[1] == 1 && out[2] == 2);
  CHECK_INT(etx_test_call(e, "multi", NULL, 0, out, 8), 8);
  CHECK(out[0] == 1 && out[1] == 1 && out[2] == 0 && isnan(out[3]) && isnan(out[4]) && isnan(out[5]) &&
        out[6] == 2.5 && isnan(out[7]));
  CHECK_INT(etx_test_call(e, "multi", NULL, 0, out, 2), 2);
  CHECK_INT(etx_test_call(e, "multi", NULL, 0, NULL, 0), 0);
  CHECK_INT(etx_test_call(e, "many", NULL, 0, out, 64), 50);
  CHECK(out[0] == 1 && out[49] == 50);
  CHECK_INT(etx_test_call(e, "none", NULL, 0, out, 4), 0);
  CHECK_INT(etx_test_call(e, "err", NULL, 0, out, 4), -1);
  CHECK(strstr(etx_test_error(e), "hook failed") != NULL);
  CHECK_INT(etx_test_call(e, "sum", a, 3, out, 1), 1);
  CHECK_STR(etx_test_error(e), "");
  CHECK_INT(etx_test_call(e, "missing", NULL, 0, out, 1), -1);
  CHECK_STR(etx_test_error(e), "no STICKTIME_TEST.missing");
  CHECK_INT(etx_test_call(e, "notfn", NULL, 0, out, 1), -1);
  CHECK_INT(etx_test_call(e, NULL, NULL, 0, out, 1), -1);
  CHECK_INT(etx_test_call(e, "sum", a, -1, out, 1), -1);    /* no args: a + b fails */
  /* strings */
  /* strings: positions count like etx_test_call's out[] (scenario.c passes the NaN slot's index) */
  char buf[64];
  CHECK_INT(etx_test_call_str(e, "name", NULL, 0, -1, buf, sizeof buf), 0);   /* the first string */
  CHECK_STR(buf, "Hoop Forest");
  CHECK_INT(etx_test_call_str(e, "name", NULL, 0, 1, buf, sizeof buf), 0);
  CHECK_STR(buf, "Hoop Forest");
  CHECK_INT(etx_test_call_str(e, "name", NULL, 0, 2, buf, sizeof buf), 0);
  CHECK_STR(buf, "second");
  CHECK_INT(etx_test_call_str(e, "name", NULL, 0, 0, buf, sizeof buf), -1);   /* a number */
  CHECK_STR(buf, "");
  CHECK_STR(etx_test_error(e), "STICKTIME_TEST.name: no string result");
  CHECK_INT(etx_test_call_str(e, "name", NULL, 0, 3, buf, sizeof buf), -1);   /* none */
  CHECK_INT(etx_test_call_str(e, "name", NULL, 0, 1, buf, 5), 0);             /* truncated */
  CHECK_STR(buf, "Hoop");
  CHECK_INT(etx_test_call_str(e, "name", NULL, 0, 1, buf, 1), 0);
  CHECK_STR(buf, "");
  CHECK_INT(etx_test_call_str(e, "name", NULL, 0, 1, NULL, 0), 0);            /* only: is there one */
  CHECK_INT(etx_test_call_str(e, "long", NULL, 0, 0, buf, sizeof buf), 0);
  CHECK_INT(strlen(buf), 63);
  CHECK_INT(etx_test_call_str(e, "multi", NULL, 0, 3, buf, sizeof buf), 0);   /* 1, true, false, "s" */
  CHECK_STR(buf, "s");
  CHECK_INT(etx_test_call_str(e, "multi", NULL, 0, 4, buf, sizeof buf), -1);  /* a table */
  CHECK_INT(etx_test_call_str(e, "sum", a, 3, -1, buf, sizeof buf), -1);
  CHECK_INT(etx_test_call_str(e, "err", NULL, 0, 0, buf, sizeof buf), -1);
  CHECK(strstr(etx_test_error(e), "hook failed") != NULL);
  CHECK_INT(etx_test_call_str(e, "nothere", NULL, 0, -1, buf, sizeof buf), -1);
  /* the game state */
  static const struct { double s; int want; } st[] = { { 4, GS_FLY }, { 1, GS_MENU }, { 8, GS_DONE }, { 9, GS_UNKNOWN },
    { 0, GS_UNKNOWN }, { 2.5, GS_UNKNOWN }, { -1, GS_UNKNOWN }, { NAN, GS_UNKNOWN }, { 7, GS_PAUSED } };
  for (size_t i = 0; i < sizeof st / sizeof st[0]; i++) {
    etx_test_call(e, "state", &st[i].s, 1, NULL, 0);
    CHECK_INT(etx_game_state(e), st[i].want);
  }
  /* a hung hook is stopped, and the state stays usable */
  int64_t t0 = plat_now_us();
  CHECK_INT(etx_test_call(e, "spin", NULL, 0, out, 1), -1);
  CHECK((plat_now_us() - t0) < 1000000);
  CHECK(strstr(etx_test_error(e), "script took too long") != NULL);
  CHECK_INT(etx_test_call(e, "sum", a, 3, out, 1), 1);
  CHECK_INT(etx_run(e, 0), 0);
  /* run(event, touch) */
  etx_touch_t tch = { 10, 20, 1, 2, 3, 4, 5 };
  CHECK_INT(etx_run_touch(e, 0x1004, &tch), 0);
  CHECK_INT(etx_test_call(e, "touch", NULL, 0, out, 7), 7);
  CHECK(out[0] == 10 && out[1] == 20 && out[2] == 1 && out[3] == 2 && out[4] == 3 && out[5] == 4 && out[6] == 5);
  CHECK_INT(etx_run(e, 0), 0);
  CHECK_INT(etx_test_call(e, "touch", NULL, 0, out, 7), 7);
  CHECK(out[0] == 10);
  etx_free(e);

  /* without STICKTIME_TEST */
  sd_write("/SCRIPTS/TESTS/plain.lua", "return { run = function() return 0 end }\n");
  etx_config_t c = cfg_test("plain.lua");
  c.test_hooks = false;
  e = open_etx(&L640, &c, 0);
  CHECK_INT(etx_test_call(e, "get", NULL, 0, out, 20), -1);
  CHECK_STR(etx_test_error(e), "no STICKTIME_TEST");
  CHECK_INT(etx_game_state(e), GS_UNKNOWN);
  CHECK_INT(etx_test_call_str(e, "name", NULL, 0, -1, buf, sizeof buf), -1);
  CHECK_INT(etx_run(e, 0), 0);
  etx_free(e);
}

/* ================================================================== globals, life cycle */
static void test_globals(void) {
  double v[4];
  etx_config_t c = cfg_test("globals.lua");
  c.lat = 0;
  etx_t *e = open_etx(&L640, &c, 0);
  CHECK_INT(etx_test_call(e, "lat", NULL, 0, v, 3), 3);
  CHECK(v[0] == 0 && v[1] == 1 && v[2] == 0);              /* STICKTIME_LAT = 0 (an integer) */
  CHECK_INT(etx_test_call(e, "av", NULL, 0, v, 2), 2);
  CHECK(v[0] == 0 && isnan(v[1]));                          /* no STICKTIME_AV, no STICKTIME_DT */
  etx_set_global_number(e, "STICKTIME_DT", 16.683);
  CHECK_INT(etx_test_call(e, "av", NULL, 0, v, 2), 2);
  CHECK_NEAR(v[1], 16.683, 1e-6);
  CHECK_INT(etx_test_call(e, "touch", NULL, 0, v, 1), 1);
  CHECK_INT(v[0], 0x1004);
  CHECK_INT(etx_test_call(e, "size", NULL, 0, v, 2), 2);
  CHECK(v[0] == 640 && v[1] == 480);
  etx_set_global_number(e, "TOOL_RUN_US", 1500);
  etx_set_global_number(e, "TOOL_SHOW_US", 2.5);
  CHECK_INT(etx_test_call(e, "tool", NULL, 0, v, 3), 3);
  CHECK(v[0] == 1500 && v[1] == 2.5 && v[2] == 1);
  etx_clear_global(e, "TOOL_RUN_US");
  CHECK_INT(etx_test_call(e, "tool", NULL, 0, v, 3), 3);
  CHECK(isnan(v[0]));
  etx_set_global_number(e, "STICKTIME_LAT", 0.0183);
  CHECK_NEAR(call1(e, "lat", 0), 0.0183, 1e-6);
  etx_set_global_number(e, NULL, 1);
  CHECK_INT(etx_run(e, ETX_EVT_ENTER), 0);
  CHECK_INT(call1(e, "ev", 0), 514);
  CHECK_INT(etx_run(e, 0), 0);
  CHECK_INT(call1(e, "ev", 0), 0);
  CHECK_INT(etx_run(e, ETX_EVT_NEXT), 0);
  CHECK_INT(call1(e, "ev", 0), 0x1E00);
  etx_stats_t st;
  etx_stats(e, &st);
  CHECK_INT(st.frames, 3);
  CHECK(st.lua_kb > 0 && st.lua_kb < 2048);
  CHECK(st.instr_count >= 0);
  etx_gc_step(e, 10);
  etx_free(e);

  c.lat = 0.0183;
  c.no_touch = true;
  c.av = true;
  e = open_etx(&L640, &c, 0);
  CHECK_INT(etx_test_call(e, "av", NULL, 0, v, 2), 2);
  CHECK_INT(v[0], 1);                                       /* STICKTIME_AV = true before the script ran */
  CHECK_INT(etx_test_call(e, "lat", NULL, 0, v, 3), 3);
  CHECK_NEAR(v[0], 0.0183, 1e-6);
  CHECK(v[1] == 0 && v[2] == 0);
  CHECK_INT(etx_test_call(e, "touch", NULL, 0, v, 1), 1);
  CHECK_INT(v[0], 0);                                       /* EVT_TOUCH_* left undefined */
  etx_free(e);

  c.lat = -1;
  c.no_touch = false;
  c.av = false;
  e = open_etx(&L640, &c, 0);
  CHECK_INT(etx_test_call(e, "lat", NULL, 0, v, 3), 3);
  CHECK(isnan(v[0]) && v[2] == 1);
  etx_free(e);

  /* no LCD: the default size, drawing does nothing */
  e = open_etx(NULL, &c, 0);
  CHECK_INT(etx_test_call(e, "size", NULL, 0, v, 2), 2);
  CHECK(v[0] == 480 && v[1] == 272);
  CHECK_INT(etx_test_call(e, "draw", NULL, 0, v, 2), 2);
  CHECK(v[0] == 0 && v[1] == 0);
  etx_free(e);

  /* before etx_load and after a failed one */
  e = etx_new(&L640, &c);
  CHECK(!etx_loaded(e));
  CHECK_STR(etx_error(e), "");
  CHECK_INT(etx_run(e, 0), -1);
  CHECK_INT(etx_test_call(e, "get", NULL, 0, v, 1), -1);
  CHECK_INT(etx_game_state(e), GS_UNKNOWN);
  etx_set_global_number(e, "X", 1);
  etx_clear_global(e, "X");
  etx_gc_step(e, 1);
  etx_stats(e, &st);
  CHECK(st.frames == 0 && st.lua_kb == 0);
  etx_tone_t tt[2];
  CHECK_INT(etx_take_tones(e, tt, 2), 0);
  etx_flush_writes(e);
  etx_free(e);
  e = etx_new(&L640, NULL);                                 /* no config at all */
  CHECK_INT(etx_load(e), -1);
  CHECK(starts(etx_error(e), "Script error: : file not found"));
  etx_free(e);
}

/* ================================================================== track slots */
static void slots_reset(void) {
  char p[512];
  snprintf(p, sizeof p, "%s/SCRIPTS/TOOLS/StickTimeTracks", SD);
  rm_rf(p);
  plat_mkdirs(p);
}

static void slot(int i, const char *s) {
  char p[128];
  snprintf(p, sizeof p, TRACKS "%d.txt", i);
  if (s) sd_write(p, s); else remove_in(SD, p);
}

static int slot_open(etx_t *e, int i, int how) {
  double a[2] = { i, how }, r[2] = { -1, -1 };
  etx_test_call(e, "open", a, 2, r, 2);
  return (int)r[0];
}

static bool slot_last(etx_t *e, const char *want) {
  char buf[16384];                                           /* a format 4 track: up to 10000 bytes */
  etx_test_call_str(e, "last", NULL, 0, 0, buf, sizeof buf);
  bool ok = !strcmp(buf, want);
  if (!ok) fprintf(stderr, "  slot content \"%s\", want \"%s\"\n", buf, want);
  return ok;
}

static bool listed(etx_t *e, int i, const char *name) {
  char buf[64] = "";
  int n = call_str(e, "listed", i, buf, sizeof buf);
  return name ? n >= 0 && !strcmp(buf, name) : n < 0;
}

#define C1 "Test Loop\n1 4242 7 9  0 0 1 0 0  0 40 1 0 0 / 20 30 3 3 0 4 2\n"
#define C2 "Second\n1 6 3 9\n"

static void test_slots(void) {
  slots_reset();
  slot(1, C1);
  slot(2, "Bad\n1 5\n");                                     /* no seed: crashes buildTrack */
  slot(3, "Broken\n2 1 1 9 0 0 1 0 0\n");                    /* format 2 (1.8.0): listed, one gate short */
  slot(4, "\nOdd File\n1 55 3 9  0 0 1 0 0  30 0 9 0 0 / 10 10 1 1 0 2 7\n");
  slot(6, "Frac\n1 7 3 9  0 0 1.005 0 0  0 40 1 0 0\n");    /* passes the type check, indexes nil */
  slot(7, "Inf\n1 8 3 9  1e400 0 1 0 0  0 40 1 0 0\n");
  slot(8, "Seed\n1 9 1e400\n");
  sd_write("/SCRIPTS/TOOLS/StickTimeTracks/c9.txt", "Bad\n1 5\n");
  log_clear();
  etx_t *e = load_test("slots.lua", false);
  CHECK(listed(e, 1, "Test Loop"));
  CHECK(listed(e, 2, NULL));
  CHECK(listed(e, 3, "Broken"));
  CHECK(listed(e, 4, "Odd File"));
  for (int i = 5; i <= 8; i++) CHECK(listed(e, i, NULL));
  CHECK(log_has("c2.txt hidden"));
  CHECK_INT(slot_open(e, 1, 0), 1);
  CHECK(slot_last(e, C1));
  static const int want[9] = { 0, 1, 0, 1, 1, 0, 0, 0, 0 };
  for (int i = 1; i <= 8; i++) CHECK_INT(slot_open(e, i, 0), want[i]);
  CHECK_INT(slot_open(e, 2, 1), 0);                          /* other spellings of the path */
  CHECK_INT(slot_open(e, 2, 2), 0);
  CHECK_INT(slot_open(e, 2, 3), 1);                          /* r+ is not the game's read */
  CHECK_INT(call1(e, "nonslot", 0), 1);                      /* c9 is no slot */
  etx_free(e);

  slots_reset();
  slot(1, "Slash\n1 5 / 1 2 3\n");                           /* the '/' ends the numbers early */
  slot(2, "Box\n1 10 3 9  0 0 1 0 0  0 40 1 0 0 / 20 30 3 3 0 4 2.005\n");
  slot(3, "BoxBad\n1 10 3 9  0 0 1 0 0  0 40 1 0 0 / 20 30 3 3 0 4 9\n");   /* dropped by the game */
  slot(4, "\xEF\xBB\xBFHoops\r\n1 77 3 18  0 0 4 0 2.5  20 30 4 45 2  0 60 4 180 3\r\n");
  slot(5, "A name longer than the sixty bytes the menu scan reads ......\n1 5 3 9\n");
  slot(6, "Inf2\n1 8 3 9  0 0 1 0 0  0 40 1 0 0 / 1e400 30 3 3 0 4 2\n");
  slot(7, "Two\n1 5 3\n");
  slot(8, "  \n \n Name \n 1 \t 2 3\n");
  e = load_test("slots.lua", false);
  static const int want2[9] = { 0, 0, 0, 1, 1, 1, 0, 1, 1 };
  for (int i = 1; i <= 8; i++) CHECK_INT(slot_open(e, i, 0), want2[i]);
  CHECK(listed(e, 3, "BoxBad"));
  CHECK(listed(e, 4, "\xEF\xBB\xBFHoops"));
  CHECK(listed(e, 5, NULL));
  CHECK(listed(e, 7, "Two"));
  CHECK(listed(e, 8, "Name "));
  etx_free(e);

  /* what the ghost file's %d needs: an int32 id, the first gate within +-1e8 (x 10 fits int32) */
  slots_reset();
  slot(1, "MaxId\n1 2147483647 3 9\n");
  slot(2, "BigId\n1 2147483648 3 9\n");                       /* a float under LUA_32BITS */
  slot(3, "Near\n1 5 3 9  99999999 -99999999 1 0 0  0 40 1 0 0\n");
  slot(4, "Far\n1 5 3 9  100000000 0 1 0 0  0 40 1 0 0\n");
  slot(5, "FarBox\n1 5 3 9  0 0 1 0 0  0 40 1 0 0 / 0 20 3 3 0 -1e8 2\n");
  slot(6, "Zeros\n1 000000000000005 3 9\n");                  /* leading zeros: still the integer 5 */
  slot(7, "HugeId\n1 99999999999999999999999999999999999999999 3 9\n");
  slot(8, "Frac2\n1 5 3 9  0 0 1 0 0  0 40 1.009 0 0\n");      /* within the game's 0.01: crashes it */
  e = load_test("slots.lua", false);
  static const int want3[9] = { 0, 1, 0, 1, 0, 0, 1, 0, 0 };
  for (int i = 1; i <= 8; i++) CHECK_INT(slot_open(e, i, 0), want3[i]);
  CHECK(listed(e, 1, "MaxId"));
  CHECK(listed(e, 2, NULL));
  CHECK(listed(e, 3, "Near"));
  CHECK(listed(e, 6, "Zeros"));
  CHECK(listed(e, 7, NULL));
  etx_free(e);

  /* format 2 (StickTime 1.8.0): up to 48 gates and 64 structures, read from 6000 bytes; once
   * listed, it keeps its last good content too, all of it (past the 4096 bytes a snapshot held) */
  static char big[6000];
  int bn = snprintf(big, sizeof big, "Big One\n2 973733 7 9");
  for (int g = 0; g < 48; g++)
    bn += snprintf(big + bn, sizeof big - (size_t)bn, "  %d.1250 -%d.8750 %d 90.5000 2.2500", g * 10, g * 3, g % 8 + 1);
  bn += snprintf(big + bn, sizeof big - (size_t)bn, " /");
  for (int s = 0; s < 64; s++)
    bn += snprintf(big + bn, sizeof big - (size_t)bn, " %d.1250 -%d.8750 1.1250 2.8750 0.5000 3.6250 %d", s * 7, s * 2, s % 3 + 1);
  bn += snprintf(big + bn, sizeof big - (size_t)bn, "\n");
  CHECK(bn > 4096 && bn < (int)sizeof big - 1);
  slots_reset();
  slot(1, big);
  e = load_test("slots.lua", false);
  CHECK(listed(e, 1, "Big One"));
  CHECK_INT(slot_open(e, 1, 0), 1);
  CHECK(slot_last(e, big));
  slot(1, "garbage");
  log_clear();
  CHECK_INT(slot_open(e, 1, 0), 1);
  CHECK(slot_last(e, big));
  CHECK(log_has("keeping the version the game loaded"));
  etx_free(e);

  /* formats 3 and 4 (StickTime 1.10.0): scenery after the structures (props / rails / decals: the
   * game checks those itself), structure kinds 1-9; the structures end at the next '/' */
  slots_reset();
  slot(1, "Scenery\n3 42 3 9  0 0 1 0 0  0 40 1 0 0 / 20 30 3 3 0 4 5 / 10 20 4 0 / -20 0 -20 60 1 2 / 0 30 4 0.1 0 4\n");
  slot(2, "NoSeed\n3 5 / / / /\n");                              /* no seed: crashes buildTrack */
  slot(3, "Inf3\n3 8 3 9  0 0 1 0 0  0 40 1 0 0 / 1e400 30 3 3 0 4 7 / / /\n");   /* a new kind of structure */
  slot(4, "Kind10\n4 8 3 9  0 0 1 0 0  0 40 1 0 0 / 1e400 30 3 3 0 4 10\n");      /* no such kind: dropped */
  slot(5, "Frac3\n3 5 3 9  0 0 1 0 0  0 40 1 0 0 / 0 20 3 3 0 4 6.005\n");
  slot(6, "Odd Props\n3 5 3 9  0 0 1 0 0  0 40 1 0 0 / / 1e400 x 4 0 / /\n");    /* the game's own checks */
  slot(7, "Five\n5 5 3 9\n");                                    /* no such format: not listed */
  e = load_test("slots.lua", false);
  static const int want4[9] = { 0, 1, 0, 0, 1, 0, 1, 1, 0 };
  for (int i = 1; i <= 8; i++) CHECK_INT(slot_open(e, i, 0), want4[i]);
  CHECK(listed(e, 1, "Scenery"));
  CHECK(listed(e, 4, "Kind10"));
  CHECK(listed(e, 6, "Odd Props"));
  CHECK(listed(e, 7, NULL));
  etx_free(e);

  /* format 4: read from 10000 bytes, and a listed one keeps all of its last good content (past the
   * 6000 bytes a snapshot held for format 2) */
  static char big4[10000];
  bn = snprintf(big4, sizeof big4, "Big Four\n4 973733 7 9");
  for (int g = 0; g < 48; g++)
    bn += snprintf(big4 + bn, sizeof big4 - (size_t)bn, "  %d.1250 -%d.8750 %d 90.5000 2.2500", g * 10, g * 3, g % 8 + 1);
  bn += snprintf(big4 + bn, sizeof big4 - (size_t)bn, " /");
  for (int s = 0; s < 64; s++)
    bn += snprintf(big4 + bn, sizeof big4 - (size_t)bn, " %d.1250 -%d.8750 1.1250 2.8750 0.5000 3.6250 %d", s * 7, s * 2, s % 9 + 1);
  bn += snprintf(big4 + bn, sizeof big4 - (size_t)bn, " /");
  for (int p = 0; p < 120; p++)
    bn += snprintf(big4 + bn, sizeof big4 - (size_t)bn, " %d.5 -%d.5 %d 0", p * 4, p * 3, p % 9 + 1);
  bn += snprintf(big4 + bn, sizeof big4 - (size_t)bn, " / -20 0 -20 60 1 2 / 0 30 4 0.1 0 4\n");
  CHECK(bn > 6000 && bn < 10000);
  slots_reset();
  slot(1, big4);
  e = load_test("slots.lua", false);
  CHECK(listed(e, 1, "Big Four"));
  CHECK_INT(slot_open(e, 1, 0), 1);
  CHECK(slot_last(e, big4));
  slot(1, "garbage");
  log_clear();
  CHECK_INT(slot_open(e, 1, 0), 1);
  CHECK(slot_last(e, big4));
  CHECK(log_has("keeping the version the game loaded"));
  etx_free(e);

  /* once listed, a slot keeps its last good content */
  slots_reset();
  slot(1, C1);
  slot(2, C2);
  e = load_test("slots.lua", false);
  CHECK(listed(e, 1, "Test Loop"));
  CHECK(listed(e, 2, "Second"));
  slot(1, "Bad\n1 5\n");
  log_clear();
  CHECK_INT(slot_open(e, 1, 0), 1);
  CHECK(slot_last(e, C1));
  CHECK(log_has("keeping the version the game loaded"));
  slot(2, NULL);
  CHECK_INT(slot_open(e, 2, 0), 1);
  CHECK(slot_last(e, C2));
  slot(1, "New One\n1 99 5 9\n");
  CHECK_INT(slot_open(e, 1, 0), 1);
  CHECK(slot_last(e, "New One\n1 99 5 9\n"));
  slot(1, "garbage");
  CHECK_INT(slot_open(e, 1, 0), 1);
  CHECK(slot_last(e, "New One\n1 99 5 9\n"));
  CHECK_INT(slot_open(e, 3, 0), 0);
  slot(3, "Late\n1 5\n");
  CHECK_INT(slot_open(e, 3, 0), 0);
  slot(3, "Late\n1 5 3\n");
  CHECK_INT(slot_open(e, 3, 0), 1);
  /* a reload forgets: not listed now, so harmless as it is */
  CHECK_INT(etx_load(e), 0);
  CHECK(listed(e, 1, NULL));
  CHECK_INT(slot_open(e, 1, 0), 1);
  CHECK(slot_last(e, "garbage"));
  CHECK_INT(slot_open(e, 2, 0), 0);
  CHECK(listed(e, 3, "Late"));
  etx_free(e);

  /* pending writes (async, held on the writer thread) are checked like files */
  slots_reset();
  e = load_test("slots.lua", true);
  etx_writer_hold(true);
  double w[2] = { 1, 1 };
  CHECK_INT(etx_test_call(e, "write", w, 2, NULL, 0), 0);    /* a good track, pending */
  w[0] = 2; w[1] = 2;
  CHECK_INT(etx_test_call(e, "write", w, 2, NULL, 0), 0);    /* a crashing one, pending */
  CHECK(!exists_in(SD, TRACKS "1.txt") && !exists_in(SD, TRACKS "2.txt"));
  CHECK_INT(slot_open(e, 1, 0), 1);
  CHECK(slot_last(e, "Pending\n1 31 3 9\n"));
  CHECK_INT(slot_open(e, 2, 0), 0);
  CHECK_INT(slot_open(e, 2, 3), 1);                          /* r+ sees it as it is */
  CHECK(slot_last(e, "Bad\n1 5\n"));
  CHECK_INT(etx_load(e), 0);                                 /* the menu scan reads the pending ones */
  CHECK(listed(e, 1, "Pending"));
  CHECK(listed(e, 2, NULL));
  /* listed, then overwritten with a crashing file while still pending: the snapshot answers */
  w[0] = 1; w[1] = 2;
  CHECK_INT(etx_test_call(e, "write", w, 2, NULL, 0), 0);
  CHECK_INT(slot_open(e, 1, 0), 1);
  CHECK(slot_last(e, "Pending\n1 31 3 9\n"));
  etx_writer_hold(false);
  etx_flush_writes(e);
  CHECK(file_is(SD, TRACKS "1.txt", "Bad\n1 5\n"));           /* the file itself is what was written */
  CHECK(file_is(SD, TRACKS "2.txt", "Bad\n1 5\n"));
  CHECK_INT(slot_open(e, 1, 0), 1);
  CHECK(slot_last(e, "Pending\n1 31 3 9\n"));
  etx_free(e);
  slots_reset();
}

/* ================================================================== the real game */
static double stats_sorted[40000];
static int nstats;
static int64_t rejected_total;
static double game_clock;

static int game_frame(etx_t *e, int ev, const int st[4]) {
  game_clock += 1000.0 / 60.0;
  etx_set_clock_ms(e, (int64_t)game_clock);
  etx_set_sticks(e, st[0], st[1], st[2], st[3]);
  memset(&L640.st, 0, sizeof L640.st);
  int64_t t0 = plat_now_us();
  int r = etx_run(e, ev);
  int64_t t1 = plat_now_us();
  if (nstats < (int)(sizeof stats_sorted / sizeof stats_sorted[0])) stats_sorted[nstats++] = (t1 - t0) / 1000.0;
  rejected_total += L640.st.rejected;
  etx_gc_step(e, 1);
  return r;
}

static const int IDLE[4] = { 0, 0, -1024, 0 };

/* harness.lua hoverStick(): the stick (0..1) that gives load x the weight in thrust */
static double hover_stick(etx_t *e, double load) {
  double p[5];
  if (etx_test_call(e, "thrust", NULL, 0, p, 5) < 5) return 0.5;
  double twr = p[0], i0 = p[1], n = p[2], dz = p[3], tk = p[4];
  double r = (load / twr - i0) / (1 - i0), lo = 0, hi = 1;
  if (r < 0) r = 0;
  for (int i = 0; i < 40; i++) {
    double x = (lo + hi) / 2;
    if ((1 - tk) * pow(x, n) + tk * pow(x, 8) < r) lo = x; else hi = x;
  }
  return dz + (1 - dz) * hi;
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/* harness.lua autopilot(), angle mode: steer for a lead point on the gate axis */
static void autopilot(etx_t *e, int st[4]) {
  double s[20], g[10];
  if (etx_test_call(e, "get", NULL, 0, s, 20) < 20) return;
  double px_ = s[0], py = s[1], pz = s[2], vx = s[3], vy = s[4], vz = s[5], fx = s[6], fz = s[8];
  double ng = s[17];
  if (etx_test_call(e, "gate", &ng, 1, g, 10) < 10) return;
  double nx = g[3], ny = g[4], nz = g[5], k = g[6], ax = g[7], ay = g[8], az = g[9];
  double d = (px_ - ax) * nx + (py - ay) * ny + (pz - az) * nz;
  double lead = clampd(-d * 0.55, 0, 14);
  if (d > 0) lead = 8;
  double tx = ax - nx * lead, ty = ay - ny * lead, tz = az - nz * lead;
  double hd = sqrt((px_ - ax) * (px_ - ax) + (pz - az) * (pz - az));
  if (k == 3) { tx = ax; tz = az; ty = hd > 1.2 ? ay + 3 : ay - 4; }
  double dx = tx - px_, dz = tz - pz, dist = sqrt(dx * dx + dz * dz) + 1e-6;
  double hl = sqrt(fx * fx + fz * fz) + 1e-6, hx = fx / hl, hz = fz / hl;
  double herr = atan2(hz * dx - hx * dz, hx * dx + hz * dz);
  st[3] = (int)(clampd(herr * 1.6, -1, 1) * 1024);
  double vf = vx * hx + vz * hz, vs = vx * hz - vz * hx;
  double vdes = fmin(11, 3 + dist * 0.35);
  if (fabs(herr) > 0.6) vdes = 3;
  if (k == 3) vdes = fmin(vdes, hd * 0.6);
  if (k < 6 && k != 3 && py - ty > 0.7) vdes = fmin(vdes, fmax(0.5, 2 * hd / (py - ty)));
  double ele = clampd((vdes - vf) * 0.12, -0.75, 0.75);
  st[1] = (int)(ele * 1024);
  st[0] = (int)(clampd(-vs * 0.15 + herr * 0.25, -0.6, 0.6) * 1024);
  double tilt = fmin(0.8, fabs(ele) * 0.9);
  double t = hover_stick(e, 1 / cos(tilt)) + (ty - py) * 0.15 - vy * 0.15;
  st[2] = (int)((clampd(t, 0, 1) * 2 - 1) * 1024);
}

static int cmp_d(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

static etx_t *game_open(lcd_t *l, bool async) {
  etx_config_t c;
  memset(&c, 0, sizeof c);
  c.sd_root = GAME_SD;
  c.script = GAME;
  c.test_hooks = true;
  c.lat = -1;
  c.stick_mode = 2;
  c.async_writes = async;
  game_clock = 0;
  etx_t *e = etx_new(l, &c);
  etx_set_log(e, logcb, NULL);
  etx_set_clock_ms(e, 0);
  int r = etx_load(e);
  CHECK_INT(r, 0);
  if (r) fprintf(stderr, "  game: %s\n", etx_error(e));
  return e;
}

static void game_reset_sd(const char *src, size_t len, const char *dat) {
  rm_rf(GAME_SD);
  sd_write_in(GAME_SD, GAME, src, len);
  char p[512];
  snprintf(p, sizeof p, "%s/SCRIPTS/TOOLS/StickTimeTracks", GAME_SD);
  plat_mkdirs(p);
  snprintf(p, sizeof p, "%s/SCRIPTS/TOOLS/StickTimeGhosts", GAME_SD);
  plat_mkdirs(p);
  if (dat) game_write("/SCRIPTS/TOOLS/StickTime.dat", dat);
}

static int info_highest(etx_t *e) {
  double v[11];
  return etx_test_call(e, "info", NULL, 0, v, 11) >= 1 ? (int)v[0] : -1;
}

static bool track_name(etx_t *e, const char *want) {
  char buf[64] = "";
  etx_test_call_str(e, "info", NULL, 0, 10, buf, sizeof buf);
  bool ok = !strcmp(buf, want);
  if (!ok) fprintf(stderr, "  track name \"%s\", want \"%s\"\n", buf, want);
  return ok;
}

/* game-api.md §10 A-C with the real game */
static void game_slot_cases(const char *src, size_t len, lcd_t *l) {
  /* A: a slot that would crash init(), saved as the current track */
  game_reset_sd(src, len, "FPVSIM2 lay=3 track=9\n");
  game_write(TRACKS "1.txt", "Bad\n1 5\n");
  etx_t *e = game_open(l, false);
  CHECK(etx_loaded(e));
  CHECK_INT(info_highest(e), 8);                             /* c1 hidden: no track 9 */
  CHECK_INT(etx_game_state(e), GS_MENU);
  CHECK_INT(game_frame(e, 0, IDLE), 0);
  etx_free(e);

  /* B, C: listed at load, then broken or deleted before it is built */
  game_reset_sd(src, len, NULL);
  game_write(TRACKS "1.txt", C1);
  game_write(TRACKS "2.txt", C2);
  e = game_open(l, false);
  CHECK_INT(info_highest(e), 10);
  game_write(TRACKS "1.txt", "Bad\n1 5\n");
  remove_in(GAME_SD, TRACKS "2.txt");
  double t = 10;
  CHECK_INT(etx_test_call(e, "track", &t, 1, NULL, 0), 0);
  CHECK(track_name(e, "Second"));
  t = 9;
  CHECK_INT(etx_test_call(e, "track", &t, 1, NULL, 0), 0);
  CHECK(track_name(e, "Test Loop"));
  /* B: the menu's Track item steps onto it (and saves StickTime.dat) */
  t = 1;
  etx_test_call(e, "track", &t, 1, NULL, 0);
  int r = 0;
  for (int i = 0; i < 4 && !r; i++) r = game_frame(e, ETX_EVT_NEXT, IDLE);   /* focus Track (item 5) */
  r |= game_frame(e, ETX_EVT_ENTER, IDLE);
  for (int i = 0; i < 12 && !r; i++) r |= game_frame(e, ETX_EVT_PREV, IDLE);  /* DEC: 1 -> 10 -> 9 ... */
  r |= game_frame(e, ETX_EVT_ENTER, IDLE);
  for (int i = 0; i < 30 && !r; i++) r |= game_frame(e, 0, IDLE);
  CHECK_INT(r, 0);
  if (r) fprintf(stderr, "  game: %s\n", etx_error(e));
  CHECK(etx_loaded(e));
  /* a track that doesn't exist is the caller's to avoid (the game leaves S.track invalid) */
  t = 12;
  CHECK_INT(etx_test_call(e, "track", &t, 1, NULL, 0), -1);
  etx_free(e);
  printf("  game: crashing track slots hidden or replaced (cases A, B, C)\n");
}

static void test_game(void) {
  size_t len = 0;
  char *src = plat_read_file(GAME_SRC, &len, 1 << 22);
  if (!src) {
    printf("  SKIP the real game: %s not found\n", GAME_SRC);
    return;
  }
  char ferr[256] = "";
  fonts_t *fonts = fonts_open("host/fonts", ferr, sizeof ferr);
  bool real_fonts = fonts != NULL;
  if (!fonts) fonts = fonts_open_stub(10.0f);
  font_class_t cls;
  font_class_for(&cls, 640, 480, NULL);
  lcd_t l;
  CHECK_INT(lcd_init(&l, 640, 480, fonts, &cls), 0);
  fonts_warm(fonts, &cls);
  lcd_t saved = L640;
  L640 = l;                                                  /* (game_frame resets L640.st) */

  game_slot_cases(src, len, &L640);

  /* angle mode, no AI, stock settings otherwise (harness.lua flies at 6:1) */
  game_reset_sd(src, len, "FPVSIM2 lay=3 mode=2 ai=0 twr=6 laps=2 ghost=0\n");
  log_clear();
  int64_t t0 = plat_now_us();
  etx_t *e = game_open(&L640, true);
  printf("  game: loaded in %.1f ms (%s fonts)\n", (plat_now_us() - t0) / 1000.0, real_fonts ? "Roboto" : "stub");
  nstats = 0;
  rejected_total = 0;
  int errors = 0;
  /* 600 frames through the menus */
  int script[600];
  for (int i = 0; i < 600; i++) script[i] = 0;
  int k = 20;
  for (int i = 0; i < 9; i++) { script[k] = ETX_EVT_NEXT; k += 2; }
  for (int i = 0; i < 9; i++) { script[k] = ETX_EVT_PREV; k += 2; }
  for (int i = 0; i < 5; i++) { script[k] = ETX_EVT_NEXT; k += 2; }
  int settings_at = k;
  script[k] = ETX_EVT_ENTER; k += 3;                         /* Settings */
  for (int item = 0; item < 4; item++) {                     /* Quad, Power, Flight mode, Rates: there and back */
    script[k] = ETX_EVT_ENTER; k += 2;
    for (int j = 0; j < 3; j++) { script[k] = ETX_EVT_NEXT; k += 2; }
    for (int j = 0; j < 3; j++) { script[k] = ETX_EVT_PREV; k += 2; }
    script[k] = ETX_EVT_ENTER; k += 2;
    script[k] = ETX_EVT_NEXT; k += 2;
  }
  int exit_at = k;
  script[k] = ETX_EVT_EXIT; k += 3;                          /* saves StickTime.dat, back to the menu */
  script[k] = ETX_EVT_PREV; k += 2;                          /* Track */
  script[k] = ETX_EVT_ENTER; k += 2;
  script[k] = ETX_EVT_NEXT; k += 2;
  script[k] = ETX_EVT_PREV; k += 2;
  script[k] = ETX_EVT_ENTER; k += 2;
  CHECK(k < 600);
  bool saw_setup = false;
  for (int i = 0; i < 600; i++) {
    int r = game_frame(e, script[i], IDLE);
    if (r != 0) { errors++; if (errors < 3) fprintf(stderr, "  frame %d: %d %s\n", i, r, etx_error(e)); }
    int st = etx_game_state(e);
    if (i > settings_at && i < exit_at && st == GS_SETUP) saw_setup = true;
  }
  CHECK(saw_setup);
  CHECK_INT(etx_game_state(e), GS_MENU);
  etx_flush_writes(e);
  size_t n = 0;
  char *dat = read_in(GAME_SD, "/SCRIPTS/TOOLS/StickTime.dat", &n);
  CHECK(dat && !strncmp(dat, "FPVSIM2 lay=3", 13) && strstr(dat, "mode=2"));
  free(dat);

  /* every track: start a race, fly a few seconds with the autopilot */
  int st[4];
  for (int t = 1; t <= 8; t++) {
    double tv = t, one = 1;
    CHECK_INT(etx_test_call(e, "track", &tv, 1, NULL, 0), 0);
    CHECK_INT(etx_test_call(e, "start", &one, 1, NULL, 0), 0);
    int flown = 0, state = 0, f = 0;
    for (; f < 60 * 20 && flown < 60 * 6; f++) {
      state = etx_game_state(e);
      memcpy(st, IDLE, sizeof st);
      if (state == GS_FLY) { autopilot(e, st); flown++; }
      int r = game_frame(e, 0, st);
      if (r != 0) { errors++; if (errors < 3) fprintf(stderr, "  track %d frame %d: %d %s\n", t, f, r, etx_error(e)); break; }
      if (state == GS_DONE) break;
    }
    double g[20], race[6];
    etx_test_call(e, "get", NULL, 0, g, 20);
    etx_test_call(e, "race", NULL, 0, race, 6);
    char name[64] = "";
    etx_test_call_str(e, "info", NULL, 0, 10, name, sizeof name);
    printf("  track %d %-14s flew %4.1f s: next gate %2d of %2d, lap %d, crashes %d\n", t, name, flown / 60.0,
           (int)g[17], (int)g[18], (int)g[16], (int)race[5]);
    CHECK(flown >= 60 * 5);
    CHECK(g[17] > 1 || g[16] > 0);                          /* passed a gate at least */
  }
  /* a whole race on track 1 (harness.lua scenario 2 through the C API): finished, no crash */
  {
    double tv = 1, one = 1;
    CHECK_INT(etx_test_call(e, "track", &tv, 1, NULL, 0), 0);
    CHECK_INT(etx_test_call(e, "start", &one, 1, NULL, 0), 0);
    int state = 0, f = 0;
    for (; f < 60 * 200; f++) {
      state = etx_game_state(e);
      if (state == GS_DONE) break;
      memcpy(st, IDLE, sizeof st);
      if (state == GS_FLY) autopilot(e, st);
      if (game_frame(e, 0, st) != 0) { errors++; fprintf(stderr, "  race: %s\n", etx_error(e)); break; }
    }
    double race[7] = { 0 };
    etx_test_call(e, "race", NULL, 0, race, 7);
    printf("  race on track 1: %s after %.1f s, %d laps, crashes %d\n", state == GS_DONE ? "finished" : "NOT finished",
           f / 60.0, (int)race[2], (int)race[5]);
    CHECK_INT(state, GS_DONE);
    CHECK_INT(race[5], 0);
    game_frame(e, ETX_EVT_EXIT, IDLE);                         /* DONE: EXIT goes to the menu */
    CHECK_INT(etx_game_state(e), GS_MENU);
    CHECK_INT(etx_test_call(e, "start", &one, 1, NULL, 0), 0);
    game_frame(e, 0, IDLE);
  }
  /* pause, resume, and EXIT in the main menu closes the script */
  game_frame(e, ETX_EVT_EXIT, IDLE);
  CHECK_INT(etx_game_state(e), GS_PAUSED);
  game_frame(e, ETX_EVT_EXIT, IDLE);
  CHECK(etx_game_state(e) != GS_PAUSED);
  double one = 1;
  etx_test_call(e, "state", &one, 1, NULL, 0);
  CHECK_INT(game_frame(e, ETX_EVT_EXIT, IDLE), 1);
  CHECK_INT(etx_run(e, 0), 1);
  CHECK_INT(errors, 0);
  CHECK_INT(rejected_total, 0);                              /* the game never draws a rejected line */
  etx_stats_t es;
  etx_stats(e, &es);
  qsort(stats_sorted, (size_t)nstats, sizeof stats_sorted[0], cmp_d);
  double sum = 0;
  for (int i = 0; i < nstats; i++) sum += stats_sorted[i];
  printf("  game: %d frames, run() avg %.2f ms, p50 %.2f, p95 %.2f, p99 %.2f, max %.2f ms; Lua %u KB, %.0f instr/frame\n",
         nstats, sum / nstats, stats_sorted[nstats / 2], stats_sorted[nstats * 95 / 100], stats_sorted[nstats * 99 / 100],
         stats_sorted[nstats - 1], (unsigned)es.lua_kb, (double)es.instr_count / (es.frames ? es.frames : 1));
  etx_free(e);
  etx_flush_writes(NULL);
  L640 = saved;
  lcd_free(&l);
  fonts_close(fonts);
  free(src);
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  plat_mutex_init(&log_mu);
  rm_rf(TMP);
  CHECK_INT(plat_mkdirs(SD), 0);
  plat_list_dir("host/tests/lua", copy_cb, NULL);
  stub_fonts = fonts_open_stub(10.0f);
  font_class_t cls;
  font_class_for(&cls, 640, 480, NULL);
  CHECK_INT(lcd_init(&L640, 640, 480, stub_fonts, &cls), 0);

  printf("paths\n"); test_paths();
  printf("api\n"); test_api();
  printf("io\n"); test_io();
  printf("loadScript\n"); test_loadscript();
  printf("errors\n"); test_errors();
  printf("hooks\n"); test_hooks();
  printf("globals\n"); test_globals();
  printf("slots\n"); test_slots();
  printf("game\n"); test_game();

  etx_flush_writes(NULL);
  lcd_free(&L640);
  fonts_close(stub_fonts);
  T_DONE();
}
