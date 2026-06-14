/*
 * Minimal, dependency-free unit-test helpers.
 *
 * Each test file defines `static void run_all(void)` that invokes its test
 * functions, then ends with VT_TEST_MAIN().  A test fails by tripping a
 * VT_CHECK* macro; the process exits non-zero if any check failed, so `make
 * test` and CI can gate on the exit status.
 */
#ifndef VEKTERM_VT_TEST_H
#define VEKTERM_VT_TEST_H

#include <stdio.h>

static int vt_checks_run = 0;
static int vt_checks_failed = 0;

#define VT_CHECK(cond)                                                                             \
    do {                                                                                           \
        vt_checks_run++;                                                                           \
        if (!(cond)) {                                                                             \
            vt_checks_failed++;                                                                    \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                               \
        }                                                                                          \
    } while (0)

#define VT_CHECK_EQ(actual, expected)                                                              \
    do {                                                                                           \
        long _a = (long)(actual);                                                                  \
        long _e = (long)(expected);                                                                \
        vt_checks_run++;                                                                           \
        if (_a != _e) {                                                                            \
            vt_checks_failed++;                                                                    \
            printf("  FAIL %s:%d: %s == %s (got %ld, want %ld)\n", __FILE__, __LINE__, #actual,    \
                   #expected, _a, _e);                                                             \
        }                                                                                          \
    } while (0)

#define VT_CHECK_EQ_U32(actual, expected)                                                          \
    do {                                                                                           \
        unsigned long _a = (unsigned long)(actual);                                                \
        unsigned long _e = (unsigned long)(expected);                                              \
        vt_checks_run++;                                                                           \
        if (_a != _e) {                                                                            \
            vt_checks_failed++;                                                                    \
            printf("  FAIL %s:%d: %s == %s (got 0x%08lX, want 0x%08lX)\n", __FILE__, __LINE__,     \
                   #actual, #expected, _a, _e);                                                    \
        }                                                                                          \
    } while (0)

#define VT_RUN(fn)                                                                                 \
    do {                                                                                           \
        printf("- %s\n", #fn);                                                                     \
        fn();                                                                                      \
    } while (0)

#define VT_TEST_MAIN()                                                                             \
    int main(void)                                                                                 \
    {                                                                                              \
        run_all();                                                                                 \
        if (vt_checks_failed != 0) {                                                               \
            printf("FAILED: %d/%d checks\n", vt_checks_failed, vt_checks_run);                     \
            return 1;                                                                              \
        }                                                                                          \
        printf("ok: %d checks\n", vt_checks_run);                                                  \
        return 0;                                                                                  \
    }

#endif /* VEKTERM_VT_TEST_H */
