#ifndef CHECK_H
#define CHECK_H

/* Minimal test harness: no framework to install, failures print file:line and the expression. */

#include <stdio.h>

static int check_failures;
static int check_count;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        check_count++;                                                                 \
        if (!(cond)) {                                                                 \
            check_failures++;                                                          \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                              \
    } while (0)

#define CHECK_STATUS(expr, expected)                                                   \
    do {                                                                               \
        ws_status_t check_actual_ = (expr);                                            \
        check_count++;                                                                 \
        if (check_actual_ != (expected)) {                                             \
            check_failures++;                                                          \
            fprintf(stderr, "%s:%d: %s returned %s, expected %s\n", __FILE__, __LINE__, \
                    #expr, ws_status_name(check_actual_), ws_status_name(expected));   \
        }                                                                              \
    } while (0)

static int check_report(const char *suite)
{
    printf("%s: %d checks, %d failed\n", suite, check_count, check_failures);
    return check_failures == 0 ? 0 : 1;
}

#endif
