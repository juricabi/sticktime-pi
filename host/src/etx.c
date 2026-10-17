/* EdgeTX Lua API for one tool script (see etx.h): a port of engine.js installApi() and the
 * colorLcd() bindings. The drawing itself is lcd.c; this file checks the arguments the way
 * EdgeTX does (luaL_checkinteger with LUA_FLOORN2I floors floats, flags are uint32) and maps
 * the "SD card" to a real folder. */
#include "etx.h"
#include "plat.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ETX_PATH_MAX 1000           /* plat's file helpers use 1024-byte buffers (+ ".tmp~") */
#define ETX_MAX_FILES 16            /* open io handles per state */
#define ETX_MAX_FILE (4u << 20)     /* the most io.open / loadScript read, and io.write keeps */
#define ETX_MEM_LIMIT ((size_t)256 << 20)   /* Lua heap while script code runs */
#define HOOK_COUNT 1000             /* instructions between watchdog checks */
#define NSLOTS 8                    /* StickTimeTracks/c1..c8.txt */
#define SLOT_SNAP_MAX 4096          /* the game reads 60 and 1000 bytes of a slot */
#define FILE_MT "FILE*"

/* ================================================================ writer thread
 * One for the process, so pending writes (and the read-through cache) survive a reload, which
 * frees one etx_t and makes another. Writes to the same path coalesce: the newest wins. */
typedef struct wjob {
  struct wjob *next;
  char path[ETX_PATH_MAX + 1];
  char *data;
  size_t len;
  uint32_t gen;
  etx_log_fn log;
  void *log_ud;
} wjob_t;

static struct {
  bool init, started, hold;
  plat_mutex mu;
  plat_cond work, idle;
  plat_thread th;
  wjob_t *head;
} W;

/* FAT and NTFS ignore case: so does the cache */
static bool path_eq(const char *a, const char *b) {
  for (;; a++, b++) {
    int x = (unsigned char)*a, y = (unsigned char)*b;
    if (x >= 'A' && x <= 'Z') x += 32;
    if (y >= 'A' && y <= 'Z') y += 32;
    if (x != y) return false;
    if (!x) return true;
  }
}

