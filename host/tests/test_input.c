/* input.c: order / invert / input parsing, source updates and the mixer (simulated time). */
#include "t.h"
#include "input.h"

#include <stdint.h>

#define MS 1000LL
#define SEC 1000000LL

static void test_kind_names(void) {
  CHECK_STR(insrc_kind_name(INSRC_NONE), "none");
  CHECK_STR(insrc_kind_name(INSRC_KEYBOARD), "keyboard");
  CHECK_STR(insrc_kind_name(INSRC_WIFI), "wifi");
  CHECK_STR(insrc_kind_name(INSRC_USB), "usb");
  CHECK_STR(insrc_kind_name(INSRC_BLUETOOTH), "bluetooth");
  CHECK_STR(insrc_kind_name(INSRC_GAMEPAD), "gamepad");
  CHECK_STR(insrc_kind_name(INSRC_JOYSTICK), "joystick");
  CHECK_STR(insrc_kind_name(INSRC_KINDS), "none");
  CHECK_STR(insrc_kind_name((insrc_kind_t)99), "none");
  CHECK_STR(insrc_kind_name((insrc_kind_t)-1), "none");
}

static void test_parse_input(void) {
  CHECK_INT(mix_parse_input("auto"), INSRC_NONE);
  CHECK_INT(mix_parse_input("AUTO"), INSRC_NONE);
  CHECK_INT(mix_parse_input(""), INSRC_NONE);
  CHECK_INT(mix_parse_input(NULL), INSRC_NONE);
  CHECK_INT(mix_parse_input("none"), INSRC_NONE);
  CHECK_INT(mix_parse_input("wifi"), INSRC_WIFI);
  CHECK_INT(mix_parse_input("WiFi"), INSRC_WIFI);
  CHECK_INT(mix_parse_input("usb"), INSRC_USB);
  CHECK_INT(mix_parse_input("bluetooth"), INSRC_BLUETOOTH);
  CHECK_INT(mix_parse_input("keyboard"), INSRC_KEYBOARD);
  CHECK_INT(mix_parse_input("gamepad"), INSRC_GAMEPAD);
  CHECK_INT(mix_parse_input("JOYSTICK"), INSRC_JOYSTICK);
  CHECK_INT(mix_parse_input("ble"), INSRC_NONE);
  CHECK_INT(mix_parse_input("wifi "), INSRC_NONE);
  CHECK_INT(mix_parse_input(" wifi"), INSRC_NONE);
  CHECK_INT(mix_parse_input("wif"), INSRC_NONE);
  CHECK_INT(mix_parse_input("wifix"), INSRC_NONE);
}

static void test_parse_order(void) {
  char o[5];
  CHECK(mix_parse_order("AETR", o)); CHECK_STR(o, "AETR");
  CHECK(mix_parse_order("taer", o)); CHECK_STR(o, "TAER");
  CHECK(mix_parse_order("ReTa", o)); CHECK_STR(o, "RETA");
  CHECK(mix_parse_order("RTEA", o)); CHECK_STR(o, "RTEA");
  /* every permutation of the four letters, and nothing else of length 4 over them */
  const char L[] = "AETR";
  int perms = 0;
  for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) for (int c = 0; c < 4; c++) for (int d = 0; d < 4; d++) {
    char s[5] = { L[a], L[b], L[c], L[d], 0 };
    bool perm = a != b && a != c && a != d && b != c && b != d && c != d;
    memcpy(o, "????", 5);
    bool ok = mix_parse_order(s, o);
    CHECK(ok == perm);
    CHECK_STR(o, perm ? s : "AETR");
    perms += ok;
  }
  CHECK_INT(perms, 24);
  const char *bad[] = { "", "A", "AET", "AETRR", "AETX", "AAER", "AETR ", " AETR", "A,E,T,R", "aet", "abcd", "AETRAETR" };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    memcpy(o, "????", 5);
    CHECK(!mix_parse_order(bad[i], o));
    CHECK_STR(o, "AETR");                    /* always usable */
  }
  memcpy(o, "????", 5);
  CHECK(!mix_parse_order(NULL, o));
  CHECK_STR(o, "AETR");
}

static int inv_bits(const char *s) {
  bool inv[4] = { true, true, true, true };   /* must be cleared */
  mix_parse_invert(s, inv);
  return inv[0] | inv[1] << 1 | inv[2] << 2 | inv[3] << 3;
}

