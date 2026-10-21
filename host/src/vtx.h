/* VTX control: sets the analog VTX's band/channel/frequency, power and pit mode over SmartAudio or
 * Tramp, from the settings (vtx, vtx_port, vtx_band, vtx_channel, vtx_freq, vtx_power, vtx_pit,
 * vtx_send_only) and the VTX table (<base>/vtxtable.json, or the built-in one). See docs/USER_GUIDE.md
 * (wiring, voltages) and docs/RESEARCH.md (protocol facts).
 *
 * Threads: everything runs on the VTX thread (vtx_open), below the frame loop's priority; the frame
 * loop and the web server only call vtx_reload() (re-read the settings and the table) and
 * vtx_get_status(), which never block on the line.
 *
 * The line: one wire, half duplex. By default (vtx_port = gpio4, ...) one GPIO does both directions in
 * software (vtx_soft.c: edge timestamps in, bits on an absolute schedule out), like a flight controller's
 * soft serial. The two-pin mode (vtx_port = uart3, ...) uses a PL011: Linux has no single-wire UART mode,
 * so the Pi's TX and RX both go to the VTX's pad through resistors, and everything sent comes back on RX:
 * the engine compares that echo with what it sent and drops it. SmartAudio wants the line low before a frame; by default the line
 * idles high (the UART's mark) and a 0x00 byte goes before every frame, the spec's way for a host that
 * can't hold the line low (INAV, ExpressLRS and PX4 do the same). VTXes that don't answer that way are
 * tried with the line held low between frames (a break: like Betaflight's pull-down, no 0x00), with no
 * lead-in, with a 0x00 after the frame (AKK/RDQ clones), with 1 stop bit (TBS Sixty9) and with baud
 * rates within the spec's +-5 %. Send-only mode (only the TX wire) reads nothing.
 *
 * Like Betaflight: the settings are what the VTX should be set to. The engine finds the VTX (auto:
 * SmartAudio, then Tramp, then SmartAudio clone variants and baud rates within the spec's +-5 %), reads
 * its state, sends what differs, reads again, and keeps polling; when someone presses the VTX's button
 * the Pi sets it back. A VTX that stops answering is searched for again. */
#ifndef VTX_H
#define VTX_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "app.h"
#include "settings.h"
#include "vtx_proto.h"
#include "vtx_table.h"

typedef enum { VTX_MODE_OFF, VTX_MODE_AUTO, VTX_MODE_SA, VTX_MODE_TRAMP } vtx_mode_t;
typedef enum { VTX_PROTO_NONE, VTX_PROTO_SA, VTX_PROTO_TRAMP } vtx_proto_t;
typedef enum {
  VTX_ST_OFF,            /* vtx = off */
  VTX_ST_NO_PORT,        /* the UART isn't there (its overlay comes at the next boot) or can't be opened */
  VTX_ST_SEARCHING,      /* looking for the VTX */
  VTX_ST_NOT_FOUND,      /* a whole round without an answer (it keeps trying) */
  VTX_ST_SYNCING,        /* found: sending the settings */
  VTX_ST_READY,          /* found, and set like the settings say */
  VTX_ST_SEND_ONLY,      /* commands sent, nothing read back */
  VTX_ST_ERROR,          /* a setting the VTX or the table can't do, or a bad table file */
} vtx_state_t;

const char *vtx_state_name(vtx_state_t s);       /* "off", "no_port", "searching", ... */
const char *vtx_proto_name(vtx_proto_t p);       /* "", "smartaudio", "tramp" */
const char *vtx_mode_name(vtx_mode_t m);         /* "off", "auto", "smartaudio", "tramp" */
vtx_mode_t  vtx_mode_parse(const char *s);

/* what the VTX should be set to */
typedef struct {
  vtx_mode_t mode;
  char port[16];                 /* uart3 | uart4 | uart5 */
  char band;                     /* a table letter, 0 = leave the frequency alone (unless freq) */
  int channel;                   /* 1..8 */
  int freq;                      /* MHz, 0 = by band/channel */
  int power;                     /* 1-based table level, 0 = leave it */
  bool pit;
  bool send_only;
  bool have_table;               /* <base>/vtxtable.json is in use */
  vtx_table_t table;
  char table_error[200];         /* the file is there but can't be used ("" = fine or missing) */
} vtx_config_t;

