/* The WiFi joystick receiver over real sockets (input_wifi.c + the vendored core, wifi_proto.c and
 * wifi_net.c).
 *
 * No arguments: a self-contained loopback test (build.sh test runs it). Frames and beacons come from
 * a local UDP socket (so the "module" is 127.0.0.1), activation POSTs go to a tiny HTTP server
 * thread; it also covers bind retry, port in use, stop/start, stale POST results, live changes,
 * the request rules (R1-R13 in wifi_proto.h: a new rate or channel count restarts a streaming ELRS module,
 * never Crossfire; a held restart, a failed one sent again, one that did not take restarted at the next
 * lock, the module IP of the locked module) and the release (R14: joystick_end on a stop, a port change and
 * the close, which waits for the answers at most WIFI_RELEASE_WAIT_US after the stop or Release that sent them;
 * Release and Connect, the release's outcome in the status, the program's next start listening again, the module
 * left by a switch, a release's 2 s time-out).
 *
 * With arguments: a probe for host/tests/wifi_live_test.py (and for trying a real module):
 *   test_wifi_live --port N --http-port N [--module-ip IP] [--interval-us N] [--channels N]
 *                  [--duration-ms N] [--poll-us N] [--stdin]
 *                  [--expect-streaming] [--expect-src IP] [--expect-nch N] [--min-frames N]
 *                  [--min-rate HZ] [--max-rate HZ]
 * It prints JSON lines on stdout ({"ev":"state"|"log"|"frame"|"stats"|"poll"|"exit", ...}) and exits
 * 0 as soon as the expectations are met, 1 when the duration ends first (0 without expectations). The
 * close's log lines (its release) come before "exit", which is always the last line.
 * --stdin reads commands, one per line: target <ip>|target, interval <us>, channels <n>,
 * http-port <n>, port <n>, stop, start, release, connect, quit. */
#include "t.h"
#include "input_wifi.h"

#include <stdarg.h>
#include <stdint.h>
#ifndef _WIN32
#include <sys/select.h>
#include <unistd.h>
#endif

static app_t app;

/* ---------------------------------------------------------------- helpers */
static int64_t vt;                                  /* virtual time added to the clock (lock timing tests) */
static int64_t now(void) { return plat_now_us() + vt; }

static char logbuf[16384];
static const char *app_logs(void) { app_log_copy(&app, logbuf, sizeof logbuf); return logbuf; }
static int count_in_log(const char *sub) {
  const char *s = app_logs();
  int c = 0;
  for (const char *p = s; (p = strstr(p, sub)); p += strlen(sub)) c++;
  return c;
}
static void clear_log(void) {
  plat_mutex_lock(&app.mu);
  app.log_len = 0;
  plat_mutex_unlock(&app.mu);

}

static int64_t poll_max_us;
static void poll1(wifi_t *w, insrc_t *s) {
  int64_t t0 = plat_now_us();
  wifi_poll(w, now(), s);
  int64_t d = plat_now_us() - t0;
  if (d > poll_max_us) poll_max_us = d;
}
static void poll_ms(wifi_t *w, insrc_t *s, int ms) {
  int64_t end = plat_now_us() + (int64_t)ms * 1000;
  do { poll1(w, s); plat_sleep_us(1000); } while (plat_now_us() < end);
}
static const char *status_of(wifi_t *w) {
  static app_status_t st;
  wifi_fill_status(w, &st);
  return st.wifi_state;
}
static const char *release_of(wifi_t *w) {           /* the last release's outcome (app_status_t.wifi_release) */
  static app_status_t st;
  wifi_fill_status(w, &st);
  return st.wifi_release;
}
#define POLL_UNTIL(w, s, ms, cond) do { int64_t e_ = plat_now_us() + (int64_t)(ms) * 1000; \
  while (!(cond) && plat_now_us() < e_) { poll1(w, s); plat_sleep_us(1000); } } while (0)

static plat_sock udp_tx;
static void udp_open(void) {
  udp_tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(0x7f000001);
  bind(udp_tx, (struct sockaddr *)&a, sizeof a);
}
static void udp_send(uint16_t port, const void *d, size_t n) {
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(0x7f000001);
  sendto(udp_tx, (const char *)d, (int)n, 0, (struct sockaddr *)&a, sizeof a);
}
static void send_frame(uint16_t port, int n, const int *v) {
  uint8_t b[64];
  b[0] = 1;
  b[1] = (uint8_t)n;
  for (int i = 0; i < n; i++) { b[2 + 2 * i] = (uint8_t)(v[i] & 0xFF); b[3 + 2 * i] = (uint8_t)(v[i] >> 8); }
  udp_send(port, b, 2 + 2 * (size_t)n);
}
static void send_same(uint16_t port, int n, int v) {
  int vals[16];
  for (int i = 0; i < n; i++) vals[i] = v;
  send_frame(port, n, vals);
}
static const uint8_t ELRS_BEACON[] = { 'E', 'L', 'R', 'S', 1, 0xf8, 0x2a, 10, 'U', 'n', 'i', 'f', 'i', 'e', 'd', ' ', 'T', 'X' };

/* ---------------------------------------------------------------- a tiny HTTP server (the module's) */
#define MH_BODIES 16
typedef struct {
  plat_sock ls;
  plat_mutex mu;
  bool quit;
  int status, delay_ms;
  int next_status, next_n;      /* the next next_n requests get next_status instead of status */
  int requests;
  char last[1024];
  char body[MH_BODIES][96];     /* the bodies of the first MH_BODIES requests, in order */
  plat_thread th;
} minihttp_t;

static bool mh_quit(minihttp_t *h) {
  plat_mutex_lock(&h->mu);
  bool q = h->quit;
  plat_mutex_unlock(&h->mu);
  return q;
}

static void *mh_thread(void *arg) {
  minihttp_t *h = arg;
  while (!mh_quit(h)) {
    fd_set f;
    FD_ZERO(&f);
    FD_SET(h->ls, &f);
    struct timeval tv = { 0, 20000 };
    if (select((int)h->ls + 1, &f, NULL, NULL, &tv) <= 0) continue;
    plat_sock c = accept(h->ls, NULL, NULL);
    if (c == PLAT_BADSOCK) continue;
    plat_sock_timeouts(c, 2000, 2000);
    char req[1024];
    size_t got = 0;
    for (;;) {
      int k = (int)recv(c, req + got, (int)(sizeof req - 1 - got), 0);
      if (k <= 0) break;
      got += (size_t)k;
      req[got] = 0;
      char *he = strstr(req, "\r\n\r\n");
      if (he) {
        long cl = wifi_http_content_length(req, (size_t)(he + 4 - req));
        if (cl < 0 || got >= (size_t)(he + 4 - req) + (size_t)cl) break;
      }
      if (got >= sizeof req - 1) break;
    }
    req[got] = 0;
    const char *b = strstr(req, "\r\n\r\n");
    plat_mutex_lock(&h->mu);
    if (h->requests < MH_BODIES) snprintf(h->body[h->requests], sizeof h->body[0], "%s", b ? b + 4 : "");
    h->requests++;
    snprintf(h->last, sizeof h->last, "%s", req);
    int status = h->status, delay = h->delay_ms;
    if (h->next_n > 0) { status = h->next_status; h->next_n--; }
    plat_mutex_unlock(&h->mu);
    for (int t = 0; t < delay && !mh_quit(h); t += 10) plat_sleep_us(10000);
    char resp[256];
    int n = snprintf(resp, sizeof resp, "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n"
                     "Connection: close\r\n\r\nok", status, status == 200 ? "OK" : status == 404 ? "Not Found" : "Error");
    send(c, resp, n, 0);
    plat_sock_close(c);
  }
  return NULL;
}

static void mh_start_at(minihttp_t *h, const char *ip, uint16_t port, int status) {
  memset(h, 0, sizeof *h);
  plat_mutex_init(&h->mu);
  h->status = status;
  h->ls = plat_tcp_listen(ip, port, 8);
  if (h->ls == PLAT_BADSOCK) { fprintf(stderr, "cannot listen on TCP %s:%u\n", ip, port); exit(2); }
  plat_thread_start(&h->th, mh_thread, h, false);
}
static void mh_start(minihttp_t *h, uint16_t port, int status) { mh_start_at(h, "127.0.0.1", port, status); }
static void mh_set(minihttp_t *h, int status, int delay_ms) {
  plat_mutex_lock(&h->mu);
  h->status = status;
  h->delay_ms = delay_ms;
  plat_mutex_unlock(&h->mu);
}
static void mh_set_next(minihttp_t *h, int status, int n) {
  plat_mutex_lock(&h->mu);
  h->next_status = status;
  h->next_n = n;
  plat_mutex_unlock(&h->mu);
}
static int mh_requests(minihttp_t *h) {
  plat_mutex_lock(&h->mu);
  int n = h->requests;
  plat_mutex_unlock(&h->mu);
  return n;
}
static void mh_last(minihttp_t *h, char *out, size_t cap) {
  plat_mutex_lock(&h->mu);
  snprintf(out, cap, "%s", h->last);
  plat_mutex_unlock(&h->mu);
}
static const char *mh_body(minihttp_t *h, int i) {      /* the body of request i (0 = the first), "" when none */
  static char out[96];
  plat_mutex_lock(&h->mu);
  snprintf(out, sizeof out, "%s", i >= 0 && i < h->requests && i < MH_BODIES ? h->body[i] : "");
  plat_mutex_unlock(&h->mu);
  return out;
}
static void mh_stop(minihttp_t *h) {
  plat_mutex_lock(&h->mu);
  h->quit = true;
  plat_mutex_unlock(&h->mu);
  plat_thread_join(h->th);
  plat_sock_close(h->ls);
  plat_mutex_destroy(&h->mu);
}

static uint32_t IP(const char *s) { uint32_t ip = 0; wifi_parse_ip(s, &ip); return ip; }

static wifi_t *open_with(uint16_t port, uint16_t http, const char *module_ip) {
  wifi_config_t c;
  wifi_config_default(&c);
  c.port = port;
  c.http_port = http;
  snprintf(c.module_ip, sizeof c.module_ip, "%s", module_ip ? module_ip : "");
  return wifi_open(&app, &c);
}

/* ---------------------------------------------------------------- self-test */
#define P_BASE 27461

