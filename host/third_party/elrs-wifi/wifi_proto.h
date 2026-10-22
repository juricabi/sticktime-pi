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
 * whether it honours joystick_end is unknown, so a module known as Crossfire is never sent one. Only
 * beacons tell the two apart (ELRS beacons only while idle, Crossfire about every 8 s also while
 * streaming): a 16-channel module without a beacon is taken for ELRS after WIFI_XF_BEACON_US of lock, so
 * a Crossfire whose beacons never reach the host (broadcasts blocked) can get a joystick_end.
 *
 * The rules of the request (the same in JoystickEngine.cs and in every doc of the three apps):
 * R1  Defaults 4000 us (250 Hz) and 16 channels. Supported: interval 1000..100000 us (presets
 *     WIFI_INTERVAL_PRESETS = 1000/500/333/250/200/150/100/50 Hz), channels 4..16.
 * R2  A change while locked: Crossfire (by its beacon) -> logged once per session, nothing sent. Surely no
 *     Crossfire (its ELRS beacon seen, or frames of a count other than 16) -> restarted now (joystick_end,
 *     then joystick_begin with the request). Unknown kind with 16 channels -> held until WIFI_XF_BEACON_US
 *     of the lock passed without a Crossfire beacon, then restarted; dropped on a Crossfire beacon. A held
 *     restart survives a lock loss of its module and goes out when that module locks again (after its
 *     window); dropped when that module's ELRS beacon gets it a joystick_begin carrying the request, or
 *     when another module takes the lock.
 * R3  Not locked: the next joystick_begin carries the request. What each module was last asked is kept
 *     per module (asked, WIFI_TABLE); a module that locks with another asked request than the request is
 *     restarted under R2's kind rules. (Checked on every applied frame: a locked module whose asked request
 *     differs is restarted too, which only wifi_proto_store_request without a switch to another module
 *     leads to.) A restart that did not take - it failed (R6) and its module is no longer locked, or a
 *     reset cut it short (R9) - leaves that module's asked request unknown, so its next lock restarts it.
 * R4  Explicit restart (wifi_proto_restart): locked -> as R2 (Crossfire: logged, nothing sent). Not
 *     locked, the module IP set: known as ELRS -> restarted; unknown or Crossfire -> a plain joystick_begin
 *     past the throttle, never joystick_end. Neither -> logged, nothing sent.
 * R5  A module surely no Crossfire streaming a channel count other than the request -> restarted once
 *     per module and session. Unknown kind streaming 16 while fewer are asked -> once R2's window passed
 *     without a Crossfire beacon, restarted once.
 * R6  A restart is done only when both POSTs got a 2xx (activation_done's ok). A failed one is sent again
 *     WIFI_THROTTLE_US after the last send while that module stays locked and is not known as Crossfire,
 *     WIFI_RESTART_TRIES sends per restart (request change); giving up is logged. A failed plain
 *     joystick_begin is never retried as a restart. A failed one whose module is no longer locked (a lock
 *     loss, another module IP, a reset) did not reach it: its asked request becomes unknown (R3).
 * R7  The module IP set to the locked module: the lock stays, nothing sent, logged. Another one:
 *     unlocked, state "connecting to <ip>", joystick_begin at once; a restart pending or held for the
 *     module being left is dropped.
 * R8  The module IP and the request together (wifi_proto_store_request, then wifi_proto_set_target): the
 *     new module's joystick_begin carries the request, the module being left gets nothing.
 * R9  A stop/start or a port change (wifi_proto_reset) starts a new session: lock, throttles and restart
 *     state cleared; the kinds from beacons and the asked requests kept, except that a module whose restart
 *     had not taken (its result still awaited, or failed) gets an unknown asked request (R3).
 * R10 Tables are bounded (WIFI_TABLE); the locked module's kind is never evicted. Full, a table drops the
 *     entry written longest ago (by a sequence number, so datagrams of one drain with the same time cannot
 *     make it drop the newest), never the locked module's or the module IP's kind or asked request. With
 *     more than WIFI_TABLE modules beaconing, each beacon round POSTs them all again (R13 allows it).
 * R11 (host) Lock loss, stop, exit and a switch to another module IP center the 8 axes and release the
 *     buttons; channels a frame does not carry are neutral; short gaps hold the last values.
 * R12 (host) wifi_proto_tick (lock release, retries, held restarts, target re-POST) runs at least every
 *     100 ms whatever traffic arrives.
 * R13 One plain activation POST per module per WIFI_THROTTLE_US; restarts bypass the throttle and restart it.
 *     (Plain: the automatic ones - beacons, the module IP's re-POST. The joystick_begin of R4 and R7 goes
 *     out at once too, and restarts the throttle.)
 *
 * Times are microseconds on a monotonic clock, passed in. IPs are IPv4 in network byte order
 * (as in sin_addr.s_addr). The host (sticktime-pi's input_wifi.c, the Linux app's engine.c) owns the
 * socket and the HTTP worker thread. */
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
#define WIFI_RESTART_TRIES       3          /* sends of a restart that fails (R6): again WIFI_THROTTLE_US
                                               after the last one while its module stays locked */
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
/* the rate in a log line: "250 Hz" for a preset, "286 Hz (3500 us)" otherwise (the rounded rate alone would
 * hide the interval: 3000 and 3001 us both read 333 Hz). As StreamRequest.Describe. Returns out. */
const char *wifi_describe_interval(int us, char *out, size_t cap);

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

/* Table entries; seq: when written (a counter: a full table drops the smallest, R10) */
typedef struct { uint32_t ip; int64_t t_us; int interval_us, channels; uint64_t seq; } wifi_ipt_t;   /* + the request it carried */
typedef struct { uint32_t ip; wifi_kind_t kind; char name[32]; uint64_t seq; } wifi_beacon_info_t;
/* interval_us 0: not known (a restart of it did not take, R3); channels then says why: WIFI_ASKED_* */
typedef struct { uint32_t ip; int interval_us, channels; uint64_t seq; } wifi_asked_t;
#define WIFI_ASKED_CUT     0   /* a reset cut its restart short (its result was still awaited) */
#define WIFI_ASKED_FAILED  1   /* its restart failed, and it was no longer locked to be sent again */

typedef struct {
  wifi_hooks_t h;
  uint32_t target;              /* explicit module (ActivationIP); 0 = discovery by beacon */
  bool bound; uint32_t bound_ip; int64_t bound_last_us;
  uint32_t warned[WIFI_TABLE];  int nwarned;           /* 2nd sources already warned about */
  uint32_t nostick[WIFI_TABLE]; int nnostick;          /* sources warned for placeholder streams */
  uint32_t nostick_ip;                                 /* the last one (status text) */
  int64_t nostick_last_us;                             /* its last placeholder frame (the state goes back after 3 s) */
  wifi_ipt_t act[WIFI_TABLE];   int nact;              /* last activation per IP (and its request), LRU-evicted */
  uint64_t tab_seq;                                    /* the last table entry's seq (never reset) */
  bool first_act_set; int64_t first_act_us; uint32_t first_act_ip; bool hint_shown;
  /* kind/name per IP from beacons, kept by reset (a module keeps its kind while it keeps its IP); the
   * kind decides whether a joystick_end may be sent, so the locked module is never evicted */
  wifi_beacon_info_t seen[WIFI_TABLE]; int nseen;
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
  /* a restart still to send: one the host was too busy for (sent when the POST in flight to its module is
   * done, or on tick, while its module is the locked one or, not locked, the module IP known as ELRS; else
   * kept until that module locks again), or with restart_retry the retry of a failed one (from
   * restart_at_us, only while its module stays locked) */
  uint32_t restart_ip; bool restart_retry; int64_t restart_at_us;
  uint32_t restart_res_ip;                             /* the restart whose result is awaited (activation_done) */
  uint32_t restart_bad_ip;                             /* the locked module whose last restart failed: its asked
                                                          request becomes unknown when its lock goes (R3, R6) */
  int64_t restart_sent_us;                             /* when the last restart went out */
  int restart_tries;                                   /* sends of the current restart (R6) */
  uint32_t restarted[WIFI_TABLE]; int nrestarted;      /* modules restarted this session: a mismatch restarts once */
  bool xf_noted;                                       /* "Crossfire ignores the request" logged */
  int64_t bound_since_us;                              /* when the lock was taken */
  bool restart_wait;                                   /* a request restart waiting to know it is no Crossfire */
  uint32_t restart_wait_ip;                            /* the module it is for */
  uint32_t restarts;                                   /* restarts sent (lifetime counter) */
  /* what each module was last asked for (R3; kept by reset: it streams on after a stop): the request of a
   * restart sent to it or of an activation after its ELRS beacon (it was not streaming then), else, from
   * its lock on, that of the last activation sent to it this session, or the request when it locked; not
   * known (interval_us 0) after a restart of it that did not take */
  wifi_asked_t asked[WIFI_TABLE]; int nasked;
} wifi_proto_t;

void wifi_proto_init(wifi_proto_t *p, const wifi_hooks_t *h);   /* request: WIFI_INTERVAL_DEFAULT, WIFI_CHANNELS_DEFAULT */
/* ResetSessionState: keeps target, request, hooks, counters, and what is known about modules (their kind
 * and name from beacons, what they were asked for): a module streams on after a stop. A restart whose
 * result was still awaited (the host drops it with wifi_actq_new_session) or that had failed leaves its
 * module with an unknown asked request: its next lock restarts it (R3, R9). */
void wifi_proto_reset(wifi_proto_t *p);
/* One datagram. now_us: poll time (lock logic). arrival_us: receive time (stats); pass now_us if unknown. */
void wifi_proto_packet(wifi_proto_t *p, const uint8_t *d, size_t n, uint32_t src, int64_t now_us, int64_t arrival_us);
/* Lock release, the "no data" hint, restarts pending, held or to be sent again, explicit-target re-POST;
 * the "sending no stick data" state ends when no placeholder frame came for WIFI_SOURCE_TIMEOUT_US (the
 * module went away). Call it on every poll, at least every 100 ms (R12). */
void wifi_proto_tick(wifi_proto_t *p, int64_t now_us);
/* SetTarget. Trims spaces/tabs. NULL/blank -> discovery (target 0, nothing else). Invalid text ->
 * logged, target unchanged, -1. The module streaming to us now (locked, data within
 * WIFI_SOURCE_TIMEOUT_US) -> target = ip, logged, the lock and a held restart for it stay, nothing is
 * sent. Else: target = ip, its throttle entry removed, unlocked, logged, state Searching "connecting to
 * <ip>", and the activation POST goes out at once. A restart pending, held or to be sent again for another
 * module is dropped (R7). Returns 0. */
int  wifi_proto_set_target(wifi_proto_t *p, const char *text, int64_t now_us);
/* EnsureStreaming: activation POST to ip unless one went out less than WIFI_THROTTLE_US ago */
void wifi_proto_ensure(wifi_proto_t *p, uint32_t ip, int64_t now_us);
/* Set what joystick_begin asks for; values are clamped. If they changed while locked to a module that is
 * not Crossfire, its stream is restarted so they apply (wifi_proto_restart, R2); locked to Crossfire, that
 * is logged once per session. Not locked: the next joystick_begin asks for them, and a module that was
 * last asked for another request (see "asked" below: e.g. one that streams on through a WiFi pause or a
 * stop) is restarted when it locks (R3). Returns true when the clamped values differ from the old ones. */
bool wifi_proto_set_request(wifi_proto_t *p, int interval_us, int channels, int64_t now_us);
/* Only store the request (clamped), restarting nothing now: for a host that changes the module IP to another
 * module and the request together (R8). Call it, then wifi_proto_set_target: the new target's joystick_begin
 * asks for it, and the module being left is not restarted for nothing. Not for the locked module when it
 * stays (the module IP unchanged, or set to it): its next frame finds it last asked for another request and
 * restarts it anyway (R3) - call wifi_proto_set_request then, which restarts it at once. */
void wifi_proto_store_request(wifi_proto_t *p, int interval_us, int channels);
/* Restart the stream of the locked module, else of the target, so it uses the request now
 * (joystick_end, then joystick_begin; no throttle, R4). Locked Crossfire: logged, nothing sent, false.
 * No module: logged, false. When the host is busy (a POST to that module in flight), it goes out as soon as
 * that POST's result is in (activation_done) or on a tick, while that module is the locked one or, not
 * locked, the module IP known as ELRS; any other module that is not locked gets it when it locks again
 * (held first if it may be Crossfire then: R2), never a joystick_end while it is not locked and not known
 * as ELRS (R4). Dropped when a beacon says Crossfire, another module locks or is set as the target, and on
 * reset. A locked module that might be Crossfire (16 channels and no beacon from it yet) is restarted only
 * once WIFI_XF_BEACON_US of lock passed without a Crossfire beacon (decided on tick; held through a lock
 * loss until it locks again; dropped when its beacon says Crossfire, another module locks or is set as the
 * target, and on reset).
 * Either is dropped when the module's ELRS beacon gets it activated: it is not streaming then, and the
 * joystick_begin carries the request. The target, not locked and not known as ELRS by its beacon, gets
 * only a joystick_begin past the throttle: it may be (or is) Crossfire, and a module that is not
 * streaming takes the request from the joystick_begin alone. A restart that fails is sent again (R6).
 * Also started by itself when a module streams a channel count other than the request (R5: surely no
 * Crossfire at once, unknown with 16 channels after the window; once per module and session), and when a
 * module locks that was last asked for another request (R3: "asked" is what a restart, or an activation
 * after its ELRS beacon, asked it for; else, from its first lock on, the request of the last activation
 * sent to it this session or, without one, the request when it locked: so a change made while it is not
 * locked - during its activation, a WiFi pause, a stop - reaches a module that streams on), or that a
 * restart that did not take left with an unknown request (it failed, then the lock went; a reset cut it
 * short).
 * A module found streaming another app's request keeps its rate (only its channel count is checked). */
bool wifi_proto_restart(wifi_proto_t *p, int64_t now_us);
/* The result of an activation the activate hook QUEUED. ok: it took - its joystick_begin got a 2xx and,
 * for a restart, its joystick_end too (a streaming module that missed the joystick_end only moved its
 * stream and kept its old request). A failed restart is sent again (R6) while its module stays locked; one
 * whose module is no longer locked leaves it with an unknown asked request (R3). A restart that waited for
 * this module's POST (the hook was BUSY) goes out now if it is due (see wifi_proto_restart). Call it with
 * the actq unlocked: it may call the activate hook. */
void wifi_proto_activation_done(wifi_proto_t *p, uint32_t ip, bool ok, int64_t now_us);
void wifi_proto_set_state(wifi_proto_t *p, wifi_state_t s, const char *detail);  /* lifecycle (input_wifi.c) */
void wifi_proto_poll_stats(wifi_proto_t *p, int64_t now_us);   /* emits when now - last >= 1 s */
void wifi_proto_emit_stats(wifi_proto_t *p, int64_t now_us);
void wifi_proto_add_arrival(wifi_proto_t *p, int64_t t_us);    /* AddArrivalForTest */
wifi_kind_t wifi_proto_kind(const wifi_proto_t *p, uint32_t ip);   /* from its beacons (kept by reset) */
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
