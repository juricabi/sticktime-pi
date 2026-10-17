/* SmartAudio and Tramp frames: see vtx_proto.h. Written from the TBS SmartAudio spec rev 09 and the
 * protocol facts in docs/RESEARCH.md (VTX); no code was taken from other projects. */
#include "vtx_proto.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ================================================================ SmartAudio */
uint8_t vtx_sa_crc8(const uint8_t *p, size_t n) {
  uint8_t crc = 0;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int b = 0; b < 8; b++) crc = (uint8_t)(crc & 0x80 ? (crc << 1) ^ 0xD5 : crc << 1);
  }
  return crc;
}

size_t vtx_sa_frame(uint8_t *out, int cmd, const uint8_t *payload, size_t n) {
  out[0] = 0xAA;
  out[1] = 0x55;
  out[2] = (uint8_t)((cmd << 1) | 1);
  out[3] = (uint8_t)n;
  if (n) memcpy(out + 4, payload, n);
  out[4 + n] = vtx_sa_crc8(out, 4 + n);
  return 5 + n;
}

size_t vtx_sa_get_settings(uint8_t *out) { return vtx_sa_frame(out, SA_GET_SETTINGS, NULL, 0); }
size_t vtx_sa_set_power(uint8_t *out, uint8_t value) { return vtx_sa_frame(out, SA_SET_POWER, &value, 1); }
size_t vtx_sa_set_channel(uint8_t *out, uint8_t index) { return vtx_sa_frame(out, SA_SET_CHANNEL, &index, 1); }
size_t vtx_sa_set_mode(uint8_t *out, uint8_t mode) { return vtx_sa_frame(out, SA_SET_MODE, &mode, 1); }

size_t vtx_sa_set_freq(uint8_t *out, uint16_t f) {
  uint8_t p[2] = { (uint8_t)(f >> 8), (uint8_t)f };     /* big-endian, like the spec's 0x16 0xE9 = 5865 */
  return vtx_sa_frame(out, SA_SET_FREQ, p, 2);
}

void vtx_sa_parser_reset(vtx_sa_parser_t *p) { memset(p, 0, sizeof *p); }

static void sa_shift(vtx_sa_parser_t *p, int k) {
  if (k >= p->n) { p->n = 0; return; }
  memmove(p->buf, p->buf + k, (size_t)(p->n - k));
  p->n -= k;
}

#define SA_MAX_LEN 32                         /* a longer length byte is garbage */

static void sa_take(vtx_sa_frame_t *f, const uint8_t *buf, int n, bool host) {
  memset(f, 0, sizeof *f);
  f->cmd = buf[2];
  f->len = buf[3];
  f->host = host;
  f->n = (uint8_t)n;
  memcpy(f->payload, buf + 4, (size_t)n);
}

/* With len = the payload, a frame must have the payload its command has: an 8-bit CRC passes by chance
 * one time in 256 (a damaged reply followed by the next frame's AA, say), and the length check makes
 * such a false frame much less likely. (The spec's own convention, len = payload + crc, is taken as is.) */
static bool sa_plausible(uint8_t cmd, int n, bool host) {
  if (host) {
    switch (cmd >> 1) {
      case SA_GET_SETTINGS: return n == 0;
      case SA_SET_POWER: case SA_SET_CHANNEL: case SA_SET_MODE: return n == 1;
      case SA_SET_FREQ: return n == 2;
      default: return true;
    }
  }
  switch (cmd) {
    case SA_GET_SETTINGS: case SA_SETTINGS_V2: return n == 5;
    case SA_SETTINGS_V21: return n >= 7;
    case SA_SET_POWER: case SA_SET_CHANNEL: case SA_SET_MODE: return n == 2;
    case SA_SET_FREQ: return n == 3;
    default: return true;
  }
}

/* find a frame in what is buffered; 1 when one was taken out */
static int sa_parse(vtx_sa_parser_t *p, vtx_sa_frame_t *f) {
  for (;;) {
    if (p->n == 0) return 0;
    if (p->buf[0] != 0xAA) { p->skipped++; sa_shift(p, 1); continue; }
    if (p->n < 2) return 0;
    if (p->buf[1] != 0x55) { p->skipped++; sa_shift(p, 1); continue; }
    if (p->n < 4) return 0;
    int L = p->buf[3];
    if (L > SA_MAX_LEN) { p->bad_len++; sa_shift(p, 1); continue; }
    /* the spec's replies: len counts the payload and the crc, crc over cmd + len + payload */
    if (L >= 1 && p->n >= 4 + L && vtx_sa_crc8(p->buf + 2, (size_t)L + 1) == p->buf[3 + L]) {
      sa_take(f, p->buf, L - 1, false);
      sa_shift(p, 4 + L);
      return 1;
    }
    if (p->n < 5 + L) return 0;
    /* len = the payload alone: a reply (crc without AA 55), or a host frame (crc over everything) */
    if (vtx_sa_crc8(p->buf + 2, (size_t)L + 2) == p->buf[4 + L] && sa_plausible(p->buf[2], L, false)) {
      sa_take(f, p->buf, L, false);
      sa_shift(p, 5 + L);
      return 1;
    }
    if (vtx_sa_crc8(p->buf, (size_t)L + 4) == p->buf[4 + L] && sa_plausible(p->buf[2], L, true)) {
      sa_take(f, p->buf, L, true);
      sa_shift(p, 5 + L);
      return 1;
    }
    p->bad_crc++;
    sa_shift(p, 1);                           /* maybe the frame starts later: look again */
  }
}

