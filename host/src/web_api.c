/* The routes of docs/WEB_API.md: pages, status, settings, the game's SD card (/api/sd with the
 * sd_write policy), songs, the remote, WiFi, updates and the system. Everything runs on the
 * connection's worker thread; the frame loop only sees app_post()/app_post_key(), the status
 * snapshot, the screenshot handoff and web_take_sd_dirty(). Helpers (sticktime-wifi,
 * sticktime-update) run as processes from here, never from the frame loop. */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "web_internal.h"
#include "etx.h"
#include "settings.h"
#include "sys.h"
#include "video.h"
#include "vtx.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/utsname.h>
#include <unistd.h>
#endif

#define SLOT_MAX (64 * 1024)               /* a track slot body */
#define SD_PUT_MAX (2 * 1024 * 1024)       /* any other /api/sd body */
#define SD_READ_MAX (16 * 1024 * 1024)
#define SETTINGS_BODY_MAX (64 * 1024)
#define SMALL_BODY_MAX 4096                /* /api/key, /api/update/apply, ... */
#define SONG_NAME_MAX 120
#define SONG_RESERVE (8LL << 20)           /* free space left for the game's saves and ghosts */
#define SCAN_FRESH_MS 10000
#define SCAN_WAIT_MS 4000

static app_t *A;
static bool inited;
static plat_mutex set_mu;                  /* settings: load, change, save */
static plat_mutex shot_mu;                 /* one screenshot at a time */
static plat_mutex scan_mu;
static plat_cond scan_cv;
static bool scan_running;
static char scan_err[256];

/* ================================================================ small helpers */
static int join(char *out, size_t n, const char *a, const char *b) {
  int k = snprintf(out, n, "%s%s%s", a, (*a && *b) ? "/" : "", b);
  if (k < 0 || (size_t)k >= n) { errno = ENAMETOOLONG; return -1; }
  return 0;
}

static void trim(char **s) {
  char *p = *s;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  size_t n = strlen(p);
  while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r' || p[n - 1] == '\n')) p[--n] = 0;
  *s = p;
}

static bool truthy(const char *v) {
  return !web_casecmp(v, "1") || !web_casecmp(v, "on") || !web_casecmp(v, "yes") || !web_casecmp(v, "true");
}

/* query "a&b=c": is key there (value into val) */
static bool query_get(const char *q, const char *key, char *val, size_t vl) {
  size_t kl = strlen(key);
  for (const char *p = q; p && *p;) {
    const char *amp = strchr(p, '&');
    size_t pl = amp ? (size_t)(amp - p) : strlen(p);
    if (pl >= kl && !strncmp(p, key, kl) && (pl == kl || p[kl] == '=')) {
      if (val && vl) {
        size_t n = pl > kl ? pl - kl - 1 : 0;
        if (n >= vl) n = vl - 1;
        memcpy(val, p + kl + 1 - (pl == kl), n);
        val[n] = 0;
        if (web_url_decode(val) != 0) val[0] = 0;
      }
      return true;
    }
    p = amp ? amp + 1 : NULL;
  }
  return false;
}

static const char *state_name(int s) {
  static const char *const n[] = { "unknown", "menu", "setup", "count", "fly", "crashed", "ready", "paused", "done" };
  return s >= 0 && s <= 8 ? n[s] : "unknown";
}

static const char *kind_name(int k) {
  switch (k) {
    case INSRC_KEYBOARD: return "keyboard";
    case INSRC_WIFI: return "wifi";
    case INSRC_USB: return "usb";
    case INSRC_BLUETOOTH: return "bluetooth";
    case INSRC_GAMEPAD: return "gamepad";
    case INSRC_JOYSTICK: return "joystick";
    default: return "none";
  }
}

/* a small file into a malloc'd, NUL-terminated buffer (NULL when missing or too big) */
static char *read_small(const char *path, size_t max, size_t *len) {
  int fd = web_fs_open_read(path);
  if (fd < 0) return NULL;
  char *b = malloc(max + 1);
  size_t n = 0;
  while (b && n < max) {
    long r = web_fs_read(fd, b + n, max - n);
    if (r <= 0) break;
    n += (size_t)r;
  }
  if (b && n == max) {
    char extra;
    if (web_fs_read(fd, &extra, 1) > 0) { free(b); b = NULL; }
  }
  web_fs_close(fd);
  if (!b) return NULL;
  b[n] = 0;
  if (len) *len = n;
  return b;
}

static void send_405(web_conn_t *c, const char *allow) {
  char x[128];
  snprintf(x, sizeof x, "Allow: %s\r\n", allow);
  web_send_error_hdr(c, 405, "MethodNotAllowed", "That method isn't allowed here.", x);
}

/* the body as one word ("enter", "game", "elrs"; "key = enter" works too), lower-cased; NULL if none */
static char *body_word(web_conn_t *c, char *buf, size_t bl) {
  char *body;
  size_t len;
  int r = web_read_body_all(c, SMALL_BODY_MAX, &body, &len);
  if (r == -2) { web_send_error(c, 413, "PayloadTooLarge", "The body is too big."); return NULL; }
  if (r != 0) return NULL;
  char *p = body, *eq = strchr(p, '=');
  if (eq) p = eq + 1;
  trim(&p);
  size_t n = 0;
  for (; p[n] && n < bl - 1; n++) buf[n] = (char)web_lc((unsigned char)p[n]);
  buf[n] = 0;
  bool ok = !p[n] && n > 0;
  for (size_t i = 0; ok && i < n; i++) ok = (buf[i] >= 'a' && buf[i] <= 'z') || buf[i] == '-' || buf[i] == '_';
  free(body);
  if (!ok) buf[0] = 0;
  return buf;
}

/* ================================================================ helper programs */
#ifdef _WIN32
static void quote_arg(char *out, size_t n, const char *s) {
  if (!strpbrk(s, " \t\"")) { plat_strlcpy(out, s, n); return; }
  snprintf(out, n, "\"%s\"", s);
}
#else
static void quote_arg(char *out, size_t n, const char *s) {
  size_t w = 0;
  if (n < 3) { if (n) out[0] = 0; return; }
  out[w++] = '\'';
  for (; *s && w + 5 < n; s++) {
    if (*s == '\'') { memcpy(out + w, "'\\''", 4); w += 4; }
    else out[w++] = *s;
  }
  out[w++] = '\'';
  out[w] = 0;
}
#endif

/* "<libexec>/<name> args" (Linux), or "python <libexec>/<name>.py args" on other systems (tests).
 * args are fixed words from this file, never request data. false when the helper isn't there. */
static bool helper_cmd(const char *name, const char *args, char *out, size_t n) {
  char path[700], q[800];
  web_stat_t st;
#ifdef _WIN32
  if (snprintf(path, sizeof path, "%s/%s.py", A->paths.libexec, name) >= (int)sizeof path) return false;
  if (web_fs_stat(path, &st) != 0 || !st.exists || st.is_dir) return false;
  const char *py = getenv("STICKTIME_PYTHON");
  char pq[600];
  quote_arg(pq, sizeof pq, py && *py ? py : "python");
  quote_arg(q, sizeof q, path);
  snprintf(out, n, "%s %s%s%s", pq, q, args && *args ? " " : "", args ? args : "");
#else
  if (snprintf(path, sizeof path, "%s/%s", A->paths.libexec, name) >= (int)sizeof path) return false;
  if (web_fs_stat(path, &st) != 0 || !st.exists || st.is_dir) return false;
  quote_arg(q, sizeof q, path);
  snprintf(out, n, "%s%s%s", q, args && *args ? " " : "", args ? args : "");
#endif
  return true;
}

static int run_capture(const char *cmd, char *out, size_t max, int timeout_ms) {
#ifdef _WIN32
  /* _popen runs "cmd /c <line>", which strips one pair of outer quotes */
  char *w = malloc(strlen(cmd) + 3);
  if (!w) return -1;
  sprintf(w, "\"%s\"", cmd);
  int r = plat_run_capture(w, out, max, timeout_ms);
  free(w);
  return r;
#else
  return plat_run_capture(cmd, out, max, timeout_ms);
#endif
}

void web_sync_system(app_t *app) {
  char upd[700], q[5][800], pq[600], cmd[4400];
  web_stat_t st;
  if (snprintf(upd, sizeof upd, "%s/lib/sticktime-update", app->paths.base) >= (int)sizeof upd) return;
  if (web_fs_stat(upd, &st) != 0 || !st.exists || st.is_dir) return;
  const char *py = getenv("STICKTIME_PYTHON");
#ifdef _WIN32
  if (!py || !*py) py = "python";
#else
  if (!py || !*py) py = "/usr/bin/python3";
#endif
  quote_arg(pq, sizeof pq, py);
  quote_arg(q[0], sizeof q[0], upd);
  quote_arg(q[1], sizeof q[1], app->paths.base);
  quote_arg(q[2], sizeof q[2], app->paths.settings);
  quote_arg(q[3], sizeof q[3], app->paths.state);
  quote_arg(q[4], sizeof q[4], app->paths.libexec);
  /* -B: no bytecode written into lib/ on the FAT partition */
  snprintf(cmd, sizeof cmd, "%s -I -B %s --base %s --settings %s --state %s sync --to %s", pq, q[0], q[1], q[2],
           q[3], q[4]);
  if (plat_spawn(cmd) != 0) app_log(app, "can't start the system scripts' sync");
}

/* start a helper in the background; false when it isn't installed or couldn't start */
static bool spawn_helper(const char *name, const char *args) {
  char cmd[1600];
  if (!helper_cmd(name, args, cmd, sizeof cmd)) {
    app_log(A, "web: helper %s is not installed", name);
    return false;
  }
  if (plat_spawn(cmd) != 0) {
    app_log(A, "web: can't start %s", name);
    return false;
  }
  app_log(A, "web: %s %s", name, args);
  return true;
}

/* things to do a moment after the answer has gone out */
enum { LATER_QUIT, LATER_REBOOT, LATER_POWEROFF };

static void power_cmd(const char *what) {
  const char *fake = getenv("STICKTIME_POWER_CMD");     /* tests: a script instead of the real thing */
  char cmd[700];
  if (fake && *fake) {
    snprintf(cmd, sizeof cmd, "%s %s", fake, what);
  } else {
#ifdef __linux__
    plat_strlcpy(cmd, !strcmp(what, "reboot") ? "systemctl reboot" : "poweroff", sizeof cmd);
#else
    app_log(A, "web: %s is only done on the Pi", what);
    return;
#endif
  }
  app_log(A, "web: %s", what);
  if (plat_spawn(cmd) != 0) app_log(A, "web: can't run %s", cmd);
}

static void do_later(int what) {
  if (what == LATER_QUIT) app_post(A, APP_CMD_QUIT);
  else power_cmd(what == LATER_REBOOT ? "reboot" : "poweroff");
}

static void *later_main(void *arg) {
  int what = *(int *)arg;
  free(arg);
  plat_thread_lower_priority();
  plat_sleep_us(400000);
  do_later(what);
  return NULL;
}

/* the answer is out already: without a thread do it now rather than never */
static void later(int what) {
  int *l = malloc(sizeof *l);
  if (l) {
    *l = what;
    if (plat_thread_start(NULL, later_main, l, true) == 0) return;
    free(l);
  }
  do_later(what);
}

/* ================================================================ settings */
/* key/value pairs of a body in settings format or application/x-www-form-urlencoded */
typedef struct { char *key, *val; } kv_t;
typedef struct { kv_t *v; int n, cap; bool bad; } kvs_t;

static void kvs_add(kvs_t *k, const char *key, const char *val) {
  for (int i = 0; i < k->n; i++)
    if (!strcmp(k->v[i].key, key)) {             /* a later line wins */
      char *nv = strdup(val);
      if (!nv) { k->bad = true; return; }
      free(k->v[i].val);
      k->v[i].val = nv;
      return;
    }
  if (k->n == k->cap) {
    int nc = k->cap ? k->cap * 2 : 16;
    kv_t *nv = realloc(k->v, sizeof *nv * (size_t)nc);
    if (!nv) { k->bad = true; return; }
    k->v = nv;
    k->cap = nc;
  }
  k->v[k->n].key = strdup(key);
  k->v[k->n].val = strdup(val);
  if (!k->v[k->n].key || !k->v[k->n].val) { free(k->v[k->n].key); free(k->v[k->n].val); k->bad = true; return; }
  k->n++;
}

