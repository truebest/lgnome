#ifndef LGNOME_INPUT_EVDEV_H
#define LGNOME_INPUT_EVDEV_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One evdev reader publishes timestamp-ordered input for the SDL thread. */

#include "input_event_queue.h"

typedef struct NativeEvdevInput {
    pthread_mutex_t lock;
    NativeEventQueue queue;
    pthread_t thread;
    void *backend;
    int wake_fd; /* eventfd: reader wakes SDL/main loop after queued input */
    int stop_fd; /* eventfd: stop wakes the poll()ing reader */
    atomic_bool running;
    bool lock_initialized;
    bool started;
    atomic_bool mouse_active;
    atomic_bool keyboard_active;
    atomic_uint event_count; /* advisory diagnostics */
} NativeEvdevInput;

bool native_evdev_input_probe_keyboard(void);
bool native_evdev_input_probe_mouse(void);
bool native_evdev_input_start(NativeEvdevInput *input);
void native_evdev_input_stop(NativeEvdevInput *input);
/* Stop capture and reconcile an overlay's SDL-button bitmask with physical
 * button state after ungrabbing, including releases already queued in evdev. */
void native_evdev_input_stop_with_buttons(NativeEvdevInput *input, uint8_t *buttons);
bool native_evdev_input_active(const NativeEvdevInput *input);
bool native_evdev_input_mouse_active(NativeEvdevInput *input);
bool native_evdev_input_keyboard_active(NativeEvdevInput *input);

int native_evdev_input_wake_fd(const NativeEvdevInput *input);
void native_evdev_input_clear_wake(NativeEvdevInput *input);

NativeEventKind native_evdev_input_next_kind(NativeEvdevInput *input);
bool native_evdev_input_take_reset(NativeEvdevInput *input);

size_t native_evdev_input_pop_mouse_batch(NativeEvdevInput *input, NativeMouseEv *out, size_t cap);
size_t native_evdev_input_pop_keyboard_batch(NativeEvdevInput *input, NativeKeyboardEv *out, size_t cap);

#endif
