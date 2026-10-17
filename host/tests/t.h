/* Minimal unit-test helpers: each tests/<name>.c is its own program with a main().
 * Run from the repository root (build.sh test does), so paths like "host/fonts" work. */
#ifndef T_H
#define T_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_fail = 0, t_count = 0;

#define CHECK(cond) do { t_count++; if (!(cond)) { t_fail++; \
  fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_INT(a, b) do { long long _a = (long long)(a), _b = (long long)(b); t_count++; if (_a != _b) { t_fail++; \
  fprintf(stderr, "%s:%d: %s == %lld, expected %s == %lld\n", __FILE__, __LINE__, #a, _a, #b, _b); } } while (0)
#define CHECK_STR(a, b) do { const char *_a = (a), *_b = (b); t_count++; \
  if (!_a || !_b || strcmp(_a, _b)) { t_fail++; \
  fprintf(stderr, "%s:%d: %s == \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #a, _a ? _a : "(null)", _b ? _b : "(null)"); } } while (0)
#define CHECK_NEAR(a, b, eps) do { double _a = (a), _b = (b); t_count++; if (_a - _b > (eps) || _b - _a > (eps)) { t_fail++; \
  fprintf(stderr, "%s:%d: %s == %g, expected %g (+-%g)\n", __FILE__, __LINE__, #a, _a, _b, (double)(eps)); } } while (0)

#define T_DONE() do { printf("%d checks, %d failed\n", t_count, t_fail); return t_fail ? 1 : 0; } while (0)

#endif
