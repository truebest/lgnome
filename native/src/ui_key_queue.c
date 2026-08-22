#include "ui_key_queue.h"

#include "clog.h"

clog_define(g_native_log_ui_key_queue, cLogLevelInfo, cLogFlags_Default, "ui.keypad", NULL);

bool native_ui_key_queue_empty(const NativeUiKeyQueue *queue) {
    return !queue || queue->head == queue->tail;
}

void native_ui_key_queue_clear(NativeUiKeyQueue *queue) {
    if (!queue) {
        return;
    }
    bool overflowed = queue->overflowed;
    queue->head = 0;
    queue->tail = 0;
    queue->overflowed = false;
    if (overflowed) {
        clog(cLogLevelTrace, "cleared overflowed key queue");
    }
}

bool native_ui_key_queue_enqueue(NativeUiKeyQueue *queue, uint32_t key, uint32_t state) {
    if (!queue || queue->overflowed) {
        return false;
    }
    unsigned next_tail = (queue->tail + 1u) % NATIVE_UI_KEY_QUEUE_CAP;
    if (next_tail == queue->head) {
        /* Refuse the entire remainder of the burst. The caller clears the queue and
         * emits a synthetic release, so no half press/release pair can get stuck. */
        queue->overflowed = true;
        return false;
    }
    queue->events[queue->tail].key = key;
    queue->events[queue->tail].state = state;
    queue->tail = next_tail;
    return true;
}

bool native_ui_key_queue_dequeue(NativeUiKeyQueue *queue, NativeUiQueuedKey *event) {
    if (!queue || !event || native_ui_key_queue_empty(queue)) {
        return false;
    }
    *event = queue->events[queue->head];
    queue->head = (queue->head + 1u) % NATIVE_UI_KEY_QUEUE_CAP;
    return true;
}
