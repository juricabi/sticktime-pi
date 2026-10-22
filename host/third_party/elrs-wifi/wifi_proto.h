/* ELRS / Crossfire WiFi joystick protocol engine: pure logic, no sockets, threads, clock or malloc.
 * MIT License, (c) juricabi. Home: github.com/juricabi/elrs-crossfire-wifi-joystick-linux (core/);
 * sticktime-pi vendors this directory unmodified (host/third_party/elrs-wifi/).
 *
 * A C port of JoystickEngine.cs from github.com/juricabi/ELRS-Crossfire-WiFi-Joystick-Windows
 * (MIT, same author), cross-checked against ExpressLRS src/lib/WIFI/wifiJoystick.cpp.
 *
 * Wire format (UDP, the module sends everything to port 11000):
 *   ELRS beacon       "ELRS" u8 version u16le port u8 name_len name   (every 5 s, only while not streaming)
 *   Crossfire beacon  "VELOCIDRONE" ...                                (~8 s, also while streaming)
 *   channel frame     u8 type(1, not checked) u8 count(4..16) count x u16le (0..32767)
 * Activation: HTTP POST /udpcontrol "action=joystick_begin&interval=<us>&channels=<n>"; the module
 * streams to the IP that posted. Module identity = source IPv4 address.
 * What we ask for (the "request"): ELRS (3.4.0 and later) takes interval >= 1000 us (less, or 0, means
 * 10000) and channels 1..16 (0 means 8, more than 16 means 16), but only while it is not streaming: a
 * joystick_begin to a streaming module just moves the stream to the poster. So a new request reaches a
 * streaming module only as "action=joystick_end" followed by joystick_begin (a restart). Crossfire
 * starts on the same POST but ignores interval and channels (always 16 channels, about 90 Hz), and
 * whether it honours joystick_end is unknown, so it is never sent one.
 *
 * Times are microseconds on a monotonic clock, passed in. IPs are IPv4 in network byte order
 * (as in sin_addr.s_addr). input_wifi.c owns the socket and the HTTP worker thread. */
#ifndef WIFI_PROTO_H
#define WIFI_PROTO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define WIFI_PORT_DEFAULT        11000
#define WIFI_HTTP_PORT_DEFAULT   80
#define WIFI_MIN_CH              4
#define WIFI_MAX_CH              16
#define WIFI_RAW_MAX             32767
#define WIFI_XF_PLACEHOLDER      0xF26A     /* Crossfire: channel without data (62058) */
#define WIFI_SOURCE_TIMEOUT_US   3000000    /* lock released when now - last valid frame > this */
#define WIFI_THROTTLE_US         5000000    /* one activation POST per IP per this */
#define WIFI_FW_HINT_US          4000000    /* "activated but no data" hint after this */
#define WIFI_STATS_PERIOD_US     1000000
#ifndef WIFI_USER_AGENT
#define WIFI_USER_AGENT          "elrs-wifi-joystick"
#endif
#define WIFI_TABLE               8          /* throttle / warned / no-stick-data / beacon / restarted tables */

/* the request: supported values */
#define WIFI_INTERVAL_MIN        1000       /* ELRS turns anything below 1000 us into 10000 */
#define WIFI_INTERVAL_MAX        100000     /* 10 Hz: slower than this makes no sense for sticks */
#define WIFI_INTERVAL_DEFAULT    4000       /* 250 Hz */
#define WIFI_CHANNELS_DEFAULT    16
#define WIFI_XF_CHANNELS         16         /* Crossfire always sends 16 */
#define WIFI_XF_BEACON_US        20000000   /* Crossfire beacons about every 8 s, also while streaming: 20 s
                                               without one (two lost) means it is not Crossfire */
/* the rates a UI offers, as intervals in us: 1000, 500, 333, 250, 200, 150, 100 and 50 Hz */
#define WIFI_INTERVAL_PRESETS    { 1000, 2000, 3000, 4000, 5000, 6667, 10000, 20000 }
#define WIFI_N_INTERVAL_PRESETS  8

