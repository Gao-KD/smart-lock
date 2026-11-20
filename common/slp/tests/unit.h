/* Minimal unit test helpers (no external framework, runs on host and in CI). */
#ifndef SMARTLOCK_UNIT_H
#define SMARTLOCK_UNIT_H

#include <stdio.h>
#include <stdlib.h>

static int unit_failures = 0;
static int unit_checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        unit_checks++;                                                       \
        if (!(cond)) {                                                       \
            unit_failures++;                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

#define RUN(test)                    \
    do {                             \
        printf("  %s\n", #test);     \
        test();                      \
    } while (0)

#define UNIT_DONE()                                                        \
    do {                                                                   \
        printf("%d checks, %d failures\n", unit_checks, unit_failures);   \
        return unit_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;          \
    } while (0)

#endif
