/* ELRS / Crossfire WiFi joystick protocol engine: see wifi_proto.h. MIT License, (c) juricabi.
 * Port of JoystickEngine.cs (ELRS-Crossfire-WiFi-Joystick-Windows, MIT, (c) juricabi): the same rules
 * (R1-R14) and log lines. What differs, because a C core has no threads or clock of its own: IPs are
 * numbers (network byte order) and times are passed in; and the activate hook may answer WIFI_ACT_BUSY (a
 * POST to that module in flight, the queue full, not listening), so a restart can wait for the host
 * (restart_ip: it goes out when that module's POST is done, from activation_done or a tick), where
 * JoystickEngine.cs queues it behind the module's POST. A plain joystick_begin to a module whose POST is in
 * flight is not sent (BUSY: tried again at the next chance), and the module's activation entry keeps the
 * request of the one in flight, where JoystickEngine.cs queues it as well and records its request. So when the
 * module IP is set again with another request while its first joystick_begin is in flight (R8), a module that
 * locks streaming the first request is restarted here (R3) but not there. */
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

static void set_remove(uint32_t *t, int *n, uint32_t ip) {
  int i = ip_index(t, *n, ip);
  if (i >= 0) t[i] = t[--*n];
}

/* Entries are ordered by when they were written, by a counter: one drain of the socket gives many datagrams
 * the same time, and with times a full table would then drop the entry it wrote last. */
static uint64_t next_seq(wifi_proto_t *p) { return ++p->tab_seq; }

/* Never dropped from the kind and asked tables: the locked module's and the module IP's entries (the kind
 * decides whether a joystick_end may go out, the asked request whether a restart is due). As
 * JoystickEngine.cs's IpTable with keep1/keep2. */
static bool kept(const wifi_proto_t *p, uint32_t ip) {
  return (p->bound && ip == p->bound_ip) || (p->target && ip == p->target);
}

static int act_find(const wifi_proto_t *p, uint32_t ip) {
  for (int i = 0; i < p->nact; i++) if (p->act[i].ip == ip) return i;
  return -1;
}

/* Full table: the entry written longest ago goes (an expired one, when there is one). Evicting a fresh entry
 * lets that IP be POSTed early; only more than WIFI_TABLE modules beaconing at once do that (then each
 * beacon round POSTs them all again). */
static void act_record(wifi_proto_t *p, uint32_t ip, int64_t now) {
  int i = act_find(p, ip);
  if (i < 0) {
    if (p->nact < WIFI_TABLE) i = p->nact++;
    else {
      i = 0;
      for (int k = 1; k < p->nact; k++) if (p->act[k].seq < p->act[i].seq) i = k;
    }
  }
  p->act[i].ip = ip;
  p->act[i].t_us = now;
  p->act[i].interval_us = p->interval_us;      /* what send_act just asked for */
  p->act[i].channels = p->channels;
  p->act[i].seq = next_seq(p);
  p->act[i].unthrottled = false;
  p->act[i].locked = false;                    /* (R14) no frame of it since this one */
}

/* The next activation POST to ip goes out at once (the module IP set, an explicit restart). The entry stays: a
 * host busy with a POST to ip (in flight) sends nothing new, and R14 still has to know that one went out. */
static void act_unthrottle(wifi_proto_t *p, uint32_t ip) {
  int i = act_find(p, ip);
  if (i >= 0) p->act[i].unthrottled = true;
}

/* Another module takes the lock: the one that had it keeps, in its act entry, whether a frame of it was applied
 * after its last activation (R14), which bound_ip and bound_last_us no longer tell. */
static void act_note_locked(wifi_proto_t *p) {
  int i = p->bound_ip ? act_find(p, p->bound_ip) : -1;
  if (i >= 0 && p->bound_last_us >= p->act[i].t_us) p->act[i].locked = true;
}

/* src takes the lock: noted for this session (R14), the one locked last at the end. Full: the one locked longest
 * ago goes. */
static void held_note(wifi_proto_t *p, uint32_t ip) {
  int i = ip_index(p->held, p->nheld, ip);
  if (i < 0 && p->nheld == WIFI_TABLE) i = 0;
  if (i >= 0) {
    memmove(p->held + i, p->held + i + 1, sizeof p->held[0] * (size_t)(p->nheld - i - 1));
    p->nheld--;
  }
  p->held[p->nheld++] = ip;
}

/* a was heard before b: by the time of its last frame, then by when it was written (one drain, one time) */
static bool dropped_before(const wifi_dropped_t *a, const wifi_dropped_t *b) {
  return a->t_us < b->t_us || (a->t_us == b->t_us && a->seq < b->seq);
}

/* ip streams to us and we drop its frames; its last frame came at t (R14). Never back in time. Full: the one heard
 * longest ago goes. */
static void dropped_put(wifi_proto_t *p, uint32_t ip, int64_t t) {
  int i = 0;
  while (i < p->ndropped && p->dropped[i].ip != ip) i++;
  if (i == p->ndropped) {
    if (p->ndropped < WIFI_TABLE) p->ndropped++;
    else {
      i = 0;
      for (int k = 1; k < p->ndropped; k++) if (dropped_before(&p->dropped[k], &p->dropped[i])) i = k;
    }
  } else if (p->dropped[i].t_us > t) {
    return;
  }
  p->dropped[i].ip = ip;
  p->dropped[i].t_us = t;
  p->dropped[i].seq = next_seq(p);
}

/* src streams to us and we drop its frame (a 2nd source, a placeholder frame, not the module IP): when, for R14.
 * With a module IP set, other modules' beacons are not heard: one of unknown kind that we neither activated nor
 * had locked this session is left out then, as it could be a Crossfire we can't tell. (One we had locked: while it
 * had the lock, a stop would have sent it joystick_end anyway.) */
