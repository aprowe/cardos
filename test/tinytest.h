#ifndef TINYTEST_H
#define TINYTEST_H
#include <stdio.h>
extern int tt_checks, tt_fails;
extern const char *tt_current;
#define CHECK(cond) do { tt_checks++; if (!(cond)) { tt_fails++; \
  printf("  FAIL %s:%d in %s: %s\n", __FILE__, __LINE__, tt_current, #cond); } } while (0)
#define CHECK_EQ(a,b) do { tt_checks++; long long _a=(long long)(a), _b=(long long)(b); \
  if (_a!=_b) { tt_fails++; printf("  FAIL %s:%d in %s: %s == %s (%lld vs %lld)\n", \
  __FILE__, __LINE__, tt_current, #a, #b, _a, _b); } } while (0)
#define RUN(fn) do { tt_current = #fn; int _before = tt_fails; fn(); \
  printf("%-58s %s\n", #fn, tt_fails == _before ? "ok" : "FAILED"); } while (0)
int tt_report(void);
#endif