int vtx_sa_parser_feed(vtx_sa_parser_t *p, uint8_t c, vtx_sa_frame_t *f) {
  if (p->n >= (int)sizeof p->buf) sa_shift(p, 1);   /* (can't happen: L <= 32 decides by 37 bytes) */
  p->buf[p->n++] = c;
  return sa_parse(p, f);
}

int vtx_sa_decode_settings(const vtx_sa_frame_t *f, vtx_sa_settings_t *s) {
  memset(s, 0, sizeof *s);
  s->dbm = -1;
  if (f->host) return -1;
  if (f->cmd == SA_GET_SETTINGS) s->version = 1;
  else if (f->cmd == SA_SETTINGS_V2) s->version = 2;
  else if (f->cmd == SA_SETTINGS_V21) s->version = 3;
  else return -1;
  if (f->n < 5) return -1;
  s->channel = f->payload[0];
  s->power = f->payload[1];
  s->mode = f->payload[2];
  s->freq = (uint16_t)((f->payload[3] << 8) | f->payload[4]);
  if (s->version == 3 && f->n >= 7) {
    s->dbm = f->payload[5];
    /* the count byte, then the levels: the spec's example says 3 and lists 4 (0, 14, 20, 26), the
     * 0 dBm one being pit mode; take what is there (count + 1 at most), without 0 and repeats */
    int count = f->payload[6], avail = f->n - 7;
    int take = avail < count + 1 ? avail : count + 1;
    for (int i = 0; i < take && s->nlevels < 8; i++) {
      uint8_t d = f->payload[7 + i];
      bool dup = false;
      for (int j = 0; j < s->nlevels; j++) dup |= s->levels[j] == d;
      if (d && !dup) s->levels[s->nlevels++] = d;
    }
  }
  return 0;
}

/* ================================================================ Tramp */
uint8_t vtx_tramp_checksum(const uint8_t f[TRAMP_FRAME]) {
  unsigned sum = 0;
  for (int i = 1; i < 14; i++) sum += f[i];
  return (uint8_t)sum;
}

void vtx_tramp_frame(uint8_t out[TRAMP_FRAME], char cmd, uint16_t param) {
  memset(out, 0, TRAMP_FRAME);
  out[0] = 0x0F;
  out[1] = (uint8_t)cmd;
  out[2] = (uint8_t)param;
  out[3] = (uint8_t)(param >> 8);
  out[14] = vtx_tramp_checksum(out);
}

void vtx_tramp_parser_reset(vtx_tramp_parser_t *p) { memset(p, 0, sizeof *p); }

int vtx_tramp_parser_feed(vtx_tramp_parser_t *p, uint8_t c, uint8_t out[TRAMP_FRAME]) {
  p->buf[p->n++] = c;
  for (;;) {
    int k = 0;
    while (k < p->n && p->buf[k] != 0x0F) k++;
    if (k) {
      p->skipped += (uint32_t)k;
      memmove(p->buf, p->buf + k, (size_t)(p->n - k));
      p->n -= k;
    }
    if (p->n < TRAMP_FRAME) return 0;
    if (vtx_tramp_checksum(p->buf) == p->buf[14] && p->buf[15] == 0) {
      memcpy(out, p->buf, TRAMP_FRAME);
      p->n = 0;
      return 1;
    }
    p->bad_sum++;
    memmove(p->buf, p->buf + 1, TRAMP_FRAME - 1);   /* look for a later 0x0F */
    p->n = TRAMP_FRAME - 1;
  }
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

int vtx_tramp_decode(const uint8_t f[TRAMP_FRAME], vtx_tramp_reply_t *r) {
  memset(r, 0, sizeof *r);
  r->cmd = (char)f[1];
  switch (f[1]) {
    case 'r':
      r->min_freq = le16(f + 2);
      r->max_freq = le16(f + 4);
      r->max_power = le16(f + 6);
      return r->min_freq ? 0 : -1;            /* 0: our own query coming back */
    case 'v':
      r->freq = le16(f + 2);
      r->power = le16(f + 4);
      r->control = f[6];
      r->pit = f[7];
      r->act_power = le16(f + 8);
      return r->freq ? 0 : -1;
    case 's':
      r->temp = (int16_t)le16(f + 6);
      return 0;
    default:
      return -1;
  }
}

/* ================================================================ power units */
int vtx_dbm_to_mw(int dbm) {
  if (dbm <= 0) return dbm == 0 ? 1 : 0;
  double mw = pow(10.0, dbm / 10.0);
  if (mw > 30) return (int)(floor(mw / 50.0 + 0.5) * 50);    /* 23 dBm -> 200, 29 -> 800 */
  return (int)floor(mw + 0.5);                                /* 14 dBm -> 25 */
}

void vtx_mw_label(char *out, size_t n, int mw) {
  if (mw < 1000) snprintf(out, n, "%d", mw < 0 ? 0 : mw);
  else if (mw % 1000 == 0 || mw >= 10000) snprintf(out, n, "%dW", (mw + 500) / 1000);
  else snprintf(out, n, "%dW%d", mw / 1000, (mw % 1000 + 50) / 100 % 10);
}