static void dropped_note(wifi_proto_t *p, uint32_t src, int64_t now) {
  if (p->target && src != p->target && act_find(p, src) < 0 && ip_index(p->held, p->nheld, src) < 0 &&
      wifi_proto_kind(p, src) == WIFI_KIND_UNKNOWN)
    return;
  dropped_put(p, src, now);
}

static void note_beacon(wifi_proto_t *p, uint32_t ip, wifi_kind_t kind, const char *name) {
  int i = 0;
  while (i < p->nseen && p->seen[i].ip != ip) i++;
  if (i == p->nseen) {
    if (p->nseen < WIFI_TABLE) p->nseen++;
    else {
      /* the least recently heard, but never the locked module or the module IP: other modules beaconing must
       * not make a Crossfire unknown. (With a module IP set, only its beacons are heard.) */
      i = -1;
      for (int k = 0; k < p->nseen; k++) {
        if (kept(p, p->seen[k].ip)) continue;
        if (i < 0 || p->seen[k].seq < p->seen[i].seq) i = k;
      }
      if (i < 0) i = 0;                /* (WIFI_TABLE > 2: never) */
    }
  }
  p->seen[i].ip = ip;
  p->seen[i].kind = kind;
  snprintf(p->seen[i].name, sizeof p->seen[i].name, "%s", name);
  p->seen[i].seq = next_seq(p);
}

wifi_kind_t wifi_proto_kind(const wifi_proto_t *p, uint32_t ip) {
  for (int i = 0; i < p->nseen; i++) if (p->seen[i].ip == ip) return p->seen[i].kind;
  return WIFI_KIND_UNKNOWN;
}

const char *wifi_proto_module_name(const wifi_proto_t *p, uint32_t ip) {
  for (int i = 0; i < p->nseen; i++) if (p->seen[i].ip == ip) return p->seen[i].name;
  return "";
}

static int asked_find(const wifi_proto_t *p, uint32_t ip) {
  for (int i = 0; i < p->nasked; i++) if (p->asked[i].ip == ip) return i;
  return -1;
}

/* ip streams interval_us/channels from now on as far as we know. The entry written longest ago makes room,
 * never the locked module's or the module IP's. */
static void asked_set(wifi_proto_t *p, uint32_t ip, int interval_us, int channels) {
  int i = asked_find(p, ip);
  if (i < 0) {
    if (p->nasked < WIFI_TABLE) i = p->nasked++;
    else {
      i = -1;
      for (int k = 0; k < p->nasked; k++) {
        if (kept(p, p->asked[k].ip)) continue;
        if (i < 0 || p->asked[k].seq < p->asked[i].seq) i = k;
      }
      if (i < 0) i = 0;                /* (WIFI_TABLE > 2: never) */
    }
  }
  p->asked[i].ip = ip;
  p->asked[i].interval_us = interval_us;
  p->asked[i].channels = channels;
  p->asked[i].seq = next_seq(p);
}

/* ip took a joystick_begin with the request (a restart, or an activation after its ELRS beacon) */
static void asked_note(wifi_proto_t *p, uint32_t ip) { asked_set(p, ip, p->interval_us, p->channels); }

/* What ip streams is not known, but likely not the request: a restart of it did not take, and it can no
 * longer be sent again (R6 sends it again only while its module stays locked). Its next lock restarts it
 * (R3, under R2's kind rules), and a channel count other than the request restarts it again (R5). Only
 * while it is not locked (a lock loss, another module, a reset), so never a restart per frame: giving up
 * while it streams on stays given up. why: WIFI_ASKED_CUT or WIFI_ASKED_FAILED. */
static void asked_unknown(wifi_proto_t *p, uint32_t ip, int why) {
  asked_set(p, ip, 0, why);
  set_remove(p->restarted, &p->nrestarted, ip);
}

/* ip took a joystick_end alone (R14): it is idle, and streams what its next joystick_begin asks for. Nothing is
 * known of it then: its next lock takes the request of the last activation sent to it this session, or the
 * request then (as a module never asked), so it is not restarted for a request it no longer streams. */
static void asked_forget(wifi_proto_t *p, uint32_t ip) {
  int i = asked_find(p, ip);
  if (i >= 0) p->asked[i] = p->asked[--p->nasked];
}

/* The lock goes (lock loss, a handover, another module IP, reset). A failed restart of that module, given
 * up or still to be sent again, can't be sent any more: it did not take (asked_unknown). */
static void lock_release(wifi_proto_t *p) {
  if (p->restart_bad_ip) asked_unknown(p, p->restart_bad_ip, WIFI_ASKED_FAILED);
  p->restart_bad_ip = 0;
  p->bound = false;
}

/* ---- engine ---- */
void wifi_proto_init(wifi_proto_t *p, const wifi_hooks_t *h) {
  memset(p, 0, sizeof *p);
  if (h) p->h = *h;
  p->state = WIFI_ST_STOPPED;
  p->last_stats_us = INT64_MIN;
  p->interval_us = WIFI_INTERVAL_DEFAULT;
  p->channels = WIFI_CHANNELS_DEFAULT;
}

