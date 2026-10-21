/* The VTX control routes of docs/WEB_API.md (GET/POST /api/vtx, GET/PUT/DELETE /api/vtx/table) and
 * the "vtx" line of /api/status. They run on the connection's worker thread: the VTX thread is only
 * asked for a copy of its status and told to re-read the files (vtx.h), never waited for. */
#include "web_internal.h"
#include "settings.h"
#include "vtx.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VTX_BODY_MAX 4096

static const char *version_text(const vtx_status_t *s) {
  if (s->proto != VTX_PROTO_SA) return "";
  return s->sa_version == 1 ? "1.0" : s->sa_version == 2 ? "2.0" : s->sa_version == 3 ? "2.1" : "";
}

static void table_json(sb_t *b, const vtx_table_t *t) {
  sb_puts(b, "\"description\":");
  sb_json_str(b, t->description);
  sb_printf(b, ",\"channels\":%d,\"bands\":[", t->nchannels);
  for (int i = 0; i < t->nbands; i++) {
    const vtx_band_t *bd = &t->band[i];
    char letter[2] = { bd->letter, 0 };
    sb_puts(b, i ? ",{\"name\":" : "{\"name\":");
    sb_json_str(b, bd->name);
    sb_puts(b, ",\"letter\":");
    sb_json_str(b, letter);
    sb_printf(b, ",\"factory\":%s,\"frequencies\":[", bd->factory ? "true" : "false");
    for (int c = 0; c < t->nchannels; c++) sb_printf(b, "%s%u", c ? "," : "", bd->freq[c]);
    sb_puts(b, "]}");
  }
  sb_puts(b, "],\"powers\":[");
  for (int i = 0; i < t->npowers; i++) {
    sb_printf(b, "%s{\"value\":%u,\"label\":", i ? "," : "", t->power[i].value);
    sb_json_str(b, t->power[i].label);
    sb_puts(b, "}");
  }
  sb_puts(b, "]");
}

static void status_json(sb_t *b, const vtx_status_t *s) {
  const vtx_table_t *t = &s->table;
  const vtx_config_t *c = &s->cfg;
  char letter[2] = { 0, 0 }, want[2] = { c->band, 0 };
  if (s->band >= 0 && s->band < t->nbands) letter[0] = t->band[s->band].letter;
  sb_printf(b, "{\"enabled\":%s,\"state\":\"%s\",\"message\":", c->mode != VTX_MODE_OFF ? "true" : "false", vtx_state_name(s->state));
  sb_json_str(b, s->message);
  sb_puts(b, ",\"error\":");
  sb_json_str(b, s->error);
  sb_printf(b, ",\"mode\":\"%s\",\"port\":", vtx_mode_name(c->mode));
  sb_json_str(b, c->port);
  sb_puts(b, ",\"device\":");
  sb_json_str(b, s->device);
  sb_printf(b, ",\"single_pin\":%s,\"send_only\":%s,\"needs_reboot\":%s,\"protocol\":\"%s\",\"version\":\"%s\",\"line\":",
            vtx_uart_node(c->port) ? "false" : "true", c->send_only ? "true" : "false",
            s->state == VTX_ST_NO_PORT && vtx_uart_node(c->port) ? "true" : "false", vtx_proto_name(s->proto), version_text(s));
  sb_json_str(b, s->line_desc);
  sb_puts(b, ",\"line_stats\":");
  sb_json_str(b, s->line_stats);
  sb_printf(b, ",\"baud\":%d,\"found\":%s,\"in_sync\":%s,\"freq\":%d,\"freq_mode\":%s,\"band\":", s->baud, s->found ? "true" : "false",
            s->in_sync ? "true" : "false", s->found ? s->freq : 0, s->freq_mode ? "true" : "false");
  sb_json_str(b, letter);
  sb_printf(b, ",\"channel\":%d,\"power\":", s->found ? s->channel : 0);
  if (s->found) {
    const char *label = s->power_level > 0 && s->power_level <= t->npowers ? t->power[s->power_level - 1].label : "";
    sb_printf(b, "{\"level\":%d,\"label\":", s->power_level);
    sb_json_str(b, label);
    sb_printf(b, ",\"value\":%d,\"mw\":%d}", s->power_value, s->power_mw);
  } else sb_puts(b, "null");
  sb_printf(b, ",\"pit\":%s,\"pit_at_boot\":%s,\"locked\":%s,\"temp_c\":", s->found && s->pit ? "true" : "false",
            s->found && s->pit_at_boot ? "true" : "false", s->found && s->locked ? "true" : "false");
  if (s->found && s->have_temp) sb_printf(b, "%d", s->temp_c);
  else sb_puts(b, "null");
  sb_puts(b, ",\"limits\":");
  if (s->found && s->max_freq) sb_printf(b, "{\"min_freq\":%d,\"max_freq\":%d,\"max_power\":%d}", s->min_freq, s->max_freq, s->max_power);
  else sb_puts(b, "null");
  sb_puts(b, ",\"settings\":{\"band\":");
  sb_json_str(b, want);
  sb_printf(b, ",\"channel\":%d,\"freq\":%d,\"power\":%d,\"pit\":%s},\"table\":{\"source\":\"%s\",\"error\":", c->channel, c->freq, c->power,
            c->pit ? "true" : "false", c->have_table ? "file" : "default");
  sb_json_str(b, c->table_error);
  sb_puts(b, ",");
  table_json(b, t);
  sb_printf(b, "},\"stats\":{\"sent\":%lu,\"replies\":%lu,\"timeouts\":%lu,\"bad_frames\":%lu,\"echo_ok\":%lu,\"echo_missing\":%lu,"
            "\"echo_bad\":%lu,\"tx_late\":%lu},\"age_ms\":%lld}",
            (unsigned long)s->sent, (unsigned long)s->replies, (unsigned long)s->timeouts, (unsigned long)s->bad_frames,
            (unsigned long)s->echo_ok, (unsigned long)s->echo_missing, (unsigned long)s->echo_bad, (unsigned long)s->tx_late,
            (long long)(s->updated_us ? (plat_now_us() - s->updated_us) / 1000 : -1));
}

