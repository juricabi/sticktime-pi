/* VTX control frames: TBS SmartAudio v1, v2, v2.1 and ImmersionRC Tramp. Encode and decode only,
 * no I/O (vtx.c does the line, the timing and the state). Facts and sources: docs/RESEARCH.md, VTX.
 *
 * SmartAudio (TBS spec rev 09): 4800 baud, 8 data bits, 2 stop bits, one wire, half duplex.
 *   host -> VTX:  AA 55 <cmd<<1|1> <len = payload bytes> <payload> <crc>
 *                 crc: CRC-8, polynomial 0xD5, initial 0, over every byte before it (AA 55 too)
 *   VTX -> host:  AA 55 <cmd> <len> <payload> <crc>
 *                 crc over cmd, len and the payload (not AA 55). In the spec's examples len counts the
 *                 payload and the crc; Betaflight's reader takes len as the payload alone. The parser
 *                 accepts both: it checks the crc where the spec puts it first, then one byte later.
 *   A host frame and a reply can share a command byte (SET_FREQ 0x09 = the v2 GET_SETTINGS reply),
 *   so the parser tells our own echo from a reply by where the crc starts.
 *
 * Tramp: 9600 baud 8N1, 16-byte frames both ways:
 *   0F <cmd> <12 bytes, 16-bit values little-endian from byte 2> <sum of bytes 1..13 mod 256> 00
 *   queries 'r' (limits), 'v' (settings), 's' (temperature) are answered with the same command
 *   letter; 'F' (frequency), 'P' (power in mW) and 'I' (pit mode) are not answered. */
#ifndef VTX_PROTO_H
#define VTX_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------- SmartAudio */
enum {
  SA_GET_SETTINGS = 0x01,
  SA_SET_POWER = 0x02,
  SA_SET_CHANNEL = 0x03,
  SA_SET_FREQ = 0x04,
  SA_SET_MODE = 0x05,
  SA_SETTINGS_V2 = 0x09,           /* GET_SETTINGS reply of a v2 VTX (version in bits 7..3) */
  SA_SETTINGS_V21 = 0x11,          /* ... of a v2.1 VTX */
};
/* operation mode as GET_SETTINGS reports it */
#define SA_MODE_FREQ      0x01     /* tuned by frequency (else by channel) */
#define SA_MODE_PIT       0x02     /* pit mode is on now */
#define SA_MODE_PIR       0x04     /* in-range pit mode flag (boots into pit mode) */
#define SA_MODE_POR       0x08     /* out-range pit mode flag */
#define SA_MODE_UNLOCKED  0x10
/* SET_MODE bits */
#define SA_SET_PIR        0x01
#define SA_SET_POR        0x02
#define SA_SET_PIT_OFF    0x04
#define SA_SET_UNLOCK     0x08
/* SET_FREQ flags */
#define SA_FREQ_SET_PIT   0x8000   /* the value is the pit mode frequency */
#define SA_FREQ_GET_PIT   0x4000   /* asks for the pit mode frequency */
#define SA_POWER_DBM      0x80     /* SET_POWER (v2.1): the value is dBm */

#define SA_FRAME_MAX      40       /* longest frame the parser keeps (a v2.1 reply is 16) */
#define SA_BAUD           4800

uint8_t vtx_sa_crc8(const uint8_t *p, size_t n);

/* A host frame into out (at least 5 + n bytes): returns its length. No lead-in byte. */
size_t vtx_sa_frame(uint8_t *out, int cmd, const uint8_t *payload, size_t n);
size_t vtx_sa_get_settings(uint8_t *out);
size_t vtx_sa_set_power(uint8_t *out, uint8_t value);
size_t vtx_sa_set_channel(uint8_t *out, uint8_t index);
size_t vtx_sa_set_freq(uint8_t *out, uint16_t freq_and_flags);
size_t vtx_sa_set_mode(uint8_t *out, uint8_t mode);

