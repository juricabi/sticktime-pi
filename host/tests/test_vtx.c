/* VTX control: SmartAudio and Tramp frames (the TBS spec's own examples), the parsers with garbage,
 * echoes and both reply-length conventions, the VTX tables and their Betaflight JSON, and the engine
 * against an in-process fake VTX on a virtual clock: finding it (every line variant), setting it,
 * setting it back after a button press, pit mode, unplug/replug, refusals, send-only, echo problems. */
#ifndef _WIN32
#define _GNU_SOURCE                        /* posix_openpt, ptsname (the pty test) */
#endif
#include "t.h"
#include "settings.h"
#include "vtx.h"
#include "vtx_proto.h"
#include "vtx_table.h"
#include "vtx_soft.h"

#include <math.h>
#include <stdint.h>
#ifdef __linux__
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#endif

static size_t hex(const char *s, uint8_t *out) {
  size_t n = 0;
  while (*s) {
    while (*s == ' ') s++;
    if (!*s) break;
    unsigned v;
    sscanf(s, "%2x", &v);
    out[n++] = (uint8_t)v;
    s += 2;
  }
  return n;
}

/* ================================================================ SmartAudio frames */
static void test_sa_frames(void) {
  uint8_t f[16], x[32];
  /* the spec's host examples: CRC over the whole frame */
  CHECK_INT(vtx_sa_get_settings(f), 5);
  CHECK(!memcmp(f, x, hex("AA 55 03 00 9F", x)));
  CHECK_INT(vtx_sa_set_power(f, 0x00), 6);
  CHECK(!memcmp(f, x, hex("AA 55 05 01 00 6B", x)));
  vtx_sa_set_power(f, 0x8E);                           /* 2.1: 14 dBm with the MSB */
  CHECK(!memcmp(f, x, hex("AA 55 05 01 8E 2C", x)));
  vtx_sa_set_channel(f, 0);
  CHECK(!memcmp(f, x, hex("AA 55 07 01 00 B8", x)));
  CHECK_INT(vtx_sa_set_freq(f, 5865), 7);
  CHECK(!memcmp(f, x, hex("AA 55 09 02 16 E9 DC", x)));
  vtx_sa_set_mode(f, 0x0A);
  CHECK(!memcmp(f, x, hex("AA 55 0B 01 0A 7B", x)));
  CHECK_INT(vtx_sa_crc8(x, 0), 0);
  /* a message followed by its CRC gives 0 (why Betaflight's reader works with TBS replies) */
  size_t n = hex("01 06 00 00 01 16 E9 4D", x);
  CHECK_INT(vtx_sa_crc8(x, n), 0);
}

static int feed_all(vtx_sa_parser_t *p, const uint8_t *b, size_t n, vtx_sa_frame_t *out, int max) {
  int k = 0;
  for (size_t i = 0; i < n; i++) {
    vtx_sa_frame_t f;
    if (vtx_sa_parser_feed(p, b[i], &f) && k < max) out[k++] = f;
  }
  return k;
}

static void test_sa_parser(void) {
  vtx_sa_parser_t p;
  vtx_sa_frame_t fr[8];
  vtx_sa_settings_t s;
  uint8_t b[128];
  size_t n;

  /* the spec's V1 reply (len counts the CRC; CRC without AA 55), a trailing 0x00 like the captures */
  vtx_sa_parser_reset(&p);
  n = hex("AA 55 01 06 00 00 01 16 E9 4D 00", b);
  CHECK_INT(feed_all(&p, b, n, fr, 8), 1);
  CHECK(!fr[0].host);
  CHECK_INT(fr[0].cmd, 1);
  CHECK_INT(fr[0].n, 5);
  CHECK_INT(vtx_sa_decode_settings(&fr[0], &s), 0);
  CHECK_INT(s.version, 1);
  CHECK_INT(s.channel, 0);
  CHECK_INT(s.mode, 1);
  CHECK_INT(s.freq, 5865);
  CHECK_INT(s.dbm, -1);

  /* V2 (the spec prints CRC 0xDA: a typo, its capture and the maths give 0x0A) */
  vtx_sa_parser_reset(&p);
  n = hex("AA 55 09 06 01 00 1A 16 E9 0A", b);
  CHECK_INT(feed_all(&p, b, n, fr, 8), 1);
  CHECK_INT(vtx_sa_decode_settings(&fr[0], &s), 0);
  CHECK_INT(s.version, 2);
  CHECK_INT(s.channel, 1);
  CHECK_INT(s.mode, 0x1A);
  CHECK(s.mode & SA_MODE_PIT);
  CHECK(s.mode & SA_MODE_UNLOCKED);
  n = hex("AA 55 09 06 01 00 1A 16 E9 DA 00", b);      /* with the typo: refused */
  vtx_sa_parser_reset(&p);
  CHECK_INT(feed_all(&p, b, n, fr, 8), 0);
  CHECK(p.bad_crc > 0);

  /* V2.1: count 3 and four levels (0 = pit mode): 14, 20, 26 dBm */
  vtx_sa_parser_reset(&p);
  n = hex("AA 55 11 0C 00 00 00 16 E9 0E 03 00 0E 14 1A 01 00 00", b);
  CHECK_INT(feed_all(&p, b, n, fr, 8), 1);
  CHECK_INT(vtx_sa_decode_settings(&fr[0], &s), 0);
  CHECK_INT(s.version, 3);
  CHECK_INT(s.dbm, 14);
  CHECK_INT(s.nlevels, 3);
  CHECK(s.levels[0] == 14 && s.levels[1] == 20 && s.levels[2] == 26);

  /* OpenVTx style: len = the payload, CRC after it; its V2.1 count is the true count, no 0 entry */
  uint8_t pl[16];
  size_t pn = hex("00 00 00 16 E9 0E 05 01 02 0E 14 1A", pl);
  b[0] = 0xAA; b[1] = 0x55; b[2] = 0x11; b[3] = (uint8_t)pn;
  memcpy(b + 4, pl, pn);
  b[4 + pn] = vtx_sa_crc8(b + 2, pn + 2);
  vtx_sa_parser_reset(&p);
  CHECK_INT(feed_all(&p, b, pn + 5, fr, 8), 1);
  CHECK_INT(fr[0].n, (int)pn);
  CHECK_INT(vtx_sa_decode_settings(&fr[0], &s), 0);
  CHECK_INT(s.nlevels, 5);
  CHECK(s.levels[2] == 14 && s.levels[4] == 26);

  /* set-command replies, both conventions; the spec's 2.1 SET_POWER reply prints 0x0F (really 0x6D) */
  vtx_sa_parser_reset(&p);
  n = hex("AA 55 02 03 00 01 0F  AA 55 02 03 0E 01 6D  AA 55 02 02 0E 01 AE", b);
  b[n - 1] = vtx_sa_crc8(b + n - 5, 4);                /* OpenVTx: len 2, CRC over 02 02 0E 01 */
  CHECK_INT(feed_all(&p, b, n, fr, 8), 3);
  CHECK(fr[0].cmd == 2 && fr[0].n == 2 && fr[0].payload[0] == 0);
  CHECK(fr[1].cmd == 2 && fr[1].payload[0] == 0x0E);
  CHECK(fr[2].cmd == 2 && fr[2].n == 2 && fr[2].payload[0] == 0x0E);
  CHECK_INT(vtx_sa_decode_settings(&fr[0], &s), -1);   /* not a settings reply */

  /* our own frames coming back are host frames, even SET_FREQ (0x09 = the v2 settings reply code) */
  vtx_sa_parser_reset(&p);
  n = hex("00 AA 55 09 02 16 E9 DC 00 AA 55 03 00 9F", b);
  CHECK_INT(feed_all(&p, b, n, fr, 8), 2);
  CHECK(fr[0].host && fr[0].cmd == 0x09 && fr[0].n == 2);
  CHECK(fr[1].host && fr[1].cmd == 0x03 && fr[1].n == 0);
  CHECK_INT(vtx_sa_decode_settings(&fr[0], &s), -1);

  /* garbage, a false AA, a too-long length, a broken frame, then a good one; byte by byte */
  vtx_sa_parser_reset(&p);
  n = hex("FF AA 13 AA 55 F0 AA 55 01 06 00 00 01 16 E9 4E AA AA 55 01 06 00 00 01 16 E9 4D", b);
  CHECK_INT(feed_all(&p, b, n, fr, 8), 1);
  CHECK_INT(fr[0].payload[4], 0xE9);
  CHECK(p.bad_len >= 1);
  CHECK(p.skipped >= 1);

  /* short / truncated input: nothing, then the rest completes it */
  vtx_sa_parser_reset(&p);
  n = hex("AA 55 01 06 00 00", b);
  CHECK_INT(feed_all(&p, b, n, fr, 8), 0);
  n = hex("01 16 E9 4D", b);
  CHECK_INT(feed_all(&p, b, n, fr, 8), 1);
  /* a settings reply that is too short to decode */
  vtx_sa_frame_t sh;
  memset(&sh, 0, sizeof sh);
  sh.cmd = SA_SETTINGS_V2;
  sh.n = 3;
  CHECK_INT(vtx_sa_decode_settings(&sh, &s), -1);
  /* v2.1 without its level list */
  memset(&sh, 0, sizeof sh);
  sh.cmd = SA_SETTINGS_V21;
  sh.n = 5;
  sh.payload[3] = 0x16; sh.payload[4] = 0xE9;
  CHECK_INT(vtx_sa_decode_settings(&sh, &s), 0);
  CHECK_INT(s.nlevels, 0);
  CHECK_INT(s.dbm, -1);
}

