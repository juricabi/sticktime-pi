/* wifi_net: the activation POST against a scripted HTTP server on 127.0.0.1, and the helpers.
 * POSIX only (a server thread); on Windows it only says so. */
#include "t.h"
#include "wifi_net.h"

#ifdef _WIN32
int main(void) {
  printf("test_wifi_net: POSIX only, skipped\n");
  return 0;
}
#else

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---------------------------------------------------------------- scripted server */
/* Each accepted connection gets the next reply of the script: a string to send after the request
 * head + body arrived (then close), or SILENT (read, never answer, wait for the client to go). */
#define SILENT ((const char *)1)
#define MAXCONN 4

typedef struct {
  int ls;
  uint16_t port;
  const char *replies[MAXCONN];
  int nreplies;
  char got[MAXCONN][1024];      /* the requests as received */
  int nconn;
  bool stop;
  pthread_t th;
} server_t;

static bool request_complete(const char *b, size_t n) {
  const char *e = strstr(b, "\r\n\r\n");
  if (!e) return false;
  const char *cl = strstr(b, "Content-Length: ");
  long len = cl ? strtol(cl + 16, NULL, 10) : 0;
  return (size_t)(e + 4 - b) + (size_t)len <= n;
}

static void *serve(void *arg) {
  server_t *sv = arg;
  while (!sv->stop && sv->nconn < sv->nreplies) {
    struct pollfd p = { sv->ls, POLLIN, 0 };
    if (poll(&p, 1, 50) <= 0) continue;
    int c = accept(sv->ls, NULL, NULL);
    if (c < 0) continue;
    int i = sv->nconn;
    size_t n = 0;
    char *b = sv->got[i];
    while (n < sizeof sv->got[i] - 1 && !request_complete(b, n)) {
      struct pollfd q = { c, POLLIN, 0 };
      if (poll(&q, 1, 2000) <= 0) break;
      ssize_t k = recv(c, b + n, sizeof sv->got[i] - 1 - n, 0);
      if (k <= 0) break;
      n += (size_t)k;
      b[n] = 0;
    }
    sv->nconn++;
    const char *r = sv->replies[i];
    if (r == SILENT) {
      /* until the client closes (or the test ends) */
      for (;;) {
        struct pollfd q = { c, POLLIN, 0 };
        if (sv->stop) break;
        if (poll(&q, 1, 50) > 0) {
          char t[64];
          if (recv(c, t, sizeof t, 0) <= 0) break;
        }
      }
    } else if (r) {
      send(c, r, strlen(r), MSG_NOSIGNAL);
    }
    close(c);
  }
  return NULL;
}

