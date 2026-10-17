/* ELRS / Crossfire WiFi joystick protocol engine: see wifi_proto.h.
 * Port of JoystickEngine.cs (ELRS-Crossfire-WiFi-Joystick-Windows, MIT, (c) juricabi).
 * Deliberate deviations from it: tick runs on every poll (no starvation by ignored traffic),
 * monotonic time, IPs as numbers, set_target POSTs at once and reports a state, a Pi-specific
 * "no data" hint (not armed by a POST answer that comes after its module's data), and the request
 * asks for the configured interval and 16 channels. */
#include "wifi_proto.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char HINT_TEXT[] =
  "!! Activated OK but NO data arriving - the module is not connected to a powered radio, "
  "another client took the stream, or the WiFi blocks client-to-client traffic.";

const char *wifi_state_name(wifi_state_t s) {
  switch (s) {
    case WIFI_ST_STOPPED: return "stopped";
    case WIFI_ST_SEARCHING: return "searching";
    case WIFI_ST_STREAMING: return "streaming";
    case WIFI_ST_ERROR: return "error";
  }
  return "?";
}

const char *wifi_kind_name(wifi_kind_t k) {
  return k == WIFI_KIND_ELRS ? "ELRS" : k == WIFI_KIND_XF ? "Crossfire" : "WiFi";
}