/* ================================================================ Tramp frames */
static void test_tramp(void) {
  uint8_t f[TRAMP_FRAME], g[TRAMP_FRAME];
  vtx_tramp_frame(f, 'F', 5800);
  CHECK(f[0] == 0x0F && f[1] == 'F' && f[2] == (5800 & 0xFF) && f[3] == (5800 >> 8) && f[15] == 0);
  CHECK_INT(f[14], (uint8_t)('F' + (5800 & 0xFF) + (5800 >> 8)));
  vtx_tramp_frame(f, 'r', 0);
  CHECK_INT(f[14], 'r');
  /* a 'v' reply: 5865 MHz, 200 mW configured, race lock off, pit on, 190 mW actual */
  memset(g, 0, sizeof g);
  g[0] = 0x0F; g[1] = 'v'; g[2] = 5865 & 0xFF; g[3] = 5865 >> 8; g[4] = 200; g[6] = 0; g[7] = 1; g[8] = 190;
  g[14] = vtx_tramp_checksum(g);
  vtx_tramp_parser_t p;
  vtx_tramp_parser_reset(&p);
  uint8_t garbage[5] = { 0x00, 0x0F, 0x76, 0xFF, 0x12 }, out[TRAMP_FRAME];
  int got = 0;
  for (int i = 0; i < 5; i++) got += vtx_tramp_parser_feed(&p, garbage[i], out);
  for (int i = 0; i < TRAMP_FRAME; i++) got += vtx_tramp_parser_feed(&p, g[i], out);
  CHECK_INT(got, 1);
  vtx_tramp_reply_t r;
  CHECK_INT(vtx_tramp_decode(out, &r), 0);
  CHECK(r.cmd == 'v' && r.freq == 5865 && r.power == 200 && r.pit == 1 && r.act_power == 190 && r.control == 0);
  /* a bad checksum, a last byte that isn't 0: refused */
  uint8_t bad[TRAMP_FRAME];
  memcpy(bad, g, sizeof bad);
  bad[14] ^= 1;
  vtx_tramp_parser_reset(&p);
  got = 0;
  for (int i = 0; i < TRAMP_FRAME; i++) got += vtx_tramp_parser_feed(&p, bad[i], out);
  CHECK_INT(got, 0);
  CHECK(p.bad_sum > 0);
  memcpy(bad, g, sizeof bad);
  bad[15] = 1;
  vtx_tramp_parser_reset(&p);
  got = 0;
  for (int i = 0; i < TRAMP_FRAME; i++) got += vtx_tramp_parser_feed(&p, bad[i], out);
  CHECK_INT(got, 0);
  /* our queries coming back decode as echoes; 'r' and 's' replies */
  vtx_tramp_frame(f, 'v', 0);
  CHECK_INT(vtx_tramp_decode(f, &r), -1);
  vtx_tramp_frame(f, 'r', 0);
  CHECK_INT(vtx_tramp_decode(f, &r), -1);
  memset(g, 0, sizeof g);
  g[0] = 0x0F; g[1] = 'r'; g[2] = 5600 & 0xFF; g[3] = 5600 >> 8; g[4] = 5950 & 0xFF; g[5] = 5950 >> 8; g[6] = 600 & 0xFF; g[7] = 600 >> 8;
  g[14] = vtx_tramp_checksum(g);
  CHECK_INT(vtx_tramp_decode(g, &r), 0);
  CHECK(r.min_freq == 5600 && r.max_freq == 5950 && r.max_power == 600);
  memset(g, 0, sizeof g);
  g[0] = 0x0F; g[1] = 's'; g[6] = (uint8_t)-5; g[7] = 0xFF;
  CHECK_INT(vtx_tramp_decode(g, &r), 0);
  CHECK_INT(r.temp, -5);
  g[1] = 'x';
  CHECK_INT(vtx_tramp_decode(g, &r), -1);
}

static void test_units(void) {
  CHECK_INT(vtx_dbm_to_mw(14), 25);
  CHECK_INT(vtx_dbm_to_mw(20), 100);
  CHECK_INT(vtx_dbm_to_mw(23), 200);
  CHECK_INT(vtx_dbm_to_mw(26), 400);
  CHECK_INT(vtx_dbm_to_mw(27), 500);
  CHECK_INT(vtx_dbm_to_mw(29), 800);
  CHECK_INT(vtx_dbm_to_mw(30), 1000);
  CHECK_INT(vtx_dbm_to_mw(0), 1);
  char l[8];
  vtx_mw_label(l, sizeof l, 25); CHECK_STR(l, "25");
  vtx_mw_label(l, sizeof l, 800); CHECK_STR(l, "800");
  vtx_mw_label(l, sizeof l, 1000); CHECK_STR(l, "1W");
  vtx_mw_label(l, sizeof l, 2500); CHECK_STR(l, "2W5");
}

/* ================================================================ single pin: the bit schedule and the decoder */
static void test_soft_schedule(void) {
  vtx_edge_t e[2048];
  int64_t end;
  const int64_t B = 208333;                               /* one bit at 4800 baud, ns (rounded) */
  uint8_t b = 0x55;                                       /* 0 1010 1010 11: an edge on every bit to the stop bit */
  int n = vtx_soft_schedule(&b, 1, 4800, 2, 1, e, 2048, &end);
  CHECK_INT(n, 10);
  for (int i = 0; i < n && i < 10; i++) {
    CHECK_NEAR((double)e[i].t_ns, i * 1e9 / 4800, 1);
    CHECK_INT(e[i].level, i % 2);                         /* start low, d0 high, ... d7 low, stop high */
  }
  CHECK_NEAR((double)end, 11 * 1e9 / 4800, 1);
  b = 0x00;                                               /* SmartAudio's lead-in on a line resting low: one edge */
  n = vtx_soft_schedule(&b, 1, 4800, 2, 0, e, 2048, &end);
  CHECK_INT(n, 1);
  CHECK(e[0].level == 1 && llabs(e[0].t_ns - 9 * B) <= 3);
  b = 0xFF;                                               /* idle high: just the start bit */
  n = vtx_soft_schedule(&b, 1, 9600, 1, 1, e, 2048, &end);
  CHECK_INT(n, 2);
  CHECK(e[0].t_ns == 0 && e[0].level == 0 && e[1].level == 1 && llabs(e[1].t_ns - 104167) <= 1);
  CHECK_NEAR((double)end, 10 * 1e9 / 9600, 1);
  /* no drift: edge times come from the frame's start, rounded once */
  uint8_t many[200];
  for (int i = 0; i < 200; i++) many[i] = (uint8_t)(i * 7 + 1);
  n = vtx_soft_schedule(many, 200, 4800, 2, 1, e, 2048, &end);
  CHECK(n > 200);
  bool exact = true;
  for (int i = 0; i < n; i++) {
    double slots = e[i].t_ns / (1e9 / 4800);
    if (fabs(slots - floor(slots + 0.5)) * (1e9 / 4800) > 0.6) exact = false;
  }
  CHECK(exact);
  CHECK_NEAR((double)end, 200 * 11 * 1e9 / 4800, 1);
  CHECK_INT(vtx_soft_schedule(many, 200, 4800, 2, 1, e, 5, &end), 5);    /* at most max */
}

static uint32_t rnd_state = 12345;
static int rnd(int span) {                                /* -span..span, the same every run */
  rnd_state = rnd_state * 1103515245u + 12345u;
  return span ? (int)((rnd_state >> 8) % (uint32_t)(2 * span + 1)) - span : 0;
}

/* send bytes through the schedule with a clock off by scale, edge jitter, from t0; decode; returns bytes */
static int soft_round(const uint8_t *p, int n, int baud, int stop, double scale, int jitter_ns, int idle_level,
                      uint8_t *out, int max, vtx_soft_rx_t *r) {
  static vtx_edge_t e[8192];
  int64_t end;
  int ne = vtx_soft_schedule(p, (size_t)n, baud, stop, 1, e, 8192, &end);
  vtx_soft_rx_init(r, baud, stop, idle_level);
  int64_t t0 = 5000000000LL;
  int k = 0;
  if (idle_level == 0) k += vtx_soft_rx_edge(r, t0 - 400000, 1, out + k, max - k);   /* the line goes to idle high first */
  for (int i = 0; i < ne; i++)
    k += vtx_soft_rx_edge(r, t0 + (int64_t)(e[i].t_ns * scale) + rnd(jitter_ns), e[i].level, out + k, max - k);
  CHECK_INT(vtx_soft_rx_idle(r, t0 + (int64_t)(end * scale) - 1000000, out + k, max - k), 0);   /* not yet */
  k += vtx_soft_rx_idle(r, t0 + (int64_t)(end * scale) + 10000000, out + k, max - k);
  return k;
}