static void test_flow(void) {
  const uint16_t U = P_BASE, HP = P_BASE + 20;
  minihttp_t mh;
  mh_start(&mh, HP, 200);
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, HP, "");
  CHECK(w != NULL);
  CHECK_STR(status_of(w), "off");
  wifi_start(w, now());
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK(in.running && in.listening);
  CHECK_INT(in.port, U);
  CHECK_STR(status_of(w), "listening");
  CHECK_INT(count_in_log("Listening for joystick data on UDP 27461"), 1);
  poll_ms(w, &src, 20);
  CHECK(!src.present);

  /* beacon -> POST */
  udp_send(U, ELRS_BEACON, sizeof ELRS_BEACON);
  bool saw_activating = false;
  int64_t e = plat_now_us() + 3000000;
  while (count_in_log("Activation POST -> 127.0.0.1: 200 OK") == 0 && plat_now_us() < e) {
    poll1(w, &src);
    if (!strcmp(status_of(w), "activating 127.0.0.1")) saw_activating = true;
    plat_sleep_us(500);
  }
  CHECK_INT(count_in_log("Activation POST -> 127.0.0.1: 200 OK"), 1);
  CHECK_INT(count_in_log("Discovery beacon from ELRS module 127.0.0.1 - activating"), 1);
  CHECK(saw_activating);
  CHECK_INT(mh_requests(&mh), 1);
  char req[1024];
  mh_last(&mh, req, sizeof req);
  CHECK(!strncmp(req, "POST /udpcontrol HTTP/1.1\r\n", 27));
  CHECK(strstr(req, "\r\nHost: 127.0.0.1:27481\r\n") != NULL);
  CHECK(strstr(req, "\r\nContent-Type: application/x-www-form-urlencoded\r\n") != NULL);
  CHECK(strstr(req, "\r\n\r\naction=joystick_begin&interval=4000&channels=16") != NULL);
  wifi_get_info(w, &in);
  CHECK_INT(in.posts, 1);
  CHECK_INT(in.posts_ok, 1);
  CHECK_INT(in.pending, 0);
  CHECK_STR(status_of(w), "listening");

  /* frames -> streaming, the source filled (MainViewModel: banner, tray, log) */
  int v16[16] = { 32767, 19, 0, 16393, 16393, 16393, 16393, 16393, 24570, 8216, 16393, 16393, 16393, 16393, 16393, 16393 };
  send_frame(U, 16, v16);
  POLL_UNTIL(w, &src, 1000, src.present);
  CHECK(src.present);
  CHECK_INT(src.kind, INSRC_WIFI);
  CHECK(!src.fixed_aetr);
  CHECK_STR(src.name, "ELRS 127.0.0.1");
  CHECK_INT(src.nch, 16);
  CHECK_INT(src.ch[0], 1024);
  CHECK_INT(src.ch[1], -1024);
  CHECK_INT(src.ch[2], -1024);
  CHECK_INT(src.ch[3], 0);
  CHECK_INT(src.ch[8], 511);
  CHECK_INT(src.ch[9], -511);
  CHECK(src.updates >= 1);
  CHECK_STR(status_of(w), "streaming");
  app_status_t st;
  memset(&st, 0, sizeof st);
  wifi_fill_status(w, &st);
  CHECK_STR(st.wifi_module, "ELRS 127.0.0.1");
  CHECK_INT(count_in_log("Joystick source locked to 127.0.0.1"), 1);
  wifi_get_info(w, &in);
  CHECK(in.bound && in.source == IP("127.0.0.1"));
  CHECK_INT(in.kind, WIFI_KIND_ELRS);
  CHECK_STR(in.module_name, "Unified TX");
  CHECK_STR(in.name, "ELRS 127.0.0.1");
  CHECK_INT(in.raw[0], 32767);
  CHECK(wifi_last_arrival_us(w) > 0 && wifi_last_arrival_us(w) <= plat_now_us());

  /* count change: channels beyond the frame read 0 */
  int v4[4] = { 100, 200, 300, 400 };
  uint32_t upd = src.updates;
  send_frame(U, 4, v4);
  POLL_UNTIL(w, &src, 1000, src.updates != upd);
  CHECK_INT(src.nch, 4);
  bool zero = true;
  for (int i = 4; i < INPUT_CH; i++) zero = zero && src.ch[i] == 0;
  CHECK(zero);
  /* an ELRS module streaming another channel count than asked for (another app's request) is restarted
   * once per session: joystick_end, then our joystick_begin */
  CHECK_INT(count_in_log("127.0.0.1 streams 4 channels, 16 asked for (another app's request?) - restarting its stream"), 1);
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.1: 200 OK") >= 2);
  CHECK_INT(count_in_log("Stream end POST -> 127.0.0.1: 200"), 1);
  CHECK_INT(count_in_log("Activation POST -> 127.0.0.1: 200 OK"), 2);
  CHECK_INT(mh_requests(&mh), 3);
  CHECK_STR(mh_body(&mh, 1), "action=joystick_end");
  CHECK_STR(mh_body(&mh, 2), "action=joystick_begin&interval=4000&channels=16");
  wifi_get_info(w, &in);
  CHECK_INT(in.posts, 2);                          /* a restart is one activation */
  CHECK_INT(in.posts_ok, 2);
  CHECK_INT(in.restarts, 1);
  CHECK_INT(in.pending, 0);
  CHECK(in.bound);                                 /* the lock is kept */

  /* a steady stream gives a rate */
  for (int i = 0; i < 60; i++) { send_same(U, 8, 16393); poll_ms(w, &src, 4); }
  poll_ms(w, &src, 1100);
  wifi_get_info(w, &in);
  CHECK(in.rate_hz > 50 && in.rate_hz < 400);      /* ~250 Hz sent; a loaded machine sends slower */
  CHECK_INT(in.gaps, 0);                           /* a steady stream has no pauses */
  fprintf(stderr, "  stream rate %.1f Hz, jitter %.2f ms\n", in.rate_hz, in.jitter_ms);

  /* silence: held, then after 3 s released and forgotten */
  upd = src.updates;
  send_same(U, 8, 16393);
  POLL_UNTIL(w, &src, 1000, src.updates != upd);
  /* the 1.1 s without frames before this one was a pause: counted, logged with how long */
  wifi_get_info(w, &in);
  CHECK_INT(in.gaps, 1);
  CHECK(in.gap_max_us >= 1000000 && in.gap_max_us < 3000000);
  CHECK_INT(count_in_log("the stick data paused for 1."), 1);
  wifi_fill_status(w, &st);
  CHECK_INT(st.wifi_gaps, 1);
  CHECK(st.wifi_gap_max_ms >= 1000 && st.wifi_gap_max_ms < 3000);
  vt += 2500000;
  poll1(w, &src);
  CHECK(src.present);
  CHECK_STR(status_of(w), "streaming");
  vt += 600000;
  poll1(w, &src);
  CHECK(!src.present);
  CHECK_INT(src.nch, 0);
  CHECK_STR(status_of(w), "listening");
  CHECK_INT(count_in_log("Source 127.0.0.1 went quiet - releasing lock"), 1);
  wifi_fill_status(w, &st);
  CHECK_STR(st.wifi_module, "");
  CHECK(st.wifi_rate_hz == 0);

  /* placeholder frames: no source, a status that says why */
  for (int i = 0; i < 3; i++) send_same(U, 16, 0xF26A);
  poll_ms(w, &src, 30);
  CHECK(!src.present);
  CHECK_STR(status_of(w), "no stick data from 127.0.0.1");
  wifi_fill_status(w, &st);
  CHECK_INT(st.wifi_dropped, 3);
  CHECK_INT(count_in_log("no stick data"), 1);

  /* garbage, truncated and oversized datagrams never hurt */
  uint8_t junk[3000];
  uint32_t x = 99;
  for (int i = 0; i < 200; i++) {
    size_t n = (size_t)(i * 13 % 70);
    for (size_t k = 0; k < n; k++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; junk[k] = (uint8_t)x; }
    udp_send(U, junk, n);
  }
  memset(junk, 0, sizeof junk);
  junk[0] = 1;
  junk[1] = 4;
  udp_send(U, junk, sizeof junk);          /* bigger than the receive buffer: the head still counts */
  poll_ms(w, &src, 50);
  wifi_get_info(w, &in);
  CHECK(in.malformed > 50);
  CHECK(src.present);                      /* the oversized frame was a valid 4-channel frame */
  CHECK_INT(src.nch, 4);

  /* the drain is capped per poll; the rest comes next poll, in order */
  wifi_get_info(w, &in);
  uint32_t rx0 = in.rx;
  for (int i = 0; i < 300; i++) send_same(U, 4, 1000 + i);
  plat_sleep_us(20000);
  poll1(w, &src);
  wifi_get_info(w, &in);
  CHECK_INT(in.rx - rx0, 256);
  poll1(w, &src);
  wifi_get_info(w, &in);
  CHECK_INT(in.rx - rx0, 300);
  CHECK_INT(in.raw[0], 1299);

  /* stop: the module streaming to us is released (R14: joystick_end alone, its answer collected by the polls),
   * Stopped, the port is free again; start twice is harmless */
  wifi_stop(w);
  CHECK_STR(status_of(w), "off");
  CHECK_INT(count_in_log("Releasing 127.0.0.1: joystick_end, so it stops streaming and broadcasts again"), 1);
  CHECK_INT(count_in_log("Stopped.\n"), 1);
  POLL_UNTIL(w, &src, 3000, count_in_log("Released 127.0.0.1") > 0);
  CHECK_INT(count_in_log("Released 127.0.0.1: it broadcasts again, any app can connect to it"), 1);
  CHECK_INT(mh_requests(&mh), 4);
  CHECK_STR(mh_body(&mh, 3), "action=joystick_end");
  wifi_get_info(w, &in);
  CHECK_INT(in.releases_ok, 1);
  CHECK_INT(in.releasing, 0);
  CHECK_INT(in.posts, 2);                          /* a release is no activation */
  poll1(w, &src);
  CHECK(!src.present);
  wifi_get_info(w, &in);
  CHECK(!in.running && !in.listening);
  plat_sock b = plat_udp_bind(U, false, false);
  CHECK(b != PLAT_BADSOCK);
  plat_sock_close(b);
  wifi_stop(w);
  CHECK_INT(count_in_log("Stopped."), 1);
  wifi_start(w, now());
  wifi_start(w, now());
  wifi_get_info(w, &in);
  CHECK(in.running && in.listening);
  CHECK_INT(count_in_log("Listening for joystick data on UDP 27461"), 2);
  /* a module that streamed on (it missed its joystick_end) relocks at once after a restart, without a POST
   * (8 channels never restarted it again in the first session; this one streams what is asked for) */
  send_same(U, 16, 16393);
  POLL_UNTIL(w, &src, 1000, src.present);
  CHECK(src.present);
  poll_ms(w, &src, 30);
  CHECK_INT(mh_requests(&mh), 4);
  CHECK_INT(count_in_log("restarting its stream"), 1);
  /* the close (the program's exit) releases it and waits for the answer */
  wifi_close(w);
  CHECK_INT(mh_requests(&mh), 5);
  CHECK_STR(mh_body(&mh, 4), "action=joystick_end");
  CHECK_INT(count_in_log("Released 127.0.0.1: it broadcasts again"), 2);
  mh_stop(&mh);
}

static void test_port_in_use(void) {
  const uint16_t U = P_BASE + 1;
  clear_log();
  vt = 0;
  plat_sock blocker = plat_udp_bind(U, false, false);
  CHECK(blocker != PLAT_BADSOCK);
  wifi_t *w = open_with(U, 80, "");
  wifi_start(w, now());
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK(in.running && !in.listening);
  CHECK_INT(count_in_log("ERROR"), 0);       /* 5 quick tries first */
  for (int i = 0; i < 4; i++) { vt += 300000; poll1(w, NULL); }
  CHECK_STR(status_of(w), "UDP port 27462 in use");
  CHECK_INT(count_in_log("ERROR: cannot listen on UDP port 27462 - it may already be in use"), 1);
  wifi_get_info(w, &in);
  CHECK_INT(in.state, WIFI_ST_ERROR);
  for (int i = 0; i < 10; i++) { vt += 400000; poll1(w, NULL); }    /* 4 s: no retry yet, no log spam */
  CHECK_INT(count_in_log("ERROR"), 1);
  plat_sock_close(blocker);
  vt += 1100000;
  poll1(w, NULL);
  CHECK_STR(status_of(w), "listening");
  CHECK_INT(count_in_log("Listening for joystick data on UDP 27462"), 1);
  wifi_close(w);
}

static void test_explicit_refused(void) {
  const uint16_t U = P_BASE + 2, HP = P_BASE + 22;      /* nothing listens on HP */
  clear_log();
  vt = 0;
  poll_max_us = 0;
  wifi_t *w = open_with(U, HP, "127.0.0.1");
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK(in.target == IP("127.0.0.1"));
  CHECK_INT(in.posts, 0);                      /* nothing goes out before the bind */
  wifi_start(w, now());
  wifi_get_info(w, &in);
  CHECK_INT(in.posts, 1);                      /* POST right after the bind */
  CHECK_STR(status_of(w), "activating 127.0.0.1");
  POLL_UNTIL(w, NULL, 6000, count_in_log("Activation failed for 127.0.0.1") > 0);
  CHECK_INT(count_in_log("Activation failed for 127.0.0.1: connection refused"), 1);
  CHECK_STR(status_of(w), "connecting to 127.0.0.1");
  CHECK(poll_max_us < 200000);       /* a blocking connect would take seconds; VMs stall for tens of ms */
  fprintf(stderr, "  refused after the POST; poll max %lld us\n", (long long)poll_max_us);
  /* explicit mode re-posts every 5 s while no data arrives */
  wifi_get_info(w, &in);
  CHECK_INT(in.posts, 1);
  CHECK_INT(in.posts_failed, 1);
  vt += 5000000;
  poll1(w, NULL);
  wifi_get_info(w, &in);
  CHECK_INT(in.posts, 2);
  /* close while a request may still be in flight: prompt */
  int64_t t0 = plat_now_us();
  wifi_close(w);
  CHECK(plat_now_us() - t0 < 1000000);
}

static void test_404_then_hint(void) {
  const uint16_t U = P_BASE + 3, HP = P_BASE + 23;
  minihttp_t mh;
  mh_start(&mh, HP, 404);
  clear_log();
  vt = 0;
  wifi_t *w = open_with(U, HP, "");
  wifi_start(w, now());
  udp_send(U, "VELOCIDRONE", 11);
  POLL_UNTIL(w, NULL, 3000, count_in_log("Activation POST") > 0);
  CHECK_INT(count_in_log("Discovery beacon from Crossfire module 127.0.0.1 - activating"), 1);
  CHECK_INT(count_in_log("Activation POST -> 127.0.0.1: 404 Not Found"), 1);
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK_INT(in.posts_failed, 1);
  vt += 4500000;
  poll1(w, NULL);
  CHECK_INT(count_in_log("!! Activated OK"), 0);          /* a failed POST starts no timer */
  /* beacons within 5 s of the POST are throttled */
  udp_send(U, "VELOCIDRONE", 11);
  poll_ms(w, NULL, 50);
  CHECK_INT(mh_requests(&mh), 1);
  mh_set(&mh, 200, 0);
  vt += 600000;
  udp_send(U, "VELOCIDRONE", 11);
  POLL_UNTIL(w, NULL, 3000, count_in_log("Activation POST -> 127.0.0.1: 200 OK") > 0);
  CHECK_INT(mh_requests(&mh), 2);
  vt += 4100000;
  poll1(w, NULL);
  CHECK_INT(count_in_log("!! Activated OK but NO data arriving"), 1);
  CHECK_STR(status_of(w), "no data from 127.0.0.1");
  poll1(w, NULL);
  CHECK_INT(count_in_log("!! Activated OK"), 1);
  wifi_close(w);
  mh_stop(&mh);
}

static void test_stale_result_discarded(void) {
  const uint16_t U = P_BASE + 4, HP = P_BASE + 24;
  minihttp_t mh;
  mh_start(&mh, HP, 200);
  mh_set(&mh, 200, 600);
  clear_log();
  vt = 0;
  wifi_t *w = open_with(U, HP, "");
  wifi_start(w, now());
  udp_send(U, ELRS_BEACON, sizeof ELRS_BEACON);
  POLL_UNTIL(w, NULL, 3000, mh_requests(&mh) > 0);
  CHECK_INT(mh_requests(&mh), 1);
  wifi_stop(w);                  /* the reply is still 600 ms away */
  wifi_start(w, now());
  poll_ms(w, NULL, 1200);
  CHECK_INT(count_in_log("Activation"), 0);
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK_INT(in.posts_ok + in.posts_failed, 0);
  CHECK_INT(in.pending, 0);
  /* the new session can POST to the same module at once */
  mh_set(&mh, 200, 0);
  udp_send(U, ELRS_BEACON, sizeof ELRS_BEACON);
  POLL_UNTIL(w, NULL, 3000, count_in_log("Activation POST -> 127.0.0.1: 200 OK") > 0);
  CHECK_INT(count_in_log("Activation POST -> 127.0.0.1: 200 OK"), 1);
  wifi_close(w);
  mh_stop(&mh);
}

