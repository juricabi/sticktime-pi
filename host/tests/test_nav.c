/* nav.c: menus by sticks, the pause switch and the signal-loss pause, with simulated time.
 * The last test drives a model of the game's event handling (game-api notes §2.2) with random
 * sticks and keys, and checks the editing inference and "no EXIT in the menu" on every frame. */
#include "t.h"
#include "nav.h"
#include "etx.h"

#include <stdint.h>

#define MS 1000LL
#define PCT(p) ((p) * 1024 / 100)

static const int ENTER = ETX_EVT_ENTER, EXIT = ETX_EVT_EXIT, PREV = ETX_EVT_PREV, NEXT = ETX_EVT_NEXT;

static sticks_t stk(int ail, int ele) {
  sticks_t s;
  memset(&s, 0, sizeof s);
  s.ail = ail;
  s.ele = ele;
  s.thr = -1024;
  s.pause = INT32_MIN;
  s.src = 0;
  s.kind = INSRC_WIFI;
  s.live = true;
  return s;
}

static sticks_t sw(int pause) {          /* centered sticks with a pause channel value */
  sticks_t s = stk(0, 0);
  s.pause = pause;
  return s;
}

static sticks_t lost(void) {
  sticks_t s = stk(0, 0);
  s.live = false;
  s.src = 0;
  return s;
}

static nav_config_t cfg(void) {
  nav_config_t c;
  nav_default_config(&c);
  return c;
}

/* a nav that is in `state` and armed (sticks were centered there) */
static void armed_in(nav_t *n, const nav_config_t *c, int state, int64_t t) {
  nav_init(n);
  sticks_t s = stk(0, 0);
  CHECK_INT(nav_update(n, c, state, &s, t), 0);
  if (state == GS_MENU || state == GS_SETUP || state == GS_PAUSED || state == GS_DONE) CHECK(n->armed);
}

static void test_defaults(void) {
  nav_config_t c;
  memset(&c, 0x7f, sizeof c);
  nav_default_config(&c);
  CHECK(c.enabled);
  CHECK_INT(c.on_pct, 60);
  CHECK_INT(c.off_pct, 40);
  CHECK_INT(c.repeat_first_ms, 450);
  CHECK_INT(c.repeat_ms, 150);
  CHECK(c.pause_on_signal_loss);
  nav_t n;
  memset(&n, 0x7f, sizeof n);
  nav_init(&n);
  CHECK_INT(n.state, GS_UNKNOWN);
  CHECK_INT(n.prev_state, GS_MENU);
  CHECK(!n.editing);
  CHECK(!n.armed);
  CHECK_INT(n.pitch_dir, 0);
  CHECK_INT(n.roll_dir, 0);
  CHECK_INT(n.pause_pos, 0);
  CHECK(!n.was_live);
  CHECK(!n.loss_sent);
}

static void test_pitch_event(void) {
  nav_t n;
  nav_init(&n);
  CHECK_INT(nav_pitch_event(&n, -1), PREV);   /* up: focus up */
  CHECK_INT(nav_pitch_event(&n, +1), NEXT);   /* down: focus down */
  CHECK_INT(nav_pitch_event(&n, 0), 0);
  CHECK_INT(nav_pitch_event(&n, -5), PREV);
  CHECK_INT(nav_pitch_event(&n, 7), NEXT);
  n.editing = true;
  CHECK_INT(nav_pitch_event(&n, -1), 0x1E00); /* up: INC (value up) */
  CHECK_INT(nav_pitch_event(&n, +1), 0x1D00); /* down: DEC */
  CHECK_INT(nav_pitch_event(&n, 0), 0);
}

static void test_arming_at_start(void) {
  nav_config_t c = cfg();
  nav_t n;
  nav_init(&n);
  sticks_t up = stk(0, 1024), mid = stk(0, 0), right = stk(1024, 0);
  CHECK_INT(nav_update(&n, &c, GS_MENU, &up, 0), 0);          /* held from before: nothing */
  CHECK_INT(nav_update(&n, &c, GS_MENU, &up, 500 * MS), 0);
  CHECK_INT(nav_update(&n, &c, GS_MENU, &right, 600 * MS), 0);
  CHECK(!n.armed);
  CHECK_INT(nav_update(&n, &c, GS_MENU, &mid, 700 * MS), 0);
  CHECK(n.armed);
  CHECK_INT(nav_update(&n, &c, GS_MENU, &up, 716 * MS), PREV);
}

/* events of holding `s` from t0 for dur, one frame per ms; times (relative, ms) into out */
static int hold(nav_t *n, const nav_config_t *c, int state, sticks_t s, int64_t t0, int dur_ms, int ev_want, int *out, int max) {
  int k = 0;
  for (int ms = 0; ms < dur_ms; ms++) {
    int ev = nav_update(n, c, state, &s, t0 + ms * MS);
    if (!ev) continue;
    CHECK_INT(ev, ev_want);
    if (k < max) out[k] = ms;
    k++;
  }
  return k;
}