static void test_parse_invert(void) {
  enum { A = 1, E = 2, T = 4, R = 8 };
  CHECK_INT(inv_bits(NULL), 0);
  CHECK_INT(inv_bits(""), 0);
  CHECK_INT(inv_bits("   ,, ;"), 0);
  CHECK_INT(inv_bits("none"), 0);
  CHECK_INT(inv_bits("ail"), A);
  CHECK_INT(inv_bits("ail, thr"), A | T);
  CHECK_INT(inv_bits("ail,thr"), A | T);
  CHECK_INT(inv_bits("a,t"), A | T);
  CHECK_INT(inv_bits("roll/pitch/throttle/yaw"), A | E | T | R);
  CHECK_INT(inv_bits("AILERON"), A);
  CHECK_INT(inv_bits("ailerons"), A);
  CHECK_INT(inv_bits("elevator rudder"), E | R);
  CHECK_INT(inv_bits("ele;rud"), E | R);
  CHECK_INT(inv_bits("Thr"), T);
  CHECK_INT(inv_bits("at"), A | T);
  CHECK_INT(inv_bits("ET"), E | T);
  CHECK_INT(inv_bits("tear"), A | E | T | R);
  CHECK_INT(inv_bits("aa"), A);
  CHECK_INT(inv_bits("ail1thr"), A | T);      /* digits separate words */
  CHECK_INT(inv_bits("ail\tthr"), A | T);
  /* unknown words are ignored (settings_validate refuses them) */
  CHECK_INT(inv_bits("foo"), 0);
  CHECK_INT(inv_bits("ail, foo"), A);
  CHECK_INT(inv_bits("aetra"), 0);            /* five letters is not a letter list */
  CHECK_INT(inv_bits("ailx"), 0);
  CHECK_INT(inv_bits("x"), 0);
  CHECK_INT(inv_bits("ch1"), 0);
  CHECK_INT(inv_bits("averyveryverylongword thr"), T);   /* an over-long word is skipped, the rest counts */
  CHECK_INT(inv_bits("thr averyveryverylongword"), T);
  CHECK_INT(inv_bits("\xC3\xA4il thr"), T);   /* non-ASCII bytes separate words: "il" is no stick */
}

/* ------------------------------------------------------------------ insrc_update */
static void set4(int16_t *ch, int a, int b, int c, int d) { ch[0] = (int16_t)a; ch[1] = (int16_t)b; ch[2] = (int16_t)c; ch[3] = (int16_t)d; }

static void test_update(void) {
  insrc_t s;
  memset(&s, 0, sizeof s);
  s.present = true;
  int16_t ch[INPUT_CH + 4];
  for (int i = 0; i < INPUT_CH + 4; i++) ch[i] = (int16_t)(i * 10);
  ch[0] = 3000;                    /* clamped */
  ch[1] = -3000;
  insrc_update(&s, ch, 8, 100);
  CHECK_INT(s.nch, 8);
  CHECK_INT(s.ch[0], 1024);
  CHECK_INT(s.ch[1], -1024);
  CHECK_INT(s.ch[7], 70);
  CHECK_INT(s.ch[8], 0);           /* channels past nch are zero */
  CHECK_INT(s.ch[15], 0);
  CHECK_INT(s.last_us, 100);
  CHECK_INT(s.moved_us, 0);        /* the first sample is the reference, not a move */
  CHECK_INT(s.updates, 1);

  insrc_update(&s, ch, 40, 200);   /* more than INPUT_CH */
  CHECK_INT(s.nch, INPUT_CH);
  CHECK_INT(s.ch[15], 150);
  CHECK_INT(s.updates, 2);
  CHECK_INT(s.moved_us, 0);

  insrc_update(&s, ch, -3, 300);   /* negative count: no channels */
  CHECK_INT(s.nch, 0);
  CHECK_INT(s.ch[0], 0);
  CHECK_INT(s.ch[15], 0);
  CHECK_INT(s.moved_us, 300);      /* 1024 -> 0 is a move */
  insrc_update(&s, NULL, 8, 400);  /* no data: zeros */
  CHECK_INT(s.nch, 0);
  CHECK_INT(s.last_us, 400);
  CHECK_INT(s.moved_us, 300);

  /* moves are measured from the position at the last move: a slow drift is still a move */
  memset(&s, 0, sizeof s);
  s.present = true;
  int16_t c[INPUT_CH] = { 0 };
  insrc_update(&s, c, 16, 1000);
  int64_t t = 1000;
  for (int v = 1; v <= 51; v++) { set4(c, v, 0, 0, 0); insrc_update(&s, c, 16, t += 1000); }
  CHECK_INT(s.moved_us, 0);        /* exactly 51 from the reference: not yet */
  set4(c, 52, 0, 0, 0);
  insrc_update(&s, c, 16, t += 1000);
  CHECK_INT(s.moved_us, t);        /* 52: moved */
  int64_t moved_at = t;
  set4(c, 52 + 51, 0, 0, 0);       /* the reference is now 52 */
  insrc_update(&s, c, 16, t += 1000);
  CHECK_INT(s.moved_us, moved_at);
  set4(c, 52 - 52, 0, 0, 0);
  insrc_update(&s, c, 16, t += 1000);
  CHECK_INT(s.moved_us, t);        /* down by 52 */
  /* every one of the four sticks counts, in both directions */
  for (int k = 0; k < 4; k++) {
    int16_t base[INPUT_CH] = { 0 };
    memcpy(base, s.ch, sizeof base);
    int64_t before = s.moved_us;
    base[k] = (int16_t)(base[k] - 60);
    insrc_update(&s, base, 16, t += 1000);
    CHECK_INT(s.moved_us, t);
    CHECK(s.moved_us != before);
  }
  /* channels 5..16 (switches) are no stick move */
  memcpy(c, s.ch, sizeof c);
  int64_t before = s.moved_us;
  for (int k = 4; k < 16; k++) c[k] = 1000;
  insrc_update(&s, c, 16, t += 1000);
  CHECK_INT(s.moved_us, before);
  /* updating in place (ch == s->ch) works */
  s.ch[2] = (int16_t)(s.ch[2] + 500);
  insrc_update(&s, s.ch, s.nch, t += 1000);
  CHECK_INT(s.moved_us, t);
  CHECK_INT(s.nch, 16);

  /* a slot reset by its module (updates = 0) starts a new reference */
  int16_t big[INPUT_CH] = { 900, 900, 900, 900 };
  memset(&s, 0, sizeof s);
  s.present = true;
  insrc_update(&s, big, 4, 5000);
  CHECK_INT(s.moved_us, 0);
  CHECK_INT(s.updates, 1);
}