static void test_configure(void) {
  const uint16_t U = P_BASE + 5, U2 = P_BASE + 6, HP = P_BASE + 25;
  minihttp_t mh;
  mh_start(&mh, HP, 200);
  clear_log();
  vt = 0;
  wifi_config_t c;
  wifi_config_default(&c);
  c.port = U;
  c.http_port = HP;
  wifi_t *w = wifi_open(&app, &c);
  wifi_start(w, now());
  c.interval_us = 2000;
  c.channels = 8;
  wifi_configure(w, &c, now());
  udp_send(U, ELRS_BEACON, sizeof ELRS_BEACON);
  POLL_UNTIL(w, NULL, 3000, count_in_log("Activation POST") > 0);
  char req[1024];
  mh_last(&mh, req, sizeof req);
  CHECK(strstr(req, "\r\n\r\naction=joystick_begin&interval=2000&channels=8") != NULL);
  /* an explicit IP POSTs at once (its throttle entry is dropped) */
  snprintf(c.module_ip, sizeof c.module_ip, " 127.0.0.1 ");
  wifi_configure(w, &c, now());
  CHECK_INT(count_in_log("Target module set to 127.0.0.1 - connecting..."), 1);
  POLL_UNTIL(w, NULL, 3000, mh_requests(&mh) >= 2);
  CHECK_INT(mh_requests(&mh), 2);
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK(in.target == IP("127.0.0.1"));
  /* the same IP spelled the same: nothing happens */
  snprintf(c.module_ip, sizeof c.module_ip, "127.0.0.1");
  wifi_configure(w, &c, now());
  CHECK_INT(count_in_log("Target module set to"), 1);
  /* invalid text: logged, back to discovery */
  snprintf(c.module_ip, sizeof c.module_ip, "elrs_tx.local");
  wifi_configure(w, &c, now());
  CHECK_INT(count_in_log("Invalid module IP \"elrs_tx.local\" - use a.b.c.d"), 1);
  wifi_get_info(w, &in);
  CHECK(in.target == 0);
  /* port change: rebind, the old port is free */
  c.module_ip[0] = 0;
  c.port = U2;
  c.interval_us = 500;          /* clamped */
  c.channels = 99;
  wifi_configure(w, &c, now());
  wifi_get_info(w, &in);
  CHECK(in.listening);
  CHECK_INT(in.port, U2);
  CHECK_INT(count_in_log("Listening for joystick data on UDP 27467"), 1);
  plat_sock b = plat_udp_bind(U, false, false);
  CHECK(b != PLAT_BADSOCK);
  plat_sock_close(b);
  /* (R14: the port change released the module IP's module, activated less than 3 s ago and not locked
   * since: the modules send to port 11000 only, a later stop could not release it) */
  CHECK_INT(count_in_log("Releasing 127.0.0.1: joystick_end"), 1);
  udp_send(U2, ELRS_BEACON, sizeof ELRS_BEACON);
  POLL_UNTIL(w, NULL, 3000, mh_requests(&mh) >= 4);
  CHECK_STR(mh_body(&mh, 2), "action=joystick_end");
  mh_last(&mh, req, sizeof req);
  CHECK(strstr(req, "\r\n\r\naction=joystick_begin&interval=1000&channels=16") != NULL);
  /* a target set while stopped waits for the bind (the stop released the module the beacon just activated;
   * stopped, the polls still collect the answer) */
  wifi_stop(w);
  CHECK_INT(count_in_log("Releasing 127.0.0.1: joystick_end"), 2);
  POLL_UNTIL(w, NULL, 3000, count_in_log("Released 127.0.0.1") == 2);
  CHECK_INT(count_in_log("Released 127.0.0.1: it broadcasts again"), 2);
  snprintf(c.module_ip, sizeof c.module_ip, "127.0.0.1");
  wifi_configure(w, &c, now());
  wifi_get_info(w, &in);
  CHECK(in.target == IP("127.0.0.1"));
  CHECK_INT(in.pending, 0);
  CHECK_STR(status_of(w), "off");
  int before = mh_requests(&mh);
  wifi_start(w, now());
  POLL_UNTIL(w, NULL, 3000, mh_requests(&mh) > before);
  CHECK_INT(mh_requests(&mh), before + 1);
  wifi_close(w);
  mh_stop(&mh);
}

static void test_bad_port_and_nulls(void) {
  clear_log();
  vt = 0;
  wifi_t *w = open_with(0, 80, "");
  wifi_start(w, now());
  CHECK_INT(count_in_log("Invalid port \"0\" - using 1-65535."), 1);
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK(!in.running);
  CHECK_STR(status_of(w), "invalid UDP port 0");
  wifi_config_t c;
  wifi_config_default(&c);
  c.port = P_BASE + 7;
  wifi_configure(w, &c, now());
  wifi_get_info(w, &in);
  CHECK(in.running && in.listening);
  wifi_close(w);
  /* NULL everywhere */
  app_status_t st;
  memset(&st, 0, sizeof st);
  wifi_poll(NULL, 0, NULL);
  wifi_start(NULL, 0);
  wifi_stop(NULL);
  wifi_configure(NULL, &c, 0);
  wifi_fill_status(NULL, &st);
  CHECK_STR(st.wifi_state, "off");
  wifi_get_info(NULL, &in);
  CHECK(!in.running);
  CHECK(wifi_last_arrival_us(NULL) == 0);
  wifi_close(NULL);
  /* defaults; an invalid module IP in the config means discovery */
  w = wifi_open(&app, NULL);
  wifi_get_info(w, &in);
  CHECK_INT(in.port, 11000);
  CHECK(!in.running);
  wifi_close(w);
  clear_log();
  w = open_with(P_BASE + 8, 80, "10.0.0");
  CHECK_INT(count_in_log("Invalid module IP \"10.0.0\""), 1);
  wifi_get_info(w, &in);
  CHECK(in.target == 0);
  wifi_close(w);
}

/* ---------------------------------------------------------------- EngineLifecycleTests.cs
 * Under their C# names, over real sockets. The C# ActivateOverride is the real worker here,
 * POSTing to the tiny HTTP server. Ports 27441.. so they never meet the Python scenarios. */
static plat_sock udp_from(const char *ip) {      /* a "module" with its own loopback address */
  plat_sock s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = IP(ip);
  if (s == PLAT_BADSOCK || bind(s, (struct sockaddr *)&a, sizeof a) != 0) {
    fprintf(stderr, "cannot bind a UDP socket to %s\n", ip);
    exit(2);
  }
  return s;
}
static void send_from(plat_sock s, uint16_t port, int n, const int *v) {
  uint8_t b[64];
  b[0] = 1;
  b[1] = (uint8_t)n;
  for (int i = 0; i < n; i++) { b[2 + 2 * i] = (uint8_t)(v[i] & 0xFF); b[3 + 2 * i] = (uint8_t)(v[i] >> 8); }
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(0x7f000001);
  sendto(s, (const char *)b, 2 + 2 * n, 0, (struct sockaddr *)&a, sizeof a);
}
static void send_raw_from(plat_sock s, uint16_t port, const void *d, size_t n) {
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(0x7f000001);
  sendto(s, (const char *)d, (int)n, 0, (struct sockaddr *)&a, sizeof a);
}
static wifi_state_t state_of(wifi_t *w) { wifi_info_t in; wifi_get_info(w, &in); return in.state; }

static void FullFlow_BeaconThenChannels_OverRealUdp(void) {
  const uint16_t U = 27441, HP = 27451;
  minihttp_t mh;
  mh_start(&mh, HP, 200);
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, HP, "");
  wifi_start(w, now());
  udp_send(U, "VELOCIDRONE", 11);
  POLL_UNTIL(w, &src, 3000, mh_requests(&mh) > 0);
  CHECK_INT(mh_requests(&mh), 1);                     /* beacon should trigger activation */
  int v[4] = { 100, 200, 300, 400 };
  send_frame(U, 4, v);
  POLL_UNTIL(w, &src, 3000, state_of(w) == WIFI_ST_STREAMING);
  CHECK_INT(state_of(w), WIFI_ST_STREAMING);          /* channel data should reach Streaming */
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK_INT(in.nch, 4);
  CHECK(in.raw[0] == 100 && in.raw[1] == 200 && in.raw[2] == 300 && in.raw[3] == 400);
  CHECK(src.present && src.nch == 4);
  wifi_close(w);
  mh_stop(&mh);
}

static void Restart_RegainsStreaming_WhileModuleKeepsBroadcasting(void) {
  const uint16_t U = 27442;
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, 80, "");
  wifi_start(w, now());
  int a[4] = { 1, 2, 3, 4 }, b[4] = { 5, 6, 7, 8 };
  int64_t e = plat_now_us() + 3000000;
  while (state_of(w) != WIFI_ST_STREAMING && plat_now_us() < e) { send_frame(U, 4, a); poll_ms(w, &src, 5); }
  CHECK_INT(state_of(w), WIFI_ST_STREAMING);          /* first run should stream */
  wifi_stop(w);
  CHECK_INT(state_of(w), WIFI_ST_STOPPED);
  wifi_start(w, now());
  e = plat_now_us() + 4000000;
  while (state_of(w) != WIFI_ST_STREAMING && plat_now_us() < e) { send_frame(U, 4, b); poll_ms(w, &src, 5); }
  CHECK_INT(state_of(w), WIFI_ST_STREAMING);          /* after restart the engine must reach Streaming again */
  CHECK_INT(count_in_log("Joystick source locked to 127.0.0.1"), 2);
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK(in.raw[0] == 5 && in.raw[3] == 8);
  wifi_close(w);
}

static void PortAlreadyInUse_ReportsErrorState(void) {
  const uint16_t U = 27443;
  plat_sock blocker = plat_udp_bind(U, false, false);
  CHECK(blocker != PLAT_BADSOCK);
  clear_log();
  vt = 0;
  wifi_t *w = open_with(U, 80, "");
  wifi_start(w, now());
  POLL_UNTIL(w, NULL, 3000, state_of(w) == WIFI_ST_ERROR);   /* real time: 5 tries 300 ms apart */
  CHECK_INT(state_of(w), WIFI_ST_ERROR);
  CHECK_STR(status_of(w), "UDP port 27443 in use");
  CHECK_INT(count_in_log("error 0"), 0);
  wifi_close(w);
  plat_sock_close(blocker);
}

static void Stop_ReleasesThePort(void) {
  const uint16_t U = 27444;
  vt = 0;
  wifi_t *w = open_with(U, 80, "");
  wifi_start(w, now());
  poll_ms(w, NULL, 50);
  wifi_stop(w);
  plat_sock s = plat_udp_bind(U, false, false);      /* if the engine cleaned up, we can bind it */
  CHECK(s != PLAT_BADSOCK);
  plat_sock_close(s);
  wifi_close(w);
}

static void StartTwice_IsIdempotent(void) {
  const uint16_t U = 27445;
  clear_log();
  vt = 0;
  wifi_t *w = open_with(U, 80, "");
  wifi_start(w, now());
  wifi_start(w, now());                              /* a harmless no-op */
  poll_ms(w, NULL, 30);
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK(in.running && in.listening);
  CHECK_INT(count_in_log("Listening for joystick data on UDP 27445"), 1);
  wifi_stop(w);
  wifi_get_info(w, &in);
  CHECK(!in.running && !in.listening);
  wifi_close(w);
}

static void StoppedState_IsReported_OnShutdown(void) {
  const uint16_t U = 27446;
  clear_log();
  vt = 0;
  wifi_t *w = open_with(U, 80, "");
  wifi_start(w, now());
  poll_ms(w, NULL, 30);
  CHECK_INT(state_of(w), WIFI_ST_SEARCHING);
  wifi_stop(w);
  CHECK_INT(state_of(w), WIFI_ST_STOPPED);
  CHECK_STR(status_of(w), "off");
  CHECK_INT(count_in_log("Stopped.\n"), 1);
  CHECK_INT(count_in_log("Releasing"), 0);         /* nothing streamed to us: nothing to release */
  wifi_close(w);
}

/* ---------------------------------------------------------------- MainViewModelTests.cs
 * The status page (app_status_t) and the stick source stand in for the WPF view model.
 * Not ported: Axes_HaveEightRows_WithExpectedNames and HelpCommand_RaisesHelpRequested (WPF
 * widgets), and MiscTests' three FirewallRule tests (Windows Firewall). */
static void ChannelFrame_SetsStreamingBanner(void) {
  const uint16_t U = 27447;
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  plat_sock m = udp_from("127.0.0.2");
  wifi_t *w = open_with(U, 80, "");
  wifi_start(w, now());
  int v[4] = { 1, 2, 3, 4 };
  send_from(m, U, 4, v);
  POLL_UNTIL(w, &src, 2000, src.present);
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK_INT(in.state, WIFI_ST_STREAMING);
  CHECK_STR(in.detail, "127.0.0.2");
  CHECK_STR(status_of(w), "streaming");
  wifi_close(w);
  plat_sock_close(m);
}

static void TickBars_UpdatesAxisValuesAndPercent(void) {
  const uint16_t U = 27448;
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, 80, "");
  wifi_start(w, now());
  int v[4] = { 32767, 16384, 0, 8192 };
  send_frame(U, 4, v);
  POLL_UNTIL(w, &src, 2000, src.present);
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK_INT(in.raw[0], 32767);
  CHECK_INT(wifi_raw_percent(in.raw[0]), 100);
  CHECK_INT(wifi_raw_percent(in.raw[1]), 50);
  CHECK_INT(wifi_raw_percent(in.raw[2]), 0);
  CHECK_INT(wifi_raw_percent(in.raw[3]), 25);
  CHECK_INT(src.ch[0], 1024);
  CHECK_INT(src.ch[2], -1024);
  wifi_close(w);
}

static void NonStreamingState_ClearsAxes(void) {
  const uint16_t U = 27449;
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, 80, "");
  wifi_start(w, now());
  /* 16 channels, what is asked for: 4 from a module of unknown kind would restart it (a POST in flight
   * makes the status "activating", not "listening") */
  send_same(U, 16, 30000);
  POLL_UNTIL(w, &src, 2000, src.present);
  CHECK(src.ch[0] > 0);
  vt += 10000000;                                    /* the stream went quiet: the tick releases the lock */
  poll1(w, &src);
  CHECK(!src.present);
  CHECK_INT(src.ch[0], 0);
  app_status_t st;
  wifi_fill_status(w, &st);
  CHECK_STR(st.wifi_state, "listening");
  CHECK_STR(st.wifi_module, "");
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK(!in.bound && in.name[0] == 0);
  wifi_close(w);
}

static void EngineLog_LandsInLogText(void) {
  const uint16_t U = 27450;
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  plat_sock m = udp_from("127.0.0.5");
  wifi_t *w = open_with(U, 80, "");
  wifi_start(w, now());
  int v[4] = { 1, 2, 3, 4 };
  send_from(m, U, 4, v);
  POLL_UNTIL(w, &src, 2000, src.present);
  CHECK_INT(count_in_log("wifi: Joystick source locked to 127.0.0.5"), 1);
  wifi_close(w);
  plat_sock_close(m);
}

static void LogIsCapped(void) {
  const uint16_t U = 27452;
  clear_log();
  vt = 0;
  wifi_t *w = open_with(U, 80, "");
  wifi_start(w, now());
  for (int i = 0; i < 1000; i++) {                   /* a beacon flood: logged only when a POST goes out */
    udp_send(U, "VELOCIDRONE", 11);
    if (i % 50 == 49) poll_ms(w, NULL, 2);
  }
  poll_ms(w, NULL, 50);
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK(in.beacons > 900);                           /* loopback may drop a few under the burst */
  CHECK_INT(count_in_log("Discovery beacon from Crossfire module 127.0.0.1 - activating\n"), 1);
  wifi_config_t c;
  wifi_config_default(&c);
  c.port = U;
  for (int i = 0; i < 1000; i++) {                   /* each logs a line */
    snprintf(c.module_ip, sizeof c.module_ip, "bad-%d", i);
    wifi_configure(w, &c, now());
  }
  plat_mutex_lock(&app.mu);
  size_t n = app.log_len;
  plat_mutex_unlock(&app.mu);
  CHECK(n > 0 && n <= sizeof app.log);               /* app_log keeps a bounded ring of whole lines */
  int lines = count_in_log("Invalid module IP \"bad-");
  CHECK(lines > 50 && lines < 1000);                 /* the oldest were dropped */
  wifi_close(w);
}

