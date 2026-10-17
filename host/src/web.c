/* The HTTP/1.1 server: see web.h and docs/WEB_API.md. An accept thread hands every connection
 * to its own worker thread (capped; the rest get 503 at once). Requests are parsed strictly
 * (16 KB of headers, Content-Length bodies only), keep-alive with a 15 s idle timeout, and
 * nothing here ever waits on the frame loop: the routes (web_api.c) talk to it through app_t. */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "web_internal.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#define WEB_SHUT_WR SD_SEND
#else
#include <dirent.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#define WEB_SHUT_WR SHUT_WR
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* ================================================================ string builder */
void sb_init(sb_t *b, size_t cap) {
  if (!cap) cap = 256;
  b->p = malloc(cap);
  b->cap = b->p ? cap : 0;
  b->len = 0;
  b->oom = !b->p;
  if (b->p) b->p[0] = 0;
}

void sb_free(sb_t *b) {
  free(b->p);
  b->p = NULL;
  b->len = b->cap = 0;
}

static bool sb_grow(sb_t *b, size_t n) {
  if (b->oom) return false;
  if (b->len + n + 1 <= b->cap) return true;
  size_t nc = b->cap ? b->cap : 256;
  while (nc < b->len + n + 1) nc *= 2;
  char *np = realloc(b->p, nc);
  if (!np) { b->oom = true; return false; }
  b->p = np;
  b->cap = nc;
  return true;
}

void sb_add(sb_t *b, const void *s, size_t n) {
  if (!sb_grow(b, n)) return;
  memcpy(b->p + b->len, s, n);
  b->len += n;
  b->p[b->len] = 0;
}

void sb_puts(sb_t *b, const char *s) { sb_add(b, s, strlen(s)); }

void sb_printf(sb_t *b, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0 || !sb_grow(b, (size_t)n)) return;
  va_start(ap, fmt);
  vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap);
  va_end(ap);
  b->len += (size_t)n;
}

void sb_json_str(sb_t *b, const char *s) {
  if (!s) s = "";
  size_t n = web_json_escape(s, NULL, 0);
  if (!sb_grow(b, n + 2)) return;
  b->p[b->len++] = '"';
  web_json_escape(s, b->p + b->len, n + 1);
  b->len += n;
  b->p[b->len++] = '"';
  b->p[b->len] = 0;
}

void sb_json_num(sb_t *b, double v, int decimals) {
  if (isnan(v) || isinf(v)) sb_puts(b, "null");
  else sb_printf(b, "%.*f", decimals, v);
}

/* ================================================================ pure helpers */
/* length of the valid UTF-8 sequence at p (RFC 3629: no overlongs, surrogates or > U+10FFFF), 0 if invalid */
static int utf8_seq(const unsigned char *p, unsigned *cp) {
  unsigned c = p[0];
  if (c < 0x80) { *cp = c; return 1; }
  if (c >= 0xC2 && c <= 0xDF) {
    if ((p[1] & 0xC0) != 0x80) return 0;
    *cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
    return 2;
  }
  if (c >= 0xE0 && c <= 0xEF) {
    unsigned lo = 0x80, hi = 0xBF;
    if (c == 0xE0) lo = 0xA0;
    if (c == 0xED) hi = 0x9F;
    if (p[1] < lo || p[1] > hi || (p[2] & 0xC0) != 0x80) return 0;
    *cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    return 3;
  }
  if (c >= 0xF0 && c <= 0xF4) {
    unsigned lo = 0x80, hi = 0xBF;
    if (c == 0xF0) lo = 0x90;
    if (c == 0xF4) hi = 0x8F;
    if (p[1] < lo || p[1] > hi || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) return 0;
    *cp = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    return 4;
  }
  return 0;
}

