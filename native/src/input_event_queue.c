#include "input_event_queue.h"
#include "clog.h"

clog_define(g_native_log_event_queue, cLogLevelInfo, "input.evdev");

void native_event_queue_publish_before(NativeEventQueue *queue, uint64_t time_us) {
    queue->publication_bounded = true;
    queue->published_before_us = time_us;
}

void native_event_queue_push(NativeEventQueue *queue, const NativeEvent *event) {
    /* Once history is incomplete, none of this sweep is safe to replay: a
     * later-read device can still contribute an old press whose release was
     * discarded. Wait for the consumer's release-all before accepting input. */
    if (queue->reset_pending) {
        return;
    }
    if (queue->count == NATIVE_EVENT_QUEUE_CAPACITY) {
        /* Losing an arbitrary key/button release can strand the remote desktop.
         * Discard the incomplete history and release held input before replaying. */
        queue->head = 0;
        queue->count = 0;
        queue->reset_pending = true;
        clog_limited(cLogLevelWarning, 2, 5000, "input queue overflow; resetting held input");
        return;
    }
    unsigned pos = queue->count;
    while (pos > 0) {
        unsigned prev = (queue->head + pos - 1u) % NATIVE_EVENT_QUEUE_CAPACITY;
        if (queue->events[prev].time_us <= event->time_us) {
            break;
        }
        queue->events[(queue->head + pos) % NATIVE_EVENT_QUEUE_CAPACITY] = queue->events[prev];
        pos--;
    }
    queue->events[(queue->head + pos) % NATIVE_EVENT_QUEUE_CAPACITY] = *event;
    queue->count++;
}

NativeEventKind native_event_queue_next_kind(const NativeEventQueue *queue) {
    if (queue->reset_pending) {
        return NATIVE_EVENT_RESET;
    }
    if (!queue->count || (queue->publication_bounded &&
                         queue->events[queue->head].time_us >= queue->published_before_us)) {
        return NATIVE_EVENT_NONE;
    }
    return queue->events[queue->head].kind;
}

bool native_event_queue_pop(NativeEventQueue *queue, NativeEventKind kind, NativeEvent *event) {
    if (kind == NATIVE_EVENT_NONE || kind == NATIVE_EVENT_RESET ||
        native_event_queue_next_kind(queue) != kind) {
        return false;
    }
    *event = queue->events[queue->head];
    queue->head = (queue->head + 1u) % NATIVE_EVENT_QUEUE_CAPACITY;
    queue->count--;
    return true;
}

bool native_event_queue_take_reset(NativeEventQueue *queue) {
    bool pending = queue->reset_pending;
    queue->reset_pending = false;
    return pending;
}
