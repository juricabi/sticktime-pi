/* ELRS / Crossfire WiFi joystick receiver: the UDP socket, the activation worker thread, and the
 * glue to the input sources (insrc_t) and the status page (app_status_t). The protocol itself is the
 * core shared with the owner's Linux app, vendored in third_party/elrs-wifi: wifi_proto.c (a port of
 * JoystickEngine.cs from ELRS-Crossfire-WiFi-Joystick-Windows) and wifi_net.c (the activation POST).
 *
 * Threads: everything here runs on the frame loop except the HTTP POST /udpcontrol, which one
 * persistent worker thread (started by wifi_open, below the frame loop's priority) sends. The frame
 * loop only drains the socket without blocking and swaps requests/results under a mutex.
 *
 * Sticks: each applied frame fills the source with ch[i] = wifi_raw_to_stick(raw[i]) for the
 * frame's channels and 0 (center) beyond. Gaps hold the last values while the mixer still counts the
 * source as live (INPUT_STALE_US, 0.5 s, like every source); after that, with no other live source, the
 * game gets centered sticks with the throttle low (mix_sticks) and pauses in flight. After 3 s without a
 * valid frame (the lock loss, WIFI_SOURCE_TIMEOUT_US), and on a stop, a close or a switch to another
 * module IP, the lock is released and wifi_poll empties the source (channels 0, not present). */
#ifndef INPUT_WIFI_H
#define INPUT_WIFI_H

#include <stdint.h>
#include <stdbool.h>
#include "app.h"
#include "input.h"
#include "wifi_proto.h"

/* WIFI_INTERVAL_MIN/MAX/DEFAULT, WIFI_CHANNELS_DEFAULT (the request) come from wifi_proto.h */
#define WIFI_GAP_US            250000    /* a pause in the stick data this long is counted and logged */

typedef struct {
  uint16_t port;          /* UDP port, 11000 (the module always sends there; others are for tests) */
  uint16_t http_port;     /* the module's web server, 80 (0 = 80) */
  char module_ip[64];     /* settings wifi_module_ip: dotted IPv4, "" = discover by beacon */
  int interval_us;        /* settings wifi_interval_us, clamped to WIFI_INTERVAL_MIN..MAX */
  int channels;           /* settings wifi_channels, clamped to WIFI_MIN_CH..WIFI_MAX_CH (Crossfire sends 16) */
} wifi_config_t;

void wifi_config_default(wifi_config_t *c);     /* 11000, 80, "", 4000 (250 Hz), 16 */

typedef struct wifi wifi_t;

/* Allocates the receiver and starts its worker thread. Does not bind: call wifi_start.
 * cfg NULL = defaults. NULL only when out of memory. Every function accepts w == NULL. */
wifi_t *wifi_open(app_t *app, const wifi_config_t *cfg);
void    wifi_close(wifi_t *w);                             /* stop, end the worker, free */
/* Reset the session and bind (retried in wifi_poll: 5 tries 300 ms apart, then state Error and a
 * try every 5 s). No-op while running. */
void    wifi_start(wifi_t *w, int64_t now_us);
void    wifi_stop(wifi_t *w);                              /* close the socket, state Stopped; POST results of the
                                                              old session are discarded (never waits) */
/* Frame loop, once per frame (never blocks): bind retry, drain UDP (at most 256 datagrams), collect
 * POST results, tick, stats, update *src (the WiFi slot; may be NULL). */
void    wifi_poll(wifi_t *w, int64_t now_us, insrc_t *src);
/* Live, by the core's request rules (R1-R13 in wifi_proto.h): module_ip -> set target (invalid text, which
 * the settings refuse, 0.0.0.0 too: logged, discovery; the module locked now: the lock kept, nothing sent; another: unlocked, joystick_begin
 * at once, a restart pending or held for the module left dropped); port -> rebind (a new session);
 * http_port -> the next POST. interval and channels (the request) -> the next joystick_begin; the module
 * locked to is restarted so they apply (joystick_end, then joystick_begin: ELRS takes them only when its
 * stream starts): at once when it surely is no Crossfire (an ELRS beacon, or not 16 channels), else (16
 * channels, no beacon from it yet) once the lock is WIFI_XF_BEACON_US old without a Crossfire beacon. Not
 * when it is Crossfire by its beacon (which ignores them) or the new module_ip names another module (its
 * joystick_begin carries them). A restart counts only when both POSTs got a 2xx; a failed one is sent again
 * (the core). */
void    wifi_configure(wifi_t *w, const wifi_config_t *cfg, int64_t now_us);
/* wifi_module ("ELRS 10.0.0.1" while streaming, else ""), wifi_state ("off", "listening",
 * "activating 10.0.0.1", "connecting to 10.0.0.1", "no data from 10.0.0.1",
 * "no stick data from 10.0.0.1", "streaming", "UDP port 11000 in use"), wifi_rate_hz, wifi_dropped */
void    wifi_fill_status(const wifi_t *w, app_status_t *st);
int64_t wifi_last_arrival_us(const wifi_t *w);             /* receive time of the newest applied frame (plat_now_us clock), 0 = none */

/* Everything the web page's sticks view and the tests may want. */
typedef struct {
  bool running;                 /* started (listening, or trying to bind) */
  bool listening;               /* socket bound */
  uint16_t port;
  wifi_state_t state;
  char detail[64];
  bool bound;
  uint32_t source;              /* locked module (network byte order), 0 = none */
  uint32_t target;              /* explicit module, 0 = discovery */
  wifi_kind_t kind;             /* of the locked module, from its beacons */
  char name[64];                /* "ELRS 192.168.1.50" while locked, else "" */
  char module_name[32];         /* ELRS device name from its beacon ("Unified TX") or "" */
  double rate_hz, jitter_ms;    /* last stats window (1 s) */
  int frames_last_s;
  uint32_t rx, beacons, malformed, dropped_filter, dropped_other, applied;
  uint32_t posts, posts_ok, posts_failed;   /* activations (a restart is one: joystick_end + joystick_begin, ok
                                               only when both got a 2xx) */
  int pending;                  /* activation requests queued or in flight */
  int interval_us, channels;    /* the request: what joystick_begin asks for */
  uint32_t restarts;            /* restarts sent (a request change, or a module streaming another channel count) */
  uint8_t nch;
  uint16_t raw[WIFI_MAX_CH];    /* last applied frame, raw 0..32767 */
  int64_t last_arrival_us;
  uint32_t gaps;                /* pauses of WIFI_GAP_US or more between frames of the locked module */
  int64_t gap_max_us;           /* the longest */
} wifi_info_t;

void wifi_get_info(const wifi_t *w, wifi_info_t *out);

#endif