void vtx_config_defaults(vtx_config_t *c);
void vtx_config_from_settings(vtx_config_t *c, const settings_t *s);
/* The settings file and the table file (either may be missing). 0 ok, -1 out of memory. */
int  vtx_config_load(vtx_config_t *c, const char *settings_path, const char *table_path);

/* what the web page shows */
typedef struct {
  vtx_state_t state;
  char message[240];             /* one line for people */
  char error[240];               /* the last problem ("" when none) */
  vtx_mode_t mode;
  char device[160];              /* "/dev/ttyAMA3 (uart3)", "tcp:127.0.0.1:5000" */
  vtx_proto_t proto;
  int sa_version;                /* 1, 2, 3 (= 2.1); 0 = not SmartAudio / unknown */
  int baud;
  bool clone_zero;               /* sends a 0x00 after each frame (AKK/RDQ clones) */
  char line_desc[96];            /* "4800 baud 8N2, a 0x00 before each frame" */
  char line_stats[400];          /* single pin: "TX edges late by 9 us on average, 38 us at most ...; 0 sent again ..." */
  bool found;                    /* the VTX answers (two-way) */
  bool in_sync;                  /* found and set like the settings */
  int freq;                      /* MHz, 0 = unknown */
  bool freq_mode;                /* tuned by frequency (SmartAudio), always for Tramp */
  int band;                      /* table index of the band it is on, -1 = not in the table */
  int channel;                   /* 1-based, 0 = not in the table */
  int power_level;               /* 1-based level in the table, 0 = not in it */
  int power_value;               /* as the VTX reports it: DAC/level (SA1/2), dBm (SA2.1), mW (Tramp); -1 */
  int power_mw;                  /* best estimate, 0 = unknown */
  bool pit, pit_at_boot, locked;
  bool have_temp;
  int temp_c;                    /* Tramp */
  int min_freq, max_freq, max_power;   /* Tramp's limits (0 = unknown) */
  vtx_config_t cfg;              /* the settings in use */
  vtx_table_t table;             /* in use: the file's, or the default for the protocol */
  uint32_t sent, replies, timeouts, bad_frames, echo_ok, echo_missing, echo_bad;
  uint32_t tx_late;              /* frames the line itself sent with bad timing (a single pin) */
  int64_t found_us, updated_us;
} vtx_status_t;

/* ---------------------------------------------------------------- the line (tests bring their own) */
typedef struct vtx_io vtx_io_t;
struct vtx_io {
  /* 0 ok; -2 the port doesn't exist (no UART overlay yet, not a Pi); -1 another error (in err) */
  int  (*open)(vtx_io_t *io, char *err, size_t errlen);
  void (*close)(vtx_io_t *io);
  int  (*set_line)(vtx_io_t *io, int baud, int stop_bits);    /* 8 data bits, no parity; 0 ok */
  int  (*write)(vtx_io_t *io, const uint8_t *p, size_t n);    /* returns when sent; 0 ok, -1 the line is gone */
  int  (*read)(vtx_io_t *io, uint8_t *p, size_t n, int timeout_ms);  /* >0 bytes, 0 none in time, -1 gone */
  void (*flush)(vtx_io_t *io);                               /* drop unread input */
  int  (*set_break)(vtx_io_t *io, bool low);                 /* hold TX low (a break) or release it; NULL = can't */
  void (*set_proto)(vtx_io_t *io, vtx_proto_t proto);        /* a hint: the protocol about to be used (bias); NULL */
  void (*stats)(vtx_io_t *io, char *out, size_t n);          /* one line about the line's timing; NULL */
  uint32_t (*late_frames)(vtx_io_t *io);                     /* frames sent with an edge too late even after
                                                              * retries (a single pin's own timing); NULL = never */
  void (*sleep_us)(vtx_io_t *io, int64_t us);                /* NULL = plat_sleep_us */
  int64_t (*now_us)(vtx_io_t *io);
  void (*destroy)(vtx_io_t *io);
  void *ud;
  char desc[160];
};

