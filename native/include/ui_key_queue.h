#ifndef LGNOME_UI_KEY_QUEUE_H
#define LGNOME_UI_KEY_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#define NATIVE_UI_KEY_QUEUE_CAP 1023u

typedef struct NativeUiQueuedKey {
    uint32_t key;
    uint32_t state;
} NativeUiQueuedKey;

typedef struct NativeUiKeyQueue {
    NativeUiQueuedKey events[NATIVE_UI_KEY_QUEUE_CAP];
    unsigned head;
    unsigned tail;
    bool overflowed;
} NativeUiKeyQueue;

bool native_ui_key_queue_empty(const NativeUiKeyQueue *queue);
void native_ui_key_queue_clear(NativeUiKeyQueue *queue);
bool native_ui_key_queue_enqueue(NativeUiKeyQueue *queue, uint32_t key, uint32_t state);
bool native_ui_key_queue_dequeue(NativeUiKeyQueue *queue, NativeUiQueuedKey *event);

#endif
