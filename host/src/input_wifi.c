/* ELRS / Crossfire WiFi joystick receiver: see input_wifi.h. The protocol is the vendored core
 * (third_party/elrs-wifi): wifi_proto.c, and wifi_net.c for the activation POST. */
#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "input_wifi.h"
#include "wifi_net.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/uio.h>
#include <time.h>
#endif

#define DRAIN_MAX 256
#define BIND_TRIES 5
#define BIND_RETRY_US 300000
#define BIND_ERROR_RETRY_US 5000000
#define RECV_ERR_LOG_US 60000000

struct wifi {
  app_t *app;
  wifi_config_t cfg;
  wifi_proto_t p;
  /* socket (frame loop) */
  plat_sock sock;
  bool running;
  int bind_tries;
  int64_t bind_at;
  bool bind_error;
  bool bad_port;
  /* the poll in progress: where applied frames go */
  insrc_t *cur;
  int64_t cur_now;
  uint32_t name_ip;
  wifi_kind_t name_kind;
  char name[64];
  int64_t last_arr;
  /* pauses in the stick data (WIFI_GAP_US or more): counted, logged at most every GAP_LOG_US */
  int64_t gap_prev;                /* arrival of the previous frame of gap_src, 0 = none */
  uint32_t gap_src;
  uint32_t gaps, gaps_unlogged;
  int64_t gap_max_us, gap_logged_at;
  uint32_t posts, posts_ok, posts_failed;
  int last_err;
  int64_t last_err_us;
  /* the worker and its queue (mu guards q and quit) */
  plat_mutex mu;
  plat_cond cv;
  wifi_actq_t q;
  bool quit;
  bool worker_ok;
  plat_thread th;
  uint8_t buf[2048];
};

static void wlog(wifi_t *w, const char *fmt, ...)
#ifdef __GNUC__
  __attribute__((format(printf, 2, 3)))
#endif
  ;
static void wlog(wifi_t *w, const char *fmt, ...) {
  char line[400];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  app_log(w->app, "wifi: %s", line);
}

void wifi_config_default(wifi_config_t *c) {
  memset(c, 0, sizeof *c);
  c->port = WIFI_PORT_DEFAULT;
  c->http_port = WIFI_HTTP_PORT_DEFAULT;
  c->interval_us = WIFI_INTERVAL_DEFAULT;
  c->channels = WIFI_CHANNELS_DEFAULT;
}

static void config_clean(wifi_config_t *out, const wifi_config_t *in) {
  if (!in) { wifi_config_default(out); return; }
  *out = *in;
  out->module_ip[sizeof out->module_ip - 1] = 0;
  if (!out->http_port) out->http_port = WIFI_HTTP_PORT_DEFAULT;
  out->interval_us = wifi_clamp_interval(out->interval_us);
  out->channels = wifi_clamp_channels(out->channels);
}

/* ================================================================ HTTP worker */
/* wifi_net_post asks this about every 100 ms: a stop or close ends a hanging request quickly */
typedef struct { wifi_t *w; uint32_t session; } post_ctx_t;

static bool post_aborted(void *ud) {
  const post_ctx_t *c = ud;
  plat_mutex_lock(&c->w->mu);
  bool a = c->w->quit || wifi_actq_stale(&c->w->q, c->session);
  plat_mutex_unlock(&c->w->mu);
  return a;
}

static void *worker(void *arg) {
  wifi_t *w = arg;
  plat_thread_lower_priority();       /* threads inherit the frame loop's SCHED_FIFO */
  plat_mutex_lock(&w->mu);
  while (!w->quit) {
    wifi_actreq_t rq;
    if (!wifi_actq_take(&w->q, &rq)) {
      plat_cond_wait(&w->cv, &w->mu, -1);
      continue;
    }
    plat_mutex_unlock(&w->mu);
    /* joystick_begin, after a joystick_end for a restart; a 2xx is success (the body is not checked) */
    wifi_actres_t rs;
    post_ctx_t ctx = { w, rq.session };
    wifi_net_post(&rq, &rs, WIFI_POST_TIMEOUT_MS, post_aborted, &ctx);
    plat_mutex_lock(&w->mu);
    wifi_actq_put(&w->q, &rs);        /* dropped when the session changed meanwhile */
  }
  plat_mutex_unlock(&w->mu);
  return NULL;
}