typedef enum { WIFI_ST_STOPPED, WIFI_ST_SEARCHING, WIFI_ST_STREAMING, WIFI_ST_ERROR } wifi_state_t;
typedef enum { WIFI_KIND_UNKNOWN, WIFI_KIND_ELRS, WIFI_KIND_XF } wifi_kind_t;

const char *wifi_state_name(wifi_state_t s);    /* "stopped", "searching", "streaming", "error" */
const char *wifi_kind_name(wifi_kind_t k);      /* "WiFi", "ELRS", "Crossfire" */

int wifi_clamp_interval(int us);          /* to WIFI_INTERVAL_MIN..MAX */
int wifi_clamp_channels(int n);           /* to WIFI_MIN_CH..WIFI_MAX_CH */
int wifi_interval_hz(int us);             /* the rate a UI shows: 1000000 / us, rounded (4000 -> 250, 3000 -> 333) */

/* activate hook results */
#define WIFI_ACT_OK       1     /* done, succeeded (synchronous: tests) */
#define WIFI_ACT_FAILED   0     /* done, failed */
#define WIFI_ACT_QUEUED  (-1)   /* the result comes later through wifi_proto_activation_done */
#define WIFI_ACT_BUSY    (-2)   /* not sent (already in flight, queue full, not listening): no throttle entry */

/* One activation for the host to send: POST joystick_begin asking for interval_us and channels;
 * restart: POST action=joystick_end first (see "the request" above). */
typedef struct { uint32_t ip; bool restart; int interval_us; int channels; } wifi_act_t;

typedef struct {
  void *ud;
  void (*log)(void *ud, const char *msg);                            /* one line, no '\n' */
  void (*state)(void *ud, wifi_state_t s, const char *detail);
  void (*frame)(void *ud, uint32_t src, const uint16_t *raw, int n);   /* each applied frame, raw 0..32767 */
  void (*stats)(void *ud, double hz, double jitter_ms, int frames);
  int  (*activate)(void *ud, const wifi_act_t *a);                   /* WIFI_ACT_* */
} wifi_hooks_t;

typedef struct { uint32_t ip; int64_t t_us; } wifi_ipt_t;
typedef struct { uint32_t ip; wifi_kind_t kind; char name[32]; int64_t t_us; } wifi_beacon_info_t;

typedef struct {
  wifi_hooks_t h;
  uint32_t target;              /* explicit module (ActivationIP); 0 = discovery by beacon */
  bool bound; uint32_t bound_ip; int64_t bound_last_us;
  uint32_t warned[WIFI_TABLE];  int nwarned;           /* 2nd sources already warned about */
  uint32_t nostick[WIFI_TABLE]; int nnostick;          /* sources warned for placeholder streams */
  uint32_t nostick_ip;                                 /* the last one (status text) */
  wifi_ipt_t act[WIFI_TABLE];   int nact;              /* last activation per IP, LRU-evicted */
  bool first_act_set; int64_t first_act_us; uint32_t first_act_ip; bool hint_shown;
  wifi_beacon_info_t seen[WIFI_TABLE]; int nseen;      /* kind/name per IP from beacons (UI only) */
  /* stats window: online, same numbers as the C# list math */
  int frames, narr, ngaps; int64_t arr_first, arr_prev; double gap_mean, gap_m2; int64_t last_stats_us;
  double last_hz, last_jitter; int last_frames;        /* last emitted */
  /* last applied frame */
  uint16_t raw[WIFI_MAX_CH]; uint8_t nch; uint32_t seq; int64_t last_arrival_us; uint32_t last_src;
  /* lifetime counters (UI) */
  uint32_t rx, beacons, malformed, dropped_filter, dropped_other;
  wifi_state_t state; char detail[64];
  /* the request (kept by reset) */
  int interval_us, channels;
  uint32_t restart_ip;                                 /* a restart the host was too busy for: retried on tick */
  uint32_t restarted[WIFI_TABLE]; int nrestarted;      /* modules restarted for a channel mismatch (once each) */
  bool xf_noted;                                       /* "Crossfire ignores the request" logged */
  int64_t bound_since_us;                              /* when the lock was taken */
  bool restart_wait;                                   /* a request restart waiting to know it is no Crossfire */
  uint32_t restarts;                                   /* restarts sent (lifetime counter) */
} wifi_proto_t;