static void test_soft_decoder(void) {
  vtx_soft_rx_t r;
  uint8_t all[256], out[600];
  for (int i = 0; i < 256; i++) all[i] = (uint8_t)i;
  static const struct { int baud, stop; double scale; int jitter; const char *what; } C[] = {
    { 4800, 2, 1.0, 0, "SmartAudio exact" }, { 4800, 2, 1.05, 0, "SmartAudio, VTX 5 % slow" },
    { 4800, 2, 0.95, 0, "SmartAudio, VTX 5 % fast" }, { 4800, 2, 1.0, 20000, "SmartAudio, 20 us jitter" },
    { 4800, 2, 1.04, 15000, "SmartAudio, 4 % slow + 15 us jitter" }, { 4800, 1, 0.96, 10000, "SmartAudio 1 stop bit, 4 % fast" },
    { 9600, 1, 1.0, 0, "Tramp exact" }, { 9600, 1, 1.05, 0, "Tramp 5 % slow" }, { 9600, 1, 0.95, 0, "Tramp 5 % fast" },
    { 9600, 1, 1.0, 10000, "Tramp, 10 us jitter" }, { 4900, 2, 1.0, 5000, "4900 baud" },
  };
  for (size_t c = 0; c < sizeof C / sizeof C[0]; c++) {
    int k = soft_round(all, 256, C[c].baud, C[c].stop, C[c].scale, C[c].jitter, 1, out, 600, &r);
    bool same = k == 256 && !memcmp(out, all, 256);
    if (!same) fprintf(stderr, "decoder: %s: %d bytes, framing %u, glitches %u\n", C[c].what, k, r.framing, r.glitches);
    CHECK(same);
    CHECK_INT(r.framing, 0);
  }
  /* a single pin on a line resting low (SmartAudio's pull-down): it rises, then the frame; then it rests low again */
  uint8_t sa[7] = { 0xAA, 0x55, 0x01, 0x06, 0x00, 0x00, 0x4D };
  int k = soft_round(sa, 7, 4800, 2, 1.0, 5000, 0, out, 600, &r);
  CHECK(k == 7 && !memcmp(out, sa, 7));
  int64_t t = 9000000000LL;
  CHECK_INT(vtx_soft_rx_edge(&r, t, 0, out, 10), 0);      /* the VTX lets go: the line falls and stays low */
  CHECK_INT(vtx_soft_rx_idle(&r, t + 5000000, out, 10), 0);
  CHECK_INT(r.breaks, 1);                                 /* not a 0x00 byte */
  /* glitches: a 4 us dip on the idle line, a 6 us spike inside a data bit (away from its middle) */
  vtx_soft_rx_init(&r, 4800, 2, 1);
  const int64_t B = 208333;
  t = 1000000;
  k = vtx_soft_rx_edge(&r, t, 0, out, 10);
  k += vtx_soft_rx_edge(&r, t + 4000, 1, out + k, 10 - k);
  CHECK_INT(r.glitches, 1);
  CHECK(!r.in_byte);
  t += 5000000;                                           /* 0xA5 = 1010 0101: start, 1,0,1,0,0,1,0,1, stop */
  static const int lv[11] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 1, 1 };
  int prev = 1;
  for (int i = 0; i < 11; i++) {
    if (lv[i] != prev) k += vtx_soft_rx_edge(&r, t + i * B, lv[i], out + k, 10 - k);
    if (i == 4) {                                         /* bit 3 (low): a spike near its start */
      k += vtx_soft_rx_edge(&r, t + i * B + 30000, 1, out + k, 10 - k);
      k += vtx_soft_rx_edge(&r, t + i * B + 36000, 0, out + k, 10 - k);
    }
    prev = lv[i];
  }
  k += vtx_soft_rx_idle(&r, t + 20 * B, out + k, 10 - k);
  CHECK_INT(k, 1);
  CHECK_INT(out[0], 0xA5);
  /* a missing stop bit: the byte stays (the CRC decides), counted as a framing error */
  vtx_soft_rx_init(&r, 4800, 2, 1);
  t = 1000000;
  k = vtx_soft_rx_edge(&r, t, 0, out, 10);                /* start */
  k += vtx_soft_rx_edge(&r, t + 1 * B, 1, out + k, 10 - k);   /* d0 = 1, d1..: */
  k += vtx_soft_rx_edge(&r, t + 2 * B, 0, out + k, 10 - k);   /* ... 0 through the stop bit */
  k += vtx_soft_rx_edge(&r, t + 14 * B, 1, out + k, 10 - k);  /* it rises much later */
  CHECK_INT(k, 1);
  CHECK_INT(out[0], 0x01);
  CHECK_INT(r.framing, 1);
  /* due(): the byte in progress finishes 10.5 bits after its start */
  vtx_soft_rx_init(&r, 9600, 1, 1);
  vtx_soft_rx_edge(&r, 777000, 0, out, 10);
  CHECK_INT(vtx_soft_rx_due(&r), 777000 + 104167 * 21 / 2);
  CHECK_INT(vtx_soft_rx_idle(&r, vtx_soft_rx_due(&r) - 1, out, 10), 0);
  CHECK_INT(vtx_soft_rx_idle(&r, vtx_soft_rx_due(&r), out, 10), 0);   /* low all along: a break, no byte */
  CHECK_INT(vtx_soft_rx_due(&r), 0);
  /* a missed rising edge (two falling edges): the second still starts a byte */
  vtx_soft_rx_init(&r, 9600, 1, 1);
  const int64_t T = 104167;
  t = 1000000;
  k = vtx_soft_rx_edge(&r, t, 0, out, 10);
  k += vtx_soft_rx_edge(&r, t + 30 * T, 0, out + k, 10 - k);   /* (the rise in between was lost) */
  k += vtx_soft_rx_edge(&r, t + 31 * T, 1, out + k, 10 - k);   /* 0xFF */
  k += vtx_soft_rx_idle(&r, t + 50 * T, out + k, 10 - k);
  CHECK_INT(k, 1);
  CHECK_INT(out[0], 0xFF);
  /* the ports */
  CHECK(vtx_soft_is_spec("gpio4") && vtx_soft_is_spec("gpio27") && vtx_soft_is_spec("gpio:/dev/gpiochip0:4"));
  CHECK(!vtx_soft_is_spec("gpio5") && !vtx_soft_is_spec("gpio") && !vtx_soft_is_spec("uart3") && !vtx_soft_is_spec("gpio4x"));
  CHECK_INT(vtx_soft_header_pin(4), 7);
  CHECK_INT(vtx_soft_header_pin(17), 11);
  CHECK_INT(vtx_soft_header_pin(14), 0);
  CHECK(vtx_port_known("gpio4") && vtx_port_known("uart5") && !vtx_port_known("gpio:/dev/gpiochip0:4") && !vtx_port_known("tcp:1:2"));
}

/* ================================================================ tables */
static const char GOOD_JSON[] =
  "\xEF\xBB\xBF{\n"
  "  \"description\": \"Test VTX (made for StickTime Pi's tests) \\u00e9\\ud83d\\ude00\",\n"
  "  \"version\": \"1.0\",\n"
  "  \"extra\": {\"nested\": [1, 2.5e3, true, null, {\"a\": \"b\"}]},\n"
  "  \"vtx_table\": {\n"
  "    \"bands_list\": [\n"
  "      {\"name\": \"BOSCAM_A\", \"letter\": \"A\", \"is_factory_band\": true, \"frequencies\": [5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725]},\n"
  "      {\"name\": \"BOSCAM_E\", \"letter\": \"E\", \"is_factory_band\": false, \"frequencies\": [5705, 5685, 5665, 0, 5885, 5905, 0, 0]},\n"
  "      {\"name\": \"LOWRACE \", \"letter\": \"L\", \"is_factory_band\": false, \"frequencies\": [5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621]}\n"
  "    ],\n"
  "    \"powerlevels_list\": [\n"
  "      {\"value\": 14, \"label\": \"25 \"}, {\"value\": 20, \"label\": \"100\"}, {\"value\": 26, \"label\": \"400\"}\n"
  "    ]\n"
  "  }\n"
  "}\n";

static int parse(const char *j, vtx_table_t *t, char *err) { return vtx_table_parse_json(j, strlen(j), t, err, 200); }

