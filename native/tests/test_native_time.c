#include <assert.h>
#include "native_time.h"

static struct timespec clock_value;

int clock_gettime(clockid_t clock_id, struct timespec *time) {
    assert(clock_id == CLOCK_MONOTONIC);
    *time = clock_value;
    return 0;
}

static void test_clock_conversion_and_deadline_wrap(void) {
    clock_value = (struct timespec){
        .tv_sec = UINT32_MAX / 1000u,
        .tv_nsec = (long)(UINT32_MAX % 1000u) * 1000000L,
    };
    assert(native_monotonic_ms() == UINT32_MAX);
    clock_value.tv_nsec += 1000000;
    assert(native_monotonic_ms() == 0);

    uint32_t start = UINT32_MAX - 49u;
    uint32_t deadline = start + 100u;
    assert(deadline == 50u);
    assert(!native_deadline_reached_ms(start, deadline));
    assert(!native_deadline_reached_ms(UINT32_MAX, deadline));
    assert(!native_deadline_reached_ms(0, deadline));
    assert(!native_deadline_reached_ms(49, deadline));
    assert(native_deadline_reached_ms(50, deadline));
    assert(native_deadline_reached_ms(51, deadline));
    assert(!native_deadline_reached_ms(UINT32_MAX, 0));
    assert(native_deadline_reached_ms(0, 0));
    /* An immediate first attempt also works late in system uptime. */
    assert(native_deadline_reached_ms(UINT32_MAX - 1000u, UINT32_MAX - 1000u));
}

static void test_seconds_cover_long_sessions(void) {
    clock_value = (struct timespec){.tv_sec = 4000000, .tv_nsec = 0};
    uint32_t started_s = native_monotonic_s();
    clock_value.tv_sec += 120 * 24 * 60 * 60;
    uint32_t elapsed_minutes = (native_monotonic_s() - started_s) / 60u;
    assert(elapsed_minutes == 120u * 24u * 60u);
}

int main(void) {
    test_clock_conversion_and_deadline_wrap();
    test_seconds_cover_long_sessions();
    return 0;
}