static void StartStop_WithInvalidPort_LogsAndDoesNotStart(void) {
  clear_log();
  vt = 0;
  wifi_t *w = open_with(0, 80, "");
  wifi_start(w, now());
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK(!in.running && !in.listening);
  CHECK(count_in_log("Invalid port") == 1);
  wifi_close(w);
}

static void TrayText_ReflectsStreamingSource(void) {
  const uint16_t U = 27453;
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  plat_sock m = udp_from("127.0.0.2");
  wifi_t *w = open_with(U, 80, "");
  wifi_start(w, now());
  int v[4] = { 1, 2, 3, 4 };
  send_from(m, U, 4, v);
  POLL_UNTIL(w, &src, 2000, src.present);
  app_status_t st;
  wifi_fill_status(w, &st);
  CHECK(strstr(st.wifi_module, "127.0.0.2") != NULL);
  CHECK_STR(st.wifi_module, "WiFi 127.0.0.2");       /* no beacon seen: kind unknown */
  CHECK(strstr(src.name, "127.0.0.2") != NULL);
  wifi_close(w);
  plat_sock_close(m);
}

/* ---------------------------------------------------------------- the request (interval, channels)
 * ELRS takes them only while it isn't streaming, so a change reaches a streaming module as
 * joystick_end + joystick_begin; Crossfire ignores them and never gets a joystick_end. */
static void test_request_restart(void) {
  const uint16_t U = P_BASE + 9, HP = P_BASE + 26;
  minihttp_t mh;
  mh_start(&mh, HP, 200);
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, HP, "");
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK_INT(in.interval_us, 4000);
  CHECK_INT(in.channels, 16);
  wifi_start(w, now());
  udp_send(U, ELRS_BEACON, sizeof ELRS_BEACON);
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.1: 200 OK") == 1);
  send_same(U, 16, 16393);
  POLL_UNTIL(w, &src, 1000, src.present);
  CHECK(src.present);
  CHECK_INT(mh_requests(&mh), 1);

  /* a new rate while it streams: restarted at once, the lock kept */
  wifi_config_t c;
  wifi_config_default(&c);
  c.port = U;
  c.http_port = HP;
  c.interval_us = 2000;
  wifi_configure(w, &c, now());
  CHECK_INT(count_in_log("Restarting the stream of 127.0.0.1: 500 Hz, 16 channels"), 1);
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.1: 200 OK") == 2);
  CHECK_INT(count_in_log("Stream end POST -> 127.0.0.1: 200"), 1);
  CHECK_INT(mh_requests(&mh), 3);
  CHECK_STR(mh_body(&mh, 1), "action=joystick_end");
  CHECK_STR(mh_body(&mh, 2), "action=joystick_begin&interval=2000&channels=16");
  wifi_get_info(w, &in);
  CHECK_INT(in.interval_us, 2000);
  CHECK_INT(in.channels, 16);
  CHECK_INT(in.restarts, 1);
  CHECK_INT(in.posts, 2);
  CHECK_INT(in.posts_ok, 2);
  CHECK_INT(in.posts_failed, 0);
  CHECK_INT(in.pending, 0);
  CHECK(in.bound && src.present);
  CHECK_STR(status_of(w), "streaming");

  /* the same request again (and out-of-range values that clamp to it): nothing */
  wifi_configure(w, &c, now());
  c.channels = 99;
  wifi_configure(w, &c, now());
  poll_ms(w, &src, 50);
  CHECK_INT(mh_requests(&mh), 3);
  CHECK_INT(count_in_log("Restarting the stream"), 1);

  /* fewer channels: restarted again; frames of the old count that still come meanwhile don't restart it a
   * second time */
  c.channels = 8;
  wifi_configure(w, &c, now());
  CHECK_INT(count_in_log("Restarting the stream of 127.0.0.1: 500 Hz, 8 channels"), 1);
  send_same(U, 16, 16393);
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.1: 200 OK") == 3);
  poll_ms(w, &src, 50);
  CHECK_INT(mh_requests(&mh), 5);
  CHECK_STR(mh_body(&mh, 3), "action=joystick_end");
  CHECK_STR(mh_body(&mh, 4), "action=joystick_begin&interval=2000&channels=8");
  CHECK_INT(count_in_log("streams 16 channels"), 0);
  uint32_t upd = src.updates;
  send_same(U, 8, 16393);
  POLL_UNTIL(w, &src, 1000, src.updates != upd);
  CHECK_INT(src.nch, 8);
  wifi_get_info(w, &in);
  CHECK_INT(in.restarts, 2);
  CHECK_INT(in.posts, 3);

  /* the module's web server is gone: both POSTs of a restart fail, both are logged */
  mh_stop(&mh);
  c.interval_us = 5000;
  wifi_configure(w, &c, now());
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation failed for 127.0.0.1") == 1);
  CHECK_INT(count_in_log("Stream end POST failed for 127.0.0.1: connection refused"), 1);
  CHECK_INT(count_in_log("Activation failed for 127.0.0.1: connection refused"), 1);
  wifi_get_info(w, &in);
  CHECK_INT(in.posts_failed, 1);
  CHECK_INT(in.restarts, 3);

  /* not streaming: a change only waits for the next joystick_begin */
  mh_start(&mh, HP, 200);
  vt += 3100000;
  poll1(w, &src);
  CHECK(!src.present);
  CHECK_STR(status_of(w), "listening");
  c.interval_us = 10000;
  c.channels = 12;
  wifi_configure(w, &c, now());
  CHECK_INT(count_in_log("Restarting the stream"), 3);
  wifi_get_info(w, &in);
  CHECK_INT(in.interval_us, 10000);
  CHECK_INT(in.channels, 12);
  CHECK_INT(in.restarts, 3);
  vt += 5100000;                                   /* past the throttle of the last POST */
  udp_send(U, ELRS_BEACON, sizeof ELRS_BEACON);
  POLL_UNTIL(w, &src, 3000, mh_requests(&mh) == 1);
  CHECK_STR(mh_body(&mh, 0), "action=joystick_begin&interval=10000&channels=12");
  wifi_close(w);
  mh_stop(&mh);
}

/* A restart whose joystick_end is refused but whose joystick_begin is taken did not apply the request (a
 * streaming module only moved its stream): it is a failed activation, logged as one. */
static void test_request_restart_end_refused(void) {
  const uint16_t U = P_BASE + 12, HP = P_BASE + 29;
  minihttp_t mh;
  mh_start(&mh, HP, 200);
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, HP, "");
  wifi_start(w, now());
  udp_send(U, ELRS_BEACON, sizeof ELRS_BEACON);
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.1: 200 OK") == 1);
  send_same(U, 16, 16393);
  POLL_UNTIL(w, &src, 1000, src.present);
  CHECK(src.present);
  mh_set_next(&mh, 503, 1);                        /* the joystick_end */
  wifi_config_t c;
  wifi_config_default(&c);
  c.port = U;
  c.http_port = HP;
  c.interval_us = 2000;
  wifi_configure(w, &c, now());
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.1: 200 OK") == 2);
  CHECK_INT(mh_requests(&mh), 3);
  CHECK_STR(mh_body(&mh, 1), "action=joystick_end");
  CHECK_STR(mh_body(&mh, 2), "action=joystick_begin&interval=2000&channels=16");
  CHECK_INT(count_in_log("Stream end POST failed for 127.0.0.1: HTTP 503"), 1);
  CHECK_INT(count_in_log("Stream end POST -> 127.0.0.1"), 0);
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK_INT(in.posts, 2);
  CHECK_INT(in.posts_ok, 1);
  CHECK_INT(in.posts_failed, 1);
  CHECK_INT(in.restarts, 1);
  CHECK(in.bound && src.present);
  wifi_close(w);
  mh_stop(&mh);
}

/* nch-channel frames of the module at U (127.0.0.1), one a (virtual) second, secs of them: a stream as
 * the virtual clock's 1 s steps see it (the lock holds: 3 s without data release it) */
static void stream_seconds(wifi_t *w, insrc_t *src, uint16_t U, int nch, int secs) {
  for (int i = 0; i < secs; i++) {
    vt += 1000000;
    uint32_t upd = src->updates;
    send_same(U, nch, 24570);                      /* off center: a centering shows */
    POLL_UNTIL(w, src, 1000, src->updates != upd);
  }
}

/* R6: a restart counts only when both POSTs got a 2xx. One that failed (the module's web server refused
 * both while it kept streaming to us) is sent again every 5 s while that module stays locked, at most 3
 * sends per request change; giving up is logged. */
static void test_request_restart_failed_retried(void) {
  const uint16_t U = P_BASE + 13, HP = P_BASE + 30;
  minihttp_t mh;
  mh_start(&mh, HP, 200);
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, HP, "");
  wifi_start(w, now());
  udp_send(U, ELRS_BEACON, sizeof ELRS_BEACON);
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.1: 200 OK") == 1);
  send_same(U, 16, 16393);
  POLL_UNTIL(w, &src, 1000, src.present);
  CHECK(src.present);
  mh_set_next(&mh, 503, 2);                        /* the restart's joystick_end and joystick_begin */
  wifi_config_t c;
  wifi_config_default(&c);
  c.port = U;
  c.http_port = HP;
  c.channels = 8;
  wifi_configure(w, &c, now());
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.1: 503") == 1);
  CHECK_INT(count_in_log("Stream end POST failed for 127.0.0.1: HTTP 503"), 1);
  CHECK_INT(count_in_log("The restart of 127.0.0.1 failed - sending it again in 5 s"), 1);
  CHECK_INT(mh_requests(&mh), 3);
  stream_seconds(w, &src, U, 16, 4);               /* its old 16 channels: no second send yet */
  CHECK_INT(mh_requests(&mh), 3);
  stream_seconds(w, &src, U, 16, 2);               /* 5 s after the first send: sent again, and it takes */
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.1: 200 OK") == 2);
  CHECK_INT(mh_requests(&mh), 5);
  CHECK_STR(mh_body(&mh, 3), "action=joystick_end");
  CHECK_STR(mh_body(&mh, 4), "action=joystick_begin&interval=4000&channels=8");
  CHECK_INT(count_in_log("Stream end POST -> 127.0.0.1: 200"), 1);
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK_INT(in.restarts, 2);
  CHECK_INT(in.posts_failed, 1);
  CHECK_INT(in.posts_ok, 2);
  CHECK(in.bound && src.present);                  /* the lock was kept throughout */
  stream_seconds(w, &src, U, 16, 10);              /* it took: nothing more (this fake sends 16 on) */
  CHECK_INT(mh_requests(&mh), 5);

  /* a new request whose restart keeps failing: 3 sends 5 s apart, then it gives up (logged), and no more */
  mh_set_next(&mh, 503, 6);
  c.channels = 12;
  wifi_configure(w, &c, now());
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.1: 503") == 2);
  stream_seconds(w, &src, U, 16, 6);
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.1: 503") == 3);
  stream_seconds(w, &src, U, 16, 6);
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.1: 503") == 4);
  CHECK_INT(count_in_log("The restart of 127.0.0.1 failed - sending it again in "), 3);
  CHECK_INT(count_in_log("The restart of 127.0.0.1 failed 3 times - giving up"), 1);
  CHECK_INT(mh_requests(&mh), 11);
  stream_seconds(w, &src, U, 16, 12);
  CHECK_INT(mh_requests(&mh), 11);
  wifi_get_info(w, &in);
  CHECK_INT(in.restarts, 5);
  CHECK_INT(in.posts_failed, 4);
  CHECK(in.bound && src.present);
  wifi_close(w);
  mh_stop(&mh);
}

/* R7 with R2: a module streaming 16 channels without a beacon yet (it may be Crossfire: an ELRS module that
 * streamed on through a restart of the game), and within its 20 s a new request saved together with that
 * module's IP as the module IP. The lock is kept (no centering, nothing POSTed for the target), and the
 * held restart still goes out once the 20 s passed without a Crossfire beacon. */
static void test_request_same_target_waits(void) {
  const uint16_t U = P_BASE + 14, HP = P_BASE + 31;
  minihttp_t mh;
  mh_start(&mh, HP, 200);
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, HP, "");
  wifi_start(w, now());
  send_same(U, 16, 16393);
  POLL_UNTIL(w, &src, 1000, src.present);
  CHECK(src.present);
  wifi_config_t c;
  wifi_config_default(&c);
  c.port = U;
  c.http_port = HP;
  c.channels = 8;
  snprintf(c.module_ip, sizeof c.module_ip, "127.0.0.1");
  wifi_configure(w, &c, now());
  CHECK_INT(count_in_log("127.0.0.1 may be Crossfire (no beacon from it yet)"), 1);
  CHECK_INT(count_in_log("Target module set to 127.0.0.1 - streaming from it already"), 1);
  CHECK_INT(count_in_log("connecting"), 0);
  poll_ms(w, &src, 50);
  CHECK(src.present);                              /* the sticks were not centered */
  CHECK_INT(state_of(w), WIFI_ST_STREAMING);
  CHECK_INT(mh_requests(&mh), 0);                  /* it streams to us already: nothing to POST */
  stream_seconds(w, &src, U, 16, 18);
  CHECK_INT(mh_requests(&mh), 0);
  stream_seconds(w, &src, U, 16, 3);
  POLL_UNTIL(w, &src, 3000, mh_requests(&mh) >= 2);
  CHECK_INT(mh_requests(&mh), 2);
  CHECK_STR(mh_body(&mh, 0), "action=joystick_end");
  CHECK_STR(mh_body(&mh, 1), "action=joystick_begin&interval=4000&channels=8");
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK_INT(in.restarts, 1);
  CHECK(in.bound && in.target == IP("127.0.0.1"));
  wifi_close(w);
  mh_stop(&mh);
}

/* R2: a held restart (a module of unknown kind streaming 16 channels) survives a pause of the stream:
 * a short one (the lock holds), and one that loses the lock (a WiFi dropout: the sticks are centered and
 * nothing is sent while it is away). It goes out once that module is locked again and its 20 s passed
 * without a Crossfire beacon. */