static void test_tables(void) {
  vtx_table_t t, u;
  char err[200];
  vtx_table_default(&t, VTX_TABLE_SA2, NULL, 0);
  CHECK_INT(t.nbands, 5);
  CHECK_INT(t.nchannels, 8);
  CHECK(t.band[0].factory && t.band[4].factory);
  CHECK_INT(t.band[4].letter, 'R');
  CHECK_INT(t.band[4].freq[0], 5658);
  CHECK_INT(t.npowers, 4);
  CHECK(t.power[0].value == 0 && t.power[3].value == 3);
  CHECK_STR(t.power[3].label, "800");
  vtx_table_default(&t, VTX_TABLE_SA1, NULL, 0);
  CHECK(t.power[0].value == 7 && t.power[3].value == 40);
  uint8_t lv[4] = { 14, 23, 27, 29 };
  vtx_table_default(&t, VTX_TABLE_SA21, lv, 4);
  CHECK_INT(t.npowers, 4);
  CHECK(t.power[1].value == 23);
  CHECK_STR(t.power[1].label, "200");
  CHECK_STR(t.power[3].label, "800");
  vtx_table_default(&t, VTX_TABLE_SA21, NULL, 0);
  CHECK(t.npowers == 3 && t.power[2].value == 26);
  vtx_table_default(&t, VTX_TABLE_TRAMP, NULL, 0);
  CHECK(!t.band[0].factory);
  CHECK(t.npowers == 5 && t.power[4].value == 600);
  /* lookups */
  CHECK_INT(vtx_table_band(&t, 'r'), 4);
  CHECK_INT(vtx_table_band(&t, 'Z'), -1);
  CHECK_INT(vtx_table_freq(&t, 4, 1), 5658);
  CHECK_INT(vtx_table_freq(&t, 4, 9), 0);
  CHECK_INT(vtx_table_freq(&t, 9, 1), 0);
  int b, c;
  CHECK(vtx_table_find_freq(&t, 5880, &b, &c));
  CHECK(b == 3 && c == 8);                               /* F8 comes before R7 */
  CHECK(!vtx_table_find_freq(&t, 5555, &b, &c));
  CHECK_INT(vtx_table_power_level(&t, 200), 3);
  CHECK_INT(vtx_table_power_level(&t, 201), 0);

  /* a good file */
  CHECK_INT(parse(GOOD_JSON, &t, err), 0);
  CHECK_STR(err, "");
  CHECK(!strncmp(t.description, "Test VTX (made for StickTime Pi's tests) \xC3\xA9\xF0\x9F\x98\x80", 64));
  CHECK_INT(t.nbands, 3);
  CHECK_INT(t.nchannels, 8);
  CHECK(t.band[0].factory && !t.band[1].factory);
  CHECK_INT(t.band[1].freq[3], 0);
  CHECK_STR(t.band[2].name, "LOWRACE");
  CHECK_INT(t.npowers, 3);
  CHECK_STR(t.power[0].label, "25");
  /* export, read back: the same table */
  size_t len;
  char *j = vtx_table_to_json(&t, &len);
  CHECK(j && strstr(j, "\"is_factory_band\": true") && strstr(j, "\"version\": \"1.0\""));
  CHECK_INT(parse(j, &u, err), 0);
  CHECK(!memcmp(&t, &u, sizeof t));
  free(j);
  /* shorter bands are padded with 0; fewer than 8 channels */
  CHECK_INT(parse("{\"vtx_table\":{\"bands_list\":[{\"letter\":\"X\",\"frequencies\":[5800,5810]},{\"letter\":\"Y\","
                  "\"frequencies\":[5900]}],\"powerlevels_list\":[{\"value\":25}]}}", &t, err), 0);
  CHECK_INT(t.nchannels, 2);
  CHECK_INT(t.band[1].freq[1], 0);
  CHECK_STR(t.band[0].name, "BAND_X");
  CHECK_STR(t.power[0].label, "25");

  /* broken files: a message each, no table */
  static const struct { const char *json, *msg; } BAD[] = {
    { "", "a JSON object" },
    { "[1,2]", "a JSON object" },
    { "{\"description\": \"x\"}", "no \"vtx_table\"" },
    { "{\"vtx_table\":{\"powerlevels_list\":[{\"value\":1,\"label\":\"1\"}]}}", "no bands" },
    { "{\"vtx_table\":{\"bands_list\":[{\"letter\":\"A\",\"frequencies\":[5800]}]}}", "no power levels" },
    { "{\"vtx_table\":{\"bands_list\":[{\"letter\":\"A\",\"frequencies\":[5800]},{\"letter\":\"a\",\"frequencies\":[5900]}],"
      "\"powerlevels_list\":[{\"value\":1}]}}", "both use the letter" },
    { "{\"vtx_table\":{\"bands_list\":[{\"letter\":\"AB\",\"frequencies\":[5800]}],\"powerlevels_list\":[{\"value\":1}]}}", "one character" },
    { "{\"vtx_table\":{\"bands_list\":[{\"frequencies\":[5800]}],\"powerlevels_list\":[{\"value\":1}]}}", "no letter" },
    { "{\"vtx_table\":{\"bands_list\":[{\"letter\":\"A\",\"frequencies\":[]}],\"powerlevels_list\":[{\"value\":1}]}}", "no frequencies" },
    { "{\"vtx_table\":{\"bands_list\":[{\"letter\":\"A\",\"frequencies\":[123]}],\"powerlevels_list\":[{\"value\":1}]}}", "isn't a frequency" },
    { "{\"vtx_table\":{\"bands_list\":[{\"letter\":\"A\",\"frequencies\":[5800.5]}],\"powerlevels_list\":[{\"value\":1}]}}", "isn't a frequency" },
    { "{\"vtx_table\":{\"bands_list\":[{\"letter\":\"A\",\"frequencies\":[5801,5802,5803,5804,5805,5806,5807,5808,5809]}],\"powerlevels_list\":[{\"value\":1}]}}", "more than 8 channels" },
    { "{\"vtx_table\":{\"bands_list\":[{\"letter\":\"A\",\"frequencies\":[5800]}],\"powerlevels_list\":[{\"label\":\"x\"}]}}", "no value" },
    { "{\"vtx_table\":{\"bands_list\":[{\"letter\":\"A\",\"frequencies\":[5800]}],\"powerlevels_list\":[{\"value\":-1}]}}", "0 to 65535" },
    { "{\"vtx_table\":{\"bands_list\":[{\"letter\":\"A\",\"frequencies\":[5800]}],\"powerlevels_list\":[{\"value\":1,\"label\":\"12345678\"}]}}", "longer than" },
    { "{\"version\":\"2.0\",\"vtx_table\":{}}", "isn't supported" },
    { "{\"vtx_table\":{\"bands_list\":[{\"letter\":\"A\",\"frequencies\":[5800]}],\"powerlevels_list\":[{\"value\":1}]}} x", "valid JSON" },
    { "{\"vtx_table\":{\"bands_list\":[{\"letter\":\"A\",\"frequencies\":[5800],}],\"powerlevels_list\":[{\"value\":1}]}}", "valid JSON" },
    { "{\"description\":\"a\nb\"}", "valid JSON" },
    { "{\"description\":\"\\ud800\"}", "valid JSON" },
    { "{\"vtx_table\":{\"bands_list\":{\"letter\":\"A\"}}}", "must be a list" },
    { "{\"vtx_table\":{\"bands_list\":[{\"letter\":\"A\",\"is_factory_band\":\"yes\",\"frequencies\":[5800]}]}}", "valid JSON" },
    { "{\"a\":[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[1]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]}", "nested too deeply" },
  };
  for (size_t i = 0; i < sizeof BAD / sizeof BAD[0]; i++) {
    memset(&t, 0x55, sizeof t);
    int r = parse(BAD[i].json, &t, err);
    CHECK_INT(r, -1);
    if (!strstr(err, BAD[i].msg)) fprintf(stderr, "bad table %d: \"%s\" lacks \"%s\"\n", (int)i, err, BAD[i].msg);
    CHECK(strstr(err, BAD[i].msg) != NULL);
    CHECK_INT(t.nbands, 0);
  }
  /* nine bands */
  char nine[2048] = "{\"vtx_table\":{\"bands_list\":[";
  for (int i = 0; i < 9; i++)
    snprintf(nine + strlen(nine), sizeof nine - strlen(nine), "%s{\"letter\":\"%c\",\"frequencies\":[5800]}", i ? "," : "", 'A' + i);
  strcat(nine, "],\"powerlevels_list\":[{\"value\":1}]}}");
  CHECK_INT(parse(nine, &t, err), -1);
  CHECK(strstr(err, "more than 8 bands") != NULL);
}

/* ================================================================ the fake VTX (in process, virtual time) */
typedef struct {
  vtx_proto_t proto;
  int version;                   /* SmartAudio 1, 2, 3 */
  bool openvtx_len;              /* replies with len = payload (OpenVTx) instead of payload + crc (TBS) */
  bool tail_zero;                /* a 0x00 after each reply (as in the spec's captures) */
  bool need_trail;               /* AKK/RDQ clone: takes a frame only with a 0x00 after it */
  bool need_low;                 /* takes a frame only after the line was held low; a 0x00 first confuses it */
  int need_stop;                 /* 0 = either */
  int baud;                      /* its real baud rate */
  int echo;                      /* 0 none (no RX wire), 1 exact, 2 garbled */
  bool plugged;
  bool ignore_power;
  int delay_us;
  /* SmartAudio state */
  uint8_t channel, power, mode;
  uint16_t freq;
  int dbm;
  uint8_t levels[8];
  int nlevels;
  /* Tramp state */
  uint16_t tfreq, tpower, tmin, tmax, tmaxp;
  uint8_t tpit, tcontrol;
  int16_t temp;
  /* the line */
  int io_baud, io_stop;
  bool low, low_seen, opened;
  /* what it took: SmartAudio cmd + first payload byte(s), Tramp letter + value */
  struct { int cmd; int v; } took[256];
  int ntook;
  /* RX queue (what the Pi's RX hears) */
  uint8_t q[8192];
  int64_t qt[8192];
  int qn, qi;
  int64_t now;
  int reads, writes;
} fake_t;

static const uint16_t SA40[40] = {
  5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725, 5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866,
  5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945, 5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880,
  5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917 };

static void fake_init(fake_t *f, vtx_proto_t proto, int version) {
  memset(f, 0, sizeof *f);
  f->proto = proto;
  f->version = version;
  f->baud = proto == VTX_PROTO_TRAMP ? 9600 : 4800;
  f->echo = 1;
  f->plugged = true;
  f->delay_us = 60000;
  f->tail_zero = true;
  f->channel = 0;
  f->freq = 5865;
  f->mode = 0x10;                                       /* unlocked, by channel */
  f->power = version == 1 ? 7 : 0;
  f->dbm = 14;
  uint8_t lv[3] = { 14, 20, 26 };
  memcpy(f->levels, lv, 3);
  f->nlevels = 3;
  f->tfreq = 5740; f->tpower = 25; f->tmin = 5600; f->tmax = 5950; f->tmaxp = 600; f->temp = 31;
  f->now = 1000000;
}

static void q_push(fake_t *f, const uint8_t *b, size_t n, int64_t t) {
  for (size_t i = 0; i < n && f->qn < (int)sizeof f->q; i++) { f->q[f->qn] = b[i]; f->qt[f->qn++] = t; }
}

static void took(fake_t *f, int cmd, int v) {
  if (f->ntook < 256) { f->took[f->ntook].cmd = cmd; f->took[f->ntook].v = v; f->ntook++; }
}

static int count_took(const fake_t *f, int cmd, int v) {
  int n = 0;
  for (int i = 0; i < f->ntook; i++) n += f->took[i].cmd == cmd && (v < 0 || f->took[i].v == v);
  return n;
}

static void sa_reply(fake_t *f, uint8_t cmd, const uint8_t *pl, size_t n) {
  uint8_t r[48];
  r[0] = 0xAA; r[1] = 0x55; r[2] = cmd;
  r[3] = (uint8_t)(f->openvtx_len ? n : n + 1);
  memcpy(r + 4, pl, n);
  r[4 + n] = vtx_sa_crc8(r + 2, n + 2);
  size_t len = n + 5;
  if (f->tail_zero && !f->openvtx_len) r[len++] = 0;
  q_push(f, r, len, f->now + f->delay_us);
}