/* ------------------------------------------------------------------ the mixer */
static void src_init(insrc_t *s, insrc_kind_t kind, int nch) {
  memset(s, 0, sizeof *s);
  s->kind = kind;
  s->present = true;
  s->nch = (uint8_t)nch;
  s->fixed_aetr = kind == INSRC_GAMEPAD || kind == INSRC_KEYBOARD;
}

/* a source heard at `last` that last moved at `moved` */
static void src_at(insrc_t *s, int64_t last, int64_t moved) {
  s->last_us = last;
  s->moved_us = moved;
  s->updates = s->updates ? s->updates : 1;
}

static mix_config_t cfg_default(void) {
  mix_config_t c;
  memset(&c, 0, sizeof c);
  c.fixed = INSRC_NONE;
  memcpy(c.order, "AETR", 5);
  c.pause_ch = 5;
  return c;
}

static void check_idle(const sticks_t *o) {
  CHECK_INT(o->ail, 0);
  CHECK_INT(o->ele, 0);
  CHECK_INT(o->rud, 0);
  CHECK_INT(o->thr, -1024);
  CHECK_INT(o->pause, INT32_MIN);
  CHECK(!o->live);
}

static void test_mix_basic(void) {
  mix_config_t cfg = cfg_default();
  sticks_t o;
  memset(&o, 0x55, sizeof o);
  mix_sticks(&cfg, NULL, 0, -1, 0, &o);
  check_idle(&o);
  CHECK_INT(o.src, -1);
  CHECK_INT(o.kind, INSRC_NONE);
  mix_sticks(&cfg, NULL, -4, 3, 0, &o);       /* negative counts and a bogus cur are harmless */
  check_idle(&o);
  CHECK_INT(o.src, -1);

  insrc_t s[3];
  src_init(&s[0], INSRC_WIFI, 16);
  set4(s[0].ch, 100, 200, 300, 400);
  s[0].ch[4] = 777;
  src_at(&s[0], 10 * SEC, 0);
  mix_sticks(&cfg, s, 1, -1, 10 * SEC, &o);
  CHECK(o.live);
  CHECK_INT(o.src, 0);
  CHECK_INT(o.kind, INSRC_WIFI);
  CHECK_INT(o.ail, 100);
  CHECK_INT(o.ele, 200);
  CHECK_INT(o.thr, 300);
  CHECK_INT(o.rud, 400);
  CHECK_INT(o.pause, 777);

  /* channel order: letter i says what CHi+1 is */
  memcpy(cfg.order, "TAER", 5);
  mix_sticks(&cfg, s, 1, 0, 10 * SEC, &o);
  CHECK_INT(o.thr, 100);
  CHECK_INT(o.ail, 200);
  CHECK_INT(o.ele, 300);
  CHECK_INT(o.rud, 400);
  memcpy(cfg.order, "rtea", 5);               /* lower case works too */
  mix_sticks(&cfg, s, 1, 0, 10 * SEC, &o);
  CHECK_INT(o.rud, 100);
  CHECK_INT(o.thr, 200);
  CHECK_INT(o.ele, 300);
  CHECK_INT(o.ail, 400);
  memcpy(cfg.order, "AAER", 5);               /* not a permutation: AETR */
  mix_sticks(&cfg, s, 1, 0, 10 * SEC, &o);
  CHECK_INT(o.ail, 100);
  CHECK_INT(o.thr, 300);
  cfg.order[0] = 0;                           /* empty: AETR */
  mix_sticks(&cfg, s, 1, 0, 10 * SEC, &o);
  CHECK_INT(o.ail, 100);
  CHECK_INT(o.rud, 400);

  /* sources whose channels are already AETR ignore the order */
  memcpy(cfg.order, "TAER", 5);
  src_init(&s[1], INSRC_GAMEPAD, 4);
  set4(s[1].ch, -10, -20, -30, -40);
  src_at(&s[1], 10 * SEC, 0);
  mix_sticks(&cfg, s + 1, 1, -1, 10 * SEC, &o);
  CHECK_INT(o.ail, -10);
  CHECK_INT(o.ele, -20);
  CHECK_INT(o.thr, -30);
  CHECK_INT(o.rud, -40);
  CHECK_INT(o.kind, INSRC_GAMEPAD);
  src_init(&s[2], INSRC_KEYBOARD, 4);
  set4(s[2].ch, 1, 2, 3, 4);
  src_at(&s[2], 10 * SEC, 0);
  mix_sticks(&cfg, s + 2, 1, -1, 10 * SEC, &o);
  CHECK_INT(o.ail, 1);
  CHECK_INT(o.thr, 3);

  /* invert, after the order */
  cfg = cfg_default();
  memcpy(cfg.order, "TAER", 5);
  cfg.invert[2] = true;                       /* thr */
  cfg.invert[0] = true;                       /* ail */
  mix_sticks(&cfg, s, 1, 0, 10 * SEC, &o);
  CHECK_INT(o.thr, -100);
  CHECK_INT(o.ail, -200);
  CHECK_INT(o.ele, 300);
  CHECK_INT(o.rud, 400);
  CHECK_INT(o.pause, 777);                    /* the pause channel is never inverted */
  set4(s[0].ch, -1024, 1024, 0, 0);
  mix_sticks(&cfg, s, 1, 0, 10 * SEC, &o);
  CHECK_INT(o.thr, 1024);
  CHECK_INT(o.ail, -1024);

  /* out-of-range channel values written straight into a slot are clamped */
  cfg = cfg_default();
  set4(s[0].ch, 5000, -5000, 1025, -1025);
  s[0].ch[4] = 2000;
  mix_sticks(&cfg, s, 1, 0, 10 * SEC, &o);
  CHECK_INT(o.ail, 1024);
  CHECK_INT(o.ele, -1024);
  CHECK_INT(o.thr, 1024);
  CHECK_INT(o.rud, -1024);
  CHECK_INT(o.pause, 1024);
}

