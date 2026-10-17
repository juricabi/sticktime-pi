/* Headless test scenarios: see scenario.h. */
#include "scenario.h"
#include "etx.h"
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  host_t h;
  int64_t now_us;
  double dt_ms;
  char png_dir[512];
} scen_t;

static scen_t *S;

static int64_t vnow(void) { return S->now_us; }

static void one_frame(double dt_ms) {
  S->now_us += (int64_t)(dt_ms * 1000.0);
  host_frame(&S->h, S->now_us);
  host_present(&S->h);
  int64_t period = (int64_t)(dt_ms * 1000.0);
  host_note_shown(&S->h, S->now_us + period, S->now_us, period);
  host_publish(&S->h, S->now_us);
  if (S->h.audio_offline) host_audio_offline(&S->h, dt_ms);
}

static int l_frames(lua_State *L) {
  int n = (int)luaL_checkinteger(L, 1);
  double dt = luaL_optnumber(L, 2, S->dt_ms);
  if (dt <= 0 || dt > 1000) return luaL_error(L, "bad dt %f", dt);
  for (int i = 0; i < n; i++) one_frame(dt);
  return 0;
}

static const struct { const char *name; int code; } KEYS[] = {
  { "enter", ETX_EVT_ENTER }, { "exit", ETX_EVT_EXIT }, { "next", ETX_EVT_NEXT }, { "prev", ETX_EVT_PREV },
  { "inc", ETX_EVT_NEXT }, { "dec", ETX_EVT_PREV }, { "nextpage", ETX_EVT_NEXT_PAGE }, { "prevpage", ETX_EVT_PREV_PAGE },
  { "menu", ETX_EVT_MENU }, { "enterlong", ETX_EVT_ENTER_LONG }, { "exitlong", ETX_EVT_EXIT_LONG },
  { "up", HOST_KEY_UP }, { "down", HOST_KEY_DOWN },
};

static int l_key(lua_State *L) {
  if (lua_type(L, 1) == LUA_TNUMBER) { host_queue_key(&S->h, (int)lua_tointeger(L, 1)); return 0; }
  const char *k = luaL_checkstring(L, 1);
  for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++)
    if (!strcmp(k, KEYS[i].name)) { host_queue_key(&S->h, KEYS[i].code); return 0; }
  return luaL_error(L, "unknown key '%s'", k);
}

static int clampi(lua_Integer v) { return v < -1024 ? -1024 : v > 1024 ? 1024 : (int)v; }

static int l_sticks(lua_State *L) {
  if (lua_isnoneornil(L, 1)) { S->h.stick_override = false; return 0; }
  for (int i = 0; i < 4; i++) S->h.ov[i] = clampi(luaL_checkinteger(L, i + 1));
  S->h.stick_override = true;
  return 0;
}

static int l_source(lua_State *L) {
  int slot = (int)luaL_checkinteger(L, 1);
  if (slot < 0 || slot >= INPUT_MAX_SOURCES) return luaL_error(L, "bad slot");
  const char *kind = luaL_checkstring(L, 2);
  luaL_checktype(L, 3, LUA_TTABLE);
  insrc_t *s = &S->h.src[slot];
  insrc_kind_t k = mix_parse_input(kind);
  if (k == INSRC_NONE) return luaL_error(L, "bad kind '%s'", kind);
  s->kind = k;
  s->present = true;
  s->fixed_aetr = k == INSRC_GAMEPAD || k == INSRC_KEYBOARD;
  snprintf(s->name, sizeof s->name, "scenario %s", kind);
  int16_t ch[INPUT_CH] = { 0 };
  int n = (int)lua_rawlen(L, 3);
  if (n > INPUT_CH) n = INPUT_CH;
  for (int i = 0; i < n; i++) {
    lua_rawgeti(L, 3, i + 1);
    ch[i] = (int16_t)clampi(luaL_checkinteger(L, -1));
    lua_pop(L, 1);
  }
  insrc_update(s, ch, n < 4 ? 4 : n, vnow());
  return 0;
}

static int l_unplug(lua_State *L) {
  int slot = (int)luaL_checkinteger(L, 1);
  if (slot < 0 || slot >= INPUT_MAX_SOURCES) return luaL_error(L, "bad slot");
  S->h.src[slot].present = false;
  return 0;
}

static int l_test(lua_State *L) {
  const char *fn = luaL_checkstring(L, 1);
  int nargs = lua_gettop(L) - 1;
  double args[8];
  if (nargs > 8) nargs = 8;
  for (int i = 0; i < nargs; i++) args[i] = luaL_checknumber(L, i + 2);
  if (!S->h.etx || !etx_loaded(S->h.etx)) return 0;
  double out[24];
  int n = etx_test_call(S->h.etx, fn, args, nargs, out, 24);
  if (n < 0) return 0;
  for (int i = 0; i < n; i++) {
    if (out[i] != out[i]) {
      /* not a number: maybe a string (a track's name) */
      char str[256];
      if (etx_test_call_str(S->h.etx, fn, args, nargs, i, str, sizeof str) == 0) lua_pushstring(L, str);
      else lua_pushnil(L);
    } else {
      lua_pushnumber(L, out[i]);
    }
  }
  return n;
}