static int hexval(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

int web_url_decode(char *s) {
  if (!s) return -1;
  char *w = s;
  for (const char *r = s; *r; r++) {
    if (*r != '%') { *w++ = *r; continue; }
    int h = hexval((unsigned char)r[1]), l = h < 0 ? -1 : hexval((unsigned char)r[2]);
    if (h < 0 || l < 0) return -1;
    int v = h * 16 + l;
    if (v == 0) return -1;
    *w++ = (char)v;
    r += 2;
  }
  *w = 0;
  return 0;
}

size_t web_json_escape(const char *s, char *out, size_t outlen) {
  size_t need = 0, w = 0;
  bool full = false;
  const unsigned char *p = (const unsigned char *)(s ? s : "");
  while (*p) {
    char tmp[8];
    size_t tl = 1, adv = 1;
    unsigned c = *p, cp;
    switch (c) {
      case '"': memcpy(tmp, "\\\"", 2); tl = 2; break;
      case '\\': memcpy(tmp, "\\\\", 2); tl = 2; break;
      case '\n': memcpy(tmp, "\\n", 2); tl = 2; break;
      case '\r': memcpy(tmp, "\\r", 2); tl = 2; break;
      case '\t': memcpy(tmp, "\\t", 2); tl = 2; break;
      case '\b': memcpy(tmp, "\\b", 2); tl = 2; break;
      case '\f': memcpy(tmp, "\\f", 2); tl = 2; break;
      default:
        /* < > & escaped too, so a JSON body can never be read as HTML */
        if (c < 0x20 || c == 0x7f || c == '<' || c == '>' || c == '&') {
          snprintf(tmp, sizeof tmp, "\\u%04x", c);
          tl = 6;
        } else if (c < 0x80) {
          tmp[0] = (char)c;
        } else {
          int n = utf8_seq(p, &cp);
          if (!n) {                             /* JSON must be UTF-8: invalid bytes become U+FFFD */
            memcpy(tmp, "\\ufffd", 6);
            tl = 6;
          } else if (cp == 0x2028 || cp == 0x2029) {
            snprintf(tmp, sizeof tmp, "\\u%04x", cp);
            tl = 6;
            adv = 3;
          } else {
            memcpy(tmp, p, (size_t)n);
            tl = adv = (size_t)n;
          }
        }
    }
    if (!full && out && w + tl < outlen) {
      memcpy(out + w, tmp, tl);
      w += tl;
    } else {
      full = true;                              /* never half an escape */
    }
    need += tl;
    p += adv;
  }
  if (out && outlen) out[w] = 0;
  return need;
}

static bool is_device_name(const char *name);

/* one decoded path component: the FAT and JSON-safe rules of WEB_API.md */
static bool component_ok(const char *c) {
  size_t n = strlen(c);
  if (!n || n > 255) return false;
  if (!strcmp(c, ".") || !strcmp(c, "..")) return false;
  if (c[n - 1] == '.' || c[n - 1] == ' ') return false;   /* vfat drops them: "c1.txt." would alias "c1.txt" */
  for (const unsigned char *p = (const unsigned char *)c; *p;) {
    unsigned cp;
    if (*p < 0x20 || *p == 0x7f || strchr("\\/\"*:<>?|", *p)) return false;
    int k = utf8_seq(p, &cp);
    if (!k) return false;
    p += k;
  }
  return true;
}

int web_clean_path(const char *in, char *out, size_t outlen) {
  if (!in || !out || !outlen) return -1;
  out[0] = 0;
  if (*in == '/') in++;
  size_t inlen = strlen(in);
  if (inlen > 1024 * 3) return -1;
  size_t w = 0;
  const char *p = in;
  for (;;) {
    const char *slash = strchr(p, '/');
    size_t rl = slash ? (size_t)(slash - p) : strlen(p);
    if (rl == 0) {
      /* empty: only the root ("") or a trailing slash */
      if (!slash && (p == in || p[-1] == '/')) break;
      return -1;
    }
    if (rl > 255 * 3) return -1;
    char comp[255 * 3 + 1];
    memcpy(comp, p, rl);
    comp[rl] = 0;
    if (web_url_decode(comp) != 0 || !component_ok(comp)) return -1;
    size_t cl = strlen(comp);
    if (w + (w ? 1 : 0) + cl > 1024 || w + (w ? 1 : 0) + cl + 1 > outlen) return -1;
    if (w) out[w++] = '/';
    memcpy(out + w, comp, cl);
    w += cl;
    out[w] = 0;
    if (!slash) break;
    p = slash + 1;
    if (!*p) break;                               /* trailing slash */
  }
  return 0;
}

/* con, prn, aux, nul, com0-9, lpt0-9, with any extension: Windows can't open them as files */
static bool is_device_name(const char *name) {
  char base[8];
  size_t n = 0;
  while (name[n] && name[n] != '.' && n < sizeof base - 1) { base[n] = (char)(name[n] | 0x20); n++; }
  if (name[n] && name[n] != '.') return false;
  base[n] = 0;
  if (!strcmp(base, "con") || !strcmp(base, "prn") || !strcmp(base, "aux") || !strcmp(base, "nul")) return true;
  if (n == 4 && (!strncmp(base, "com", 3) || !strncmp(base, "lpt", 3)) && base[3] >= '0' && base[3] <= '9') return true;
  return false;
}
bool web_is_device_name(const char *name) { return is_device_name(name); }

/* ================================================================ JSON scanning */
static const char *jws(const char *p, const char *e) {
  while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  return p;
}

static const char *jstr(const char *p, const char *e) {
  if (p >= e || *p != '"') return NULL;
  for (p++; p < e; p++) {
    unsigned char c = (unsigned char)*p;
    if (c == '"') return p + 1;
    if (c < 0x20) return NULL;
    if (c == '\\') {
      if (++p >= e) return NULL;
      if (*p == 'u') {
        for (int i = 1; i <= 4; i++) if (p + i >= e || hexval((unsigned char)p[i]) < 0) return NULL;
        p += 4;
      } else if (!strchr("\"\\/bfnrt", *p)) {
        return NULL;
      }
    }
  }
  return NULL;
}

static const char *jnum(const char *p, const char *e) {
  if (p < e && *p == '-') p++;
  if (p >= e) return NULL;
  if (*p == '0') p++;
  else if (*p >= '1' && *p <= '9') { while (p < e && *p >= '0' && *p <= '9') p++; }
  else return NULL;
  if (p < e && *p == '.') {
    p++;
    if (p >= e || *p < '0' || *p > '9') return NULL;
    while (p < e && *p >= '0' && *p <= '9') p++;
  }
  if (p < e && (*p == 'e' || *p == 'E')) {
    p++;
    if (p < e && (*p == '+' || *p == '-')) p++;
    if (p >= e || *p < '0' || *p > '9') return NULL;
    while (p < e && *p >= '0' && *p <= '9') p++;
  }
  return p;
}

/* past one value, without the spaces after it */
static const char *jval(const char *p, const char *e, int depth) {
  p = jws(p, e);
  if (p >= e || depth > 64) return NULL;
  switch (*p) {
    case '{':
    case '[': {
      char close = *p == '{' ? '}' : ']';
      bool obj = *p == '{';
      p = jws(p + 1, e);
      if (p < e && *p == close) return p + 1;
      for (;;) {
        if (obj) {
          p = jstr(jws(p, e), e);
          if (!p) return NULL;
          p = jws(p, e);
          if (p >= e || *p != ':') return NULL;
          p++;
        }
        p = jval(p, e, depth + 1);
        if (!p) return NULL;
        p = jws(p, e);
        if (p < e && *p == ',') { p++; continue; }
        if (p < e && *p == close) return p + 1;
        return NULL;
      }
    }
    case '"': return jstr(p, e);
    case 't': return e - p >= 4 && !memcmp(p, "true", 4) ? p + 4 : NULL;
    case 'f': return e - p >= 5 && !memcmp(p, "false", 5) ? p + 5 : NULL;
    case 'n': return e - p >= 4 && !memcmp(p, "null", 4) ? p + 4 : NULL;
    default: return jnum(p, e);
  }
}

const char *web_json_skip(const char *p, const char *end) {
  const char *q = jval(p, end, 0);
  return q ? jws(q, end) : NULL;
}

bool web_json_valid(const char *p, size_t len) {
  if (!p) return false;
  const char *q = web_json_skip(p, p + len);
  return q && q == p + len;
}

bool web_json_find(const char *p, size_t len, const char *key, const char **v, const char **vend) {
  const char *e = p + len;
  p = jws(p, e);
  if (p >= e || *p != '{') return false;
  p = jws(p + 1, e);
  if (p < e && *p == '}') return false;
  size_t kl = strlen(key);
  for (;;) {
    const char *k = jws(p, e), *ke = jstr(k, e);
    if (!ke) return false;
    p = jws(ke, e);
    if (p >= e || *p != ':') return false;
    const char *vs = jws(p + 1, e), *ve = jval(vs, e, 1);
    if (!ve) return false;
    if ((size_t)(ke - k) == kl + 2 && !memcmp(k + 1, key, kl)) {
      *v = vs;
      *vend = ve;
      return true;
    }
    p = jws(ve, e);
    if (p < e && *p == ',') { p++; continue; }
    return false;
  }
}

static size_t put_utf8(char *o, unsigned cp) {
  if (cp < 0x80) { o[0] = (char)cp; return 1; }
  if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) {
    o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

bool web_json_get_str(const char *p, size_t len, const char *key, char *out, size_t outlen) {
  const char *v, *ve;
  if (!outlen || !web_json_find(p, len, key, &v, &ve) || *v != '"') return false;
  size_t w = 0;
  for (const char *q = v + 1; q < ve - 1;) {
    char tmp[4];
    size_t tl = 1;
    if (*q != '\\') { tmp[0] = *q++; }
    else {
      q++;
      switch (*q) {
        case 'n': tmp[0] = '\n'; break;
        case 'r': tmp[0] = '\r'; break;
        case 't': tmp[0] = '\t'; break;
        case 'b': tmp[0] = '\b'; break;
        case 'f': tmp[0] = '\f'; break;
        case 'u': {
          unsigned cp = 0;
          for (int i = 1; i <= 4; i++) cp = cp * 16 + (unsigned)hexval((unsigned char)q[i]);
          q += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF && q + 6 < ve && q[1] == '\\' && q[2] == 'u') {
            unsigned lo = 0;
            for (int i = 3; i <= 6; i++) lo = lo * 16 + (unsigned)hexval((unsigned char)q[i]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); q += 6; }
          }
          if ((cp >= 0xD800 && cp <= 0xDFFF) || cp == 0) cp = 0xFFFD;
          tl = put_utf8(tmp, cp);
          break;
        }
        default: tmp[0] = *q;
      }
      q++;
    }
    if (w + tl >= outlen) break;
    memcpy(out + w, tmp, tl);
    w += tl;
  }
  out[w] = 0;
  return true;
}

/* ================================================================ files */
#ifdef _WIN32
static wchar_t *wpath(const char *p) {
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, -1, NULL, 0);
  if (n <= 0) { errno = EINVAL; return NULL; }
  wchar_t *w = malloc(sizeof(wchar_t) * (size_t)n);
  if (!w) { errno = ENOMEM; return NULL; }
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p, -1, w, n);
  for (wchar_t *c = w; *c; c++) if (*c == L'/') *c = L'\\';
  /* FAT allows 255-character names: past MAX_PATH Windows needs the \\?\ form of the full path
   * (our paths never hold "." or ".." components, which that form doesn't resolve) */
  if (n > 200 && wcsncmp(w, L"\\\\", 2)) {
    DWORD need = GetFullPathNameW(w, 0, NULL, NULL);
    wchar_t *full = need ? malloc(sizeof(wchar_t) * (need + 4)) : NULL;
    if (full && GetFullPathNameW(w, need, full + 4, NULL) > 0) {
      memcpy(full, L"\\\\?\\", 4 * sizeof(wchar_t));
      free(w);
      return full;
    }
    free(full);
  }
  return w;
}