static void sa_process(fake_t *f, uint8_t cmd, const uint8_t *pl, int n) {
  uint8_t r[24];
  int v = n ? pl[0] : 0;
  switch (cmd >> 1) {
    case SA_GET_SETTINGS: {
      took(f, SA_GET_SETTINGS, 0);
      uint8_t code = f->version == 1 ? SA_GET_SETTINGS : f->version == 2 ? SA_SETTINGS_V2 : SA_SETTINGS_V21;
      size_t k = 0;
      r[k++] = f->channel; r[k++] = f->power; r[k++] = f->mode; r[k++] = (uint8_t)(f->freq >> 8); r[k++] = (uint8_t)f->freq;
      if (f->version == 3) {
        r[k++] = (uint8_t)f->dbm;
        if (f->openvtx_len) { r[k++] = (uint8_t)f->nlevels; for (int i = 0; i < f->nlevels; i++) r[k++] = f->levels[i]; }
        else { r[k++] = (uint8_t)f->nlevels; r[k++] = 0; for (int i = 0; i < f->nlevels; i++) r[k++] = f->levels[i]; }
      }
      sa_reply(f, code, r, k);
      return;
    }
    case SA_SET_POWER:
      took(f, SA_SET_POWER, v);
      if (!f->ignore_power) {
        if (f->version == 3 && (v & 0x80)) {
          int d = v & 0x7F;
          if (d == 0) f->mode |= SA_MODE_PIT;           /* 0 dBm: pit mode, the dBm figure stays */
          else {
            int best = f->levels[0];
            for (int i = 0; i < f->nlevels; i++) if (f->levels[i] <= d) best = f->levels[i];
            f->dbm = best;
            f->mode &= (uint8_t)~SA_MODE_PIT;
          }
        } else {
          f->power = (uint8_t)v;
          if (f->version == 3 && v < f->nlevels) { f->dbm = f->levels[v]; f->mode &= (uint8_t)~SA_MODE_PIT; }
        }
      }
      r[0] = (uint8_t)v; r[1] = 1;
      sa_reply(f, SA_SET_POWER, r, 2);
      return;
    case SA_SET_CHANNEL:
      took(f, SA_SET_CHANNEL, v);
      if (v < 40) { f->channel = (uint8_t)v; f->freq = SA40[v]; f->mode &= (uint8_t)~SA_MODE_FREQ; }
      r[0] = (uint8_t)v; r[1] = 1;
      sa_reply(f, SA_SET_CHANNEL, r, 2);
      return;
    case SA_SET_FREQ: {
      int fq = n >= 2 ? (pl[0] << 8 | pl[1]) : 0;
      took(f, SA_SET_FREQ, fq);
      if (!(fq & (SA_FREQ_GET_PIT | SA_FREQ_SET_PIT))) { f->freq = (uint16_t)fq; f->mode |= SA_MODE_FREQ; }
      r[0] = (uint8_t)(fq >> 8); r[1] = (uint8_t)fq; r[2] = 1;
      sa_reply(f, SA_SET_FREQ, r, 3);
      return;
    }
    case SA_SET_MODE:
      took(f, SA_SET_MODE, v);
      if (f->version >= 2) {
        uint8_t flags = (uint8_t)((v & SA_SET_PIR ? SA_MODE_PIR : 0) | (v & SA_SET_POR ? SA_MODE_POR : 0));
        f->mode = (uint8_t)((f->mode & ~(SA_MODE_PIR | SA_MODE_POR)) | flags);
        if (v & SA_SET_PIT_OFF) f->mode &= (uint8_t)~SA_MODE_PIT;
        else if (flags && f->version == 3) f->mode |= SA_MODE_PIT;     /* 2.1: at once; 2.0: at the next power-up */
        if (v & SA_SET_UNLOCK) f->mode |= SA_MODE_UNLOCKED;
      }
      r[0] = (uint8_t)v; r[1] = 1;
      sa_reply(f, SA_SET_MODE, r, 2);
      return;
  }
}

static void tramp_process(fake_t *f, const uint8_t *fr) {
  int v = fr[2] | fr[3] << 8;
  uint8_t r[TRAMP_FRAME];
  memset(r, 0, sizeof r);
  r[0] = 0x0F;
  r[1] = fr[1];
  took(f, fr[1], v);
  switch (fr[1]) {
    case 'r': r[2] = (uint8_t)f->tmin; r[3] = (uint8_t)(f->tmin >> 8); r[4] = (uint8_t)f->tmax; r[5] = (uint8_t)(f->tmax >> 8);
              r[6] = (uint8_t)f->tmaxp; r[7] = (uint8_t)(f->tmaxp >> 8); break;
    case 'v': r[2] = (uint8_t)f->tfreq; r[3] = (uint8_t)(f->tfreq >> 8); r[4] = (uint8_t)f->tpower; r[5] = (uint8_t)(f->tpower >> 8);
              r[6] = f->tcontrol; r[7] = f->tpit; r[8] = (uint8_t)f->tpower; r[9] = (uint8_t)(f->tpower >> 8); break;
    case 's': r[6] = (uint8_t)f->temp; r[7] = (uint8_t)(f->temp >> 8); break;
    case 'F': if (!(f->tcontrol & 1)) f->tfreq = (uint16_t)v; return;
    case 'P': if (!(f->tcontrol & 1) && !f->ignore_power) f->tpower = (uint16_t)v; return;
    case 'I': f->tpit = v ? 0 : 1; return;              /* 0 = pit on */
    default: return;
  }
  r[14] = vtx_tramp_checksum(r);
  q_push(f, r, TRAMP_FRAME, f->now + f->delay_us);
}

/* what the VTX makes of bytes on the wire */
static void fake_hear(fake_t *f, const uint8_t *p, size_t n) {
  bool low_before = f->low_seen;
  f->low_seen = false;
  if (!f->plugged) return;
  int d = f->io_baud - f->baud;
  if (d < 0) d = -d;
  if (d * 100 > f->baud * 2) return;                    /* more than 2 % off: garbage to it */
  if (f->need_stop && f->io_stop != f->need_stop) return;
  if (f->proto == VTX_PROTO_TRAMP) {
    for (size_t i = 0; i + TRAMP_FRAME <= n; i++)
      if (p[i] == 0x0F && vtx_tramp_checksum(p + i) == p[i + 14] && p[i + 15] == 0) { tramp_process(f, p + i); i += TRAMP_FRAME - 1; }
    return;
  }
  if (f->need_low && (!low_before || p[0] == 0x00)) return;
  for (size_t i = 0; i + 5 <= n; i++) {
    if (p[i] != 0xAA || p[i + 1] != 0x55) continue;
    int len = p[i + 3];
    if (i + 5 + (size_t)len > n) break;
    if (vtx_sa_crc8(p + i, (size_t)len + 4) != p[i + 4 + len]) continue;
    if (!(p[i + 2] & 1)) continue;
    if (f->need_trail && (i + 5 + (size_t)len >= n || p[i + 5 + len] != 0)) return;
    sa_process(f, p[i + 2], p + i + 4, len);
    i += 4 + (size_t)len;
  }
}

static fake_t *FK(vtx_io_t *io) { return io->ud; }
static int f_open(vtx_io_t *io, char *err, size_t n) { FK(io)->opened = true; return 0; }
static void f_close(vtx_io_t *io) { FK(io)->opened = false; FK(io)->low = false; }
static int f_set_line(vtx_io_t *io, int baud, int stop) { FK(io)->io_baud = baud; FK(io)->io_stop = stop; return 0; }
static int f_break(vtx_io_t *io, bool low) {
  fake_t *f = FK(io);
  if (f->low && !low) f->low_seen = true;
  f->low = low;
  return 0;
}
static int f_write(vtx_io_t *io, const uint8_t *p, size_t n) {
  fake_t *f = FK(io);
  f->writes++;
  int64_t tx_us = (int64_t)n * 11 * 1000000 / (f->io_baud ? f->io_baud : 4800);
  f->now += tx_us;                                       /* back when it is on the wire (tcdrain) */
  if (f->echo) {
    uint8_t e[64];
    size_t k = n < sizeof e ? n : sizeof e;
    memcpy(e, p, k);
    if (f->echo == 2 && k > 2) e[1] ^= 0x24;
    q_push(f, e, k, f->now);
  }
  fake_hear(f, p, n);
  return 0;
}
static int f_read(vtx_io_t *io, uint8_t *p, size_t n, int timeout_ms) {
  fake_t *f = FK(io);
  f->reads++;
  int64_t until = f->now + (int64_t)timeout_ms * 1000;
  if (f->qi >= f->qn || f->qt[f->qi] > until) { f->now = until; return 0; }
  if (f->qt[f->qi] > f->now) f->now = f->qt[f->qi];
  size_t k = 0;
  while (k < n && f->qi < f->qn && f->qt[f->qi] <= f->now) p[k++] = f->q[f->qi++];
  return (int)k;
}
static void f_flush(vtx_io_t *io) {
  fake_t *f = FK(io);
  while (f->qi < f->qn && f->qt[f->qi] <= f->now) f->qi++;
}
static void f_sleep(vtx_io_t *io, int64_t us) { FK(io)->now += us; }
static int64_t f_now(vtx_io_t *io) { return FK(io)->now; }

static vtx_io_t IO;
static void io_for(fake_t *f) {
  memset(&IO, 0, sizeof IO);
  IO.open = f_open; IO.close = f_close; IO.set_line = f_set_line; IO.write = f_write; IO.read = f_read;
  IO.flush = f_flush; IO.set_break = f_break; IO.sleep_us = f_sleep; IO.now_us = f_now; IO.ud = f;
  snprintf(IO.desc, sizeof IO.desc, "fake");
}

static vtx_config_t CFG;
static void cfg(const char *settings_text) {
  vtx_config_defaults(&CFG);
  settings_t *s = settings_parse(settings_text, strlen(settings_text));
  vtx_config_from_settings(&CFG, s);
  settings_free(s);
}

/* run the engine for ms of virtual time */
static vtx_status_t ST;
static void run(vtx_engine_t *e, fake_t *f, int ms) {
  int64_t end = f->now + (int64_t)ms * 1000;
  while (f->now < end) {
    int w = vtx_engine_step(e);
    if (w < 1) w = 1;
    int64_t until = f->now + (int64_t)w * 1000;
    f->now = until < end ? until : end;
  }
  vtx_engine_status(e, &ST);
}

