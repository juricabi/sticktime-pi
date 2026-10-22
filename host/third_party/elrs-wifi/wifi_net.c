/* The activation POST and socket helpers: see wifi_net.h. MIT License, (c) juricabi. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "wifi_net.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET net_sock;
#define NET_BADSOCK INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
typedef int net_sock;
#define NET_BADSOCK (-1)
#endif

/* ================================================================ helpers */
int wifi_net_init(void) {
#ifdef _WIN32
  WSADATA d;
  return WSAStartup(MAKEWORD(2, 2), &d) == 0 ? 0 : -1;
#else
  return 0;
#endif
}

int wifi_net_errno(void) {
#ifdef _WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

int64_t wifi_net_now_us(void) {
#ifdef _WIN32
  static LARGE_INTEGER f;
  LARGE_INTEGER c;
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (int64_t)(c.QuadPart / f.QuadPart * 1000000 + c.QuadPart % f.QuadPart * 1000000 / f.QuadPart);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
}

void wifi_net_err_text(int e, char *out, size_t cap) {
  if (!cap) return;
  const char *t = NULL;
#ifdef _WIN32
  if (e == WSAECONNREFUSED) t = "connection refused";
  else if (e == WSAETIMEDOUT) t = "timed out";
  else if (e == WSAEHOSTUNREACH) t = "host unreachable";
  else if (e == WSAENETUNREACH) t = "network unreachable";
  else if (e == WSAECONNRESET) t = "connection reset";
  else if (e == WSAECONNABORTED) t = "connection aborted";
  else if (e == WSAEADDRNOTAVAIL) t = "address not available";
  else if (e == WSAEADDRINUSE) t = "address already in use";
  else if (e == WSAEACCES) t = "permission denied";
#else
  if (e == ECONNREFUSED) t = "connection refused";
  else if (e == ETIMEDOUT) t = "timed out";
  else if (e == EHOSTUNREACH) t = "host unreachable";
  else if (e == ENETUNREACH) t = "network unreachable";
  else if (e == ECONNRESET) t = "connection reset";
  else if (e == ECONNABORTED) t = "connection aborted";
  else if (e == EPIPE) t = "connection closed";
  else if (e == EADDRNOTAVAIL) t = "address not available";
  else if (e == EADDRINUSE) t = "address already in use";
  else if (e == EACCES) t = "permission denied";
#endif
  if (t) snprintf(out, cap, "%s", t);
  else if (e) snprintf(out, cap, "error %d", e);
  else out[0] = 0;     /* lost (Windows clears it when the failed socket is closed) */
}

static bool in_progress(int e) {
#ifdef _WIN32
  return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
  return e == EINPROGRESS || e == EAGAIN || e == EWOULDBLOCK || e == EINTR;
#endif
}

static void sock_close(net_sock s) {
#ifdef _WIN32
  closesocket(s);
#else
  close(s);
#endif
}

static int sock_nonblock(net_sock s) {
#ifdef _WIN32
  u_long on = 1;
  return ioctlsocket(s, FIONBIO, &on) == 0 ? 0 : -1;
#else
  int f = fcntl(s, F_GETFL, 0);
  return f < 0 || fcntl(s, F_SETFL, f | O_NONBLOCK) < 0 ? -1 : 0;
#endif
}

/* 1 ready, 0 timeout, -1 error */
static int wait_sock(net_sock s, bool wr, int ms) {
#ifdef _WIN32
  fd_set f, e;
  FD_ZERO(&f);
  FD_ZERO(&e);
  FD_SET(s, &f);
  FD_SET(s, &e);
  struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
  int r = select(0, wr ? NULL : &f, wr ? &f : NULL, &e, &tv);
  return r < 0 ? -1 : r > 0 ? 1 : 0;
#else
  struct pollfd p = { s, (short)(wr ? POLLOUT : POLLIN), 0 };
  int r = poll(&p, 1, ms);
  if (r < 0) return errno == EINTR ? 0 : -1;
  return r > 0 ? 1 : 0;
#endif
}

/* Wait in slices of at most 100 ms so an abort ends a hanging request quickly.
 * 1 ready, 0 deadline passed, -1 aborted, -2 error */
static int wait_io(net_sock s, bool wr, int64_t deadline, wifi_abort_fn aborted, void *ud) {
  for (;;) {
    int64_t left = deadline - wifi_net_now_us();
    if (left <= 0) return 0;
    int ms = left > 100000 ? 100 : (int)((left + 999) / 1000);
    int r = wait_sock(s, wr, ms);
    if (r > 0) return 1;
    if (r < 0) return -2;
    if (aborted && aborted(ud)) return -1;
  }
}

static bool head_end(const char *b, size_t n, size_t *len) {
  for (size_t i = 0; i + 1 < n; i++) {
    if (b[i] == '\n' && b[i + 1] == '\n') { *len = i + 2; return true; }
    if (i + 3 < n && b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n') { *len = i + 4; return true; }
  }
  return false;
}

/* ================================================================ POST */
/* One POST /udpcontrol within timeout_ms. Returns the HTTP status (> 0), or 0 with err set; *was_aborted
 * tells an abort apart. Any 2xx is success for the caller (the body, "ok", is not checked). */
static int post_one(uint32_t ip, uint16_t http_port, const char *body, int timeout_ms, wifi_abort_fn aborted,
                    void *ud, char *reason, size_t rcap, char *err, size_t ecap, bool *was_aborted) {
  if (rcap) reason[0] = 0;
  if (ecap) err[0] = 0;
  *was_aborted = false;
  char req[512];
  int len = wifi_build_post(req, sizeof req, ip, http_port, body);
  if (len < 0) { snprintf(err, ecap, "request too long"); return 0; }
  int64_t deadline = wifi_net_now_us() + (int64_t)timeout_ms * 1000;
  int secs = (timeout_ms + 999) / 1000;

  net_sock s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == NET_BADSOCK) { wifi_net_err_text(wifi_net_errno(), err, ecap); return 0; }
#ifndef _WIN32
  fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  int code = 0, e = 0, st;
  if (sock_nonblock(s) != 0) { wifi_net_err_text(wifi_net_errno(), err, ecap); goto done; }
  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(http_port);
  a.sin_addr.s_addr = ip;
  if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
    e = wifi_net_errno();
    if (!in_progress(e)) { wifi_net_err_text(e, err, ecap); goto done; }
    st = wait_io(s, true, deadline, aborted, ud);
    if (st == 0) { snprintf(err, ecap, "timed out after %d s", secs); goto done; }
    if (st == -1) { snprintf(err, ecap, "aborted"); *was_aborted = true; goto done; }
    if (st < 0) { snprintf(err, ecap, "wait failed"); goto done; }
#ifdef _WIN32
    int el = sizeof e;
#else
    socklen_t el = sizeof e;
#endif
    e = 0;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&e, &el) != 0) e = wifi_net_errno();
    if (e) { wifi_net_err_text(e, err, ecap); goto done; }
  }

  for (int sent = 0; sent < len;) {
#ifdef MSG_NOSIGNAL
    int k = (int)send(s, req + sent, (size_t)(len - sent), MSG_NOSIGNAL);
#else
    int k = (int)send(s, req + sent, len - sent, 0);
#endif
    if (k > 0) { sent += k; continue; }
    e = wifi_net_errno();
    if (k < 0 && in_progress(e)) {
      st = wait_io(s, true, deadline, aborted, ud);
      if (st == 1) continue;
      if (st == -1) { snprintf(err, ecap, "aborted"); *was_aborted = true; }
      else if (st == 0) snprintf(err, ecap, "timed out after %d s", secs);
      else snprintf(err, ecap, "wait failed");
      goto done;
    }
    wifi_net_err_text(e, err, ecap);
    goto done;
  }

  char resp[1024];
  size_t got = 0, hlen = 0;
  long clen = -1;
  bool timed_out = false, have_head = false;
  for (;;) {
    if (got >= sizeof resp - 1) break;          /* the status line is all we need */
    int k = (int)recv(s, resp + got, (int)(sizeof resp - 1 - got), 0);
    if (k > 0) {
      got += (size_t)k;
      if (!have_head && head_end(resp, got, &hlen)) {
        have_head = true;
        clen = wifi_http_content_length(resp, hlen);
      }
      /* read the body too when its length is known, so the module never sees a reset */
      if (have_head && clen >= 0 && got >= hlen + (size_t)clen) break;
      continue;
    }
    if (k == 0) break;                          /* Connection: close */
    e = wifi_net_errno();
    if (in_progress(e)) {
      st = wait_io(s, false, deadline, aborted, ud);
      if (st == 1) continue;
      if (st == -1) { snprintf(err, ecap, "aborted"); *was_aborted = true; goto done; }
      timed_out = st == 0;
      if (!timed_out) wifi_net_err_text(e, err, ecap);
      break;
    }
    wifi_net_err_text(e, err, ecap);            /* e.g. reset after the reply: the status still counts */
    break;
  }
  resp[got] = 0;
  code = wifi_parse_http_status(resp, got, reason, rcap);
  if (code > 0) {
    if (ecap) err[0] = 0;
  } else {
    if (code == 0) snprintf(err, ecap, "not an HTTP reply");
    else if (timed_out) snprintf(err, ecap, "timed out after %d s", secs);
    else if (!err[0]) snprintf(err, ecap, got ? "incomplete reply" : "closed without a reply");
    code = 0;
  }
