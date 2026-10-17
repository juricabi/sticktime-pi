/* Stick sources and the mixer: see input.h. */
#include "input.h"

#include <string.h>

#define MOVED_DELTA 51              /* ~5% of a full deflection */
#define TAKEOVER_US 1000000         /* another source must have moved this much more recently */

static const char *const kind_names[INSRC_KINDS] = {
  "none", "keyboard", "wifi", "usb", "bluetooth", "gamepad", "joystick"
};

const char *insrc_kind_name(insrc_kind_t k) {
  return k >= 0 && k < INSRC_KINDS ? kind_names[k] : "none";
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static bool is_alpha(char c) { c = lower(c); return c >= 'a' && c <= 'z'; }

static bool ieq(const char *a, const char *b) {
  while (*a && lower(*a) == lower(*b)) a++, b++;
  return lower(*a) == lower(*b);
}

static int clamp_ch(int v) { return v < -1024 ? -1024 : v > 1024 ? 1024 : v; }

/* On failure out is "AETR", so it is always usable. */
bool mix_parse_order(const char *s, char out[5]) {
  char o[5];
  int seen = 0;
  for (int i = 0; s && i < 4; i++) {
    char c = lower(s[i]);
    int bit = c == 'a' ? 1 : c == 'e' ? 2 : c == 't' ? 4 : c == 'r' ? 8 : 0;
    if (!bit || (seen & bit)) break;
    seen |= bit;
    o[i] = (char)(c - 32);
  }
  if (seen != 15 || s[4]) {
    memcpy(out, "AETR", 5);
    return false;
  }
  o[4] = 0;
  memcpy(out, o, 5);
  return true;
}

/* Words of letters, anything else separates them. A word that isn't a stick's name but is up to
 * four of the letters a, e, t, r ("at", "ET") names those sticks; other words are ignored
 * (settings_validate refuses them). */
void mix_parse_invert(const char *s, bool inv[4]) {
  static const struct { const char *name; int stick; } names[] = {
    { "ail", 0 }, { "aileron", 0 }, { "ailerons", 0 }, { "roll", 0 },
    { "ele", 1 }, { "elevator", 1 }, { "pitch", 1 },
    { "thr", 2 }, { "throttle", 2 },
    { "rud", 3 }, { "rudder", 3 }, { "yaw", 3 },
  };
  for (int i = 0; i < 4; i++) inv[i] = false;
  for (const char *p = s; p && *p; ) {
    if (!is_alpha(*p)) { p++; continue; }
    const char *q = p;
    while (is_alpha(*q)) q++;
    char tok[16];
    size_t len = (size_t)(q - p);
    if (len < sizeof tok) memcpy(tok, p, len);
    p = q;
    if (len >= sizeof tok) continue;
    tok[len] = 0;
    bool named = false;
    for (size_t k = 0; k < sizeof names / sizeof names[0]; k++)
      if (ieq(tok, names[k].name)) { inv[names[k].stick] = true; named = true; break; }
    if (named || len > 4) continue;
    bool letters = true;
    for (size_t k = 0; k < len; k++) letters = letters && strchr("aetr", lower(tok[k]));
    for (size_t k = 0; letters && k < len; k++) inv[strchr("aetr", lower(tok[k])) - "aetr"] = true;
  }
}

insrc_kind_t mix_parse_input(const char *s) {
  for (int k = INSRC_KEYBOARD; s && k < INSRC_KINDS; k++)
    if (ieq(s, kind_names[k])) return (insrc_kind_t)k;
  return INSRC_NONE;
}

/* insrc_t has no room for the stick positions at the last "moved" stamp, so they live here, keyed
 * by the slot's address (the input modules keep their slots in the host's array). Comparing with
 * the previous sample instead would miss slow moves at 250-1000 updates a second. Frame loop only. */
#define NREF 32
static struct { const insrc_t *src; int16_t ref[4]; uint32_t used; } refs[NREF];
static uint32_t ref_clock;

static int16_t *ref_of(const insrc_t *s, bool *fresh) {
  int pick = 0;
  for (int i = 0; i < NREF; i++) {
    if (refs[i].src == s) { refs[i].used = ++ref_clock; *fresh = false; return refs[i].ref; }
    if (refs[i].used < refs[pick].used) pick = i;
  }
  refs[pick].src = s;
  refs[pick].used = ++ref_clock;
  *fresh = true;
  return refs[pick].ref;
}

void insrc_update(insrc_t *s, const int16_t *ch, int nch, int64_t now_us) {
  if (!ch || nch < 0) nch = 0;
  if (nch > INPUT_CH) nch = INPUT_CH;
  for (int i = 0; i < INPUT_CH; i++) s->ch[i] = (int16_t)(i < nch ? clamp_ch(ch[i]) : 0);   /* ch may be s->ch */
  s->nch = (uint8_t)nch;
  bool fresh;
  int16_t *ref = ref_of(s, &fresh);
  bool moved = false;
  if (!fresh && s->updates)
    for (int i = 0; i < 4; i++) moved = moved || s->ch[i] - ref[i] > MOVED_DELTA || ref[i] - s->ch[i] > MOVED_DELTA;
  if (fresh || !s->updates || moved) memcpy(ref, s->ch, sizeof(int16_t) * 4);
  if (moved) s->moved_us = now_us;
  s->last_us = now_us;
  s->updates++;
}

static bool is_live(const insrc_t *s, int64_t now_us) {
  return s->present && s->updates && now_us - s->last_us <= INPUT_STALE_US;
}

static bool wanted(const mix_config_t *cfg, const insrc_t *s) {
  return cfg->fixed == INSRC_NONE || s->kind == cfg->fixed;
}

/* the newest mover among the live sources (then the newest update, then the lowest index) */
static int best_live(const mix_config_t *cfg, const insrc_t *src, int n, int skip, int64_t now_us) {
  int b = -1;
  for (int i = 0; i < n; i++) {
    if (i == skip || !wanted(cfg, &src[i]) || !is_live(&src[i], now_us)) continue;
    if (b < 0 || src[i].moved_us > src[b].moved_us ||
        (src[i].moved_us == src[b].moved_us && src[i].last_us > src[b].last_us)) b = i;
  }
  return b;
}

static int pick_source(const mix_config_t *cfg, const insrc_t *src, int n, int cur, int64_t now_us) {
  if (cur >= 0 && cur < n && wanted(cfg, &src[cur]) && is_live(&src[cur], now_us)) {
    int o = best_live(cfg, src, n, cur, now_us);
    if (o < 0 || !src[o].moved_us) return cur;
    /* a current source that never moved yields to any source that did */
    return !src[cur].moved_us || src[o].moved_us - src[cur].moved_us > TAKEOVER_US ? o : cur;
  }
  return best_live(cfg, src, n, -1, now_us);
}

void mix_sticks(const mix_config_t *cfg, const insrc_t *src, int n, int cur, int64_t now_us, sticks_t *out) {
  if (n > INPUT_MAX_SOURCES) n = INPUT_MAX_SOURCES;
  if (n < 0) n = 0;
  out->ail = out->ele = out->rud = 0;
  out->thr = -1024;
  out->pause = INT32_MIN;
  out->live = false;
  int p = pick_source(cfg, src, n, cur, now_us);
  if (p < 0) {
    /* nothing live: report the source that was active (or the last one heard), centered */
    if (cur >= 0 && cur < n && wanted(cfg, &src[cur]) && src[cur].present && src[cur].updates) p = cur;
    else for (int j = 0; j < n; j++)
      if (wanted(cfg, &src[j]) && src[j].present && src[j].updates && (p < 0 || src[j].last_us > src[p].last_us)) p = j;
    out->src = p;
    out->kind = p >= 0 ? src[p].kind : INSRC_NONE;
    return;
  }
  const insrc_t *s = &src[p];
  char order[5];
  if (s->fixed_aetr || !mix_parse_order(cfg->order, order)) memcpy(order, "AETR", 5);
  int v[4] = { 0, 0, -1024, 0 };    /* ail, ele, thr, rud; a missing throttle channel stays low */
  bool have[4] = { false, false, false, false };
  for (int i = 0; i < 4; i++) {
    int stick = order[i] == 'A' ? 0 : order[i] == 'E' ? 1 : order[i] == 'T' ? 2 : 3;
    if (i < s->nch) { v[stick] = clamp_ch(s->ch[i]); have[stick] = true; }
  }
  /* inverting a missing throttle would give full throttle */
  for (int i = 0; i < 4; i++) if (cfg->invert[i] && have[i]) v[i] = -v[i];
  out->ail = v[0];
  out->ele = v[1];
  out->thr = v[2];
  out->rud = v[3];
  if (cfg->pause_ch >= 1 && cfg->pause_ch <= INPUT_CH && cfg->pause_ch <= s->nch) out->pause = clamp_ch(s->ch[cfg->pause_ch - 1]);
  out->src = p;
  out->kind = s->kind;
  out->live = true;
}