void wifi_proto_init(wifi_proto_t *p, const wifi_hooks_t *h);   /* request: WIFI_INTERVAL_DEFAULT, WIFI_CHANNELS_DEFAULT */
void wifi_proto_reset(wifi_proto_t *p);                 /* ResetSessionState: keeps target, request, hooks, counters */
/* One datagram. now_us: poll time (lock logic). arrival_us: receive time (stats); pass now_us if unknown. */
void wifi_proto_packet(wifi_proto_t *p, const uint8_t *d, size_t n, uint32_t src, int64_t now_us, int64_t arrival_us);
/* Lock release, the "no data" hint, explicit-target re-POST. Call it on every poll. */
void wifi_proto_tick(wifi_proto_t *p, int64_t now_us);
/* SetTarget. Trims spaces/tabs. NULL/blank -> discovery (target 0, nothing else). Invalid text ->
 * logged, target unchanged, -1. Else: target = ip, its throttle entry removed, unlocked, logged,
 * state Searching "connecting to <ip>", and the activation POST goes out at once. Returns 0. */
int  wifi_proto_set_target(wifi_proto_t *p, const char *text, int64_t now_us);
/* EnsureStreaming: activation POST to ip unless one went out less than WIFI_THROTTLE_US ago */
void wifi_proto_ensure(wifi_proto_t *p, uint32_t ip, int64_t now_us);
/* Set what joystick_begin asks for; values are clamped. If they changed while locked to a module that is
 * not Crossfire, its stream is restarted so they apply (wifi_proto_restart); locked to Crossfire, that is
 * logged once per session. Returns true when the clamped values differ from the old ones. */
bool wifi_proto_set_request(wifi_proto_t *p, int interval_us, int channels, int64_t now_us);
/* Only store the request (clamped), restarting nothing: for a host that changes the target and the
 * request together. Call it, then wifi_proto_set_target: the new target's joystick_begin asks for it,
 * and the module being left is not restarted for nothing. */
void wifi_proto_store_request(wifi_proto_t *p, int interval_us, int channels);
/* Restart the stream of the locked module, else of the target, so it uses the request now
 * (joystick_end, then joystick_begin; no throttle). Crossfire: logged, nothing sent. Returns false when
 * there is no module or it is Crossfire. When the host is busy, it is retried on every tick. A locked
 * module that might be Crossfire (16 channels and no beacon yet this session) is restarted only once
 * WIFI_XF_BEACON_US of lock passed without a Crossfire beacon (decided on tick; dropped when its beacon
 * says Crossfire or the lock is lost).
 * Also sent by itself, once per module and session, when a module that is surely not Crossfire (ELRS by
 * its beacon, or a channel count other than 16) streams a channel count other than the request. */
bool wifi_proto_restart(wifi_proto_t *p, int64_t now_us);
void wifi_proto_activation_done(wifi_proto_t *p, uint32_t ip, bool ok, int64_t now_us);
void wifi_proto_set_state(wifi_proto_t *p, wifi_state_t s, const char *detail);  /* lifecycle (input_wifi.c) */
void wifi_proto_poll_stats(wifi_proto_t *p, int64_t now_us);   /* emits when now - last >= 1 s */
void wifi_proto_emit_stats(wifi_proto_t *p, int64_t now_us);
void wifi_proto_add_arrival(wifi_proto_t *p, int64_t t_us);    /* AddArrivalForTest */
wifi_kind_t wifi_proto_kind(const wifi_proto_t *p, uint32_t ip);   /* from beacons seen this session */
const char *wifi_proto_module_name(const wifi_proto_t *p, uint32_t ip);  /* ELRS device name or "" */

/* ---- pure helpers ---- */
bool    wifi_starts_with(const uint8_t *d, size_t n, const char *prefix);   /* "" matches anything */
/* ELRS beacon: 1 full header (n >= 8), 0 "ELRS" but cut short, -1 not one. version/port -1 when absent.
 * name: up to namecap-1 bytes of what is present (name_len is not trusted), non-printables -> '?'. */