static void log_to(etx_log_fn fn, void *ud, const char *fmt, ...) {
  char line[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  if (fn) fn(ud, line);
  else fprintf(stderr, "etx: %s\n", line);
}

static int commit_file(const char *path, const void *data, size_t len) {
  char dir[ETX_PATH_MAX + 1];
  plat_strlcpy(dir, path, sizeof dir);
  char *slash = strrchr(dir, '/');
  if (slash && slash != dir) {
    *slash = 0;
    if (plat_mkdirs(dir) != 0) return -1;
  }
  return plat_write_file_atomic(path, data, len);
}

static void w_init(void) {
  if (W.init) return;
  W.init = true;
  plat_mutex_init(&W.mu);
  plat_cond_init(&W.work);
  plat_cond_init(&W.idle);
}

static void *w_main(void *arg) {
  (void)arg;
  plat_thread_lower_priority();
  plat_mutex_lock(&W.mu);
  for (;;) {
    wjob_t *j = W.hold ? NULL : W.head;
    if (!j) {
      plat_cond_broadcast(&W.idle);
      plat_cond_wait(&W.work, &W.mu, -1);
      continue;
    }
    /* write a copy, so the frame thread can replace the data meanwhile */
    size_t len = j->len;
    uint32_t gen = j->gen;
    char path[ETX_PATH_MAX + 1];
    memcpy(path, j->path, sizeof path);
    etx_log_fn log = j->log;
    void *ud = j->log_ud;
    char *copy = malloc(len ? len : 1);
    if (copy) memcpy(copy, j->data, len);
    plat_mutex_unlock(&W.mu);
    int r = copy ? commit_file(path, copy, len) : -1;
    int err = copy ? errno : ENOMEM;
    free(copy);
    if (r != 0) log_to(log, ud, "io: can't write %s: %s", path, strerror(err));
    plat_mutex_lock(&W.mu);
    if (j->gen == gen) {           /* else newer data came: write it next round */
      wjob_t **pp = &W.head;
      while (*pp != j) pp = &(*pp)->next;
      *pp = j->next;
      free(j->data);
      free(j);
    }
  }
  return NULL;
}

/* takes data (malloc'd) on success */
static int w_enqueue(const char *path, char *data, size_t len, etx_log_fn log, void *ud) {
  w_init();
  plat_mutex_lock(&W.mu);
  if (!W.started) {
    if (plat_thread_start(&W.th, w_main, NULL, true) != 0) { plat_mutex_unlock(&W.mu); return -1; }
    W.started = true;
  }
  wjob_t *j = W.head, **tail = &W.head;
  for (; j && !path_eq(j->path, path); j = j->next) tail = &j->next;
  if (j) {
    free(j->data);
  } else {
    j = calloc(1, sizeof *j);
    if (!j) { plat_mutex_unlock(&W.mu); return -1; }
    plat_strlcpy(j->path, path, sizeof j->path);
    *tail = j;
  }
  j->data = data;
  j->len = len;
  j->gen++;
  j->log = log;
  j->log_ud = ud;
  plat_cond_signal(&W.work);
  plat_mutex_unlock(&W.mu);
  return 0;
}

/* a copy of the pending content of path, or NULL (none pending / no memory) */
static char *w_lookup(const char *path, size_t *len) {
  if (!W.init) return NULL;
  char *out = NULL;
  plat_mutex_lock(&W.mu);
  for (wjob_t *j = W.head; j; j = j->next) {
    if (!path_eq(j->path, path)) continue;
    out = malloc(j->len + 1);
    if (out) { memcpy(out, j->data, j->len); out[j->len] = 0; *len = j->len; }
    break;
  }
  plat_mutex_unlock(&W.mu);
  return out;
}

void etx_writer_hold(bool hold) {
  w_init();
  plat_mutex_lock(&W.mu);
  W.hold = hold;
  plat_cond_broadcast(&W.work);
  plat_mutex_unlock(&W.mu);
}

/* ================================================================ state */
typedef struct {
  bool used, dirty;
  uint32_t gen;
  char mode;                       /* 'r', 'w', 'a' */
  bool plus;
  char path[ETX_PATH_MAX + 1];     /* real path */
  char *buf;
  size_t len, cap, pos;
} efile_t;

typedef struct { int slot; uint32_t gen; } ehandle_t;

struct etx {
  lcd_t *lcd;
  etx_config_t cfg;
  char sd_root[ETX_PATH_MAX + 1];
  char script[ETX_PATH_MAX + 1];
  etx_log_fn log;
  void *log_ud;

  lua_State *L;
  int run_ref, init_ref, check_ref;
  bool loaded;
  int result;                      /* 0 running, 1 closed, -1 error */
  char err[4096];
  char test_err[512];

  int64_t clock_ms;
  int sticks[4];

  int depth;                       /* nesting of script calls (watchdog, memory limit) */
  int64_t deadline_us;
  bool expired;                    /* the watchdog fired: the hook runs every instruction */
  int64_t instr;
  size_t mem;
  int frames;

  etx_tone_t tones[ETX_TONE_QUEUE];
  int tone_head, tone_n;

  efile_t files[ETX_MAX_FILES];
  uint32_t file_gen;
  char *snap[NSLOTS];              /* the last good content of each listed track slot */
  size_t snap_len[NSLOTS];
};

static etx_t *E(lua_State *L) { return *(etx_t **)lua_getextraspace(L); }

static void elog(etx_t *e, const char *fmt, ...) {
  char line[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  log_to(e->log, e->log_ud, "%s", line);
}

/* ================================================================ SD paths */
/* "/SCRIPTS//TOOLS/./x" -> "SCRIPTS/TOOLS/x". '\' separates too (FatFs accepts it). A component
 * of only dots and spaces ("..", "...", ".. ") is refused: Windows strips trailing dots and
 * spaces, so those could climb out. ':' (drives, NTFS streams) and control characters too. */
static int sd_rel(const char *p, char *out, size_t outlen) {
  size_t o = 0;
  if (!p || outlen == 0) return -1;
  while (*p) {
    while (*p == '/' || *p == '\\') p++;
    if (!*p) break;
    const char *s = p;
    while (*p && *p != '/' && *p != '\\') p++;
    size_t n = (size_t)(p - s);
    if (n == 1 && s[0] == '.') continue;
    bool dots = true;
    for (size_t i = 0; i < n; i++) {
      unsigned char c = (unsigned char)s[i];
      if (c != '.' && c != ' ') dots = false;
      if (c < 0x20 || c == ':' || c == 0x7F) return -1;
    }
    if (dots) return -1;
    if (o + (o ? 1 : 0) + n >= outlen) return -1;
    if (o) out[o++] = '/';
    memcpy(out + o, s, n);
    o += n;
  }
  out[o] = 0;
  return 0;
}

int etx_sd_path(const etx_t *e, const char *sdpath, char *out, size_t outlen) {
  char rel[ETX_PATH_MAX + 1];
  if (!out || outlen == 0) return -1;
  out[0] = 0;
  if (sd_rel(sdpath, rel, sizeof rel) != 0) return -1;
  const char *root = e->sd_root[0] ? e->sd_root : ".";
  size_t rl = strlen(root);
  while (rl > 1 && (root[rl - 1] == '/' || root[rl - 1] == '\\')) rl--;
  bool slash_root = rl == 1 && root[0] == '/';
  int n;
  if (!rel[0]) n = snprintf(out, outlen, "%.*s", (int)rl, root);
  else n = snprintf(out, outlen, "%.*s/%s", slash_root ? 0 : (int)rl, root, rel);
  if (n < 0 || (size_t)n >= outlen || (size_t)n > ETX_PATH_MAX) { out[0] = 0; return -1; }
  return 0;
}

static int slot_of(const char *sdpath) {
  static const char dir[] = "SCRIPTS/TOOLS/StickTimeTracks/c";
  char rel[ETX_PATH_MAX + 1];
  if (sd_rel(sdpath, rel, sizeof rel) != 0) return 0;
  size_t n = sizeof dir - 1;
  if (strlen(rel) != n + 5) return 0;
  char head[sizeof dir];
  memcpy(head, rel, n);
  head[n] = 0;
  if (!path_eq(head, dir) || rel[n] < '1' || rel[n] > '8' || !path_eq(rel + n + 1, ".txt")) return 0;
  return rel[n] - '0';
}

/* 1 found (buf malloc'd, NUL-terminated), 0 missing, -1 error (errno). Pending writes first. */
static int load_file(const char *real, char **buf, size_t *len) {
  *buf = w_lookup(real, len);
  if (*buf) return 1;
  *buf = plat_read_file(real, len, ETX_MAX_FILE);
  if (*buf) return 1;
  int err = errno;
  plat_stat_t st;
  if (plat_stat(real, &st) == 0 && !st.exists) return 0;
  errno = st.is_dir ? EISDIR : (err ? err : EIO);
  return -1;
}

/* ================================================================ Lua glue */
static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  etx_t *e = ud;
  size_t old = ptr ? osize : 0;
  if (nsize == 0) {
    free(ptr);
    e->mem -= old;
    return NULL;
  }
  /* only script code gets "not enough memory": the host's own calls must never fail */
  if (nsize > old && e->depth > 0 && e->mem - old + nsize > ETX_MEM_LIMIT) return NULL;
  void *p = realloc(ptr, nsize);
  if (!p) return nsize <= old ? ptr : NULL;
  e->mem = e->mem - old + nsize;
  return p;
}

static int l_panic(lua_State *L) {
  fprintf(stderr, "etx: unprotected Lua error: %s\n", lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
  return 0;   /* Lua aborts */
}

static void hook(lua_State *L, lua_Debug *ar) {
  (void)ar;
  etx_t *e = E(L);
  e->instr += e->expired ? 1 : HOOK_COUNT;
  if (e->deadline_us && plat_now_us() > e->deadline_us) {
    if (!e->expired) {
      /* from now on every instruction raises it: a script that catches the error with pcall
       * gets one instruction outside it, not another chance to loop */
      e->expired = true;
      lua_sethook(e->L, hook, LUA_MASKCOUNT, 1);
      if (L != e->L) lua_sethook(L, hook, LUA_MASKCOUNT, 1);
    }
    /* level 0: a hook runs in the frame of the interrupted function (luaL_error's level 1 would
     * name its caller, usually C) */
    luaL_where(L, 0);
    lua_pushliteral(L, "script took too long");
    lua_concat(L, 2);
    lua_error(L);
  }
}

static void script_enter(etx_t *e) {
  if (e->depth++ == 0) {
    int ms = e->cfg.watchdog_ms > 0 ? e->cfg.watchdog_ms : 2000;
    e->deadline_us = plat_now_us() + (int64_t)ms * 1000;
  }
}

static void script_leave(etx_t *e) {
  if (--e->depth > 0) return;
  e->deadline_us = 0;
  if (e->expired) {
    e->expired = false;
    if (e->L) lua_sethook(e->L, hook, LUA_MASKCOUNT, HOOK_COUNT);
  }
}

/* engine.js pcall(): the message gets a traceback */
static int msgh(lua_State *L) {
  const char *msg = lua_tostring(L, 1);
  if (!msg) {
    if (luaL_callmeta(L, 1, "__tostring") && lua_type(L, -1) == LUA_TSTRING) msg = lua_tostring(L, -1);
    else msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
  }
  luaL_traceback(L, L, msg, 1);
  return 1;
}

static int pcall_wd(etx_t *e, int nargs, int nres, bool traceback) {
  lua_State *L = e->L;
  int base = 0;
  if (traceback) {
    base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, msgh);
    lua_insert(L, base);
  }
  script_enter(e);
  int st = lua_pcall(L, nargs, nres, base);
  script_leave(e);
  if (traceback) lua_remove(L, base);
  return st;
}

static int fail(etx_t *e, const char *title, const char *msg) {
  snprintf(e->err, sizeof e->err, "%s: %s", title, msg && *msg ? msg : "(no message)");
  e->result = -1;
  return -1;
}

static int fail_pop(etx_t *e, const char *title) {
  const char *m = lua_tostring(e->L, -1);
  fail(e, title, m ? m : "(no message)");
  lua_pop(e->L, 1);
  return -1;
}

/* EdgeTX reads flags with luaL_optunsigned; engine.js floors and takes the low 32 bits. Integers
 * keep their bits (lcd.RGB colors are negative int32); floats beyond int32 (0xFFFF8380 written in
 * decimal) wrap like engine.js; NaN and infinities give 0. */
static uint32_t flags_arg(lua_State *L, int i, uint32_t def) {
  if (lua_isnoneornil(L, i)) return def;
  int isint = 0;
  lua_Integer v = lua_tointegerx(L, i, &isint);
  if (isint) return (uint32_t)v;
  if (lua_isnumber(L, i)) {
    double f = floor((double)lua_tonumber(L, i));
    if (!isfinite(f)) return 0;
    f = fmod(f, 4294967296.0);
    if (f < 0) f += 4294967296.0;
    return (uint32_t)f;
  }
  return (uint32_t)luaL_checkinteger(L, i);   /* the standard error */
}

static int ci(lua_State *L, int i) { return (int)luaL_checkinteger(L, i); }
static int oi(lua_State *L, int i, int d) { return (int)luaL_optinteger(L, i, d); }

/* integers when they are (the firmware sets TOOL_RUN_US as integers), else floats */
static void push_num(lua_State *L, double v) {
  if (v == floor(v) && v >= (double)LUA_MININTEGER && v <= (double)LUA_MAXINTEGER) lua_pushinteger(L, (lua_Integer)v);
  else lua_pushnumber(L, (lua_Number)v);
}

/* ================================================================ general functions */
static int src_id(const char *s) {
  static const char *names[4] = { "ail", "ele", "thr", "rud" };
  if (strlen(s) != 3) return 0;
  for (int k = 0; k < 4; k++) {
    int i = 0;
    for (; i < 3; i++) {
      int c = (unsigned char)s[i];
      if (c >= 'A' && c <= 'Z') c += 32;
      if (c != names[k][i]) break;
    }
    if (i == 3) return k + 1;
  }
  return 0;
}

static int l_getTime(lua_State *L) {
  int64_t ms = E(L)->clock_ms;
  int64_t t = ms >= 0 ? ms / 10 : -((-ms + 9) / 10);
  lua_pushinteger(L, (lua_Integer)t);
  return 1;
}

static int l_getValue(lua_State *L) {
  etx_t *e = E(L);
  int id = lua_type(L, 1) == LUA_TNUMBER ? ci(L, 1) : src_id(luaL_checkstring(L, 1));
  int v = id >= 1 && id <= 4 ? e->sticks[id - 1] : 0;
  lua_pushinteger(L, v < -1024 ? -1024 : v > 1024 ? 1024 : v);
  return 1;
}

static int l_getFieldInfo(lua_State *L) {
  const char *s = luaL_checkstring(L, 1);
  int id = src_id(s);
  if (!id) { lua_pushnil(L); return 1; }
  static const char *names[4] = { "ail", "ele", "thr", "rud" };
  lua_createtable(L, 0, 3);
  lua_pushinteger(L, id);
  lua_setfield(L, -2, "id");
  lua_pushstring(L, names[id - 1]);
  lua_setfield(L, -2, "name");
  lua_pushstring(L, names[id - 1]);
  lua_setfield(L, -2, "desc");
  return 1;
}

static int l_getStickMode(lua_State *L) {
  int m = E(L)->cfg.stick_mode;
  lua_pushinteger(L, m >= 1 && m <= 4 ? m : 2);
  return 1;
}

static void tone_push(etx_t *e, const etx_tone_t *t) {
  if (e->tone_n == ETX_TONE_QUEUE) {          /* full: drop the oldest */
    e->tone_head = (e->tone_head + 1) % ETX_TONE_QUEUE;
    e->tone_n--;
  }
  e->tones[(e->tone_head + e->tone_n) % ETX_TONE_QUEUE] = *t;
  e->tone_n++;
}

static int l_playTone(lua_State *L) {
  etx_tone_t t;
  memset(&t, 0, sizeof t);
  t.freq = ci(L, 1);
  t.len_ms = ci(L, 2);
  t.pause_ms = oi(L, 3, 0);
  t.flags = (int)flags_arg(L, 4, 0);
  t.freq_incr = oi(L, 5, 0);
  t.volume = oi(L, 6, 0);
  tone_push(E(L), &t);
  return 0;
}

static int l_playHaptic(lua_State *L) {
  etx_tone_t t;
  memset(&t, 0, sizeof t);
  t.haptic = true;
  t.len_ms = ci(L, 1);
  t.pause_ms = oi(L, 2, 0);
  t.flags = (int)flags_arg(L, 3, 0);
  tone_push(E(L), &t);
  return 0;
}

static int l_nop(lua_State *L) { (void)L; return 0; }
static int l_zero(lua_State *L) { lua_pushinteger(L, 0); return 1; }

static int l_getAvailableMemory(lua_State *L) { lua_pushinteger(L, 4 * 1024 * 1024); return 1; }

static int l_getVersion(lua_State *L) {
  lua_pushliteral(L, "2.12.0-emu");
  lua_pushliteral(L, "emulator");
  lua_pushinteger(L, 2);
  lua_pushinteger(L, 12);
  lua_pushinteger(L, 0);
  lua_pushliteral(L, "EdgeTX");
  return 6;
}

static int l_print(lua_State *L) {
  char line[1024];
  size_t o = 0;
  int n = lua_gettop(L);
  for (int i = 1; i <= n; i++) {
    size_t len;
    const char *s = luaL_tolstring(L, i, &len);
    if (i > 1 && o < sizeof line - 1) line[o++] = '\t';
    if (len > sizeof line - 1 - o) len = sizeof line - 1 - o;
    memcpy(line + o, s, len);
    o += len;
    lua_pop(L, 1);
  }
  line[o] = 0;
  etx_t *e = E(L);
  log_to(e->log, e->log_ud, "lua: %s", line);
  return 0;
}

static int l_model_getInfo(lua_State *L) {
  lua_createtable(L, 0, 1);
  lua_pushliteral(L, "SIM");
  lua_setfield(L, -2, "name");
  return 1;
}

/* load() without binary chunks: crafted bytecode can break out of the Lua sandbox */
static int l_load_text(lua_State *L) {
  luaL_checkany(L, 1);
  while (lua_gettop(L) < 3) lua_pushnil(L);
  lua_pushliteral(L, "t");
  lua_replace(L, 3);
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_insert(L, 1);
  lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
  return lua_gettop(L);
}

/* setmetatable refuses __gc: Lua runs finalizers with hooks off (lgc.c GCTM), so the watchdog
 * could not stop a hung one, and lua_close runs them all. Only a __gc present at setmetatable
 * time makes an object finalizable, so a raw check here is enough. The game uses no metatables. */
static int l_setmetatable(lua_State *L) {
  int t = lua_type(L, 2);
  luaL_checktype(L, 1, LUA_TTABLE);       /* (here, so the messages name 'setmetatable') */
  luaL_argcheck(L, t == LUA_TNIL || t == LUA_TTABLE, 2, "nil or table expected");
  if (t == LUA_TTABLE) {
    lua_pushliteral(L, "__gc");
    if (lua_rawget(L, 2) != LUA_TNIL) return luaL_argerror(L, 2, "__gc metamethods are not supported");
    lua_pop(L, 1);
  }
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_insert(L, 1);
  lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
  return lua_gettop(L);
}

/* xpcall: when the watchdog raises its error, Lua calls the message handler from inside the hook,
 * with hooks off, where nothing could stop a handler that loops. Once the watchdog fired, the
 * script's handler is skipped and the message passes through. (A handler that loops on an
 * ordinary error is stopped by the hook, whose error then comes through here as well.) */
static int xpcall_handler(lua_State *L) {
  if (E(L)->expired) return 1;
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_insert(L, 1);
  lua_call(L, lua_gettop(L) - 1, 1);
  return 1;
}

static int xpcall_finish(lua_State *L, int status, lua_KContext extra) {
  if (status != LUA_OK && status != LUA_YIELD) {
    lua_pushboolean(L, 0);
    lua_pushvalue(L, -2);
    return 2;
  }
  return lua_gettop(L) - (int)extra;
}

/* luaB_xpcall with the handler wrapped (lua_pcallk, so the function can still yield) */
static int l_xpcall(lua_State *L) {
  int n = lua_gettop(L);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  lua_pushvalue(L, 2);
  lua_pushcclosure(L, xpcall_handler, 1);
  lua_replace(L, 2);
  lua_pushboolean(L, 1);
  lua_pushvalue(L, 1);
  lua_rotate(L, 3, 2);
  int status = lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, xpcall_finish);
  return xpcall_finish(L, status, 2);
}