static void test_mix_missing_channels(void) {
  mix_config_t cfg = cfg_default();
  sticks_t o;
  insrc_t s;
  src_init(&s, INSRC_JOYSTICK, 2);
  set4(s.ch, 500, -500, 900, 900);            /* ch 3, 4 beyond nch are not used */
  src_at(&s, SEC, 0);
  mix_sticks(&cfg, &s, 1, -1, SEC, &o);
  CHECK(o.live);
  CHECK_INT(o.ail, 500);
  CHECK_INT(o.ele, -500);
  CHECK_INT(o.thr, -1024);                    /* a missing throttle stays low */
  CHECK_INT(o.rud, 0);
  CHECK_INT(o.pause, INT32_MIN);              /* pause_ch 5 > nch */
  cfg.invert[2] = cfg.invert[3] = true;       /* inverting a missing throttle is no full throttle */
  mix_sticks(&cfg, &s, 1, -1, SEC, &o);
  CHECK_INT(o.thr, -1024);
  CHECK_INT(o.rud, 0);
  memcpy(cfg.order, "TAER", 5);               /* CH1 = thr is there: inverted */
  mix_sticks(&cfg, &s, 1, -1, SEC, &o);
  CHECK_INT(o.thr, -500);
  CHECK_INT(o.ail, -500);
  CHECK_INT(o.ele, 0);
  /* no channels at all */
  s.nch = 0;
  cfg = cfg_default();
  mix_sticks(&cfg, &s, 1, -1, SEC, &o);
  CHECK(o.live);
  CHECK_INT(o.ail, 0);
  CHECK_INT(o.thr, -1024);
}

static void test_mix_pause_channel(void) {
  mix_config_t cfg = cfg_default();
  sticks_t o;
  insrc_t s;
  src_init(&s, INSRC_WIFI, 16);
  for (int i = 0; i < 16; i++) s.ch[i] = (int16_t)(i * 50 - 400);
  src_at(&s, SEC, 0);
  for (int pc = 1; pc <= 16; pc++) {
    cfg.pause_ch = pc;
    mix_sticks(&cfg, &s, 1, -1, SEC, &o);
    CHECK_INT(o.pause, (pc - 1) * 50 - 400);
  }
  cfg.pause_ch = 0;
  mix_sticks(&cfg, &s, 1, -1, SEC, &o);
  CHECK_INT(o.pause, INT32_MIN);
  cfg.pause_ch = 17;
  mix_sticks(&cfg, &s, 1, -1, SEC, &o);
  CHECK_INT(o.pause, INT32_MIN);
  cfg.pause_ch = -1;
  mix_sticks(&cfg, &s, 1, -1, SEC, &o);
  CHECK_INT(o.pause, INT32_MIN);
  cfg.pause_ch = 8;
  s.nch = 8;
  mix_sticks(&cfg, &s, 1, -1, SEC, &o);
  CHECK_INT(o.pause, 7 * 50 - 400);
  s.nch = 7;
  mix_sticks(&cfg, &s, 1, -1, SEC, &o);
  CHECK_INT(o.pause, INT32_MIN);
  /* stale: no pause value either */
  s.nch = 16;
  mix_sticks(&cfg, &s, 1, 0, SEC + INPUT_STALE_US + 1, &o);
  CHECK_INT(o.pause, INT32_MIN);
}

static void test_mix_stale(void) {
  mix_config_t cfg = cfg_default();
  sticks_t o;
  insrc_t s[2];
  src_init(&s[0], INSRC_WIFI, 16);
  set4(s[0].ch, 600, 600, 600, 600);
  src_at(&s[0], 2 * SEC, 0);
  mix_sticks(&cfg, s, 1, -1, 2 * SEC + INPUT_STALE_US, &o);     /* exactly the limit: still live */
  CHECK(o.live);
  CHECK_INT(o.thr, 600);
  mix_sticks(&cfg, s, 1, 0, 2 * SEC + INPUT_STALE_US + 1, &o);  /* one us later: stale */
  check_idle(&o);
  CHECK_INT(o.src, 0);                       /* still reported: what was active */
  CHECK_INT(o.kind, INSRC_WIFI);
  mix_sticks(&cfg, s, 1, -1, 60 * SEC, &o);  /* no cur: the last one heard */
  check_idle(&o);
  CHECK_INT(o.src, 0);

  /* not present / never updated: as if there were no source */
  s[0].present = false;
  mix_sticks(&cfg, s, 1, 0, 2 * SEC, &o);
  check_idle(&o);
  CHECK_INT(o.src, -1);
  CHECK_INT(o.kind, INSRC_NONE);
  src_init(&s[0], INSRC_USB, 8);
  s[0].last_us = 2 * SEC;                    /* updates == 0 */
  mix_sticks(&cfg, s, 1, -1, 2 * SEC, &o);
  check_idle(&o);
  CHECK_INT(o.src, -1);

  /* two stale sources, no cur: the one heard last is reported */
  src_init(&s[0], INSRC_USB, 8);
  src_at(&s[0], 1 * SEC, 0);
  src_init(&s[1], INSRC_WIFI, 8);
  src_at(&s[1], 3 * SEC, 0);
  mix_sticks(&cfg, s, 2, -1, 10 * SEC, &o);
  check_idle(&o);
  CHECK_INT(o.src, 1);
  mix_sticks(&cfg, s, 2, 0, 10 * SEC, &o);   /* with cur: cur */
  CHECK_INT(o.src, 0);
  CHECK_INT(o.kind, INSRC_USB);
}