static void test_request_held_through_pause(void) {
  const uint16_t U = P_BASE + 16, HP = P_BASE + 33;
  minihttp_t mh;
  mh_start(&mh, HP, 200);
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, HP, "");
  wifi_start(w, now());
  send_same(U, 16, 16393);
  POLL_UNTIL(w, &src, 1000, src.present);
  CHECK(src.present);
  wifi_config_t c;
  wifi_config_default(&c);
  c.port = U;
  c.http_port = HP;
  c.channels = 8;
  wifi_configure(w, &c, now());
  CHECK_INT(count_in_log("127.0.0.1 may be Crossfire (no beacon from it yet)"), 1);
  stream_seconds(w, &src, U, 16, 5);
  /* a short pause (2.5 s): held values, the lock kept */
  vt += 1500000;
  poll1(w, &src);
  CHECK(src.present && src.ch[0] > 400);           /* the last values held */
  stream_seconds(w, &src, U, 16, 1);
  CHECK_INT(count_in_log("went quiet"), 0);
  /* a pause that loses the lock */
  vt += 3100000;
  poll1(w, &src);
  CHECK(!src.present);
  CHECK_INT(src.ch[0], 0);                         /* centered */
  CHECK_INT(count_in_log("Source 127.0.0.1 went quiet - releasing lock"), 1);
  poll_ms(w, &src, 50);
  CHECK_INT(mh_requests(&mh), 0);
  /* back: locked again, the restart still held (its 20 s count from this lock) */
  stream_seconds(w, &src, U, 16, 1);
  CHECK(src.present);
  CHECK_INT(count_in_log("Joystick source locked to 127.0.0.1"), 2);
  CHECK_INT(count_in_log("may be Crossfire"), 1);
  stream_seconds(w, &src, U, 16, 18);
  CHECK_INT(mh_requests(&mh), 0);
  stream_seconds(w, &src, U, 16, 3);
  POLL_UNTIL(w, &src, 3000, mh_requests(&mh) >= 2);
  CHECK_INT(count_in_log("Restarting the stream of 127.0.0.1: 250 Hz, 8 channels"), 1);
  CHECK_INT(mh_requests(&mh), 2);
  CHECK_STR(mh_body(&mh, 0), "action=joystick_end");
  CHECK_STR(mh_body(&mh, 1), "action=joystick_begin&interval=4000&channels=8");
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK_INT(in.restarts, 1);
  CHECK(in.bound);
  wifi_close(w);
  mh_stop(&mh);
}

/* R2: a held restart is dropped when its module, gone quiet, sends its ELRS beacon (an ELRS module beacons
 * only while it is not streaming): the joystick_begin that beacon gets carries the request already. */
static void test_request_held_dropped_by_beacon(void) {
  const uint16_t U = P_BASE + 17, HP = P_BASE + 34;
  minihttp_t mh;
  mh_start(&mh, HP, 200);
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, HP, "");
  wifi_start(w, now());
  send_same(U, 16, 16393);
  POLL_UNTIL(w, &src, 1000, src.present);
  wifi_config_t c;
  wifi_config_default(&c);
  c.port = U;
  c.http_port = HP;
  c.channels = 8;
  wifi_configure(w, &c, now());
  CHECK_INT(count_in_log("127.0.0.1 may be Crossfire (no beacon from it yet)"), 1);
  stream_seconds(w, &src, U, 16, 2);
  vt += 3100000;
  poll1(w, &src);
  CHECK(!src.present);
  udp_send(U, ELRS_BEACON, sizeof ELRS_BEACON);
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.1: 200 OK") == 1);
  CHECK_INT(mh_requests(&mh), 1);
  CHECK_STR(mh_body(&mh, 0), "action=joystick_begin&interval=4000&channels=8");
  stream_seconds(w, &src, U, 8, 25);               /* it streams the request: nothing to restart */
  CHECK(src.present);
  CHECK_INT(src.nch, 8);
  CHECK_INT(mh_requests(&mh), 1);
  CHECK_INT(count_in_log("Restarting"), 0);
  CHECK_INT(count_in_log("- restarting its stream"), 0);
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK_INT(in.restarts, 0);
  CHECK_INT(in.kind, WIFI_KIND_ELRS);
  wifi_close(w);
  mh_stop(&mh);
}

/* R7: a restart the worker was too busy for (a slow POST to the module still in flight), then the module
 * IP moves to another module: the module being left is not restarted, the new one gets joystick_begin at
 * once with the request. */
static void test_request_busy_then_target_moves(void) {
  const uint16_t U = P_BASE + 15, HP = P_BASE + 32;
  minihttp_t ma, mb;
  mh_start_at(&ma, "127.0.0.2", HP, 200);
  mh_start_at(&mb, "127.0.0.3", HP, 200);
  mh_set(&ma, 200, 800);
  plat_sock a = udp_from("127.0.0.2");
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, HP, "");
  wifi_start(w, now());
  send_raw_from(a, U, ELRS_BEACON, sizeof ELRS_BEACON);
  POLL_UNTIL(w, &src, 3000, mh_requests(&ma) == 1);       /* in flight: its answer takes 800 ms */
  int v[16] = { 16393, 16393, 16393, 16393, 16393, 16393, 16393, 16393, 16393, 16393, 16393, 16393, 16393, 16393, 16393, 16393 };
  send_from(a, U, 16, v);
  POLL_UNTIL(w, &src, 1000, src.present);
  CHECK(src.present);
  wifi_config_t c;
  wifi_config_default(&c);
  c.port = U;
  c.http_port = HP;
  c.interval_us = 2000;
  wifi_configure(w, &c, now());
  CHECK_INT(count_in_log("Restarting the stream of 127.0.0.2: 500 Hz, 16 channels"), 1);
  snprintf(c.module_ip, sizeof c.module_ip, "127.0.0.3");
  wifi_configure(w, &c, now());
  CHECK_INT(count_in_log("Target module set to 127.0.0.3 - connecting..."), 1);
  poll1(w, &src);
  CHECK(!src.present);                             /* switching to another module centers the sticks */
  CHECK_STR(status_of(w), "activating 127.0.0.3");
  POLL_UNTIL(w, &src, 3000, mh_requests(&mb) >= 1);
  CHECK_STR(mh_body(&mb, 0), "action=joystick_begin&interval=2000&channels=16");
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.2: 200 OK") == 1);
  poll_ms(w, &src, 300);
  CHECK_INT(mh_requests(&ma), 1);                  /* no joystick_end for the module left behind */
  CHECK_INT(count_in_log("Stream end POST"), 0);
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK(in.target == IP("127.0.0.3"));
  CHECK_INT(in.restarts, 0);
  wifi_close(w);
  plat_sock_close(a);
  mh_stop(&ma);
  mh_stop(&mb);
}

static void test_request_crossfire(void) {
  const uint16_t U = P_BASE + 10, HP = P_BASE + 27;
  minihttp_t mh;
  mh_start(&mh, HP, 200);
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, HP, "");
  wifi_start(w, now());
  udp_send(U, "VELOCIDRONE", 11);
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.1: 200 OK") == 1);
  send_same(U, 16, 16393);
  POLL_UNTIL(w, &src, 1000, src.present);
  CHECK_STR(src.name, "Crossfire 127.0.0.1");
  wifi_config_t c;
  wifi_config_default(&c);
  c.port = U;
  c.http_port = HP;
  c.interval_us = 2000;
  wifi_configure(w, &c, now());
  CHECK_INT(count_in_log("Crossfire module 127.0.0.1 ignores the rate and channel count: it always sends 16 channels at about 90 Hz"), 1);
  c.channels = 8;                                  /* logged once per session */
  wifi_configure(w, &c, now());
  CHECK_INT(count_in_log("ignores the rate and channel count"), 1);
  /* its 16 channels are no mismatch to restart for */
  uint32_t upd = src.updates;
  send_same(U, 16, 16393);
  POLL_UNTIL(w, &src, 1000, src.updates != upd);
  poll_ms(w, &src, 100);
  CHECK_INT(mh_requests(&mh), 1);                  /* never a joystick_end */
  CHECK_INT(count_in_log("Restarting"), 0);
  CHECK_INT(count_in_log("restarting its stream"), 0);
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK_INT(in.restarts, 0);
  CHECK_INT(in.interval_us, 2000);
  CHECK_INT(in.channels, 8);
  CHECK(in.bound);
  wifi_close(w);
  mh_stop(&mh);
}

/* The module IP and the request in one change (R8): the new target's joystick_begin carries the request;
 * the module left behind is not restarted for it. Locked to it again, it is restarted because it was asked
 * for another request (R3). The module locked now set as the target keeps its lock (R7) and is restarted
 * for the new request. */
static void test_request_with_target(void) {
  const uint16_t U = P_BASE + 11, HP = P_BASE + 28;
  minihttp_t ma, mb;
  mh_start_at(&ma, "127.0.0.2", HP, 200);
  mh_start_at(&mb, "127.0.0.3", HP, 200);
  plat_sock a = udp_from("127.0.0.2");
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, HP, "");
  wifi_start(w, now());
  send_raw_from(a, U, ELRS_BEACON, sizeof ELRS_BEACON);
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.2: 200 OK") == 1);
  int v[16] = { 16393, 16393, 16393, 16393, 16393, 16393, 16393, 16393, 16393, 16393, 16393, 16393, 16393, 16393, 16393, 16393 };
  send_from(a, U, 16, v);
  POLL_UNTIL(w, &src, 1000, src.present);
  CHECK_STR(src.name, "ELRS 127.0.0.2");

  /* another module, and a new rate */
  wifi_config_t c;
  wifi_config_default(&c);
  c.port = U;
  c.http_port = HP;
  c.interval_us = 2000;
  snprintf(c.module_ip, sizeof c.module_ip, "127.0.0.3");
  wifi_configure(w, &c, now());
  POLL_UNTIL(w, &src, 3000, mh_requests(&mb) >= 1);
  CHECK_STR(mh_body(&mb, 0), "action=joystick_begin&interval=2000&channels=16");
  poll_ms(w, &src, 50);
  CHECK_INT(mh_requests(&ma), 1);                  /* no joystick_end for the module left behind */
  CHECK_INT(count_in_log("Restarting"), 0);
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK(in.target == IP("127.0.0.3"));
  CHECK_INT(in.restarts, 0);
  CHECK_INT(in.interval_us, 2000);

  /* back to discovery, locked to .2 again: it streams what its activation asked for (250 Hz), not the
   * rate changed since (500 Hz), so it is restarted when it locks (R3; ELRS by its beacon: at once) */
  c.module_ip[0] = 0;
  wifi_configure(w, &c, now());
  send_from(a, U, 16, v);
  POLL_UNTIL(w, &src, 1000, wifi_last_arrival_us(w) && state_of(w) == WIFI_ST_STREAMING);
  wifi_get_info(w, &in);
  CHECK(in.bound && in.source == IP("127.0.0.2"));
  CHECK_INT(count_in_log("127.0.0.2 still streams 250 Hz, 16 channels from before the change - restarting its "
                         "stream with 500 Hz, 16 channels"), 1);
  POLL_UNTIL(w, &src, 3000, mh_requests(&ma) >= 3);
  CHECK_STR(mh_body(&ma, 1), "action=joystick_end");
  CHECK_STR(mh_body(&ma, 2), "action=joystick_begin&interval=2000&channels=16");
  /* then .2 as the target with a new rate (R7): the lock is kept (it streams to us already: no centering,
   * no joystick_begin for the target), and the request restarts its stream */
  snprintf(c.module_ip, sizeof c.module_ip, "127.0.0.2");
  c.interval_us = 5000;
  wifi_configure(w, &c, now());
  CHECK_INT(count_in_log("Restarting the stream of 127.0.0.2: 200 Hz, 16 channels"), 1);
  CHECK_INT(count_in_log("Target module set to 127.0.0.2 - streaming from it already"), 1);
  CHECK_INT(count_in_log("Target module set to 127.0.0.2 - connecting..."), 0);
  poll1(w, &src);
  CHECK_INT(state_of(w), WIFI_ST_STREAMING);
  CHECK(src.present);                              /* the sticks were not centered */
  POLL_UNTIL(w, &src, 3000, mh_requests(&ma) >= 5);
  CHECK_STR(mh_body(&ma, 3), "action=joystick_end");
  CHECK_STR(mh_body(&ma, 4), "action=joystick_begin&interval=5000&channels=16");
  send_from(a, U, 16, v);                          /* it streams on */
  POLL_UNTIL(w, &src, 1000, state_of(w) == WIFI_ST_STREAMING);
  CHECK_INT(state_of(w), WIFI_ST_STREAMING);
  poll_ms(w, &src, 50);
  /* any later POST asks for the same */
  for (int i = 5; i < mh_requests(&ma) && i < MH_BODIES; i++)
    CHECK_STR(mh_body(&ma, i), "action=joystick_begin&interval=5000&channels=16");
  wifi_get_info(w, &in);
  CHECK_INT(in.restarts, 2);
  CHECK_INT(mh_requests(&mb), 1);
  wifi_close(w);
  plat_sock_close(a);
  mh_stop(&ma);
  mh_stop(&mb);
}

/* R9 with R3 (the core): a stop cuts a restart short (its answer still awaited: dropped with the session).
 * The module may still stream its old request (here it streams on before its release reached it: the release
 * waits behind the module's slow answer), so when it locks again after the start it is restarted once more. */
static void test_request_cut_by_stop(void) {
  const uint16_t U = P_BASE + 19, HP = P_BASE + 36;
  minihttp_t mh;
  mh_start(&mh, HP, 200);
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, HP, "");
  wifi_start(w, now());
  udp_send(U, ELRS_BEACON, sizeof ELRS_BEACON);
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.1: 200 OK") == 1);
  send_same(U, 16, 16393);
  POLL_UNTIL(w, &src, 1000, src.present);
  CHECK(src.present);
  mh_set(&mh, 200, 1500);                          /* the restart's answers take 1.5 s */
  wifi_config_t c;
  wifi_config_default(&c);
  c.port = U;
  c.http_port = HP;
  c.interval_us = 2000;
  wifi_configure(w, &c, now());
  CHECK_INT(count_in_log("Restarting the stream of 127.0.0.1: 500 Hz, 16 channels"), 1);
  POLL_UNTIL(w, &src, 2000, mh_requests(&mh) == 2);  /* its joystick_end is in flight */
  CHECK_STR(mh_body(&mh, 1), "action=joystick_end");
  wifi_stop(w);
  wifi_start(w, now());
  mh_set(&mh, 200, 0);
  poll_ms(w, &src, 200);
  CHECK_INT(count_in_log("Stream end POST"), 0);     /* its result went with the session */
  CHECK_INT(count_in_log("Activation POST -> 127.0.0.1"), 1);
  send_same(U, 16, 16393);                         /* the module streamed on */
  POLL_UNTIL(w, &src, 1000, src.present);
  CHECK(src.present);
  CHECK_INT(count_in_log("127.0.0.1 may still stream its old request (Stop cut its restart short) - restarting its "
                         "stream with 500 Hz, 16 channels"), 1);
  POLL_UNTIL(w, &src, 4000, mh_requests(&mh) >= 5);
  CHECK_STR(mh_body(&mh, 2), "action=joystick_end");   /* the stop's release (R14) */
  CHECK_STR(mh_body(&mh, 3), "action=joystick_end");   /* the new restart */
  CHECK_STR(mh_body(&mh, 4), "action=joystick_begin&interval=2000&channels=16");
  POLL_UNTIL(w, &src, 3000, count_in_log("Stream end POST -> 127.0.0.1: 200") == 1);
  CHECK_INT(count_in_log("Stream end POST -> 127.0.0.1: 200"), 1);
  CHECK_INT(count_in_log("Released 127.0.0.1: it broadcasts again"), 1);
  stream_seconds(w, &src, U, 16, 6);               /* it took: nothing more */
  CHECK_INT(mh_requests(&mh), 5);
  CHECK_INT(count_in_log("may still stream its old request"), 1);
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK_INT(in.restarts, 2);
  wifi_close(w);
  mh_stop(&mh);
}