/* ================================================================ protocol hooks */
#define GAP_LOG_US 5000000

/* the WiFi signal level in dBm from /proc/net/wireless (first interface listed), or 0 */
static int wifi_signal_dbm(void) {
#ifdef __linux__
  FILE *f = fopen("/proc/net/wireless", "r");
  if (!f) return 0;
  char line[256];
  int dbm = 0;
  for (int i = 0; fgets(line, sizeof line, f); i++) {
    if (i < 2) continue;                                     /* two header lines */
    char ifn[32];
    unsigned st;
    float link, level;
    if (sscanf(line, " %31[^:]: %x %f %f", ifn, &st, &link, &level) == 4) { dbm = (int)level; break; }
  }
  fclose(f);
  return dbm;
#else
  return 0;
#endif
}

/* A pause in the stream: a WiFi scan (the web page's, or the WiFi chip's own background scan when
 * the signal is weak), interference, or the module. The log says when, so it can be matched with
 * what else happened. Runs on the frame loop only when a gap ends (a /proc read, no disk). */
static void note_gap(wifi_t *w, int64_t gap_us, int64_t now) {
  w->gaps++;
  if (gap_us > w->gap_max_us) w->gap_max_us = gap_us;
  if (w->gap_logged_at && now - w->gap_logged_at < GAP_LOG_US) { w->gaps_unlogged++; return; }
  char more[48] = "", sig[32] = "";
  if (w->gaps_unlogged) snprintf(more, sizeof more, ", %u shorter or earlier ones not logged", w->gaps_unlogged);
  int dbm = wifi_signal_dbm();
  if (dbm) snprintf(sig, sizeof sig, ", Pi WiFi signal %d dBm", dbm);
  wlog(w, "the stick data paused for %.2f s (%s%s%s)", gap_us / 1e6, w->name, sig, more);
  w->gaps_unlogged = 0;
  w->gap_logged_at = now;
}

static void h_log(void *ud, const char *msg) {
  wifi_t *w = ud;
  app_log(w->app, "wifi: %s", msg);
}

static void h_frame(void *ud, uint32_t src, const uint16_t *raw, int n) {
  wifi_t *w = ud;
  insrc_t *s = w->cur;
  if (!s) return;
  int64_t arr = w->p.last_arrival_us;                          /* this frame's (set just before the hook) */
  if (w->gap_prev && src == w->gap_src && arr - w->gap_prev >= WIFI_GAP_US) note_gap(w, arr - w->gap_prev, w->cur_now);
  w->gap_prev = arr;
  w->gap_src = src;
  wifi_kind_t k = wifi_proto_kind(&w->p, src);
  if (src != w->name_ip || k != w->name_kind || !w->name[0]) {
    char ip[16];
    wifi_ip_str(src, ip);
    snprintf(w->name, sizeof w->name, "%s %s", wifi_kind_name(k), ip);
    w->name_ip = src;
    w->name_kind = k;
  }
  int16_t ch[INPUT_CH];
  for (int i = 0; i < INPUT_CH; i++) ch[i] = i < n ? wifi_raw_to_stick(raw[i]) : 0;
  s->kind = INSRC_WIFI;
  s->present = true;
  s->fixed_aetr = false;
  if (strcmp(s->name, w->name)) snprintf(s->name, sizeof s->name, "%s", w->name);
  insrc_update(s, ch, n, w->cur_now);
  for (int i = n; i < INPUT_CH; i++) s->ch[i] = 0;
  s->nch = (uint8_t)n;
}

static int h_activate(void *ud, const wifi_act_t *a) {
  wifi_t *w = ud;
  if (!w->worker_ok) return WIFI_ACT_FAILED;
  if (w->sock == PLAT_BADSOCK) return WIFI_ACT_BUSY;        /* not listening: POST after the bind */
  char body[64];
  if (wifi_build_body(body, sizeof body, "joystick_begin", a->interval_us, a->channels) < 0)
    return WIFI_ACT_FAILED;
  plat_mutex_lock(&w->mu);
  bool was_pending = wifi_actq_is_pending(&w->q, a->ip);
  int r = wifi_actq_push(&w->q, a->ip, w->cfg.http_port, body, a->restart);
  if (r == WIFI_ACT_QUEUED) plat_cond_signal(&w->cv);
  plat_mutex_unlock(&w->mu);
  /* a restart that took over a request still in the queue is still one activation (one result) */
  if (r == WIFI_ACT_QUEUED && !was_pending) w->posts++;
  return r;
}