static void test_repeat_timing(void) {
  nav_config_t c = cfg();
  nav_t n;
  int t[32];
  armed_in(&n, &c, GS_MENU, 0);
  int k = hold(&n, &c, GS_MENU, stk(0, 1024), 1000 * MS, 1000, PREV, t, 32);
  CHECK_INT(k, 5);                             /* 0, 450, 600, 750, 900 */
  CHECK_INT(t[0], 0);
  CHECK_INT(t[1], 450);
  CHECK_INT(t[2], 600);
  CHECK_INT(t[3], 750);
  CHECK_INT(t[4], 900);
  /* released: nothing; pushed again: at once */
  CHECK_INT(hold(&n, &c, GS_MENU, stk(0, 0), 2000 * MS, 500, 0, t, 32), 0);
  k = hold(&n, &c, GS_MENU, stk(0, 1024), 2500 * MS, 451, PREV, t, 32);
  CHECK_INT(k, 2);
  CHECK_INT(t[0], 0);
  CHECK_INT(t[1], 450);
  /* straight from up to down (through the center in one frame) */
  k = hold(&n, &c, GS_MENU, stk(0, -1024), 2951 * MS, 700, NEXT, t, 32);
  CHECK_INT(k, 3);
  CHECK_INT(t[0], 0);
  CHECK_INT(t[1], 450);
  CHECK_INT(t[2], 600);

  /* other timings */
  c.repeat_first_ms = 300;
  c.repeat_ms = 100;
  armed_in(&n, &c, GS_SETUP, 0);
  k = hold(&n, &c, GS_SETUP, stk(0, -800), 100 * MS, 650, NEXT, t, 32);
  CHECK_INT(k, 5);                             /* 0, 300, 400, 500, 600 */
  CHECK_INT(t[1], 300);
  CHECK_INT(t[4], 600);
  c.repeat_ms = 0;                             /* 0: every frame after the first delay */
  armed_in(&n, &c, GS_SETUP, 0);
  k = hold(&n, &c, GS_SETUP, stk(0, -800), 100 * MS, 310, NEXT, t, 32);
  CHECK_INT(k, 1 + 10);
  c.repeat_first_ms = 0;
  armed_in(&n, &c, GS_SETUP, 0);
  sticks_t d = stk(0, -800);
  CHECK_INT(nav_update(&n, &c, GS_SETUP, &d, 10 * MS), NEXT);
  CHECK_INT(nav_update(&n, &c, GS_SETUP, &d, 10 * MS + 1), NEXT);

  /* at 60 fps: the repeats land on the first frame at or after each due time */
  c = cfg();
  armed_in(&n, &c, GS_PAUSED, 0);
  sticks_t up = stk(0, 1024);
  int fired[8], nf = 0;
  for (int f = 0; f < 60; f++) {
    int64_t now = 1000 * MS + (int64_t)f * 16667;
    if (nav_update(&n, &c, GS_PAUSED, &up, now) && nf < 8) fired[nf++] = f;
  }
  CHECK_INT(nf, 5);
  CHECK_INT(fired[0], 0);
  CHECK_INT(fired[1], 27);                     /* 450 ms = 27 frames */
  CHECK_INT(fired[2], 36);                     /* +150 ms = 9 frames */
  CHECK_INT(fired[3], 45);
  CHECK_INT(fired[4], 54);

  /* a stall: one event, then the next repeat_ms later (no burst of the missed ones) */
  armed_in(&n, &c, GS_MENU, 0);
  CHECK_INT(nav_update(&n, &c, GS_MENU, &up, 100 * MS), PREV);
  CHECK_INT(nav_update(&n, &c, GS_MENU, &up, 3100 * MS), PREV);
  CHECK_INT(nav_update(&n, &c, GS_MENU, &up, 3101 * MS), 0);
  CHECK_INT(nav_update(&n, &c, GS_MENU, &up, 3249 * MS), 0);
  CHECK_INT(nav_update(&n, &c, GS_MENU, &up, 3250 * MS), PREV);
  /* a frame late: the schedule stays on its grid */
  CHECK_INT(nav_update(&n, &c, GS_MENU, &up, 3420 * MS), PREV);
  CHECK_INT(nav_update(&n, &c, GS_MENU, &up, 3549 * MS), 0);
  CHECK_INT(nav_update(&n, &c, GS_MENU, &up, 3550 * MS), PREV);
}

static void test_hysteresis(void) {
  nav_config_t c = cfg();
  nav_t n;
  sticks_t s;
  armed_in(&n, &c, GS_MENU, 0);
  int64_t t = 10 * MS;
  /* 60% of 1024 is 614.4: 614 is below, 615 above */
  s = stk(0, 614);  CHECK_INT(nav_update(&n, &c, GS_MENU, &s, t += MS), 0);
  s = stk(0, 615);  CHECK_INT(nav_update(&n, &c, GS_MENU, &s, t += MS), PREV);
  s = stk(0, 410);  CHECK_INT(nav_update(&n, &c, GS_MENU, &s, t += MS), 0);   /* 40.04%: still held */
  s = stk(0, 1024); CHECK_INT(nav_update(&n, &c, GS_MENU, &s, t += MS), 0);   /* no new crossing */
  s = stk(0, 409);  CHECK_INT(nav_update(&n, &c, GS_MENU, &s, t += MS), 0);   /* 39.94%: released */
  s = stk(0, 600);  CHECK_INT(nav_update(&n, &c, GS_MENU, &s, t += MS), 0);   /* between: nothing */
  s = stk(0, 615);  CHECK_INT(nav_update(&n, &c, GS_MENU, &s, t += MS), PREV);
  /* a held stick dipping into the band keeps repeating */
  s = stk(0, 500);  CHECK_INT(nav_update(&n, &c, GS_MENU, &s, t + 450 * MS), PREV);
  s = stk(0, 0);    CHECK_INT(nav_update(&n, &c, GS_MENU, &s, t += 500 * MS), 0);
  /* the negative side */
  s = stk(0, -614); CHECK_INT(nav_update(&n, &c, GS_MENU, &s, t += MS), 0);
  s = stk(0, -615); CHECK_INT(nav_update(&n, &c, GS_MENU, &s, t += MS), NEXT);
  s = stk(0, -410); CHECK_INT(nav_update(&n, &c, GS_MENU, &s, t += MS), 0);
  s = stk(0, -1024); CHECK_INT(nav_update(&n, &c, GS_MENU, &s, t += MS), 0);
  s = stk(0, -409); CHECK_INT(nav_update(&n, &c, GS_MENU, &s, t += MS), 0);
  s = stk(0, -615); CHECK_INT(nav_update(&n, &c, GS_MENU, &s, t += MS), NEXT);
  s = stk(0, 0);    CHECK_INT(nav_update(&n, &c, GS_MENU, &s, t += MS), 0);
  /* roll, both sides (PAUSED: roll left is EXIT there) */
  armed_in(&n, &c, GS_PAUSED, 0);
  s = stk(614, 0);  CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, t += MS), 0);
  s = stk(615, 0);  CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, t += MS), ENTER);
  s = stk(410, 0);  CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, t += MS), 0);
  s = stk(1024, 0); CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, t += MS), 0);
  s = stk(409, 0);  CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, t += MS), 0);
  s = stk(615, 0);  CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, t += MS), ENTER);
  s = stk(-615, 0); CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, t += MS), EXIT);   /* right to left in one frame */
  s = stk(-410, 0); CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, t += MS), 0);
  s = stk(-409, 0); CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, t += MS), 0);
  s = stk(-615, 0); CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, t += MS), EXIT);
  /* out-of-range values are clamped (no overflow) */
  s = stk(0, 0);     CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, t += MS), 0);
  s = stk(0, 99999); CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, t += MS), PREV);
  s = stk(0, 0);     CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, t += MS), 0);
  s = stk(INT32_MIN / 200, 0); CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, t += MS), EXIT);

  /* other thresholds, and nonsense ones are clamped: on 2..100, off 1..on-1 */
  c.on_pct = 80;
  c.off_pct = 20;
  armed_in(&n, &c, GS_SETUP, 0);
  s = stk(0, 800);  CHECK_INT(nav_update(&n, &c, GS_SETUP, &s, t += MS), 0);     /* 78% */
  s = stk(0, 820);  CHECK_INT(nav_update(&n, &c, GS_SETUP, &s, t += MS), PREV);  /* 80.1% */
  s = stk(0, 210);  CHECK_INT(nav_update(&n, &c, GS_SETUP, &s, t += MS), 0);
  s = stk(0, 820);  CHECK_INT(nav_update(&n, &c, GS_SETUP, &s, t += MS), 0);
  s = stk(0, 200);  CHECK_INT(nav_update(&n, &c, GS_SETUP, &s, t += MS), 0);
  s = stk(0, 820);  CHECK_INT(nav_update(&n, &c, GS_SETUP, &s, t += MS), PREV);
  c.on_pct = 0;
  c.off_pct = 0;                                  /* on 2% (20.48), off 1% (10.24) */
  armed_in(&n, &c, GS_SETUP, 0);
  s = stk(20, 0);   CHECK_INT(nav_update(&n, &c, GS_SETUP, &s, t += MS), 0);
  s = stk(21, 0);   CHECK_INT(nav_update(&n, &c, GS_SETUP, &s, t += MS), ENTER);
  s = stk(11, 0);   CHECK_INT(nav_update(&n, &c, GS_SETUP, &s, t += MS), 0);
  s = stk(10, 0);   CHECK_INT(nav_update(&n, &c, GS_SETUP, &s, t += MS), 0);
  s = stk(21, 0);   CHECK_INT(nav_update(&n, &c, GS_SETUP, &s, t += MS), ENTER);
  c.on_pct = 60;
  c.off_pct = 90;                                 /* off >= on: off becomes 59% */
  armed_in(&n, &c, GS_SETUP, 0);
  s = stk(615, 0);  CHECK_INT(nav_update(&n, &c, GS_SETUP, &s, t += MS), ENTER);
  s = stk(605, 0);  CHECK_INT(nav_update(&n, &c, GS_SETUP, &s, t += MS), 0);     /* 59.08%: held */
  s = stk(615, 0);  CHECK_INT(nav_update(&n, &c, GS_SETUP, &s, t += MS), 0);
  s = stk(603, 0);  CHECK_INT(nav_update(&n, &c, GS_SETUP, &s, t += MS), 0);     /* 58.9%: released */
  s = stk(615, 0);  CHECK_INT(nav_update(&n, &c, GS_SETUP, &s, t += MS), ENTER);
  c.on_pct = 150;                                 /* 100%: a full stick is not above it */
  c.off_pct = 40;
  armed_in(&n, &c, GS_SETUP, 0);
  s = stk(1024, 1024); CHECK_INT(nav_update(&n, &c, GS_SETUP, &s, t += MS), 0);
}

