/* Between web.c (the HTTP/1.1 server, responses, files) and web_api.c (the routes of
 * docs/WEB_API.md). Not for other modules: they use web.h. */
#ifndef WEB_INTERNAL_H
#define WEB_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "web.h"
#include "plat.h"

#define WEB_HEAD_MAX 16384           /* request line + headers */
#define WEB_IDLE_MS 15000            /* keep-alive idle, slow headers, stalled bodies */
#define WEB_MAX_HEADERS 100
#define WEB_DRAIN_MAX (1 << 20)      /* unread body we still read away to keep the connection */

/* ---------------------------------------------------------------- string builder (worker threads) */
typedef struct {
  char *p;
  size_t len, cap;
  bool oom;                          /* an allocation failed: the content is truncated */
} sb_t;

void sb_init(sb_t *b, size_t cap);
void sb_free(sb_t *b);
void sb_add(sb_t *b, const void *s, size_t n);
void sb_puts(sb_t *b, const char *s);
void sb_printf(sb_t *b, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
  ;
void sb_json_str(sb_t *b, const char *s);     /* a quoted, escaped JSON string ("" for NULL) */
void sb_json_num(sb_t *b, double v, int decimals);  /* null for NaN and infinities */

/* ---------------------------------------------------------------- one connection */
typedef struct web_conn {
  plat_sock s;
  app_t *app;
  char buf[WEB_HEAD_MAX + 1];
  size_t blen;                       /* bytes in buf */
  size_t rpos;                       /* next unread byte (after the head: the body, then the next request) */
  /* the request */
  char method[16];
  char target[WEB_HEAD_MAX];         /* path and query, NUL between them */
  const char *path, *query;          /* raw (percent-encoded); query "" when none */
  int minor;                         /* HTTP/1.<minor> */
  char host[256], origin[256], ctype[128], inm[512], accept_enc[128];
  bool has_host, has_origin;
  bool xst;                          /* X-StickTime: 1 (or X-StickTime-SD: 1) */
  bool has_len, te, expect100, sent100;
  int64_t clen, body_left;
  bool head_only;                    /* HEAD: headers only */
  bool keep_alive;
  bool responded;
  bool dead;                         /* the socket failed: no more output */
  int status;
} web_conn_t;

/* ASCII case-insensitive compares (FAT names, header names): locale-free, on every platform */
static inline int web_lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static inline int web_ncasecmp(const char *a, const char *b, size_t n) {
  for (; n; n--, a++, b++) {
    int d = web_lc((unsigned char)*a) - web_lc((unsigned char)*b);
    if (d || !*a) return d;
  }
  return 0;
}
static inline int web_casecmp(const char *a, const char *b) { return web_ncasecmp(a, b, (size_t)-1); }

/* the routes (web_api.c) */
void web_route(web_conn_t *c);
void web_api_start(app_t *app);      /* accept thread, before serving: folders, stale temp files */
void web_api_stop(void);
void web_note_sd_dirty(void);
bool web_stopping(void);
/* names the Host header may carry besides IPs and localhost (the old one stays valid until a reboot) */
void web_add_hostname(const char *name);
void web_hostname(char *out, size_t n);                        /* the configured one */
size_t web_max_upload(void);                                   /* web_config_t.max_upload (songs) */
bool web_is_device_name(const char *name);                     /* con, nul, com1.txt, ... */
/* Save settings like POST /api/settings does (under its lock; live keys reach the frame loop): the
 * keys that changed go into saved as JSON strings. 0 ok, -1 (errno). */
int  web_save_settings(const char *const *keys, const char *const *vals, int n, sb_t *saved);

/* VTX control (web_vtx.c) */
void web_vtx_route(web_conn_t *c);                             /* /api/vtx, /api/vtx/table */
void web_vtx_summary(sb_t *b, app_t *app);                     /* the "vtx" object of /api/status */

/* ---------------------------------------------------------------- requests and responses (web.c) */
/* Read body bytes: >0 count, 0 at the end, -1 on error/timeout (c->dead). Sends "100 Continue" first when asked. */
long web_read_body(web_conn_t *c, void *dst, size_t n);
/* The whole body, malloc'd and NUL-terminated. 0 ok, -2 over max (nothing read), -1 failed (c->dead). */
int  web_read_body_all(web_conn_t *c, size_t max, char **out, size_t *len);
/* Status line + headers. clen < 0: no Content-Length. extra: "Name: value\r\n" lines or NULL. */
int  web_send_head(web_conn_t *c, int status, const char *ctype, int64_t clen, const char *extra);
int  web_send_raw(web_conn_t *c, const void *p, size_t n);     /* nothing for HEAD; -1 on error */
void web_send_json(web_conn_t *c, int status, const char *json, size_t len);
void web_send_sb(web_conn_t *c, int status, sb_t *b);          /* JSON from a builder (500 if it ran out of memory) */
void web_send_error(web_conn_t *c, int status, const char *name, const char *msg);
void web_send_error_hdr(web_conn_t *c, int status, const char *name, const char *msg, const char *extra);
void web_send_ok(web_conn_t *c, int status);                   /* {"ok":true} */
void web_send_empty(web_conn_t *c, int status, const char *extra);   /* 204, 301, 304: no body */
/* A static file: ETag, Cache-Control: no-cache, 304 for a matching If-None-Match, a .gz sibling when
 * the client takes gzip. ctype NULL = by extension. 404 JSON when missing or a directory. */
void web_send_file(web_conn_t *c, const char *path, const char *ctype);
/* SSE: wait up to ms; 1 when the client went away (or the socket failed), else 0 */
int  web_wait_closed(web_conn_t *c, int ms);
const char *web_mime(const char *path);
bool web_etag_match(const char *inm, const char *etag);
bool web_host_ok(const char *host);
bool web_origin_ok(const char *origin, const char *host);

/* ---------------------------------------------------------------- files, without following links */
typedef struct {
  bool exists, is_dir, is_link;
  int64_t size;
  int64_t mtime_ms;
} web_stat_t;
int  web_fs_stat(const char *path, web_stat_t *st);            /* 0 (exists may be false), -1 error (errno) */
/* entries except . and ..; cb returns nonzero to stop (returned). -1 if the directory can't be read */
typedef int (*web_list_cb)(void *ud, const char *name, const web_stat_t *st);
int  web_fs_list(const char *dir, web_list_cb cb, void *ud);
int  web_fs_open_read(const char *path);                       /* fd, -1 (errno) */
int  web_fs_fstat(int fd, web_stat_t *st);                     /* of what was opened (a rename may follow the stat) */
int  web_fs_create_excl(const char *path);                     /* fd of a new file, -1 (errno, EEXIST) */
long web_fs_read(int fd, void *p, size_t n);
int  web_fs_write_all(int fd, const void *p, size_t n);        /* 0, -1 (errno, ENOSPC) */
int  web_fs_sync_close(int fd);                                /* fsync + close: 0, -1 (errno) */
void web_fs_close(int fd);
int  web_fs_unlink(const char *path);
int  web_fs_rmdir(const char *path);
int  web_fs_mkdir(const char *path);                           /* one level; -1 + EEXIST when there */
int  web_fs_rename(const char *from, const char *to);          /* replaces a file; long paths on Windows */
int  web_fs_true_name(const char *dir, const char *name, char *out, size_t outlen);  /* on-disk spelling */
int  web_fs_disk(const char *path, int64_t *free_bytes, int64_t *total_bytes);
bool web_is_temp_name(const char *name);                       /* ours (.st-tmp-*) or plat's (*.tmp~) */
void web_temp_name(char *out, size_t outlen);                  /* a fresh ".st-tmp-<pid>-<n>" */

/* JSON scanning (helper output, the updater's state file) */
const char *web_json_skip(const char *p, const char *end);     /* past one value (and spaces), NULL if invalid */
bool web_json_valid(const char *p, size_t len);                /* exactly one value */
/* the value of a top-level key of an object: [*v, *vend) */
bool web_json_find(const char *p, size_t len, const char *key, const char **v, const char **vend);
bool web_json_get_str(const char *p, size_t len, const char *key, char *out, size_t outlen);

#endif