int     wifi_parse_elrs_beacon(const uint8_t *d, size_t n, int *version, int *port, char *name, size_t namecap);
int16_t wifi_raw_to_stick(int raw);       /* 0..32767 -> -1024..1024: the inverse of EdgeTX -> CRSF -> ELRS */
int     wifi_raw_percent(int raw);        /* clamp(raw, 0, 32767) * 100 / 32767 (the reference's bars) */
/* Dotted IPv4 "a.b.c.d" (decimal, 0..255, no leading zeros, nothing else), spaces/tabs trimmed.
 * 1 ok, 0 blank or NULL, -1 invalid. */
int     wifi_parse_ip(const char *text, uint32_t *ip);
void    wifi_ip_str(uint32_t ip, char out[16]);
/* "action=<a>&interval=<us>&channels=<n>"; length, or -1 when it does not fit */
int     wifi_build_body(char *buf, size_t cap, const char *action, int interval_us, int channels);
/* the whole POST /udpcontrol request (Host gets ":port" unless 80; User-Agent WIFI_USER_AGENT, which a
 * host may define, default "elrs-wifi-joystick"); length, or -1 when it does not fit */
int     wifi_build_post(char *buf, size_t cap, uint32_t ip, uint16_t http_port, const char *body);
/* HTTP status line: -1 need more bytes, 0 not HTTP, else the status code (100..999); reason copied when non-NULL */
int     wifi_parse_http_status(const char *buf, size_t n, char *reason, size_t rcap);
/* Content-Length of a complete response head (buf holds at least the head), -1 when absent or bad */
long    wifi_http_content_length(const char *buf, size_t n);

/* ---- activation queue: requests to the HTTP worker and its results (no locking: the caller locks) ----
 * The frame loop pushes requests and takes results; the worker takes requests and puts results.
 * A session counter (new on every start/stop) drops queued requests and results of an old session.
 * An IP stays "pending" from push until its result is taken, so it is never queued twice; a restart
 * pushed while a plain request for the IP still waits in the queue turns that one into the restart. */
#define WIFI_ACTQ 8

/* end_first: POST action=joystick_end before body (a restart) */
typedef struct { uint32_t ip; uint16_t http_port; uint32_t session; bool end_first; char body[64]; } wifi_actreq_t;
/* ok/status/reason/err: the joystick_begin; end_status/end_err: the joystick_end of a restart (0/"" otherwise) */
typedef struct {
  uint32_t ip; uint32_t session; bool ok; int status; char reason[32]; char err[64];
  bool restart; int end_status; char end_err[64];
} wifi_actres_t;

typedef struct {
  wifi_actreq_t req[WIFI_ACTQ]; int rq_head, rq_n;
  wifi_actres_t res[WIFI_ACTQ]; int rs_head, rs_n;
  uint32_t pending[WIFI_ACTQ]; int npending;     /* frame-loop side only */
  uint32_t session;
} wifi_actq_t;

void wifi_actq_init(wifi_actq_t *q);
void wifi_actq_new_session(wifi_actq_t *q);     /* drops queued requests, results and pending IPs */
bool wifi_actq_is_pending(const wifi_actq_t *q, uint32_t ip);
/* WIFI_ACT_QUEUED, or WIFI_ACT_BUSY when ip is pending already (unless an end_first push finds its
 * request still queued: that one becomes the restart, with this body), the queue is full or body is too long */
int  wifi_actq_push(wifi_actq_t *q, uint32_t ip, uint16_t http_port, const char *body, bool end_first);
bool wifi_actq_take(wifi_actq_t *q, wifi_actreq_t *out);          /* worker: next request */
bool wifi_actq_stale(const wifi_actq_t *q, uint32_t session);     /* worker: abort a request of an old session */
void wifi_actq_put(wifi_actq_t *q, const wifi_actres_t *r);       /* worker: old sessions are dropped */
bool wifi_actq_get(wifi_actq_t *q, wifi_actres_t *out);           /* frame loop: next result, unpends its IP */

#endif