/* ================================================================ socket */
static void log_local_addresses(wifi_t *w) {
#ifdef __linux__
  char line[300];
  int n = wifi_net_local_ipv4(line, sizeof line);
  if (n) {
    wlog(w, "This Pi: %s", line);
    if (n > 1) wlog(w, "Multiple adapters - your module must be on one of these networks.");
  }
#else
  (void)w;      /* a test platform: the PC's addresses don't matter here */
#endif
}

static void try_bind(wifi_t *w, int64_t now) {
  plat_sock s = plat_udp_bind(w->cfg.port, false, false);
  if (s == PLAT_BADSOCK) {
    int e = wifi_net_errno();
    if (++w->bind_tries < BIND_TRIES) { w->bind_at = now + BIND_RETRY_US; return; }
    w->bind_at = now + BIND_ERROR_RETRY_US;
    if (!w->bind_error) {
      char detail[64], why[64];
      wifi_net_err_text(e, why, sizeof why);
      w->bind_error = true;
      wlog(w, "ERROR: cannot listen on UDP port %u - it may already be in use (another instance, or another app "
              "on this port).%s%s", w->cfg.port, why[0] ? " " : "", why);
      snprintf(detail, sizeof detail, "UDP port %u in use", w->cfg.port);
      wifi_proto_set_state(&w->p, WIFI_ST_ERROR, detail);
    }
    return;
  }
#if defined(__linux__) && defined(SO_TIMESTAMPNS)
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof one);
#endif
#ifndef _WIN32
  fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
  w->sock = s;
  w->bind_tries = 0;
  w->bind_error = false;
  wlog(w, "Listening for joystick data on UDP %u", w->cfg.port);
  log_local_addresses(w);
  wifi_proto_set_state(&w->p, WIFI_ST_SEARCHING, "waiting for module");
  if (w->p.target) wifi_proto_ensure(&w->p, w->p.target, now);
}

static void recv_error(wifi_t *w, int e, int64_t now) {
  if (e == w->last_err && now - w->last_err_us < RECV_ERR_LOG_US) return;
  char t[64];
  wifi_net_err_text(e, t, sizeof t);
  wlog(w, "recv error: %s", t);
  w->last_err = e;
  w->last_err_us = now;
}

/* Drain what is queued, in order (the newest valid frame wins). On Linux each datagram carries its
 * kernel receive time: one drain per frame would otherwise stamp a whole batch with one time and
 * make the rate and jitter meaningless. */
static void drain(wifi_t *w, int64_t now) {
#if defined(__linux__) && defined(SO_TIMESTAMPNS)
  int64_t mono = 0, real_ns = 0;
  bool clocks = false;
#endif
  for (int i = 0; i < DRAIN_MAX; i++) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    size_t len;
    int64_t arrival = now;
#ifdef _WIN32
    int sl = sizeof sa;
    int n = recvfrom(w->sock, (char *)w->buf, (int)sizeof w->buf, 0, (struct sockaddr *)&sa, &sl);
    if (n < 0) {
      int e = WSAGetLastError();
      if (e == WSAEWOULDBLOCK) break;
      if (e == WSAEMSGSIZE) n = (int)sizeof w->buf;          /* truncated: the head is what counts */
      else if (e == WSAECONNRESET || e == WSAEINTR) continue;  /* ICMP echo of an old send; signal */
      else { recv_error(w, e, now); break; }
    }
    len = (size_t)n;
#else
    struct iovec iov = { w->buf, sizeof w->buf };
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(struct timespec))]; } ctl;
    struct msghdr m;
    memset(&m, 0, sizeof m);
    m.msg_name = &sa;
    m.msg_namelen = sizeof sa;
    m.msg_iov = &iov;
    m.msg_iovlen = 1;
    m.msg_control = ctl.b;
    m.msg_controllen = sizeof ctl.b;
    ssize_t n = recvmsg(w->sock, &m, MSG_DONTWAIT);
    if (n < 0) {
      int e = errno;
      if (e == EAGAIN || e == EWOULDBLOCK) break;
      if (e == EINTR || e == ECONNREFUSED) continue;
      recv_error(w, e, now);
      break;
    }
    len = (size_t)n;                                           /* MSG_TRUNC: the copied head */