/* ================================================================ io (EdgeTX signatures) */
static efile_t *getfile(lua_State *L, ehandle_t **hp) {
  ehandle_t *h = luaL_checkudata(L, 1, FILE_MT);
  if (hp) *hp = h;
  if (h->slot < 0 || h->slot >= ETX_MAX_FILES) return NULL;
  efile_t *f = &E(L)->files[h->slot];
  return f->used && f->gen == h->gen ? f : NULL;
}

static void file_release(efile_t *f) {
  free(f->buf);
  memset(f, 0, sizeof *f);
}

static int io_fail(lua_State *L, const char *path, const char *why) {
  lua_pushnil(L);
  lua_pushfstring(L, "%s: %s", path, why);
  return 2;
}

/* Track slots: io.open of StickTimeTracks/cN.txt returns nil when the game would crash on the
 * file (game-api.md §10 A-C), checked by this function with the game's own patterns:
 * listed: the game's menu scan takes it (header in the first 60 bytes, format 1);
 * buildable: buildTrack works on it (a seed token, no NaN/inf, no fractional types that pass
 * its type checks) and so does the ghost save (an int32 id, coordinates |v| < 1e8). Verified
 * against the real game with the filter off; a few files it hides do not crash (an inf seed or
 * height, most values beyond 1e8), which only hand-made files have. It runs in the script's
 * state, with the string library as it was at load.
 * Fractions are tested with "x - floor(x) > 0": under LUA_FLOORN2I a float equals an integer
 * when it floors to it (1.005 == 1), while table keys stay exact (GHW[1.005] is nil). */