static void test_roll(void) {
  nav_config_t c = cfg();
  nav_t n;
  int t[8];
  /* roll right: one ENTER per deflection, however long it is held */
  armed_in(&n, &c, GS_SETUP, 0);
  CHECK_INT(hold(&n, &c, GS_SETUP, stk(1024, 0), 10 * MS, 3000, ENTER, t, 8), 1);
  CHECK_INT(hold(&n, &c, GS_SETUP, stk(0, 0), 3010 * MS, 10, 0, t, 8), 0);
  CHECK_INT(hold(&n, &c, GS_SETUP, stk(700, 0), 3020 * MS, 1000, ENTER, t, 8), 1);
  /* roll left: EXIT once per deflection outside the menu */
  CHECK_INT(hold(&n, &c, GS_SETUP, stk(-1024, 0), 4020 * MS, 2000, EXIT, t, 8), 1);
  int states[] = { GS_PAUSED, GS_DONE };
  for (int i = 0; i < 2; i++) {
    armed_in(&n, &c, states[i], 0);
    CHECK_INT(hold(&n, &c, states[i], stk(-1024, 0), 10 * MS, 1000, EXIT, t, 8), 1);
  }

  /* MENU, not editing: roll left never sends EXIT (it would close the game) */
  armed_in(&n, &c, GS_MENU, 0);
  for (int k = 0; k < 20; k++) {
    CHECK_INT(hold(&n, &c, GS_MENU, stk(-1024, 0), (k * 200 + 10) * MS, 100, 0, t, 8), 0);
    CHECK_INT(hold(&n, &c, GS_MENU, stk(0, 0), (k * 200 + 110) * MS, 90, 0, t, 8), 0);
  }
  /* ...while pitch still works with the roll held left */
  CHECK_INT(hold(&n, &c, GS_MENU, stk(-1024, 1024), 5000 * MS, 500, PREV, t, 8), 2);
  CHECK_INT(hold(&n, &c, GS_MENU, stk(-1024, -1024), 5500 * MS, 10, NEXT, t, 8), 1);
  /* a roll held left when editing starts doesn't fire: it must come back first */
  nav_after_run(&n, ENTER, GS_MENU);
  CHECK(n.editing);
  CHECK_INT(hold(&n, &c, GS_MENU, stk(-1024, 0), 6000 * MS, 100, 0, t, 8), 0);
  CHECK_INT(hold(&n, &c, GS_MENU, stk(0, 0), 6100 * MS, 10, 0, t, 8), 0);
  /* MENU, editing: roll left is EXIT (leaves editing) */
  CHECK_INT(hold(&n, &c, GS_MENU, stk(-1024, 0), 6110 * MS, 100, EXIT, t, 8), 1);
  nav_after_run(&n, EXIT, GS_MENU);
  CHECK(!n.editing);
  CHECK_INT(hold(&n, &c, GS_MENU, stk(0, 0), 6210 * MS, 10, 0, t, 8), 0);
  CHECK_INT(hold(&n, &c, GS_MENU, stk(-1024, 0), 6220 * MS, 100, 0, t, 8), 0);

  /* a roll and a pitch crossing in the same frame: the roll first, the pitch on the next frame */
  armed_in(&n, &c, GS_PAUSED, 0);
  sticks_t both = stk(1024, 1024);
  CHECK_INT(nav_update(&n, &c, GS_PAUSED, &both, 10 * MS), ENTER);
  CHECK_INT(nav_update(&n, &c, GS_PAUSED, &both, 26 * MS), PREV);
  CHECK_INT(nav_update(&n, &c, GS_PAUSED, &both, 42 * MS), 0);
  /* in the menu, a suppressed roll left doesn't hold the pitch back */
  armed_in(&n, &c, GS_MENU, 0);
  both = stk(-1024, -1024);
  CHECK_INT(nav_update(&n, &c, GS_MENU, &both, 10 * MS), NEXT);
}

