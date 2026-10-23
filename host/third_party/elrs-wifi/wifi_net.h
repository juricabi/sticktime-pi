/* The network side of the ELRS / Crossfire WiFi joystick that wifi_proto leaves to its host: the
 * activation POST (blocking: run it on a worker thread), and the socket helpers every host needs.
 * MIT License, (c) juricabi. Home: github.com/juricabi/elrs-crossfire-wifi-joystick-linux (core/);
 * sticktime-pi vendors this directory unmodified (host/third_party/elrs-wifi/).
 *
 * POSIX (Linux, macOS, BSD) and Windows (winsock: link ws2_32, call wifi_net_init first). No
 * allocation, no global state. */
#ifndef WIFI_NET_H
#define WIFI_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "wifi_proto.h"

#define WIFI_POST_TIMEOUT_MS 5000      /* connect + request + response head, per POST */

/* Called about every 100 ms while a POST waits; returning true ends it with err "aborted". */
typedef bool (*wifi_abort_fn)(void *ud);

/* One activation (blocking): POST rq->body to http://<rq->ip>:<rq->http_port>/udpcontrol. With
 * rq->end_first, "action=joystick_end" goes first (its answer in rs->end_status / rs->end_err; a
 * failure there does not stop the joystick_begin, unless it was aborted). Each POST gets timeout_ms.
 * rs is filled completely: ip, session and restart from rq; ok = the begin got a 2xx; status = its HTTP
 * status (0 when there was none); reason = its reason phrase; err = why it failed (or ""). The body of
 * the answer ("ok") is not checked, like the reference. aborted may be NULL.
 * With rq->release (R14), "action=joystick_end" alone is sent (rq->body unused): rs->release set, restart
 * false, ok/status/reason/err are the joystick_end's, end_status 0 and end_err "". */
void wifi_net_post(const wifi_actreq_t *rq, wifi_actres_t *rs, int timeout_ms, wifi_abort_fn aborted, void *ud);

/* "192.168.1.20 (wlan0), 10.0.0.2 (eth0)": the IPv4 addresses of the interfaces that are up, without
 * loopback. Returns how many (0 when there are none or the platform can't tell). */
int  wifi_net_local_ipv4(char *out, size_t cap);

int  wifi_net_init(void);              /* Windows: WSAStartup (once is enough); elsewhere nothing. 0 = ok */
int  wifi_net_errno(void);             /* the last socket error (errno / WSAGetLastError) */
/* A short English text for a socket error: "connection refused", "timed out", ..., else "error <n>";
 * "" for 0. */
void wifi_net_err_text(int e, char *out, size_t cap);
int64_t wifi_net_now_us(void);         /* monotonic clock, microseconds */

#endif
