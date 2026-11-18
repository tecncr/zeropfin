/* zt.h — tiny unit-test helpers (no external framework). */
#ifndef ZT_H
#define ZT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int zt_failures;
static int zt_checks;

#define ZT_CHECK(cond)                                                                                                 \
    do {                                                                                                               \
        zt_checks++;                                                                                                   \
        if (!(cond)) {                                                                                                 \
            zt_failures++;                                                                                             \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                   \
        }                                                                                                              \
    } while (0)

#define ZT_EQ_INT(a, b)                                                                                                \
    do {                                                                                                               \
        long long _a = (long long)(a), _b = (long long)(b);                                                            \
        zt_checks++;                                                                                                   \
        if (_a != _b) {                                                                                                \
            zt_failures++;                                                                                             \
            fprintf(stderr, "%s:%d: %s == %s failed: %lld != %lld\n", __FILE__, __LINE__, #a, #b, _a, _b);             \
        }                                                                                                              \
    } while (0)

#define ZT_EQ_STR(a, b)                                                                                                \
    do {                                                                                                               \
        const char *_a = (a), *_b = (b);                                                                               \
        zt_checks++;                                                                                                   \
        if (!_a || !_b || strcmp(_a, _b) != 0) {                                                                       \
            zt_failures++;                                                                                             \
            fprintf(stderr, "%s:%d: %s == %s failed: \"%s\" != \"%s\"\n", __FILE__, __LINE__, #a, #b,                  \
                    _a ? _a : "(null)", _b ? _b : "(null)");                                                           \
        }                                                                                                              \
    } while (0)

#define ZT_RUN(fn)                                                                                                     \
    do {                                                                                                               \
        int _before = zt_failures;                                                                                     \
        fn();                                                                                                          \
        fprintf(stderr, "%s %s\n", zt_failures == _before ? "ok  " : "FAIL", #fn);                                     \
    } while (0)

#define ZT_DONE()                                                                                                      \
    do {                                                                                                               \
        fprintf(stderr, "%d checks, %d failures\n", zt_checks, zt_failures);                                           \
        return zt_failures ? 1 : 0;                                                                                    \
    } while (0)

#endif
