#ifndef LGNOME_NATIVE_TIME_H
#define LGNOME_NATIVE_TIME_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Milliseconds for short intervals; compare stamps by unsigned subtraction. */
static inline uint32_t native_monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint32_t)now.tv_sec * 1000u + (uint32_t)now.tv_nsec / 1000000u;
}

/* Deadlines must be less than INT32_MAX milliseconds away. */
static inline bool native_deadline_reached_ms(uint32_t now_ms, uint32_t deadline_ms) {
    return (uint32_t)(now_ms - deadline_ms) <= INT32_MAX;
}

/* Seconds for session duration: a 32-bit range covers about 136 years. */
static inline uint32_t native_monotonic_s(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint32_t)now.tv_sec;
}

/* Resume the remaining sleep after EINTR. */
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