/* R6 with R3 (the core): a failed restart is sent again only while its module stays locked. When the lock
 * goes first (a 3 s pause), what the module streams is not known: nothing is sent while it is not locked, and
 * when it locks again it is restarted once more. A rate that is no preset is logged with its interval. */
static void test_request_failed_then_quiet(void) {
  const uint16_t U = P_BASE + 37, HP = P_BASE + 38;
  minihttp_t mh;
  mh_start(&mh, HP, 200);
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, HP, "");
  wifi_start(w, now());
  udp_send(U, ELRS_BEACON, sizeof ELRS_BEACON);
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.1: 200 OK") == 1);
  send_same(U, 16, 16393);
  POLL_UNTIL(w, &src, 1000, src.present);
  CHECK(src.present);
  mh_set_next(&mh, 503, 2);                        /* the restart's joystick_end and joystick_begin */
  wifi_config_t c;
  wifi_config_default(&c);
  c.port = U;
  c.http_port = HP;
  c.interval_us = 3500;
  wifi_configure(w, &c, now());
  CHECK_INT(count_in_log("Restarting the stream of 127.0.0.1: 286 Hz (3500 us), 16 channels"), 1);
  POLL_UNTIL(w, &src, 3000, count_in_log("The restart of 127.0.0.1 failed - sending it again in 5 s") == 1);
  CHECK_INT(count_in_log("The restart of 127.0.0.1 failed - sending it again in 5 s"), 1);
  CHECK_INT(mh_requests(&mh), 3);
  vt += 3100000;                                   /* the stream pauses: the lock goes before the retry */
  poll1(w, &src);
  CHECK(!src.present);
  CHECK_INT(count_in_log("Source 127.0.0.1 went quiet - releasing lock"), 1);
  vt += 3000000;                                   /* past the retry's time: not locked, nothing sent */
  poll_ms(w, &src, 50);
  CHECK_INT(mh_requests(&mh), 3);
  send_same(U, 16, 16393);                         /* back, with its old request */
  POLL_UNTIL(w, &src, 1000, src.present);
  CHECK(src.present);
  CHECK_INT(count_in_log("127.0.0.1 may still stream its old request (its restart failed) - restarting its stream "
                         "with 286 Hz (3500 us), 16 channels"), 1);
  POLL_UNTIL(w, &src, 3000, mh_requests(&mh) >= 5);
  CHECK_STR(mh_body(&mh, 3), "action=joystick_end");
  CHECK_STR(mh_body(&mh, 4), "action=joystick_begin&interval=3500&channels=16");
  POLL_UNTIL(w, &src, 3000, count_in_log("Stream end POST -> 127.0.0.1: 200") == 1);
  CHECK_INT(count_in_log("Stream end POST -> 127.0.0.1: 200"), 1);
  stream_seconds(w, &src, U, 16, 7);               /* it took: nothing more */
  CHECK_INT(mh_requests(&mh), 5);
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK_INT(in.restarts, 2);
  CHECK_INT(in.posts_failed, 1);
  wifi_close(w);
  mh_stop(&mh);
}

/* ---------------------------------------------------------------- R14: the release
 * An ELRS module streams until it gets joystick_end and beacons only while idle: one left streaming to the Pi
 * can't be found by any other app. The close (the program's exit) and the web page's Release send it
 * joystick_end alone; Connect, or the program's next start (nothing is saved), listens again.
 * Ports 27420.. */
#define P_REL 27420

/* the module at 127.0.0.1 activated by its beacon and locked (16 channels) */
static void lock_elrs(wifi_t *w, insrc_t *src, uint16_t U, minihttp_t *mh, int before) {
  udp_send(U, ELRS_BEACON, sizeof ELRS_BEACON);
  POLL_UNTIL(w, src, 3000, mh_requests(mh) > before);
  send_same(U, 16, 16393);
  POLL_UNTIL(w, src, 2000, src->present);
  CHECK(src->present);
}

/* the program's exit: the locked module gets joystick_end, and the close waits for its answer */
static void test_release_on_close(void) {
  const uint16_t U = P_REL, HP = P_REL + 10;
  minihttp_t mh;
  mh_start(&mh, HP, 200);
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, HP, "");
  wifi_start(w, now());
  lock_elrs(w, &src, U, &mh, 0);
  CHECK_INT(mh_requests(&mh), 1);
  int64_t t0 = plat_now_us();
  wifi_close(w);
  int64_t took = plat_now_us() - t0;
  CHECK_INT(mh_requests(&mh), 2);
  CHECK_STR(mh_body(&mh, 1), "action=joystick_end");
  CHECK_INT(count_in_log("Releasing 127.0.0.1: joystick_end, so it stops streaming and broadcasts again"), 1);
  CHECK_INT(count_in_log("Released 127.0.0.1: it broadcasts again, any app can connect to it"), 1);
  const char *l = app_logs(), *a = strstr(l, "Releasing 127.0.0.1"), *b = strstr(l, "Stopped."), *c = strstr(l, "Released 127.0.0.1");
  CHECK(a && b && c && a < b && b < c);            /* handed over on the stop, answered before the close returns */
  CHECK(took < WIFI_RELEASE_WAIT_US);
  fprintf(stderr, "  close with a release: %lld ms\n", (long long)(took / 1000));
  mh_stop(&mh);
}

/* A module that does not answer holds the exit at most WIFI_RELEASE_WAIT_US: then the worker aborts it, and a
 * release still queued behind it is never sent. The locked module goes first, then one that streams to us as a
 * 2nd source (R14's order); each one's result is logged. */
static void test_release_on_close_no_answer(void) {
  const uint16_t U = P_REL + 1, HP = P_REL + 11;
  minihttp_t ma, mb;
  mh_start_at(&ma, "127.0.0.2", HP, 200);
  mh_start_at(&mb, "127.0.0.3", HP, 200);
  plat_sock a = udp_from("127.0.0.2"), b = udp_from("127.0.0.3");
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, HP, "");
  wifi_start(w, now());
  send_raw_from(a, U, ELRS_BEACON, sizeof ELRS_BEACON);
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.2: 200 OK") == 1);
  int v[16];
  for (int i = 0; i < 16; i++) v[i] = 16393;
  send_from(a, U, 16, v);
  POLL_UNTIL(w, &src, 2000, src.present);
  send_from(b, U, 16, v);                          /* another module streams to us too: its frames are dropped */
  POLL_UNTIL(w, &src, 2000, count_in_log("Ignoring 2nd source 127.0.0.3") == 1);
  CHECK_INT(count_in_log("Ignoring 2nd source 127.0.0.3 (locked to 127.0.0.2)"), 1);
  mh_set(&ma, 200, 5000);                          /* .2 answers no more within the exit's wait */
  int64_t t0 = plat_now_us();
  wifi_close(w);
  int64_t took = plat_now_us() - t0;
  CHECK(took >= WIFI_RELEASE_WAIT_US - 50000 && took < WIFI_RELEASE_WAIT_US + 700000);
  fprintf(stderr, "  close with a release that gets no answer: %lld ms\n", (long long)(took / 1000));
  const char *l = app_logs(), *ra = strstr(l, "Releasing 127.0.0.2: joystick_end"), *rb = strstr(l, "Releasing 127.0.0.3: joystick_end");
  CHECK(ra && rb && ra < rb);
  CHECK_INT(count_in_log("Releasing 127.0.0.2 failed: no answer in time - it may stream on until its WiFi or power is "
                         "cycled"), 1);
  CHECK_INT(count_in_log("aborted"), 0);
  CHECK_INT(count_in_log("Releasing 127.0.0.3 failed: not sent, the wait ran out before its turn - it may stream on "
                         "until its WiFi or power is cycled"), 1);
  CHECK_INT(mh_requests(&ma), 2);
  CHECK_STR(mh_body(&ma, 1), "action=joystick_end");
  CHECK_INT(mh_requests(&mb), 0);
  plat_sock_close(a);
  plat_sock_close(b);
  mh_stop(&ma);
  mh_stop(&mb);
}

/* The web page's Release: the module is released and the receiver stops listening (the module's next beacon
 * would get it a joystick_begin at once); settings saved meanwhile apply on Connect, which listens again. */
static void test_release_and_connect(void) {
  const uint16_t U = P_REL + 2, U2 = P_REL + 3, HP = P_REL + 12;
  minihttp_t mh;
  mh_start(&mh, HP, 200);
  clear_log();
  vt = 0;
  poll_max_us = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, HP, "");
  wifi_start(w, now());
  lock_elrs(w, &src, U, &mh, 0);
  CHECK_STR(release_of(w), "");                    /* no release yet */
  wifi_release(w, now());
  CHECK_INT(count_in_log("Release: not listening until Connect (the web page) or StickTime Pi starts again, so "
                         "other apps can find the module"), 1);
  CHECK_INT(count_in_log("Releasing 127.0.0.1: joystick_end, so it stops streaming and broadcasts again"), 1);
  CHECK_STR(status_of(w), "released");
  CHECK_STR(release_of(w), "sending");             /* until a poll collects its answer */
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK(in.released && !in.running && !in.listening && !in.bound);
  CHECK_INT(in.state, WIFI_ST_STOPPED);
  CHECK_STR(in.detail, "released");
  CHECK_INT(in.posts, 1);                          /* the release is no activation */
  poll1(w, &src);
  CHECK(!src.present);                             /* the source is gone (the sticks center) */
  POLL_UNTIL(w, &src, 3000, count_in_log("Released 127.0.0.1") > 0);   /* the polls collect its answer */
  CHECK_INT(count_in_log("Released 127.0.0.1: it broadcasts again, any app can connect to it"), 1);
  CHECK_STR(release_of(w), "ok");
  CHECK_INT(mh_requests(&mh), 2);
  CHECK_STR(mh_body(&mh, 1), "action=joystick_end");
  wifi_get_info(w, &in);
  CHECK_INT(in.releases_ok, 1);
  CHECK_INT(in.releases_failed, 0);
  CHECK_INT(in.releasing, 0);
  CHECK(poll_max_us < 200000);                     /* nothing waited on the frame loop */
  app_status_t st;
  memset(&st, 0, sizeof st);
  wifi_fill_status(w, &st);
  CHECK_STR(st.wifi_state, "released");
  CHECK_STR(st.wifi_module, "");
  /* not listening: the port is free, the module's beacons get it nothing */
  plat_sock b = plat_udp_bind(U, false, false);
  CHECK(b != PLAT_BADSOCK);
  plat_sock_close(b);
  udp_send(U, ELRS_BEACON, sizeof ELRS_BEACON);
  vt += 6000000;
  poll_ms(w, &src, 50);
  CHECK_INT(mh_requests(&mh), 2);
  /* a second Release does nothing */
  wifi_release(w, now());
  CHECK_INT(count_in_log("Release: not listening"), 1);
  /* settings saved while released: kept, nothing sent; a port change does not listen either */
  wifi_config_t c;
  wifi_config_default(&c);
  c.port = U2;
  c.http_port = HP;
  c.interval_us = 2000;
  c.channels = 8;
  wifi_configure(w, &c, now());
  poll_ms(w, &src, 30);
  wifi_get_info(w, &in);
  CHECK(in.released && !in.running && !in.listening);
  CHECK_INT(in.port, U2);
  CHECK_INT(in.interval_us, 2000);
  CHECK_INT(in.channels, 8);
  CHECK_STR(status_of(w), "released");
  CHECK_INT(mh_requests(&mh), 2);
  /* Connect: listening again (on the new port), the next beacon gets the saved request */
  wifi_connect(w, now());
  CHECK_INT(count_in_log("Connect: listening again"), 1);
  CHECK_INT(count_in_log("Listening for joystick data on UDP 27423"), 1);
  CHECK_STR(status_of(w), "listening");
  wifi_get_info(w, &in);
  CHECK(!in.released && in.running && in.listening);
  wifi_connect(w, now());                          /* listening already: nothing */
  CHECK_INT(count_in_log("Connect: listening again"), 1);
  udp_send(U2, ELRS_BEACON, sizeof ELRS_BEACON);
  POLL_UNTIL(w, &src, 3000, mh_requests(&mh) >= 3);
  CHECK_STR(mh_body(&mh, 2), "action=joystick_begin&interval=2000&channels=8");
  send_same(U2, 8, 16393);
  POLL_UNTIL(w, &src, 2000, src.present);
  CHECK(src.present);
  CHECK_STR(status_of(w), "streaming");
  CHECK_STR(release_of(w), "ok");                  /* still the last release's */
  /* a release that fails: logged with why, and the status says so */
  mh_set(&mh, 503, 0);
  wifi_release(w, now());
  POLL_UNTIL(w, &src, 3000, count_in_log("Releasing 127.0.0.1 failed") > 0);
  CHECK_INT(count_in_log("Releasing 127.0.0.1 failed: HTTP 503 - it may stream on until its WiFi or power is cycled"), 1);
  CHECK_STR(release_of(w), "failed");
  wifi_get_info(w, &in);
  CHECK_INT(in.releases_ok, 1);
  CHECK_INT(in.releases_failed, 1);
  /* the program's next start (wifi_start) ends the release too */
  wifi_start(w, now());
  wifi_get_info(w, &in);
  CHECK(!in.released && in.listening);
  CHECK_STR(status_of(w), "listening");
  wifi_release(w, now());                          /* nothing streams to us: nothing to send */
  CHECK_STR(status_of(w), "released");
  CHECK_STR(release_of(w), "none");
  int64_t t0 = plat_now_us();
  wifi_close(w);                                   /* released: nothing more to release or wait for */
  CHECK(plat_now_us() - t0 < 500000);
  CHECK_INT(mh_requests(&mh), 4);
  CHECK_INT(count_in_log("Releasing 127.0.0.1"), 3);   /* 2 handed over, 1 failed */
  mh_stop(&mh);
}