static void kvs_free(kvs_t *k) {
  for (int i = 0; i < k->n; i++) { free(k->v[i].key); free(k->v[i].val); }
  free(k->v);
  memset(k, 0, sizeof *k);
}

static void kvs_each_cb(void *ud, const char *key, const char *value) { kvs_add(ud, key, value); }

/* false: a malformed form body */
static bool parse_kvs(const char *ctype, char *body, size_t len, kvs_t *out) {
  memset(out, 0, sizeof *out);
  if (!web_ncasecmp(ctype, "application/x-www-form-urlencoded", 33)) {
    for (char *p = body; p && *p;) {
      char *amp = strchr(p, '&');
      if (amp) *amp = 0;
      if (*p) {
        char *eq = strchr(p, '=');
        char *k = p, *v = eq ? eq + 1 : (char *)"";
        if (eq) *eq = 0;
        for (char *x = k; *x; x++) if (*x == '+') *x = ' ';
        for (char *x = v; *x; x++) if (*x == '+') *x = ' ';
        if (web_url_decode(k) != 0 || web_url_decode(v) != 0) return false;
        trim(&k);
        for (char *x = k; *x; x++) *x = (char)web_lc((unsigned char)*x);
        if (*k) kvs_add(out, k, v);
      }
      p = amp ? amp + 1 : NULL;
    }
    return !out->bad;
  }
  settings_t *s = settings_parse(body, len);
  if (!s) return false;
  settings_each(s, kvs_each_cb, out);
  settings_free(s);
  return !out->bad;
}

static bool key_name_ok(const char *k) {
  size_t n = strlen(k);
  if (!n || n > 64) return false;
  for (size_t i = 0; i < n; i++)     /* settings.c's rule: [a-z0-9_][a-z0-9_.-]* */
    if (!((k[i] >= 'a' && k[i] <= 'z') || (k[i] >= '0' && k[i] <= '9') || k[i] == '_' || (i && (k[i] == '-' || k[i] == '.'))))
      return false;
  return true;
}

static bool value_ok(const char *v) {
  if (strlen(v) > 1024) return false;
  for (; *v; v++) if ((unsigned char)*v < 0x20 || *v == 0x7f) return false;
  return true;
}

static bool in_list(const char *k, const char *const *list) {
  for (; *list; list++) if (!strcmp(k, *list)) return true;
  return false;
}

/* not live, but the host reads them at start: the service restarts */
static const char *const RESTART_KEYS[] = { "aspect", "av_mode", "hdmi_mode", "margin", "fonts", "audio_out", NULL };
/* applied by sticktime-setup at boot */
static const char *const BOOT_KEYS[] = { "wifi_ssid", "wifi_password", "wifi_country", "hotspot", "hotspot_ssid",
                                         "hotspot_password", "bluetooth", "hostname", "ssh", "password", "keep_logs",
                                         NULL };

static const char *kind_str(settings_kind_t k) {
  switch (k) {
    case SK_ENUM: return "enum";
    case SK_INT: return "int";
    case SK_NUM: return "num";
    case SK_BOOL: return "bool";
    case SK_SECRET: return "secret";
    default: return "string";
  }
}

static void r_settings_get(web_conn_t *c) {
  bool exists = false;
  settings_t *s = settings_load(A->paths.settings, &exists);
  if (!s) { web_send_error(c, 500, "Internal", "Can't read the settings."); return; }
  sb_t b;
  sb_init(&b, 16384);
  sb_puts(&b, "{\"values\":{");
  for (int i = 0; i < SETTINGS_NKEYS; i++) {
    const settings_key_t *k = &SETTINGS_KEYS[i];
    if (i) sb_puts(&b, ",");
    sb_json_str(&b, k->key);
    sb_puts(&b, ":");
    if (k->kind == SK_SECRET) {
      const char *raw = settings_raw(s, k->key);
      sb_puts(&b, "\"\",");
      char sk[96];
      snprintf(sk, sizeof sk, "%s_set", k->key);
      sb_json_str(&b, sk);
      sb_puts(&b, raw && *raw ? ":true" : ":false");
    } else {
      sb_json_str(&b, settings_get(s, k->key));
    }
  }
  sb_puts(&b, "},\"keys\":[");
  for (int i = 0; i < SETTINGS_NKEYS; i++) {
    const settings_key_t *k = &SETTINGS_KEYS[i];
    if (i) sb_puts(&b, ",");
    sb_puts(&b, "{\"key\":");
    sb_json_str(&b, k->key);
    sb_printf(&b, ",\"kind\":\"%s\",\"default\":", kind_str(k->kind));
    sb_json_str(&b, k->kind == SK_SECRET ? "" : k->def);
    sb_puts(&b, ",\"choices\":[");
    if (k->kind == SK_ENUM && k->choices) {
      const char *p = k->choices;
      bool first = true;
      while (*p) {
        const char *bar = strchr(p, '|');
        size_t n = bar ? (size_t)(bar - p) : strlen(p);
        char ch[64];
        if (n >= sizeof ch) n = sizeof ch - 1;
        memcpy(ch, p, n);
        ch[n] = 0;
        if (!first) sb_puts(&b, ",");
        sb_json_str(&b, ch);
        first = false;
        p += n + (bar ? 1 : 0);
        if (!bar) break;
      }
    }
    sb_puts(&b, "]");
    if ((k->kind == SK_INT || k->kind == SK_NUM) && k->choices && strchr(k->choices, '|')) {
      double lo = atof(k->choices), hi = atof(strchr(k->choices, '|') + 1);
      sb_printf(&b, ",\"min\":%g,\"max\":%g", lo, hi);
    }
    sb_printf(&b, ",\"live\":%s,\"reboot\":%s,\"restart\":%s,\"help\":", k->live ? "true" : "false",
              (k->reboot || in_list(k->key, BOOT_KEYS)) ? "true" : "false", in_list(k->key, RESTART_KEYS) ? "true" : "false");
    sb_json_str(&b, k->help);
    sb_puts(&b, "}");
  }
  sb_puts(&b, "],\"warnings\":");
  sb_json_str(&b, settings_warnings(s));
  sb_printf(&b, ",\"exists\":%s}", exists ? "true" : "false");
  settings_free(s);
  web_send_sb(c, 200, &b);
  sb_free(&b);
}

typedef struct { const char *key, *val; } change_t;     /* val NULL: remove the key's lines */

/* Apply changes to sticktime.txt under set_mu. Fills saved (JSON array items) and the flags.
 * 0 ok, -1 can't load/save (errno). */
static int apply_changes(const change_t *ch, int n, sb_t *saved, bool *live, bool *reboot, bool *restart) {
  bool exists;
  *live = *reboot = *restart = false;
  settings_t *s = settings_load(A->paths.settings, &exists);
  if (!s) { errno = ENOMEM; return -1; }
  char uart_before[16];
  vtx_uart_overlay(s, uart_before, sizeof uart_before);
  int nsaved = 0;
  for (int i = 0; i < n; i++) {
    const char *oraw = settings_raw(s, ch[i].key);
    char *old_raw = oraw ? strdup(oraw) : NULL, *old_eff = strdup(settings_get(s, ch[i].key));
    if (!settings_set(s, ch[i].key, ch[i].val)) { free(old_raw); free(old_eff); continue; }
    const char *nraw = settings_raw(s, ch[i].key), *neff = settings_get(s, ch[i].key);
    bool raw_changed = (!old_raw != !nraw) || (old_raw && strcmp(old_raw, nraw));
    bool eff_changed = !old_eff || strcmp(old_eff, neff);
    if (raw_changed) {
      if (nsaved++) sb_puts(saved, ",");
      sb_json_str(saved, ch[i].key);
    }
    if (eff_changed) {
      const settings_key_t *k = settings_key(ch[i].key);
      if (k && k->live) *live = true;
      if (k && (k->reboot || in_list(k->key, BOOT_KEYS))) *reboot = true;
      if (k && in_list(k->key, RESTART_KEYS)) *restart = true;
      if (!strcmp(ch[i].key, "hostname") && *neff) web_add_hostname(neff);
    }
    free(old_raw);
    free(old_eff);
  }
  /* VTX control on a UART (two-pin mode): its overlay goes into config.txt at the next boot (stboot.py) */
  char uart_after[16];
  vtx_uart_overlay(s, uart_after, sizeof uart_after);
  if (uart_after[0] && strcmp(uart_after, uart_before)) *reboot = true;
  int r = 0;
  if (nsaved) r = settings_save(s, A->paths.settings);
  int e = errno;
  settings_free(s);
  errno = e;
  return r;
}

static void r_settings_post(web_conn_t *c) {
  char *body;
  size_t len;
  int r = web_read_body_all(c, SETTINGS_BODY_MAX, &body, &len);
  if (r == -2) { web_send_error(c, 413, "PayloadTooLarge", "The settings are too big (64 KB at most)."); return; }
  if (r != 0) return;
  kvs_t kv;
  if (!parse_kvs(c->ctype, body, len, &kv)) {
    free(body);
    kvs_free(&kv);
    web_send_error(c, 400, "BadRequest", "The body isn't in settings format (key = value lines) or form encoding.");
    return;
  }
  free(body);
  change_t *ch = calloc((size_t)kv.n + 1, sizeof *ch);
  sb_t fields;
  sb_init(&fields, 256);
  int nch = 0, nbad = 0;
  for (int i = 0; ch && i < kv.n; i++) {
    const char *key = kv.v[i].key, *val = kv.v[i].val;
    size_t kl = strlen(key);
    char base[80] = "";
    if (kl > 6 && kl - 6 < sizeof base && !strcmp(key + kl - 6, "_clear")) { memcpy(base, key, kl - 6); base[kl - 6] = 0; }
    else if (kl > 4 && kl - 4 < sizeof base && !strcmp(key + kl - 4, "_set")) { memcpy(base, key, kl - 4); base[kl - 4] = 0; }
    const settings_key_t *bk = base[0] ? settings_key(base) : NULL;
    if (bk && bk->kind == SK_SECRET) {
      /* "<secret>_clear = 1" empties it; "<secret>_set" (echoed from GET) is ignored */
      if (!strcmp(key + kl - 6, "_clear") && truthy(val)) {
        int j;
        for (j = 0; j < nch && strcmp(ch[j].key, bk->key); j++) {}
        ch[j].key = bk->key;
        ch[j].val = "";
        if (j == nch) nch++;
      }
      continue;
    }
    const char *msg = NULL;
    if (!key_name_ok(key)) msg = "not a valid setting name";
    else if (!value_ok(val)) msg = "must be one line of at most 1024 characters";
    else {
      const settings_key_t *k = settings_key(key);
      if (k && k->kind == SK_SECRET && !*val) continue;    /* an empty secret means "unchanged" */
      if (*val) msg = settings_validate(key, val);
    }
    if (msg) {
      if (nbad++) sb_puts(&fields, ",");
      sb_json_str(&fields, key);
      sb_puts(&fields, ":");
      sb_json_str(&fields, msg);
      continue;
    }
    int j;
    for (j = 0; j < nch && strcmp(ch[j].key, key); j++) {}
    if (j < nch && !*ch[j].val && settings_key(key) && settings_key(key)->kind == SK_SECRET) continue;  /* _clear wins */
    ch[j].key = key;
    ch[j].val = val;
    if (j == nch) nch++;
  }
  if (!ch) {
    web_send_error(c, 500, "Internal", "out of memory");
  } else if (nbad) {
    sb_t b;
    sb_init(&b, 512);
    sb_puts(&b, "{\"error\":\"BadRequest\",\"message\":\"Some values are not valid: nothing was saved.\",\"fields\":{");
    sb_add(&b, fields.p, fields.len);
    sb_puts(&b, "}}");
    web_send_sb(c, 400, &b);
    sb_free(&b);
  } else {
    sb_t saved;
    sb_init(&saved, 256);
    bool live, reboot, restart;
    plat_mutex_lock(&set_mu);
    int rr = apply_changes(ch, nch, &saved, &live, &reboot, &restart);
    int e = errno;
    plat_mutex_unlock(&set_mu);
    if (rr != 0) {
      char m[200];
      snprintf(m, sizeof m, "Couldn't save the settings: %s", strerror(e));
      web_send_error(c, e == EROFS || e == EACCES ? 403 : 500, e == EROFS || e == EACCES ? "Forbidden" : "Internal", m);
    } else {
      sb_t b;
      sb_init(&b, 256);
      sb_puts(&b, "{\"ok\":true,\"saved\":[");
      sb_add(&b, saved.p, saved.len);
      sb_printf(&b, "],\"reboot\":%s,\"restart\":%s}", reboot ? "true" : "false", restart ? "true" : "false");
      web_send_sb(c, 200, &b);
      sb_free(&b);
      if (live) app_post(A, APP_CMD_RELOAD_SETTINGS);
      if (restart) {
        app_log(A, "web: settings need a restart");
        later(LATER_QUIT);
      }
    }
    sb_free(&saved);
  }
  free(ch);
  sb_free(&fields);
  kvs_free(&kv);
}