static const char SLOT_CHECK[] =
  "local match, gmatch, sub, tonumber, floor, huge, mtype = string.match, string.gmatch, string.sub, tonumber,\n"
  "  math.floor, math.huge, math.type\n"
  /* the ghost file writes the first gate's x, z times 10 with %d: |v| < 1e8 keeps them in int32 */
  "local function sane(v) return v == v and v > -1e8 and v < 1e8 end\n"
  "return function(s)\n"
  "  local name, ver, id = match(sub(s, 1, 60), '^%s*([^\\r\\n]+)[\\r\\n]+%s*(%d+)%s+(%d+)')\n"
  "  local listed = name ~= nil and ver == '1'\n"
  /* an id beyond int32 is a float under LUA_32BITS: the ghost file's %d raises on it */
  "  if listed and mtype(tonumber(id)) ~= 'integer' then return listed, false end\n"
  "  local g, b = match(match(sub(s, 1, 1000), '^%s*[^\\r\\n]*[\\r\\n]+(.*)') or '', '([^/]*)/?(.*)')\n"
  "  local n, d, seed = 0, {}, nil\n"
  "  for v in gmatch(g or '', '%S+') do\n"
  "    n = n + 1\n"
  "    if n == 3 then seed = tonumber(v) or 1\n"
  "    elseif n > 4 and n <= 124 then d[n - 4] = tonumber(v) or 0 end\n"
  "  end\n"
  "  if n < 3 or seed ~= seed or seed == huge or seed == -huge then return listed, false end\n"
  "  local ok = #d >= 10 and #d % 5 == 0\n"
  "  for i = 3, #d, 5 do if d[i] < 1 or d[i] > 8 or d[i] - floor(d[i]) > 0.01 then ok = false end end\n"
  "  if ok then\n"
  "    for i = 1, #d do if not sane(d[i]) then return listed, false end end\n"
  "    for i = 3, #d, 5 do if d[i] - floor(d[i]) > 0 then return listed, false end end\n"
  "  end\n"
  "  local bd = {}\n"
  "  for v in gmatch(b or '', '%S+') do if #bd < 112 then bd[#bd + 1] = tonumber(v) or 0 end end\n"
  "  ok = #bd % 7 == 0\n"
  "  for i = 7, #bd, 7 do if bd[i] < 1 or bd[i] > 3 or bd[i] - floor(bd[i]) > 0.01 then ok = false end end\n"
  "  if ok then\n"
  "    for i = 1, #bd do if not sane(bd[i]) then return listed, false end end\n"
  "    for i = 7, #bd, 7 do if bd[i] - floor(bd[i]) > 0 then return listed, false end end\n"
  "  end\n"
  "  return listed, true\n"
  "end\n";

/* Decide what a read of slot n (1..8) sees. f holds the file (found) or nothing. Returns false
 * when the open must fail. Once a slot was listed, the game may build it any time: then a bad or
 * missing file is replaced by the last good one instead of crashing the game. */
static bool slot_filter(lua_State *L, etx_t *e, int n, efile_t *f, bool found, const char *sdpath) {
  bool listed = false, buildable = false;
  int k = n - 1;
  if (found) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, e->check_ref);
    lua_pushlstring(L, f->buf, f->len);
    lua_call(L, 1, 2);
    listed = lua_toboolean(L, -2);
    buildable = lua_toboolean(L, -1);
    lua_pop(L, 2);
  }
  if (found && buildable) {
    if (listed) {
      size_t len = f->len < SLOT_SNAP_MAX ? f->len : SLOT_SNAP_MAX;
      char *s = malloc(len + 1);
      if (s) {
        memcpy(s, f->buf, len);
        s[len] = 0;
        free(e->snap[k]);
        e->snap[k] = s;
        e->snap_len[k] = len;
      }
    }
    return true;
  }
  if (e->snap[k]) {
    char *s = malloc(e->snap_len[k] + 1);
    if (!s) return false;
    memcpy(s, e->snap[k], e->snap_len[k] + 1);
    free(f->buf);
    f->buf = s;
    f->len = f->cap = e->snap_len[k];
    elog(e, "tracks: %s %s, keeping the version the game loaded", sdpath, found ? "is unreadable" : "is gone");
    return true;
  }
  if (found && !listed) return true;   /* the game's scan skips it */
  if (found) elog(e, "tracks: %s hidden: the game would crash on it", sdpath);
  return false;
}

static int io_open(lua_State *L) {
  etx_t *e = E(L);
  size_t plen;
  const char *path = luaL_checklstring(L, 1, &plen);
  const char *mode = luaL_optstring(L, 2, "r");
  /* EdgeTX's liolib check: [rwa]%+?b* */
  bool ok = mode[0] && strchr("rwa", mode[0]);
  bool plus = ok && mode[1] == '+';
  if (ok) {
    for (const char *m = mode + 1 + (plus ? 1 : 0); *m; m++)
      if (*m != 'b') { ok = false; break; }
  }
  if (!ok) return luaL_error(L, "invalid mode '%s' (should match '[rwa]%%+?b*')", mode);
  char m = mode[0];

  char real[ETX_PATH_MAX + 1];
  if (strlen(path) != plen || etx_sd_path(e, path, real, sizeof real) != 0) return io_fail(L, path, "invalid path");
  int slot = -1;
  for (int i = 0; i < ETX_MAX_FILES; i++) if (!e->files[i].used) { slot = i; break; }
  if (slot < 0) return io_fail(L, path, "too many open files");
  if (m == 'w') {                      /* (r and a find out when they read it) */
    plat_stat_t st;
    if (plat_stat(real, &st) == 0 && st.is_dir) return io_fail(L, path, "is a directory");
  }

  /* the handle first: if anything below raises an error, its __gc frees the slot */
  ehandle_t *h = lua_newuserdata(L, sizeof *h);
  h->slot = -1;
  h->gen = 0;
  luaL_setmetatable(L, FILE_MT);

  char *buf = NULL;
  size_t len = 0;
  bool found = false;
  if (m != 'w') {
    int r = load_file(real, &buf, &len);
    if (r < 0) return io_fail(L, path, r == -1 && errno == EISDIR ? "is a directory" : strerror(errno));
    found = r == 1;
  }
  int n = m == 'r' && !plus ? slot_of(path) : 0;
  if (!found && m == 'r' && !n) return io_fail(L, path, "No such file or directory");

  efile_t *f = &e->files[slot];
  memset(f, 0, sizeof *f);
  f->used = true;
  f->gen = ++e->file_gen;
  f->mode = m;
  f->plus = plus;
  plat_strlcpy(f->path, real, sizeof f->path);
  f->buf = buf;
  f->len = f->cap = len;
  f->pos = m == 'a' ? len : 0;
  h->slot = slot;
  h->gen = f->gen;
  if (n && !slot_filter(L, e, n, f, found, path)) {
    file_release(f);
    h->slot = -1;
    return io_fail(L, path, found ? "the game can't read this track file" : "No such file or directory");
  }
  return 1;
}

static int io_read(lua_State *L) {
  efile_t *f = getfile(L, NULL);
  lua_Integer n = luaL_optinteger(L, 2, 1);
  if (!f) { lua_pushnil(L); return 1; }
  if (n <= 0 || f->pos >= f->len) { lua_pushliteral(L, ""); return 1; }
  size_t k = f->len - f->pos;
  if ((lua_Unsigned)n < k) k = (size_t)n;
  lua_pushlstring(L, f->buf + f->pos, k);
  f->pos += k;
  return 1;
}

static bool file_put(etx_t *e, efile_t *f, const char *s, size_t n) {
  if (f->mode == 'a') f->pos = f->len;
  if (f->pos > ETX_MAX_FILE || n > ETX_MAX_FILE - f->pos) {
    elog(e, "io: %s: file too large, write dropped", f->path);
    return false;
  }
  size_t end = f->pos + n;
  if (end > f->cap) {
    size_t cap = f->cap ? f->cap : 256;
    while (cap < end) cap *= 2;
    char *b = realloc(f->buf, cap);
    if (!b) return false;
    f->buf = b;
    f->cap = cap;
  }
  if (f->pos > f->len) memset(f->buf + f->len, 0, f->pos - f->len);   /* after a seek past the end */
  memcpy(f->buf + f->pos, s, n);
  f->pos = end;
  if (end > f->len) f->len = end;
  f->dirty = true;
  return true;
}

static int io_write(lua_State *L) {
  etx_t *e = E(L);
  int n = lua_gettop(L);
  luaL_checkudata(L, 1, FILE_MT);
  for (int i = 2; i <= n; i++) {
    size_t len;
    const char *s = luaL_tolstring(L, i, &len);
    /* (looked up again: a __tostring may have closed or reopened it) */
    efile_t *f = getfile(L, NULL);
    if (!f || (f->mode == 'r' && !f->plus)) return 0;
    if (!file_put(e, f, s, len)) return 0;
    lua_pop(L, 1);
  }
  return 0;
}

static int io_seek(lua_State *L) {
  efile_t *f = getfile(L, NULL);
  lua_Integer pos = luaL_optinteger(L, 2, 0);
  if (!f) return 0;
  f->pos = pos < 0 ? 0 : (size_t)pos;
  lua_pushinteger(L, 0);
  return 1;
}