static void server_start(server_t *sv, int n, const char **replies) {
  memset(sv, 0, sizeof *sv);
  sv->ls = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(sv->ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(bind(sv->ls, (struct sockaddr *)&a, sizeof a) == 0);
  socklen_t al = sizeof a;
  getsockname(sv->ls, (struct sockaddr *)&a, &al);
  sv->port = ntohs(a.sin_port);
  CHECK(listen(sv->ls, 8) == 0);
  for (int i = 0; i < n; i++) sv->replies[i] = replies[i];
  sv->nreplies = n;
  pthread_create(&sv->th, NULL, serve, sv);
}

static void server_stop(server_t *sv) {
  sv->stop = true;
  pthread_join(sv->th, NULL);
  close(sv->ls);
}

static uint32_t LO(void) { return htonl(INADDR_LOOPBACK); }

static wifi_actreq_t req(uint16_t port, bool end_first) {
  wifi_actreq_t r;
  memset(&r, 0, sizeof r);
  r.ip = LO();
  r.http_port = port;
  r.session = 7;
  r.end_first = end_first;
  wifi_build_body(r.body, sizeof r.body, "joystick_begin", 4000, 16);
  return r;
}

static const char OK200[] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 2\r\n\r\nok";

/* ---------------------------------------------------------------- tests */
static void Post_Ok(void) {
  server_t sv;
  const char *r[] = { OK200 };
  server_start(&sv, 1, r);
  wifi_actreq_t rq = req(sv.port, false);
  wifi_actres_t rs;
  wifi_net_post(&rq, &rs, 2000, NULL, NULL);
  server_stop(&sv);
  CHECK(rs.ok);
  CHECK_INT(rs.status, 200);
  CHECK_STR(rs.reason, "OK");
  CHECK_STR(rs.err, "");
  CHECK(rs.ip == LO());
  CHECK_INT(rs.session, 7);
  CHECK(!rs.restart);
  CHECK_INT(rs.end_status, 0);
  CHECK_INT(sv.nconn, 1);
  CHECK(strncmp(sv.got[0], "POST /udpcontrol HTTP/1.1\r\n", 27) == 0);
  char host[32];
  snprintf(host, sizeof host, "\r\nHost: 127.0.0.1:%u\r\n", sv.port);
  CHECK(strstr(sv.got[0], host) != NULL);
  CHECK(strstr(sv.got[0], "\r\nUser-Agent: " WIFI_USER_AGENT "\r\n") != NULL);
  CHECK(strstr(sv.got[0], "\r\n\r\naction=joystick_begin&interval=4000&channels=16") != NULL);
}

static void Post_Restart_EndThenBegin(void) {
  server_t sv;
  const char *r[] = { OK200, "HTTP/1.1 200 OK\r\n\r\nok" };   /* the second without Content-Length: read to close */
  server_start(&sv, 2, r);
  wifi_actreq_t rq = req(sv.port, true);
  wifi_actres_t rs;
  wifi_net_post(&rq, &rs, 2000, NULL, NULL);
  server_stop(&sv);
  CHECK_INT(sv.nconn, 2);
  CHECK(strstr(sv.got[0], "\r\n\r\naction=joystick_end") != NULL);
  CHECK(strstr(sv.got[0], "Content-Length: 19\r\n") != NULL);
  CHECK(strstr(sv.got[1], "\r\n\r\naction=joystick_begin&interval=4000&channels=16") != NULL);
  CHECK(rs.restart);
  CHECK_INT(rs.end_status, 200);
  CHECK_STR(rs.end_err, "");
  CHECK(rs.ok);
  CHECK_INT(rs.status, 200);
}

static void Post_Restart_EndFails_BeginStillSent(void) {
  server_t sv;
  const char *r[] = { "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n", OK200 };
  server_start(&sv, 2, r);
  wifi_actreq_t rq = req(sv.port, true);
  wifi_actres_t rs;
  wifi_net_post(&rq, &rs, 2000, NULL, NULL);
  server_stop(&sv);
  CHECK_INT(sv.nconn, 2);
  CHECK_INT(rs.end_status, 404);
  CHECK(rs.ok);
}

static void Post_Non2xx_IsNotOk(void) {
  server_t sv;
  const char *r[] = { "HTTP/1.0 500 Internal Server Error\r\n\r\n" };
  server_start(&sv, 1, r);
  wifi_actreq_t rq = req(sv.port, false);
  wifi_actres_t rs;
  wifi_net_post(&rq, &rs, 2000, NULL, NULL);
  server_stop(&sv);
  CHECK(!rs.ok);
  CHECK_INT(rs.status, 500);
  CHECK_STR(rs.reason, "Internal Server Error");
  CHECK_STR(rs.err, "");
}

static void Post_Refused(void) {
  /* a port nobody listens on: bind one, close it */
  int s = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bind(s, (struct sockaddr *)&a, sizeof a);
  socklen_t al = sizeof a;
  getsockname(s, (struct sockaddr *)&a, &al);
  close(s);
  wifi_actreq_t rq = req(ntohs(a.sin_port), false);
  wifi_actres_t rs;
  wifi_net_post(&rq, &rs, 2000, NULL, NULL);
  CHECK(!rs.ok);
  CHECK_INT(rs.status, 0);
  CHECK_STR(rs.err, "connection refused");
  /* a restart to it: both fail, no crash */
  rq.end_first = true;
  wifi_net_post(&rq, &rs, 2000, NULL, NULL);
  CHECK(!rs.ok);
  CHECK_INT(rs.end_status, 0);
  CHECK_STR(rs.end_err, "connection refused");
  CHECK_STR(rs.err, "connection refused");
}

static void Post_Timeout(void) {
  server_t sv;
  const char *r[] = { SILENT };
  server_start(&sv, 1, r);
  wifi_actreq_t rq = req(sv.port, false);
  wifi_actres_t rs;
  int64_t t0 = wifi_net_now_us();
  wifi_net_post(&rq, &rs, 300, NULL, NULL);
  int64_t dt = wifi_net_now_us() - t0;
  server_stop(&sv);
  CHECK(!rs.ok);
  CHECK_INT(rs.status, 0);
  CHECK_STR(rs.err, "timed out after 1 s");
  CHECK(dt >= 290000 && dt < 1500000);
}

static int64_t abort_at;
static int abort_calls;
static bool abort_after(void *ud) {
  (void)ud;
  abort_calls++;
  return wifi_net_now_us() >= abort_at;
}

static void Post_Aborted(void) {
  server_t sv;
  const char *r[] = { SILENT };
  server_start(&sv, 1, r);
  wifi_actreq_t rq = req(sv.port, true);           /* abort during the joystick_end: no begin */
  wifi_actres_t rs;
  int64_t t0 = wifi_net_now_us();
  abort_at = t0 + 150000;
  abort_calls = 0;
  wifi_net_post(&rq, &rs, 5000, abort_after, NULL);
  int64_t dt = wifi_net_now_us() - t0;
  server_stop(&sv);
  CHECK(!rs.ok);
  CHECK_STR(rs.err, "aborted");
  CHECK_STR(rs.end_err, "aborted");
  CHECK_INT(sv.nconn, 1);
  CHECK(abort_calls >= 1);
  CHECK(dt >= 140000 && dt < 1000000);
}

static void Post_BadReplies(void) {
  struct { const char *reply; const char *err; } cases[] = {
    { "garbage that is not http\r\n\r\n", "not an HTTP reply" },
    { "", "closed without a reply" },
    { "HTTP/1.1 2", "incomplete reply" },
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    server_t sv;
    const char *r[] = { cases[i].reply };
    server_start(&sv, 1, r);
    wifi_actreq_t rq = req(sv.port, false);
    wifi_actres_t rs;
    wifi_net_post(&rq, &rs, 2000, NULL, NULL);
    server_stop(&sv);
    CHECK(!rs.ok);
    CHECK_INT(rs.status, 0);
    CHECK_STR(rs.err, cases[i].err);
  }
}

static void Post_BodyTooLong(void) {
  wifi_actreq_t rq = req(80, false);
  memset(rq.body, 'a', sizeof rq.body - 1);
  rq.body[sizeof rq.body - 1] = 0;
  wifi_actres_t rs;
  wifi_net_post(&rq, &rs, 100, NULL, NULL);     /* fits the 512-byte request: goes out (and fails) */
  CHECK(!rs.ok);
}

static void Helpers(void) {
  char t[64];
  wifi_net_err_text(ECONNREFUSED, t, sizeof t);
  CHECK_STR(t, "connection refused");
  wifi_net_err_text(ETIMEDOUT, t, sizeof t);
  CHECK_STR(t, "timed out");
  wifi_net_err_text(EADDRINUSE, t, sizeof t);
  CHECK_STR(t, "address already in use");
  wifi_net_err_text(0, t, sizeof t);
  CHECK_STR(t, "");
  wifi_net_err_text(99999, t, sizeof t);
  CHECK_STR(t, "error 99999");
  wifi_net_err_text(ECONNREFUSED, t, 4);
  CHECK_STR(t, "con");
  wifi_net_err_text(ECONNREFUSED, t, 0);          /* no write */
  CHECK_INT(wifi_net_init(), 0);
  int64_t a = wifi_net_now_us(), b = wifi_net_now_us();
  CHECK(a > 0 && b >= a);

  char ips[256];
  int n = wifi_net_local_ipv4(ips, sizeof ips);
  CHECK(n >= 0);
  CHECK(strlen(ips) < sizeof ips);
  if (n == 0) CHECK_STR(ips, "");
  else CHECK(strchr(ips, '(') != NULL && strstr(ips, "127.0.0.1") == NULL);
  char tiny[6];
  memset(tiny, 'x', sizeof tiny);
  int m = wifi_net_local_ipv4(tiny, sizeof tiny);  /* too small for one entry: empty, terminated */
  CHECK(m == 0);
  CHECK_STR(tiny, "");
}

int main(void) {
  signal(SIGPIPE, SIG_IGN);
  Post_Ok();
  Post_Restart_EndThenBegin();
  Post_Restart_EndFails_BeginStillSent();
  Post_Non2xx_IsNotOk();
  Post_Refused();
  Post_Timeout();
  Post_Aborted();
  Post_BadReplies();
  Post_BodyTooLong();
  Helpers();
  T_DONE();
}
#endif