static void r_settings(web_conn_t *c) {
  if (!strcmp(c->method, "POST")) r_settings_post(c);
  else r_settings_get(c);
}

/* a few keys from a handler (WiFi, module AP), under set_mu */
static int save_keys(const change_t *ch, int n) {
  sb_t saved;
  sb_init(&saved, 64);
  bool live, reboot, restart;
  plat_mutex_lock(&set_mu);
  int r = apply_changes(ch, n, &saved, &live, &reboot, &restart);
  plat_mutex_unlock(&set_mu);
  sb_free(&saved);
  if (r == 0 && live) app_post(A, APP_CMD_RELOAD_SETTINGS);
  return r;
}

/* the same for other route files (web_internal.h) */
int web_save_settings(const char *const *keys, const char *const *vals, int n, sb_t *saved) {
  change_t ch[16];
  if (n < 0 || n > 16) { errno = EINVAL; return -1; }
  for (int i = 0; i < n; i++) { ch[i].key = keys[i]; ch[i].val = vals[i]; }
  bool live, reboot, restart;
  plat_mutex_lock(&set_mu);
  int r = apply_changes(ch, n, saved, &live, &reboot, &restart);
  int e = errno;
  plat_mutex_unlock(&set_mu);
  if (r == 0 && live) app_post(A, APP_CMD_RELOAD_SETTINGS);
  errno = e;
  return r;
}

static settings_t *load_settings(void) {
  bool exists;
  return settings_load(A->paths.settings, &exists);
}

/* ================================================================ status, sticks, log, screenshot */
static void sticks_json(sb_t *b, const app_status_t *st) {
  sb_puts(b, "{\"sources\":[");
  int n = st->nsrc < 0 ? 0 : st->nsrc > INPUT_MAX_SOURCES ? INPUT_MAX_SOURCES : st->nsrc;
  for (int i = 0; i < n; i++) {
    const app_source_status_t *s = &st->src[i];
    char name[sizeof s->name + 1];
    memcpy(name, s->name, sizeof s->name);
    name[sizeof s->name] = 0;
    sb_printf(b, "%s{\"kind\":\"%s\",\"name\":", i ? "," : "", kind_name(s->kind));
    sb_json_str(b, name);
    sb_printf(b, ",\"live\":%s,\"rate_hz\":", s->live ? "true" : "false");
    sb_json_num(b, s->rate_hz, 1);
    sb_printf(b, ",\"nch\":%d,\"ch\":[", s->nch);
    for (int k = 0; k < INPUT_CH; k++) sb_printf(b, "%s%d", k ? "," : "", s->ch[k]);
    sb_puts(b, "]}");
  }
  const sticks_t *k = &st->sticks;
  sb_printf(b, "],\"active\":%d,\"kind\":\"%s\",\"live\":%s,\"sticks\":{\"ail\":%d,\"ele\":%d,\"thr\":%d,\"rud\":%d,\"pause\":",
            k->src, kind_name(k->kind), k->live ? "true" : "false", k->ail, k->ele, k->thr, k->rud);
  if (k->pause == INT32_MIN) sb_puts(b, "null}}");
  else sb_printf(b, "%d}}", k->pause);
}

static void copy_str(char *dst, size_t n, const char *src, size_t srcmax) {
  size_t k = 0;
  while (k < srcmax && src[k] && k < n - 1) { dst[k] = src[k]; k++; }
  dst[k] = 0;
}

/* "client" + SSID, "hotspot" + its SSID, "ethernet" or "": <state>/network.json when something
 * writes it, else the hotspot daemon's state (the NetworkManager connection it sees), else a guess
 * from the interface that has the address */
static void network_mode(const char *iface, char *ssid, size_t sl, char *mode, size_t ml) {
  char path[700], conn[128] = "";
  size_t n;
  char *j = join(path, sizeof path, A->paths.state, "network.json") == 0 ? read_small(path, 8192, &n) : NULL;
  bool done = false;
  if (j && web_json_valid(j, n)) {
    done = web_json_get_str(j, n, "mode", mode, ml);
    web_json_get_str(j, n, "ssid", ssid, sl);
  }
  free(j);
  if (done) return;
  j = join(path, sizeof path, A->paths.state, "hotspot.json") == 0 ? read_small(path, 8192, &n) : NULL;
  if (j && web_json_valid(j, n)) web_json_get_str(j, n, "connection", conn, sizeof conn);
  free(j);
  const char *key = NULL;
  if (!strcmp(conn, "sticktime-hotspot")) { plat_strlcpy(mode, "hotspot", ml); key = "hotspot_ssid"; }
  else if (!strcmp(conn, "sticktime-wifi")) { plat_strlcpy(mode, "client", ml); key = "wifi_ssid"; }
  else if (conn[0]) plat_strlcpy(mode, "client", ml);
  else if (!strncmp(iface, "wl", 2)) { plat_strlcpy(mode, "client", ml); key = "wifi_ssid"; }
  else if (iface[0] == 'e') plat_strlcpy(mode, "ethernet", ml);
  if (key) {
    settings_t *s = load_settings();
    if (s) plat_strlcpy(ssid, settings_get(s, key), sl);
    settings_free(s);
  }
}

static void r_status(web_conn_t *c) {
  app_status_t *st = malloc(sizeof *st);
  if (!st) { web_send_error(c, 500, "Internal", "out of memory"); return; }
  app_snapshot(A, st);
  /* the frame loop's strings may lack a NUL at the end: copy them bounded */
  char err[sizeof st->game_error + 1], gv[sizeof st->game_version + 1], vd[sizeof st->video_desc + 1];
  char wm[sizeof st->wifi_module + 1], ws[sizeof st->wifi_state + 1], ad[sizeof st->audio_device + 1];
  char np[sizeof st->now_playing + 1], sc[sizeof st->script + 1];
  copy_str(err, sizeof err, st->game_error, sizeof st->game_error);
  copy_str(gv, sizeof gv, st->game_version, sizeof st->game_version);
  copy_str(vd, sizeof vd, st->video_desc, sizeof st->video_desc);
  copy_str(wm, sizeof wm, st->wifi_module, sizeof st->wifi_module);
  copy_str(ws, sizeof ws, st->wifi_state, sizeof st->wifi_state);
  copy_str(ad, sizeof ad, st->audio_device, sizeof st->audio_device);
  copy_str(np, sizeof np, st->now_playing, sizeof st->now_playing);
  copy_str(sc, sizeof sc, st->script, sizeof st->script);

  char ip[64] = "", iface[32] = "", host[64], ssid[128] = "", mode[32] = "";
  sys_ip_address(ip, sizeof ip, iface, sizeof iface);
  web_hostname(host, sizeof host);
  network_mode(iface, ssid, sizeof ssid, mode, sizeof mode);
  char throttled[24] = "";
#ifdef __linux__
  {
    size_t tl;
    char *t = read_small("/sys/devices/platform/soc/soc:firmware/get_throttled", 64, &tl);
    if (t) {
      unsigned long v = strtoul(t, NULL, 16);
      snprintf(throttled, sizeof throttled, "0x%lx", v);
      free(t);
    }
  }
#endif

  sb_t b;
  sb_init(&b, 8192);
  sb_puts(&b, "{\"version\":");
  sb_json_str(&b, A->version);
  sb_puts(&b, ",\"game_version\":");
  sb_json_str(&b, gv);
  sb_printf(&b, ",\"uptime_ms\":%lld,\"fps\":", (long long)((plat_now_us() - A->start_us) / 1000));
  sb_json_num(&b, st->fps, 1);
  sb_puts(&b, ",\"frame_ms\":{\"avg\":");
  sb_json_num(&b, st->frame_ms_avg, 2);
  sb_puts(&b, ",\"max\":");
  sb_json_num(&b, st->frame_ms_max, 2);
  sb_puts(&b, "},\"run_ms\":{\"avg\":");
  sb_json_num(&b, st->run_ms_avg, 2);
  sb_puts(&b, ",\"p95\":");
  sb_json_num(&b, st->run_ms_p95, 2);
  sb_puts(&b, "},\"present_ms\":");
  sb_json_num(&b, st->present_ms_avg, 2);
  sb_puts(&b, ",\"latency_ms\":");
  sb_json_num(&b, st->latency_ms, 1);
  sb_printf(&b, ",\"frames\":%lu,\"missed\":%lu", (unsigned long)st->frames, (unsigned long)st->missed);
  sb_printf(&b, ",\"game\":{\"state\":%d,\"state_name\":\"%s\",\"loaded\":%s,\"error\":", st->game_state,
            state_name(st->game_state), st->game_loaded ? "true" : "false");
  sb_json_str(&b, err);
  sb_printf(&b, ",\"lua_kb\":%lu,\"script\":", (unsigned long)st->lua_kb);
  sb_json_str(&b, sc);
  sb_puts(&b, "},\"video\":{\"desc\":");
  sb_json_str(&b, vd);
  sb_printf(&b, ",\"render_w\":%d,\"render_h\":%d},\"input\":", st->render_w, st->render_h);
  sticks_json(&b, st);
  sb_puts(&b, ",\"wifi_joystick\":{\"module\":");
  sb_json_str(&b, wm);
  sb_puts(&b, ",\"state\":");
  sb_json_str(&b, ws);
  sb_puts(&b, ",\"rate_hz\":");
  sb_json_num(&b, st->wifi_rate_hz, 1);
  sb_printf(&b, ",\"dropped\":%lu,\"gaps\":%lu,\"gap_max_ms\":%lu},\"audio\":{\"device\":", (unsigned long)st->wifi_dropped,
            (unsigned long)st->wifi_gaps, (unsigned long)st->wifi_gap_max_ms);
  sb_json_str(&b, ad);
  sb_printf(&b, ",\"ok\":%s,\"music\":", st->audio_ok ? "true" : "false");
  sb_json_str(&b, np);
  sb_puts(&b, "},\"network\":{\"ip\":");
  sb_json_str(&b, ip);
  sb_puts(&b, ",\"iface\":");
  sb_json_str(&b, iface);
  sb_puts(&b, ",\"ssid\":");
  sb_json_str(&b, ssid);
  sb_puts(&b, ",\"mode\":");
  sb_json_str(&b, mode);
  sb_puts(&b, ",\"hostname\":");
  sb_json_str(&b, host);
  sb_puts(&b, "},\"system\":{\"cpu_temp\":");
  sb_json_num(&b, sys_cpu_temp(), 1);
  sb_puts(&b, ",\"throttled\":");
  sb_json_str(&b, throttled);
  sb_puts(&b, "},\"vtx\":");
  web_vtx_summary(&b, A);
  sb_puts(&b, "}");
  free(st);
  web_send_sb(c, 200, &b);
  sb_free(&b);
}

static void r_sticks(web_conn_t *c) {
  app_status_t *st = malloc(sizeof *st);
  if (!st) { web_send_error(c, 500, "Internal", "out of memory"); return; }
  app_snapshot(A, st);
  sb_t b;
  sb_init(&b, 4096);
  sticks_json(&b, st);
  free(st);
  web_send_sb(c, 200, &b);
  sb_free(&b);
}