void web_vtx_summary(sb_t *b, app_t *app) {
  vtx_status_t *s = malloc(sizeof *s);
  if (!s) { sb_puts(b, "{\"state\":\"error\",\"text\":\"out of memory\"}"); return; }
  vtx_get_status(app->vtx, s);
  char line[200];
  vtx_status_line(s, line, sizeof line);
  sb_printf(b, "{\"state\":\"%s\",\"text\":", vtx_state_name(s->state));
  sb_json_str(b, line);
  sb_puts(b, "}");
  free(s);
}

static void r_get(web_conn_t *c) {
  vtx_status_t *s = malloc(sizeof *s);
  if (!s) { web_send_error(c, 500, "Internal", "out of memory"); return; }
  vtx_get_status(c->app->vtx, s);
  sb_t b;
  sb_init(&b, 8192);
  status_json(&b, s);
  free(s);
  web_send_sb(c, 200, &b);
  sb_free(&b);
}

typedef struct { const char *name, *key; } alias_t;
static const alias_t FIELDS[] = {
  { "band", "vtx_band" }, { "channel", "vtx_channel" }, { "freq", "vtx_freq" }, { "power", "vtx_power" }, { "pit", "vtx_pit" },
};

static void field_err(sb_t *f, int *nbad, const char *name, const char *msg) {
  if ((*nbad)++) sb_puts(f, ",");
  sb_json_str(f, name);
  sb_puts(f, ":");
  sb_json_str(f, msg);
}

/* POST /api/vtx: "band = R", "channel = 1", "freq = 5800", "power = 2", "pit = on" (settings format;
 * the vtx_* names work too). Checked against the table in use, saved to sticktime.txt, applied. */