static void test_arming_after_state_change(void) {
  nav_config_t c = cfg();
  nav_t n;
  int t[8];
  armed_in(&n, &c, GS_MENU, 0);
  /* flying with the roll right: nothing (not a nav state), then the race ends with it held */
  CHECK_INT(hold(&n, &c, GS_FLY, stk(1024, 300), 10 * MS, 500, 0, t, 8), 0);
  CHECK(!n.armed);
  CHECK_INT(hold(&n, &c, GS_DONE, stk(1024, 300), 510 * MS, 500, 0, t, 8), 0);
  /* the roll comes back but the pitch stays above off: still not armed */
  CHECK_INT(hold(&n, &c, GS_DONE, stk(0, 500), 1010 * MS, 100, 0, t, 8), 0);
  CHECK(!n.armed);
  CHECK_INT(hold(&n, &c, GS_DONE, stk(1024, 500), 1110 * MS, 100, 0, t, 8), 0);
  CHECK_INT(hold(&n, &c, GS_DONE, stk(1024, -700), 1210 * MS, 100, 0, t, 8), 0);
  /* both under off (39%) once: armed */
  CHECK_INT(hold(&n, &c, GS_DONE, stk(-399, 399), 1310 * MS, 1, 0, t, 8), 0);
  CHECK(n.armed);
  CHECK_INT(hold(&n, &c, GS_DONE, stk(1024, 0), 1311 * MS, 100, ENTER, t, 8), 1);
  /* exactly at off (40.04%) doesn't arm */
  armed_in(&n, &c, GS_MENU, 0);
  CHECK_INT(hold(&n, &c, GS_PAUSED, stk(410, 0), 10 * MS, 100, 0, t, 8), 0);
  CHECK(!n.armed);
  CHECK_INT(hold(&n, &c, GS_PAUSED, stk(0, -410), 110 * MS, 100, 0, t, 8), 0);
  CHECK(!n.armed);
  CHECK_INT(hold(&n, &c, GS_PAUSED, stk(0, -409), 210 * MS, 1, 0, t, 8), 0);
  CHECK(n.armed);

  /* MENU -> SETUP (ENTER on Settings) with the roll still right: no second ENTER until centered */
  armed_in(&n, &c, GS_MENU, 0);
  sticks_t r = stk(1024, 0), mid = stk(0, 0);
  CHECK_INT(nav_update(&n, &c, GS_MENU, &r, 10 * MS), ENTER);
  nav_after_run(&n, ENTER, GS_SETUP);
  CHECK(!n.editing);
  CHECK_INT(hold(&n, &c, GS_SETUP, stk(1024, 0), 26 * MS, 2000, 0, t, 8), 0);
  CHECK_INT(nav_update(&n, &c, GS_SETUP, &mid, 2100 * MS), 0);
  CHECK_INT(nav_update(&n, &c, GS_SETUP, &r, 2116 * MS), ENTER);
  /* a pitch held through a state change stops repeating and must come back */
  armed_in(&n, &c, GS_PAUSED, 0);
  CHECK_INT(hold(&n, &c, GS_PAUSED, stk(0, -1024), 10 * MS, 500, NEXT, t, 8), 2);
  CHECK_INT(hold(&n, &c, GS_SETUP, stk(0, -1024), 510 * MS, 2000, 0, t, 8), 0);
  CHECK_INT(hold(&n, &c, GS_SETUP, stk(0, 0), 2510 * MS, 1, 0, t, 8), 0);
  CHECK_INT(hold(&n, &c, GS_SETUP, stk(0, -1024), 2511 * MS, 1, NEXT, t, 8), 1);
  /* GS_UNKNOWN (no test hooks) is no nav state */
  armed_in(&n, &c, GS_UNKNOWN, 0);
  CHECK_INT(hold(&n, &c, GS_UNKNOWN, stk(1024, 1024), 10 * MS, 1000, 0, t, 8), 0);
  CHECK_INT(hold(&n, &c, 42, stk(-1024, -1024), 10 * MS, 1000, 0, t, 8), 0);
}