/* text/event-stream: one event every 33 ms until the client goes away */
static void r_stream(web_conn_t *c) {
  if (web_send_head(c, 200, "text/event-stream; charset=utf-8", -1, "Cache-Control: no-store\r\n") != 0) return;
  if (c->head_only) return;
  app_status_t *st = malloc(sizeof *st);
  sb_t b;
  sb_init(&b, 4096);
  if (st && web_send_raw(c, "retry: 1000\n\n", 13) == 0) {
    int64_t next = plat_now_us();
    while (!web_stopping() && !c->dead) {
      app_snapshot(A, st);
      b.len = 0;
      sb_puts(&b, "data: ");
      sticks_json(&b, st);
      sb_puts(&b, "\n\n");
      if (b.oom || web_send_raw(c, b.p, b.len) != 0) break;
      next += 33000;
      int64_t now = plat_now_us();
      if (next < now) next = now;                   /* fell behind: don't burst */
      int ms = (int)((next - now) / 1000);
      if (web_wait_closed(c, ms) || web_wait_closed(c, 0)) break;
    }
  }
  free(st);
  sb_free(&b);
  c->keep_alive = false;
}

static void r_log(web_conn_t *c) {
  size_t cap = sizeof A->log + 1;
  char *buf = malloc(cap);
  if (!buf) { web_send_error(c, 500, "Internal", "out of memory"); return; }
  size_t n = app_log_copy(A, buf, cap);
  if (web_send_head(c, 200, "text/plain; charset=utf-8", (int64_t)n, "Cache-Control: no-store\r\n") == 0) web_send_raw(c, buf, n);
  free(buf);
}

static void r_screenshot(web_conn_t *c) {
  plat_mutex_lock(&shot_mu);
  plat_mutex_lock(&A->mu);
  A->shot_ready = false;
  plat_mutex_unlock(&A->mu);
  app_post(A, APP_CMD_SCREENSHOT);
  uint16_t *copy = NULL;
  int w = 0, h = 0;
  int64_t end = plat_now_us() + 500000;
  /* the frame loop shares A->mu: hold it for the memcpy only, never for a malloc */
  while (!copy && plat_now_us() < end && !web_stopping()) {
    plat_mutex_lock(&A->mu);
    bool ready = A->shot_ready && A->shot && A->shot_w > 0 && A->shot_h > 0 && A->shot_w <= 8192 && A->shot_h <= 8192;
    int sw = A->shot_w, sh = A->shot_h;
    plat_mutex_unlock(&A->mu);
    if (!ready) { plat_sleep_us(5000); continue; }
    uint16_t *buf = malloc((size_t)sw * (size_t)sh * sizeof(uint16_t));
    if (!buf) break;
    plat_mutex_lock(&A->mu);
    if (A->shot_ready && A->shot && A->shot_w == sw && A->shot_h == sh) {
      memcpy(buf, A->shot, (size_t)sw * (size_t)sh * sizeof(uint16_t));
      A->shot_ready = false;
      copy = buf;
      w = sw;
      h = sh;
    }
    plat_mutex_unlock(&A->mu);
    if (!copy) free(buf);                         /* resized meanwhile: try again */
  }
  plat_mutex_unlock(&shot_mu);
  if (!copy) { web_send_error(c, 503, "Busy", "The display didn't hand over a frame in time: try again."); return; }
  int len = 0;
  unsigned char *png = video_png_encode(copy, w, h, &len);
  free(copy);
  if (!png || len <= 0) { free(png); web_send_error(c, 500, "Internal", "Couldn't encode the PNG."); return; }
  if (web_send_head(c, 200, "image/png", len, "Cache-Control: no-store\r\n") == 0) web_send_raw(c, png, (size_t)len);
  free(png);
}

/* ================================================================ game and remote */
static void r_reload(web_conn_t *c) {
  app_post(A, APP_CMD_RELOAD_GAME);
  web_send_ok(c, 200);
}

static void r_key(web_conn_t *c) {
  static const struct { const char *name; int evt; } keys[] = {
    { "enter", ETX_EVT_ENTER }, { "exit", ETX_EVT_EXIT }, { "next", ETX_EVT_NEXT }, { "prev", ETX_EVT_PREV },
    { "inc", ETX_EVT_NEXT }, { "dec", ETX_EVT_PREV }, { "nextpage", ETX_EVT_NEXT_PAGE }, { "prevpage", ETX_EVT_PREV_PAGE },
    { "menu", ETX_EVT_MENU }, { "up", APP_KEY_UP }, { "down", APP_KEY_DOWN },
    { "enterlong", ETX_EVT_ENTER_LONG }, { "exitlong", ETX_EVT_EXIT_LONG },
  };
  char w[32];
  if (!body_word(c, w, sizeof w)) return;
  for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
    if (!strcmp(w, keys[i].name)) {
      app_post_key(A, keys[i].evt);
      web_send_ok(c, 200);
      return;
    }
  web_send_error(c, 400, "BadRequest", "The key must be enter, exit, next, prev, inc, dec, nextpage, prevpage, menu, up or down.");
}

/* ================================================================ the SD card */
enum { POL_GAME, POL_TRACKS, POL_ALL, POL_OFF };
enum { RES_OK = 0, RES_NOPARENT, RES_NOTDIR, RES_LINK, RES_IO };

static int sd_policy(void) {
  settings_t *s = load_settings();
  int p = POL_GAME;
  if (s) {
    const char *v = settings_get(s, "sd_write");
    if (!web_casecmp(v, "tracks")) p = POL_TRACKS;
    else if (!web_casecmp(v, "all")) p = POL_ALL;
    else if (!web_casecmp(v, "off")) p = POL_OFF;
    settings_free(s);
  }
  return p;
}

static const char *iprefix(const char *s, const char *prefix) {
  size_t n = strlen(prefix);
  return web_ncasecmp(s, prefix, n) ? NULL : s + n;
}

static bool is_slot(const char *rel) {
  const char *r = iprefix(rel, "SCRIPTS/TOOLS/StickTimeTracks/");
  return r && (r[0] == 'c' || r[0] == 'C') && r[1] >= '1' && r[1] <= '8' && !web_casecmp(r + 2, ".txt");
}

