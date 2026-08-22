#include "ui_key_queue.h"

#include <assert.h>
#include <string.h>

static void test_fifo_and_wrap(void) {
    NativeUiKeyQueue queue;
    memset(&queue, 0, sizeof(queue));

    for (uint32_t i = 0; i < NATIVE_UI_KEY_QUEUE_CAP - 1u; i++) {
        assert(native_ui_key_queue_enqueue(&queue, i, i & 1u));
    }
    for (uint32_t i = 0; i < NATIVE_UI_KEY_QUEUE_CAP / 2u; i++) {
        NativeUiQueuedKey event;
        assert(native_ui_key_queue_dequeue(&queue, &event));
        assert(event.key == i);
        assert(event.state == (i & 1u));
    }
    for (uint32_t i = 0; i < NATIVE_UI_KEY_QUEUE_CAP / 2u; i++) {
        assert(native_ui_key_queue_enqueue(&queue, 10000u + i, 1u));
    }

    uint32_t expected = NATIVE_UI_KEY_QUEUE_CAP / 2u;
    NativeUiQueuedKey event;
    while (native_ui_key_queue_dequeue(&queue, &event)) {
        if (expected < NATIVE_UI_KEY_QUEUE_CAP - 1u) {
            assert(event.key == expected++);
        } else {
            assert(event.key == 10000u + expected - (NATIVE_UI_KEY_QUEUE_CAP - 1u));
            expected++;
        }
    }
    assert(expected == (NATIVE_UI_KEY_QUEUE_CAP - 1u) + NATIVE_UI_KEY_QUEUE_CAP / 2u);
}

static void test_overflow_discards_burst(void) {
    NativeUiKeyQueue queue = {0};
    for (uint32_t i = 0; i < NATIVE_UI_KEY_QUEUE_CAP - 1u; i++) {
        assert(native_ui_key_queue_enqueue(&queue, i, 1u));
    }
    assert(!native_ui_key_queue_enqueue(&queue, 9999u, 0u));
    assert(queue.overflowed);
    assert(!native_ui_key_queue_enqueue(&queue, 10000u, 1u));

    native_ui_key_queue_clear(&queue);
    assert(native_ui_key_queue_empty(&queue));
    assert(!queue.overflowed);
    assert(native_ui_key_queue_enqueue(&queue, 42u, 1u));
    NativeUiQueuedKey event;
    assert(native_ui_key_queue_dequeue(&queue, &event));
    assert(event.key == 42u);
}

int main(void) {
    test_fifo_and_wrap();
    test_overflow_discards_burst();
    return 0;
}