static void test_mix_auto(void) {
  mix_config_t cfg = cfg_default();
  sticks_t o;
  insrc_t s[4];
  src_init(&s[0], INSRC_WIFI, 16);
  set4(s[0].ch, 11, 12, 13, 14);
  src_init(&s[1], INSRC_GAMEPAD, 4);
  set4(s[1].ch, 21, 22, 23, 24);
  src_init(&s[2], INSRC_USB, 8);
  set4(s[2].ch, 31, 32, 33, 34);
  src_init(&s[3], INSRC_KEYBOARD, 4);
  int64_t now = 100 * SEC;

  /* nobody moved: the newest update wins when there is no current source */
  src_at(&s[0], now - 10 * MS, 0);
  src_at(&s[1], now - 2 * MS, 0);
  src_at(&s[2], now - 5 * MS, 0);
  mix_sticks(&cfg, s, 3, -1, now, &o);
  CHECK_INT(o.src, 1);
  CHECK_INT(o.ail, 21);
  /* ...and a current source is kept while nobody moved (no flapping between streams) */
  src_at(&s[0], now, 0);
  mix_sticks(&cfg, s, 3, 1, now, &o);
  CHECK_INT(o.src, 1);
  mix_sticks(&cfg, s, 3, 2, now, &o);
  CHECK_INT(o.src, 2);

  /* no current: the newest mover wins, ties broken by the newest update */
  src_at(&s[0], now, now - 3 * SEC);
  src_at(&s[1], now, now - 1 * SEC);
  src_at(&s[2], now, now - 2 * SEC);
  mix_sticks(&cfg, s, 3, -1, now, &o);
  CHECK_INT(o.src, 1);
  src_at(&s[2], now, now - 1 * SEC);
  src_at(&s[1], now - 1 * MS, now - 1 * SEC);
  mix_sticks(&cfg, s, 3, -1, now, &o);
  CHECK_INT(o.src, 2);

  /* keep the current source unless another moved more than 1 s more recently */
  src_at(&s[0], now, now - 2 * SEC);         /* current */
  src_at(&s[1], now, now - 1500 * MS);       /* 0.5 s newer */
  src_at(&s[2], now, 0);
  mix_sticks(&cfg, s, 3, 0, now, &o);
  CHECK_INT(o.src, 0);
  CHECK_INT(o.ail, 11);
  src_at(&s[1], now, now - 1 * SEC);         /* exactly 1 s newer: still kept */
  mix_sticks(&cfg, s, 3, 0, now, &o);
  CHECK_INT(o.src, 0);
  src_at(&s[1], now, now - 1 * SEC + 1);     /* 1 s + 1 us newer: taken over */
  mix_sticks(&cfg, s, 3, 0, now, &o);
  CHECK_INT(o.src, 1);
  CHECK_INT(o.ail, 21);
  CHECK_INT(o.kind, INSRC_GAMEPAD);
  /* the new current is kept in turn */
  src_at(&s[0], now, now - 500 * MS);
  mix_sticks(&cfg, s, 3, 1, now, &o);
  CHECK_INT(o.src, 1);
  /* an older mover never takes over */
  src_at(&s[0], now, now - 9 * SEC);
  mix_sticks(&cfg, s, 3, 1, now, &o);
  CHECK_INT(o.src, 1);

  /* a current source that never moved yields to any source that did */
  src_at(&s[0], now, 0);
  src_at(&s[2], now, now - 50 * SEC);
  src_at(&s[1], now, 0);
  mix_sticks(&cfg, s, 3, 0, now, &o);
  CHECK_INT(o.src, 2);

  /* the current source goes stale: the newest live mover takes over at once */
  src_at(&s[0], now - INPUT_STALE_US - 1, now - 1 * MS);
  src_at(&s[1], now, now - 30 * SEC);
  src_at(&s[2], now, now - 40 * SEC);
  mix_sticks(&cfg, s, 3, 0, now, &o);
  CHECK(o.live);
  CHECK_INT(o.src, 1);
  /* a stale source that moved last is never picked */
  mix_sticks(&cfg, s, 3, -1, now, &o);
  CHECK_INT(o.src, 1);
  /* a source that disappears (present = false) is dropped even as cur */
  src_at(&s[0], now, now);
  s[0].present = false;
  mix_sticks(&cfg, s, 3, 0, now, &o);
  CHECK_INT(o.src, 1);
  s[0].present = true;

  /* cur out of range */
  mix_sticks(&cfg, s, 3, 7, now, &o);
  CHECK(o.src >= 0 && o.src < 3);
  mix_sticks(&cfg, s, 3, -9, now, &o);
  CHECK_INT(o.src, 0);                       /* s[0] moved at `now` */

  /* n is capped at INPUT_MAX_SOURCES */
  insrc_t many[INPUT_MAX_SOURCES + 2];
  for (int i = 0; i < INPUT_MAX_SOURCES + 2; i++) {
    src_init(&many[i], INSRC_JOYSTICK, 4);
    src_at(&many[i], now, now - (INPUT_MAX_SOURCES + 2 - i) * SEC);
  }
  mix_sticks(&cfg, many, INPUT_MAX_SOURCES + 2, -1, now, &o);
  CHECK_INT(o.src, INPUT_MAX_SOURCES - 1);   /* the two newest movers are past the cap */
  mix_sticks(&cfg, many, INPUT_MAX_SOURCES + 2, INPUT_MAX_SOURCES + 1, now, &o);
  CHECK_INT(o.src, INPUT_MAX_SOURCES - 1);
}