/* [A-Za-z0-9_-]+\.(lua|luac|txt|new), never data.txt */
static bool edition_file(const char *n) {
  const char *dot = strchr(n, '.');
  if (!dot || dot == n || strchr(dot + 1, '.') || strchr(n, '/')) return false;
  for (const char *p = n; p < dot; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
  const char *e = dot + 1;
  if (web_casecmp(e, "lua") && web_casecmp(e, "luac") && web_casecmp(e, "txt") && web_casecmp(e, "new")) return false;
  return web_casecmp(n, "data.txt") != 0;
}

static bool game_file(const char *rel) {
  static const char *const files[] = { "SCRIPTS/TOOLS/StickTime.lua", "SCRIPTS/TOOLS/StickTimeBW.lua",
    "SCRIPTS/TOOLS/StickTimeLite.lua", "SCRIPTS/TOOLS/StickTimeTracks/README.txt", "SCRIPTS/TOOLS/StickTimeGhosts/README.txt", NULL };
  if (is_slot(rel)) return true;
  for (int i = 0; files[i]; i++) if (!web_casecmp(rel, files[i])) return true;
  const char *r = iprefix(rel, "SCRIPTS/TOOLS/StickTimeBW/");
  if (!r) r = iprefix(rel, "SCRIPTS/TOOLS/StickTimeLite/");
  return r && edition_file(r);
}

static bool dir_in(const char *rel, int n) {
  static const char *const dirs[] = { "SCRIPTS", "SCRIPTS/TOOLS", "SCRIPTS/TOOLS/StickTimeTracks",
    "SCRIPTS/TOOLS/StickTimeGhosts", "SCRIPTS/TOOLS/StickTimeBW", "SCRIPTS/TOOLS/StickTimeLite" };
  for (int i = 0; i < n; i++) if (!web_casecmp(rel, dirs[i])) return true;
  return false;
}

static bool policy_allows(int pol, const char *rel, bool dir) {
  switch (pol) {
    case POL_ALL: return true;
    case POL_TRACKS: return dir ? dir_in(rel, 3) : is_slot(rel);
    case POL_GAME: return dir ? dir_in(rel, 6) : game_file(rel);
    default: return false;
  }
}

static void send_refused(web_conn_t *c, int pol) {
  const char *m = pol == POL_TRACKS ? "This Pi lets the track editor write only its track slots (c1.txt to c8.txt)." :
                  pol == POL_GAME ? "This Pi lets the web page write only the track slots (c1.txt to c8.txt) and the game's "
                                    "own files (sd_write = game in sticktime.txt)." :
                  pol == POL_OFF ? "This Pi doesn't let the web page change its SD card (sd_write = off in sticktime.txt)." :
                  "That can't be changed.";
  web_send_error(c, 403, "NoModificationAllowedError", m);
}

static void send_errno(web_conn_t *c, int e, const char *rel) {
  char m[400];
  switch (e) {
    case ENOENT: snprintf(m, sizeof m, "%s: not found.", rel); web_send_error(c, 404, "NotFoundError", m); return;
    case ENOTDIR:
    case EISDIR: snprintf(m, sizeof m, "%s: a file is where a folder should be, or the other way round.", rel);
                 web_send_error(c, 409, "TypeMismatchError", m); return;
    case ENOTEMPTY:
    case EEXIST: snprintf(m, sizeof m, "%s: the folder isn't empty.", rel);
                 web_send_error(c, 409, "InvalidModificationError", m); return;
    case ENOSPC:
    case EFBIG: web_send_error(c, 507, "QuotaExceededError", "The SD card is full."); return;
    case EROFS: web_send_error(c, 403, "NoModificationAllowedError",
                               "The SD card is read-only now (the system found errors on it): reboot the Pi so it is checked."); return;
    case EACCES:
    case EPERM: snprintf(m, sizeof m, "%s: permission denied.", rel);
                web_send_error(c, 403, "NoModificationAllowedError", m); return;
    case ENAMETOOLONG: web_send_error(c, 400, "TypeError", "The path is too long."); return;
    default: snprintf(m, sizeof m, "%s: %s", rel, strerror(e)); web_send_error(c, 500, "Internal", m);
  }
}

typedef struct {
  char real[1400];       /* on-disk path (the last component as asked when it is missing) */
  char parent[1400];     /* its folder ("" for the root) */
  char name[260];        /* last component: as found on disk, else as asked */
  web_stat_t st;
  int err;               /* RES_IO: errno */
} sd_res_t;

typedef struct { const char *want; char found[260]; int n; } ci_t;

static int ci_cb(void *ud, const char *name, const web_stat_t *st) {
  ci_t *c = ud;
  if (web_is_temp_name(name)) return 0;
  if (!web_casecmp(name, c->want) && (!c->n || strcmp(name, c->found) < 0)) {
    plat_strlcpy(c->found, name, sizeof c->found);
    c->n++;
  }
  return 0;
}

/* Resolve rel (cleaned) under root, case-insensitively per component, never through links. */
static int sd_resolve(const char *root, const char *rel, sd_res_t *r) {
  memset(r, 0, sizeof *r);
  plat_strlcpy(r->real, root, sizeof r->real);
  if (web_fs_stat(root, &r->st) != 0) { r->err = errno; return RES_IO; }
  if (!r->st.exists) return RES_NOPARENT;
  if (!r->st.is_dir) { r->err = ENOTDIR; return RES_IO; }
  for (const char *p = rel; *p;) {
    const char *slash = strchr(p, '/');
    size_t cl = slash ? (size_t)(slash - p) : strlen(p);
    char comp[260];
    if (cl >= sizeof comp) { r->err = ENAMETOOLONG; return RES_IO; }
    memcpy(comp, p, cl);
    comp[cl] = 0;
    plat_strlcpy(r->parent, r->real, sizeof r->parent);
    char cand[1400];
    if (join(cand, sizeof cand, r->parent, comp) != 0) { r->err = ENAMETOOLONG; return RES_IO; }
    web_stat_t st;
    if (web_fs_stat(cand, &st) != 0) { r->err = errno; return RES_IO; }
    const char *name = comp;
    ci_t ci = { comp, "", 0 };
    if (!st.exists) {
      /* a case-sensitive file system (dev runs on ext4): look for another spelling */
      if (web_fs_list(r->parent, ci_cb, &ci) < 0) { r->err = errno; return RES_IO; }
      if (ci.n) {
        name = ci.found;
        if (join(cand, sizeof cand, r->parent, name) != 0 || web_fs_stat(cand, &st) != 0) { r->err = errno; return RES_IO; }
      }
    }
    if (!st.exists) {
      if (slash) return RES_NOPARENT;
      plat_strlcpy(r->real, cand, sizeof r->real);
      plat_strlcpy(r->name, comp, sizeof r->name);
      r->st = st;
      return RES_OK;
    }
    if (st.is_link) return RES_LINK;
    plat_strlcpy(r->real, cand, sizeof r->real);
    plat_strlcpy(r->name, name, sizeof r->name);
    r->st = st;
    if (!slash) break;
    if (!st.is_dir) return RES_NOTDIR;
    p = slash + 1;
  }
  return RES_OK;
}

/* answer a resolve failure; true when it did */
static bool sd_res_fail(web_conn_t *c, int rr, const sd_res_t *r, const char *rel) {
  char m[400];
  switch (rr) {
    case RES_OK: return false;
    case RES_NOPARENT: snprintf(m, sizeof m, "%s: a folder on the way doesn't exist.", rel);
                     web_send_error(c, 404, "NotFoundError", m); return true;
    case RES_NOTDIR: snprintf(m, sizeof m, "%s: a file is in the way where a folder should be.", rel);
                   web_send_error(c, 409, "TypeMismatchError", m); return true;
    case RES_LINK: snprintf(m, sizeof m, "%s: links are not followed.", rel);
                 web_send_error(c, 404, "NotFoundError", m); return true;
    default: send_errno(c, r->err, rel); return true;
  }
}

/* the on-disk spelling of the last component (the file system may have matched another case) */
static void sd_true_name(sd_res_t *r) {
  char tn[260];
  if (r->parent[0] && r->name[0] && web_fs_true_name(r->parent, r->name, tn, sizeof tn) == 0) {
    plat_strlcpy(r->name, tn, sizeof r->name);
    join(r->real, sizeof r->real, r->parent, tn);
  }
}

static void stat_json(sb_t *b, const char *name, const web_stat_t *st) {
  sb_puts(b, "{\"name\":");
  sb_json_str(b, name);
  sb_printf(b, ",\"kind\":\"%s\",\"size\":%lld,\"mtime\":%lld}", st->is_dir ? "directory" : "file",
            (long long)(st->is_dir ? 0 : st->size), (long long)st->mtime_ms);
}

static void send_stat(web_conn_t *c, int status, sd_res_t *r, const char *rel) {
  if (*rel) sd_true_name(r);
  web_stat_t st;
  if (web_fs_stat(r->real, &st) != 0 || !st.exists) st = r->st;
  sb_t b;
  sb_init(&b, 256);
  stat_json(&b, *rel ? r->name : "", &st);
  web_send_sb(c, status, &b);
  sb_free(&b);
}

typedef struct { char *name; web_stat_t st; } entry_t;
typedef struct { entry_t *v; int n, cap; bool oom; } entries_t;
enum { COLLECT_VISIBLE, COLLECT_ALL, COLLECT_CLEAN };
typedef struct { entries_t *e; int mode; } collect_t;

/* VISIBLE: what clients see (no temp files, no links); ALL: everything; CLEAN: folders and our temp files */
static int collect_cb(void *ud, const char *name, const web_stat_t *st) {
  collect_t *k = ud;
  entries_t *e = k->e;
  if (k->mode == COLLECT_VISIBLE && (web_is_temp_name(name) || st->is_link)) return 0;
  if (k->mode == COLLECT_CLEAN && (st->is_link || (!st->is_dir && strncmp(name, ".st-tmp-", 8)))) return 0;
  if (e->n >= 20000) return 1;
  if (e->n == e->cap) {
    int nc = e->cap ? e->cap * 2 : 32;
    entry_t *nv = realloc(e->v, sizeof *nv * (size_t)nc);
    if (!nv) { e->oom = true; return 1; }
    e->v = nv;
    e->cap = nc;
  }
  e->v[e->n].name = strdup(name);
  if (!e->v[e->n].name) { e->oom = true; return 1; }
  e->v[e->n].st = *st;
  e->n++;
  return 0;
}

static int collect(const char *dir, int mode, entries_t *e) {
  collect_t k = { e, mode };
  memset(e, 0, sizeof *e);
  return web_fs_list(dir, collect_cb, &k);
}

static int entry_cmp(const void *a, const void *b) {
  const entry_t *x = a, *y = b;
  int d = web_casecmp(x->name, y->name);
  return d ? d : strcmp(x->name, y->name);
}

static void entries_free(entries_t *e) {
  for (int i = 0; i < e->n; i++) free(e->v[i].name);
  free(e->v);
}

static void sd_list(web_conn_t *c, const sd_res_t *r) {
  entries_t e;
  if (collect(r->real, COLLECT_VISIBLE, &e) < 0) { send_errno(c, errno, "folder"); entries_free(&e); return; }
  if (e.oom) { entries_free(&e); web_send_error(c, 500, "Internal", "out of memory"); return; }
  qsort(e.v, (size_t)e.n, sizeof *e.v, entry_cmp);
  sb_t b;
  sb_init(&b, 256 + (size_t)e.n * 96);
  sb_puts(&b, "{\"entries\":[");
  for (int i = 0; i < e.n; i++) {
    if (i) sb_puts(&b, ",");
    stat_json(&b, e.v[i].name, &e.v[i].st);
  }
  sb_puts(&b, "]}");
  entries_free(&e);
  web_send_sb(c, 200, &b);
  sb_free(&b);
}

/* RFC 7231 date from ms since the epoch (civil-from-days, no gmtime) */
static void http_date(int64_t ms, char *out, size_t n) {
  static const char *const wd[] = { "Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed" };
  static const char *const mo[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  int64_t secs = ms / 1000;
  if (ms < 0 && ms % 1000) secs--;
  int64_t days = secs / 86400, rem = secs % 86400;
  if (rem < 0) { rem += 86400; days--; }
  int w = (int)(((days % 7) + 7) % 7);
  int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
  int64_t doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
  int d = (int)(doy - (153 * mp + 2) / 5 + 1), m = (int)(mp < 10 ? mp + 3 : mp - 9);
  if (m <= 2) y++;
  snprintf(out, n, "%s, %02d %s %04lld %02d:%02d:%02d GMT", wd[w], d, mo[m - 1], (long long)y,
           (int)(rem / 3600), (int)(rem / 60 % 60), (int)(rem % 60));
}

static void sd_read(web_conn_t *c, const sd_res_t *r, const char *rel) {
  web_stat_t st;
  int fd = web_fs_open_read(r->real);
  if (fd < 0) { send_errno(c, errno, rel); return; }
  /* what was opened: a PUT may have replaced the file since it was resolved */
  if (web_fs_fstat(fd, &st) != 0) { int e = errno; web_fs_close(fd); send_errno(c, e, rel); return; }
  if (st.is_dir) { web_fs_close(fd); web_send_error(c, 409, "TypeMismatchError", "That is a folder, not a file (add ?list)."); return; }
  if (st.size > SD_READ_MAX) {
    web_fs_close(fd);
    web_send_error(c, 413, "NotReadableError", "The file is too big to read here (16 MB at most).");
    return;
  }
  char extra[256], date[64];
  http_date(st.mtime_ms, date, sizeof date);
  snprintf(extra, sizeof extra, "Cache-Control: no-store\r\nX-Mtime-Ms: %lld\r\nLast-Modified: %s\r\n", (long long)st.mtime_ms, date);
  if (web_send_head(c, 200, "application/octet-stream", st.size, extra) == 0 && !c->head_only) {
    char *buf = malloc(65536);
    int64_t left = st.size;
    while (buf && left > 0) {
      long n = web_fs_read(fd, buf, left > 65536 ? 65536 : (size_t)left);
      if (n <= 0 || web_send_raw(c, buf, (size_t)n) != 0) break;
      left -= n;
    }
    free(buf);
    if (left > 0) c->keep_alive = false;
  }
  web_fs_close(fd);
}

/* The body into dir/name: a temp file in the same folder, fsync, rename over the target, fsync
 * the folder. 0 ok; -1 failed (errno; the caller answers); -2 the client went away. */
static int body_to_file(web_conn_t *c, const char *dir, const char *name) {
  char tmpname[64], tmp[1400], dst[1400];
  web_temp_name(tmpname, sizeof tmpname);
  if (join(tmp, sizeof tmp, dir, tmpname) != 0 || join(dst, sizeof dst, dir, name) != 0) return -1;
  int fd = web_fs_create_excl(tmp);
  if (fd < 0) return -1;
  char *buf = malloc(65536);
  int rc = 0, e = 0;
  if (!buf) { rc = -1; e = ENOMEM; }
  while (!rc && c->body_left > 0) {
    long n = web_read_body(c, buf, 65536);
    if (n <= 0) { rc = -2; break; }
    if (web_fs_write_all(fd, buf, (size_t)n) != 0) { rc = -1; e = errno; }
  }
  free(buf);
  if (rc) {
    web_fs_close(fd);
    web_fs_unlink(tmp);
    errno = e;
    return rc;
  }
  if (web_fs_sync_close(fd) != 0 || web_fs_rename(tmp, dst) != 0) {
    e = errno;
    web_fs_unlink(tmp);
    errno = e;
    return -1;
  }
  plat_sync_dir(dir);
  return 0;
}

/* every component of a path we write: Windows device names never (the card is read on PCs) */
static bool has_device_name(const char *rel) {
  char comp[260];
  for (const char *p = rel; *p;) {
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - p) : strlen(p);
    if (n >= sizeof comp) return false;
    memcpy(comp, p, n);
    comp[n] = 0;
    if (web_is_device_name(comp)) return true;
    if (!slash) break;
    p = slash + 1;
  }
  return false;
}

static bool has_temp_name(const char *rel) {
  for (const char *p = rel; p && *p;) {
    if (!strncmp(p, ".st-tmp-", 8)) return true;
    const char *slash = strchr(p, '/');
    char comp[260];
    size_t n = slash ? (size_t)(slash - p) : strlen(p);
    if (n < sizeof comp) {
      memcpy(comp, p, n);
      comp[n] = 0;
      if (web_is_temp_name(comp)) return true;
    }
    p = slash ? slash + 1 : NULL;
  }
  return false;
}

static void sd_put(web_conn_t *c, const char *rel, int pol) {
  if (!c->has_len) { web_send_error(c, 411, "BadRequest", "Send the file with a Content-Length."); return; }
  if (!*rel) { web_send_error(c, 409, "TypeMismatchError", "The SD card's root is a folder."); return; }
  if (!policy_allows(pol, rel, false)) { send_refused(c, pol); return; }
  int64_t max = is_slot(rel) ? SLOT_MAX : SD_PUT_MAX;
  if (c->clen > max) {
    char m[160];
    snprintf(m, sizeof m, "The file is too big: %lld KB at most here.", (long long)(max / 1024));
    web_send_error(c, 413, "QuotaExceededError", m);
    return;
  }
  sd_res_t r;
  int rr = sd_resolve(A->paths.sd, rel, &r);
  if (sd_res_fail(c, rr, &r, rel)) return;
  if (r.st.exists && r.st.is_dir) { web_send_error(c, 409, "TypeMismatchError", "That is a folder, not a file."); return; }
  if (r.st.exists) sd_true_name(&r);              /* keep the spelling on disk */
  int br = body_to_file(c, r.parent, r.name);
  if (br == -2) return;
  if (br != 0) { send_errno(c, errno, rel); return; }
  web_stat_t st;
  if (web_fs_stat(r.real, &st) != 0 || !st.exists) memset(&st, 0, sizeof st);
  if (game_file(rel)) web_note_sd_dirty();
  char json[96];
  int n = snprintf(json, sizeof json, "{\"size\":%lld,\"mtime\":%lld}", (long long)st.size, (long long)st.mtime_ms);
  web_send_json(c, 200, json, (size_t)n);
}

static void sd_create(web_conn_t *c, const char *rel, int pol) {
  char kind[16] = "";
  query_get(c->query, "create", kind, sizeof kind);
  bool dir;
  if (!strcmp(kind, "file")) dir = false;
  else if (!strcmp(kind, "directory")) dir = true;
  else { web_send_error(c, 400, "BadRequest", "Use ?create=file or ?create=directory."); return; }
  sd_res_t r;
  int rr = sd_resolve(A->paths.sd, rel, &r);
  /* already there: nothing changes, so the policy has no say (what getDirectoryHandle(create) expects) */
  if (rr == RES_OK && r.st.exists) {
    if (r.st.is_dir != dir) {
      web_send_error(c, 409, "TypeMismatchError", r.st.is_dir ? "That is a folder, not a file." : "That is a file, not a folder.");
      return;
    }
    send_stat(c, 200, &r, rel);
    return;
  }
  if (!policy_allows(pol, rel, dir)) { send_refused(c, pol); return; }
  if (sd_res_fail(c, rr, &r, rel)) return;
  int made;
  if (dir) {
    made = web_fs_mkdir(r.real) == 0;
  } else {
    int fd = web_fs_create_excl(r.real);
    made = fd >= 0 && web_fs_sync_close(fd) == 0;
  }
  if (!made) {
    int e = errno;
    web_stat_t st;
    if (e == EEXIST && web_fs_stat(r.real, &st) == 0 && st.exists && st.is_dir == dir) {   /* made meanwhile */
      r.st = st;
      send_stat(c, 200, &r, rel);
      return;
    }
    send_errno(c, e == EEXIST ? ENOTDIR : e, rel);
    return;
  }
  plat_sync_dir(r.parent);
  if (!dir && game_file(rel)) web_note_sd_dirty();
  web_fs_stat(r.real, &r.st);
  send_stat(c, 201, &r, rel);
}

static int rm_tree(const char *path, int depth) {
  if (depth > 32) { errno = ELOOP; return -1; }
  entries_t e;
  if (collect(path, COLLECT_ALL, &e) < 0 || e.oom) { int er = e.oom ? ENOMEM : errno; entries_free(&e); errno = er; return -1; }
  int rc = 0;
  for (int i = 0; i < e.n && !rc; i++) {
    char p[1400];
    if (join(p, sizeof p, path, e.v[i].name) != 0) { rc = -1; break; }
    /* a link is removed itself, never followed */
    rc = e.v[i].st.is_dir && !e.v[i].st.is_link ? rm_tree(p, depth + 1) : web_fs_unlink(p);
  }
  entries_free(&e);
  return rc ? rc : web_fs_rmdir(path);
}

static void sd_delete(web_conn_t *c, const char *rel, int pol) {
  if (!*rel) { web_send_error(c, 403, "NoModificationAllowedError", "The SD card's root can't be deleted."); return; }
  if (!policy_allows(pol, rel, false) && !policy_allows(pol, rel, true)) { send_refused(c, pol); return; }
  char rv[16] = "";
  bool recursive = query_get(c->query, "recursive", rv, sizeof rv) && (!rv[0] || truthy(rv));
  sd_res_t r;
  int rr = sd_resolve(A->paths.sd, rel, &r);
  if (sd_res_fail(c, rr, &r, rel)) return;
  if (!r.st.exists) {
    char m[400];
    snprintf(m, sizeof m, "%s: not found.", rel);
    web_send_error(c, 404, "NotFoundError", m);
    return;
  }
  if (!policy_allows(pol, rel, r.st.is_dir)) { send_refused(c, pol); return; }
  int rc;
  if (r.st.is_dir) {
    if (recursive && pol == POL_ALL) {
      rc = rm_tree(r.real, 0);
    } else {
      rc = web_fs_rmdir(r.real);
      if (rc != 0 && (errno == ENOTEMPTY || errno == EEXIST) && recursive) {
        web_send_error(c, 403, "NoModificationAllowedError", "This Pi lets the web page delete only empty folders (sd_write = all allows more).");
        return;
      }
    }
  } else {
    rc = web_fs_unlink(r.real);
  }
  if (rc != 0) { send_errno(c, errno, rel); return; }
  plat_sync_dir(r.parent);
  if (game_file(rel) || (r.st.is_dir && dir_in(rel, 6))) web_note_sd_dirty();
  web_send_empty(c, 204, NULL);
}

static void r_sd(web_conn_t *c) {
  const char *rest = c->path + 7;                /* after "/api/sd" */
  char rel[1100];
  if (web_clean_path(rest, rel, sizeof rel) != 0 || has_temp_name(rel)) {
    web_send_error(c, 400, "TypeError", "That name isn't allowed on the SD card (no \\ / : * ? \" < > |, control "
                                        "characters, . or .. or a name ending in a dot or space).");
    return;
  }
  const char *m = c->method;
  bool get = !strcmp(m, "GET") || !strcmp(m, "HEAD");
  if (!get && strcmp(m, "PUT") && strcmp(m, "POST") && strcmp(m, "DELETE")) { send_405(c, "GET, HEAD, PUT, POST, DELETE"); return; }
#ifdef _WIN32
  bool dev = has_device_name(rel);               /* opening "con" here would read the console */
#else
  bool dev = !get && has_device_name(rel);
#endif
  if (dev) { web_send_error(c, 400, "TypeError", "Names like CON, NUL, COM1 or LPT1 aren't allowed (Windows can't open them)."); return; }
  if (get) {
    sd_res_t r;
    int rr = sd_resolve(A->paths.sd, rel, &r);
    if (sd_res_fail(c, rr, &r, rel)) return;
    if (!r.st.exists) {
      char msg[400];
      snprintf(msg, sizeof msg, "%s: not found.", *rel ? rel : "the SD card");
      web_send_error(c, 404, "NotFoundError", msg);
      return;
    }
    if (query_get(c->query, "list", NULL, 0)) {
      if (!r.st.is_dir) { web_send_error(c, 409, "TypeMismatchError", "That is a file, not a folder."); return; }
      sd_list(c, &r);
    } else if (query_get(c->query, "stat", NULL, 0)) {
      send_stat(c, 200, &r, rel);
    } else {
      if (r.st.is_dir) { web_send_error(c, 409, "TypeMismatchError", "That is a folder, not a file (add ?list)."); return; }
      sd_read(c, &r, rel);
    }
    return;
  }
  int pol = sd_policy();
  if (!strcmp(m, "PUT")) sd_put(c, rel, pol);
  else if (!strcmp(m, "POST")) sd_create(c, rel, pol);
  else sd_delete(c, rel, pol);
  if (c->status && c->status < 300) app_log(A, "web: %s /api/sd/%s", m, rel);
}

/* ================================================================ tools */
typedef struct { sb_t *b; const char *dir; int n; entries_t e; } tools_t;

static void tool_name(const char *path, const char *file, char *out, size_t n) {
  size_t len;
  char *t = read_small(path, 1024, &len);
  if (!t) {
    /* bigger than 1 KB: read just the start */
    int fd = web_fs_open_read(path);
    if (fd >= 0) {
      t = malloc(1025);
      long r = t ? web_fs_read(fd, t, 1024) : -1;
      if (t) t[r > 0 ? r : 0] = 0;
      web_fs_close(fd);
    }
  }
  const char *a = t ? strstr(t, "TNS|") : NULL, *z = a ? strstr(a + 4, "|TNE") : NULL;
  if (a && z && z - a - 4 > 0 && (size_t)(z - a - 4) < n && !memchr(a + 4, '\n', (size_t)(z - a - 4))) {
    memcpy(out, a + 4, (size_t)(z - a - 4));
    out[z - a - 4] = 0;
  } else {
    plat_strlcpy(out, file, n);
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
  }
  free(t);
}

static void r_tools(web_conn_t *c) {
  settings_t *s = load_settings();
  sb_t b;
  sb_init(&b, 1024);
  sb_puts(&b, "{\"current\":");
  sb_json_str(&b, s ? settings_get(s, "script") : "/SCRIPTS/TOOLS/StickTime.lua");
  if (s) settings_free(s);
  sb_puts(&b, ",\"tools\":[");
  sd_res_t r;
  entries_t e = { 0 };
  if (sd_resolve(A->paths.sd, "SCRIPTS/TOOLS", &r) == RES_OK && r.st.exists && r.st.is_dir) collect(r.real, COLLECT_VISIBLE, &e);
  qsort(e.v, (size_t)e.n, sizeof *e.v, entry_cmp);
  int n = 0;
  for (int i = 0; i < e.n; i++) {
    size_t nl = strlen(e.v[i].name);
    if (e.v[i].st.is_dir || nl < 5 || web_casecmp(e.v[i].name + nl - 4, ".lua")) continue;
    char p[1400], name[128];
    if (join(p, sizeof p, r.real, e.v[i].name) != 0) continue;
    tool_name(p, e.v[i].name, name, sizeof name);
    sb_puts(&b, n++ ? ",{\"path\":" : "{\"path\":");
    char sdp[400];
    snprintf(sdp, sizeof sdp, "/SCRIPTS/TOOLS/%s", e.v[i].name);
    sb_json_str(&b, sdp);
    sb_puts(&b, ",\"name\":");
    sb_json_str(&b, name);
    sb_puts(&b, "}");
  }
  entries_free(&e);
  sb_puts(&b, "]}");
  web_send_sb(c, 200, &b);
  sb_free(&b);
}

/* ================================================================ songs */
static void music_dir(char *out, size_t n) { join(out, n, A->paths.base, "music"); }

static bool song_ext(const char *name) {
  const char *dot = strrchr(name, '.');
  return dot && (!web_casecmp(dot, ".mp3") || !web_casecmp(dot, ".ogg") || !web_casecmp(dot, ".wav"));
}

static int64_t free_mb_of(const char *path) {
  int64_t fr, tot;
  return web_fs_disk(path, &fr, &tot) == 0 ? fr / (1024 * 1024) : -1;
}

static void r_songs(web_conn_t *c) {
  char dir[700];
  music_dir(dir, sizeof dir);
  entries_t e;
  collect(dir, COLLECT_VISIBLE, &e);           /* missing: no songs */
  qsort(e.v, (size_t)e.n, sizeof *e.v, entry_cmp);
  sb_t b;
  sb_init(&b, 1024);
  sb_puts(&b, "{\"songs\":[");
  int n = 0;
  for (int i = 0; i < e.n; i++) {
    if (e.v[i].st.is_dir || !song_ext(e.v[i].name)) continue;
    sb_puts(&b, n++ ? "," : "");
    stat_json(&b, e.v[i].name, &e.v[i].st);
  }
  entries_free(&e);
  web_stat_t st;
  int64_t fm = free_mb_of(web_fs_stat(dir, &st) == 0 && st.exists ? dir : A->paths.base);
  sb_printf(&b, "],\"free_mb\":%lld}", (long long)fm);
  web_send_sb(c, 200, &b);
  sb_free(&b);
}

static void r_song(web_conn_t *c) {
  const char *m = c->method;
  bool get = !strcmp(m, "GET") || !strcmp(m, "HEAD");
  if (!get && strcmp(m, "PUT") && strcmp(m, "DELETE")) { send_405(c, "GET, HEAD, PUT, DELETE"); return; }
  char name[1100];
  if (web_clean_path(c->path + 11, name, sizeof name) != 0 || !name[0] || strchr(name, '/') || strlen(name) > SONG_NAME_MAX ||
      web_is_temp_name(name) || web_is_device_name(name) || name[0] == '.') {
    web_send_error(c, 400, "BadRequest", "That song name isn't allowed (120 bytes at most, no / \\ : * ? \" < > |).");
    return;
  }
  if (!song_ext(name)) { web_send_error(c, 400, "BadRequest", "Songs must be .mp3, .ogg or .wav files."); return; }
  char dir[700], tn[260], path[1400];
  music_dir(dir, sizeof dir);
  bool found = web_fs_true_name(dir, name, tn, sizeof tn) == 0;
  if (!found) plat_strlcpy(tn, name, sizeof tn);
  if (join(path, sizeof path, dir, tn) != 0) { web_send_error(c, 400, "BadRequest", "The name is too long."); return; }
  if (get) {
    web_send_file(c, path, NULL);
    return;
  }
  if (!strcmp(m, "DELETE")) {
    web_stat_t st;
    if (!found || web_fs_stat(path, &st) != 0 || !st.exists || st.is_dir) {
      web_send_error(c, 404, "NotFound", "There is no such song.");
      return;
    }
    if (web_fs_unlink(path) != 0) { send_errno(c, errno, name); return; }
    plat_sync_dir(dir);
    app_post(A, APP_CMD_SONGS_CHANGED);
    web_send_empty(c, 204, NULL);
    return;
  }
  if (!c->has_len) { web_send_error(c, 411, "BadRequest", "Send the song with a Content-Length."); return; }
  if (c->clen > (int64_t)web_max_upload()) {
    char msg[96];
    snprintf(msg, sizeof msg, "The song is too big (%lu MB at most).", (unsigned long)(web_max_upload() >> 20));
    web_send_error(c, 413, "PayloadTooLarge", msg);
    return;
  }
  if (plat_mkdirs(dir) != 0) { send_errno(c, errno, "music"); return; }
  web_stat_t st;
  if (web_fs_stat(path, &st) == 0 && st.exists && st.is_dir) {
    web_send_error(c, 409, "TypeMismatchError", "A folder has that name.");
    return;
  }
  int64_t fr, tot;
  if (web_fs_disk(dir, &fr, &tot) == 0 && fr < c->clen + SONG_RESERVE) {
    char msg[128];
    snprintf(msg, sizeof msg, "Not enough space on the SD card (%lld MB free).", (long long)(fr >> 20));
    web_send_error(c, 507, "QuotaExceededError", msg);
    return;
  }
  int br = body_to_file(c, dir, tn);
  if (br == -2) return;
  if (br != 0) { send_errno(c, errno, name); return; }
  app_post(A, APP_CMD_SONGS_CHANGED);
  app_log(A, "web: song %s uploaded", tn);
  char json[64];
  int n = snprintf(json, sizeof json, "{\"size\":%lld}", (long long)c->clen);
  web_send_json(c, 200, json, (size_t)n);
}

static void r_music_next(web_conn_t *c) {
  app_post(A, APP_CMD_MUSIC_NEXT);
  web_send_ok(c, 200);
}

/* ================================================================ WiFi */
static void scan_path(char *out, size_t n) { join(out, n, A->paths.state, "wifi-scan.json"); }

static void *scan_main(void *arg) {
  (void)arg;
  plat_thread_lower_priority();
  char cmd[1600], err[256] = "";
  char *out = malloc(256 * 1024);
  if (!out) plat_strlcpy(err, "out of memory", sizeof err);
  else if (!helper_cmd("sticktime-wifi", "scan", cmd, sizeof cmd)) plat_strlcpy(err, "The WiFi helper isn't installed.", sizeof err);
  else {
    app_log(A, "web: WiFi scan (the WiFi joystick may pause for a moment)");
    int rc = run_capture(cmd, out, 256 * 1024, 30000);
    char *p = out, msg[200] = "", note[200] = "";
    trim(&p);
    size_t len = strlen(p);
    const char *v = NULL, *ve = NULL;
    bool valid = web_json_valid(p, len), obj = valid && *p == '{';
    /* the helper prints {"networks":[...],"at":..,"error"?,"note"?} even when it fails */
    if (obj && !web_json_get_str(p, len, "error", msg, sizeof msg)) web_json_get_str(p, len, "message", msg, sizeof msg);
    if (obj) web_json_get_str(p, len, "note", note, sizeof note);
    if (rc != 0 && msg[0]) snprintf(err, sizeof err, "The WiFi scan failed: %s", msg);
    else if (rc != 0) snprintf(err, sizeof err, "The WiFi scan failed (exit code %d).", rc);
    else if (!valid) plat_strlcpy(err, "The WiFi helper gave no valid JSON.", sizeof err);
    else if (*p == '[') { v = p; ve = p + len; }
    else if (!obj || !web_json_find(p, len, "networks", &v, &ve) || *v != '[') plat_strlcpy(err, "The WiFi helper gave no network list.", sizeof err);
    if (!err[0]) {
      sb_t b;
      sb_init(&b, (size_t)(ve - v) + 512);
      sb_puts(&b, "{\"networks\":");
      sb_add(&b, v, (size_t)(ve - v));
      if (note[0] || msg[0]) {                    /* "showing the last results", "the hotspot is on" */
        sb_puts(&b, ",\"note\":");
        sb_json_str(&b, note[0] ? note : msg);
      }
      sb_printf(&b, ",\"at\":%lld}", (long long)plat_wall_ms());
      char path[700];
      scan_path(path, sizeof path);
      plat_mkdirs(A->paths.state);
      if (b.oom || plat_write_file_atomic(path, b.p, b.len) != 0) snprintf(err, sizeof err, "Can't keep the scan: %s", strerror(errno));
      sb_free(&b);
    }
  }
  free(out);
  if (err[0]) app_log(A, "web: wifi scan: %s", err);
  plat_mutex_lock(&scan_mu);
  plat_strlcpy(scan_err, err, sizeof scan_err);
  scan_running = false;
  plat_cond_broadcast(&scan_cv);
  plat_mutex_unlock(&scan_mu);
  return NULL;
}

/* the cache when it is valid; *age_ms how old */
static char *scan_cache(size_t *len, int64_t *age_ms) {
  char path[700];
  scan_path(path, sizeof path);
  char *t = read_small(path, 1 << 20, len);
  const char *v, *ve;
  if (!t || !web_json_valid(t, *len) || !web_json_find(t, *len, "at", &v, &ve)) { free(t); return NULL; }
  int64_t at = strtoll(v, NULL, 10), now = plat_wall_ms();
  *age_ms = now >= at ? now - at : INT64_MAX;      /* from the future: the clock was set since */
  return t;
}

static void r_wifi_scan(web_conn_t *c) {
  size_t len = 0;
  int64_t age = 0;
  char *t = scan_cache(&len, &age);
  /* ?cached=1: the last results however old, never a new scan. A scan takes the WiFi radio off
   * its channel for a moment, which interrupts the WiFi joystick's stream. */
  char cv[8];
  if (query_get(c->query, "cached", cv, sizeof cv) && (!cv[0] || truthy(cv))) {
    if (t) web_send_json(c, 200, t, len);
    else web_send_error(c, 404, "NotFound", "No scan yet.");
    free(t);
    return;
  }
  if (t && age < SCAN_FRESH_MS) { web_send_json(c, 200, t, len); free(t); return; }
  plat_mutex_lock(&scan_mu);
  if (!scan_running) {
    scan_running = true;
    scan_err[0] = 0;
    if (plat_thread_start(NULL, scan_main, NULL, true) != 0) scan_running = false;
  }
  int64_t end = plat_now_us() + (int64_t)SCAN_WAIT_MS * 1000;
  while (scan_running && plat_now_us() < end && !web_stopping()) plat_cond_wait(&scan_cv, &scan_mu, 100);
  bool done = !scan_running;
  char err[256];
  plat_strlcpy(err, scan_err, sizeof err);
  plat_mutex_unlock(&scan_mu);
  if (done && !err[0]) {
    free(t);
    t = scan_cache(&len, &age);
  }
  if (t) web_send_json(c, 200, t, len);
  else if (done && err[0]) web_send_error(c, 500, "Internal", err);
  else web_send_error(c, 503, "Busy", "Scanning for networks: try again in a few seconds.");
  free(t);
}

static void r_wifi(web_conn_t *c) {
  char *body;
  size_t len;
  int r = web_read_body_all(c, SMALL_BODY_MAX, &body, &len);
  if (r == -2) { web_send_error(c, 413, "PayloadTooLarge", "The body is too big."); return; }
  if (r != 0) return;
  kvs_t kv;
  bool ok = parse_kvs(c->ctype, body, len, &kv);
  free(body);
  if (!ok) { kvs_free(&kv); web_send_error(c, 400, "BadRequest", "The body isn't in settings format or form encoding."); return; }
  const char *ssid = NULL, *pw = NULL, *country = NULL;
  bool hidden = false;
  for (int i = 0; i < kv.n; i++) {
    if (!strcmp(kv.v[i].key, "ssid")) ssid = kv.v[i].val;
    else if (!strcmp(kv.v[i].key, "password")) pw = kv.v[i].val;
    else if (!strcmp(kv.v[i].key, "country")) country = kv.v[i].val;
    else if (!strcmp(kv.v[i].key, "hidden")) hidden = truthy(kv.v[i].val);
  }
  sb_t f;
  sb_init(&f, 256);
  int nbad = 0;
#define BAD(k, m) do { sb_printf(&f, "%s\"%s\":", nbad++ ? "," : "", k); sb_json_str(&f, m); } while (0)
  if (!ssid || !*ssid) BAD("ssid", "the network name is needed");
  else if (strlen(ssid) > 32) BAD("ssid", "at most 32 bytes");
  else if (!value_ok(ssid)) BAD("ssid", "no control characters");
  size_t pl = pw ? strlen(pw) : 0;
  if (pw && pl) {
    bool hex64 = pl == 64;
    for (size_t i = 0; hex64 && i < 64; i++) hex64 = (pw[i] >= '0' && pw[i] <= '9') || ((pw[i] | 0x20) >= 'a' && (pw[i] | 0x20) <= 'f');
    if (!hex64 && (pl < 8 || pl > 63)) BAD("password", "8 to 63 characters (empty for an open network)");
    else if (!value_ok(pw)) BAD("password", "no control characters");
  }
  char cc[3] = "";
  if (country && *country) {
    if (strlen(country) != 2 || !((country[0] | 0x20) >= 'a' && (country[0] | 0x20) <= 'z') ||
        !((country[1] | 0x20) >= 'a' && (country[1] | 0x20) <= 'z')) BAD("country", "a two-letter country code (US, GB, DE, HR, ...)");
    else { cc[0] = (char)(country[0] & ~0x20); cc[1] = (char)(country[1] & ~0x20); }
  }
#undef BAD
  if (nbad) {
    sb_t b;
    sb_init(&b, 256);
    sb_puts(&b, "{\"error\":\"BadRequest\",\"message\":\"Some values are not valid: nothing was saved.\",\"fields\":{");
    sb_add(&b, f.p, f.len);
    sb_puts(&b, "}}");
    web_send_sb(c, 400, &b);
    sb_free(&b);
  } else {
    /* sticktime-wifi apply reads all of it from sticktime.txt (wifi_hidden too: not a documented key) */
    change_t ch[4];
    int n = 0;
    ch[n++] = (change_t){ "wifi_ssid", ssid };
    if (pw) ch[n++] = (change_t){ "wifi_password", pw };
    if (cc[0]) ch[n++] = (change_t){ "wifi_country", cc };
    ch[n++] = (change_t){ "wifi_hidden", hidden ? "on" : NULL };
    if (save_keys(ch, n) != 0) {
      send_errno(c, errno, "sticktime.txt");
    } else {
      bool applied = spawn_helper("sticktime-wifi", "apply");
      char json[64];
      int k = snprintf(json, sizeof json, "{\"ok\":true,\"applied\":%s}", applied ? "true" : "false");
      web_send_json(c, 200, json, (size_t)k);
    }
  }
  sb_free(&f);
  kvs_free(&kv);
}

static void r_module_ap(web_conn_t *c) {
  char w[32];
  if (!body_word(c, w, sizeof w)) return;
  const char *ip = !strcmp(w, "elrs") ? "10.0.0.1" : !strcmp(w, "tbs") ? "192.168.4.1" : NULL;
  if (!ip) { web_send_error(c, 400, "BadRequest", "The module must be elrs or tbs."); return; }
  change_t ch = { "wifi_module_ip", ip };
  if (save_keys(&ch, 1) != 0) { send_errno(c, errno, "sticktime.txt"); return; }
  char args[32];
  snprintf(args, sizeof args, "module-ap %s", w);
  bool applied = spawn_helper("sticktime-wifi", args);
  char json[96];
  int n = snprintf(json, sizeof json, "{\"ok\":true,\"applied\":%s,\"module_ip\":\"%s\"}", applied ? "true" : "false", ip);
  web_send_json(c, 200, json, (size_t)n);
}

/* ================================================================ updates and the system */
static void r_update(web_conn_t *c) {
  char path[700];
  size_t len = 0;
  char *t = join(path, sizeof path, A->paths.state, "update.json") == 0 ? read_small(path, 1 << 20, &len) : NULL;
  if (t) {
    const char *p = t;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p == '{' && web_json_valid(t, len)) { web_send_json(c, 200, t, len); free(t); return; }
    free(t);
  }
  app_status_t *st = malloc(sizeof *st);
  char gv[40] = "";
  if (st) {
    app_snapshot(A, st);
    copy_str(gv, sizeof gv, st->game_version, sizeof st->game_version);
    free(st);
  }
  sb_t b;
  sb_init(&b, 512);
  sb_puts(&b, "{\"state\":\"idle\",\"message\":\"\",\"progress\":0,\"game\":{\"installed\":");
  sb_json_str(&b, gv);
  sb_puts(&b, ",\"latest\":\"\",\"staged\":\"\",\"notes\":\"\"},\"app\":{\"installed\":");
  sb_json_str(&b, A->version);
  sb_puts(&b, ",\"latest\":\"\",\"notes\":\"\"},\"checked_at\":0,\"error\":\"\"}");
  web_send_sb(c, 200, &b);
  sb_free(&b);
}