void wifi_proto_reset(wifi_proto_t *p) {
  /* a restart whose result is still awaited: the host drops it with the session (wifi_actq_new_session), or
   * aborts it in flight. The module may still stream its old request, not what asked says. (When it did
   * take after all, its next lock restarts it once more.) As JoystickEngine.cs's RestartAbandoned. */
  if (p->restart_res_ip) asked_unknown(p, p->restart_res_ip, WIFI_ASKED_CUT);
  lock_release(p);                     /* and one that failed */
  p->bound_ip = 0;
  p->bound_last_us = 0;
  p->nwarned = p->nnostick = 0;
  p->nostick_ip = 0;
  p->nact = 0;
  p->ndropped = 0;
  p->nheld = 0;
  p->first_act_set = false;
  p->first_act_us = 0;
  p->first_act_ip = 0;
  p->hint_shown = false;
  /* seen and asked stay: a module keeps its kind while it keeps its IP, and streams on after a stop (an
   * ELRS module never beacons while it streams, so a stop and start must not make it unknown) */
  p->frames = p->narr = p->ngaps = 0;
  p->arr_first = p->arr_prev = 0;
  p->gap_mean = p->gap_m2 = 0;
  p->last_hz = p->last_jitter = 0;
  p->last_frames = 0;
  p->nch = 0;
  p->last_src = 0;
  p->restart_ip = 0;
  p->restart_retry = false;
  p->restart_at_us = 0;
  p->restart_res_ip = 0;
  p->restart_tries = 0;
  p->nrestarted = 0;
  p->xf_noted = false;
  p->restart_wait = false;
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

static int send_act(wifi_proto_t *p, uint32_t ip, bool restart) {
  wifi_act_t a = { .ip = ip, .restart = restart, .interval_us = p->interval_us, .channels = p->channels,
                   .release = false };
  return p->h.activate ? p->h.activate(p->h.ud, &a) : WIFI_ACT_FAILED;
}

/* EnsureStreaming; true when an activation went out (not throttled, the host not busy) */
static bool ensure(wifi_proto_t *p, uint32_t ip, int64_t now) {
  int i = act_find(p, ip);
  if (i >= 0 && !p->act[i].unthrottled && now - p->act[i].t_us < WIFI_THROTTLE_US) return false;
  int r = send_act(p, ip, false);
  if (r == WIFI_ACT_BUSY) return false;    /* nothing went out: try again on the next chance */
  act_record(p, ip, now);              /* failures are throttled too */
  if (r == WIFI_ACT_OK) mark_activated(p, ip, now);
  return true;
}

void wifi_proto_ensure(wifi_proto_t *p, uint32_t ip, int64_t now) { ensure(p, ip, now); }

/* A restart still to send (the host was busy, or a failed one to send again) is not wanted any more. */
static void restart_pending_drop(wifi_proto_t *p) {
  p->restart_ip = 0;
  p->restart_retry = false;
}

/* A restart that failed (its joystick_end or joystick_begin got no 2xx): a module still streaming to us kept
 * its old request, so it is sent again WIFI_THROTTLE_US after the last send (tick), while that module stays
 * locked and is not known as Crossfire, WIFI_RESTART_TRIES sends in all (R6). One that stopped streaming to
 * us takes the request from its next joystick_begin (its ELRS beacon gets it one). */
static void restart_failed(wifi_proto_t *p, uint32_t ip, int64_t now) {
  if (wifi_proto_kind(p, ip) == WIFI_KIND_XF) return;        /* (a beacon said so since: it ignores the request) */
  if (!p->bound || p->bound_ip != ip) {
    /* the lock went before the result came: nothing is sent again, and what it streams is not known */
    asked_unknown(p, ip, WIFI_ASKED_FAILED);
    return;
  }
  p->restart_bad_ip = ip;                                    /* until a restart of it is sent again: lock_release */
  if (p->restart_ip == ip && !p->restart_retry) return;      /* a newer restart of it waits to go out already */
  char s[16];
  wifi_ip_str(ip, s);
  if (p->restart_tries >= WIFI_RESTART_TRIES) {
    plog(p, "The restart of %s failed %d times - giving up: it may still stream its old rate and channel count",
         s, p->restart_tries);
    return;
  }
  p->restart_ip = ip;
  p->restart_retry = true;
  p->restart_at_us = p->restart_sent_us + WIFI_THROTTLE_US;
  int64_t in = p->restart_at_us - now;
  char when[24] = "now";
  if (in > 0) snprintf(when, sizeof when, "in %d s", (int)((in + 999999) / 1000000));
  plog(p, "The restart of %s failed - sending it again %s", s, when);
}

/* joystick_end + joystick_begin to ip, past the throttle (and it restarts the throttle). */
static void send_restart(wifi_proto_t *p, uint32_t ip, int64_t now) {
  int r = send_act(p, ip, true);
  if (r == WIFI_ACT_BUSY) { p->restart_ip = ip; return; }    /* sent later (a retry stays one): restart_pending_send */
  p->restart_ip = 0;
  p->restart_retry = false;
  p->restart_res_ip = 0;
  if (p->restart_bad_ip == ip) p->restart_bad_ip = 0;         /* this one's result decides now */
  p->restarts++;
  p->restart_tries++;
  p->restart_sent_us = now;
  act_record(p, ip, now);
  asked_note(p, ip);
  if (r == WIFI_ACT_OK) mark_activated(p, ip, now);
  else if (r == WIFI_ACT_QUEUED) p->restart_res_ip = ip;     /* its result: wifi_proto_activation_done */
  else restart_failed(p, ip, now);
}

/* A new restart (a request change, an explicit one, a mismatch), not a retry: WIFI_RESTART_TRIES sends. */
static void start_restart(wifi_proto_t *p, uint32_t ip, int64_t now) {
  p->restart_tries = 0;
  p->restart_retry = false;
  send_restart(p, ip, now);
}

static void note_xf_ignores(wifi_proto_t *p, uint32_t ip) {
  if (p->xf_noted) return;
  char s[16];
  wifi_ip_str(ip, s);
  plog(p, "Crossfire module %s ignores the rate and channel count: it always sends %d channels at about 90 Hz",
       s, WIFI_XF_CHANNELS);
  p->xf_noted = true;
}

/* Locked, no beacon from it this session yet, 16 channels: it may be Crossfire, which is never sent a
 * joystick_end (e.g. a Crossfire module still streaming when the host started, relocked by its frames).
 * Crossfire beacons about every 8 s also while streaming, ELRS never while streaming, so after
 * WIFI_XF_BEACON_US of lock without one it is ELRS. */
static bool kind_unsure(const wifi_proto_t *p, int64_t now) {
  return p->bound && wifi_proto_kind(p, p->bound_ip) == WIFI_KIND_UNKNOWN && p->nch == WIFI_XF_CHANNELS &&
         now - p->bound_since_us < WIFI_XF_BEACON_US;
}

/* The locked module ip may be Crossfire (kind_unsure): its restart is held until it is known (decided on tick). */
static void restart_hold(wifi_proto_t *p, uint32_t ip, int64_t now) {
  char s[16];
  wifi_ip_str(ip, s);
  if (!p->restart_wait || p->restart_wait_ip != ip)
    plog(p, "%s may be Crossfire (no beacon from it yet), which is never sent joystick_end: restarting its stream "
            "within %d s unless its beacon says Crossfire", s,
         (int)((p->bound_since_us + WIFI_XF_BEACON_US - now + 999999) / 1000000));
  p->restart_wait = true;
  p->restart_wait_ip = ip;
}

/* ip (the locked module, or the target) takes the request now: see wifi_proto_restart. why: the log line of
 * a restart (NULL: the explicit one's). */
static bool restart_module(wifi_proto_t *p, uint32_t ip, int64_t now, const char *why) {
  char s[16], hz[32];
  wifi_ip_str(ip, s);
  wifi_describe_interval(p->interval_us, hz, sizeof hz);
  wifi_kind_t k = wifi_proto_kind(p, ip);
  bool locked = p->bound && ip == p->bound_ip;
  if (locked && k == WIFI_KIND_XF) {
    plog(p, "Crossfire module %s always sends %d channels at its own rate (about 90 Hz) - nothing to restart",
         s, WIFI_XF_CHANNELS);
    return false;
  }
  if (locked && kind_unsure(p, now)) {
    restart_hold(p, ip, now);
    return true;
  }
  if (!locked && k != WIFI_KIND_ELRS) {
    /* the module IP, not streaming to us: a joystick_begin starts it, and a module that is not streaming takes
     * the request from it alone. It may be (or is) Crossfire, which is never sent joystick_end: a Crossfire
     * that has not beaconed yet, or whose beacons do not get here. */
    if (k == WIFI_KIND_XF)
      plog(p, "Crossfire module %s is not streaming to us: activating it (it always sends %d channels at about 90 Hz)",
           s, WIFI_XF_CHANNELS);
    else
      plog(p, "%s has sent no beacon yet (it may be Crossfire, which is never sent joystick_end): activating it with "
              "%s, %d channels", s, hz, p->channels);
    act_unthrottle(p, ip);
    ensure(p, ip, now);
    return true;
  }
  if (why) plog(p, "%s", why);
  else plog(p, "Restarting the stream of %s: %s, %d channels", s, hz, p->channels);
  /* a restart held for it (its window passed a moment ago, no tick resolved it yet) is this one: not a second */
  if (p->restart_wait && p->restart_wait_ip == ip) p->restart_wait = false;
  /* frames of the old request keep coming until the module has the restart: no mismatch restart then */
  set_add(p->restarted, &p->nrestarted, ip);
  start_restart(p, ip, now);
  return true;
}

bool wifi_proto_restart(wifi_proto_t *p, int64_t now) {
  uint32_t ip = p->bound ? p->bound_ip : p->target;
  if (!ip) {
    plog(p, "No module to restart - none is streaming and no module IP is set");
    return false;
  }
  return restart_module(p, ip, now, NULL);
}

bool wifi_proto_set_request(wifi_proto_t *p, int interval_us, int channels, int64_t now) {
  interval_us = wifi_clamp_interval(interval_us);
  channels = wifi_clamp_channels(channels);
  if (interval_us == p->interval_us && channels == p->channels) return false;
  p->interval_us = interval_us;
  p->channels = channels;
  if (!p->bound) return true;          /* the next joystick_begin asks for them, or the lock restarts it (asked) */
  if (wifi_proto_kind(p, p->bound_ip) == WIFI_KIND_XF) {
    note_xf_ignores(p, p->bound_ip);
    return true;
  }
  wifi_proto_restart(p, now);          /* may wait for the kind (kind_unsure) */
  return true;
}

void wifi_proto_store_request(wifi_proto_t *p, int interval_us, int channels) {
  p->interval_us = wifi_clamp_interval(interval_us);
  p->channels = wifi_clamp_channels(channels);
}

/* A restart the host was too busy for (not a retry): it goes out while its module is the locked one (held
 * first when it may be Crossfire, e.g. after a relock: R2), or, not locked, when it is the module IP known as
 * ELRS (R4). Any other module that is not locked gets it when it locks again: one that streams on through a
 * lock loss keeps its old request until it is restarted, and one that is not known as ELRS is never sent a
 * joystick_end while it is not locked (it may be Crossfire). Dropped once a beacon says Crossfire. */
static void restart_pending_send(wifi_proto_t *p, int64_t now) {
  uint32_t ip = p->restart_ip;
  if (!ip || p->restart_retry) return;
  wifi_kind_t k = wifi_proto_kind(p, ip);
  if (k == WIFI_KIND_XF) { restart_pending_drop(p); return; }
  if (p->bound) {
    if (p->bound_ip != ip) return;
    if (kind_unsure(p, now)) {
      restart_pending_drop(p);
      restart_hold(p, ip, now);
      return;
    }
  } else if (p->target != ip || k != WIFI_KIND_ELRS) {
    return;
  }
  send_restart(p, ip, now);
}

void wifi_proto_activation_done(wifi_proto_t *p, uint32_t ip, bool ok, int64_t now) {
  /* one activation per module is under way at a time (wifi_actq): this is the restart's result */
  if (ip && ip == p->restart_res_ip) {
    p->restart_res_ip = 0;
    if (!ok) restart_failed(p, ip, now);
  }
  if (ok) mark_activated(p, ip, now);
  /* a restart that waited for this POST of its module goes out now, not a tick later */
  if (ip && ip == p->restart_ip) restart_pending_send(p, now);
}

/* ---- R14: release ---- */
/* One module: Crossfire (by its beacon) is logged only; anything else gets joystick_end alone through the hook.
 * *n counts what the hook did not refuse. */
static void release_one(wifi_proto_t *p, uint32_t ip, int *n) {
  char s[16];
  wifi_ip_str(ip, s);
  if (wifi_proto_kind(p, ip) == WIFI_KIND_XF) {
    plog(p, "Not releasing %s: Crossfire broadcasts while streaming, other apps find it as it is", s);
    return;
  }
  plog(p, "Releasing %s: joystick_end, so it stops streaming and broadcasts again", s);
  wifi_act_t a = { .ip = ip, .restart = false, .interval_us = 0, .channels = 0, .release = true };
  int r = p->h.activate ? p->h.activate(p->h.ud, &a) : WIFI_ACT_FAILED;
  if (r == WIFI_ACT_BUSY) {
    plog(p, "Releasing %s failed: the request queue is full - it may stream on until its WiFi or power is cycled", s);
    return;
  }
  (*n)++;
  if (r == WIFI_ACT_OK || r == WIFI_ACT_FAILED) wifi_proto_release_done(p, ip, r == WIFI_ACT_OK, 0, NULL);
}

int wifi_proto_release(wifi_proto_t *p, int64_t now) {
  uint32_t ips[1 + 2 * WIFI_TABLE];
  int nips = 0;
  /* The modules known to stream to us first: the host's worker sends the releases in turn (a POST may take
   * WIFI_POST_TIMEOUT_MS) and an exit waits at most WIFI_RELEASE_WAIT_US, so a module that may not be there must
   * not hold them up. The locked module, */
  if (p->bound) ips[nips++] = p->bound_ip;
  /* then each module that streams to us while we drop its frames (one came less than WIFI_SOURCE_TIMEOUT_US
   * ago), the one heard longest ago first: a 2nd source, placeholder frames, the module left by a switch (from
   * its last frame on, applied or not: set_target) */
  const wifi_dropped_t *last = NULL;
  for (;;) {
    const wifi_dropped_t *k = NULL;
    for (int i = 0; i < p->ndropped; i++) {
      const wifi_dropped_t *d = &p->dropped[i];
      if ((last && !dropped_before(last, d)) || now - d->t_us >= WIFI_SOURCE_TIMEOUT_US) continue;
      if (!k || dropped_before(d, k)) k = d;
    }
    if (!k) break;
    last = k;
    if (ip_index(ips, nips, k->ip) < 0) ips[nips++] = k->ip;
  }
  /* then each module activated less than WIFI_SOURCE_TIMEOUT_US ago that has not locked since (it may be about
   * to stream to us), in the order they were activated. "Locked since": a frame of it was applied after its
   * activation. The last locked module tells by bound_ip and bound_last_us (lock_release keeps them), one that
   * had the lock before it by its entry's locked (act_note_locked): within that time only a switch to another
   * module IP takes a lock away, and the module left gets nothing by this rule (R7), also when the new one has
   * locked since. (The rule above releases it while its last frame is that recent: the switch put it in the
   * dropped table.) */
  uint64_t after = 0;
  for (;;) {
    int k = -1;
    for (int i = 0; i < p->nact; i++) {
      const wifi_ipt_t *a = &p->act[i];
      if (a->seq <= after || now - a->t_us >= WIFI_SOURCE_TIMEOUT_US) continue;
      if (a->locked || (a->ip == p->bound_ip && p->bound_last_us >= a->t_us)) continue;
      if (k < 0 || a->seq < p->act[k].seq) k = i;
    }
    if (k < 0) break;
    after = p->act[k].seq;
    if (ip_index(ips, nips, p->act[k].ip) < 0) ips[nips++] = p->act[k].ip;
  }
  int n = 0;
  for (int i = 0; i < nips; i++) release_one(p, ips[i], &n);
  return n;
}

void wifi_proto_release_done(wifi_proto_t *p, uint32_t ip, bool ok, int status, const char *err) {
  char s[16];
  wifi_ip_str(ip, s);
  if (ok) {
    /* idle now: what it was last asked no longer tells what it streams (R3). The worker sends in order, so all
     * that went to it after the stop went after this joystick_end: its act entry tells what it streams now. */
    asked_forget(p, ip);
    plog(p, "Released %s: it broadcasts again, any app can connect to it", s);
    return;
  }
  char why[80];
  if (status > 0) snprintf(why, sizeof why, "HTTP %d", status);
  else snprintf(why, sizeof why, "%s", err && err[0] ? err : "no answer");
  plog(p, "Releasing %s failed: %s - it may stream on until its WiFi or power is cycled", s, why);
}

static const char NOSTICK_STATE[] = "module found, but it is sending no stick data";

/* placeholder frames from src while nothing is locked: warned once per source; the state says so again
 * when they come back after the state went back to waiting (tick) */
static void warn_no_stick(wifi_proto_t *p, uint32_t src, int64_t now) {
  p->nostick_last_us = now;
  if (!set_add(p->nostick, &p->nnostick, src)) {
    if (p->state == WIFI_ST_SEARCHING && strcmp(p->detail, NOSTICK_STATE) && ip_index(p->nostick, p->nnostick, src) >= 0) {
      p->nostick_ip = src;
      wifi_proto_set_state(p, WIFI_ST_SEARCHING, NOSTICK_STATE);
    }
    return;
  }
  char ip[16];
  wifi_ip_str(src, ip);
  p->nostick_ip = src;
  plog(p, "!! %s is streaming placeholder frames - no stick data. Either the radio is not feeding the module "
          "right now, or the firmware has the known regression (XF 6.42/6.48, WiFi 3.20 - use XF 6.31 or 6.36, "
          "WiFi up to 3.10).", ip);
  wifi_proto_set_state(p, WIFI_ST_SEARCHING, NOSTICK_STATE);
}

void wifi_proto_tick(wifi_proto_t *p, int64_t now) {
  if (p->bound && now - p->bound_last_us > WIFI_SOURCE_TIMEOUT_US) {
    char ip[16];
    wifi_ip_str(p->bound_ip, ip);
    plog(p, "Source %s went quiet - releasing lock", ip);
    lock_release(p);
    wifi_proto_set_state(p, WIFI_ST_SEARCHING, "waiting for module");
  }
  /* the module that sent placeholder frames went away: no more "sending no stick data" */
  if (!p->bound && p->state == WIFI_ST_SEARCHING && !strcmp(p->detail, NOSTICK_STATE) &&
      now - p->nostick_last_us > WIFI_SOURCE_TIMEOUT_US)
    wifi_proto_set_state(p, WIFI_ST_SEARCHING, "waiting for module");
  /* placeholder frames have their own explanation: the network is demonstrably fine then */
  if (p->first_act_set && !p->bound && !p->hint_shown && p->nnostick == 0 &&
      now - p->first_act_us > WIFI_FW_HINT_US) {
    p->hint_shown = true;
    plog(p, "%s", HINT_TEXT);
    wifi_proto_set_state(p, WIFI_ST_SEARCHING, "activated, but no data (radio off? another client?)");
  }
  /* a restart the host was too busy for: restart_pending_send. A failed one: sent again from restart_at_us
   * while its module stays locked (R6; its lock loss left it asked "unknown": lock_release). Never once a
   * beacon said Crossfire. */
  if (p->restart_ip) {
    uint32_t ip = p->restart_ip;
    if (wifi_proto_kind(p, ip) == WIFI_KIND_XF) restart_pending_drop(p);
    else if (p->restart_retry) {
      if (!p->bound || p->bound_ip != ip) restart_pending_drop(p);
      else if (now >= p->restart_at_us) send_restart(p, ip, now);
    } else restart_pending_send(p, now);
  }
  /* a restart held to know the module is no Crossfire: decided while it is locked (through a lock loss it
   * waits for the next lock, for the same reason) */
  if (p->restart_wait) {
    if (wifi_proto_kind(p, p->restart_wait_ip) == WIFI_KIND_XF) {
      p->restart_wait = false;
      note_xf_ignores(p, p->restart_wait_ip);
    } else if (p->bound && p->bound_ip == p->restart_wait_ip && !kind_unsure(p, now)) {
      p->restart_wait = false;
      wifi_proto_restart(p, now);
    }
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
    note_beacon(p, src, xf ? WIFI_KIND_XF : WIFI_KIND_ELRS, name);
    bool locked_other = p->bound && p->bound_ip != src;
    bool streaming = p->bound && p->bound_ip == src && now - p->bound_last_us < WIFI_SOURCE_TIMEOUT_US;
    /* said only when the POST goes out: beacons within the throttle (a flood, a loop) log nothing */
    if (!locked_other && !streaming && ensure(p, src, now)) {
      char ip[16];
      wifi_ip_str(src, ip);
      plog(p, "Discovery beacon from %s module %s - activating", xf ? "Crossfire" : "ELRS", ip);
      /* ELRS beacons only while it is not streaming: it streams what this joystick_begin asks for, so a
       * restart held or pending for it has nothing left to do */
      if (!xf) {
        asked_note(p, src);
        if (p->restart_wait && p->restart_wait_ip == src) p->restart_wait = false;
        if (p->restart_ip == src) restart_pending_drop(p);
        if (p->restart_bad_ip == src) p->restart_bad_ip = 0;
      }
    }
    return;     /* never parsed as channels, whatever follows */
  }

  int count = d[1];
  if (count < WIFI_MIN_CH || count > WIFI_MAX_CH || n < 2 + 2 * (size_t)count) { p->malformed++; return; }
  if (p->target && src != p->target) {
    dropped_note(p, src, now);         /* (e.g. the module left by a switch) */
    p->dropped_other++;
    return;
  }

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
    if (!p->bound || p->bound_ip != src) dropped_note(p, src, now);   /* (the locked module is released anyway) */
    if (!p->bound) warn_no_stick(p, src, now);
    return;
  }

  if (p->bound && p->bound_ip != src) {
    if (now - p->bound_last_us > WIFI_SOURCE_TIMEOUT_US) lock_release(p);
    else {
      if (set_add(p->warned, &p->nwarned, src)) {
        char a[16], b[16];
        wifi_ip_str(src, a);
        wifi_ip_str(p->bound_ip, b);
        plog(p, "Ignoring 2nd source %s (locked to %s)", a, b);
      }
      dropped_note(p, src, now);
      p->dropped_other++;
      return;
    }
  }
  if (!p->bound) {
    char ip[16];
    wifi_ip_str(src, ip);
    if (p->bound_ip != src) act_note_locked(p);
    p->bound = true;
    p->bound_ip = src;
    p->nwarned = p->nnostick = 0;
    p->first_act_set = false;
    p->hint_shown = false;
    p->bound_since_us = now;
    held_note(p, src);
    /* a restart pending or held for another module is not wanted now; one for this module goes on */
    if (p->restart_wait && p->restart_wait_ip != src) p->restart_wait = false;
    if (p->restart_ip != src) restart_pending_drop(p);
    /* Not known what it streams: what the last activation sent to it this session asked for (it was not
     * streaming then, or it streams what it streamed before, which nothing can tell), else the request now
     * (found streaming). Either way a change of the request made while it is not locked from here on - a
     * WiFi pause, a stop - restarts it when it locks again: it streams on through those, keeping the old one. */
    if (asked_find(p, src) < 0) {
      int k = act_find(p, src);
      if (k >= 0) asked_set(p, src, p->act[k].interval_us, p->act[k].channels);
      else asked_note(p, src);
    }
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

  /* A module still streaming another request (another app's, or ours from before a change) keeps it
   * until it is restarted (R5). Crossfire ignores the request: restarted at once where it surely is none
   * (ELRS by its beacon, or a count Crossfire never sends); an unknown one with 16 channels once
   * WIFI_XF_BEACON_US of lock passed without a Crossfire beacon (held until then: restart_module). Once per
   * module and session, and never when the table is full: no restart per frame. */
  if (count != p->channels && p->restart_ip != src && !p->restart_wait &&
      ip_index(p->restarted, p->nrestarted, src) < 0 && p->nrestarted < WIFI_TABLE &&
      wifi_proto_kind(p, src) != WIFI_KIND_XF) {
    char why[160], ip[16];
    wifi_ip_str(src, ip);
    snprintf(why, sizeof why, "%s streams %d channels, %d asked for (another app's request?) - restarting its stream",
             ip, count, p->channels);
    restart_module(p, src, now, why);
  }
  /* A module last asked for another request than the one now (it changed while the module was not locked:
   * during the activation, a WiFi pause, a stop, a switch to it from another module), or left with an
   * unknown one by a restart that did not take, streams that one until it is restarted (R3), under R2's kind
   * rules: Crossfire is only noted; may wait for the kind (kind_unsure). Once per change: the restart records
   * the new request. (Every frame: also a locked module after wifi_proto_store_request.) */
  int a = asked_find(p, src);
  if (a >= 0 && (p->asked[a].interval_us != p->interval_us || p->asked[a].channels != p->channels) &&
      p->restart_ip != src && !p->restart_wait) {
    if (wifi_proto_kind(p, src) == WIFI_KIND_XF) note_xf_ignores(p, src);
    else {
      char why[240], ip[16], want[32], had[32];
      wifi_ip_str(src, ip);
      wifi_describe_interval(p->interval_us, want, sizeof want);
      if (p->asked[a].interval_us == 0)
        snprintf(why, sizeof why, "%s may still stream its old request (%s) - restarting its stream with %s, %d "
                 "channels", ip, p->asked[a].channels == WIFI_ASKED_CUT ? "Stop cut its restart short"
                 : "its restart failed", want, p->channels);
      else
        snprintf(why, sizeof why, "%s still streams %s, %d channels from before the change - restarting its stream "
                 "with %s, %d channels", ip, wifi_describe_interval(p->asked[a].interval_us, had, sizeof had),
                 p->asked[a].channels, want, p->channels);
      restart_module(p, src, now, why);
    }
  }
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
  /* a restart pending or held for another module is not wanted any more (the module being left) */
  if (p->restart_ip != ip) restart_pending_drop(p);
  if (p->restart_wait && p->restart_wait_ip != ip) p->restart_wait = false;
  act_unthrottle(p, ip);      /* allow an immediate POST */
  if (p->bound && p->bound_ip == ip && now - p->bound_last_us <= WIFI_SOURCE_TIMEOUT_US) {
    /* the module streaming to us now: unlocking it would center the joystick for a frame (and drop a held
     * restart) for nothing, and a joystick_begin would not change its stream */
    plog(p, "Target module set to %s - streaming from it already", s);
    return 0;
  }
  if (p->bound) {                      /* let the new target take over */
    /* The module left gets nothing now (R7) and streams on to us: its frames are dropped from here on, so for R14
     * it is one that streams to us while we drop its frames, from its last frame on. A stop before its next
     * frame comes, or after the new module IP locked, releases it too. */
    if (p->bound_ip != ip) dropped_put(p, p->bound_ip, p->bound_last_us);
    lock_release(p);
  }
  plog(p, "Target module set to %s - connecting...", s);
  snprintf(detail, sizeof detail, "connecting to %s", s);
  wifi_proto_set_state(p, WIFI_ST_SEARCHING, detail);
  wifi_proto_ensure(p, ip, now);
  return 0;
}

/* ---- pure helpers ---- */
int wifi_clamp_interval(int us) {
  return us < WIFI_INTERVAL_MIN ? WIFI_INTERVAL_MIN : us > WIFI_INTERVAL_MAX ? WIFI_INTERVAL_MAX : us;
}

int wifi_clamp_channels(int n) {
  return n < WIFI_MIN_CH ? WIFI_MIN_CH : n > WIFI_MAX_CH ? WIFI_MAX_CH : n;
}

int wifi_interval_hz(int us) {
  return us > 0 ? (int)((1000000L + us / 2) / us) : 0;
}

const char *wifi_describe_interval(int us, char *out, size_t cap) {
  static const int presets[] = WIFI_INTERVAL_PRESETS;
  bool preset = false;
  for (int i = 0; i < WIFI_N_INTERVAL_PRESETS; i++) if (presets[i] == us) preset = true;
  if (preset) snprintf(out, cap, "%d Hz", wifi_interval_hz(us));
  else snprintf(out, cap, "%d Hz (%d us)", wifi_interval_hz(us), us);
  return out;
}

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
                   "User-Agent: " WIFI_USER_AGENT "\r\n"
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

/* ---- activation queue ----
 * Releases (R14) are the requests and results of the session WIFI_ACTQ_ANY: never pending, never stale, kept by
 * wifi_actq_new_session, counted by nrelease from push until their result is taken. */
static bool is_release(uint32_t session) { return session == WIFI_ACTQ_ANY; }

void wifi_actq_init(wifi_actq_t *q) {
  memset(q, 0, sizeof *q);
  q->session = 1;
}

void wifi_actq_new_session(wifi_actq_t *q) {
  /* the releases stay, in their order (compacted behind the read position: nothing unread is overwritten) */
  int k = 0;
  for (int i = 0; i < q->rq_n; i++) {
    const wifi_actreq_t *r = &q->req[(q->rq_head + i) % WIFI_ACTQ];
    if (is_release(r->session)) q->req[(q->rq_head + k++) % WIFI_ACTQ] = *r;
  }
  q->rq_n = k;
  k = 0;
  for (int i = 0; i < q->rs_n; i++) {
    const wifi_actres_t *r = &q->res[(q->rs_head + i) % WIFI_ACTQ];
    if (is_release(r->session)) q->res[(q->rs_head + k++) % WIFI_ACTQ] = *r;
  }
  q->rs_n = k;
  q->npending = 0;
  if (++q->session == WIFI_ACTQ_ANY) q->session++;   /* the wrap skips the releases' session */
}

bool wifi_actq_is_pending(const wifi_actq_t *q, uint32_t ip) {
  for (int i = 0; i < q->npending; i++) if (q->pending[i] == ip) return true;
  return false;
}

static void unpend(wifi_actq_t *q, uint32_t ip) {
  for (int i = 0; i < q->npending; i++)
    if (q->pending[i] == ip) { q->pending[i] = q->pending[--q->npending]; return; }
}

int wifi_actq_push(wifi_actq_t *q, uint32_t ip, uint16_t http_port, const char *body, bool end_first) {
  if (strlen(body) >= sizeof q->req[0].body) return WIFI_ACT_BUSY;
  if (wifi_actq_is_pending(q, ip)) {
    if (!end_first) return WIFI_ACT_BUSY;
    /* a restart supersedes a plain request that has not gone out yet (a release queued for the IP stays one) */
    for (int i = 0; i < q->rq_n; i++) {
      wifi_actreq_t *r = &q->req[(q->rq_head + i) % WIFI_ACTQ];
      if (r->ip != ip || r->session != q->session) continue;
      r->http_port = http_port;
      r->end_first = true;
      strcpy(r->body, body);
      return WIFI_ACT_QUEUED;
    }
    return WIFI_ACT_BUSY;             /* in flight: the caller asks again */
  }
  if (q->rq_n >= WIFI_ACTQ || q->npending >= WIFI_ACTQ) return WIFI_ACT_BUSY;
  wifi_actreq_t *r = &q->req[(q->rq_head + q->rq_n) % WIFI_ACTQ];
  r->ip = ip;
  r->http_port = http_port;
  r->session = q->session;
  r->end_first = end_first;
  r->release = false;
  strcpy(r->body, body);
  q->rq_n++;
  q->pending[q->npending++] = ip;
  return WIFI_ACT_QUEUED;
}

int wifi_actq_push_release(wifi_actq_t *q, uint32_t ip, uint16_t http_port) {
  /* the last request queued for ip is a release not taken yet: it goes out after everything else to ip, so it
   * is this one too */
  for (int i = q->rq_n - 1; i >= 0; i--) {
    wifi_actreq_t *r = &q->req[(q->rq_head + i) % WIFI_ACTQ];
    if (r->ip != ip) continue;
    if (!is_release(r->session)) break;
    r->http_port = http_port;
    return WIFI_ACT_QUEUED;
  }
  if (q->rq_n >= WIFI_ACTQ) return WIFI_ACT_BUSY;
  wifi_actreq_t *r = &q->req[(q->rq_head + q->rq_n) % WIFI_ACTQ];
  r->ip = ip;
  r->http_port = http_port;
  r->session = WIFI_ACTQ_ANY;
  r->end_first = false;
  r->release = true;
  r->body[0] = 0;
  q->rq_n++;
  q->nrelease++;
  return WIFI_ACT_QUEUED;
}

int wifi_actq_releases(const wifi_actq_t *q) { return q->nrelease; }

bool wifi_actq_take(wifi_actq_t *q, wifi_actreq_t *out) {
  while (q->rq_n > 0) {
    wifi_actreq_t *r = &q->req[q->rq_head];
    q->rq_head = (q->rq_head + 1) % WIFI_ACTQ;
    q->rq_n--;
    if (r->session != q->session && !is_release(r->session)) continue;
    *out = *r;
    return true;
  }
  return false;
}

bool wifi_actq_stale(const wifi_actq_t *q, uint32_t session) {
  return session != q->session && !is_release(session);
}

void wifi_actq_put(wifi_actq_t *q, const wifi_actres_t *r) {
  if (r->session != q->session && !is_release(r->session)) return;
  if (q->rs_n >= WIFI_ACTQ) {
    /* can't happen while the frame loop takes the results (results <= pending IPs + releases, and it takes up
     * to WIFI_ACTQ per poll); if it did, the oldest goes: its IP unpended so it is asked again, or a release
     * no longer counted (its result is lost) */
    const wifi_actres_t *o = &q->res[q->rs_head];
    if (is_release(o->session)) { if (q->nrelease > 0) q->nrelease--; }
    else unpend(q, o->ip);
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
    if (is_release(r->session)) {
      if (q->nrelease > 0) q->nrelease--;
    } else if (r->session != q->session) {
      continue;
    } else {
      unpend(q, r->ip);
    }
    *out = *r;
    return true;
  }
  return false;
}
