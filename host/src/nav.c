/* Menus by sticks, the pause switch and the pause on signal loss: see nav.h.
 * The game's event handling these rules follow is in the game-api notes §2.2-2.4. */
#include "nav.h"
#include "etx.h"

#include <string.h>

#define PAUSE_HYST 200              /* the pause switch is on above +200, off below -200 */

static bool nav_state(int st) { return st == GS_MENU || st == GS_SETUP || st == GS_PAUSED || st == GS_DONE; }
static bool flight_state(int st) { return st == GS_COUNT || st == GS_FLY || st == GS_CRASHED || st == GS_READY; }
static int clamp_stick(int v) { return v < -1024 ? -1024 : v > 1024 ? 1024 : v; }

void nav_init(nav_t *n) {
  memset(n, 0, sizeof *n);
  n->state = GS_UNKNOWN;
  n->prev_state = GS_MENU;          /* the game starts in the menu */
}

void nav_default_config(nav_config_t *c) {
  c->enabled = true;
  c->on_pct = 60;
  c->off_pct = 40;
  c->repeat_first_ms = 450;
  c->repeat_ms = 150;
  c->pause_on_signal_loss = true;
}

int nav_pitch_event(const nav_t *n, int dir) {
  /* PREV == DEC and NEXT == INC: stick up is "focus up" in a list but "value up" while editing */
  if (dir < 0) return n->editing ? ETX_EVT_NEXT : ETX_EVT_PREV;
  if (dir > 0) return n->editing ? ETX_EVT_PREV : ETX_EVT_NEXT;
  return 0;
}

int nav_update(nav_t *n, const nav_config_t *c, int st, const sticks_t *s, int64_t now_us) {
  if (st != n->state) {
    /* a roll held in flight must not press ENTER on the results screen: re-arm first */
    n->state = st;
    n->editing = false;
    n->armed = false;
    n->pitch_dir = n->roll_dir = 0;
  }
  int ev = 0;

  /* signal loss: was_live stays set through the outage, so a pause still happens if the game
   * starts flying while the sticks are gone; once per outage */
  if (s->live) {
    n->was_live = true;
    n->loss_sent = false;
  } else if (n->was_live && !n->loss_sent && c->pause_on_signal_loss && flight_state(st)) {
    n->loss_sent = true;
    ev = ETX_EVT_EXIT;
  }

  /* pause switch: the first position seen only sets the baseline */
  if (s->live && s->pause != INT32_MIN) {
    int pos = s->pause > PAUSE_HYST ? 1 : s->pause < -PAUSE_HYST ? -1 : n->pause_pos;
    if (pos != n->pause_pos) {
      if (n->pause_pos && pos > 0 && flight_state(st)) ev = ETX_EVT_EXIT;
      if (n->pause_pos && pos < 0 && st == GS_PAUSED) ev = ETX_EVT_EXIT;
      n->pause_pos = pos;
    }
  }
  if (ev) return ev;

  if (!c->enabled || !nav_state(st)) {
    n->armed = false;
    n->pitch_dir = n->roll_dir = 0;
    return 0;
  }
  int on = c->on_pct < 2 ? 2 : c->on_pct > 100 ? 100 : c->on_pct;
  int off = c->off_pct < 1 ? 1 : c->off_pct >= on ? on - 1 : c->off_pct;
  int a = clamp_stick(s->ail) * 100, e = clamp_stick(s->ele) * 100;
  int on_u = on * 1024, off_u = off * 1024;
  if (!n->armed) {
    if (a > -off_u && a < off_u && e > -off_u && e < off_u) n->armed = true;
    else return 0;
  }

  /* roll: once per deflection. A roll and a pitch in the same frame: the roll goes first, the
   * pitch is still "crossing" next frame. */
  if ((n->roll_dir > 0 && a < off_u) || (n->roll_dir < 0 && a > -off_u)) n->roll_dir = 0;
  if (!n->roll_dir) {
    if (a > on_u) {
      n->roll_dir = 1;
      return ETX_EVT_ENTER;
    }
    if (a < -on_u) {
      n->roll_dir = -1;
      if (st != GS_MENU || n->editing) return ETX_EVT_EXIT;   /* EXIT in the menu closes the game */
    }
  }

  /* pitch: on the crossing, then repeats while held */
  if ((n->pitch_dir < 0 && e < off_u) || (n->pitch_dir > 0 && e > -off_u)) n->pitch_dir = 0;
  if (!n->pitch_dir) {
    if (e > on_u || e < -on_u) {
      n->pitch_dir = e > 0 ? -1 : 1;
      n->pitch_next_us = now_us + (int64_t)c->repeat_first_ms * 1000;
      return nav_pitch_event(n, n->pitch_dir);
    }
    return 0;
  }
  if (now_us < n->pitch_next_us) return 0;
  int64_t rep = (int64_t)(c->repeat_ms > 0 ? c->repeat_ms : 1) * 1000;
  n->pitch_next_us += rep;
  if (n->pitch_next_us <= now_us) n->pitch_next_us = now_us + rep;   /* a stall: no burst */
  return nav_pitch_event(n, n->pitch_dir);
}

void nav_after_run(nav_t *n, int ev, int st) {
  /* the game shows its state, not focus/editing: in MENU (Track item) and SETUP, an ENTER that
   * keeps the state enters or leaves editing, an EXIT that keeps it leaves editing */
  if (st != n->prev_state) n->editing = false;
  else if (st == GS_MENU || st == GS_SETUP) {
    if (ev == ETX_EVT_ENTER) n->editing = !n->editing;
    else if (ev == ETX_EVT_EXIT) n->editing = false;
  }
  n->prev_state = st;
}

void nav_reset(nav_t *n) {
  n->editing = false;
  n->prev_state = GS_MENU;
  n->state = GS_UNKNOWN;
  n->armed = false;
  n->pitch_dir = n->roll_dir = 0;
}