static vtx_engine_t *engine(fake_t *f, const char *settings_text) {
  io_for(f);
  vtx_engine_t *e = vtx_engine_new();
  cfg(settings_text);
  vtx_engine_configure(e, &CFG);
  vtx_engine_set_io(e, &IO);
  return e;
}

static void test_engine_sa21(void) {
  static fake_t f;
  fake_init(&f, VTX_PROTO_SA, 3);
  vtx_engine_t *e = engine(&f, "vtx = auto\nvtx_band = R\nvtx_channel = 1\nvtx_power = 2\n");
  run(e, &f, 3000);
  CHECK_INT(ST.state, VTX_ST_READY);
  CHECK(ST.found && ST.in_sync);
  CHECK_INT(ST.proto, VTX_PROTO_SA);
  CHECK_INT(ST.sa_version, 3);
  CHECK_INT(ST.baud, 4800);
  CHECK_INT(f.channel, 32);                              /* R1 by channel number (factory band) */
  CHECK_INT(f.dbm, 20);                                  /* level 2 of 14/20/26 dBm, sent as dBm */
  CHECK_INT(count_took(&f, SA_SET_POWER, 0x80 | 20), 1);
  CHECK_INT(ST.freq, 5658);
  CHECK(ST.band == 4 && ST.channel == 1);
  CHECK(ST.power_level == 2 && ST.power_value == 20 && ST.power_mw == 100);
  CHECK(!ST.pit);
  CHECK(strstr(ST.message, "SmartAudio 2.1: R1 5658 MHz, 100 mW, pit mode off") != NULL);
  CHECK_INT(ST.table.npowers, 3);                        /* the default table from the VTX's levels */
  CHECK(ST.echo_ok > 0 && ST.echo_missing == 0 && ST.echo_bad == 0);
  CHECK(strstr(ST.line_desc, "a 0x00 before each frame") != NULL);
  char line[200];
  vtx_status_line(&ST, line, sizeof line);
  CHECK_STR(line, "SmartAudio 2.1 \xC2\xB7 R1 5658 MHz \xC2\xB7 100 mW");

  /* someone presses the VTX's button: the Pi sets it back */
  int before = count_took(&f, SA_SET_CHANNEL, 32);
  f.channel = 5; f.freq = SA40[5];
  f.dbm = 26;
  run(e, &f, 4000);
  CHECK_INT(f.channel, 32);
  CHECK_INT(f.dbm, 20);
  CHECK_INT(count_took(&f, SA_SET_CHANNEL, 32), before + 1);
  CHECK_INT(ST.state, VTX_ST_READY);

  /* pit mode on: 0 dBm (2.1, no boot flag); off: SET_MODE quit + unlock */
  cfg("vtx = auto\nvtx_band = R\nvtx_channel = 1\nvtx_power = 2\nvtx_pit = on\n");
  vtx_engine_configure(e, &CFG);
  run(e, &f, 2000);
  CHECK(f.mode & SA_MODE_PIT);
  CHECK_INT(count_took(&f, SA_SET_POWER, 0x80), 1);
  CHECK(ST.pit && ST.in_sync);
  CHECK(strstr(ST.message, "pit mode on") != NULL);
  int pw = count_took(&f, SA_SET_POWER, -1);
  run(e, &f, 5000);
  CHECK_INT(count_took(&f, SA_SET_POWER, -1), pw);       /* no power command while in pit mode */
  cfg("vtx = auto\nvtx_band = R\nvtx_channel = 1\nvtx_power = 2\nvtx_pit = off\n");
  vtx_engine_configure(e, &CFG);
  run(e, &f, 2000);
  CHECK(!(f.mode & SA_MODE_PIT));
  CHECK_INT(count_took(&f, SA_SET_MODE, SA_SET_PIT_OFF | SA_SET_UNLOCK), 1);

  /* a frequency instead of band/channel */
  cfg("vtx = auto\nvtx_band = R\nvtx_freq = 5800\n");
  vtx_engine_configure(e, &CFG);
  run(e, &f, 2000);
  CHECK_INT(f.freq, 5800);
  CHECK(f.mode & SA_MODE_FREQ);
  CHECK(ST.freq == 5800 && ST.freq_mode);
  CHECK(ST.band == 3 && ST.channel == 4);                /* F4 in the table */

  /* unplugged: searched for again, "not found"; plugged back: set again */
  f.plugged = false;
  run(e, &f, 30000);
  CHECK_INT(ST.state, VTX_ST_NOT_FOUND);
  CHECK(!ST.found);
  CHECK(strstr(ST.message, "No answer from the VTX") != NULL);
  f.plugged = true;
  f.freq = 5865; f.mode = 0x10; f.channel = 0;           /* it forgot (power cycle) */
  run(e, &f, 30000);
  CHECK_INT(ST.state, VTX_ST_READY);
  CHECK_INT(f.freq, 5800);
  vtx_engine_free(e);
}

static void test_engine_sa_v1_v2(void) {
  static fake_t f;
  /* v1: DAC values, no pit mode */
  fake_init(&f, VTX_PROTO_SA, 1);
  f.openvtx_len = true;                                  /* and the other reply length convention */
  vtx_engine_t *e = engine(&f, "vtx = smartaudio\nvtx_band = A\nvtx_channel = 8\nvtx_power = 3\nvtx_pit = on\n");
  run(e, &f, 3000);
  CHECK_INT(ST.state, VTX_ST_READY);
  CHECK_INT(ST.sa_version, 1);
  CHECK_INT(f.channel, 7);
  CHECK_INT(f.power, 25);                                /* level 3 = DAC 25 (500 mW) */
  CHECK_INT(ST.power_mw, 500);
  CHECK_INT(count_took(&f, SA_SET_MODE, -1), 0);
  CHECK(strstr(ST.error, "no pit mode") != NULL);
  vtx_engine_free(e);

  /* v2: levels 0..3; pit mode on only sets the flag (from the next power-up) */
  fake_init(&f, VTX_PROTO_SA, 2);
  e = engine(&f, "vtx = auto\nvtx_band = E\nvtx_channel = 2\nvtx_power = 4\nvtx_pit = on\n");
  run(e, &f, 3000);
  CHECK_INT(ST.state, VTX_ST_READY);
  CHECK_INT(ST.sa_version, 2);
  CHECK_INT(f.channel, 17);
  CHECK_INT(f.power, 3);
  CHECK_INT(count_took(&f, SA_SET_MODE, SA_SET_PIR | SA_SET_UNLOCK), 1);
  CHECK(f.mode & SA_MODE_PIR);
  CHECK(!(f.mode & SA_MODE_PIT));
  CHECK(ST.pit_at_boot && !ST.pit && ST.in_sync);
  CHECK(strstr(ST.message, "pit mode at the next power-up") != NULL);
  /* it boots in pit mode; pit off: quit and clear the flags */
  f.mode |= SA_MODE_PIT;
  cfg("vtx = auto\nvtx_band = E\nvtx_channel = 2\nvtx_power = 4\n");
  vtx_engine_configure(e, &CFG);
  run(e, &f, 3000);
  CHECK(!(f.mode & (SA_MODE_PIT | SA_MODE_PIR)));
  /* a power level the table doesn't have */
  cfg("vtx = auto\nvtx_power = 6\n");
  vtx_engine_configure(e, &CFG);
  run(e, &f, 3000);
  CHECK_INT(ST.state, VTX_ST_ERROR);
  CHECK(strstr(ST.error, "Power level 6 isn't in the VTX table") != NULL);
  /* a band the table doesn't have */
  cfg("vtx = auto\nvtx_band = L\n");
  vtx_engine_configure(e, &CFG);
  run(e, &f, 3000);
  CHECK_INT(ST.state, VTX_ST_ERROR);
  CHECK(strstr(ST.error, "Band L isn't in the VTX table") != NULL);
  /* the VTX doesn't take the power: an error after some tries; a button press: tried again */
  f.ignore_power = true;
  cfg("vtx = auto\nvtx_power = 2\n");
  vtx_engine_configure(e, &CFG);
  run(e, &f, 8000);
  CHECK_INT(ST.state, VTX_ST_ERROR);
  CHECK(strstr(ST.error, "didn't take the power") != NULL);
  int tries = count_took(&f, SA_SET_POWER, 1);
  CHECK(tries >= 3 && tries <= 6);
  run(e, &f, 10000);
  CHECK_INT(count_took(&f, SA_SET_POWER, 1), tries);     /* it waits */
  f.ignore_power = false;
  f.power = 2;                                           /* someone pressed the button */
  run(e, &f, 5000);
  CHECK_INT(f.power, 1);
  CHECK_INT(ST.state, VTX_ST_READY);
  vtx_engine_free(e);
}

