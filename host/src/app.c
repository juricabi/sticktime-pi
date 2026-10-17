/* Shared state between the frame loop and the workers: see app.h. */
#include "app.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void app_init(app_t *a) {
  memset(a, 0, sizeof *a);
  plat_mutex_init(&a->mu);
  plat_strlcpy(a->version, STP_VERSION, sizeof a->version);
  a->start_us = plat_now_us();
  a->st.sticks.src = -1;
  a->st.sticks.thr = -1024;
}

void app_post(app_t *a, uint32_t cmds) {
  plat_mutex_lock(&a->mu);
  a->cmds |= cmds;
  plat_mutex_unlock(&a->mu);
}

void app_post_key(app_t *a, int evt) {
  plat_mutex_lock(&a->mu);
  if (a->nkeys < APP_KEYQ) a->keyq[a->nkeys++] = evt;
  plat_mutex_unlock(&a->mu);
}

uint32_t app_take(app_t *a, int *keys, int *nkeys) {
  plat_mutex_lock(&a->mu);
  uint32_t c = a->cmds;
  a->cmds = 0;
  int n = a->nkeys;
  if (keys && nkeys) {
    memcpy(keys, a->keyq, sizeof(int) * (size_t)n);
    *nkeys = n;
  }
  a->nkeys = 0;
  plat_mutex_unlock(&a->mu);
  return c;
}

void app_publish(app_t *a, const app_status_t *st) {
  plat_mutex_lock(&a->mu);
  a->st = *st;
  plat_mutex_unlock(&a->mu);
}

void app_snapshot(app_t *a, app_status_t *out) {
  plat_mutex_lock(&a->mu);
  *out = a->st;
  plat_mutex_unlock(&a->mu);
}

void app_log(app_t *a, const char *fmt, ...) {
  char line[600];
  int64_t ms = (plat_now_us() - (a ? a->start_us : 0)) / 1000;
  int n = snprintf(line, sizeof line, "[%7.3f] ", (double)ms / 1000.0);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
  va_end(ap);
  size_t len = strlen(line);
  while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
  fprintf(stderr, "%s\n", line);
  if (!a) return;
  line[len++] = '\n';
  plat_mutex_lock(&a->mu);
  if (len > sizeof a->log) len = sizeof a->log;
  if (a->log_len + len > sizeof a->log) {
    /* drop whole lines from the front */
    size_t need = a->log_len + len - sizeof a->log;
    char *nl = need < a->log_len ? memchr(a->log + need, '\n', a->log_len - need) : NULL;
    size_t drop = nl ? (size_t)(nl - a->log) + 1 : a->log_len;
    memmove(a->log, a->log + drop, a->log_len - drop);
    a->log_len -= drop;
  }
  memcpy(a->log + a->log_len, line, len);
  a->log_len += len;
  plat_mutex_unlock(&a->mu);
}

size_t app_log_copy(app_t *a, char *out, size_t max) {
  plat_mutex_lock(&a->mu);
  size_t n = a->log_len < max - 1 ? a->log_len : max - 1;
  memcpy(out, a->log + (a->log_len - n), n);
  out[n] = 0;
  plat_mutex_unlock(&a->mu);
  return n;
}