static void test_mix_fixed(void) {
  mix_config_t cfg = cfg_default();
  sticks_t o;
  insrc_t s[3];
  int64_t now = 50 * SEC;
  src_init(&s[0], INSRC_GAMEPAD, 4);
  set4(s[0].ch, 1, 1, 1, 1);
  src_at(&s[0], now, now);                   /* the newest mover */
  src_init(&s[1], INSRC_WIFI, 16);
  set4(s[1].ch, 2, 2, 2, 2);
  src_at(&s[1], now, 0);                     /* never moved */
  src_init(&s[2], INSRC_WIFI, 16);
  set4(s[2].ch, 3, 3, 3, 3);
  src_at(&s[2], now - 1 * MS, now - 5 * SEC);
  cfg.fixed = INSRC_WIFI;
  mix_sticks(&cfg, s, 3, -1, now, &o);
  CHECK_INT(o.src, 2);                       /* among wifi sources, auto rules apply */
  CHECK_INT(o.kind, INSRC_WIFI);
  mix_sticks(&cfg, s, 3, 0, now, &o);        /* cur of another kind is dropped */
  CHECK_INT(o.src, 2);
  mix_sticks(&cfg, s, 3, 1, now, &o);        /* cur that never moved yields */
  CHECK_INT(o.src, 2);
  /* the fixed kind goes stale: centered, even though the gamepad is live and moving */
  src_at(&s[1], now - SEC, 0);
  src_at(&s[2], now - SEC, now - 5 * SEC);
  mix_sticks(&cfg, s, 3, 2, now, &o);
  check_idle(&o);
  CHECK_INT(o.src, 2);
  CHECK_INT(o.kind, INSRC_WIFI);
  mix_sticks(&cfg, s, 3, 0, now, &o);        /* cur gamepad: never reported for a wifi setting */
  CHECK(o.src == 1 || o.src == 2);
  CHECK_INT(o.kind, INSRC_WIFI);
  /* a fixed kind with no source at all */
  cfg.fixed = INSRC_BLUETOOTH;
  mix_sticks(&cfg, s, 3, 0, now, &o);
  check_idle(&o);
  CHECK_INT(o.src, -1);
  CHECK_INT(o.kind, INSRC_NONE);
  /* back to auto: the gamepad */
  cfg.fixed = INSRC_NONE;
  mix_sticks(&cfg, s, 3, -1, now, &o);
  CHECK_INT(o.src, 0);
  CHECK_INT(o.kind, INSRC_GAMEPAD);
}

/* the update + mixer together, as the host runs them: a pilot moving one radio, then another */
static void test_takeover_scenario(void) {
  mix_config_t cfg = cfg_default();
  insrc_t s[2];
  src_init(&s[0], INSRC_WIFI, 16);
  src_init(&s[1], INSRC_USB, 8);
  s[0].updates = s[1].updates = 0;
  int16_t a[INPUT_CH] = { 0, 0, -1024, 0 }, b[INPUT_CH] = { 0, 0, -1024, 0 };
  int cur = -1;
  sticks_t o;
  int64_t t = 0;
  int switched_at = -1;
  for (int ms = 0; ms < 6000; ms += 4) {
    t = ms * MS;
    /* radio A flies for 2 s, then its sticks rest; radio B starts moving at 2.5 s */
    if (ms < 2000) a[0] = (int16_t)((ms % 400) * 4 - 800);
    if (ms >= 2500) b[1] = (int16_t)((ms % 400) * 4 - 800);
    insrc_update(&s[0], a, 16, t);
    insrc_update(&s[1], b, 8, t);
    mix_sticks(&cfg, s, 2, cur, t, &o);
    if (cur == 0 && o.src == 1 && switched_at < 0) switched_at = ms;
    cur = o.src;
    if (ms == 1000) CHECK_INT(o.src, 0);
    if (ms == 2800) CHECK_INT(o.src, 0);     /* B moved, but A did only 0.8 s before */
  }
  CHECK_INT(cur, 1);
  /* A last moved shortly before 2000 ms: B takes over just after 3000 ms */
  CHECK(switched_at > 2900 && switched_at <= 3100);
  /* A falls silent: B stays, and A coming back still doesn't take over without moving */
  for (int ms = 6000; ms < 8000; ms += 4) {
    t = ms * MS;
    if (ms >= 7000) insrc_update(&s[0], a, 16, t);
    insrc_update(&s[1], b, 8, t);
    mix_sticks(&cfg, s, 2, cur, t, &o);
    cur = o.src;
    if (ms == 6600) CHECK(!s[0].present || t - s[0].last_us > INPUT_STALE_US);
  }
  CHECK_INT(cur, 1);
  CHECK(o.live);
  /* B has rested since 6 s; A moving again at 8 s is 2 s ahead: it takes over on its first move */
  int64_t b_moved = s[1].moved_us;
  CHECK(b_moved > 5900 * MS && b_moved < 6000 * MS);
  switched_at = -1;
  for (int ms = 8000; ms < 10000; ms += 4) {
    t = ms * MS;
    a[3] = (int16_t)((ms % 400) * 4 - 800);
    insrc_update(&s[0], a, 16, t);
    insrc_update(&s[1], b, 8, t);
    mix_sticks(&cfg, s, 2, cur, t, &o);
    if (cur == 1 && o.src == 0 && switched_at < 0) switched_at = ms;
    cur = o.src;
  }
  CHECK_INT(cur, 0);
  CHECK_INT(switched_at, 8000);
}