static void r_post(web_conn_t *c) {
  char *body;
  size_t len;
  int r = web_read_body_all(c, VTX_BODY_MAX, &body, &len);
  if (r == -2) { web_send_error(c, 413, "PayloadTooLarge", "The body is too big (4 KB at most)."); return; }
  if (r != 0) return;
  settings_t *in = settings_parse(body, len);
  free(body);
  vtx_status_t *s = malloc(sizeof *s);
  if (!in || !s) {
    settings_free(in);
    free(s);
    web_send_error(c, 500, "Internal", "out of memory");
    return;
  }
  vtx_get_status(c->app->vtx, s);
  const vtx_table_t *t = &s->table;
  const char *val[5] = { NULL };
  int nset = 0;
  for (int i = 0; i < 5; i++) {
    const char *v = settings_raw(in, FIELDS[i].name);
    if (!v) v = settings_raw(in, FIELDS[i].key);
    if (v) { val[i] = v; nset++; }
  }
  sb_t f;
  sb_init(&f, 256);
  int nbad = 0;
  char band[2] = "", chan[16] = "", freq[16] = "", pwr[16] = "", pit[4] = "";
  int b = -1;
  if (val[0]) {
    if (val[0][0] && (val[0][1] || (b = vtx_table_band(t, val[0][0])) < 0)) {
      char m[160];
      size_t k = (size_t)snprintf(m, sizeof m, "isn't a band of the VTX table (");
      for (int i = 0; i < t->nbands && k + 4 < sizeof m; i++) k += (size_t)snprintf(m + k, sizeof m - k, "%s%c", i ? ", " : "", t->band[i].letter);
      snprintf(m + k, sizeof m - k, ")");
      field_err(&f, &nbad, "band", m);
    } else if (val[0][0]) {
      band[0] = t->band[b].letter;
      if (band[0] >= 'a' && band[0] <= 'z') band[0] = (char)(band[0] - 32);
    }
  }
  if (val[1]) {
    char *end;
    long ch = strtol(val[1], &end, 10);
    int bb = b >= 0 ? b : (s->cfg.band ? vtx_table_band(t, s->cfg.band) : -1);
    if (*end || end == val[1] || ch < 1 || ch > (t->nchannels > 0 ? t->nchannels : 8)) {
      char m[80];
      snprintf(m, sizeof m, "must be a channel from 1 to %d", t->nchannels > 0 ? t->nchannels : 8);
      field_err(&f, &nbad, "channel", m);
    } else if (bb >= 0 && !vtx_table_freq(t, bb, (int)ch)) {
      field_err(&f, &nbad, "channel", "isn't available in that band (frequency 0 in the VTX table)");
    } else snprintf(chan, sizeof chan, "%ld", ch);
  } else if (b >= 0 && !vtx_table_freq(t, b, s->cfg.channel)) {
    field_err(&f, &nbad, "channel", "isn't available in that band (frequency 0 in the VTX table)");
  }
  if (val[2]) {
    char *end;
    long fq = strtol(val[2], &end, 10);
    if (*end || end == val[2] || (fq != 0 && (fq < 4900 || fq > 6200))) field_err(&f, &nbad, "freq", "must be 0 (band and channel) or 4900 to 6200 MHz");
    else snprintf(freq, sizeof freq, "%ld", fq);
  }
  if (val[3]) {
    char *end;
    long pw = strtol(val[3], &end, 10);
    if (*end || end == val[3] || pw < 0 || pw > t->npowers) {
      char m[80];
      snprintf(m, sizeof m, "must be a level from 1 to %d (0 = leave it)", t->npowers);
      field_err(&f, &nbad, "power", m);
    } else snprintf(pwr, sizeof pwr, "%ld", pw);
  }
  if (val[4]) {
    const char *v = val[4];
    if (!web_casecmp(v, "on") || !web_casecmp(v, "true") || !web_casecmp(v, "yes") || !strcmp(v, "1")) plat_strlcpy(pit, "on", sizeof pit);
    else if (!web_casecmp(v, "off") || !web_casecmp(v, "false") || !web_casecmp(v, "no") || !strcmp(v, "0")) plat_strlcpy(pit, "off", sizeof pit);
    else field_err(&f, &nbad, "pit", "must be on or off");
  }
  settings_free(in);
  if (!nset && !nbad) {
    free(s);
    sb_free(&f);
    web_send_error(c, 400, "BadRequest", "Nothing to set: send band, channel, freq, power or pit (key = value lines).");
    return;
  }
  if (nbad) {
    free(s);
    sb_t e;
    sb_init(&e, 512);
    sb_puts(&e, "{\"error\":\"BadRequest\",\"message\":\"Some values are not valid: nothing was saved.\",\"fields\":{");
    sb_add(&e, f.p, f.len);
    sb_puts(&e, "}}");
    web_send_sb(c, 400, &e);
    sb_free(&e);
    sb_free(&f);
    return;
  }
  sb_free(&f);
  const char *keys[6], *vals[6];
  int n = 0;
  if (val[0]) { keys[n] = "vtx_band"; vals[n++] = band; }
  if (val[1]) { keys[n] = "vtx_channel"; vals[n++] = chan; }
  if (val[2]) { keys[n] = "vtx_freq"; vals[n++] = freq; }
  else if (val[0] && band[0]) { keys[n] = "vtx_freq"; vals[n++] = "0"; }   /* a band: by band and channel again */
  if (val[3]) { keys[n] = "vtx_power"; vals[n++] = pwr; }
  if (val[4]) { keys[n] = "vtx_pit"; vals[n++] = pit; }
  free(s);
  sb_t saved;
  sb_init(&saved, 128);
  if (web_save_settings(keys, vals, n, &saved) != 0) {
    int e = errno;
    char m[200];
    snprintf(m, sizeof m, "Couldn't save the settings: %s", strerror(e));
    web_send_error(c, e == EROFS || e == EACCES ? 403 : 500, e == EROFS || e == EACCES ? "Forbidden" : "Internal", m);
    sb_free(&saved);
    return;
  }
  vtx_reload(c->app->vtx);
  sb_t b2;
  sb_init(&b2, 256);
  sb_puts(&b2, "{\"ok\":true,\"saved\":[");
  sb_add(&b2, saved.p, saved.len);
  sb_puts(&b2, "]}");
  web_send_sb(c, 200, &b2);
  sb_free(&b2);
  sb_free(&saved);
}