static void test_editing_inference(void) {
  nav_config_t c = cfg();
  nav_t n;
  sticks_t up = stk(0, 1024), down = stk(0, -1024), mid = stk(0, 0), right = stk(1024, 0), left = stk(-1024, 0);
  int64_t t = 0;
#define STEP(st, s, want) do { sticks_t _s = (s); t += 20 * MS; CHECK_INT(nav_update(&n, &c, (st), &_s, t), (want)); } while (0)
  /* MENU: four NEXTs to Track (item 5), ENTER starts editing it */
  armed_in(&n, &c, GS_MENU, t);
  for (int i = 0; i < 4; i++) {
    STEP(GS_MENU, down, NEXT);
    nav_after_run(&n, NEXT, GS_MENU);
    STEP(GS_MENU, mid, 0);
    CHECK(!n.editing);
  }
  STEP(GS_MENU, right, ENTER);
  nav_after_run(&n, ENTER, GS_MENU);
  CHECK(n.editing);
  STEP(GS_MENU, mid, 0);
  STEP(GS_MENU, up, 0x1E00);             /* INC: next track */
  nav_after_run(&n, 0x1E00, GS_MENU);
  CHECK(n.editing);
  STEP(GS_MENU, mid, 0);
  STEP(GS_MENU, down, 0x1D00);           /* DEC: previous track */
  nav_after_run(&n, 0x1D00, GS_MENU);
  STEP(GS_MENU, mid, 0);
  STEP(GS_MENU, right, ENTER);           /* ENTER leaves editing */
  nav_after_run(&n, ENTER, GS_MENU);
  CHECK(!n.editing);
  STEP(GS_MENU, mid, 0);
  STEP(GS_MENU, up, PREV);
  nav_after_run(&n, PREV, GS_MENU);
  /* frames without an event change nothing */
  nav_after_run(&n, 0, GS_MENU);
  CHECK(!n.editing);
  n.editing = true;
  nav_after_run(&n, 0, GS_MENU);
  CHECK(n.editing);
  nav_after_run(&n, ETX_EVT_ENTER_LONG, GS_MENU);
  nav_after_run(&n, ETX_EVT_NEXT_PAGE, GS_MENU);
  CHECK(n.editing);
  /* EXIT that keeps the state leaves editing (from the keyboard too) */
  nav_after_run(&n, EXIT, GS_MENU);
  CHECK(!n.editing);
  nav_after_run(&n, EXIT, GS_MENU);      /* (a second one would have closed the game; still false) */
  CHECK(!n.editing);
  /* ENTER that changes the state (Race, Settings) never starts editing */
  nav_after_run(&n, ENTER, GS_COUNT);
  CHECK(!n.editing);
  CHECK_INT(n.prev_state, GS_COUNT);
  nav_after_run(&n, 0, GS_PAUSED);
  nav_after_run(&n, ENTER, GS_PAUSED);   /* PAUSED/DONE have no editing */
  CHECK(!n.editing);
  nav_after_run(&n, ENTER, GS_DONE);
  nav_after_run(&n, ENTER, GS_DONE);
  CHECK(!n.editing);
  nav_after_run(&n, ENTER, GS_FLY);
  nav_after_run(&n, ENTER, GS_FLY);
  CHECK(!n.editing);

  /* SETUP: ENTER edits a row, ENTER or EXIT leaves editing, EXIT then goes back */
  nav_after_run(&n, ENTER, GS_SETUP);
  CHECK(!n.editing);                     /* came from PAUSED via Settings */
  t = 100000 * MS;
  armed_in(&n, &c, GS_SETUP, t);
  n.prev_state = GS_SETUP;
  STEP(GS_SETUP, right, ENTER);
  nav_after_run(&n, ENTER, GS_SETUP);
  CHECK(n.editing);
  STEP(GS_SETUP, mid, 0);
  STEP(GS_SETUP, up, 0x1E00);
  nav_after_run(&n, 0x1E00, GS_SETUP);
  STEP(GS_SETUP, mid, 0);
  STEP(GS_SETUP, left, EXIT);
  nav_after_run(&n, EXIT, GS_SETUP);
  CHECK(!n.editing);
  STEP(GS_SETUP, mid, 0);
  STEP(GS_SETUP, up, PREV);
  nav_after_run(&n, PREV, GS_SETUP);
  STEP(GS_SETUP, mid, 0);
  STEP(GS_SETUP, right, ENTER);
  nav_after_run(&n, ENTER, GS_SETUP);
  CHECK(n.editing);
  STEP(GS_SETUP, mid, 0);
  STEP(GS_SETUP, right, ENTER);
  nav_after_run(&n, ENTER, GS_SETUP);
  CHECK(!n.editing);
  STEP(GS_SETUP, mid, 0);
  STEP(GS_SETUP, left, EXIT);            /* not editing: saves and goes back to the menu */
  nav_after_run(&n, EXIT, GS_MENU);
  CHECK(!n.editing);
  /* the state changing under it (nav_update) also ends editing */
  n.editing = true;
  STEP(GS_PAUSED, mid, 0);
  CHECK(!n.editing);
  /* a state change between MENU and SETUP while "editing" (keyboard taps) resets it */
  n.editing = true;
  n.prev_state = GS_MENU;
  nav_after_run(&n, ENTER, GS_SETUP);
  CHECK(!n.editing);
  n.editing = true;
  nav_after_run(&n, 0, GS_MENU);
  CHECK(!n.editing);

  /* reload / script closed: editing off, the menu next, and the sticks must re-arm */
  n.editing = true;
  n.armed = true;
  n.state = GS_SETUP;
  n.prev_state = GS_SETUP;
  nav_reset(&n);
  CHECK(!n.editing);
  CHECK(!n.armed);
  CHECK_INT(n.prev_state, GS_MENU);
  CHECK_INT(n.state, GS_UNKNOWN);
  STEP(GS_MENU, right, 0);
  STEP(GS_MENU, mid, 0);
  STEP(GS_MENU, right, ENTER);
  nav_after_run(&n, ENTER, GS_MENU);     /* the first run after the reload: MENU item 5 */
  CHECK(n.editing);
  /* nav_reset keeps the pause switch position and the signal state */
  n.pause_pos = 1;
  n.was_live = true;
  n.loss_sent = true;
  nav_reset(&n);
  CHECK_INT(n.pause_pos, 1);
  CHECK(n.was_live);
  CHECK(n.loss_sent);
#undef STEP
}

static void test_done(void) {
  nav_config_t c = cfg();
  nav_t n;
  int t[8];
  armed_in(&n, &c, GS_FLY, 0);
  sticks_t mid = stk(0, 0), up = stk(0, 1024), down = stk(0, -1024), right = stk(1024, 0), left = stk(-1024, 0);
  CHECK_INT(nav_update(&n, &c, GS_DONE, &mid, 10 * MS), 0);
  CHECK(n.armed);
  nav_after_run(&n, 0, GS_DONE);
  CHECK_INT(nav_update(&n, &c, GS_DONE, &down, 20 * MS), NEXT);     /* Again -> Menu */
  nav_after_run(&n, NEXT, GS_DONE);
  CHECK_INT(nav_update(&n, &c, GS_DONE, &mid, 30 * MS), 0);
  CHECK_INT(nav_update(&n, &c, GS_DONE, &up, 40 * MS), PREV);
  nav_after_run(&n, PREV, GS_DONE);
  CHECK_INT(nav_update(&n, &c, GS_DONE, &mid, 50 * MS), 0);
  /* ENTER in DONE that keeps the state (impossible in the game) still never means editing */
  CHECK_INT(nav_update(&n, &c, GS_DONE, &right, 60 * MS), ENTER);
  nav_after_run(&n, ENTER, GS_DONE);
  CHECK(!n.editing);
  CHECK_INT(nav_update(&n, &c, GS_DONE, &mid, 70 * MS), 0);
  /* pitch repeats in DONE too (two buttons, listNav wraps) */
  CHECK_INT(hold(&n, &c, GS_DONE, down, 80 * MS, 1000, NEXT, t, 8), 5);
  CHECK_INT(nav_update(&n, &c, GS_DONE, &mid, 1100 * MS), 0);
  /* roll left = EXIT = back to the menu */
  CHECK_INT(nav_update(&n, &c, GS_DONE, &left, 1110 * MS), EXIT);
  nav_after_run(&n, EXIT, GS_MENU);
  CHECK(!n.editing);
  /* and in the menu the still-held roll left is nothing */
  CHECK_INT(hold(&n, &c, GS_MENU, left, 1120 * MS, 1000, 0, t, 8), 0);
  /* roll right (Again) -> COUNT: the nav goes quiet */
  armed_in(&n, &c, GS_DONE, 0);
  CHECK_INT(nav_update(&n, &c, GS_DONE, &right, 10 * MS), ENTER);
  nav_after_run(&n, ENTER, GS_COUNT);
  CHECK_INT(hold(&n, &c, GS_COUNT, right, 20 * MS, 3000, 0, t, 8), 0);
}