static void commit(etx_t *e, efile_t *f) {
  if (e->cfg.async_writes) {
    char *data = f->buf ? f->buf : malloc(1);
    if (data && w_enqueue(f->path, data, f->len, e->log, e->log_ud) == 0) {
      f->buf = NULL;
      f->cap = 0;
      return;
    }
    if (data != f->buf) free(data);
  }
  if (commit_file(f->path, f->buf ? f->buf : "", f->len) != 0) elog(e, "io: can't write %s: %s", f->path, strerror(errno));
}

static int io_close(lua_State *L) {
  ehandle_t *h;
  efile_t *f = getfile(L, &h);
  if (!f) return 0;
  if (f->mode == 'w' || f->dirty) commit(E(L), f);
  file_release(f);
  h->slot = -1;
  lua_pushboolean(L, 1);
  return 1;
}

/* a handle the script dropped without io.close: its writes are discarded, never half-written */
static int io_gc(lua_State *L) {
  ehandle_t *h;
  efile_t *f = getfile(L, &h);
  if (f) file_release(f);
  h->slot = -1;
  return 0;
}

static int io_tostring(lua_State *L) {
  ehandle_t *h;
  efile_t *f = getfile(L, &h);
  if (f) lua_pushfstring(L, "file (%p)", (void *)h);
  else lua_pushliteral(L, "file (closed)");
  return 1;
}

/* loadScript(path [, mode [, env]]): text chunks from the SD folder. As on EdgeTX, a third
 * argument leaves the chunk with no _ENV at all (EdgeTX clears the stack before reading it). */