/* R14 with R3 (the core): Release, a setting, Connect, with the module IP set. The release took, so the module is
 * idle: the joystick_begin Connect sends it carries the new rate, and the module that locks is not restarted for
 * the old one (known as ELRS: at once; of unknown kind: 20 s into the stream, a pause in flight). A release that
 * failed leaves it streaming its old request: Connect's lock restarts it then, as before. */
static void test_release_then_connect_new_request(void) {
  const uint16_t U = P_REL + 9, HP = P_REL + 19;
  minihttp_t mh;
  mh_start(&mh, HP, 200);
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_config_t c;
  wifi_config_default(&c);
  c.port = U;
  c.http_port = HP;
  snprintf(c.module_ip, sizeof c.module_ip, "127.0.0.1");
  wifi_t *w = open_with(U, HP, "127.0.0.1");
  wifi_start(w, now());
  lock_elrs(w, &src, U, &mh, 0);                   /* its beacon: known as ELRS */
  CHECK_INT(mh_requests(&mh), 1);
  CHECK_STR(mh_body(&mh, 0), "action=joystick_begin&interval=4000&channels=16");
  wifi_release(w, now());
  POLL_UNTIL(w, &src, 3000, count_in_log("Released 127.0.0.1") > 0);
  CHECK_STR(release_of(w), "ok");
  c.interval_us = 2000;                            /* saved while released: kept for Connect */
  wifi_configure(w, &c, now());
  poll_ms(w, &src, 30);
  CHECK_INT(mh_requests(&mh), 2);
  wifi_connect(w, now());
  POLL_UNTIL(w, &src, 3000, mh_requests(&mh) >= 3);
  CHECK_STR(mh_body(&mh, 2), "action=joystick_begin&interval=2000&channels=16");
  send_same(U, 16, 16393);                         /* the idle module streams what it was asked */
  POLL_UNTIL(w, &src, 2000, src.present);
  CHECK(src.present);
  stream_seconds(w, &src, U, 16, 6);
  poll_ms(w, &src, 100);
  CHECK_INT(mh_requests(&mh), 3);
  CHECK_INT(count_in_log("from before the change"), 0);
  CHECK_INT(count_in_log("Restarting the stream"), 0);
  /* the release fails (HTTP 503): the module streams on at 500 Hz, so Connect's lock restarts it for 250 Hz */
  mh_set(&mh, 503, 0);
  wifi_release(w, now());
  POLL_UNTIL(w, &src, 3000, count_in_log("Releasing 127.0.0.1 failed") > 0);
  CHECK_INT(count_in_log("Releasing 127.0.0.1 failed: HTTP 503 - it may stream on until its WiFi or power is "
                         "cycled"), 1);
  mh_set(&mh, 200, 0);
  c.interval_us = 4000;
  wifi_configure(w, &c, now());
  wifi_connect(w, now());
  POLL_UNTIL(w, &src, 3000, mh_requests(&mh) >= 5);
  CHECK_STR(mh_body(&mh, 4), "action=joystick_begin&interval=4000&channels=16");
  send_same(U, 16, 16393);                         /* it streams on: only moved to us */
  POLL_UNTIL(w, &src, 3000, mh_requests(&mh) >= 7);
  CHECK_INT(count_in_log("127.0.0.1 still streams 500 Hz, 16 channels from before the change - restarting its "
                         "stream with 250 Hz, 16 channels"), 1);
  CHECK_STR(mh_body(&mh, 5), "action=joystick_end");
  CHECK_STR(mh_body(&mh, 6), "action=joystick_begin&interval=4000&channels=16");
  wifi_close(w);
  mh_stop(&mh);

  /* unknown kind (its beacons never reach the Pi): no restart either, also once its 20 s are over */
  mh_start(&mh, HP, 200);
  clear_log();
  vt = 0;
  memset(&src, 0, sizeof src);
  c.interval_us = 4000;
  w = open_with(U, HP, "127.0.0.1");
  wifi_start(w, now());
  POLL_UNTIL(w, &src, 3000, mh_requests(&mh) >= 1);
  send_same(U, 16, 16393);
  POLL_UNTIL(w, &src, 2000, src.present);
  CHECK(src.present);
  wifi_release(w, now());
  POLL_UNTIL(w, &src, 3000, count_in_log("Released 127.0.0.1") > 0);
  c.interval_us = 2000;
  wifi_configure(w, &c, now());
  wifi_connect(w, now());
  POLL_UNTIL(w, &src, 3000, mh_requests(&mh) >= 3);
  CHECK_STR(mh_body(&mh, 2), "action=joystick_begin&interval=2000&channels=16");
  send_same(U, 16, 16393);
  POLL_UNTIL(w, &src, 2000, src.present);
  stream_seconds(w, &src, U, 16, 25);
  poll_ms(w, &src, 100);
  CHECK_INT(mh_requests(&mh), 3);
  CHECK_INT(count_in_log("may be Crossfire"), 0);
  CHECK_INT(count_in_log("Restarting the stream"), 0);
  wifi_close(w);
  mh_stop(&mh);
}

/* Crossfire (by its beacon) is never sent a joystick_end: it beacons while streaming, other apps find it as it
 * is. Logged, nothing sent, on Release and on the close. */
static void test_release_crossfire(void) {
  const uint16_t U = P_REL + 4, HP = P_REL + 14;
  minihttp_t mh;
  mh_start(&mh, HP, 200);
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, HP, "");
  wifi_start(w, now());
  udp_send(U, "VELOCIDRONE", 11);
  POLL_UNTIL(w, &src, 3000, mh_requests(&mh) >= 1);
  send_same(U, 16, 16393);
  POLL_UNTIL(w, &src, 2000, src.present);
  CHECK_STR(src.name, "Crossfire 127.0.0.1");
  wifi_release(w, now());
  CHECK_INT(count_in_log("Not releasing 127.0.0.1: Crossfire broadcasts while streaming, other apps find it as it is"), 1);
  CHECK_STR(status_of(w), "released");
  CHECK_STR(release_of(w), "none");                /* nothing sent: it broadcasts anyway */
  poll1(w, &src);
  CHECK(!src.present);
  wifi_connect(w, now());
  send_same(U, 16, 16393);                         /* it streams on: locked again at once */
  POLL_UNTIL(w, &src, 2000, src.present);
  CHECK(src.present);
  wifi_close(w);
  CHECK_INT(count_in_log("Not releasing 127.0.0.1"), 2);
  CHECK_INT(count_in_log("Releasing 127.0.0.1"), 0);
  CHECK_INT(mh_requests(&mh), 1);                  /* only the joystick_begin */
  wifi_release(NULL, 0);
  wifi_connect(NULL, 0);
  mh_stop(&mh);
}

/* Nothing of a Release is saved: the program's next start (wifi_open, then wifi_start, as host_init) listens
 * again. A module IP set gets its joystick_begin at once then, so a module released for another app is taken back
 * (the docs say so); its exit releases it again (activated less than 3 s ago, not locked since). */
static void test_release_next_start(void) {
  const uint16_t U = P_REL + 5, HP = P_REL + 15;
  minihttp_t mh;
  mh_start(&mh, HP, 200);
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, HP, "");
  wifi_start(w, now());
  lock_elrs(w, &src, U, &mh, 0);
  wifi_release(w, now());
  POLL_UNTIL(w, &src, 3000, count_in_log("Released 127.0.0.1") > 0);
  CHECK_STR(release_of(w), "ok");
  wifi_close(w);                                   /* the exit, released: nothing more to send */
  CHECK_INT(mh_requests(&mh), 2);
  CHECK_STR(mh_body(&mh, 1), "action=joystick_end");
  /* the next start, with a module IP set */
  memset(&src, 0, sizeof src);
  w = open_with(U, HP, "127.0.0.1");
  wifi_start(w, now());
  wifi_info_t in;
  wifi_get_info(w, &in);
  CHECK(!in.released && in.running && in.listening);
  CHECK_STR(release_of(w), "");                    /* this run released nothing yet */
  CHECK(strcmp(status_of(w), "released") != 0);
  POLL_UNTIL(w, &src, 3000, mh_requests(&mh) >= 3);
  CHECK_STR(mh_body(&mh, 2), "action=joystick_begin&interval=4000&channels=16");
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.1: 200 OK") == 2);
  wifi_close(w);
  CHECK_INT(count_in_log("Releasing 127.0.0.1: joystick_end"), 2);
  CHECK_INT(mh_requests(&mh), 4);
  CHECK_STR(mh_body(&mh, 3), "action=joystick_end");
  CHECK_INT(count_in_log("Released 127.0.0.1: it broadcasts again"), 2);
  mh_stop(&mh);
}

/* R7 with R14: the module left for another module IP gets nothing at the switch, but it streams on to the Pi (it
 * does not know it was left), so a stop while its frames still come releases it: first (it is known to stream to
 * us), then the new module IP (activated a moment ago, not locked since). */
static void test_release_module_left_by_switch(void) {
  const uint16_t U = P_REL + 6, HP = P_REL + 16;
  minihttp_t ma, mb;
  mh_start_at(&ma, "127.0.0.2", HP, 200);
  mh_start_at(&mb, "127.0.0.3", HP, 200);
  plat_sock a = udp_from("127.0.0.2");
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, HP, "");
  wifi_start(w, now());
  send_raw_from(a, U, ELRS_BEACON, sizeof ELRS_BEACON);
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.2: 200 OK") == 1);
  int v[16];
  for (int i = 0; i < 16; i++) v[i] = 16393;
  send_from(a, U, 16, v);
  POLL_UNTIL(w, &src, 2000, src.present);
  CHECK(src.present);
  wifi_config_t c;
  wifi_config_default(&c);
  c.port = U;
  c.http_port = HP;
  snprintf(c.module_ip, sizeof c.module_ip, "127.0.0.3");
  wifi_configure(w, &c, now());
  POLL_UNTIL(w, &src, 3000, mh_requests(&mb) >= 1);
  CHECK_STR(mh_body(&mb, 0), "action=joystick_begin&interval=4000&channels=16");
  CHECK_INT(count_in_log("Releasing"), 0);         /* nothing at the switch (R7) */
  send_from(a, U, 16, v);                          /* the module left streams on: its frames are dropped */
  poll_ms(w, &src, 30);
  CHECK(!src.present);
  CHECK_INT(mh_requests(&ma), 1);
  wifi_stop(w);
  const char *l = app_logs(), *ra = strstr(l, "Releasing 127.0.0.2: joystick_end"), *rb = strstr(l, "Releasing 127.0.0.3: joystick_end");
  CHECK(ra && rb && ra < rb);
  POLL_UNTIL(w, &src, 3000, count_in_log("Released 127.0.0.") == 2);
  CHECK_INT(count_in_log("Released 127.0.0.2: it broadcasts again, any app can connect to it"), 1);
  CHECK_INT(count_in_log("Released 127.0.0.3: it broadcasts again, any app can connect to it"), 1);
  CHECK_INT(mh_requests(&ma), 2);
  CHECK_STR(mh_body(&ma, 1), "action=joystick_end");
  CHECK_INT(mh_requests(&mb), 2);
  CHECK_STR(mh_body(&mb, 1), "action=joystick_end");
  CHECK_STR(release_of(w), "ok");
  wifi_close(w);                                   /* stopped: nothing more */
  CHECK_INT(mh_requests(&ma) + mh_requests(&mb), 4);
  plat_sock_close(a);
  mh_stop(&ma);
  mh_stop(&mb);
}

/* A release whose module does not answer gets 2 s (input_wifi.c's RELEASE_POST_MS), not the 5 s of an activation:
 * the worker sends one request at a time, so it holds what queues behind it, here Connect's joystick_begin to the
 * module IP, at most that long (as in the Linux app). */
static void test_release_post_timeout(void) {
  const uint16_t U = P_REL + 7, HP = P_REL + 17;
  minihttp_t ma, mb;
  mh_start_at(&ma, "127.0.0.2", HP, 200);
  mh_start_at(&mb, "127.0.0.3", HP, 200);
  plat_sock a = udp_from("127.0.0.2");
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, HP, "");
  wifi_start(w, now());
  send_raw_from(a, U, ELRS_BEACON, sizeof ELRS_BEACON);
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.2: 200 OK") == 1);
  int v[16];
  for (int i = 0; i < 16; i++) v[i] = 16393;
  send_from(a, U, 16, v);
  POLL_UNTIL(w, &src, 2000, src.present);
  CHECK(src.present);
  mh_set(&ma, 200, 4000);                          /* .2 answers no more within 2 s (it just lost power) */
  int64_t t0 = plat_now_us();
  wifi_release(w, now());
  /* a module IP saved while released, then Connect: its joystick_begin goes out at once, behind the release */
  wifi_config_t c;
  wifi_config_default(&c);
  c.port = U;
  c.http_port = HP;
  snprintf(c.module_ip, sizeof c.module_ip, "127.0.0.3");
  wifi_configure(w, &c, now());
  wifi_connect(w, now());
  POLL_UNTIL(w, &src, 6000, mh_requests(&mb) >= 1);
  int64_t took = plat_now_us() - t0;
  CHECK_STR(mh_body(&mb, 0), "action=joystick_begin&interval=4000&channels=16");
  CHECK(took >= 1900000 && took < 3000000);        /* 2 s, not 5 */
  fprintf(stderr, "  a release without an answer held Connect's joystick_begin %lld ms\n", (long long)(took / 1000));
  POLL_UNTIL(w, &src, 3000, count_in_log("Releasing 127.0.0.2 failed") > 0);
  CHECK_INT(count_in_log("Releasing 127.0.0.2 failed: timed out after 2 s - it may stream on until its WiFi or power is "
                         "cycled"), 1);
  CHECK_STR(release_of(w), "failed");
  CHECK_INT(mh_requests(&ma), 2);
  CHECK_STR(mh_body(&ma, 1), "action=joystick_end");
  POLL_UNTIL(w, &src, 3000, count_in_log("Activation POST -> 127.0.0.3: 200 OK") == 1);
  wifi_close(w);                                   /* .3: activated less than 3 s ago, not locked since */
  CHECK_INT(count_in_log("Released 127.0.0.3: it broadcasts again, any app can connect to it"), 1);
  CHECK_INT(mh_requests(&mb), 2);
  CHECK_STR(mh_body(&mb, 1), "action=joystick_end");
  CHECK_INT(mh_requests(&ma), 2);
  plat_sock_close(a);
  mh_stop(&ma);
  mh_stop(&mb);
}

/* The exit's wait counts from the stop that handed the releases over, here the web page's Release, as in the Linux
 * app: an exit 1 s after a Release whose module does not answer waits about 0.5 s more, not 1.5 s; what is left
 * then is cut short ("no answer in time"). */