/* pause switch edges: expected EXIT for the edge to on / to off in each state */
static void test_pause_switch(void) {
  nav_config_t c = cfg();
  nav_t n;
  for (int st = GS_MENU; st <= GS_DONE; st++) {
    bool flight = st == GS_COUNT || st == GS_FLY || st == GS_CRASHED || st == GS_READY;
    nav_init(&n);
    sticks_t s = sw(-1024);
    CHECK_INT(nav_update(&n, &c, st, &s, 10 * MS), 0);             /* first sight: the baseline */
    CHECK_INT(n.pause_pos, -1);
    s = sw(1024);
    CHECK_INT(nav_update(&n, &c, st, &s, 20 * MS), flight ? EXIT : 0);
    CHECK_INT(nav_update(&n, &c, st, &s, 30 * MS), 0);              /* held: once */
    s = sw(-1024);
    CHECK_INT(nav_update(&n, &c, st, &s, 40 * MS), st == GS_PAUSED ? EXIT : 0);
    CHECK_INT(nav_update(&n, &c, st, &s, 50 * MS), 0);
    /* first sight "on": no event either */
    nav_init(&n);
    s = sw(1024);
    CHECK_INT(nav_update(&n, &c, st, &s, 10 * MS), 0);
    CHECK_INT(n.pause_pos, 1);
  }

  /* hysteresis around 0: +-200 */
  armed_in(&n, &c, GS_FLY, 0);
  int seq[][2] = {
    { -1024, 0 }, { 0, 0 }, { 200, 0 }, { 150, 0 }, { 201, EXIT }, { 1024, 0 }, { 0, 0 }, { -200, 0 },
    { 199, 0 }, { -201, 0 }, { 150, 0 }, { 200, 0 }, { 201, EXIT }, { -150, 0 }, { 201, 0 },
  };
  for (size_t i = 0; i < sizeof seq / sizeof seq[0]; i++) {
    sticks_t s = sw(seq[i][0]);
    int ev = nav_update(&n, &c, GS_FLY, &s, (int64_t)(100 + i) * MS);
    if (ev != seq[i][1]) fprintf(stderr, "hysteresis step %d (pause %d)\n", (int)i, seq[i][0]);
    CHECK_INT(ev, seq[i][1]);
  }
  /* not seen yet and in the band: still not seen; the first value out of it is the baseline */
  nav_init(&n);
  sticks_t s = sw(0);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &s, 0), 0);
  CHECK_INT(n.pause_pos, 0);
  s = sw(199);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &s, MS), 0);
  CHECK_INT(n.pause_pos, 0);
  s = sw(1024);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &s, 2 * MS), 0);
  CHECK_INT(n.pause_pos, 1);
  /* no pause channel (INT32_MIN) or no live source: ignored, the position is remembered */
  s = sw(INT32_MIN);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &s, 3 * MS), 0);
  CHECK_INT(n.pause_pos, 1);
  s = sw(1024);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &s, 4 * MS), 0);           /* still on: no edge */
  s = sw(-1024);
  s.live = false;
  s.pause = -1024;                       /* a stale value must not count */
  c.pause_on_signal_loss = false;
  CHECK_INT(nav_update(&n, &c, GS_FLY, &s, 5 * MS), 0);
  CHECK_INT(n.pause_pos, 1);
  c.pause_on_signal_loss = true;
  s = sw(INT32_MIN);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &s, 6 * MS), 0);
  s = sw(-1024);                         /* on -> (unknown) -> off: an edge to off */
  CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, 7 * MS), EXIT);

  /* the switch works with stick menus off, and while the sticks are deflected (not armed) */
  c.enabled = false;
  armed_in(&n, &c, GS_FLY, 0);
  s = sw(-1024);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &s, 10 * MS), 0);
  s = sw(1024);
  s.ail = 1024;
  s.ele = -1024;
  CHECK_INT(nav_update(&n, &c, GS_FLY, &s, 20 * MS), EXIT);
  c.enabled = true;
  CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, 30 * MS), 0);        /* state change, not armed */
  CHECK(!n.armed);
  s.pause = -1024;
  CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, 40 * MS), EXIT);      /* resumes anyway */

  /* an edge in DONE / SETUP / MENU only moves the position: no late EXIT in flight */
  armed_in(&n, &c, GS_DONE, 0);
  s = sw(-1024);
  CHECK_INT(nav_update(&n, &c, GS_DONE, &s, 10 * MS), 0);
  s = sw(1024);
  CHECK_INT(nav_update(&n, &c, GS_DONE, &s, 20 * MS), 0);
  CHECK_INT(nav_update(&n, &c, GS_COUNT, &s, 30 * MS), 0);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &s, 40 * MS), 0);
  s = sw(-1024);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &s, 50 * MS), 0);
  s = sw(1024);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &s, 60 * MS), EXIT);

  /* the whole flow: fly, switch on -> paused, switch off -> resumed, again */
  armed_in(&n, &c, GS_READY, 0);
  int64_t t = 0;
  s = sw(-1024);
  CHECK_INT(nav_update(&n, &c, GS_READY, &s, t += 16 * MS), 0);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &s, t += 16 * MS), 0);
  for (int k = 0; k < 3; k++) {
    s = sw(1024);
    CHECK_INT(nav_update(&n, &c, GS_FLY, &s, t += 16 * MS), EXIT);
    nav_after_run(&n, EXIT, GS_PAUSED);
    CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, t += 16 * MS), 0);
    CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, t += 16 * MS), 0);
    s = sw(-1024);
    CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, t += 16 * MS), EXIT);
    nav_after_run(&n, EXIT, GS_FLY);
    CHECK_INT(nav_update(&n, &c, GS_FLY, &s, t += 16 * MS), 0);
  }
  /* paused from the menu by the sticks, the switch still "off": switching it on and off resumes */
  armed_in(&n, &c, GS_FLY, 0);
  s = sw(-1024);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &s, 10 * MS), 0);
  CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, 20 * MS), 0);         /* paused by the keyboard */
  s = sw(1024);
  CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, 30 * MS), 0);
  s = sw(-1024);
  CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, 40 * MS), EXIT);
}