done:
  if (code <= 0 && ecap && !err[0]) snprintf(err, ecap, "failed");
  sock_close(s);
  return code;
}

void wifi_net_post(const wifi_actreq_t *rq, wifi_actres_t *rs, int timeout_ms, wifi_abort_fn aborted, void *ud) {
  memset(rs, 0, sizeof *rs);
  rs->ip = rq->ip;
  rs->session = rq->session;
  rs->restart = rq->end_first;
  bool ab = false;
  if (rq->end_first) {
    char unused[32];
    rs->end_status = post_one(rq->ip, rq->http_port, "action=joystick_end", timeout_ms, aborted, ud,
                              unused, sizeof unused, rs->end_err, sizeof rs->end_err, &ab);
    if (ab) {
      snprintf(rs->err, sizeof rs->err, "aborted");
      return;
    }
  }
  rs->status = post_one(rq->ip, rq->http_port, rq->body, timeout_ms, aborted, ud, rs->reason, sizeof rs->reason,
                        rs->err, sizeof rs->err, &ab);
  rs->ok = rs->status >= 200 && rs->status <= 299;
}

/* ================================================================ local addresses */
int wifi_net_local_ipv4(char *out, size_t cap) {
  if (cap) out[0] = 0;
#ifdef _WIN32
  return 0;           /* GetAdaptersAddresses would need iphlpapi; the hosts on Windows are test builds */
#else
  struct ifaddrs *ifa = NULL;
  if (getifaddrs(&ifa) != 0) return 0;
  size_t len = 0;
  int n = 0;
  for (struct ifaddrs *i = ifa; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP))
      continue;
    char ip[16];
    wifi_ip_str(((struct sockaddr_in *)(void *)i->ifa_addr)->sin_addr.s_addr, ip);
    int k = snprintf(out + len, cap - len, "%s%s (%s)", n ? ", " : "", ip, i->ifa_name);
    if (k < 0 || (size_t)k >= cap - len) {
      if (cap > len) out[len] = 0;      /* keep the whole entries only */
      break;
    }
    len += (size_t)k;
    n++;
  }
  freeifaddrs(ifa);
  return n;
#endif
}