static void update_run(web_conn_t *c, const char *verb, bool needs_what) {
  char args[48], w[32] = "";
  if (needs_what) {
    if (!body_word(c, w, sizeof w)) return;
    if (strcmp(w, "game") && strcmp(w, "app")) { web_send_error(c, 400, "BadRequest", "Say game or app."); return; }
    snprintf(args, sizeof args, "%s %s", verb, w);
  } else {
    plat_strlcpy(args, verb, sizeof args);
  }
  if (!spawn_helper("sticktime-update", args)) { web_send_error(c, 500, "Internal", "The updater isn't installed."); return; }
  web_send_ok(c, 202);
}

static void r_update_check(web_conn_t *c) { update_run(c, "check", false); }
static void r_update_apply(web_conn_t *c) { update_run(c, "apply", true); }
static void r_update_rollback(web_conn_t *c) { update_run(c, "rollback", true); }

static void r_reboot(web_conn_t *c) { web_send_ok(c, 202); later(LATER_REBOOT); }
static void r_shutdown(web_conn_t *c) { web_send_ok(c, 202); later(LATER_POWEROFF); }
static void r_restart(web_conn_t *c) { web_send_ok(c, 202); app_log(A, "web: restart"); later(LATER_QUIT); }

static void r_system(web_conn_t *c) {
  settings_t *s = load_settings();
  int64_t fr = 0, tot = 0;
  web_fs_disk(A->paths.base, &fr, &tot);
  char os[160] = "", kernel[96] = "", model[128] = "";
#ifdef __linux__
  size_t len;
  /* /etc/os-release is a symlink to /usr/lib/os-release on Debian and Pi OS (read_small won't follow one) */
  char *t = read_small("/etc/os-release", 8192, &len);
  if (!t) t = read_small("/usr/lib/os-release", 8192, &len);
  if (t) {
    char *p = strstr(t, "PRETTY_NAME=");
    if (p && (p == t || p[-1] == '\n')) {
      p += 12;
      if (*p == '"') p++;
      size_t n = strcspn(p, "\"\n");
      if (n >= sizeof os) n = sizeof os - 1;
      memcpy(os, p, n);
      os[n] = 0;
    }
    free(t);
  }
  struct utsname u;
  if (uname(&u) == 0) plat_strlcpy(kernel, u.release, sizeof kernel);
  t = read_small("/proc/device-tree/model", 512, &len);
  if (t) { plat_strlcpy(model, t, sizeof model); free(t); }
#else
  plat_strlcpy(os, "not a Pi (development build)", sizeof os);
#endif
  sb_t b;
  sb_init(&b, 512);
  sb_puts(&b, "{\"hostname\":");
  sb_json_str(&b, s ? settings_get(s, "hostname") : "sticktime");
  const char *pw = s ? settings_raw(s, "password") : NULL;
  sb_printf(&b, ",\"ssh\":%s,\"password_set\":%s,\"disk\":{\"free_mb\":%lld,\"total_mb\":%lld},\"os\":",
            s && settings_get_bool(s, "ssh", false) ? "true" : "false", pw && *pw ? "true" : "false",
            (long long)(fr >> 20), (long long)(tot >> 20));
  sb_json_str(&b, os);
  sb_puts(&b, ",\"kernel\":");
  sb_json_str(&b, kernel);
  sb_puts(&b, ",\"model\":");
  sb_json_str(&b, model);
  sb_puts(&b, ",\"version\":");
  sb_json_str(&b, A->version);
  sb_puts(&b, "}");
  if (s) settings_free(s);
  web_send_sb(c, 200, &b);
  sb_free(&b);
}

