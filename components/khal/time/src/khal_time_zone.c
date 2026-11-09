/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_time_zone.c
 * @brief Local time: the C library's zone, applied and read under its environment lock.
 */
#include "khal_time.h"

#include <stdlib.h>
/* MISRA 21.10 deviation: tzset() and localtime_r() read the POSIX TZ rules the library holds. */
/* cppcheck-suppress misra-c2012-21.10 ; the zone is the C library's */
#include <time.h>

#include "khal_sync.h"

/** @brief The environment variable the C library reads the zone from. */
#define KHAL_TZ_VAR        "TZ"
/** @brief setenv()'s third argument: replace a TZ already there. */
#define KHAL_TZ_OVERWRITE  1
/** @brief setenv()'s answer when it took the value. */
#define KHAL_TZ_SET_OK     0

bool khal_time_zone_set(const char *tz)
{
    bool applied = false;

    if (tz != NULL) {
        khal_env_lock();
        /* cppcheck-suppress misra-c2012-17.3 ; a POSIX call: the compiler sees picolibc declare it, cppcheck does not */
        if (setenv(KHAL_TZ_VAR, tz, KHAL_TZ_OVERWRITE) == KHAL_TZ_SET_OK) {
            applied = true;
        }
        else {
            /* No memory for the string: TZ goes, so the zone is UTC, not what the library kept. */
            /* cppcheck-suppress misra-c2012-17.3 ; a POSIX call: the compiler sees picolibc declare it, cppcheck does not */
            (void)unsetenv(KHAL_TZ_VAR);
        }
        /* cppcheck-suppress misra-c2012-17.3 ; a POSIX call: the compiler sees picolibc declare it, cppcheck does not */
        tzset();
        khal_env_unlock();
    }
    return applied;
}

bool khal_time_local(int64_t unix_s, struct tm *out)
{
    bool       done = false;
    struct tm  t;
    time_t const secs = (time_t)unix_s;

    if (out != NULL) {
        khal_env_lock();
        /* cppcheck-suppress misra-c2012-17.3 ; a POSIX call: the compiler sees picolibc declare it, cppcheck does not */
        done = (localtime_r(&secs, &t) != NULL);
        khal_env_unlock();
        if (done) {
            *out = t;
        }
    }
    return done;
}