static int l_state(lua_State *L) { lua_pushinteger(L, S->h.game_state); return 1; }
static int l_editing(lua_State *L) { lua_pushboolean(L, S->h.nav.editing); return 1; }

static int l_shot(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  char path[1024];
  snprintf(path, sizeof path, "%s/%s.png", S->png_dir, name);
  plat_mkdirs(S->png_dir);
  if (video_snapshot_png(S->h.lcd.fb, S->h.lcd.w, S->h.lcd.h, path) != 0) return luaL_error(L, "can't write %s", path);
  return 0;
}

static int l_pixel(lua_State *L) {
  int x = (int)luaL_checkinteger(L, 1), y = (int)luaL_checkinteger(L, 2);
  if (x < 0 || y < 0 || x >= S->h.lcd.w || y >= S->h.lcd.h) return luaL_error(L, "pixel off screen");
  lua_pushinteger(L, S->h.lcd.fb[y * S->h.lcd.w + x]);
  return 1;
}

static int l_hash(lua_State *L) {
  uint32_t hsh = 2166136261u;
  const uint8_t *p = (const uint8_t *)S->h.lcd.fb;
  size_t n = (size_t)S->h.lcd.w * (size_t)S->h.lcd.h * 2;
  for (size_t i = 0; i < n; i++) { hsh ^= p[i]; hsh *= 16777619u; }
  char buf[16];
  snprintf(buf, sizeof buf, "%08x", hsh);
  lua_pushstring(L, buf);
  return 1;
}

static int l_err(lua_State *L) {
  const char *e = S->h.etx ? etx_error(S->h.etx) : S->h.err;
  bool running = S->h.etx && etx_loaded(S->h.etx) && S->h.last_result == 0;
  if (running || !e || !e[0]) lua_pushnil(L); else lua_pushstring(L, e);
  return 1;
}

static int l_reload(lua_State *L) {
  lua_pushboolean(L, host_reload_game(&S->h) == 0);
  return 1;
}

static int l_setting(lua_State *L) {
  const char *k = luaL_checkstring(L, 1);
  const char *v = luaL_optstring(L, 2, NULL);
  bool exists;
  settings_t *s = settings_load(S->h.app->paths.settings, &exists);
  if (!s) return luaL_error(L, "settings");
  if (!settings_set(s, k, v)) { settings_free(s); return luaL_error(L, "bad key '%s'", k); }
  int r = settings_save(s, S->h.app->paths.settings);
  settings_free(s);
  if (r != 0) return luaL_error(L, "can't save settings");
  host_apply_settings(&S->h);
  return 0;
}

static int sd_real(lua_State *L, const char *sdpath, char *out, size_t n) {
  if (snprintf(out, n, "%s%s%s", S->h.app->paths.sd, sdpath[0] == '/' ? "" : "/", sdpath) >= (int)n)
    return luaL_error(L, "path too long");
  if (strstr(sdpath, "..")) return luaL_error(L, "no '..' in SD paths");
  return 0;
}

static int l_sd_write(lua_State *L) {
  char p[1024];
  size_t len;
  const char *data = luaL_checklstring(L, 2, &len);
  sd_real(L, luaL_checkstring(L, 1), p, sizeof p);
  char dir[1024];
  plat_strlcpy(dir, p, sizeof dir);
  char *slash = strrchr(dir, '/');
  if (slash) { *slash = 0; plat_mkdirs(dir); }
  lua_pushboolean(L, plat_write_file_atomic(p, data, len) == 0);
  return 1;
}

static int l_sd_read(lua_State *L) {
  char p[1024];
  sd_real(L, luaL_checkstring(L, 1), p, sizeof p);
  if (S->h.etx) etx_flush_writes(S->h.etx);
  size_t n;
  char *d = plat_read_file(p, &n, 16 << 20);
  if (!d) { lua_pushnil(L); return 1; }
  lua_pushlstring(L, d, n);
  free(d);
  return 1;
}

static int l_sd_remove(lua_State *L) {
  char p[1024];
  sd_real(L, luaL_checkstring(L, 1), p, sizeof p);
  lua_pushboolean(L, plat_remove(p) == 0);
  return 1;
}