static void win_errno(void) {
  switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: case ERROR_BAD_PATHNAME: errno = ENOENT; break;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: errno = EACCES; break;
    case ERROR_DIR_NOT_EMPTY: errno = ENOTEMPTY; break;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: errno = EEXIST; break;
    case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: errno = ENOSPC; break;
    case ERROR_WRITE_PROTECT: errno = EROFS; break;
    case ERROR_FILENAME_EXCED_RANGE: errno = ENAMETOOLONG; break;
    case ERROR_DIRECTORY: errno = ENOTDIR; break;
    default: errno = EIO;
  }
}

static int64_t ft_ms(FILETIME ft) {
  uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  return (int64_t)(t / 10000ULL) - 11644473600000LL;
}

int web_fs_stat(const char *path, web_stat_t *st) {
  memset(st, 0, sizeof *st);
  wchar_t *w = wpath(path);
  if (!w) return -1;
  WIN32_FILE_ATTRIBUTE_DATA d;
  BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &d);
  free(w);
  if (!ok) {
    DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND || e == ERROR_INVALID_NAME || e == ERROR_DIRECTORY) return 0;
    win_errno();
    return -1;
  }
  st->exists = true;
  st->is_link = (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
  st->is_dir = !st->is_link && (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  st->size = st->is_dir ? 0 : (int64_t)(((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow);
  st->mtime_ms = ft_ms(d.ftLastWriteTime);
  return 0;
}

int web_fs_list(const char *dir, web_list_cb cb, void *ud) {
  size_t n = strlen(dir);
  char *pat = malloc(n + 3);
  if (!pat) return -1;
  memcpy(pat, dir, n);
  memcpy(pat + n, "/*", 3);
  wchar_t *w = wpath(pat);
  free(pat);
  if (!w) return -1;
  WIN32_FIND_DATAW d;
  HANDLE h = FindFirstFileW(w, &d);
  free(w);
  if (h == INVALID_HANDLE_VALUE) { win_errno(); return -1; }
  int r = 0;
  do {
    if (!wcscmp(d.cFileName, L".") || !wcscmp(d.cFileName, L"..")) continue;
    char name[1024];
    if (!WideCharToMultiByte(CP_UTF8, 0, d.cFileName, -1, name, sizeof name, NULL, NULL)) continue;
    web_stat_t st;
    st.exists = true;
    st.is_link = (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    st.is_dir = !st.is_link && (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    st.size = st.is_dir ? 0 : (int64_t)(((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow);
    st.mtime_ms = ft_ms(d.ftLastWriteTime);
    r = cb(ud, name, &st);
  } while (!r && FindNextFileW(h, &d));
  FindClose(h);
  return r;
}

int web_fs_open_read(const char *path) {
  wchar_t *w = wpath(path);
  if (!w) return -1;
  /* FILE_SHARE_DELETE: a PUT may replace the file while it is being sent, as on Linux */
  HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, NULL);
  free(w);
  if (h == INVALID_HANDLE_VALUE) { win_errno(); return -1; }
  int fd = _open_osfhandle((intptr_t)h, _O_RDONLY | _O_BINARY);
  if (fd < 0) CloseHandle(h);
  return fd;
}

int web_fs_create_excl(const char *path) {
  wchar_t *w = wpath(path);
  if (!w) return -1;
  int fd = _wopen(w, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
  free(w);
  return fd;
}

int web_fs_fstat(int fd, web_stat_t *st) {
  memset(st, 0, sizeof *st);
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  BY_HANDLE_FILE_INFORMATION i;
  if (h == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h, &i)) { win_errno(); return -1; }
  st->exists = true;
  st->is_dir = (i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  st->size = st->is_dir ? 0 : (int64_t)(((uint64_t)i.nFileSizeHigh << 32) | i.nFileSizeLow);
  st->mtime_ms = ft_ms(i.ftLastWriteTime);
  return 0;
}

long web_fs_read(int fd, void *p, size_t n) {
  if (n > 1u << 30) n = 1u << 30;
  return _read(fd, p, (unsigned)n);
}

int web_fs_write_all(int fd, const void *p, size_t n) {
  const char *c = p;
  while (n) {
    unsigned k = n > (1u << 30) ? (1u << 30) : (unsigned)n;
    int r = _write(fd, c, k);
    if (r <= 0) { if (!errno) errno = EIO; return -1; }
    c += r;
    n -= (size_t)r;
  }
  return 0;
}

int web_fs_sync_close(int fd) {
  int ok = _commit(fd) == 0;
  int e = errno;
  if (_close(fd) != 0) ok = 0;
  if (!ok) errno = e ? e : EIO;
  return ok ? 0 : -1;
}

void web_fs_close(int fd) { if (fd >= 0) _close(fd); }

int web_fs_unlink(const char *path) {
  wchar_t *w = wpath(path);
  if (!w) return -1;
  BOOL ok = DeleteFileW(w);
  if (!ok) win_errno();
  free(w);
  return ok ? 0 : -1;
}

int web_fs_rmdir(const char *path) {
  wchar_t *w = wpath(path);
  if (!w) return -1;
  BOOL ok = RemoveDirectoryW(w);
  if (!ok) win_errno();
  free(w);
  return ok ? 0 : -1;
}

int web_fs_mkdir(const char *path) {
  wchar_t *w = wpath(path);
  if (!w) return -1;
  BOOL ok = CreateDirectoryW(w, NULL);
  if (!ok) win_errno();
  free(w);
  return ok ? 0 : -1;
}

int web_fs_rename(const char *from, const char *to) {
  wchar_t *a = wpath(from), *b = a ? wpath(to) : NULL;
  BOOL ok = FALSE;
  DWORD e = 0;
  /* two replaces of one file at once, or a reader without FILE_SHARE_DELETE, fail for a moment
   * here (never on Linux, where rename() is atomic): retry briefly */
  for (int i = 0; a && b && i < 50; i++) {
    ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    e = ok ? 0 : GetLastError();
    if (ok || (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION)) break;
    Sleep(10);
  }
  if (!ok && a && b) { SetLastError(e); win_errno(); }
  free(a);
  free(b);
  return ok ? 0 : -1;
}

int web_fs_true_name(const char *dir, const char *name, char *out, size_t outlen) {
  size_t a = strlen(dir), b = strlen(name);
  char *p = malloc(a + b + 2);
  if (!p) return -1;
  memcpy(p, dir, a);
  p[a] = '/';
  memcpy(p + a + 1, name, b + 1);
  wchar_t *w = wpath(p);
  free(p);
  if (!w) return -1;
  WIN32_FIND_DATAW d;
  HANDLE h = FindFirstFileW(w, &d);
  free(w);
  if (h == INVALID_HANDLE_VALUE) return -1;
  FindClose(h);
  return WideCharToMultiByte(CP_UTF8, 0, d.cFileName, -1, out, (int)outlen, NULL, NULL) ? 0 : -1;
}

int web_fs_disk(const char *path, int64_t *free_bytes, int64_t *total_bytes) {
  wchar_t *w = wpath(path);
  if (!w) return -1;
  ULARGE_INTEGER fr, tot;
  BOOL ok = GetDiskFreeSpaceExW(w, &fr, &tot, NULL);
  free(w);
  if (!ok) { win_errno(); return -1; }
  *free_bytes = (int64_t)fr.QuadPart;
  *total_bytes = (int64_t)tot.QuadPart;
  return 0;
}

static long my_pid(void) { return (long)_getpid(); }
#else
static int64_t st_ms(const struct stat *s) {
  return (int64_t)s->st_mtim.tv_sec * 1000 + s->st_mtim.tv_nsec / 1000000;
}

int web_fs_stat(const char *path, web_stat_t *st) {
  memset(st, 0, sizeof *st);
  struct stat s;
  if (lstat(path, &s) != 0) {
    if (errno == ENOENT || errno == ENOTDIR) return 0;
    return -1;
  }
  st->exists = true;
  st->is_link = S_ISLNK(s.st_mode);
  st->is_dir = S_ISDIR(s.st_mode);
  st->size = st->is_dir ? 0 : (int64_t)s.st_size;
  st->mtime_ms = st_ms(&s);
  return 0;
}

int web_fs_list(const char *dir, web_list_cb cb, void *ud) {
  DIR *d = opendir(dir);
  if (!d) return -1;
  struct dirent *e;
  int r = 0;
  size_t dl = strlen(dir);
  char *full = malloc(dl + 1024);
  if (!full) { closedir(d); return -1; }
  while (!r && (e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    size_t nl = strlen(e->d_name);
    if (nl > 1000) continue;
    memcpy(full, dir, dl);
    full[dl] = '/';
    memcpy(full + dl + 1, e->d_name, nl + 1);
    struct stat s;
    if (lstat(full, &s) != 0) continue;
    web_stat_t st = { true, S_ISDIR(s.st_mode), S_ISLNK(s.st_mode), S_ISDIR(s.st_mode) ? 0 : (int64_t)s.st_size, st_ms(&s) };
    r = cb(ud, e->d_name, &st);
  }
  free(full);
  closedir(d);
  return r;
}

int web_fs_open_read(const char *path) { return open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW); }

int web_fs_fstat(int fd, web_stat_t *st) {
  memset(st, 0, sizeof *st);
  struct stat s;
  if (fstat(fd, &s) != 0) return -1;
  st->exists = true;
  st->is_dir = S_ISDIR(s.st_mode);
  st->size = st->is_dir ? 0 : (int64_t)s.st_size;
  st->mtime_ms = st_ms(&s);
  return 0;
}
int web_fs_create_excl(const char *path) { return open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644); }

long web_fs_read(int fd, void *p, size_t n) {
  for (;;) {
    ssize_t r = read(fd, p, n);
    if (r < 0 && errno == EINTR) continue;
    return (long)r;
  }
}

int web_fs_write_all(int fd, const void *p, size_t n) {
  const char *c = p;
  while (n) {
    ssize_t r = write(fd, c, n);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) { if (r == 0) errno = EIO; return -1; }
    c += r;
    n -= (size_t)r;
  }
  return 0;
}

int web_fs_sync_close(int fd) {
  int ok = fsync(fd) == 0;
  int e = errno;
  if (close(fd) != 0) { if (ok) e = errno; ok = 0; }
  if (!ok) errno = e ? e : EIO;
  return ok ? 0 : -1;
}

void web_fs_close(int fd) { if (fd >= 0) close(fd); }
int web_fs_unlink(const char *path) { return unlink(path); }
int web_fs_rmdir(const char *path) { return rmdir(path); }
int web_fs_mkdir(const char *path) { return mkdir(path, 0755); }
int web_fs_rename(const char *from, const char *to) { return rename(from, to); }

typedef struct { const char *name; char *out; size_t outlen; int found; } truename_t;
static int truename_cb(void *ud, const char *name, const web_stat_t *st) {
  truename_t *t = ud;
  if (!strcmp(name, t->name)) { plat_strlcpy(t->out, name, t->outlen); t->found = 2; return 1; }
  if (!web_casecmp(name, t->name) && (!t->found || strcmp(name, t->out) < 0)) {
    plat_strlcpy(t->out, name, t->outlen);
    t->found = 1;
  }
  return 0;
}

int web_fs_true_name(const char *dir, const char *name, char *out, size_t outlen) {
  truename_t t = { name, out, outlen, 0 };
  if (web_fs_list(dir, truename_cb, &t) < 0) return -1;
  return t.found ? 0 : -1;
}

int web_fs_disk(const char *path, int64_t *free_bytes, int64_t *total_bytes) {
  struct statvfs v;
  if (statvfs(path, &v) != 0) return -1;
  *free_bytes = (int64_t)v.f_bavail * (int64_t)v.f_frsize;
  *total_bytes = (int64_t)v.f_blocks * (int64_t)v.f_frsize;
  return 0;
}

static long my_pid(void) { return (long)getpid(); }
#endif

bool web_is_temp_name(const char *name) {
  size_t n = strlen(name);
  return !strncmp(name, ".st-tmp-", 8) || (n >= 5 && !strcmp(name + n - 5, ".tmp~"));
}

void web_temp_name(char *out, size_t outlen) {
  static int counter;
  int k = __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED);
  snprintf(out, outlen, ".st-tmp-%ld-%d", my_pid(), k);
}

/* ================================================================ server state */
static struct {
  bool inited, running;
  plat_mutex mu;                 /* names, counters */
  plat_cond cv;
  app_t *app;
  web_config_t cfg;
  char names[2][64];
  plat_sock ls;
  plat_thread th;
  int stop;                      /* atomic */
  int nworkers, nreject;
} W;

static int sd_dirty;             /* atomic */

void web_note_sd_dirty(void) { __atomic_store_n(&sd_dirty, 1, __ATOMIC_SEQ_CST); }
bool web_take_sd_dirty(void) { return __atomic_exchange_n(&sd_dirty, 0, __ATOMIC_SEQ_CST) != 0; }
bool web_stopping(void) { return __atomic_load_n(&W.stop, __ATOMIC_SEQ_CST) != 0; }

static void web_init_once(void) {
  if (W.inited) return;
  plat_mutex_init(&W.mu);
  plat_cond_init(&W.cv);
  W.inited = true;
}

void web_hostname(char *out, size_t n) {
  web_init_once();
  plat_mutex_lock(&W.mu);
  plat_strlcpy(out, W.names[0][0] ? W.names[0] : "sticktime", n);
  plat_mutex_unlock(&W.mu);
}

void web_add_hostname(const char *name) {
  web_init_once();
  if (!name || !*name || strlen(name) >= sizeof W.names[0]) return;
  plat_mutex_lock(&W.mu);
  if (web_casecmp(name, W.names[0]) && web_casecmp(name, W.names[1])) {
    if (!W.names[0][0]) plat_strlcpy(W.names[0], name, sizeof W.names[0]);
    else plat_strlcpy(W.names[1], name, sizeof W.names[1]);
  }
  plat_mutex_unlock(&W.mu);
}

/* ================================================================ Host / Origin checks */
static bool ipv4_literal(const char *s, size_t n) {
  int parts = 0;
  size_t i = 0;
  while (i < n) {
    size_t d = 0;
    int v = 0;
    while (i < n && s[i] >= '0' && s[i] <= '9' && d < 4) { v = v * 10 + (s[i] - '0'); i++; d++; }
    if (!d || d > 3 || v > 255) return false;
    parts++;
    if (i == n) break;
    if (s[i] != '.' || parts == 4) return false;
    i++;
    if (i == n) return false;
  }
  return parts == 4;
}

static bool port_ok(const char *p, size_t n) {
  if (!n || n > 5) return false;
  long v = 0;
  for (size_t i = 0; i < n; i++) {
    if (p[i] < '0' || p[i] > '9') return false;
    v = v * 10 + (p[i] - '0');
  }
  return v <= 65535;
}

static bool name_is(const char *s, size_t n, const char *name, const char *suffix) {
  size_t a = strlen(name), b = suffix ? strlen(suffix) : 0;
  if (!a || n != a + b) return false;
  return !web_ncasecmp(s, name, a) && (!b || !web_ncasecmp(s + a, suffix, b));
}

bool web_host_ok(const char *host) {
  if (!host) return false;
  size_t n = strlen(host);
  if (!n || n > 255) return false;
  const char *h = host;
  size_t hl;
  if (h[0] == '[') {
    const char *rb = memchr(h, ']', n);
    if (!rb) return false;
    hl = (size_t)(rb - h) + 1;
    bool colon = false;
    for (const char *p = h + 1; p < rb; p++) {
      if (*p == ':') colon = true;
      else if (!(hexval((unsigned char)*p) >= 0 || *p == '.')) return false;
    }
    if (!colon || hl < 4 || hl > 47) return false;
    if (hl < n && (h[hl] != ':' || !port_ok(h + hl + 1, n - hl - 1))) return false;
    return true;
  }
  const char *colon = memchr(h, ':', n);
  hl = colon ? (size_t)(colon - h) : n;
  if (colon && !port_ok(colon + 1, n - hl - 1)) return false;
  if (hl && h[hl - 1] == '.') hl--;               /* "sticktime.local." is the same name */
  if (!hl) return false;
  if (ipv4_literal(h, hl)) return true;
  if (name_is(h, hl, "localhost", NULL)) return true;
  bool ok = false;
  web_init_once();
  plat_mutex_lock(&W.mu);
  for (int i = 0; i < 2 && !ok; i++)
    ok = W.names[i][0] && (name_is(h, hl, W.names[i], NULL) || name_is(h, hl, W.names[i], ".local"));
  plat_mutex_unlock(&W.mu);
  return ok;
}

/* "host[:port]" without the scheme's default port */
static size_t hostport_norm(const char *s, size_t n, bool https, char *out, size_t outlen) {
  const char *dp = https ? ":443" : ":80";
  size_t dl = strlen(dp);
  if (n > dl && !strncmp(s + n - dl, dp, dl) && s[n - dl - 1] != ':') n -= dl;
  if (n >= outlen) return 0;
  for (size_t i = 0; i < n; i++) out[i] = (char)((s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i]);
  out[n] = 0;
  return n;
}

bool web_origin_ok(const char *origin, const char *host) {
  if (!origin || !host) return false;
  const char *sep = strstr(origin, "://");
  if (!sep) return false;
  size_t sl = (size_t)(sep - origin);
  bool https;
  if (sl == 4 && !web_ncasecmp(origin, "http", 4)) https = false;
  else if (sl == 5 && !web_ncasecmp(origin, "https", 5)) https = true;
  else return false;
  const char *hp = sep + 3;
  size_t hn = strlen(hp);
  if (hn && hp[hn - 1] == '/') hn--;
  if (!hn || memchr(hp, '/', hn) || memchr(hp, '@', hn)) return false;
  char a[300], b[300];
  if (!hostport_norm(hp, hn, https, a, sizeof a)) return false;
  if (!hostport_norm(host, strlen(host), false, b, sizeof b)) return false;
  return !strcmp(a, b);
}

/* ================================================================ sockets */
static int wait_readable(plat_sock s, int ms) {
#ifdef _WIN32
  fd_set r;
  FD_ZERO(&r);
  FD_SET(s, &r);
  struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
  return select(0, &r, NULL, NULL, &tv);
#else
  struct pollfd p = { s, POLLIN, 0 };
  int r = poll(&p, 1, ms);
  if (r < 0 && errno == EINTR) return 0;
  return r;
#endif
}

static void no_inherit(plat_sock s) {
#ifdef _WIN32
  /* _popen children (helpers) would otherwise keep our sockets open */
  SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
#else
  (void)s;
#endif
}

static int send_all(web_conn_t *c, const void *p, size_t n) {
  if (c->dead) return -1;
  const char *b = p;
  while (n) {
    int k = (int)(n > (1u << 20) ? (1u << 20) : n);
    int r = (int)send(c->s, b, k, MSG_NOSIGNAL);
    if (r <= 0) { c->dead = true; return -1; }
    b += r;
    n -= (size_t)r;
  }
  return 0;
}

/* recv with the idle timeout; >0 bytes, 0 closed, -1 error/timeout/stopping */
static long recv_some(web_conn_t *c, void *p, size_t n, int timeout_ms) {
  int64_t end = plat_now_us() + (int64_t)timeout_ms * 1000;
  for (;;) {
    if (web_stopping()) return -1;
    int left = (int)((end - plat_now_us()) / 1000);
    if (left <= 0) return -1;
    int w = wait_readable(c->s, left < 250 ? left : 250);
    if (w < 0) return -1;
    if (w == 0) continue;
    int k = (int)(n > (1u << 20) ? (1u << 20) : n);
    int r = (int)recv(c->s, p, k, 0);
    return r < 0 ? -1 : r;
  }
}

int web_wait_closed(web_conn_t *c, int ms) {
  if (c->dead) return 1;
  int w = wait_readable(c->s, ms);
  if (w < 0) return 1;
  if (w == 0) return 0;
  char tmp[512];
  int r = (int)recv(c->s, tmp, sizeof tmp, 0);
  if (r <= 0) { c->dead = true; return 1; }
  return 0;
}

/* half-close, read what the client still sends (so it sees our answer, not a reset), then close */
static void close_conn(plat_sock s, int linger_ms, int64_t max_bytes) {
  shutdown(s, WEB_SHUT_WR);
  int64_t end = plat_now_us() + (int64_t)linger_ms * 1000, got = 0;
  char tmp[16384];
  while (got < max_bytes) {
    int left = (int)((end - plat_now_us()) / 1000);
    if (left <= 0 || wait_readable(s, left) <= 0) break;
    int r = (int)recv(s, tmp, sizeof tmp, 0);
    if (r <= 0) break;
    got += r;
  }
  plat_sock_close(s);
}

/* ================================================================ responses */
static const char *reason(int st) {
  switch (st) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 417: return "Expectation Failed";
    case 421: return "Misdirected Request";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
    case 507: return "Insufficient Storage";
    default: return "Unknown";
  }
}

int web_send_head(web_conn_t *c, int status, const char *ctype, int64_t clen, const char *extra) {
  if (c->dead || c->responded) return -1;
  c->responded = true;
  c->status = status;
  bool bodyless = status == 204 || status == 304;
  /* a big unread body: closing is cheaper than reading it away */
  if (c->body_left > WEB_DRAIN_MAX || (c->body_left > 0 && c->expect100 && !c->sent100)) c->keep_alive = false;
  if (clen < 0 && !bodyless) c->keep_alive = false;
  if (web_stopping()) c->keep_alive = false;
  char h[4096];
  int n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nServer: sticktime-pi\r\n", status, reason(status));
  if (ctype) n += snprintf(h + n, sizeof h - (size_t)n, "Content-Type: %s\r\n", ctype);
  if (clen >= 0 && !bodyless) n += snprintf(h + n, sizeof h - (size_t)n, "Content-Length: %lld\r\n", (long long)clen);
  n += snprintf(h + n, sizeof h - (size_t)n, "X-Content-Type-Options: nosniff\r\n%sConnection: %s\r\n\r\n",
                extra ? extra : "", c->keep_alive ? "keep-alive" : "close");
  if (n >= (int)sizeof h) { c->dead = true; return -1; }
  return send_all(c, h, (size_t)n);
}

int web_send_raw(web_conn_t *c, const void *p, size_t n) {
  if (c->head_only) return 0;
  return send_all(c, p, n);
}

static void send_json_x(web_conn_t *c, int status, const char *json, size_t len, const char *extra) {
  char x[512];
  snprintf(x, sizeof x, "Cache-Control: no-store\r\n%s", extra ? extra : "");
  if (web_send_head(c, status, "application/json; charset=utf-8", (int64_t)len, x) == 0) web_send_raw(c, json, len);
}

void web_send_json(web_conn_t *c, int status, const char *json, size_t len) { send_json_x(c, status, json, len, NULL); }

void web_send_error_hdr(web_conn_t *c, int status, const char *name, const char *msg, const char *extra) {
  sb_t b;
  sb_init(&b, 256);
  sb_puts(&b, "{\"error\":");
  sb_json_str(&b, name);
  sb_puts(&b, ",\"message\":");
  sb_json_str(&b, msg);
  sb_puts(&b, "}");
  if (b.oom) send_json_x(c, status, "{\"error\":\"Internal\",\"message\":\"out of memory\"}", 47, extra);
  else send_json_x(c, status, b.p, b.len, extra);
  sb_free(&b);
}

void web_send_error(web_conn_t *c, int status, const char *name, const char *msg) {
  web_send_error_hdr(c, status, name, msg, status == 503 ? "Retry-After: 1\r\n" : NULL);
}

void web_send_sb(web_conn_t *c, int status, sb_t *b) {
  if (b->oom) web_send_error(c, 500, "Internal", "out of memory");
  else web_send_json(c, status, b->p, b->len);
}

void web_send_ok(web_conn_t *c, int status) { web_send_json(c, status, "{\"ok\":true}", 11); }

void web_send_empty(web_conn_t *c, int status, const char *extra) {
  char x[1024];
  snprintf(x, sizeof x, "Cache-Control: no-store\r\n%s", extra ? extra : "");
  web_send_head(c, status, NULL, 0, x);
}

/* ================================================================ static files */
const char *web_mime(const char *path) {
  static const char *const map[][2] = {
    { "html", "text/html; charset=utf-8" }, { "htm", "text/html; charset=utf-8" },
    { "js", "text/javascript; charset=utf-8" }, { "mjs", "text/javascript; charset=utf-8" },
    { "css", "text/css; charset=utf-8" }, { "json", "application/json; charset=utf-8" },
    { "map", "application/json; charset=utf-8" }, { "webmanifest", "application/manifest+json" },
    { "png", "image/png" }, { "svg", "image/svg+xml" }, { "ico", "image/x-icon" },
    { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" }, { "gif", "image/gif" }, { "webp", "image/webp" },
    { "txt", "text/plain; charset=utf-8" }, { "woff2", "font/woff2" }, { "woff", "font/woff" },
    { "ttf", "font/ttf" }, { "wav", "audio/wav" }, { "mp3", "audio/mpeg" }, { "ogg", "audio/ogg" },
    { "flac", "audio/flac" }, { "lua", "text/plain; charset=utf-8" },
  };
  const char *slash = strrchr(path, '/'), *dot = strrchr(path, '.');
  if (!dot || (slash && dot < slash)) return "application/octet-stream";
  for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
    if (!web_casecmp(dot + 1, map[i][0])) return map[i][1];
  return "application/octet-stream";
}

/* If-None-Match: "*", or a list of (weak) tags compared weakly */
bool web_etag_match(const char *inm, const char *etag) {
  if (!inm || !etag) return false;
  const char *e = etag;
  if (!strncmp(e, "W/", 2)) e += 2;
  size_t el = strlen(e);
  const char *p = inm;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == ',') p++;
    if (!*p) break;
    const char *s = p;
    if (*p == '*') {
      p++;
      while (*p == ' ' || *p == '\t') p++;
      if (!*p || *p == ',') return true;
      while (*p && *p != ',') p++;                /* "**", "*x": not a tag */
      continue;
    }
    if (!strncmp(p, "W/", 2)) { p += 2; s = p; }
    if (*p != '"') { while (*p && *p != ',') p++; continue; }
    const char *q = strchr(p + 1, '"');
    if (!q) return false;
    if ((size_t)(q + 1 - s) == el && !memcmp(s, e, el)) return true;
    p = q + 1;
  }
  return false;
}

static bool accepts_gzip(const char *ae) {
  for (const char *p = ae; *p;) {
    while (*p == ' ' || *p == ',') p++;
    const char *t = p;
    while (*p && *p != ',' && *p != ';' && *p != ' ') p++;
    bool gz = (p - t == 4 && !web_ncasecmp(t, "gzip", 4));
    double q = 1;
    while (*p == ' ') p++;
    if (*p == ';') {
      const char *qq = strstr(p, "q=");
      const char *comma = strchr(p, ',');
      if (qq && (!comma || qq < comma)) q = atof(qq + 2);
    }
    if (gz) return q > 0;
    while (*p && *p != ',') p++;
  }
  return false;
}

void web_send_file(web_conn_t *c, const char *path, const char *ctype) {
  web_stat_t st;
  if (web_fs_stat(path, &st) != 0 || !st.exists || st.is_dir || st.is_link) {
    web_send_error(c, 404, "NotFound", "There is no such page or file.");
    return;
  }
  char gzp[1200], etag[96], extra[512];
  web_stat_t gst = { 0 };
  bool gz = false, have_gz = false;
  if (snprintf(gzp, sizeof gzp, "%s.gz", path) < (int)sizeof gzp && web_fs_stat(gzp, &gst) == 0 && gst.exists &&
      !gst.is_dir && !gst.is_link) {
    have_gz = true;
    gz = accepts_gzip(c->accept_enc);
  }
  /* size and time of what is opened: an update may replace the file between the stat and here */
  web_stat_t fs;
  int fd = web_fs_open_read(gz ? gzp : path);
  if (fd < 0 || web_fs_fstat(fd, &fs) != 0 || fs.is_dir) {
    web_fs_close(fd);
    web_send_error(c, 404, "NotFound", "There is no such page or file.");
    return;
  }
  const web_stat_t *s = &fs;
  snprintf(etag, sizeof etag, "\"%llx-%llx%s\"", (unsigned long long)s->size, (unsigned long long)s->mtime_ms, gz ? "-gz" : "");
  snprintf(extra, sizeof extra, "ETag: %s\r\nCache-Control: no-cache\r\n%s%s", etag,
           gz ? "Content-Encoding: gzip\r\n" : "", have_gz ? "Vary: Accept-Encoding\r\n" : "");
  if (c->inm[0] && web_etag_match(c->inm, etag)) {
    web_fs_close(fd);
    web_send_head(c, 304, NULL, 0, extra);
    return;
  }
  if (web_send_head(c, 200, ctype ? ctype : web_mime(path), s->size, extra) == 0 && !c->head_only) {
    char *buf = malloc(65536);
    int64_t left = s->size;
    while (buf && left > 0) {
      long r = web_fs_read(fd, buf, left > 65536 ? 65536 : (size_t)left);
      if (r <= 0 || send_all(c, buf, (size_t)r) != 0) break;
      left -= r;
    }
    free(buf);
    if (left > 0) c->keep_alive = false;        /* the file shrank under us: the length is a lie now */
  }
  web_fs_close(fd);
}

/* ================================================================ request bodies */
long web_read_body(web_conn_t *c, void *dst, size_t n) {
  if (c->dead) return -1;
  if (c->body_left <= 0 || !n) return 0;
  if ((int64_t)n > c->body_left) n = (size_t)c->body_left;
  if (c->expect100 && !c->sent100) {
    c->sent100 = true;
    if (send_all(c, "HTTP/1.1 100 Continue\r\n\r\n", 25) != 0) return -1;
  }
  size_t avail = c->blen - c->rpos;
  if (avail) {
    size_t k = avail < n ? avail : n;
    memcpy(dst, c->buf + c->rpos, k);
    c->rpos += k;
    c->body_left -= (int64_t)k;
    return (long)k;
  }
  long r = recv_some(c, dst, n, WEB_IDLE_MS);
  if (r <= 0) { c->dead = true; return -1; }
  c->body_left -= r;
  return r;
}

int web_read_body_all(web_conn_t *c, size_t max, char **out, size_t *len) {
  *out = NULL;
  if (len) *len = 0;
  if (c->body_left > (int64_t)max) return -2;
  size_t n = (size_t)c->body_left, got = 0;
  char *b = malloc(n + 1);
  if (!b) return -1;
  while (got < n) {
    long r = web_read_body(c, b + got, n - got);
    if (r <= 0) { free(b); c->dead = true; return -1; }
    got += (size_t)r;
  }
  b[got] = 0;
  *out = b;
  if (len) *len = got;
  return 0;
}

/* ================================================================ request parsing */
static bool tchar(int ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || (ch && strchr("!#$%&'*+-.^_`|~", ch));
}

static void copy_field(char *dst, size_t dl, const char *v, size_t vl, bool *too_long) {
  if (vl >= dl) { if (too_long) *too_long = true; vl = dl - 1; }
  memcpy(dst, v, vl);
  dst[vl] = 0;
}

static void reset_request(web_conn_t *c) {
  c->method[0] = 0;
  c->target[0] = 0;
  c->path = c->query = "";
  c->minor = 1;
  c->host[0] = c->origin[0] = c->ctype[0] = c->inm[0] = c->accept_enc[0] = 0;
  c->has_host = c->has_origin = c->xst = c->has_len = c->te = c->expect100 = c->sent100 = false;
  c->clen = c->body_left = 0;
  c->head_only = c->responded = false;
  c->keep_alive = true;
  c->status = 0;
}

/* Parse the head in h[0..n): 0 ok, else the HTTP status of the error. */
static int parse_head(web_conn_t *c, char *h, size_t n) {
  if (memchr(h, 0, n)) return 400;
  char *end = h + n, *line = h;
  bool first = true, conn_close = false, conn_ka = false;
  int nheaders = 0;
  while (line < end) {
    char *nl = memchr(line, '\n', (size_t)(end - line));
    if (!nl) return 400;
    char *le = nl;
    if (le > line && le[-1] == '\r') le--;
    if (memchr(line, '\r', (size_t)(le - line))) return 400;   /* a bare CR */
    size_t ll = (size_t)(le - line);
    if (first) {
      first = false;
      char *sp1 = memchr(line, ' ', ll);
      if (!sp1) return 400;
      char *sp2 = memchr(sp1 + 1, ' ', (size_t)(le - sp1 - 1));
      if (!sp2 || memchr(sp2 + 1, ' ', (size_t)(le - sp2 - 1))) return 400;
      size_t ml = (size_t)(sp1 - line), tl = (size_t)(sp2 - sp1 - 1), vl = (size_t)(le - sp2 - 1);
      if (!ml || ml >= sizeof c->method) return 400;
      for (size_t i = 0; i < ml; i++) if (line[i] < 'A' || line[i] > 'Z') return 400;
      memcpy(c->method, line, ml);
      c->method[ml] = 0;
      const char *v = sp2 + 1;
      if (vl == 8 && !memcmp(v, "HTTP/1.1", 8)) c->minor = 1;
      else if (vl == 8 && !memcmp(v, "HTTP/1.0", 8)) c->minor = 0;
      else if (vl == 8 && !memcmp(v, "HTTP/", 5) && v[5] >= '0' && v[5] <= '9' && v[6] == '.' && v[7] >= '0' && v[7] <= '9') return 505;
      else return 400;
      const char *t = sp1 + 1;
      if (!tl || tl >= sizeof c->target || t[0] != '/') return 400;
      for (size_t i = 0; i < tl; i++) if ((unsigned char)t[i] <= 0x20 || (unsigned char)t[i] >= 0x7f || t[i] == '#') return 400;
      memcpy(c->target, t, tl);
      c->target[tl] = 0;
      char *q = strchr(c->target, '?');
      if (q) { *q = 0; c->query = q + 1; }
      else c->query = "";
      c->path = c->target;
    } else {
      if (!ll) break;                               /* the blank line */
      if (++nheaders > WEB_MAX_HEADERS) return 431;
      if (line[0] == ' ' || line[0] == '\t') return 400;   /* obsolete line folding */
      char *colon = memchr(line, ':', ll);
      if (!colon || colon == line) return 400;
      for (char *p = line; p < colon; p++) if (!tchar((unsigned char)*p)) return 400;
      size_t nl2 = (size_t)(colon - line);
      char *v = colon + 1, *ve = le;
      while (v < ve && (*v == ' ' || *v == '\t')) v++;
      while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
      for (char *p = v; p < ve; p++) {
        unsigned char ch = (unsigned char)*p;
        if ((ch < 0x20 && ch != '\t') || ch == 0x7f) return 400;
      }
      size_t vlen = (size_t)(ve - v);
#define HN(s) (nl2 == sizeof(s) - 1 && !web_ncasecmp(line, s, nl2))
      if (HN("host")) {
        bool tl2 = false;
        if (c->has_host) return 400;
        c->has_host = true;
        copy_field(c->host, sizeof c->host, v, vlen, &tl2);
        if (tl2) return 400;
      } else if (HN("content-length")) {
        if (!vlen || vlen > 15) return 400;
        int64_t x = 0;
        for (size_t i = 0; i < vlen; i++) {
          if (v[i] < '0' || v[i] > '9') return 400;
          x = x * 10 + (v[i] - '0');
        }
        if (c->has_len && x != c->clen) return 400;
        c->has_len = true;
        c->clen = x;
      } else if (HN("transfer-encoding")) {
        c->te = true;
      } else if (HN("connection")) {
        for (char *p = v; p < ve;) {
          while (p < ve && (*p == ' ' || *p == '\t' || *p == ',')) p++;
          char *s = p;
          while (p < ve && *p != ',' && *p != ' ' && *p != '\t') p++;
          if (p - s == 5 && !web_ncasecmp(s, "close", 5)) conn_close = true;
          if (p - s == 10 && !web_ncasecmp(s, "keep-alive", 10)) conn_ka = true;
        }
      } else if (HN("expect")) {
        if (vlen == 12 && !web_ncasecmp(v, "100-continue", 12)) c->expect100 = true;
        else return 417;
      } else if (HN("origin")) {
        bool tl2 = false;
        c->has_origin = true;
        copy_field(c->origin, sizeof c->origin, v, vlen, &tl2);
        if (tl2) c->origin[0] = 0;                  /* too long: never matches */
      } else if (HN("content-type")) {
        copy_field(c->ctype, sizeof c->ctype, v, vlen, NULL);
      } else if (HN("if-none-match")) {
        bool tl2 = false;
        copy_field(c->inm, sizeof c->inm, v, vlen, &tl2);
        if (tl2) c->inm[0] = 0;
      } else if (HN("accept-encoding")) {
        copy_field(c->accept_enc, sizeof c->accept_enc, v, vlen, NULL);
      } else if (HN("x-sticktime") || HN("x-sticktime-sd")) {
        if (vlen == 1 && v[0] == '1') c->xst = true;
      }
#undef HN
    }
    line = nl + 1;
  }
  c->keep_alive = c->minor == 1 ? !conn_close : (conn_ka && !conn_close);
  return 0;
}

static size_t head_end(const char *p, size_t n, size_t from) {
  for (size_t i = from; i + 1 < n; i++) {
    if (p[i] != '\n') continue;
    if (p[i + 1] == '\n') return i + 2;
    if (p[i + 1] == '\r' && i + 2 < n && p[i + 2] == '\n') return i + 3;
  }
  return 0;
}

/* 1 a request is ready, 0 the connection ended quietly, -1 an error was answered (close) */
static int read_request(web_conn_t *c) {
  reset_request(c);
  int64_t deadline = plat_now_us() + (int64_t)WEB_IDLE_MS * 1000;
  size_t scanned = 0;
  for (;;) {
    /* blank lines between requests are allowed */
    while (c->rpos < c->blen && (c->buf[c->rpos] == '\r' || c->buf[c->rpos] == '\n')) c->rpos++;
    if (c->rpos) {
      memmove(c->buf, c->buf + c->rpos, c->blen - c->rpos);
      c->blen -= c->rpos;
      scanned = scanned > c->rpos ? scanned - c->rpos : 0;
      c->rpos = 0;
    }
    size_t he = head_end(c->buf, c->blen, scanned > 3 ? scanned - 3 : 0);
    if (he) {
      int err = parse_head(c, c->buf, he);
      c->rpos = he;
      if (err) {
        c->keep_alive = false;
        c->body_left = 0;
        const char *msg = err == 431 ? "The request headers are too big." :
                          err == 505 ? "Only HTTP/1.0 and HTTP/1.1 are supported." :
                          err == 417 ? "Only Expect: 100-continue is supported." : "That isn't a valid HTTP request.";
        web_send_error(c, err, "BadRequest", msg);
        return -1;
      }
      return 1;
    }
    scanned = c->blen;
    if (c->blen >= WEB_HEAD_MAX) {
      c->keep_alive = false;
      web_send_error(c, 431, "BadRequest", "The request headers are too big.");
      return -1;
    }
    int left = (int)((deadline - plat_now_us()) / 1000);
    if (left <= 0) {
      if (!c->blen) return 0;                     /* idle keep-alive connection */
      c->keep_alive = false;
      web_send_error(c, 408, "BadRequest", "The request headers took too long.");
      return -1;
    }
    if (web_stopping()) return 0;
    int w = wait_readable(c->s, left < 250 ? left : 250);
    if (w < 0) return 0;
    if (w == 0) continue;
    int r = (int)recv(c->s, c->buf + c->blen, (int)(WEB_HEAD_MAX - c->blen), 0);
    if (r <= 0) return 0;
    c->blen += (size_t)r;
  }
}

static bool is_mutating(const char *m) { return !strcmp(m, "POST") || !strcmp(m, "PUT") || !strcmp(m, "DELETE"); }

static void handle_request(web_conn_t *c) {
  if (c->te) {
    c->keep_alive = false;                        /* we can't find the end of this body */
    web_send_error(c, 411, "BadRequest", "Send the body with a Content-Length (chunked transfer encoding isn't supported).");
    return;
  }
  c->body_left = c->has_len ? c->clen : 0;
  if (c->minor == 1 && !c->has_host) {
    web_send_error(c, 400, "BadRequest", "The Host header is missing.");
    return;
  }
  if (c->has_host && !web_host_ok(c->host)) {
    web_send_error(c, 421, "Forbidden", "Use this Pi's IP address or its name (sticktime.local) in the address bar.");
    return;
  }
  if (is_mutating(c->method)) {
    if (!c->xst) {
      web_send_error(c, 403, "Forbidden", "Changes need the header X-StickTime: 1 (requests from other sites are refused).");
      return;
    }
    if (c->has_origin && !web_origin_ok(c->origin, c->host)) {
      web_send_error(c, 403, "Forbidden", "Requests from other sites are refused.");
      return;
    }
  }
  c->head_only = !strcmp(c->method, "HEAD");
  web_route(c);
  if (!c->responded && !c->dead) web_send_error(c, 500, "Internal", "The server had no answer for that.");
}

/* ================================================================ threads */
static void worker_done(void) {
  plat_mutex_lock(&W.mu);
  W.nworkers--;
  plat_cond_broadcast(&W.cv);
  plat_mutex_unlock(&W.mu);
}

static void *worker_main(void *arg) {
  web_conn_t *c = arg;
  plat_thread_lower_priority();
  int one = 1;
  setsockopt(c->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
  plat_sock_timeouts(c->s, WEB_IDLE_MS, WEB_IDLE_MS);
  int linger = 1000;
  for (;;) {
    int r = read_request(c);
    if (r <= 0) break;
    handle_request(c);
    if (c->dead) { linger = 0; break; }
    if (c->body_left > 0) {
      /* the route didn't read the body: read it away if that is cheap, else close */
      if (!c->keep_alive || c->body_left > WEB_DRAIN_MAX || (c->expect100 && !c->sent100)) { linger = 10000; break; }
      char tmp[4096];
      while (c->body_left > 0 && web_read_body(c, tmp, sizeof tmp) > 0) {}
      if (c->dead) { linger = 0; break; }
    }
    if (!c->keep_alive || web_stopping()) break;
  }
  if (linger) close_conn(c->s, linger, 256LL << 20);
  else plat_sock_close(c->s);
  free(c);
  worker_done();
  return NULL;
}

typedef struct { plat_sock s; } reject_t;

static void *reject_main(void *arg) {
  reject_t *r = arg;
  static const char body[] = "{\"error\":\"Busy\",\"message\":\"Too many connections: try again in a moment.\"}";
  char h[512];
  int n = snprintf(h, sizeof h,
                   "HTTP/1.1 503 Service Unavailable\r\nServer: sticktime-pi\r\nContent-Type: application/json; charset=utf-8\r\n"
                   "Content-Length: %d\r\nRetry-After: 1\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                   "Connection: close\r\n\r\n%s", (int)sizeof body - 1, body);
  plat_sock_timeouts(r->s, 1000, 1000);
  send(r->s, h, n, MSG_NOSIGNAL);
  close_conn(r->s, 1000, 1 << 20);
  free(r);
  plat_mutex_lock(&W.mu);
  W.nreject--;
  plat_cond_broadcast(&W.cv);
  plat_mutex_unlock(&W.mu);
  return NULL;
}

static void reject(plat_sock s) {
  plat_mutex_lock(&W.mu);
  bool ok = W.nreject < 16;
  if (ok) W.nreject++;
  plat_mutex_unlock(&W.mu);
  reject_t *r = ok ? malloc(sizeof *r) : NULL;
  if (r) {
    r->s = s;
    if (plat_thread_start(NULL, reject_main, r, true) == 0) return;
    free(r);
  }
  if (ok) {
    plat_mutex_lock(&W.mu);
    W.nreject--;
    plat_mutex_unlock(&W.mu);
  }
  plat_sock_close(s);
}

static void *accept_main(void *arg) {
  (void)arg;
  plat_thread_lower_priority();
  web_api_start(W.app);
  while (!web_stopping()) {
    if (wait_readable(W.ls, 200) <= 0) continue;
    struct sockaddr_storage sa;
    plat_socklen sl = sizeof sa;
    plat_sock s = accept(W.ls, (struct sockaddr *)&sa, &sl);
    if (s == PLAT_BADSOCK) { plat_sleep_us(10000); continue; }
    no_inherit(s);
#ifndef _WIN32
    fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
    plat_mutex_lock(&W.mu);
    bool ok = W.nworkers < W.cfg.max_clients;
    if (ok) W.nworkers++;
    plat_mutex_unlock(&W.mu);
    if (!ok) { reject(s); continue; }
    web_conn_t *c = calloc(1, sizeof *c);
    if (c) {
      c->s = s;
      c->app = W.app;
      if (plat_thread_start(NULL, worker_main, c, true) == 0) continue;
      free(c);
    }
    plat_sock_close(s);
    worker_done();
  }
  return NULL;
}

int web_start(app_t *app, const web_config_t *cfg) {
  web_init_once();
  if (W.running) return 0;
  plat_net_init();
  W.app = app;
  memset(&W.cfg, 0, sizeof W.cfg);
  if (cfg) W.cfg = *cfg;
  if (!W.cfg.port) W.cfg.port = 80;
  if (W.cfg.max_clients <= 0) W.cfg.max_clients = 16;
  if (!W.cfg.max_upload) W.cfg.max_upload = 64u << 20;
  W.cfg.hostname = NULL;                          /* the caller's buffer may go away: names[] keeps it */
  plat_mutex_lock(&W.mu);
  W.names[0][0] = W.names[1][0] = 0;
  plat_strlcpy(W.names[0], cfg && cfg->hostname && *cfg->hostname ? cfg->hostname : "sticktime", sizeof W.names[0]);
  plat_mutex_unlock(&W.mu);
  __atomic_store_n(&W.stop, 0, __ATOMIC_SEQ_CST);
  W.ls = plat_tcp_listen(W.cfg.bind_addr, W.cfg.port, 64);
  if (W.ls == PLAT_BADSOCK) {
    app_log(app, "web: can't listen on %s:%u", W.cfg.bind_addr ? W.cfg.bind_addr : "*", (unsigned)W.cfg.port);
    return -1;
  }
  no_inherit(W.ls);
#ifndef _WIN32
  fcntl(W.ls, F_SETFD, FD_CLOEXEC);
#endif
  W.cfg.bind_addr = NULL;
  if (plat_thread_start(&W.th, accept_main, NULL, false) != 0) {
    plat_sock_close(W.ls);
    W.ls = PLAT_BADSOCK;
    app_log(app, "web: can't start the server thread");
    return -1;
  }
  W.running = true;
  app_log(app, "web: listening on port %u", (unsigned)W.cfg.port);
  return 0;
}

void web_stop(void) {
  if (!W.running) return;
  __atomic_store_n(&W.stop, 1, __ATOMIC_SEQ_CST);
  plat_thread_join(W.th);
  plat_sock_close(W.ls);
  W.ls = PLAT_BADSOCK;
  /* workers notice within 250 ms; one stuck in a helper is left behind after 3 s */
  plat_mutex_lock(&W.mu);
  int64_t end = plat_now_us() + 3000000;
  while ((W.nworkers > 0 || W.nreject > 0) && plat_now_us() < end) plat_cond_wait(&W.cv, &W.mu, 100);
  plat_mutex_unlock(&W.mu);
  web_api_stop();
  W.running = false;
}

/* web_config_t limits for the routes */
size_t web_max_upload(void) { return W.cfg.max_upload ? W.cfg.max_upload : (64u << 20); }