static int l_loadScript(lua_State *L) {
  etx_t *e = E(L);
  size_t plen;
  const char *path = luaL_checklstring(L, 1, &plen);
  const char *mode = luaL_optstring(L, 2, "bt");
  bool with_env = lua_gettop(L) >= 3;
  if (!strchr(mode, 't')) {
    lua_pushnil(L);
    lua_pushfstring(L, "%s: binary chunks are not supported", path);
    return 2;
  }
  char real[ETX_PATH_MAX + 1], name[256];
  char *buf = NULL;
  size_t len = 0;
  if (strlen(path) != plen || etx_sd_path(e, path, real, sizeof real) != 0 || load_file(real, &buf, &len) != 1) {
    lua_pushnil(L);
    lua_pushfstring(L, "%s: file not found", path);
    return 2;
  }
  const char *base = path;
  for (const char *p = path; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
  snprintf(name, sizeof name, "@%s", base);
  int st = luaL_loadbufferx(L, buf, len, name, "t");
  free(buf);
  if (st != LUA_OK) {
    lua_pushnil(L);
    lua_insert(L, -2);
    return 2;
  }
  if (with_env) {
    lua_pushnil(L);
    if (!lua_setupvalue(L, -2, 1)) lua_pop(L, 1);
  }
  return 1;
}

/* ================================================================ lcd */
#define LCD_DRAW(L) lcd_t *l = E(L)->lcd; if (!l) return 0

static int l_lcd_clear(lua_State *L) {
  uint32_t fl = lua_isnoneornil(L, 1) ? (5u << 16) : flags_arg(L, 1, 0);
  LCD_DRAW(L);
  lcd_clear(l, lcd_color_of(fl));
  return 0;
}

static int l_lcd_RGB(lua_State *L) {
  int r, g, b;
  if (lua_gettop(L) == 1) {
    lua_Integer v = luaL_checkinteger(L, 1);
    r = (int)((v >> 16) & 255); g = (int)((v >> 8) & 255); b = (int)(v & 255);
  } else {
    r = ci(L, 1); g = ci(L, 2); b = ci(L, 3);
  }
  uint32_t c = ((uint32_t)lcd_rgb565(r, g, b) << 16) | LCDF_RGB;
  lua_pushinteger(L, (lua_Integer)(int32_t)c);
  return 1;
}

static int l_lcd_getColor(lua_State *L) { lua_pushinteger(L, luaL_checkinteger(L, 1)); return 1; }

static int l_lcd_drawPoint(lua_State *L) {
  int x = ci(L, 1), y = ci(L, 2);
  uint32_t fl = flags_arg(L, 3, 0);
  LCD_DRAW(L);
  if (x >= 0 && y >= 0 && x < l->w && y < l->h) lcd_point(l, x, y, lcd_color_of(fl));
  return 0;
}

static int l_lcd_drawLine(lua_State *L) {
  int x1 = ci(L, 1), y1 = ci(L, 2), x2 = ci(L, 3), y2 = ci(L, 4), pat = ci(L, 5) & 0xFF;
  uint32_t fl = flags_arg(L, 6, 0);
  LCD_DRAW(L);
  lcd_draw_line_api(l, x1, y1, x2, y2, pat, fl);
  return 0;
}

static int l_lcd_drawLineWithClipping(lua_State *L) {
  int x1 = ci(L, 1), y1 = ci(L, 2), x2 = ci(L, 3), y2 = ci(L, 4);
  for (int i = 5; i <= 8; i++) ci(L, i);   /* xmin xmax ymin ymax: checked, then ignored like engine.js */
  int pat = ci(L, 9) & 0xFF;
  uint32_t fl = flags_arg(L, 10, 0);
  LCD_DRAW(L);
  lcd_line(l, x1, y1, x2, y2, (uint8_t)pat, lcd_color_of(fl));
  return 0;
}

static int l_lcd_drawFilledRectangle(lua_State *L) {
  int x = ci(L, 1), y = ci(L, 2), w = ci(L, 3), h = ci(L, 4);
  uint32_t fl = flags_arg(L, 5, 0);
  int opa = oi(L, 6, 0) & 15;
  LCD_DRAW(L);
  lcd_fill_rect(l, x, y, w, h, lcd_color_of(fl), opa);
  return 0;
}

static int l_lcd_drawRectangle(lua_State *L) {
  int x = ci(L, 1), y = ci(L, 2), w = ci(L, 3), h = ci(L, 4);
  uint32_t fl = flags_arg(L, 5, 0);
  int t = oi(L, 6, 1), opa = oi(L, 7, 0) & 15;
  LCD_DRAW(L);
  lcd_draw_rectangle(l, x, y, w, h, fl, t, opa);
  return 0;
}

static int l_lcd_invertRect(lua_State *L) {
  int x = ci(L, 1), y = ci(L, 2), w = ci(L, 3), h = ci(L, 4);
  LCD_DRAW(L);
  lcd_invert_rect(l, x, y, w, h);
  return 0;
}

static int l_lcd_drawFilledTriangle(lua_State *L) {
  int x1 = ci(L, 1), y1 = ci(L, 2), x2 = ci(L, 3), y2 = ci(L, 4), x3 = ci(L, 5), y3 = ci(L, 6);
  uint32_t fl = flags_arg(L, 7, 0);
  int opa = oi(L, 8, 0) & 15;
  LCD_DRAW(L);
  lcd_tri(l, x1, y1, x2, y2, x3, y3, lcd_color_of(fl), opa);
  return 0;
}

static int l_lcd_drawTriangle(lua_State *L) {
  int x1 = ci(L, 1), y1 = ci(L, 2), x2 = ci(L, 3), y2 = ci(L, 4), x3 = ci(L, 5), y3 = ci(L, 6);
  uint32_t fl = flags_arg(L, 7, 0);
  LCD_DRAW(L);
  uint16_t c = lcd_color_of(fl);
  lcd_line(l, x1, y1, x2, y2, 0xFF, c);
  lcd_line(l, x2, y2, x3, y3, 0xFF, c);
  lcd_line(l, x3, y3, x1, y1, 0xFF, c);
  return 0;
}

static int l_lcd_drawFilledCircle(lua_State *L) {
  int x = ci(L, 1), y = ci(L, 2), r = ci(L, 3);
  uint32_t fl = flags_arg(L, 4, 0);
  LCD_DRAW(L);
  lcd_filled_circle(l, x, y, r, lcd_color_of(fl));
  return 0;
}

static int l_lcd_drawCircle(lua_State *L) {
  int x = ci(L, 1), y = ci(L, 2), r = ci(L, 3);
  uint32_t fl = flags_arg(L, 4, 0);
  LCD_DRAW(L);
  lcd_circle(l, x, y, r, lcd_color_of(fl));
  return 0;
}

static int l_lcd_drawText(lua_State *L) {
  int x = ci(L, 1), y = ci(L, 2);
  size_t len;
  const char *s = luaL_checklstring(L, 3, &len);
  uint32_t fl = flags_arg(L, 4, 0);
  LCD_DRAW(L);
  int w, h;
  lcd_draw_text(l, x, y, s, (int)(len > INT_MAX ? INT_MAX : len), fl, &w, &h);
  return 0;
}

static int l_lcd_sizeText(lua_State *L) {
  size_t len;
  const char *s = luaL_checklstring(L, 1, &len);
  uint32_t fl = flags_arg(L, 2, 0);
  int w = 0, h = 0;
  lcd_t *l = E(L)->lcd;
  if (l) lcd_size_text(l, s, (int)(len > INT_MAX ? INT_MAX : len), fl, &w, &h);
  lua_pushinteger(L, w);
  lua_pushinteger(L, h);
  return 2;
}

static int l_lcd_drawNumber(lua_State *L) {
  int x = ci(L, 1), y = ci(L, 2), v = ci(L, 3);
  uint32_t fl = flags_arg(L, 4, 0);
  LCD_DRAW(L);
  lcd_draw_number(l, x, y, v, fl);
  return 0;
}

static int l_lcd_drawTimer(lua_State *L) {
  int x = ci(L, 1), y = ci(L, 2), v = ci(L, 3);
  uint32_t fl = flags_arg(L, 4, 0);
  LCD_DRAW(L);
  lcd_draw_timer(l, x, y, v, fl);
  return 0;
}

static int l_lcd_drawGauge(lua_State *L) {
  int x = ci(L, 1), y = ci(L, 2), w = ci(L, 3), h = ci(L, 4), n = ci(L, 5), d = ci(L, 6);
  uint32_t fl = flags_arg(L, 7, 0);
  LCD_DRAW(L);
  lcd_draw_gauge(l, x, y, w, h, n, d, fl);
  return 0;
}

static const luaL_Reg LCD_FUNCS[] = {
  { "clear", l_lcd_clear }, { "refresh", l_nop }, { "resetBacklightTimeout", l_nop },
  { "exitFullScreen", l_nop }, { "RGB", l_lcd_RGB }, { "setColor", l_nop }, { "getColor", l_lcd_getColor },
  { "drawPoint", l_lcd_drawPoint }, { "drawLine", l_lcd_drawLine },
  { "drawLineWithClipping", l_lcd_drawLineWithClipping }, { "drawFilledRectangle", l_lcd_drawFilledRectangle },
  { "drawRectangle", l_lcd_drawRectangle }, { "invertRect", l_lcd_invertRect },
  { "drawFilledTriangle", l_lcd_drawFilledTriangle }, { "drawTriangle", l_lcd_drawTriangle },
  { "drawFilledCircle", l_lcd_drawFilledCircle }, { "drawCircle", l_lcd_drawCircle },
  { "drawText", l_lcd_drawText }, { "sizeText", l_lcd_sizeText }, { "drawNumber", l_lcd_drawNumber },
  { "drawTimer", l_lcd_drawTimer }, { "drawGauge", l_lcd_drawGauge }, { "drawHudRectangle", l_nop },
  { "drawBitmap", l_nop }, { "drawSource", l_nop }, { "drawSwitch", l_nop }, { "drawChannel", l_nop },
  { NULL, NULL }
};

/* ================================================================ globals (engine.js values) */
static const struct { const char *name; int v; } CONSTS[] = {
  { "INVERS", 0x01 }, { "VCENTER", 0x02 }, { "CENTER", 0x04 }, { "RIGHT", 0x08 }, { "LEFT", 0x00 },
  { "SHADOWED", 0x80 }, { "BLINK", 0x1000 }, { "TIMEHOUR", 0x2000 }, { "LEADING0", 0x10 },
  { "PREC1", 0x20 }, { "PREC2", 0x30 }, { "SOLID", 0xFF }, { "DOTTED", 0x55 }, { "BOLD", 0x100 },
  { "TINSIZE", 0x200 }, { "SMLSIZE", 0x300 }, { "MIDSIZE", 0x400 }, { "DBLSIZE", 0x500 }, { "XXLSIZE", 0x600 },
  /* events */
  { "EVT_VIRTUAL_PREV", 0x1D00 }, { "EVT_VIRTUAL_NEXT", 0x1E00 }, { "EVT_VIRTUAL_DEC", 0x1D00 },
  { "EVT_VIRTUAL_INC", 0x1E00 }, { "EVT_VIRTUAL_PREV_PAGE", ETX_BREAK(ETX_K_PAGEUP) },
  { "EVT_VIRTUAL_NEXT_PAGE", ETX_BREAK(ETX_K_PAGEDN) }, { "EVT_VIRTUAL_MENU", ETX_BREAK(ETX_K_MODEL) },
  { "EVT_VIRTUAL_MENU_LONG", ETX_LONG(ETX_K_MODEL) }, { "EVT_VIRTUAL_ENTER", ETX_BREAK(ETX_K_ENTER) },
  { "EVT_VIRTUAL_ENTER_LONG", ETX_LONG(ETX_K_ENTER) }, { "EVT_VIRTUAL_EXIT", ETX_BREAK(ETX_K_EXIT) },
  { "EVT_EXIT_BREAK", ETX_BREAK(ETX_K_EXIT) }, { "EVT_ENTER_FIRST", ETX_FIRST(ETX_K_ENTER) },
  { "EVT_ENTER_BREAK", ETX_BREAK(ETX_K_ENTER) }, { "EVT_ENTER_LONG", ETX_LONG(ETX_K_ENTER) },
  { "EVT_ENTER_REPT", ETX_REPT(ETX_K_ENTER) },
  { "EVT_PAGEUP_FIRST", ETX_FIRST(ETX_K_PAGEUP) }, { "EVT_PAGEUP_BREAK", ETX_BREAK(ETX_K_PAGEUP) },
  { "EVT_PAGEUP_LONG", ETX_LONG(ETX_K_PAGEUP) }, { "EVT_PAGEDN_FIRST", ETX_FIRST(ETX_K_PAGEDN) },
  { "EVT_PAGEDN_BREAK", ETX_BREAK(ETX_K_PAGEDN) }, { "EVT_PAGEDN_LONG", ETX_LONG(ETX_K_PAGEDN) },
  { "EVT_MODEL_FIRST", ETX_FIRST(ETX_K_MODEL) }, { "EVT_MODEL_BREAK", ETX_BREAK(ETX_K_MODEL) },
  { "EVT_MODEL_LONG", ETX_LONG(ETX_K_MODEL) }, { "EVT_SYS_FIRST", ETX_FIRST(ETX_K_SYS) },
  { "EVT_SYS_BREAK", ETX_BREAK(ETX_K_SYS) }, { "EVT_SYS_LONG", ETX_LONG(ETX_K_SYS) },
  { "EVT_TELEM_FIRST", ETX_FIRST(ETX_K_TELE) }, { "EVT_TELEM_BREAK", ETX_BREAK(ETX_K_TELE) },
  { "EVT_TELEM_LONG", ETX_LONG(ETX_K_TELE) }, { "EVT_ROT_LEFT", 0x1D00 }, { "EVT_ROT_RIGHT", 0x1E00 },
  { "PLAY_NOW", ETX_PLAY_NOW }, { "PLAY_BACKGROUND", ETX_PLAY_BACKGROUND },
};

static const char *const TOUCH_EVENTS[4] = { "EVT_TOUCH_FIRST", "EVT_TOUCH_BREAK", "EVT_TOUCH_SLIDE", "EVT_TOUCH_TAP" };

/* engine.js NAMED, in order: index << 16 */
static const char *const NAMED[31] = {
  "COLOR_THEME_PRIMARY1", "COLOR_THEME_PRIMARY2", "COLOR_THEME_PRIMARY3", "COLOR_THEME_SECONDARY1",
  "COLOR_THEME_SECONDARY2", "COLOR_THEME_SECONDARY3", "COLOR_THEME_FOCUS", "COLOR_THEME_EDIT",
  "COLOR_THEME_ACTIVE", "COLOR_THEME_WARNING", "COLOR_THEME_DISABLED", "BLACK", "WHITE", "LIGHTWHITE",
  "LIGHTGREY", "GREY", "DARKGREY", "RED", "DARKRED", "LIGHTRED", "GREEN", "DARKGREEN", "BRIGHTGREEN",
  "BLUE", "DARKBLUE", "CYAN", "YELLOW", "LIGHTBROWN", "DARKBROWN", "ORANGE", "MAGENTA",
};

static void set_global_int(lua_State *L, const char *name, lua_Integer v) {
  lua_pushinteger(L, v);
  lua_setglobal(L, name);
}

static void set_global_fn(lua_State *L, const char *name, lua_CFunction f) {
  lua_pushcfunction(L, f);
  lua_setglobal(L, name);
}

/* everything before the chunk loads (run protected) */
static int setup_state(lua_State *L) {
  etx_t *e = E(L);
  /* EdgeTX's set of libraries; no os, io, package or debug (the script gets EdgeTX's io) */
  luaL_requiref(L, "_G", luaopen_base, 1);
  luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
  luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
  luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
  luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
  luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
#if defined(LUA_COMPAT_BITLIB)
  luaL_requiref(L, LUA_BITLIBNAME, luaopen_bit32, 1);
#endif
  lua_settop(L, 0);
  lua_pushnil(L);
  lua_setglobal(L, "dofile");
  lua_pushnil(L);
  lua_setglobal(L, "loadfile");
  lua_getglobal(L, "load");
  lua_pushcclosure(L, l_load_text, 1);
  lua_setglobal(L, "load");
  lua_getglobal(L, "setmetatable");
  lua_pushcclosure(L, l_setmetatable, 1);
  lua_setglobal(L, "setmetatable");
  set_global_fn(L, "xpcall", l_xpcall);

  /* constants */
  set_global_int(L, "LCD_W", e->lcd ? e->lcd->w : 480);
  set_global_int(L, "LCD_H", e->lcd ? e->lcd->h : 272);
  for (size_t i = 0; i < sizeof CONSTS / sizeof CONSTS[0]; i++) set_global_int(L, CONSTS[i].name, CONSTS[i].v);
  for (int i = 0; i < 31; i++) set_global_int(L, NAMED[i], (lua_Integer)i << 16);
  if (!e->cfg.no_touch)
    for (int i = 0; i < 4; i++) set_global_int(L, TOUCH_EVENTS[i], 0x1001 + i);

  /* general functions */
  set_global_fn(L, "getTime", l_getTime);
  set_global_fn(L, "getValue", l_getValue);
  set_global_fn(L, "getFieldInfo", l_getFieldInfo);
  set_global_fn(L, "getStickMode", l_getStickMode);
  set_global_fn(L, "playTone", l_playTone);
  set_global_fn(L, "playHaptic", l_playHaptic);
  set_global_fn(L, "playFile", l_nop);
  set_global_fn(L, "playNumber", l_nop);
  set_global_fn(L, "killEvents", l_nop);
  set_global_fn(L, "getUsage", l_zero);
  set_global_fn(L, "getAvailableMemory", l_getAvailableMemory);
  set_global_fn(L, "getRSSI", l_zero);
  set_global_fn(L, "getVersion", l_getVersion);
  set_global_fn(L, "print", l_print);
  set_global_fn(L, "loadScript", l_loadScript);
  lua_createtable(L, 0, 1);
  lua_pushcfunction(L, l_model_getInfo);
  lua_setfield(L, -2, "getInfo");
  lua_setglobal(L, "model");

  /* io: handles are userdata with no methods (EdgeTX: io.read(f, n), io.write(f, ...)) */
  luaL_newmetatable(L, FILE_MT);
  lua_pushcfunction(L, io_gc);
  lua_setfield(L, -2, "__gc");
  lua_pushcfunction(L, io_tostring);
  lua_setfield(L, -2, "__tostring");
  lua_pushliteral(L, FILE_MT);          /* getmetatable(f): no way to swap in a script __gc */
  lua_setfield(L, -2, "__metatable");
  lua_pop(L, 1);
  static const luaL_Reg io_funcs[] = {
    { "open", io_open }, { "read", io_read }, { "write", io_write }, { "seek", io_seek }, { "close", io_close },
    { NULL, NULL }
  };
  luaL_newlib(L, io_funcs);
  lua_setglobal(L, "io");

  /* the track slot checker, with the string library as it is now */
  if (luaL_loadbufferx(L, SLOT_CHECK, sizeof SLOT_CHECK - 1, "=etx", "t") != LUA_OK) return lua_error(L);
  lua_call(L, 0, 1);
  e->check_ref = luaL_ref(L, LUA_REGISTRYINDEX);

  /* extra globals */
  if (e->cfg.test_hooks) {
    lua_newtable(L);
    lua_setglobal(L, "STICKTIME_TEST");
  }
  if (e->cfg.lat >= 0) {
    push_num(L, e->cfg.lat);
    lua_setglobal(L, "STICKTIME_LAT");
  }

  luaL_newlib(L, LCD_FUNCS);
  lua_setglobal(L, "lcd");
  return 0;
}

/* ================================================================ public */
etx_t *etx_new(lcd_t *lcd, const etx_config_t *cfg) {
  etx_t *e = calloc(1, sizeof *e);
  if (!e) return NULL;
  e->lcd = lcd;
  if (cfg) e->cfg = *cfg;
  plat_strlcpy(e->sd_root, e->cfg.sd_root ? e->cfg.sd_root : "", sizeof e->sd_root);
  plat_strlcpy(e->script, e->cfg.script ? e->cfg.script : "", sizeof e->script);
  e->cfg.sd_root = e->sd_root;
  e->cfg.script = e->script;
  e->sticks[2] = -1024;               /* throttle low until the host says otherwise */
  e->run_ref = e->init_ref = e->check_ref = LUA_NOREF;
  w_init();
  return e;
}

static void close_state(etx_t *e) {
  if (e->L) {
    /* finalizers run here: the watchdog covers them too */
    lua_State *L = e->L;
    script_enter(e);
    lua_close(L);
    e->L = NULL;
    script_leave(e);
  }
  for (int i = 0; i < ETX_MAX_FILES; i++) file_release(&e->files[i]);
  for (int i = 0; i < NSLOTS; i++) { free(e->snap[i]); e->snap[i] = NULL; e->snap_len[i] = 0; }
  e->run_ref = e->init_ref = e->check_ref = LUA_NOREF;
  e->loaded = false;
  e->mem = 0;
  e->depth = 0;
  e->deadline_us = 0;
  e->expired = false;
}

void etx_free(etx_t *e) {
  if (!e) return;
  close_state(e);
  free(e);
}

void etx_set_log(etx_t *e, etx_log_fn fn, void *ud) {
  e->log = fn;
  e->log_ud = ud;
}

int etx_load(etx_t *e) {
  close_state(e);
  e->err[0] = 0;
  e->test_err[0] = 0;
  e->result = 0;
  e->frames = 0;
  e->tone_head = e->tone_n = 0;

  lua_State *L = lua_newstate(l_alloc, e);
  if (!L) return fail(e, "Script error", "not enough memory");
  *(etx_t **)lua_getextraspace(L) = e;
  lua_atpanic(L, l_panic);
  e->L = L;
  lua_sethook(L, hook, LUA_MASKCOUNT, HOOK_COUNT);
  lua_pushcfunction(L, setup_state);
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) return fail_pop(e, "Script error");

  char real[ETX_PATH_MAX + 1], name[256], msg[ETX_PATH_MAX + 64];
  char *buf = NULL;
  size_t len = 0;
  int r = etx_sd_path(e, e->script, real, sizeof real) == 0 ? load_file(real, &buf, &len) : -2;
  if (r != 1) {
    bool missing = r == 0 || r == -2 || errno == EISDIR;   /* (FatFs: a folder is no file either) */
    snprintf(msg, sizeof msg, "%s: %s", e->script, missing ? "file not found" : strerror(errno));
    return fail(e, "Script error", msg);
  }
  const char *base = e->script;
  for (const char *p = e->script; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
  snprintf(name, sizeof name, "@%s", base);
  int st = luaL_loadbufferx(L, buf, len, name, "t");
  free(buf);
  if (st != LUA_OK) return fail_pop(e, "Syntax error");
  if (pcall_wd(e, 0, 1, true) != LUA_OK) return fail_pop(e, "Script error");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return fail(e, "Script error", "script must return a table with init/run");
  }
  /* (raw: a metatable on the returned table can't raise an error outside a pcall) */
  lua_pushliteral(L, "init");
  lua_rawget(L, -2);
  if (lua_isfunction(L, -1)) e->init_ref = luaL_ref(L, LUA_REGISTRYINDEX); else lua_pop(L, 1);
  lua_pushliteral(L, "run");
  lua_rawget(L, -2);
  if (lua_isfunction(L, -1)) e->run_ref = luaL_ref(L, LUA_REGISTRYINDEX); else lua_pop(L, 1);
  lua_pop(L, 1);
  if (e->run_ref == LUA_NOREF) return fail(e, "Script error", "script must return a table with init/run");
  if (e->init_ref != LUA_NOREF) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, e->init_ref);
    if (pcall_wd(e, 0, 0, true) != LUA_OK) return fail_pop(e, "init() error");
  }
  e->loaded = true;
  return 0;
}

