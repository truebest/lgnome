#include "input_event_queue.h"

#include <assert.h>
#include <string.h>

static NativeEvent key(uint64_t time, uint16_t code, bool down) {
    NativeEvent event = {.time_us = time, .kind = NATIVE_EVENT_KEYBOARD};
    event.data.keyboard = (NativeKeyboardEv){.code = code, .down = down};
    return event;
}

static NativeEvent mouse(uint64_t time, NativeMouseEvKind kind) {
    NativeEvent event = {.time_us = time, .kind = NATIVE_EVENT_MOUSE};
    event.data.mouse.kind = kind;
    event.data.mouse.wheel_y = 1;
    return event;
}

static void expect(NativeEventQueue *queue, NativeEvent expected) {
    NativeEvent actual;
    assert(native_event_queue_next_kind(queue) == expected.kind);
    assert(native_event_queue_pop(queue, expected.kind, &actual));
    assert(actual.time_us == expected.time_us);
    if (actual.kind == NATIVE_EVENT_KEYBOARD) {
        assert(actual.data.keyboard.code == expected.data.keyboard.code);
        assert(actual.data.keyboard.down == expected.data.keyboard.down);
    } else {
        assert(actual.data.mouse.kind == expected.data.mouse.kind);
    }
}

static void test_ctrl_wheel(bool right_ctrl) {
    NativeEventQueue queue = {0};
    uint16_t code = right_ctrl ? 97 : 29;
    NativeEvent down = key(10, code, true);
    NativeEvent zoom = mouse(20, NATIVE_MOUSE_EV_WHEEL);
    NativeEvent up = key(30, code, false);
    NativeEvent scroll = mouse(40, NATIVE_MOUSE_EV_WHEEL);
    /* Mouse fd drained before keyboard fd, as happens with separate USB devices. */
    native_event_queue_push(&queue, &zoom);
    native_event_queue_push(&queue, &scroll);
    native_event_queue_push(&queue, &down);
    native_event_queue_push(&queue, &up);
    NativeEvent unused;
    assert(!native_event_queue_pop(&queue, NATIVE_EVENT_MOUSE, &unused));
    expect(&queue, down);
    expect(&queue, zoom);
    expect(&queue, up);
    expect(&queue, scroll);
    assert(native_event_queue_next_kind(&queue) == NATIVE_EVENT_NONE);
    /* Reverse fd order must produce the same result. */
    native_event_queue_push(&queue, &down);
    native_event_queue_push(&queue, &up);
    native_event_queue_push(&queue, &zoom);
    native_event_queue_push(&queue, &scroll);
    expect(&queue, down);
    expect(&queue, zoom);
    expect(&queue, up);
    expect(&queue, scroll);
}

static void test_motion_does_not_cross_modifier(void) {
    NativeEventQueue queue = {0};
    NativeEvent before = mouse(1, NATIVE_MOUSE_EV_MOTION);
    NativeEvent after = mouse(3, NATIVE_MOUSE_EV_MOTION);
    NativeEvent shift = key(2, 42, true);
    native_event_queue_push(&queue, &before);
    native_event_queue_push(&queue, &after);
    native_event_queue_push(&queue, &shift);
    expect(&queue, before);
    expect(&queue, shift);
    expect(&queue, after);
}

static void test_equal_times_and_wrap(void) {
    NativeEventQueue queue = {0};
    for (unsigned i = 0; i < NATIVE_EVENT_QUEUE_CAPACITY + 3u; i++) {
        NativeEvent down = key(i, 29, true);
        NativeEvent wheel = mouse(i, NATIVE_MOUSE_EV_WHEEL);
        native_event_queue_push(&queue, &down);
        native_event_queue_push(&queue, &wheel);
        expect(&queue, down);
        expect(&queue, wheel);
    }
    /* Sorted insertion across the physical end of the ring. */
    queue.head = NATIVE_EVENT_QUEUE_CAPACITY - 1u;
    NativeEvent first = key(1, 29, true);
    NativeEvent last = key(3, 29, false);
    NativeEvent middle = mouse(2, NATIVE_MOUSE_EV_WHEEL);
    native_event_queue_push(&queue, &last);
    native_event_queue_push(&queue, &middle);
    native_event_queue_push(&queue, &first);
    expect(&queue, first);
    expect(&queue, middle);
    expect(&queue, last);
}

static void test_overflow_requires_release(void) {
    NativeEventQueue queue = {0};
    for (unsigned i = 0; i < NATIVE_EVENT_QUEUE_CAPACITY; i++) {
        NativeEvent event = mouse(i, NATIVE_MOUSE_EV_MOTION);
        native_event_queue_push(&queue, &event);
    }
    NativeEvent up = key(NATIVE_EVENT_QUEUE_CAPACITY, 29, false);
    native_event_queue_push(&queue, &up);
    assert(native_event_queue_next_kind(&queue) == NATIVE_EVENT_RESET);
    NativeEvent unused;
    assert(!native_event_queue_pop(&queue, NATIVE_EVENT_KEYBOARD, &unused));
    assert(native_event_queue_take_reset(&queue));
    assert(!native_event_queue_take_reset(&queue));
    assert(native_event_queue_next_kind(&queue) == NATIVE_EVENT_NONE);
    native_event_queue_push(&queue, &up);
    expect(&queue, up);
}

static void test_overflow_discards_late_read_press(void) {
    NativeEventQueue queue = {0};
    NativeEvent up = key(20, 29, false);
    native_event_queue_push(&queue, &up);
    for (unsigned i = 1; i <= NATIVE_EVENT_QUEUE_CAPACITY; i++) {
        NativeEvent event = mouse(30 + i, NATIVE_MOUSE_EV_MOTION);
        native_event_queue_push(&queue, &event);
    }
    /* A different fd is read later but contains a press older than the lost up. */
    NativeEvent down = key(10, 29, true);
    native_event_queue_push(&queue, &down);
    assert(native_event_queue_take_reset(&queue));
    assert(native_event_queue_next_kind(&queue) == NATIVE_EVENT_NONE);
    down.time_us = 2000;
    native_event_queue_push(&queue, &down);
    expect(&queue, down);
}

static void test_events_arriving_during_sweep(void) {
    NativeEventQueue queue = {0};
    native_event_queue_publish_before(&queue, 0);
    /* Keyboard was already read; Ctrl arrives there at t=11, then mouse t=12
     * is read in the same sweep which started at t=10. */
    NativeEvent wheel = mouse(12, NATIVE_MOUSE_EV_WHEEL);
    native_event_queue_push(&queue, &wheel);
    native_event_queue_publish_before(&queue, 10);
    assert(native_event_queue_next_kind(&queue) == NATIVE_EVENT_NONE);
    NativeEvent ctrl = key(11, 29, true);
    native_event_queue_push(&queue, &ctrl);
    native_event_queue_publish_before(&queue, 20);
    expect(&queue, ctrl);
    expect(&queue, wheel);
    /* Events exactly on the cutoff wait too (kernel times use microseconds). */
    wheel.time_us = 20;
    native_event_queue_push(&queue, &wheel);
    assert(native_event_queue_next_kind(&queue) == NATIVE_EVENT_NONE);
    native_event_queue_publish_before(&queue, 21);
    expect(&queue, wheel);
}

int main(void) {
    test_events_arriving_during_sweep();
    test_ctrl_wheel(false);
    test_ctrl_wheel(true);
    test_motion_does_not_cross_modifier();
    test_equal_times_and_wrap();
    test_overflow_requires_release();
    test_overflow_discards_late_read_press();
    return 0;
}