static void test_engine_tramp(void) {
  static fake_t f;
  fake_init(&f, VTX_PROTO_TRAMP, 0);
  vtx_engine_t *e = engine(&f, "vtx = auto\nvtx_band = R\nvtx_channel = 3\nvtx_power = 3\nvtx_pit = on\n");
  run(e, &f, 8000);
  CHECK_INT(ST.state, VTX_ST_READY);
  CHECK_INT(ST.proto, VTX_PROTO_TRAMP);
  CHECK_INT(ST.baud, 9600);
  CHECK_INT(f.tfreq, 5732);                              /* Tramp: always by frequency */
  CHECK_INT(f.tpower, 200);                              /* level 3 of 25/100/200/400/600 mW */
  CHECK_INT(f.tpit, 1);
  CHECK_INT(count_took(&f, 'I', 0), 1);                  /* 0 = pit on */
  CHECK(ST.min_freq == 5600 && ST.max_freq == 5950 && ST.max_power == 600);
  CHECK(ST.have_temp && ST.temp_c == 31);
  CHECK(strstr(ST.message, "Tramp: R3 5732 MHz, 200 mW, pit mode on") != NULL);
  /* outside its limits, more than its power */
  cfg("vtx = tramp\nvtx_freq = 5990\nvtx_power = 5\n");
  vtx_engine_configure(e, &CFG);
  f.tmaxp = 400;
  run(e, &f, 5000);
  CHECK_INT(ST.state, VTX_ST_ERROR);
  CHECK(strstr(ST.error, "5600 to 5950") != NULL);
  cfg("vtx = tramp\nvtx_power = 5\n");
  vtx_engine_configure(e, &CFG);
  run(e, &f, 5000);
  CHECK(strstr(ST.error, "more than this VTX's 400 mW") != NULL);
  /* race lock: frequency and power can't change */
  f.tcontrol = 1;
  cfg("vtx = tramp\nvtx_band = F\nvtx_channel = 1\n");
  vtx_engine_configure(e, &CFG);
  run(e, &f, 5000);
  CHECK_INT(ST.state, VTX_ST_ERROR);
  CHECK(strstr(ST.message, "race-locked") != NULL);
  CHECK(ST.locked);
  f.tcontrol = 0;
  run(e, &f, 5000);
  CHECK_INT(f.tfreq, 5740);
  CHECK_INT(ST.state, VTX_ST_READY);
  /* a slow clock: found at another baud rate */
  vtx_engine_free(e);
  fake_init(&f, VTX_PROTO_TRAMP, 0);
  f.baud = 9400;
  e = engine(&f, "vtx = tramp\nvtx_power = 1\n");
  run(e, &f, 10000);
  CHECK_INT(ST.state, VTX_ST_READY);
  CHECK_INT(ST.baud, 9400);
  vtx_engine_free(e);
}

static void variant(fake_t *f, const char *what, int expect_baud, const char *line_has) {
  vtx_engine_t *e = engine(f, "vtx = auto\nvtx_band = F\nvtx_channel = 4\n");
  run(e, f, 60000);
  if (ST.state != VTX_ST_READY) fprintf(stderr, "variant %s: %s (%s)\n", what, ST.message, ST.line_desc);
  CHECK_INT(ST.state, VTX_ST_READY);
  CHECK_INT(f->channel, 27);
  CHECK_INT(ST.baud, expect_baud);
  if (!strstr(ST.line_desc, line_has)) fprintf(stderr, "variant %s: line \"%s\"\n", what, ST.line_desc);
  CHECK(strstr(ST.line_desc, line_has) != NULL);
  vtx_engine_free(e);
}

static void test_engine_variants(void) {
  static fake_t f;
  fake_init(&f, VTX_PROTO_SA, 3);
  f.need_low = true;                                     /* wants the line low before a frame, no 0x00 */
  variant(&f, "low", 4800, "line low between frames");
  fake_init(&f, VTX_PROTO_SA, 2);
  f.need_trail = true;                                   /* AKK/RDQ clone */
  variant(&f, "trail", 4800, "after it (clone)");
  fake_init(&f, VTX_PROTO_SA, 3);
  f.need_stop = 1;                                       /* TBS Sixty9 */
  variant(&f, "1 stop", 4800, "8N1");
  fake_init(&f, VTX_PROTO_SA, 2);
  f.baud = 4900;                                         /* a warm Unify */
  variant(&f, "4900", 4900, "4900 baud");
  fake_init(&f, VTX_PROTO_SA, 2);
  f.baud = 4600;
  variant(&f, "4600", 4600, "4600 baud");
  /* it remembers the variant that worked: found fast after an unplug */
  fake_init(&f, VTX_PROTO_SA, 2);
  f.baud = 4700;
  vtx_engine_t *e = engine(&f, "vtx = auto\n");
  run(e, &f, 60000);
  CHECK_INT(ST.state, VTX_ST_READY);
  f.plugged = false;
  run(e, &f, 10000);
  CHECK(ST.state == VTX_ST_SEARCHING || ST.state == VTX_ST_NOT_FOUND);
  f.plugged = true;
  run(e, &f, 1500);
  CHECK_INT(ST.state, VTX_ST_READY);
  vtx_engine_free(e);
}

static void test_engine_echo(void) {
  static fake_t f;
  /* no RX wire: nothing comes back at all */
  fake_init(&f, VTX_PROTO_SA, 2);
  f.echo = 0;
  f.plugged = false;
  vtx_engine_t *e = engine(&f, "vtx = auto\n");
  run(e, &f, 30000);
  CHECK_INT(ST.state, VTX_ST_NOT_FOUND);
  CHECK(strstr(ST.message, "Nothing comes back on RX") != NULL);
  CHECK(ST.echo_missing > 0 && ST.echo_ok == 0);
  vtx_engine_free(e);
  /* the replies come without our echo (an odd wiring): still works */
  fake_init(&f, VTX_PROTO_SA, 2);
  f.echo = 0;
  e = engine(&f, "vtx = auto\nvtx_band = B\nvtx_channel = 1\n");
  run(e, &f, 5000);
  CHECK_INT(ST.state, VTX_ST_READY);
  CHECK_INT(f.channel, 8);
  vtx_engine_free(e);
  /* garbled echo, no VTX */
  fake_init(&f, VTX_PROTO_SA, 2);
  f.echo = 2;
  f.plugged = false;
  e = engine(&f, "vtx = smartaudio\n");
  run(e, &f, 30000);
  CHECK_INT(ST.state, VTX_ST_NOT_FOUND);
  CHECK(strstr(ST.message, "garbled") != NULL);
  CHECK(ST.echo_bad > 0);
  vtx_engine_free(e);
  /* garbled echo, but the VTX answers: the replies are still read */
  fake_init(&f, VTX_PROTO_SA, 3);
  f.echo = 2;
  e = engine(&f, "vtx = smartaudio\nvtx_power = 3\n");
  run(e, &f, 5000);
  CHECK_INT(ST.state, VTX_ST_READY);
  CHECK_INT(f.dbm, 26);
  vtx_engine_free(e);
  /* the wrong protocol forced: never found, says why */
  fake_init(&f, VTX_PROTO_TRAMP, 0);
  e = engine(&f, "vtx = smartaudio\n");
  run(e, &f, 30000);
  CHECK_INT(ST.state, VTX_ST_NOT_FOUND);
  CHECK(strstr(ST.message, "tried SmartAudio)") != NULL);
  vtx_engine_free(e);
}

static void test_engine_send_only(void) {
  static fake_t f;
  fake_init(&f, VTX_PROTO_SA, 2);
  vtx_engine_t *e = engine(&f, "vtx = smartaudio\nvtx_send_only = on\nvtx_band = R\nvtx_channel = 8\nvtx_power = 2\n");
  run(e, &f, 5000);
  CHECK_INT(ST.state, VTX_ST_SEND_ONLY);
  CHECK_INT(f.reads, 0);                                 /* nothing is read */
  CHECK_INT(f.channel, 39);
  CHECK_INT(f.power, 1);
  CHECK_INT(count_took(&f, SA_SET_CHANNEL, 39), 2);      /* two rounds */
  CHECK_INT(count_took(&f, SA_SET_MODE, SA_SET_PIT_OFF | SA_SET_UNLOCK), 2);
  CHECK_INT(count_took(&f, SA_GET_SETTINGS, -1), 0);
  CHECK(strstr(ST.message, "can't show what it is set to") != NULL);
  CHECK(!ST.found);
  run(e, &f, 60000);
  CHECK_INT(count_took(&f, SA_SET_CHANNEL, 39), 2);      /* only after a change */
  cfg("vtx = smartaudio\nvtx_send_only = on\nvtx_band = R\nvtx_channel = 7\nvtx_power = 2\n");
  vtx_engine_configure(e, &CFG);
  run(e, &f, 3000);
  CHECK_INT(f.channel, 38);
  vtx_engine_free(e);
  /* Tramp send-only; a dBm table file makes SmartAudio send dBm */
  fake_init(&f, VTX_PROTO_TRAMP, 0);
  e = engine(&f, "vtx = tramp\nvtx_send_only = on\nvtx_freq = 5800\nvtx_power = 2\nvtx_pit = on\n");
  run(e, &f, 5000);
  CHECK_INT(f.tfreq, 5800);
  CHECK_INT(f.tpower, 100);
  CHECK_INT(f.tpit, 1);
  CHECK_INT(f.reads, 0);
  vtx_engine_free(e);
  /* auto can't be send-only */
  fake_init(&f, VTX_PROTO_SA, 2);
  e = engine(&f, "vtx = auto\nvtx_send_only = on\n");
  run(e, &f, 3000);
  CHECK_INT(ST.state, VTX_ST_ERROR);
  CHECK(strstr(ST.message, "set VTX control to SmartAudio or Tramp") != NULL);
  CHECK_INT(f.writes, 0);
  vtx_engine_free(e);
}

static void test_engine_table_file(void) {
  static fake_t f;
  fake_init(&f, VTX_PROTO_SA, 3);
  vtx_engine_t *e = engine(&f, "vtx = auto\nvtx_band = L\nvtx_channel = 2\nvtx_power = 3\n");
  CFG.have_table = parse(GOOD_JSON, &CFG.table, (char[200]){ 0 }) == 0;
  CHECK(CFG.have_table);
  vtx_engine_configure(e, &CFG);
  run(e, &f, 3000);
  CHECK_INT(ST.state, VTX_ST_READY);
  CHECK_INT(f.freq, 5399);                               /* L is not a factory band: by frequency */
  CHECK(f.mode & SA_MODE_FREQ);
  CHECK_INT(f.dbm, 26);
  CHECK(ST.band == 2 && ST.channel == 2);
  CHECK_STR(ST.table.description, CFG.table.description);
  /* a channel that is 0 in the table */
  cfg("vtx = auto\nvtx_band = E\nvtx_channel = 4\n");
  CFG.have_table = parse(GOOD_JSON, &CFG.table, (char[200]){ 0 }) == 0;
  vtx_engine_configure(e, &CFG);
  run(e, &f, 3000);
  CHECK_INT(ST.state, VTX_ST_ERROR);
  CHECK(strstr(ST.error, "no channel 4") != NULL);
  /* a broken file: the built-in table and an error */
  cfg("vtx = auto\n");
  snprintf(CFG.table_error, sizeof CFG.table_error, "vtxtable.json can't be used: broken.");
  vtx_engine_configure(e, &CFG);
  run(e, &f, 3000);
  CHECK_INT(ST.state, VTX_ST_ERROR);
  CHECK(strstr(ST.error, "vtxtable.json") != NULL);
  CHECK_INT(ST.table.nbands, 5);
  /* off */
  cfg("vtx = off\n");
  vtx_engine_configure(e, &CFG);
  run(e, &f, 1000);
  CHECK_INT(ST.state, VTX_ST_OFF);
  CHECK(!f.opened);
  vtx_engine_free(e);
  /* no line at all */
  e = vtx_engine_new();
  cfg("vtx = auto\nvtx_port = uart5\n");
  vtx_engine_configure(e, &CFG);
  vtx_engine_step(e);
  vtx_engine_status(e, &ST);
  CHECK_INT(ST.state, VTX_ST_ERROR);
  vtx_engine_free(e);
}