/* ================================================================ pages */
/* a file under dir by its URL path (no dot files, no listings; "x/" means x/index.html) */
static void static_file(web_conn_t *c, const char *dir, const char *urlpath, const char *ctype) {
  char rel[1100], path[1400];
  if (web_clean_path(urlpath, rel, sizeof rel) != 0) { web_send_error(c, 400, "BadRequest", "That path isn't allowed."); return; }
  for (const char *p = rel; p; p = strchr(p, '/') ? strchr(p, '/') + 1 : NULL)
    if (*p == '.') { web_send_error(c, 404, "NotFound", "There is no such page or file."); return; }
  size_t ul = strlen(urlpath);
  bool dirslash = !ul || urlpath[ul - 1] == '/';
  if (dirslash) {
    if (rel[0] && strlen(rel) + 11 < sizeof rel) strcat(rel, "/index.html");
    else if (!rel[0]) plat_strlcpy(rel, "index.html", sizeof rel);
  }
  if (join(path, sizeof path, dir, rel) != 0) { web_send_error(c, 404, "NotFound", "There is no such page or file."); return; }
  web_send_file(c, path, ctype);
}

static void r_editor(web_conn_t *c) {
  const char *p = c->path + 7;                    /* after "/editor" */
  if (!strcmp(p, "/pi-sd.js")) {
    char path[700];
    join(path, sizeof path, A->paths.web, "pi-sd.js");
    web_send_file(c, path, "text/javascript; charset=utf-8");
    return;
  }
  char dir[700];
  join(dir, sizeof dir, A->paths.base, "editor");
  static_file(c, dir, p, NULL);
}