#if defined(__linux__) && defined(SO_TIMESTAMPNS)
    /* the timestamp is the only control message this socket enables (and musl's CMSG_NXTHDR
     * trips -Wsign-compare) */
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS &&
        c->cmsg_len >= CMSG_LEN(sizeof(struct timespec))) {
      struct timespec ts;
      memcpy(&ts, CMSG_DATA(c), sizeof ts);
      if (!clocks) {
        struct timespec rn;
        clock_gettime(CLOCK_REALTIME, &rn);
        mono = plat_now_us();
        real_ns = (int64_t)rn.tv_sec * 1000000000 + rn.tv_nsec;
        clocks = true;
      }
      int64_t age_us = (real_ns - ((int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec)) / 1000;
      if (age_us < 0) age_us = 0;
      if (age_us > 5000000) age_us = 5000000;
      arrival = mono - age_us;
    }
#endif
#endif
    if (sa.sin_family != AF_INET) continue;
    if (arrival < w->last_arr) arrival = w->last_arr;
    w->last_arr = arrival;
    wifi_proto_packet(&w->p, w->buf, len, sa.sin_addr.s_addr, now, arrival);
  }
}

static void collect_results(wifi_t *w, int64_t now) {
  wifi_actres_t res[WIFI_ACTQ];
  int n = 0;
  plat_mutex_lock(&w->mu);
  while (n < WIFI_ACTQ && wifi_actq_get(&w->q, &res[n])) n++;
  plat_mutex_unlock(&w->mu);
  for (int i = 0; i < n; i++) {
    const wifi_actres_t *r = &res[i];
    char ip[16];
    wifi_ip_str(r->ip, ip);
    /* a restart: its joystick_end first (a failure there still lets the joystick_begin go out) */
    if (r->restart) {
      if (r->end_status > 0) wlog(w, "Stream end POST -> %s: %d", ip, r->end_status);
      else wlog(w, "Stream end POST failed for %s: %s", ip, r->end_err[0] ? r->end_err : "failed");
    }
    if (r->status > 0)
      wlog(w, "Activation POST -> %s: %d%s%s", ip, r->status, r->reason[0] ? " " : "", r->reason);
    else
      wlog(w, "Activation failed for %s: %s", ip, r->err[0] ? r->err : "failed");
    if (r->ok) w->posts_ok++;
    else w->posts_failed++;
    wifi_proto_activation_done(&w->p, r->ip, r->ok, now);
  }
}

/* ================================================================ API */
static void apply_target(wifi_t *w, int64_t now) {
  if (w->running) {
    if (wifi_proto_set_target(&w->p, w->cfg.module_ip, now) < 0) wifi_proto_set_target(&w->p, NULL, now);
    return;
  }
  /* stopped: just remember it; the POST goes out after the next bind */
  uint32_t ip = 0;
  int r = wifi_parse_ip(w->cfg.module_ip, &ip);
  if (r > 0 && ip) w->p.target = ip;
  else {
    w->p.target = 0;
    if (r < 0 || (r > 0 && !ip)) wifi_proto_set_target(&w->p, w->cfg.module_ip, now);   /* logs "Invalid module IP" */
  }
}

wifi_t *wifi_open(app_t *app, const wifi_config_t *cfg) {
  wifi_t *w = calloc(1, sizeof *w);
  if (!w) return NULL;
  w->app = app;
  w->sock = PLAT_BADSOCK;
  config_clean(&w->cfg, cfg);
  wifi_hooks_t h = { w, h_log, NULL, h_frame, NULL, h_activate };
  wifi_proto_init(&w->p, &h);
  wifi_proto_set_request(&w->p, w->cfg.interval_us, w->cfg.channels, 0);   /* not locked: nothing sent */
  plat_mutex_init(&w->mu);
  plat_cond_init(&w->cv);
  wifi_actq_init(&w->q);
  plat_net_init();
  /* once, here: a thread per request would fault its locked stack in on the frame loop (mlockall) */
  w->worker_ok = plat_thread_start(&w->th, worker, w, false) == 0;
  if (!w->worker_ok) wlog(w, "ERROR: cannot start the activation thread - modules must already be streaming");
  apply_target(w, 0);
  return w;
}