static void test_config(void) {
  cfg("vtx = Tramp\nvtx_port = uart9\nvtx_band = b\nvtx_channel = 12\nvtx_freq = 5800\nvtx_power = 3\nvtx_pit = yes\nvtx_send_only = 1\n");
  CHECK_INT(CFG.mode, VTX_MODE_TRAMP);
  CHECK_STR(CFG.port, "gpio4");                          /* an unknown port: the default (one pin) */
  CHECK_INT(CFG.band, 'B');
  CHECK_INT(CFG.channel, 1);                             /* invalid: the default */
  CHECK_INT(CFG.freq, 5800);
  CHECK_INT(CFG.power, 3);
  CHECK(CFG.pit && CFG.send_only);
  cfg("");
  CHECK_INT(CFG.mode, VTX_MODE_OFF);
  CHECK_INT(CFG.band, 0);
  CHECK_STR(vtx_mode_name(VTX_MODE_SA), "smartaudio");
  CHECK_INT(vtx_mode_parse("tramp"), VTX_MODE_TRAMP);
  CHECK_STR(vtx_state_name(VTX_ST_NOT_FOUND), "not_found");
  CHECK_STR(vtx_uart_node("uart3"), "serial@7e201600");
  CHECK_STR(vtx_uart_node("uart5"), "serial@7e201a00");
  CHECK(vtx_uart_node("uart0") == NULL);
  char err[200];
  vtx_io_t *io = vtx_io_new("tcp:127.0.0.1:5000", err, sizeof err);
  CHECK(io != NULL);
  vtx_io_free(io);
  CHECK(vtx_io_new("tcp:nohost", err, sizeof err) == NULL);
  CHECK(vtx_io_new("com3", err, sizeof err) == NULL);
  CHECK(strstr(err, "isn't a VTX port") != NULL);
  vtx_status_t s;
  vtx_get_status(NULL, &s);
  CHECK_INT(s.state, VTX_ST_OFF);
  char line[64];
  vtx_status_line(&s, line, sizeof line);
  CHECK_STR(line, "off");
}

#ifdef __linux__
/* the UART found by its device-tree node, whatever its ttyAMA number */
static void test_find_uart(void) {
  char dir[] = "/tmp/stp-vtx-sysXXXXXX";
  if (!mkdtemp(dir)) { CHECK(0); return; }
  char p[512], q[512];
  static const char *const tty[] = { "ttyAMA0", "ttyAMA1", "ttyAMA4" };
  static const char *const node[] = { "serial@7e201000", "serial@7e201800", "serial@7e201600" };
  for (int i = 0; i < 3; i++) {
    snprintf(p, sizeof p, "%s/%s", dir, tty[i]); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/%s/device", dir, tty[i]); mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/%s/device/of_node", dir, tty[i]);
    snprintf(q, sizeof q, "../../../firmware/devicetree/base/soc/%s", node[i]);
    CHECK_INT(symlink(q, p), 0);
  }
  /* a tty without of_node, found by its platform device name */
  snprintf(p, sizeof p, "%s/ttyAMA5", dir); mkdir(p, 0755);
  snprintf(p, sizeof p, "%s/ttyAMA5/device", dir);
  CHECK_INT(symlink("../../devices/platform/soc/fe201a00.serial", p), 0);
  char name[64];
  CHECK_INT(vtx_find_uart_in(dir, "uart3", name, sizeof name), 0);
  CHECK_STR(name, "ttyAMA4");
  CHECK_INT(vtx_find_uart_in(dir, "uart4", name, sizeof name), 0);
  CHECK_STR(name, "ttyAMA1");
  CHECK_INT(vtx_find_uart_in(dir, "uart5", name, sizeof name), 0);
  CHECK_STR(name, "ttyAMA5");
  CHECK_INT(vtx_find_uart_in(dir, "uart2", name, sizeof name), -1);
  CHECK_INT(vtx_find_uart_in("/nonexistent", "uart3", name, sizeof name), -1);
  char cmd[600];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
  CHECK_INT(system(cmd), 0);
}

/* the real tty line (termios raw, 8N2/8N1, a custom baud rate, break) on a pty pair */
struct t_termios2 { unsigned int c_iflag, c_oflag, c_cflag, c_lflag; unsigned char c_line, c_cc[19]; unsigned int c_ispeed, c_ospeed; };
static void test_tty_line(void) {
  int m = posix_openpt(O_RDWR | O_NOCTTY);
  if (m < 0 || grantpt(m) || unlockpt(m)) { CHECK(0); return; }
  char spec[300];
  snprintf(spec, sizeof spec, "tty:%s", ptsname(m));
  char err[200] = "";
  vtx_io_t *io = vtx_io_new(spec, err, sizeof err);
  CHECK(io != NULL);
  if (!io) { close(m); return; }
  CHECK_INT(io->open(io, err, sizeof err), 0);
  CHECK_INT(io->set_line(io, 4800, 2), 0);
  /* look at the line's settings from outside: termios ioctls on the master act on the slave (tty_mode_ioctl),
   * and opening the slave again would fail for non-root users (the transport holds TIOCEXCL) */
  int s = dup(m);
  struct termios tio;
  if (s >= 0 && tcgetattr(s, &tio) == 0) {
    CHECK(tio.c_cflag & CSTOPB);
    CHECK((tio.c_cflag & CSIZE) == CS8);
    CHECK(!(tio.c_cflag & PARENB));
    CHECK(!(tio.c_lflag & (ICANON | ECHO)));
    CHECK_INT(cfgetospeed(&tio), B4800);
  } else CHECK(0);
  struct t_termios2 t2;
  if (io->set_line(io, 4900, 2) != 0) {                 /* termios2 BOTHER: the engine skips that variant without */
    printf("note: no custom baud rates (TCSETS2) here (qemu-user?): not checked\n");
  } else if (s >= 0 && ioctl(s, _IOR('T', 0x2A, struct t_termios2), &t2) == 0) {
    CHECK_INT(t2.c_ospeed, 4900);
    CHECK_INT(t2.c_cflag & 0010017, 0010000);
  } else CHECK(0);
  CHECK_INT(io->set_line(io, 9600, 1), 0);
  if (s >= 0 && tcgetattr(s, &tio) == 0) {
    CHECK(!(tio.c_cflag & CSTOPB));
    CHECK_INT(cfgetospeed(&tio), B9600);
  }
  if (s >= 0) close(s);
  uint8_t out[5] = { 0x00, 0xAA, 0x55, 0x03, 0x00 }, buf[16];
  CHECK_INT(io->write(io, out, 5), 0);
  ssize_t k = read(m, buf, sizeof buf);
  CHECK_INT(k, 5);
  CHECK(k == 5 && !memcmp(buf, out, 5));               /* raw: no translation of 0x00 or anything */
  CHECK_INT(io->read(io, buf, sizeof buf, 50), 0);       /* nothing yet: a timeout */
  uint8_t in[3] = { 0x0D, 0x0A, 0x11 };                  /* CR, LF, XON pass unchanged */
  CHECK_INT(write(m, in, 3), 3);
  k = io->read(io, buf, sizeof buf, 500);
  CHECK_INT(k, 3);
  CHECK(k == 3 && !memcmp(buf, in, 3));
  CHECK_INT(write(m, in, 3), 3);
  plat_sleep_us(50000);
  io->flush(io);
  CHECK_INT(io->read(io, buf, sizeof buf, 50), 0);       /* flushed */
  if (io->set_break(io, true) != 0) {                   /* qemu-user doesn't pass TIOCSBRK on */
    printf("note: no line break (TIOCSBRK) here (qemu-user?): not checked\n");
  } else {
    CHECK_INT(io->set_break(io, false), 0);
  }
  io->close(io);
  vtx_io_free(io);
  close(m);
  /* a missing tty, a missing UART */
  io = vtx_io_new("tty:/dev/nonexistent-tty", err, sizeof err);
  CHECK(io && io->open(io, err, sizeof err) == -2);
  vtx_io_free(io);
  io = vtx_io_new("uart4", err, sizeof err);
  CHECK(io != NULL);
  char path[64];
  if (io && vtx_find_uart("uart4", path, sizeof path) != 0) {   /* (not on a Pi with uart4 on) */
    CHECK_INT(io->open(io, err, sizeof err), -2);
    CHECK(strstr(err, "Reboot once") != NULL);
  }
  vtx_io_free(io);
}
#endif

int main(void) {
  test_sa_frames();
  test_sa_parser();
  test_tramp();
  test_units();
  test_soft_schedule();
  test_soft_decoder();
  test_tables();
  test_config();
  test_engine_sa21();
  test_engine_sa_v1_v2();
  test_engine_tramp();
  test_engine_variants();
  test_engine_echo();
  test_engine_send_only();
  test_engine_table_file();
#ifdef __linux__
  test_find_uart();
  test_tty_line();
#endif
  T_DONE();
}