/* ================================================================ dispatch */
enum { M_GET = 1, M_HEAD = 2, M_POST = 4, M_PUT = 8, M_DELETE = 16 };
typedef struct { const char *path; int methods; void (*fn)(web_conn_t *c); } route_t;

static const route_t ROUTES[] = {
  { "/api/status", M_GET | M_HEAD, r_status },
  { "/api/sticks", M_GET | M_HEAD, r_sticks },
  { "/api/sticks/stream", M_GET, r_stream },
  { "/api/log", M_GET | M_HEAD, r_log },
  { "/api/screenshot.png", M_GET | M_HEAD, r_screenshot },
  { "/api/settings", M_GET | M_HEAD | M_POST, r_settings },
  { "/api/game/reload", M_POST, r_reload },
  { "/api/key", M_POST, r_key },
  { "/api/tools", M_GET | M_HEAD, r_tools },
  { "/api/songs", M_GET | M_HEAD, r_songs },
  { "/api/music/next", M_POST, r_music_next },
  { "/api/wifi/scan", M_GET | M_HEAD, r_wifi_scan },
  { "/api/wifi", M_POST, r_wifi },
  { "/api/wifi/module-ap", M_POST, r_module_ap },
  { "/api/update", M_GET | M_HEAD, r_update },
  { "/api/update/check", M_POST, r_update_check },
  { "/api/update/apply", M_POST, r_update_apply },
  { "/api/update/rollback", M_POST, r_update_rollback },
  { "/api/reboot", M_POST, r_reboot },
  { "/api/shutdown", M_POST, r_shutdown },
  { "/api/restart", M_POST, r_restart },
  { "/api/system", M_GET | M_HEAD, r_system },
  { "/api/vtx", M_GET | M_HEAD | M_POST, web_vtx_route },
  { "/api/vtx/table", M_GET | M_HEAD | M_PUT | M_DELETE, web_vtx_route },
};

static int method_bit(const char *m) {
  if (!strcmp(m, "GET")) return M_GET;
  if (!strcmp(m, "HEAD")) return M_HEAD;
  if (!strcmp(m, "POST")) return M_POST;
  if (!strcmp(m, "PUT")) return M_PUT;
  if (!strcmp(m, "DELETE")) return M_DELETE;
  return 0;
}

static void allow_str(int m, char *out, size_t n) {
  static const char *const names[] = { "GET", "HEAD", "POST", "PUT", "DELETE" };
  out[0] = 0;
  for (int i = 0; i < 5; i++)
    if (m & (1 << i)) {
      size_t l = strlen(out);
      snprintf(out + l, n - l, "%s%s", l ? ", " : "", names[i]);
    }
}

void web_route(web_conn_t *c) {
  const char *p = c->path;
  int mb = method_bit(c->method);
  bool get = mb == M_GET || mb == M_HEAD;
  for (size_t i = 0; i < sizeof ROUTES / sizeof ROUTES[0]; i++) {
    if (strcmp(p, ROUTES[i].path)) continue;
    if (!(ROUTES[i].methods & mb)) {
      char allow[64];
      allow_str(ROUTES[i].methods, allow, sizeof allow);
      send_405(c, allow);
      return;
    }
    ROUTES[i].fn(c);
    return;
  }
  if (!strcmp(p, "/api/sd") || !strncmp(p, "/api/sd/", 8)) { r_sd(c); return; }
  if (!strncmp(p, "/api/songs/", 11)) { r_song(c); return; }
  if (!strncmp(p, "/api/", 5) || !strcmp(p, "/api")) { web_send_error(c, 404, "NotFound", "There is no such API route."); return; }
  if (!get) { send_405(c, "GET, HEAD"); return; }
  if (!strcmp(p, "/editor")) {
    char loc[1024];
    int k = snprintf(loc, sizeof loc, "Location: /editor/%s%s\r\n", c->query[0] ? "?" : "", c->query);
    if (k < 0 || k >= (int)sizeof loc) plat_strlcpy(loc, "Location: /editor/\r\n", sizeof loc);   /* a huge query: drop it */
    web_send_empty(c, 301, loc);
    return;
  }
  if (!strncmp(p, "/editor/", 8)) { r_editor(c); return; }
  if (!strcmp(p, "/favicon.ico")) {
    char path[700];
    web_stat_t st;
    join(path, sizeof path, A->paths.web, "favicon.ico");
    if (web_fs_stat(path, &st) == 0 && st.exists && !st.is_dir) web_send_file(c, path, NULL);
    else web_send_empty(c, 204, NULL);
    return;
  }
  static_file(c, A->paths.web, p, NULL);
}

/* ================================================================ start / stop */
/* our temp files left by a power cut (only ours: plat's *.tmp~ may be in flight in the game's writer) */
static void clean_temp(const char *dir, int depth) {
  if (depth > 8) return;
  entries_t e;
  if (collect(dir, COLLECT_CLEAN, &e) < 0) { entries_free(&e); return; }
  for (int i = 0; i < e.n; i++) {
    char p[1400];
    if (join(p, sizeof p, dir, e.v[i].name) != 0) continue;
    if (e.v[i].st.is_dir) clean_temp(p, depth + 1);
    else web_fs_unlink(p);
  }
  entries_free(&e);
}

void web_api_start(app_t *app) {
  A = app;
  if (!inited) {
    plat_mutex_init(&set_mu);
    plat_mutex_init(&shot_mu);
    plat_mutex_init(&scan_mu);
    plat_cond_init(&scan_cv);
    inited = true;
  }
  scan_running = false;
  web_stat_t st;
  if (web_fs_stat(app->paths.sd, &st) == 0 && st.exists && st.is_dir) {
    /* the editor looks for SCRIPTS/ (and sends tracks into StickTimeTracks/) */
    char p[700];
    if (join(p, sizeof p, app->paths.sd, "SCRIPTS/TOOLS/StickTimeTracks") == 0) {
      sd_res_t r;
      if (sd_resolve(app->paths.sd, "SCRIPTS/TOOLS/StickTimeTracks", &r) != RES_OK || !r.st.exists) plat_mkdirs(p);
    }
    clean_temp(app->paths.sd, 0);
  }
  char m[700];
  music_dir(m, sizeof m);
  clean_temp(m, 0);
}

void web_api_stop(void) {}