static void plog(wifi_proto_t *p, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
  ;
static void plog(wifi_proto_t *p, const char *fmt, ...) {
  if (!p->h.log) return;
  char line[320];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  p->h.log(p->h.ud, line);
}

void wifi_proto_set_state(wifi_proto_t *p, wifi_state_t s, const char *detail) {
  p->state = s;
  snprintf(p->detail, sizeof p->detail, "%s", detail ? detail : "");
  if (p->h.state) p->h.state(p->h.ud, s, p->detail);
}

/* ---- small fixed tables (no allocation; a full table refuses, so logging can never spam) ---- */
static int ip_index(const uint32_t *t, int n, uint32_t ip) {
  for (int i = 0; i < n; i++) if (t[i] == ip) return i;
  return -1;
}

static bool set_add(uint32_t *t, int *n, uint32_t ip) {
  if (ip_index(t, *n, ip) >= 0 || *n >= WIFI_TABLE) return false;
  t[(*n)++] = ip;
  return true;
}

static int act_find(const wifi_proto_t *p, uint32_t ip) {
  for (int i = 0; i < p->nact; i++) if (p->act[i].ip == ip) return i;
  return -1;
}

/* Full table: reuse an expired entry, else the least recently used one. Evicting a fresh entry
 * lets that IP be POSTed early; only more than WIFI_TABLE modules beaconing at once do that. */
static void act_record(wifi_proto_t *p, uint32_t ip, int64_t now) {
  int i = act_find(p, ip);
  if (i < 0) {
    if (p->nact < WIFI_TABLE) i = p->nact++;
    else {
      i = 0;
      for (int k = 1; k < p->nact; k++) if (p->act[k].t_us < p->act[i].t_us) i = k;
    }
  }
  p->act[i].ip = ip;
  p->act[i].t_us = now;
}

static void act_remove(wifi_proto_t *p, uint32_t ip) {
  int i = act_find(p, ip);
  if (i < 0) return;
  p->act[i] = p->act[--p->nact];
}

static void note_beacon(wifi_proto_t *p, uint32_t ip, wifi_kind_t kind, const char *name, int64_t now) {
  int i = 0;
  while (i < p->nseen && p->seen[i].ip != ip) i++;
  if (i == p->nseen) {
    if (p->nseen < WIFI_TABLE) p->nseen++;
    else {
      i = 0;
      for (int k = 1; k < p->nseen; k++) if (p->seen[k].t_us < p->seen[i].t_us) i = k;
    }
  }
  p->seen[i].ip = ip;
  p->seen[i].kind = kind;
  snprintf(p->seen[i].name, sizeof p->seen[i].name, "%s", name);
  p->seen[i].t_us = now;
}

wifi_kind_t wifi_proto_kind(const wifi_proto_t *p, uint32_t ip) {
  for (int i = 0; i < p->nseen; i++) if (p->seen[i].ip == ip) return p->seen[i].kind;
  return WIFI_KIND_UNKNOWN;
}

const char *wifi_proto_module_name(const wifi_proto_t *p, uint32_t ip) {
  for (int i = 0; i < p->nseen; i++) if (p->seen[i].ip == ip) return p->seen[i].name;
  return "";
}

/* ---- engine ---- */
void wifi_proto_init(wifi_proto_t *p, const wifi_hooks_t *h) {
  memset(p, 0, sizeof *p);
  if (h) p->h = *h;
  p->state = WIFI_ST_STOPPED;
  p->last_stats_us = INT64_MIN;
}

void wifi_proto_reset(wifi_proto_t *p) {
  p->bound = false;
  p->bound_ip = 0;
  p->bound_last_us = 0;
  p->nwarned = p->nnostick = 0;
  p->nostick_ip = 0;
  p->nact = 0;
  p->first_act_set = false;
  p->first_act_us = 0;
  p->first_act_ip = 0;
  p->hint_shown = false;
  p->nseen = 0;
  p->frames = p->narr = p->ngaps = 0;
  p->arr_first = p->arr_prev = 0;
  p->gap_mean = p->gap_m2 = 0;
  p->last_hz = p->last_jitter = 0;
  p->last_frames = 0;
  p->nch = 0;
  p->last_src = 0;
}

static void mark_activated(wifi_proto_t *p, uint32_t ip, int64_t now) {
  /* ELRS starts streaming before it answers the POST, so the answer often comes after the lock.
   * Data from that module already proved the path: arming the hint then would make it fire after
   * any later lock loss, depending on which packet won the race (the reference has that race). */
  if (p->bound && p->bound_ip == ip && now - p->bound_last_us <= WIFI_SOURCE_TIMEOUT_US) return;
  if (p->first_act_set) return;
  p->first_act_set = true;
  p->first_act_us = now;
  p->first_act_ip = ip;
}

void wifi_proto_ensure(wifi_proto_t *p, uint32_t ip, int64_t now) {
  int i = act_find(p, ip);
  if (i >= 0 && now - p->act[i].t_us < WIFI_THROTTLE_US) return;
  int r = p->h.activate ? p->h.activate(p->h.ud, ip) : WIFI_ACT_FAILED;
  if (r == WIFI_ACT_BUSY) return;      /* nothing went out: try again on the next chance */
  act_record(p, ip, now);              /* failures are throttled too */
  if (r == WIFI_ACT_OK) mark_activated(p, ip, now);
}

void wifi_proto_activation_done(wifi_proto_t *p, uint32_t ip, bool ok, int64_t now) {
  if (ok) mark_activated(p, ip, now);
}

static void warn_no_stick(wifi_proto_t *p, uint32_t src) {
  if (!set_add(p->nostick, &p->nnostick, src)) return;
  char ip[16];
  wifi_ip_str(src, ip);
  p->nostick_ip = src;
  plog(p, "!! %s is streaming placeholder frames - no stick data. Either the radio is not feeding the module "
          "right now, or the firmware has the known regression (XF 6.42/6.48, WiFi 3.20 - use XF 6.31 or 6.36, "
          "WiFi up to 3.10).", ip);
  wifi_proto_set_state(p, WIFI_ST_SEARCHING, "module found, but it is sending no stick data");
}

void wifi_proto_tick(wifi_proto_t *p, int64_t now) {
  if (p->bound && now - p->bound_last_us > WIFI_SOURCE_TIMEOUT_US) {
    char ip[16];
    wifi_ip_str(p->bound_ip, ip);
    plog(p, "Source %s went quiet - releasing lock", ip);
    p->bound = false;
    wifi_proto_set_state(p, WIFI_ST_SEARCHING, "waiting for module");
  }
  /* placeholder frames have their own explanation: the network is demonstrably fine then */
  if (p->first_act_set && !p->bound && !p->hint_shown && p->nnostick == 0 &&
      now - p->first_act_us > WIFI_FW_HINT_US) {
    p->hint_shown = true;
    plog(p, "%s", HINT_TEXT);
    wifi_proto_set_state(p, WIFI_ST_SEARCHING, "activated, but no data (radio off? another client?)");
  }
  if (p->target && !p->bound) wifi_proto_ensure(p, p->target, now);
}

void wifi_proto_add_arrival(wifi_proto_t *p, int64_t t) {
  p->frames++;
  if (p->narr++ == 0) {
    p->arr_first = p->arr_prev = t;
    return;
  }
  /* Welford: the population variance of the gaps without storing them */
  double g = (double)(t - p->arr_prev) / 1000.0;
  p->arr_prev = t;
  p->ngaps++;
  double d = g - p->gap_mean;
  p->gap_mean += d / p->ngaps;
  p->gap_m2 += d * (g - p->gap_mean);
}

void wifi_proto_emit_stats(wifi_proto_t *p, int64_t now) {
  double hz = 0, jit = 0;
  if (p->narr > 2) {
    jit = sqrt(p->gap_m2 > 0 ? p->gap_m2 / p->ngaps : 0);
    double span = (double)(p->arr_prev - p->arr_first) / 1000.0;
    if (span > 0) hz = (p->narr - 1) * 1000.0 / span;
  }
  p->last_hz = hz;
  p->last_jitter = jit;
  p->last_frames = p->frames;
  p->last_stats_us = now;
  if (p->h.stats) p->h.stats(p->h.ud, hz, jit, p->frames);
  p->frames = p->narr = p->ngaps = 0;
  p->gap_mean = p->gap_m2 = 0;
}

void wifi_proto_poll_stats(wifi_proto_t *p, int64_t now) {
  if (p->last_stats_us == INT64_MIN) p->last_stats_us = now;
  else if (now - p->last_stats_us >= WIFI_STATS_PERIOD_US) wifi_proto_emit_stats(p, now);
}

void wifi_proto_packet(wifi_proto_t *p, const uint8_t *d, size_t n, uint32_t src, int64_t now, int64_t arrival) {
  p->rx++;
  if (!d || n < 3) { p->malformed++; return; }

  bool xf = wifi_starts_with(d, n, "VELOCIDRONE");
  if (xf || wifi_starts_with(d, n, "ELRS")) {
    p->beacons++;
    if (p->target && src != p->target) { p->dropped_other++; return; }   /* only the chosen module */
    char name[32] = "";
    if (!xf) wifi_parse_elrs_beacon(d, n, NULL, NULL, name, sizeof name);
    note_beacon(p, src, xf ? WIFI_KIND_XF : WIFI_KIND_ELRS, name, now);
    bool locked_other = p->bound && p->bound_ip != src;
    bool streaming = p->bound && p->bound_ip == src && now - p->bound_last_us < WIFI_SOURCE_TIMEOUT_US;
    if (!locked_other && !streaming) {
      char ip[16];
      wifi_ip_str(src, ip);
      plog(p, "Discovery beacon from %s module %s - activating", xf ? "Crossfire" : "ELRS", ip);
      wifi_proto_ensure(p, src, now);
    }
    return;     /* never parsed as channels, whatever follows */
  }

  int count = d[1];
  if (count < WIFI_MIN_CH || count > WIFI_MAX_CH || n < 2 + 2 * (size_t)count) { p->malformed++; return; }
  if (p->target && src != p->target) { p->dropped_other++; return; }

  uint16_t ch[WIFI_MAX_CH];
  int out_of_range = 0;
  bool placeholder = false;
  for (int i = 0; i < count; i++) {
    int v = d[2 + 2 * i] | d[3 + 2 * i] << 8;
    if (v == WIFI_XF_PLACEHOLDER) placeholder = true;
    /* a stick past a wide endpoint reads a little above 32767 (0x816B seen live): full deflection */
    if (v > WIFI_RAW_MAX) { out_of_range++; v = WIFI_RAW_MAX; }
    ch[i] = (uint16_t)v;
  }
  /* 0xF26A frames carry no stick data (radio not feeding the module, or a corrupt frame under
   * fast stick movement): applying one would spike every axis for a frame. A placeholder flood
   * also keeps the socket busy, so the reference ticks from here (harmless on top of ours). */
  if (placeholder || out_of_range == count) {
    p->dropped_filter++;
    wifi_proto_tick(p, now);
    if (!p->bound) warn_no_stick(p, src);
    return;
  }

  if (p->bound && p->bound_ip != src) {
    if (now - p->bound_last_us > WIFI_SOURCE_TIMEOUT_US) p->bound = false;
    else {
      if (set_add(p->warned, &p->nwarned, src)) {
        char a[16], b[16];
        wifi_ip_str(src, a);
        wifi_ip_str(p->bound_ip, b);
        plog(p, "Ignoring 2nd source %s (locked to %s)", a, b);
      }
      p->dropped_other++;
      return;
    }
  }
  if (!p->bound) {
    char ip[16];
    wifi_ip_str(src, ip);
    p->bound = true;
    p->bound_ip = src;
    p->nwarned = p->nnostick = 0;
    p->first_act_set = false;
    p->hint_shown = false;
    plog(p, "Joystick source locked to %s", ip);
    wifi_proto_set_state(p, WIFI_ST_STREAMING, ip);
  }
  p->bound_last_us = now;

  memcpy(p->raw, ch, sizeof(uint16_t) * (size_t)count);
  p->nch = (uint8_t)count;
  p->seq++;
  p->last_arrival_us = arrival;
  p->last_src = src;
  if (p->h.frame) p->h.frame(p->h.ud, src, p->raw, count);
  wifi_proto_add_arrival(p, arrival);
}

int wifi_proto_set_target(wifi_proto_t *p, const char *text, int64_t now) {
  uint32_t ip = 0;
  int r = wifi_parse_ip(text, &ip);
  if (r == 0) { p->target = 0; return 0; }
  if (r < 0 || ip == 0) {
    /* keep the log line one clean line whatever was typed */
    char safe[44];
    size_t k = 0;
    for (const char *s = text; *s && k < 40; s++) safe[k++] = (*s >= 0x20 && *s < 0x7f) ? *s : '?';
    safe[k] = 0;
    plog(p, "Invalid module IP \"%s\" - use a.b.c.d", safe);
    return -1;
  }
  char s[16], detail[64];
  wifi_ip_str(ip, s);
  p->target = ip;
  act_remove(p, ip);          /* allow an immediate POST */
  p->bound = false;           /* let the new target take over */
  plog(p, "Target module set to %s - connecting...", s);
  snprintf(detail, sizeof detail, "connecting to %s", s);
  wifi_proto_set_state(p, WIFI_ST_SEARCHING, detail);
  wifi_proto_ensure(p, ip, now);
  return 0;
}

/* ---- pure helpers ---- */
bool wifi_starts_with(const uint8_t *d, size_t n, const char *prefix) {
  size_t k = strlen(prefix);
  if (n < k) return false;
  return k == 0 || memcmp(d, prefix, k) == 0;
}

int wifi_parse_elrs_beacon(const uint8_t *d, size_t n, int *version, int *port, char *name, size_t namecap) {
  if (version) *version = -1;
  if (port) *port = -1;
  if (name && namecap) name[0] = 0;
  if (!d || !wifi_starts_with(d, n, "ELRS")) return -1;
  if (n < 8) return 0;
  if (version) *version = d[4];
  if (port) *port = d[5] | d[6] << 8;
  if (name && namecap) {
    size_t len = d[7];
    if (len > n - 8) len = n - 8;
    if (len > namecap - 1) len = namecap - 1;
    for (size_t i = 0; i < len; i++) {
      uint8_t c = d[8 + i];
      name[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
    }
    name[len] = 0;
  }
  return 1;
}

/* Exact inverse of EdgeTX (crsf = 992 + ch*4/5) and ELRS (raw = (crsf-172)*32767/1639): center 16393
 * reads 0, +-100 % read +-1024, the error is at most 1 (EdgeTX's 4/5 quantization). */
int16_t wifi_raw_to_stick(int raw) {
  if (raw < 0) raw = 0;
  if (raw > WIFI_RAW_MAX) raw = WIFI_RAW_MAX;
  int off = (raw * 1639 + 16383) / 32767;          /* crsf - 172, 0..1639 */
  int d5 = (off - 820) * 5;                        /* (crsf - 992) * 5 */
  int v = d5 >= 0 ? (d5 + 2) / 4 : -((-d5 + 2) / 4);
  return (int16_t)(v < -1024 ? -1024 : v > 1024 ? 1024 : v);
}

int wifi_raw_percent(int raw) {
  if (raw < 0) raw = 0;
  if (raw > WIFI_RAW_MAX) raw = WIFI_RAW_MAX;
  return raw * 100 / WIFI_RAW_MAX;
}

static bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

int wifi_parse_ip(const char *text, uint32_t *ip) {
  if (!text) return 0;
  const char *s = text, *e = text + strlen(text);
  while (s < e && is_blank(*s)) s++;
  while (e > s && is_blank(e[-1])) e--;
  if (s == e) return 0;
  uint8_t b[4];
  for (int i = 0; i < 4; i++) {
    if (i) {
      if (s >= e || *s != '.') return -1;
      s++;
    }
    const char *d0 = s;
    int v = 0, nd = 0;
    while (s < e && *s >= '0' && *s <= '9' && nd < 4) { v = v * 10 + (*s - '0'); s++; nd++; }
    if (nd == 0 || nd > 3 || v > 255 || (nd > 1 && *d0 == '0')) return -1;
    b[i] = (uint8_t)v;
  }
  if (s != e) return -1;
  if (ip) memcpy(ip, b, 4);
  return 1;
}

void wifi_ip_str(uint32_t ip, char out[16]) {
  uint8_t b[4];
  memcpy(b, &ip, 4);
  snprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

int wifi_build_body(char *buf, size_t cap, const char *action, int interval_us, int channels) {
  int n = snprintf(buf, cap, "action=%s&interval=%d&channels=%d", action, interval_us, channels);
  return n < 0 || (size_t)n >= cap ? -1 : n;
}

int wifi_build_post(char *buf, size_t cap, uint32_t ip, uint16_t http_port, const char *body) {
  char host[24];
  wifi_ip_str(ip, host);
  if (http_port != 80) {
    size_t k = strlen(host);
    snprintf(host + k, sizeof host - k, ":%u", http_port);
  }
  int n = snprintf(buf, cap,
                   "POST /udpcontrol HTTP/1.1\r\n"
                   "Host: %s\r\n"
                   "User-Agent: sticktime-pi\r\n"
                   "Content-Type: application/x-www-form-urlencoded\r\n"
                   "Content-Length: %u\r\n"
                   "Connection: close\r\n"
                   "\r\n"
                   "%s", host, (unsigned)strlen(body), body);
  return n < 0 || (size_t)n >= cap ? -1 : n;
}

int wifi_parse_http_status(const char *buf, size_t n, char *reason, size_t rcap) {
  if (reason && rcap) reason[0] = 0;
  size_t m = n < 5 ? n : 5;
  if (m && memcmp(buf, "HTTP/", m) != 0) return 0;
  const char *nl = n ? memchr(buf, '\n', n) : NULL;
  if (!nl) return -1;
  size_t end = (size_t)(nl - buf);
  if (end > 0 && buf[end - 1] == '\r') end--;
  if (end < 5) return 0;
  size_t i = 5, v0 = 5;
  while (i < end && ((buf[i] >= '0' && buf[i] <= '9') || buf[i] == '.')) i++;
  if (i == v0 || i >= end || buf[i] != ' ') return 0;
  while (i < end && buf[i] == ' ') i++;
  if (end - i < 3) return 0;
  int code = 0;
  for (int k = 0; k < 3; k++) {
    char c = buf[i + k];
    if (c < '0' || c > '9') return 0;
    code = code * 10 + (c - '0');
  }
  i += 3;
  if ((i < end && buf[i] != ' ') || code < 100) return 0;
  while (i < end && buf[i] == ' ') i++;
  if (reason && rcap) {
    size_t k = 0;
    for (; i < end && k + 1 < rcap; i++) reason[k++] = (buf[i] >= 0x20 && buf[i] < 0x7f) ? buf[i] : '?';
    while (k && reason[k - 1] == ' ') k--;
    reason[k] = 0;
  }
  return code;
}

long wifi_http_content_length(const char *buf, size_t n) {
  static const char key[] = "content-length:";
  size_t i = 0;
  while (i < n) {
    size_t j = i;
    while (j < n && buf[j] != '\n') j++;
    size_t len = j - i;
    if (len && buf[i + len - 1] == '\r') len--;
    if (len == 0 && i > 0) return -1;            /* end of the head */
    if (len >= sizeof key - 1) {
      size_t k = 0;
      while (k < sizeof key - 1) {
        char c = buf[i + k];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != key[k]) break;
        k++;
      }
      if (k == sizeof key - 1) {
        size_t q = i + k, e = i + len;
        while (q < e && (buf[q] == ' ' || buf[q] == '\t')) q++;
        if (q == e) return -1;
        long v = 0;
        for (; q < e && buf[q] >= '0' && buf[q] <= '9'; q++) {
          if (v > 100000000L) return -1;
          v = v * 10 + (buf[q] - '0');
        }
        while (q < e && (buf[q] == ' ' || buf[q] == '\t')) q++;
        return q == e ? v : -1;
      }
    }
    i = j + 1;
  }
  return -1;
}

/* ---- activation queue ---- */
void wifi_actq_init(wifi_actq_t *q) {
  memset(q, 0, sizeof *q);
  q->session = 1;
}

void wifi_actq_new_session(wifi_actq_t *q) {
  q->rq_head = q->rq_n = 0;
  q->rs_head = q->rs_n = 0;
  q->npending = 0;
  q->session++;
}

bool wifi_actq_is_pending(const wifi_actq_t *q, uint32_t ip) {
  for (int i = 0; i < q->npending; i++) if (q->pending[i] == ip) return true;
  return false;
}

static void unpend(wifi_actq_t *q, uint32_t ip) {
  for (int i = 0; i < q->npending; i++)
    if (q->pending[i] == ip) { q->pending[i] = q->pending[--q->npending]; return; }
}

int wifi_actq_push(wifi_actq_t *q, uint32_t ip, uint16_t http_port, const char *body) {
  if (wifi_actq_is_pending(q, ip) || q->rq_n >= WIFI_ACTQ || q->npending >= WIFI_ACTQ) return WIFI_ACT_BUSY;
  wifi_actreq_t *r = &q->req[(q->rq_head + q->rq_n) % WIFI_ACTQ];
  if (strlen(body) >= sizeof r->body) return WIFI_ACT_BUSY;
  r->ip = ip;
  r->http_port = http_port;
  r->session = q->session;
  strcpy(r->body, body);
  q->rq_n++;
  q->pending[q->npending++] = ip;
  return WIFI_ACT_QUEUED;
}

bool wifi_actq_take(wifi_actq_t *q, wifi_actreq_t *out) {
  while (q->rq_n > 0) {
    wifi_actreq_t *r = &q->req[q->rq_head];
    q->rq_head = (q->rq_head + 1) % WIFI_ACTQ;
    q->rq_n--;
    if (r->session != q->session) continue;
    *out = *r;
    return true;
  }
  return false;
}

bool wifi_actq_stale(const wifi_actq_t *q, uint32_t session) { return session != q->session; }

void wifi_actq_put(wifi_actq_t *q, const wifi_actres_t *r) {
  if (r->session != q->session) return;
  if (q->rs_n >= WIFI_ACTQ) {
    /* can't happen (results <= pending IPs <= WIFI_ACTQ); if it did, unpend so the IP is retried */
    unpend(q, q->res[q->rs_head].ip);
    q->rs_head = (q->rs_head + 1) % WIFI_ACTQ;
    q->rs_n--;
  }
  q->res[(q->rs_head + q->rs_n) % WIFI_ACTQ] = *r;
  q->rs_n++;
}

bool wifi_actq_get(wifi_actq_t *q, wifi_actres_t *out) {
  while (q->rs_n > 0) {
    wifi_actres_t *r = &q->res[q->rs_head];
    q->rs_head = (q->rs_head + 1) % WIFI_ACTQ;
    q->rs_n--;
    if (r->session != q->session) continue;
    unpend(q, r->ip);
    *out = *r;
    return true;
  }
  return false;
}
