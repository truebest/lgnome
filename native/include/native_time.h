#ifndef GNOMECAST_NATIVE_TIME_H
#define GNOMECAST_NATIVE_TIME_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Shared monotonic-clock and sleep helpers. Header-only so SDL-free workers,
 * the SDL main loop, and host tests all use the same clock without a link
 * dependency. CLOCK_MONOTONIC is assumed available (Linux-only tree); a failed
 * clock_gettime reads as 0 rather than aborting, matching the previous
 * per-file copies. */

static inline uint64_t native_monotonic_ms64(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

/* Narrow wrapper for short intervals only: subtraction of two values is
 * intentionally wrap-safe, absolute values are not meaningful. */
static inline uint32_t native_monotonic_ms(void) {
    return (uint32_t)native_monotonic_ms64();
}

/* Sleeps the full duration: EINTR resumes with the remaining time. Callers
 * that must wake early should poll an fd/eventfd instead of sleeping. */
static inline void native_sleep_ms(unsigned milliseconds) {
    struct timespec delay = {
        .tv_sec = (time_t)(milliseconds / 1000u),
        .tv_nsec = (long)(milliseconds % 1000u) * 1000000L,
    };
    while (nanosleep(&delay, &delay) != 0) {
    }
}

static inline void native_timespec_add_ns(struct timespec *time, uint64_t nanoseconds) {
    time->tv_sec += (time_t)(nanoseconds / 1000000000u);
    time->tv_nsec += (long)(nanoseconds % 1000000000u);
    if (time->tv_nsec >= 1000000000L) {
        time->tv_sec++;
        time->tv_nsec -= 1000000000L;
    }
}

static inline bool native_timespec_after(const struct timespec *left, const struct timespec *right) {
    return left->tv_sec > right->tv_sec || (left->tv_sec == right->tv_sec && left->tv_nsec > right->tv_nsec);
}

#endif