int etx_run_touch(etx_t *e, int event, const etx_touch_t *t) {
  if (!e->L || !e->loaded) return -1;
  if (e->result != 0) return e->result;
  lua_State *L = e->L;
  lua_rawgeti(L, LUA_REGISTRYINDEX, e->run_ref);
  lua_pushinteger(L, event);
  int n = 1;
  if (t) {
    lua_createtable(L, 0, 7);
    lua_pushinteger(L, t->x); lua_setfield(L, -2, "x");
    lua_pushinteger(L, t->y); lua_setfield(L, -2, "y");
    lua_pushinteger(L, t->start_x); lua_setfield(L, -2, "startX");
    lua_pushinteger(L, t->start_y); lua_setfield(L, -2, "startY");
    lua_pushinteger(L, t->slide_x); lua_setfield(L, -2, "slideX");
    lua_pushinteger(L, t->slide_y); lua_setfield(L, -2, "slideY");
    lua_pushinteger(L, t->tap_count); lua_setfield(L, -2, "tapCount");
    n = 2;
  }
  if (pcall_wd(e, n, 1, true) != LUA_OK) return fail_pop(e, "Script error");
  int isnum = 0;
  lua_Number r = lua_tonumberx(L, -1, &isnum);
  lua_pop(L, 1);
  e->frames++;
  if (isnum && r != 0) e->result = 1;
  return e->result;
}