void wifi_close(wifi_t *w) {
  if (!w) return;
  wifi_stop(w);
  plat_mutex_lock(&w->mu);
  w->quit = true;
  wifi_actq_new_session(&w->q);
  plat_cond_broadcast(&w->cv);
  plat_mutex_unlock(&w->mu);
  if (w->worker_ok) plat_thread_join(w->th);     /* a request in flight notices within 100 ms */
  plat_mutex_destroy(&w->mu);
  free(w);
}

void wifi_start(wifi_t *w, int64_t now) {
  if (!w || w->running) return;
  if (!w->cfg.port) {
    if (!w->bad_port) wlog(w, "Invalid port \"%u\" - using 1-65535.", w->cfg.port);
    w->bad_port = true;
    wifi_proto_set_state(&w->p, WIFI_ST_ERROR, "invalid UDP port 0");
    return;
  }
  w->bad_port = false;
  plat_mutex_lock(&w->mu);
  wifi_actq_new_session(&w->q);
  plat_mutex_unlock(&w->mu);
  wifi_proto_reset(&w->p);
  w->running = true;
  w->bind_tries = 0;
  w->bind_error = false;
  w->last_arr = 0;
  try_bind(w, now);
}

void wifi_stop(wifi_t *w) {
  if (!w || !w->running) return;
  w->running = false;
  plat_sock_close(w->sock);
  w->sock = PLAT_BADSOCK;
  plat_mutex_lock(&w->mu);
  wifi_actq_new_session(&w->q);       /* the worker aborts a request in flight */
  plat_mutex_unlock(&w->mu);
  wifi_proto_reset(&w->p);
  wifi_proto_set_state(&w->p, WIFI_ST_STOPPED, "");
  wlog(w, "Stopped. (The module keeps broadcasting until powered off.)");
}

void wifi_poll(wifi_t *w, int64_t now, insrc_t *src) {
  if (!w) return;
  w->cur = src;
  w->cur_now = now;
  if (w->running && w->sock == PLAT_BADSOCK && now >= w->bind_at) try_bind(w, now);
  if (w->sock != PLAT_BADSOCK) drain(w, now);
  collect_results(w, now);
  if (w->sock != PLAT_BADSOCK) wifi_proto_tick(&w->p, now);
  if (w->running) wifi_proto_poll_stats(&w->p, now);
  if (src && !w->p.bound && src->present) {
    /* lock released or stopped: the values are forgotten (and a later lock is no "gap") */
    w->gap_prev = 0;
    src->present = false;
    src->nch = 0;
    src->name[0] = 0;
    memset(src->ch, 0, sizeof src->ch);
  }
  w->cur = NULL;
}

void wifi_configure(wifi_t *w, const wifi_config_t *cfg, int64_t now) {
  if (!w) return;
  wifi_config_t c;
  config_clean(&c, cfg);
  uint32_t a = 0, b = 0;
  int ra = wifi_parse_ip(c.module_ip, &a), rb = wifi_parse_ip(w->cfg.module_ip, &b);
  bool ip_changed = ra != rb || a != b || (ra < 0 && strcmp(c.module_ip, w->cfg.module_ip));
  bool port_changed = c.port != w->cfg.port;
  w->cfg = c;
  /* The request before the target, so the joystick_begin a new target gets asks for it. Locked to a
   * module the new target replaces, there's nothing to restart: that module isn't ours any more. */
  if (ip_changed && ra > 0 && a && w->p.bound && a != w->p.bound_ip) {
    wifi_proto_store_request(&w->p, c.interval_us, c.channels);
  } else {
    wifi_proto_set_request(&w->p, c.interval_us, c.channels, now);   /* locked: restarts it so they apply */
  }
  if (ip_changed) apply_target(w, now);
  if (port_changed && w->running) {
    wifi_stop(w);
    wifi_start(w, now);
  } else if (port_changed && w->bad_port) {
    wifi_start(w, now);
  }
}

static int first_pending(const wifi_t *w, uint32_t *ip) {
  plat_mutex *mu = (plat_mutex *)&w->mu;
  plat_mutex_lock(mu);
  int n = w->q.npending;
  if (n && ip) *ip = w->q.pending[0];
  plat_mutex_unlock(mu);
  return n;
}

