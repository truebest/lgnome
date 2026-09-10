#ifndef LGNOME_INPUT_EVENT_QUEUE_H
#define LGNOME_INPUT_EVENT_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum NativeMouseEvKind {
    NATIVE_MOUSE_EV_MOTION = 0, /* relative dx/dy */
    NATIVE_MOUSE_EV_BUTTON = 1, /* button down/up */
    NATIVE_MOUSE_EV_WHEEL = 2,  /* wheel delta */
} NativeMouseEvKind;

typedef struct NativeMouseEv {
    NativeMouseEvKind kind;
    int dx, dy;           /* MOTION: accumulated relative delta */
    uint8_t sdl_button;   /* BUTTON: SDL_BUTTON_* value */
    bool down;            /* BUTTON: press vs release */
    int wheel_x, wheel_y; /* WHEEL: axis deltas */
} NativeMouseEv;

typedef struct NativeKeyboardEv {
    uint16_t code; /* Linux input event code (KEY_*) */
    bool down;     /* press (also autorepeat) vs release */
    /* True only for a TV-remote virtual node (RCU / LGE Network Input). This
     * lets app-level remote shortcuts coexist with the same keys on a physical
     * USB keyboard, which must keep flowing to RDP. */
    bool from_remote;
} NativeKeyboardEv;

typedef enum NativeEventKind {
    NATIVE_EVENT_NONE,
    NATIVE_EVENT_MOUSE,
    NATIVE_EVENT_KEYBOARD,
    NATIVE_EVENT_RESET,
} NativeEventKind;

typedef struct NativeEvent {
    uint64_t time_us; /* Kernel event time, common clock across devices. */
    NativeEventKind kind;
    union {
        NativeMouseEv mouse;
        NativeKeyboardEv keyboard;
    } data;
} NativeEvent;

#define NATIVE_EVENT_QUEUE_CAPACITY 1024u

typedef struct NativeEventQueue {
    NativeEvent events[NATIVE_EVENT_QUEUE_CAPACITY];
    unsigned head;
    unsigned count;
    bool reset_pending;
    bool publication_bounded;
    uint64_t published_before_us;
} NativeEventQueue;

/* Caller serializes access. Publish a complete device-read sweep before consuming:
 * device enumeration order must not replace kernel event order. Equal timestamps
 * preserve arrival order. Motion is coalesced only later, within ordered mouse runs. */
/* Only events older than a completed sweep's starting time are visible. */
void native_event_queue_publish_before(NativeEventQueue *queue, uint64_t time_us);
void native_event_queue_push(NativeEventQueue *queue, const NativeEvent *event);
NativeEventKind native_event_queue_next_kind(const NativeEventQueue *queue);
bool native_event_queue_pop(NativeEventQueue *queue, NativeEventKind kind, NativeEvent *event);
bool native_event_queue_take_reset(NativeEventQueue *queue);

#endif