static void r_table_get(web_conn_t *c) {
  vtx_status_t *s = malloc(sizeof *s);
  if (!s) { web_send_error(c, 500, "Internal", "out of memory"); return; }
  vtx_get_status(c->app->vtx, s);
  size_t len = 0;
  char *j = vtx_table_to_json(&s->table, &len);
  free(s);
  if (!j) { web_send_error(c, 500, "Internal", "out of memory"); return; }
  if (web_send_head(c, 200, "application/json; charset=utf-8", (int64_t)len,
                    "Cache-Control: no-store\r\nContent-Disposition: attachment; filename=\"vtxtable.json\"\r\n") == 0)
    web_send_raw(c, j, len);
  free(j);
}

static void r_table_put(web_conn_t *c) {
  char *body;
  size_t len;
  int r = web_read_body_all(c, VTX_TABLE_FILE_MAX, &body, &len);
  if (r == -2) { web_send_error(c, 413, "PayloadTooLarge", "A VTX table file is at most 64 KB."); return; }
  if (r != 0) return;
  vtx_table_t *t = malloc(sizeof *t);
  if (!t) { free(body); web_send_error(c, 500, "Internal", "out of memory"); return; }
  char err[200];
  int pr = vtx_table_parse_json(body, len, t, err, sizeof err);
  free(body);
  if (pr != 0) {
    free(t);
    web_send_error(c, 400, "BadRequest", err);
    return;
  }
  size_t jl = 0;
  char *j = vtx_table_to_json(t, &jl);                /* stored as read back: the file always parses */
  char path[700];
  vtx_table_path(c->app, path, sizeof path);
  int wr = j ? plat_write_file_atomic(path, j, jl) : -1;
  int e = errno;
  free(j);
  if (wr != 0) {
    free(t);
    char m[200];
    snprintf(m, sizeof m, "Couldn't save the VTX table: %s", strerror(e));
    web_send_error(c, e == EROFS || e == EACCES ? 403 : 500, e == EROFS || e == EACCES ? "Forbidden" : "Internal", m);
    return;
  }
  app_log(c->app, "web: VTX table loaded: %s (%d bands, %d power levels)", t->description, t->nbands, t->npowers);
  vtx_reload(c->app->vtx);
  sb_t b;
  sb_init(&b, 512);
  sb_puts(&b, "{\"ok\":true,\"description\":");
  sb_json_str(&b, t->description);
  sb_printf(&b, ",\"bands\":%d,\"channels\":%d,\"powers\":%d}", t->nbands, t->nchannels, t->npowers);
  free(t);
  web_send_sb(c, 200, &b);
  sb_free(&b);
}

static void r_table_delete(web_conn_t *c) {
  char path[700];
  vtx_table_path(c->app, path, sizeof path);
  plat_stat_t st;
  if (plat_stat(path, &st) == 0 && st.exists && plat_remove(path) != 0) {
    char m[200];
    snprintf(m, sizeof m, "Couldn't delete the VTX table: %s", strerror(errno));
    web_send_error(c, 500, "Internal", m);
    return;
  }
  app_log(c->app, "web: VTX table back to the built-in one");
  vtx_reload(c->app->vtx);
  web_send_ok(c, 200);
}

void web_vtx_route(web_conn_t *c) {
  bool table = !strcmp(c->path, "/api/vtx/table");
  const char *m = c->method;
  if (!table) {
    if (!strcmp(m, "POST")) r_post(c);
    else r_get(c);
  } else if (!strcmp(m, "PUT")) r_table_put(c);
  else if (!strcmp(m, "DELETE")) r_table_delete(c);
  else r_table_get(c);
}