typedef struct {
  uint8_t cmd;                     /* reply: 0x01/0x09/0x11 settings, 2..5 the set commands; host frame: cmd<<1|1 */
  bool host;                       /* a host frame (our own echo, or another host) */
  uint8_t len;                     /* the length byte as sent */
  uint8_t n;                       /* payload bytes */
  uint8_t payload[SA_FRAME_MAX];
} vtx_sa_frame_t;

typedef struct {
  uint8_t buf[SA_FRAME_MAX + 8];
  int n;
  uint32_t bad_crc, bad_len, skipped;   /* statistics */
} vtx_sa_parser_t;

void vtx_sa_parser_reset(vtx_sa_parser_t *p);
/* Feed one received byte. Returns 1 when a frame (reply or host frame) completed into *f, else 0.
 * Garbage, a bad length or a bad crc drop bytes until the next AA 55. */
int vtx_sa_parser_feed(vtx_sa_parser_t *p, uint8_t c, vtx_sa_frame_t *f);

typedef struct {
  int version;                     /* 1 = v1, 2 = v2, 3 = v2.1 */
  uint8_t channel;                 /* 0..39: band * 8 + channel of the VTX's own table (A, B, E, F, R) */
  uint8_t power;                   /* v1: the PA DAC value, v2: level 0..3, v2.1: as v2 (ignore) */
  uint8_t mode;                    /* SA_MODE_* */
  uint16_t freq;                   /* MHz */
  int dbm;                         /* v2.1: power now in dBm, else -1 */
  int nlevels;                     /* v2.1: the power levels it offers (dBm, without the 0 dBm pit entry) */
  uint8_t levels[8];
} vtx_sa_settings_t;

/* A GET_SETTINGS reply (cmd 0x01, 0x09 or 0x11). 0 ok, -1 not one or too short. */
int vtx_sa_decode_settings(const vtx_sa_frame_t *f, vtx_sa_settings_t *s);

/* ---------------------------------------------------------------- Tramp */
#define TRAMP_FRAME 16
#define TRAMP_BAUD 9600
#define TRAMP_LOCKED 0x01          /* control mode bit: race lock (frequency and power can't change) */

uint8_t vtx_tramp_checksum(const uint8_t f[TRAMP_FRAME]);
void vtx_tramp_frame(uint8_t out[TRAMP_FRAME], char cmd, uint16_t param);

typedef struct {
  uint8_t buf[TRAMP_FRAME];
  int n;
  uint32_t bad_sum, skipped;
} vtx_tramp_parser_t;

void vtx_tramp_parser_reset(vtx_tramp_parser_t *p);
/* Feed one byte; 1 when 16 bytes with 0x0F, a good checksum and the final 00 completed into out. */
int vtx_tramp_parser_feed(vtx_tramp_parser_t *p, uint8_t c, uint8_t out[TRAMP_FRAME]);

typedef struct {
  char cmd;                        /* 'r', 'v' or 's' */
  uint16_t min_freq, max_freq, max_power;            /* 'r' */
  uint16_t freq, power, act_power;                   /* 'v': configured and actual mW */
  uint8_t control, pit;                              /* 'v': TRAMP_LOCKED, pit mode (1 = on) */
  int16_t temp;                                      /* 's': degrees C */
} vtx_tramp_reply_t;

/* A reply frame. 0 ok; -1 unknown command, or a query's echo ('r'/'v' with frequency 0). */
int vtx_tramp_decode(const uint8_t f[TRAMP_FRAME], vtx_tramp_reply_t *r);

/* ---------------------------------------------------------------- power units */
int  vtx_dbm_to_mw(int dbm);                         /* 14 -> 25, 20 -> 100, 26 -> 400 (rounded like VTX labels) */
void vtx_mw_label(char *out, size_t n, int mw);      /* "25", "400", "1W", "2.5W" (at most 4 characters) */

#endif