/* ------------------------------------------------------------------ stick_channels */
static int chans(const char *s, uint8_t c[4]) {
  memset(c, 0x77, 4);
  return mix_parse_channels(s, c);
}

static void test_parse_channels(void) {
  uint8_t c[4];
  CHECK_INT(chans("1 2 3 4", c), 1);
  CHECK(c[0] == 1 && c[1] == 2 && c[2] == 3 && c[3] == 4);
  CHECK_INT(chans("16,15,14,13", c), 1);
  CHECK(c[0] == 16 && c[1] == 15 && c[2] == 14 && c[3] == 13);
  CHECK_INT(chans(" ch3, CH1 ,02\t4 ", c), 1);
  CHECK(c[0] == 3 && c[1] == 1 && c[2] == 2 && c[3] == 4);
  CHECK_INT(chans(",5,6,7,8,", c), 1);
  CHECK(c[0] == 5 && c[3] == 8);
  /* none: use channel_order */
  static const char *none[] = { "", " ", ",", " , \t,", NULL };
  for (int i = 0; none[i]; i++) {
    CHECK_INT(chans(none[i], c), 0);
    CHECK(c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0);
  }
  CHECK_INT(chans(NULL, c), 0);
  static const char *bad[] = { "1 2 3", "1 2 3 4 5", "1 1 2 3", "1 2 3 01", "0 1 2 3", "17 1 2 3", "1 2 3 x",
                               "1-2-3-4", "1;2;3;4", "1234", "ch 1 2 3", "c1 2 3 4", "001 2 3 4", "1.0 2 3 4",
                               "+1 2 3 4", "1 2 3 4x", "AETR", "1 2 3 99", "-1 2 3 4", NULL };
  for (int i = 0; bad[i]; i++) {
    if (chans(bad[i], c) != -1) fprintf(stderr, "stick_channels \"%s\" should be invalid\n", bad[i]);
    CHECK_INT(chans(bad[i], c), -1);
    CHECK(c[0] == 0 && c[1] == 0 && c[2] == 0 && c[3] == 0);
  }
}

static mix_config_t cfg_chan(int a, int e, int t, int r) {
  mix_config_t c = cfg_default();
  c.chan[0] = (uint8_t)a; c.chan[1] = (uint8_t)e; c.chan[2] = (uint8_t)t; c.chan[3] = (uint8_t)r;
  return c;
}