static void test_signal_loss(void) {
  nav_config_t c = cfg();
  nav_t n;
  sticks_t live = stk(0, 0), gone = lost();
  /* each flight state: EXIT once; nav states: nothing */
  for (int st = GS_MENU; st <= GS_DONE; st++) {
    bool flight = st == GS_COUNT || st == GS_FLY || st == GS_CRASHED || st == GS_READY;
    nav_init(&n);
    CHECK_INT(nav_update(&n, &c, st, &live, 10 * MS), 0);
    CHECK_INT(nav_update(&n, &c, st, &gone, 20 * MS), flight ? EXIT : 0);
    for (int k = 0; k < 50; k++) CHECK_INT(nav_update(&n, &c, st, &gone, (30 + k * 10) * MS), 0);
  }
  /* never live: never a loss */
  nav_init(&n);
  for (int k = 0; k < 50; k++) CHECK_INT(nav_update(&n, &c, GS_FLY, &gone, k * 10 * MS), 0);
  /* lost, paused, still lost after resuming: once per outage; back and lost again: again */
  nav_init(&n);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &live, 0), 0);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &gone, 10 * MS), EXIT);
  nav_after_run(&n, EXIT, GS_PAUSED);
  CHECK_INT(nav_update(&n, &c, GS_PAUSED, &gone, 20 * MS), 0);
  nav_after_run(&n, EXIT, GS_FLY);       /* resumed from the keyboard */
  CHECK_INT(nav_update(&n, &c, GS_FLY, &gone, 30 * MS), 0);
  CHECK_INT(nav_update(&n, &c, GS_CRASHED, &gone, 40 * MS), 0);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &live, 50 * MS), 0);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &gone, 60 * MS), EXIT);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &gone, 70 * MS), 0);
  /* lost in the menu, then a race starts (keyboard) while it is still gone: paused once */
  nav_init(&n);
  CHECK_INT(nav_update(&n, &c, GS_MENU, &live, 0), 0);
  CHECK_INT(nav_update(&n, &c, GS_MENU, &gone, 10 * MS), 0);
  CHECK_INT(nav_update(&n, &c, GS_MENU, &gone, 20 * MS), 0);
  CHECK_INT(nav_update(&n, &c, GS_COUNT, &gone, 30 * MS), EXIT);
  CHECK_INT(nav_update(&n, &c, GS_COUNT, &gone, 40 * MS), 0);
  /* turned off */
  c.pause_on_signal_loss = false;
  nav_init(&n);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &live, 0), 0);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &gone, 10 * MS), 0);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &gone, 20 * MS), 0);
  /* and with stick menus off it still pauses */
  c = cfg();
  c.enabled = false;
  nav_init(&n);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &live, 0), 0);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &gone, 10 * MS), EXIT);
  /* a loss frame and a pause switch edge at once: one EXIT (the stale switch is ignored) */
  c = cfg();
  nav_init(&n);
  sticks_t s = sw(-1024);
  CHECK_INT(nav_update(&n, &c, GS_FLY, &s, 0), 0);
  s = sw(1024);
  s.live = false;
  CHECK_INT(nav_update(&n, &c, GS_FLY, &s, 10 * MS), EXIT);
  CHECK_INT(n.pause_pos, -1);
  s.live = true;                          /* back with the switch on: that edge pauses (if flying) */
  CHECK_INT(nav_update(&n, &c, GS_PAUSED, &s, 20 * MS), 0);
  CHECK_INT(n.pause_pos, 1);
}

static void test_disabled(void) {
  nav_config_t c = cfg();
  c.enabled = false;
  nav_t n;
  int t[8];
  nav_init(&n);
  int states[] = { GS_MENU, GS_SETUP, GS_PAUSED, GS_DONE };
  for (int i = 0; i < 4; i++) {
    CHECK_INT(hold(&n, &c, states[i], stk(0, 0), 0, 10, 0, t, 8), 0);
    CHECK_INT(hold(&n, &c, states[i], stk(1024, 0), 10 * MS, 100, 0, t, 8), 0);
    CHECK_INT(hold(&n, &c, states[i], stk(0, 0), 110 * MS, 10, 0, t, 8), 0);
    CHECK_INT(hold(&n, &c, states[i], stk(-1024, 0), 120 * MS, 100, 0, t, 8), 0);
    CHECK_INT(hold(&n, &c, states[i], stk(0, 1024), 220 * MS, 1000, 0, t, 8), 0);
    CHECK_INT(hold(&n, &c, states[i], stk(0, -1024), 1220 * MS, 1000, 0, t, 8), 0);
    CHECK(!n.armed);
  }
  /* turned on in the same state with a stick held: it must come back first */
  c.enabled = true;
  CHECK_INT(hold(&n, &c, GS_DONE, stk(0, -1024), 3000 * MS, 1000, 0, t, 8), 0);
  CHECK_INT(hold(&n, &c, GS_DONE, stk(0, 0), 4000 * MS, 1, 0, t, 8), 0);
  CHECK_INT(hold(&n, &c, GS_DONE, stk(0, -1024), 4001 * MS, 1, NEXT, t, 8), 1);
  /* flight states never navigate */
  armed_in(&n, &c, GS_MENU, 0);
  int fl[] = { GS_COUNT, GS_FLY, GS_CRASHED, GS_READY };
  for (int i = 0; i < 4; i++) {
    CHECK_INT(hold(&n, &c, fl[i], stk(1024, 1024), 10 * MS, 500, 0, t, 8), 0);
    CHECK_INT(hold(&n, &c, fl[i], stk(0, 0), 510 * MS, 10, 0, t, 8), 0);
    CHECK_INT(hold(&n, &c, fl[i], stk(-1024, -1024), 520 * MS, 500, 0, t, 8), 0);
  }
}

/* ------------------------------------------------------------------ a model of the game */
typedef struct {
  int state, focus, prev, paused_from;
  bool editing, closed;
  int64_t since;                          /* when the state was entered (timed transitions) */
  int track;
} game_t;

static uint32_t rng = 2463534242u;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

static void g_set(game_t *g, int st, int focus, int64_t now) {
  g->state = st;
  g->focus = focus;
  g->since = now;
}

static void g_list(game_t *g, int ev, int n) {
  if (ev == NEXT) g->focus = g->focus % n + 1;
  else if (ev == PREV) g->focus = (g->focus - 2 + n) % n + 1;
}

static void g_start(game_t *g, int mode, int64_t now) { g_set(g, mode == 3 ? GS_READY : GS_COUNT, 1, now); }

/* handleEvent (G:3396-3470), the parts the sticks and keys reach */
static void g_event(game_t *g, int ev, int64_t now) {
  if (!ev) return;
  switch (g->state) {
    case GS_MENU:
      if (g->editing) {
        if (ev == NEXT) g->track++;
        else if (ev == PREV) g->track--;
        else if (ev == ENTER || ev == EXIT) g->editing = false;
        return;
      }
      if (ev == EXIT) { g->closed = true; return; }
      g_list(g, ev, 7);
      if (ev != ENTER) return;
      if (g->focus == 5) g->editing = true;
      else if (g->focus <= 4) g_start(g, g->focus, now);
      else if (g->focus == 6) { g->prev = GS_MENU; g_set(g, GS_SETUP, 1, now); }
      else g->closed = true;
      return;
    case GS_SETUP:
      if (g->editing) {
        if (ev == ENTER || ev == EXIT) g->editing = false;
        return;
      }
      g_list(g, ev, 25);
      if (ev == EXIT || (ev == ENTER && g->focus == 25)) {
        if (g->prev == GS_PAUSED) g_set(g, GS_PAUSED, 4, now);
        else g_set(g, GS_MENU, 6, now);
      } else if (ev == ENTER) {
        g->editing = true;
      }
      return;
    case GS_COUNT: case GS_FLY: case GS_CRASHED: case GS_READY:
      if (ev == EXIT) { g->paused_from = g->state; g_set(g, GS_PAUSED, 1, now); }
      return;
    case GS_PAUSED:
      g_list(g, ev, 5);
      if (ev == EXIT) { g_set(g, g->paused_from, 1, now); return; }
      if (ev != ENTER) return;
      if (g->focus == 1) g_set(g, g->paused_from, 1, now);
      else if (g->focus == 2) g_set(g, g->paused_from == GS_COUNT ? GS_COUNT : GS_READY, 1, now);
      else if (g->focus == 3) g_start(g, 1, now);
      else if (g->focus == 4) { g->prev = GS_PAUSED; g_set(g, GS_SETUP, 1, now); }
      else g_set(g, GS_MENU, 1, now);
      return;
    case GS_DONE:
      g_list(g, ev, 2);
      if (ev == EXIT || (ev == ENTER && g->focus == 2)) g_set(g, GS_MENU, 1, now);
      else if (ev == ENTER) g_start(g, 1, now);
      return;
  }
}