static int l_stats(lua_State *L) {
  app_status_t st;
  host_publish(&S->h, S->now_us + 1000000000LL);   /* force a snapshot */
  app_snapshot(S->h.app, &st);
  lua_newtable(L);
  lua_pushinteger(L, S->h.frames_drawn); lua_setfield(L, -2, "frames");
  lua_pushnumber(L, st.run_ms_avg); lua_setfield(L, -2, "run_avg_ms");
  lua_pushnumber(L, st.run_ms_p95); lua_setfield(L, -2, "run_p95_ms");
  lua_pushinteger(L, (lua_Integer)st.lua_kb); lua_setfield(L, -2, "lua_kb");
  lua_pushinteger(L, st.game_state); lua_setfield(L, -2, "state");
  lua_pushstring(L, st.game_version); lua_setfield(L, -2, "game_version");
  lua_pushboolean(L, st.sticks.live); lua_setfield(L, -2, "live");
  lua_pushinteger(L, st.sticks.src); lua_setfield(L, -2, "src");
  lua_pushinteger(L, st.sticks.ail); lua_setfield(L, -2, "ail");
  lua_pushinteger(L, st.sticks.ele); lua_setfield(L, -2, "ele");
  lua_pushinteger(L, st.sticks.thr); lua_setfield(L, -2, "thr");
  lua_pushinteger(L, st.sticks.rud); lua_setfield(L, -2, "rud");
  return 1;
}

static int l_now(lua_State *L) { lua_pushnumber(L, (double)(S->now_us - S->h.clock0_us) / 1000.0); return 1; }
static int l_size(lua_State *L) { lua_pushinteger(L, S->h.lcd.w); lua_pushinteger(L, S->h.lcd.h); return 2; }

static int l_log(lua_State *L) {
  int n = lua_gettop(L);
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  for (int i = 1; i <= n; i++) {
    if (i > 1) luaL_addchar(&b, '\t');
    luaL_tolstring(L, i, NULL);
    luaL_addvalue(&b);
  }
  luaL_pushresult(&b);
  fprintf(stderr, "[scenario] %s\n", lua_tostring(L, -1));
  return 0;
}

int scenario_run(app_t *app, const host_options_t *opt_in, const char *path) {
  static scen_t sc;
  S = &sc;
  memset(&sc, 0, sizeof sc);
  host_options_t opt = *opt_in;
  if (!opt.video_backend) opt.video_backend = "png";
  if (!opt.render_w) { opt.render_w = 640; opt.render_h = 480; }
  if (!opt.png_dir) opt.png_dir = "build/shots";
  plat_strlcpy(sc.png_dir, opt.png_dir, sizeof sc.png_dir);
  opt.no_inputs = true;
  opt.no_vtx = true;
  if (!opt.audio_out) opt.no_audio = true;
  opt.async_writes = false;
  opt.start_us = 1000000;
  opt.rt_prio = 0;
  sc.now_us = opt.start_us;
  sc.dt_ms = 1000.0 / 60.0;
  if (host_init(&sc.h, app, &opt) != 0) {
    fprintf(stderr, "scenario: host_init failed: %s\n", sc.h.err);
    return 1;
  }
  lua_State *L = luaL_newstate();
  luaL_openlibs(L);
  static const luaL_Reg fns[] = {
    { "frames", l_frames }, { "key", l_key }, { "sticks", l_sticks }, { "source", l_source }, { "unplug", l_unplug },
    { "test", l_test }, { "state", l_state }, { "editing", l_editing }, { "shot", l_shot }, { "pixel", l_pixel },
    { "hash", l_hash }, { "err", l_err }, { "reload", l_reload }, { "setting", l_setting },
    { "sd_write", l_sd_write }, { "sd_read", l_sd_read }, { "sd_remove", l_sd_remove }, { "stats", l_stats },
    { "now", l_now }, { "size", l_size }, { "log", l_log }, { NULL, NULL },
  };
  for (const luaL_Reg *f = fns; f->name; f++) lua_register(L, f->name, f->func);
  /* SCEN_DIR: the scenario's folder (for dofile of shared helpers); GALLERY_TRACK from the environment */
  char dir[1024];
  plat_strlcpy(dir, path, sizeof dir);
  for (char *c = dir; *c; c++) if (*c == '\\') *c = '/';
  char *slash = strrchr(dir, '/');
  if (slash) *slash = 0; else plat_strlcpy(dir, ".", sizeof dir);
  lua_pushstring(L, dir);
  lua_setglobal(L, "SCEN_DIR");
  const char *gt = getenv("STICKTIME_GALLERY_TRACK");
  if (gt) { lua_pushstring(L, gt); lua_setglobal(L, "GALLERY_TRACK"); }
  int rc = 0;
  if (luaL_loadfile(L, path) != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
    fprintf(stderr, "scenario: %s\n", lua_tostring(L, -1));
    rc = 1;
  } else if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
    fprintf(stderr, "scenario: returned false\n");
    rc = 1;
  }
  lua_close(L);
  host_close(&sc.h);
  fprintf(stderr, "scenario: %s\n", rc ? "FAILED" : "OK");
  return rc;
}