static void test_release_wait_counts_from_the_release(void) {
  const uint16_t U = P_REL + 8, HP = P_REL + 18;
  minihttp_t mh;
  mh_start(&mh, HP, 200);
  clear_log();
  vt = 0;
  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = open_with(U, HP, "");
  wifi_start(w, now());
  lock_elrs(w, &src, U, &mh, 0);
  mh_set(&mh, 200, 4000);                          /* the release gets no answer within its 2 s */
  int64_t t0 = plat_now_us();
  wifi_release(w, now());
  poll_ms(w, &src, 1000);
  int64_t t1 = plat_now_us();
  wifi_close(w);
  int64_t t2 = plat_now_us(), from = t1 - t0 > WIFI_RELEASE_WAIT_US ? t1 - t0 : WIFI_RELEASE_WAIT_US;
  CHECK(t2 - t0 >= WIFI_RELEASE_WAIT_US - 50000 && t2 - t0 < from + 700000);   /* 1.5 s after the Release */
  CHECK(t2 - t1 < WIFI_RELEASE_WAIT_US - 300000);   /* not 1.5 s after the exit */
  fprintf(stderr, "  exit 1 s after a Release that gets no answer: %lld ms\n", (long long)((t2 - t1) / 1000));
  CHECK_INT(count_in_log("Releasing 127.0.0.1 failed: no answer in time - it may stream on until its WiFi or power is "
                         "cycled"), 1);
  CHECK_INT(count_in_log("Releasing 127.0.0.1: joystick_end"), 1);   /* released already: the exit sends nothing more */
  CHECK_INT(mh_requests(&mh), 2);
  mh_stop(&mh);
}

static int self_test(void) {
  plat_net_init();
  app_init(&app);
  udp_open();
  int64_t t0 = plat_now_us();
  FullFlow_BeaconThenChannels_OverRealUdp();
  Restart_RegainsStreaming_WhileModuleKeepsBroadcasting();
  PortAlreadyInUse_ReportsErrorState();
  Stop_ReleasesThePort();
  StartTwice_IsIdempotent();
  StoppedState_IsReported_OnShutdown();
  ChannelFrame_SetsStreamingBanner();
  TickBars_UpdatesAxisValuesAndPercent();
  NonStreamingState_ClearsAxes();
  EngineLog_LandsInLogText();
  LogIsCapped();
  StartStop_WithInvalidPort_LogsAndDoesNotStart();
  TrayText_ReflectsStreamingSource();
  test_flow();
  test_port_in_use();
  test_explicit_refused();
  test_404_then_hint();
  test_stale_result_discarded();
  test_configure();
  test_bad_port_and_nulls();
  test_request_restart();
  test_request_restart_end_refused();
  test_request_restart_failed_retried();
  test_request_same_target_waits();
  test_request_held_through_pause();
  test_request_held_dropped_by_beacon();
  test_request_busy_then_target_moves();
  test_request_crossfire();
  test_request_with_target();
  test_request_cut_by_stop();
  test_request_failed_then_quiet();
  test_release_on_close();
  test_release_on_close_no_answer();
  test_release_and_connect();
  test_release_then_connect_new_request();
  test_release_crossfire();
  test_release_next_start();
  test_release_module_left_by_switch();
  test_release_post_timeout();
  test_release_wait_counts_from_the_release();
  fprintf(stderr, "  self-test took %.1f s\n", (double)(plat_now_us() - t0) / 1e6);
  plat_sock_close(udp_tx);
  T_DONE();
}

/* ---------------------------------------------------------------- probe */
static int64_t t_start;
static void jstr(const char *s) {
  putchar('"');
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
    else if (c < 0x20 || c >= 0x7f) printf("\\u%04x", c);
    else putchar(c);
  }
  putchar('"');
}
static void ev_begin(const char *ev) { printf("{\"t\":%lld,\"ev\":\"%s\"", (long long)((plat_now_us() - t_start) / 1000), ev); }
static void ev_end(void) { printf("}\n"); fflush(stdout); }

/* stdin commands (a reader thread; the loop never blocks on them) */
static plat_mutex cmd_mu;
static char cmds[16][128];
static int ncmds;
static void *stdin_thread(void *arg) {
  char line[128];
  while (fgets(line, sizeof line, stdin)) {
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    plat_mutex_lock(&cmd_mu);
    if (ncmds < 16) snprintf(cmds[ncmds++], sizeof cmds[0], "%s", line);
    plat_mutex_unlock(&cmd_mu);
  }
  return NULL;
}

static void emit_logs(void) {
  static char buf[16384];
  plat_mutex_lock(&app.mu);
  size_t n = app.log_len;
  memcpy(buf, app.log, n);
  app.log_len = 0;
  plat_mutex_unlock(&app.mu);
  buf[n] = 0;
  for (char *p = buf, *nl; *p; p = nl + 1) {
    nl = strchr(p, '\n');
    if (!nl) break;
    *nl = 0;
    ev_begin("log");
    printf(",\"msg\":");
    jstr(p);
    ev_end();
  }
}

static void emit_raw_vals(const wifi_info_t *in, const insrc_t *src) {
  printf(",\"n\":%d,\"raw\":[", in->nch);
  for (int i = 0; i < in->nch; i++) printf("%s%u", i ? "," : "", in->raw[i]);
  printf("],\"val\":[");
  for (int i = 0; i < src->nch; i++) printf("%s%d", i ? "," : "", src->ch[i]);
  printf("]");
}

static int probe(int argc, char **argv) {
  wifi_config_t cfg;
  wifi_config_default(&cfg);
  cfg.port = 27401;
  cfg.http_port = 27480;
  int duration_ms = 10000, poll_us = 2000, expect_nch = 0, min_frames = 1;
  double min_rate = -1, max_rate = -1;
  bool expect_streaming = false, use_stdin = false;
  uint32_t expect_src = 0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define ARG(name) (!strcmp(a, name) && v && ++i)
    if (ARG("--port")) cfg.port = (uint16_t)atoi(v);
    else if (ARG("--http-port")) cfg.http_port = (uint16_t)atoi(v);
    else if (ARG("--module-ip")) snprintf(cfg.module_ip, sizeof cfg.module_ip, "%s", v);
    else if (ARG("--interval-us")) cfg.interval_us = atoi(v);
    else if (ARG("--channels")) cfg.channels = atoi(v);
    else if (ARG("--duration-ms")) duration_ms = atoi(v);
    else if (ARG("--poll-us")) poll_us = atoi(v);
    else if (ARG("--expect-src")) { expect_src = IP(v); expect_streaming = true; }
    else if (ARG("--expect-nch")) { expect_nch = atoi(v); expect_streaming = true; }
    else if (ARG("--min-frames")) { min_frames = atoi(v); expect_streaming = true; }
    else if (ARG("--min-rate")) { min_rate = atof(v); expect_streaming = true; }
    else if (ARG("--max-rate")) { max_rate = atof(v); expect_streaming = true; }
    else if (!strcmp(a, "--expect-streaming")) expect_streaming = true;
    else if (!strcmp(a, "--stdin")) use_stdin = true;
    else { fprintf(stderr, "unknown or incomplete argument %s (see the comment at the top of test_wifi_live.c)\n", a); return 2; }
#undef ARG
  }
  plat_net_init();
  app_init(&app);
  t_start = plat_now_us();
  plat_mutex_init(&cmd_mu);
  if (use_stdin) plat_thread_start(NULL, stdin_thread, NULL, true);

  insrc_t src;
  memset(&src, 0, sizeof src);
  wifi_t *w = wifi_open(&app, &cfg);
  wifi_start(w, plat_now_us());

  static int64_t hist[1001];          /* poll times in 10 us buckets, the last one is "10 ms or more" */
  int64_t npoll = 0, sum_us = 0, max_us = 0;
  char last_state[64] = "", last_module[64] = "", last_raw[256] = "";
  uint32_t last_updates = 0;
  int64_t last_frame_ev = 0, last_stats_ev = plat_now_us(), end = plat_now_us() + (int64_t)duration_ms * 1000;
  bool rate_ok_seen = false, quit = false;
  int code = expect_streaming ? 1 : 0;
  const char *reason = expect_streaming ? "duration ended before the expectations were met" : "duration ended";
  while (!quit && plat_now_us() < end) {
    int64_t t0 = plat_now_us();
    wifi_poll(w, t0, &src);
    int64_t d = plat_now_us() - t0;
    hist[d / 10 < 1000 ? d / 10 : 1000]++;
    npoll++;
    sum_us += d;
    if (d > max_us) max_us = d;
    emit_logs();

    app_status_t st;
    memset(&st, 0, sizeof st);
    wifi_fill_status(w, &st);
    wifi_info_t in;
    wifi_get_info(w, &in);
    if (strcmp(st.wifi_state, last_state) || strcmp(st.wifi_module, last_module)) {   /* a handover keeps "streaming" */
      snprintf(last_state, sizeof last_state, "%s", st.wifi_state);
      snprintf(last_module, sizeof last_module, "%s", st.wifi_module);
      ev_begin("state");
      printf(",\"state\":"); jstr(st.wifi_state);
      printf(",\"module\":"); jstr(st.wifi_module);
      printf(",\"proto\":\"%s\",\"detail\":", wifi_state_name(in.state)); jstr(in.detail);
      ev_end();
    }
    int64_t tn = plat_now_us();
    bool live = src.present && tn - src.last_us <= INPUT_STALE_US;
    if (src.updates != last_updates && src.present) {
      char raw[256];
      size_t k = 0;
      for (int i = 0; i < in.nch && k < sizeof raw - 8; i++) k += (size_t)snprintf(raw + k, sizeof raw - k, "%u,", in.raw[i]);
      raw[k] = 0;
      if (strcmp(raw, last_raw) || tn - last_frame_ev >= 250000) {
        char ip[16];
        wifi_ip_str(in.source, ip);
        ev_begin("frame");
        printf(",\"src\":\"%s\",\"name\":", ip); jstr(src.name);
        emit_raw_vals(&in, &src);
        printf(",\"applied\":%u,\"live\":%s", in.applied, live ? "true" : "false");
        ev_end();
        snprintf(last_raw, sizeof last_raw, "%s", raw);
        last_frame_ev = tn;
      }
      last_updates = src.updates;
    }
    if (tn - last_stats_ev >= 1000000) {
      last_stats_ev = tn;
      ev_begin("stats");
      printf(",\"hz\":%.2f,\"jitter\":%.3f,\"frames\":%d,\"rx\":%u,\"beacons\":%u,\"malformed\":%u,"
             "\"dropped_filter\":%u,\"dropped_other\":%u,\"applied\":%u,\"posts\":%u,\"posts_ok\":%u,"
             "\"posts_failed\":%u,\"pending\":%d,\"live\":%s,\"present\":%s,\"listening\":%s,"
             "\"interval_us\":%d,\"channels\":%d,\"restarts\":%u,\"released\":%s,\"releasing\":%d,"
             "\"releases_ok\":%u,\"releases_failed\":%u",
             in.rate_hz, in.jitter_ms, in.frames_last_s, in.rx, in.beacons, in.malformed, in.dropped_filter,
             in.dropped_other, in.applied, in.posts, in.posts_ok, in.posts_failed, in.pending,
             live ? "true" : "false", src.present ? "true" : "false", in.listening ? "true" : "false",
             in.interval_us, in.channels, in.restarts, in.released ? "true" : "false", in.releasing,
             in.releases_ok, in.releases_failed);
      printf(",\"release\":"); jstr(st.wifi_release);
      ev_end();
      if (in.bound && in.frames_last_s > 0 && (min_rate < 0 || in.rate_hz >= min_rate) &&
          (max_rate < 0 || in.rate_hz <= max_rate))
        rate_ok_seen = true;
    }
    if (expect_streaming && in.bound && (int)in.applied >= min_frames && (!expect_src || in.source == expect_src) &&
        (!expect_nch || in.nch == expect_nch) && (min_rate < 0 && max_rate < 0 ? true : rate_ok_seen)) {
      code = 0;
      reason = "expectations met";
      break;
    }

    plat_mutex_lock(&cmd_mu);
    int nc = ncmds;
    char local[16][128];
    memcpy(local, cmds, sizeof local);
    ncmds = 0;
    plat_mutex_unlock(&cmd_mu);
    for (int i = 0; i < nc; i++) {
      char *c = local[i], *arg = strchr(c, ' ');
      if (arg) *arg++ = 0;
      ev_begin("cmd");
      printf(",\"cmd\":"); jstr(c);
      ev_end();
      if (!strcmp(c, "quit")) { quit = true; reason = "quit"; code = 0; }
      else if (!strcmp(c, "stop")) wifi_stop(w);
      else if (!strcmp(c, "start")) wifi_start(w, plat_now_us());
      else if (!strcmp(c, "release")) wifi_release(w, plat_now_us());
      else if (!strcmp(c, "connect")) wifi_connect(w, plat_now_us());
      else {
        if (!strcmp(c, "target")) snprintf(cfg.module_ip, sizeof cfg.module_ip, "%s", arg ? arg : "");
        else if (!strcmp(c, "interval") && arg) cfg.interval_us = atoi(arg);
        else if (!strcmp(c, "channels") && arg) cfg.channels = atoi(arg);
        else if (!strcmp(c, "http-port") && arg) cfg.http_port = (uint16_t)atoi(arg);
        else if (!strcmp(c, "port") && arg) cfg.port = (uint16_t)atoi(arg);
        wifi_configure(w, &cfg, plat_now_us());
      }
    }
    int64_t sleep = poll_us - (plat_now_us() - t0);
    if (sleep > 0) plat_sleep_us(sleep);
  }
  emit_logs();
  int64_t p99 = 0, acc = 0;
  for (int i = 0; i <= 1000; i++) {
    acc += hist[i];
    if (acc * 100 >= npoll * 99) { p99 = (int64_t)i * 10 + 10; break; }
  }
  ev_begin("poll");
  printf(",\"n\":%lld,\"avg_us\":%.1f,\"max_us\":%lld,\"p99_us\":%lld", (long long)npoll,
         npoll ? (double)sum_us / (double)npoll : 0.0, (long long)max_us, (long long)p99);
  ev_end();
  int64_t tc = plat_now_us();
  wifi_close(w);                      /* the program's exit: its release, and the wait for the answers */
  emit_logs();
  ev_begin("exit");
  printf(",\"code\":%d,\"reason\":", code); jstr(reason);
  printf(",\"close_ms\":%lld", (long long)((plat_now_us() - tc) / 1000));
  ev_end();
  /* not exit(): musl locks every FILE to flush it, and the stdin thread holds stdin's lock in fgets */
  fflush(stdout);
  fflush(stderr);
  _Exit(code);
}

int main(int argc, char **argv) {
  if (argc > 1) return probe(argc, argv);
  return self_test();
}