/* "gpio4" etc. (one pin, vtx_soft.c), "uart3".."uart5" (Linux: the PL011 tty whose device-tree node is
 * that UART), "gpio:/dev/gpiochipN:OFFSET", "tty:/dev/...", "tcp:HOST:PORT" (a fake VTX in tests).
 * NULL for an unknown spec (err says why). */
vtx_io_t *vtx_io_new(const char *spec, char *err, size_t errlen);
/* the vtx_port values: gpio4, gpio17, gpio22, gpio23, gpio27 (one pin), uart3, uart4, uart5 (two pins) */
bool vtx_port_known(const char *port);
/* the UART whose overlay the settings need ("uart3"; "" with VTX control off or on a single pin), like
 * stboot.py puts it into config.txt */
void vtx_uart_overlay(const settings_t *s, char *out, size_t n);

/* For a real Pi (sticktime-pi --vtx-test / --vtx-loopback-test): send frames on a line and report what
 * came back and the line's timing. vtx_line_test: SmartAudio GET_SETTINGS or Tramp 'v' to a VTX.
 * vtx_loopback_test: test patterns from one pin to another, wired together through 1 kOhm (no VTX).
 * Exit status: 0 all good, 1 errors, 2 the line can't be used. */
int vtx_line_test(const char *spec, const char *proto, int frames, FILE *out);
int vtx_loopback_test(const char *tx_spec, const char *rx_spec, int frames, FILE *out);
void vtx_io_free(vtx_io_t *io);
/* Linux: the /dev path of uart3/uart4/uart5, found by its device-tree node; 0 ok, -1 not there */
int  vtx_find_uart(const char *port, char *path, size_t n);
/* (for tests) the device-tree name of a port: "serial@7e201600" for uart3, NULL if unknown */
const char *vtx_uart_node(const char *port);
/* (for tests) look for the port's tty under a sysfs class directory (normally /sys/class/tty) */
int  vtx_find_uart_in(const char *sys_class_tty, const char *port, char *name, size_t n);

/* ---------------------------------------------------------------- the engine (no threads) */
typedef struct vtx_engine vtx_engine_t;
vtx_engine_t *vtx_engine_new(void);
void vtx_engine_free(vtx_engine_t *e);               /* closes the line; doesn't free the io */
void vtx_engine_set_io(vtx_engine_t *e, vtx_io_t *io);   /* closes the old line */
void vtx_engine_configure(vtx_engine_t *e, const vtx_config_t *c);
/* One transaction or decision (blocks for at most about 300 ms on the line). Returns how long to
 * wait before the next step, in ms (the thread waits less when the settings change). */
int  vtx_engine_step(vtx_engine_t *e);
void vtx_engine_status(const vtx_engine_t *e, vtx_status_t *out);

/* ---------------------------------------------------------------- the thread */
typedef struct vtx vtx_t;
typedef struct {
  const char *transport;         /* overrides vtx_port: "tcp:127.0.0.1:5000", "tty:/dev/pts/3", "uart4" */
} vtx_options_t;

/* Starts the VTX thread (it reads app->paths.settings and <base>/vtxtable.json). NULL on OOM. */
vtx_t *vtx_open(app_t *app, const vtx_options_t *opt);
void   vtx_reload(vtx_t *v);                         /* never blocks: the thread re-reads both files */
void   vtx_get_status(vtx_t *v, vtx_status_t *out);  /* a copy; v NULL gives "off" */
void   vtx_close(vtx_t *v);                          /* stops the thread, closes the line */
/* <base>/vtxtable.json */
void   vtx_table_path(const app_t *app, char *out, size_t n);

/* a short line for the status page: "SmartAudio 2.1 · R1 5658 MHz · 25 mW", "searching", ... */
void   vtx_status_line(const vtx_status_t *s, char *out, size_t n);

#endif