void wifi_fill_status(const wifi_t *w, app_status_t *st) {
  if (!w) {
    snprintf(st->wifi_state, sizeof st->wifi_state, "off");
    st->wifi_module[0] = 0;
    st->wifi_rate_hz = 0;
    st->wifi_dropped = 0;
    st->wifi_gaps = 0;
    st->wifi_gap_max_ms = 0;
    return;
  }
  const wifi_proto_t *p = &w->p;
  char ip[16];
  uint32_t pend = 0;
  snprintf(st->wifi_module, sizeof st->wifi_module, "%s", p->bound ? w->name : "");
  st->wifi_rate_hz = p->bound ? (float)p->last_hz : 0.0f;
  st->wifi_dropped = p->dropped_filter;
  st->wifi_gaps = w->gaps;
  st->wifi_gap_max_ms = (uint32_t)(w->gap_max_us / 1000);
  switch (p->state) {
    case WIFI_ST_STOPPED: snprintf(st->wifi_state, sizeof st->wifi_state, "off"); break;
    case WIFI_ST_ERROR: snprintf(st->wifi_state, sizeof st->wifi_state, "%s", p->detail); break;
    case WIFI_ST_STREAMING: snprintf(st->wifi_state, sizeof st->wifi_state, "streaming"); break;
    case WIFI_ST_SEARCHING:
      if (!strncmp(p->detail, "module found", 12)) {
        wifi_ip_str(p->nostick_ip, ip);
        snprintf(st->wifi_state, sizeof st->wifi_state, "no stick data from %s", ip);
      } else if (first_pending(w, &pend)) {
        wifi_ip_str(pend, ip);
        snprintf(st->wifi_state, sizeof st->wifi_state, "activating %s", ip);
      } else if (!strncmp(p->detail, "activated", 9)) {
        wifi_ip_str(p->first_act_ip, ip);
        snprintf(st->wifi_state, sizeof st->wifi_state, "no data from %s", ip);
      } else if (p->target) {
        wifi_ip_str(p->target, ip);
        snprintf(st->wifi_state, sizeof st->wifi_state, "connecting to %s", ip);
      } else {
        snprintf(st->wifi_state, sizeof st->wifi_state, "listening");
      }
      break;
  }
}

int64_t wifi_last_arrival_us(const wifi_t *w) {
  return w && w->p.seq ? w->p.last_arrival_us : 0;
}

void wifi_get_info(const wifi_t *w, wifi_info_t *o) {
  memset(o, 0, sizeof *o);
  if (!w) return;
  const wifi_proto_t *p = &w->p;
  o->running = w->running;
  o->listening = w->sock != PLAT_BADSOCK;
  o->port = w->cfg.port;
  o->state = p->state;
  snprintf(o->detail, sizeof o->detail, "%s", p->detail);
  o->bound = p->bound;
  o->source = p->bound ? p->bound_ip : 0;
  o->target = p->target;
  o->kind = p->bound ? wifi_proto_kind(p, p->bound_ip) : WIFI_KIND_UNKNOWN;
  if (p->bound) {
    snprintf(o->name, sizeof o->name, "%s", w->name);
    snprintf(o->module_name, sizeof o->module_name, "%s", wifi_proto_module_name(p, p->bound_ip));
  }
  o->rate_hz = p->last_hz;
  o->jitter_ms = p->last_jitter;
  o->frames_last_s = p->last_frames;
  o->rx = p->rx;
  o->beacons = p->beacons;
  o->malformed = p->malformed;
  o->dropped_filter = p->dropped_filter;
  o->dropped_other = p->dropped_other;
  o->applied = p->seq;
  o->posts = w->posts;
  o->posts_ok = w->posts_ok;
  o->posts_failed = w->posts_failed;
  o->pending = first_pending(w, NULL);
  o->interval_us = p->interval_us;
  o->channels = p->channels;
  o->restarts = p->restarts;
  o->nch = p->nch;
  memcpy(o->raw, p->raw, sizeof o->raw);
  o->last_arrival_us = wifi_last_arrival_us(w);
  o->gaps = w->gaps;
  o->gap_max_us = w->gap_max_us;
}