/* update(): timed transitions */
static void g_tick(game_t *g, int64_t now) {
  int64_t e = now - g->since;
  if (g->state == GS_COUNT && e > 3000 * MS) g_set(g, GS_FLY, 1, now);
  else if (g->state == GS_CRASHED && e > 1200 * MS) g_set(g, GS_READY, 1, now);
  else if (g->state == GS_READY && e > 2000 * MS) g_set(g, GS_FLY, 1, now);
  else if (g->state == GS_FLY && e > 1500 * MS && rnd() % 200 == 0) g_set(g, rnd() % 3 ? GS_CRASHED : GS_DONE, 1, now);
}

static int pick_axis(void) {
  static const int v[] = { 0, 0, 0, 100, -100, 300, -300, 409, -409, 410, -410, 500, -500, 614, -614, 615, -615, 800, -800, 1024, -1024 };
  return (rnd() & 7) == 0 ? (int)(rnd() % 2049) - 1024 : v[rnd() % (sizeof v / sizeof v[0])];
}

static void test_game_model(void) {
  nav_config_t c = cfg();
  nav_t n;
  nav_init(&n);
  game_t g;
  memset(&g, 0, sizeof g);
  g_set(&g, GS_MENU, 1, 0);
  int ail = 0, ele = 0, pause = -1024, hold_frames = 0;
  bool live = true;
  int bad_editing = 0, bad_exit = 0, bad_arm = 0, bad_other = 0;
  int count[8] = { 0 }, closes = 0, edits = 0, setups = 0, pauses = 0;
  bool armed_model = false;
  int last_state = GS_UNKNOWN;
  int64_t now = 0;
  for (int f = 0; f < 400000; f++) {
    now += 16667;
    if (--hold_frames <= 0) {
      hold_frames = 1 + (int)(rnd() % 40);
      ail = pick_axis();
      ele = pick_axis();
      if (rnd() % 8 == 0) { ail = 0; ele = 0; }
      if (rnd() % 30 == 0) pause = -pause;
      if (rnd() % 50 == 0) live = !live;
      else if (!live && rnd() % 3 == 0) live = true;
    }
    sticks_t s = stk(ail, ele);
    s.pause = pause;
    s.live = live;
    if (!live) { s.ail = s.ele = s.rud = 0; s.thr = -1024; s.pause = INT32_MIN; }

    /* the test's own idea of arming: both sticks under 40% once since the state changed */
    if (g.state != last_state) armed_model = false;
    last_state = g.state;
    bool nav_st = g.state == GS_MENU || g.state == GS_SETUP || g.state == GS_PAUSED || g.state == GS_DONE;
    if (nav_st && s.ail * 100 < 40 * 1024 && s.ail * 100 > -40 * 1024 && s.ele * 100 < 40 * 1024 && s.ele * 100 > -40 * 1024)
      armed_model = true;

    int ev = nav_update(&n, &c, g.state, &s, now);
    if (ev == EXIT && g.state == GS_MENU && !g.editing) bad_exit++;
    if (ev && ev != EXIT && !nav_st) bad_other++;
    if (ev && nav_st && !armed_model && !(ev == EXIT && g.state == GS_PAUSED)) bad_arm++;
    /* now and then a key instead (only on frames the sticks are quiet: the host queues the other) */
    if (!ev && rnd() % 97 == 0) {
      static const int keys[] = { ETX_EVT_ENTER, ETX_EVT_EXIT, ETX_EVT_PREV, ETX_EVT_NEXT, 0 };
      int k = keys[rnd() % 5];
      ev = k ? k : nav_pitch_event(&n, rnd() & 1 ? 1 : -1);
    }
    int before = g.state;
    g_event(&g, ev, now);
    if (g.closed) {                      /* run() returned 1: the host reloads the script */
      closes++;
      memset(&g, 0, sizeof g);
      g_set(&g, GS_MENU, 1, now);
      nav_reset(&n);
      last_state = GS_UNKNOWN;
      continue;
    }
    g_tick(&g, now);
    nav_after_run(&n, ev, g.state);
    if (n.editing != g.editing) {
      if (bad_editing < 5) fprintf(stderr, "frame %d: state %d->%d ev 0x%X nav editing %d, game %d\n", f, before, g.state, ev, n.editing, g.editing);
      bad_editing++;
    }
    count[g.state & 7]++;
    if (g.editing) edits++;
    if (g.state == GS_SETUP) setups++;
    if (g.state == GS_PAUSED) pauses++;
  }
  CHECK_INT(bad_editing, 0);
  CHECK_INT(bad_exit, 0);
  CHECK_INT(bad_arm, 0);
  CHECK_INT(bad_other, 0);
  /* the walk reached everything */
  for (int st = GS_MENU; st <= GS_DONE; st++) {
    if (!count[st & 7]) fprintf(stderr, "state %d never reached\n", st);
    CHECK(count[st & 7] > 0);
  }
  CHECK(closes > 0);
  CHECK(edits > 1000);
  CHECK(setups > 1000);
  CHECK(pauses > 1000);
  printf("model: %d closes, %d editing frames, %d setup frames, %d paused frames\n", closes, edits, setups, pauses);
}

int main(void) {
  test_defaults();
  test_pitch_event();
  test_arming_at_start();
  test_repeat_timing();
  test_hysteresis();
  test_roll();
  test_arming_after_state_change();
  test_editing_inference();
  test_done();
  test_pause_switch();
  test_signal_loss();
  test_disabled();
  test_game_model();
  T_DONE();
}
