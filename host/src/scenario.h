/* Headless test scenarios: a Lua script (plain Lua, its own state) drives the host frame by frame
 * on a virtual clock, with PNG screenshots. Used by the tests and for screenshots.
 *
 * Functions a scenario can call:
 *   frames(n [, dt_ms])        run n frames (dt default 1000/60)
 *   key(name)                  queue an event: enter exit next prev inc dec up down nextpage prevpage menu
 *                              enterlong exitlong, or a number
 *   sticks(ail, ele, thr, rud) override the mixed sticks (-1024..1024); sticks() ends the override
 *   source(slot, kind, {ch..}) feed a stick source (slot 0..11, kind "wifi"/"usb"/"gamepad"/...; ch -1024..1024)
 *   unplug(slot)               the source goes away
 *   test(name, ...)            STICKTIME_TEST.<name>(...): returns its numbers / strings
 *   state()                    the game state (1 menu .. 8 done)
 *   editing()                  whether the host thinks a value is being edited
 *   shot(name)                 write <png-dir>/<name>.png of the current frame
 *   pixel(x, y)                RGB565 of a pixel;  hash()  FNV-1a hash of the frame
 *   err()                      the game's error text, or nil
 *   reload([av])               reload the game now; av: as if on analog video (STICKTIME_AV) or not
 *   frame_dt(on)               whether frames give the game their interval (STICKTIME_DT; default
 *                              on, as on the Pi; off like a radio or the browser emulator)
 *   setting(key, value)        change sticktime.txt and apply it (live keys)
 *   sd_write(path, text), sd_read(path), sd_remove(path)   the game's SD folder (SD paths)
 *   stats()                    {frames=, run_avg_ms=, run_max_ms=, lua_kb=}
 *   now()                      virtual time in ms;  size()  LCD_W, LCD_H
 *   log(...)                   print to stderr
 * The scenario fails (exit 1) on a Lua error in it or when it returns false. */
#ifndef SCENARIO_H
#define SCENARIO_H

#include "app.h"
#include "host.h"

int scenario_run(app_t *app, const host_options_t *opt, const char *path);

#endif
