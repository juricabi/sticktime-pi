/* Menus with the sticks, so the pilot never takes the goggles off (Betaflight OSD style).
 * Spec: game-api notes §2.2-2.4 (the game exposes `state` via STICKTIME_TEST.get(), not focus/editing).
 *
 * Active in MENU, SETUP, PAUSED and DONE:
 *   pitch up (ele > +on)    not editing: PREV 0x1D00 (focus up)   editing: INC 0x1E00 (value up)
 *   pitch down (ele < -on)  not editing: NEXT 0x1E00 (focus down) editing: DEC 0x1D00 (value down)
 *     -> first event on crossing, repeats while held: after repeat_first_ms, then every repeat_ms
 *   roll right (ail > +on)  ENTER 0x202, once per deflection
 *   roll left (ail < -on)   EXIT 0x201, once per deflection; NEVER in MENU while not editing (it closes the game)
 * on = on_pct of full stick, released below off_pct (hysteresis).
 * Arming: after the state changes into a nav state, nothing fires until ail and ele have both been
 * below off_pct once (a roll held in flight must not press ENTER on the results screen).
 * Editing inference (after each run()): state changed -> editing = false; else in MENU/SETUP:
 * ENTER toggles editing, EXIT clears it. Reload / script closed -> editing = false.
 * Pause switch (pause channel, threshold 0 with hysteresis +-200, ignored until seen once):
 *   edge to "on"  -> EXIT if state is COUNT, FLY, CRASHED or READY (pauses)
 *   edge to "off" -> EXIT if state is PAUSED (resumes)
 * Signal loss: the active source stops being live while in COUNT/FLY/CRASHED/READY -> EXIT (pause), once.
 * Pure logic; time comes in as an argument. */
#ifndef NAV_H
#define NAV_H

#include <stdint.h>
#include <stdbool.h>
#include "input.h"

typedef struct {
  int state;                  /* game state at the last nav_update */
  int prev_state;             /* for the editing inference */
  bool editing;
  bool armed;
  int pitch_dir;              /* -1 up, 0, +1 down: held */
  int64_t pitch_next_us;      /* when the next repeat fires */
  int roll_dir;               /* -1, 0, +1: held (fires once) */
  int pause_pos;              /* -1 off, +1 on, 0 not seen yet */
  bool was_live;
  bool loss_sent;
} nav_t;

typedef struct {
  bool enabled;               /* sticks drive the menus (stick_menus) */
  int on_pct, off_pct;        /* 60, 40 */
  int repeat_first_ms, repeat_ms;   /* 450, 150 */
  bool pause_on_signal_loss;  /* true */
} nav_config_t;

void nav_init(nav_t *n);
void nav_default_config(nav_config_t *c);
/* Before run(): returns the ETX_EVT_* to deliver from the sticks / pause switch / signal loss, or 0. */
int  nav_update(nav_t *n, const nav_config_t *c, int game_state, const sticks_t *s, int64_t now_us);
/* After run(): the event that was delivered (from any source: sticks, keyboard, web) and the state now. */
void nav_after_run(nav_t *n, int delivered_event, int state_after);
/* The script was reloaded or closed. */
void nav_reset(nav_t *n);
/* Keyboard Up/Down use the same editing-aware mapping: dir -1 = up, +1 = down. */
int  nav_pitch_event(const nav_t *n, int dir);

#endif