int etx_run(etx_t *e, int event) { return etx_run_touch(e, event, NULL); }

const char *etx_error(const etx_t *e) { return e->err; }
bool etx_loaded(const etx_t *e) { return e->L && e->loaded && e->result != -1; }

void etx_set_clock_ms(etx_t *e, int64_t ms) { e->clock_ms = ms; }

void etx_set_sticks(etx_t *e, int ail, int ele, int thr, int rud) {
  e->sticks[0] = ail;
  e->sticks[1] = ele;
  e->sticks[2] = thr;
  e->sticks[3] = rud;
}

/* raw: a script's strict-mode metatable on _G must not raise errors here */
static void set_global(etx_t *e, const char *name, bool num, double v) {
  if (!e->L || !name) return;
  lua_State *L = e->L;
  lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
  lua_pushstring(L, name);
  if (num) push_num(L, v); else lua_pushnil(L);
  lua_rawset(L, -3);
  lua_pop(L, 1);
}

void etx_set_global_number(etx_t *e, const char *name, double v) { set_global(e, name, true, v); }
void etx_clear_global(etx_t *e, const char *name) { set_global(e, name, false, 0); }

/* STICKTIME_TEST.<fn>(args) with its results on the stack above the returned top; -1 if none */
static int test_call(etx_t *e, const char *fn, const double *args, int nargs) {
  e->test_err[0] = 0;
  if (!e->L || !fn) { plat_strlcpy(e->test_err, "no Lua state", sizeof e->test_err); return -1; }
  lua_State *L = e->L;
  int top = lua_gettop(L);
  lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
  lua_pushliteral(L, "STICKTIME_TEST");
  lua_rawget(L, -2);
  if (!lua_istable(L, -1)) {
    lua_settop(L, top);
    plat_strlcpy(e->test_err, "no STICKTIME_TEST", sizeof e->test_err);
    return -1;
  }
  lua_pushstring(L, fn);
  lua_rawget(L, -2);
  if (!lua_isfunction(L, -1)) {
    lua_settop(L, top);
    snprintf(e->test_err, sizeof e->test_err, "no STICKTIME_TEST.%s", fn);
    return -1;
  }
  lua_replace(L, top + 1);
  lua_settop(L, top + 1);
  if (nargs < 0) nargs = 0;
  if (!lua_checkstack(L, nargs + 20)) { lua_settop(L, top); return -1; }
  for (int i = 0; i < nargs; i++) push_num(L, args[i]);
  if (pcall_wd(e, nargs, LUA_MULTRET, false) != LUA_OK) {
    const char *m = lua_tostring(L, -1);
    snprintf(e->test_err, sizeof e->test_err, "%s", m ? m : "(error)");
    lua_settop(L, top);
    return -1;
  }
  return top;
}

int etx_test_call(etx_t *e, const char *fn, const double *args, int nargs, double *out, int nout) {
  int top = test_call(e, fn, args, nargs);
  if (top < 0) return -1;
  lua_State *L = e->L;
  int n = lua_gettop(L) - top, k = 0;
  for (; k < n && k < nout; k++) {
    int i = top + 1 + k;
    int t = lua_type(L, i);
    out[k] = t == LUA_TNUMBER ? (double)lua_tonumber(L, i) : t == LUA_TBOOLEAN ? (lua_toboolean(L, i) ? 1.0 : 0.0) : NAN;
  }
  lua_settop(L, top);
  return k;
}

int etx_test_call_str(etx_t *e, const char *fn, const double *args, int nargs, int idx, char *out, size_t outlen) {
  if (out && outlen) out[0] = 0;
  int top = test_call(e, fn, args, nargs);
  if (top < 0) return -1;
  lua_State *L = e->L;
  int n = lua_gettop(L) - top, at = 0;
  if (idx >= 0) {                  /* the position, counted like etx_test_call's out[] */
    if (idx < n && lua_type(L, top + 1 + idx) == LUA_TSTRING) at = top + 1 + idx;
  } else {
    for (int i = 1; i <= n && !at; i++) if (lua_type(L, top + i) == LUA_TSTRING) at = top + i;
  }
  int r = -1;
  if (at) {
    if (out && outlen) {
      size_t len;
      const char *s = lua_tolstring(L, at, &len);
      if (len > outlen - 1) len = outlen - 1;
      memcpy(out, s, len);
      out[len] = 0;
    }
    r = 0;
  } else {
    snprintf(e->test_err, sizeof e->test_err, "STICKTIME_TEST.%s: no string result", fn);
  }
  lua_settop(L, top);
  return r;
}

const char *etx_test_error(const etx_t *e) { return e->test_err; }

int etx_game_state(etx_t *e) {
  double v[16];
  if (etx_test_call(e, "get", NULL, 0, v, 16) < 16 || v[15] != v[15]) return GS_UNKNOWN;
  int s = (int)v[15];
  return s >= GS_MENU && s <= GS_DONE && (double)s == v[15] ? s : GS_UNKNOWN;
}

static int gc_step(lua_State *L) {
  lua_gc(L, LUA_GCSTEP, (int)lua_tointeger(L, 1));
  return 0;
}

/* protected: a script's __gc metamethods run here */
void etx_gc_step(etx_t *e, int kb) {
  if (!e->L) return;
  lua_pushcfunction(e->L, gc_step);
  lua_pushinteger(e->L, kb);
  if (pcall_wd(e, 1, 0, false) != LUA_OK) {
    elog(e, "gc: %s", lua_tostring(e->L, -1) ? lua_tostring(e->L, -1) : "error");
    lua_pop(e->L, 1);
  }
}

void etx_stats(etx_t *e, etx_stats_t *st) {
  memset(st, 0, sizeof *st);
  st->instr_count = e->instr;
  st->frames = e->frames;
  if (e->L) st->lua_kb = (size_t)lua_gc(e->L, LUA_GCCOUNT, 0);
}

int etx_take_tones(etx_t *e, etx_tone_t *out, int max) {
  int n = 0;
  while (n < max && e->tone_n > 0) {
    out[n++] = e->tones[e->tone_head];
    e->tone_head = (e->tone_head + 1) % ETX_TONE_QUEUE;
    e->tone_n--;
  }
  return n;
}

void etx_flush_writes(etx_t *e) {
  (void)e;
  if (!W.init) return;
  plat_mutex_lock(&W.mu);
  while (W.head && !W.hold) plat_cond_wait(&W.idle, &W.mu, 1000);
  plat_mutex_unlock(&W.mu);
}