static void test_mix_channels(void) {
  sticks_t o;
  insrc_t s[3];
  src_init(&s[0], INSRC_WIFI, 16);
  for (int i = 0; i < 16; i++) s[0].ch[i] = (int16_t)((i + 1) * 10);     /* CHn = n * 10 */
  src_at(&s[0], 10 * SEC, 0);

  /* each stick reads its channel */
  mix_config_t cfg = cfg_chan(6, 2, 7, 12);
  mix_sticks(&cfg, s, 1, -1, 10 * SEC, &o);
  CHECK(o.live);
  CHECK_INT(o.ail, 60);
  CHECK_INT(o.ele, 20);
  CHECK_INT(o.thr, 70);
  CHECK_INT(o.rud, 120);
  CHECK_INT(o.pause, 50);                      /* the pause switch stays CH5 */

  /* it overrides channel_order */
  memcpy(cfg.order, "TAER", 5);
  mix_sticks(&cfg, s, 1, 0, 10 * SEC, &o);
  CHECK_INT(o.ail, 60);
  CHECK_INT(o.thr, 70);
  cfg = cfg_chan(1, 2, 3, 4);                  /* "1 2 3 4" with TAER: still AETR */
  memcpy(cfg.order, "TAER", 5);
  mix_sticks(&cfg, s, 1, 0, 10 * SEC, &o);
  CHECK_INT(o.ail, 10);
  CHECK_INT(o.ele, 20);
  CHECK_INT(o.thr, 30);
  CHECK_INT(o.rud, 40);
  cfg = cfg_chan(3, 1, 2, 4);                  /* TAER as channels */
  mix_sticks(&cfg, s, 1, 0, 10 * SEC, &o);
  CHECK_INT(o.ail, 30);
  CHECK_INT(o.ele, 10);
  CHECK_INT(o.thr, 20);
  CHECK_INT(o.rud, 40);

  /* invert applies per stick, after the mapping */
  cfg = cfg_chan(6, 2, 7, 12);
  cfg.invert[0] = cfg.invert[2] = true;
  mix_sticks(&cfg, s, 1, 0, 10 * SEC, &o);
  CHECK_INT(o.ail, -60);
  CHECK_INT(o.ele, 20);
  CHECK_INT(o.thr, -70);
  CHECK_INT(o.rud, 120);
  CHECK_INT(o.pause, 50);

  /* a channel the source doesn't have: centred, throttle low (and an inverted missing throttle too) */
  src_init(&s[1], INSRC_USB, 8);
  for (int i = 0; i < 16; i++) s[1].ch[i] = 500;
  src_at(&s[1], 10 * SEC, 0);
  cfg = cfg_chan(1, 9, 12, 8);
  cfg.invert[2] = true;
  mix_sticks(&cfg, s + 1, 1, -1, 10 * SEC, &o);
  CHECK_INT(o.ail, 500);
  CHECK_INT(o.ele, 0);
  CHECK_INT(o.thr, -1024);
  CHECK_INT(o.rud, 500);

  /* gamepads and the keyboard keep their own layout */
  cfg = cfg_chan(4, 3, 2, 1);
  src_init(&s[2], INSRC_GAMEPAD, 7);
  set4(s[2].ch, -10, -20, -30, -40);
  src_at(&s[2], 10 * SEC, 0);
  mix_sticks(&cfg, s + 2, 1, -1, 10 * SEC, &o);
  CHECK_INT(o.ail, -10);
  CHECK_INT(o.ele, -20);
  CHECK_INT(o.thr, -30);
  CHECK_INT(o.rud, -40);
  src_init(&s[2], INSRC_KEYBOARD, 4);
  set4(s[2].ch, 1, 2, 3, 4);
  src_at(&s[2], 10 * SEC, 0);
  cfg.invert[3] = true;
  mix_sticks(&cfg, s + 2, 1, -1, 10 * SEC, &o);
  CHECK_INT(o.ail, 1);
  CHECK_INT(o.thr, 3);
  CHECK_INT(o.rud, -4);                        /* invert still applies */

  /* bad maps (a channel twice, out of range, partly set) fall back to channel_order */
  static const uint8_t badmaps[][4] = { { 1, 1, 2, 3 }, { 0, 2, 3, 4 }, { 1, 2, 3, 17 }, { 5, 0, 0, 0 }, { 255, 2, 3, 4 } };
  for (size_t i = 0; i < sizeof badmaps / sizeof badmaps[0]; i++) {
    cfg = cfg_default();
    memcpy(cfg.order, "TAER", 5);
    memcpy(cfg.chan, badmaps[i], 4);
    mix_sticks(&cfg, s, 1, 0, 10 * SEC, &o);
    CHECK_INT(o.thr, 10);
    CHECK_INT(o.ail, 20);
    CHECK_INT(o.ele, 30);
    CHECK_INT(o.rud, 40);
  }

  /* moves count on the mapped channels (a stick on CH6 moves the source; CH1 is now a switch) */
  insrc_t r, pad;
  memset(&r, 0, sizeof r);
  r.kind = INSRC_WIFI;
  r.present = true;
  memset(&pad, 0, sizeof pad);
  pad.kind = INSRC_GAMEPAD;
  pad.present = true;
  pad.fixed_aetr = true;
  int16_t c[INPUT_CH] = { 0 };
  cfg = cfg_chan(5, 6, 7, 8);
  mix_sticks(&cfg, &r, 1, -1, 0, &o);
  insrc_update(&r, c, 16, 1000);
  insrc_update(&pad, c, 7, 1000);
  c[0] = 1000;                                 /* CH1 */
  insrc_update(&r, c, 16, 2000);
  insrc_update(&pad, c, 7, 2000);
  CHECK_INT(r.moved_us, 0);
  CHECK_INT(pad.moved_us, 2000);               /* a gamepad's CH1 is its aileron */
  c[5] = -600;                                 /* CH6 = elevator */
  insrc_update(&r, c, 16, 3000);
  insrc_update(&pad, c, 7, 3000);
  CHECK_INT(r.moved_us, 3000);
  CHECK_INT(pad.moved_us, 2000);               /* its CH6 is a trigger */
  /* back to channel_order: CH1-4 again */
  cfg = cfg_default();
  mix_sticks(&cfg, &r, 1, -1, 3000, &o);
  c[7] = 900;
  insrc_update(&r, c, 16, 4000);
  CHECK_INT(r.moved_us, 3000);
  c[1] = 900;
  insrc_update(&r, c, 16, 5000);
  CHECK_INT(r.moved_us, 5000);
}

int main(void) {
  test_kind_names();
  test_parse_input();
  test_parse_order();
  test_parse_invert();
  test_parse_channels();
  test_update();
  test_mix_basic();
  test_mix_missing_channels();
  test_mix_pause_channel();
  test_mix_stale();
  test_mix_auto();
  test_mix_fixed();
  test_takeover_scenario();
  test_mix_channels();
  test_takeover_scenario();                    /* again after a stick_channels map (back to CH1-4) */
  T_DONE();
}
