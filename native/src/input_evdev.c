#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "input_evdev.h"

#include "native_time.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <linux/input.h>
#include <libevdev/libevdev.h>
#include <SDL.h>

#include "clog.h"

clog_define(g_native_log_input, cLogLevelInfo, "input.evdev");

#define NATIVE_EVDEV_MAX_DEVICES 16
/* Consecutive poll() failures tolerated before the reader gives up (see evdev_thread). */
#define NATIVE_EVDEV_MAX_POLL_FAILURES 100

typedef struct NativeEvdevDevice {
    int fd;
    struct libevdev *dev;
    /* Combined keyboard/trackpad nodes have independent keyboard and pointer capabilities. */
    bool is_mouse;
    bool is_keyboard;
    /* A TV-remote virtual node, recognized by its stable webOS device name.
     * Its confirm key has app-level meaning while physical keyboard Enter does not. */
    bool is_remote;
    char path[64]; /* /dev/input node path, used to de-duplicate hotplug re-scans */
} NativeEvdevDevice;

typedef struct NativeEvdevBackend {
    NativeEvdevDevice devices[NATIVE_EVDEV_MAX_DEVICES];
    int ndevices;
    int inotify_fd; /* watches /dev/input for hotplugged nodes; -1 when unavailable */
} NativeEvdevBackend;

typedef struct NativeEvdevDrainResult {
    bool pushed;
    bool remove;
    bool pending; /* Stopped at the cutoff; libevdev may still buffer events. */
} NativeEvdevDrainResult;

static bool evdev_is_mouse(const struct libevdev *dev) {
    return libevdev_has_event_code(dev, EV_KEY, BTN_MOUSE) && libevdev_has_event_code(dev, EV_REL, REL_X) &&
           libevdev_has_event_code(dev, EV_REL, REL_Y);
}

/* Reject full-keymap TV remotes with pointer buttons but no relative-mouse capability. */
static bool evdev_is_keyboard(const struct libevdev *dev) {
    if (!(libevdev_has_event_code(dev, EV_KEY, KEY_A) && libevdev_has_event_code(dev, EV_KEY, KEY_Z) &&
          libevdev_has_event_code(dev, EV_KEY, KEY_SPACE))) {
        return false;
    }
    bool has_pointer_buttons =
        libevdev_has_event_code(dev, EV_KEY, BTN_LEFT) || libevdev_has_event_code(dev, EV_KEY, BTN_MOUSE);
    return !has_pointer_buttons || evdev_is_mouse(dev);
}

/* Stops and returns true when visit(path, user) returns true. */
typedef bool (*NativeEvdevNodeVisitor)(const char *path, void *user);

static bool evdev_for_each_input_node(NativeEvdevNodeVisitor visit, void *user) {
    DIR *dir = opendir("/dev/input");
    if (!dir) {
        return false;
    }
    bool stopped = false;
    struct dirent *ent;
    while (!stopped && (ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0) {
            continue;
        }
        char path[300];
        (void)snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        stopped = visit(path, user);
    }
    closedir(dir);
    return stopped;
}

static bool evdev_probe_keyboard_node(const char *path, void *user) {
    (void)user;
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }
    struct libevdev *dev = NULL;
    bool is_keyboard = false;
    if (libevdev_new_from_fd(fd, &dev) == 0) {
        is_keyboard = evdev_is_keyboard(dev);
        libevdev_free(dev);
    }
    close(fd);
    return is_keyboard; /* stop at the first keyboard found */
}

bool native_evdev_input_probe_keyboard(void) {
    return evdev_for_each_input_node(evdev_probe_keyboard_node, NULL);
}

static bool evdev_probe_mouse_node(const char *path, void *user) {
    (void)user;
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }
    struct libevdev *dev = NULL;
    bool is_mouse = false;
    if (libevdev_new_from_fd(fd, &dev) == 0) {
        is_mouse = evdev_is_mouse(dev);
        libevdev_free(dev);
    }
    close(fd);
    return is_mouse; /* stop at the first relative mouse found */
}

bool native_evdev_input_probe_mouse(void) {
    return evdev_for_each_input_node(evdev_probe_mouse_node, NULL);
}

static uint8_t evdev_mouse_button(uint16_t code) {
    switch (code) {
    case BTN_LEFT:
        return SDL_BUTTON_LEFT;
    case BTN_RIGHT:
        return SDL_BUTTON_RIGHT;
    case BTN_MIDDLE:
        return SDL_BUTTON_MIDDLE;
    case BTN_SIDE:
        return SDL_BUTTON_X1;
    case BTN_EXTRA:
        return SDL_BUTTON_X2;
    case BTN_FORWARD:
        return SDL_BUTTON_X2 + 1;
    case BTN_BACK:
        return SDL_BUTTON_X2 + 2;
    case BTN_TASK:
        return SDL_BUTTON_X2 + 3;
    default:
        return 0;
    }
}

/* The reader holds input->lock across the complete device sweep. */
static uint64_t evdev_event_time(const struct input_event *raw) {
    return (uint64_t)raw->time.tv_sec * 1000000u + (uint64_t)raw->time.tv_usec;
}

static bool evdev_dispatch_mouse(NativeEvdevInput *input, const struct input_event *raw) {
    NativeMouseEv ev;
    memset(&ev, 0, sizeof(ev));
    switch (raw->type) {
    case EV_REL:
        switch (raw->code) {
        case REL_X:
            ev.kind = NATIVE_MOUSE_EV_MOTION;
            ev.dx = raw->value;
            break;
        case REL_Y:
            ev.kind = NATIVE_MOUSE_EV_MOTION;
            ev.dy = raw->value;
            break;
        case REL_HWHEEL:
            ev.kind = NATIVE_MOUSE_EV_WHEEL;
            ev.wheel_x = raw->value;
            break;
        case REL_WHEEL:
            ev.kind = NATIVE_MOUSE_EV_WHEEL;
            ev.wheel_y = raw->value;
            break;
        default:
            return false;
        }
        break;
    case EV_KEY:
        if (raw->code < BTN_LEFT || raw->code > BTN_TASK) {
            return false;
        }
        ev.kind = NATIVE_MOUSE_EV_BUTTON;
        ev.sdl_button = evdev_mouse_button((uint16_t)raw->code);
        if (ev.sdl_button == 0) {
            return false;
        }
        ev.down = raw->value != 0;
        break;
    default:
        return false;
    }
    atomic_fetch_add(&input->event_count, 1u);
    NativeEvent event = {.time_us = evdev_event_time(raw), .kind = NATIVE_EVENT_MOUSE};
    event.data.mouse = ev;
    native_event_queue_push(&input->queue, &event);
    return true;
}

static bool evdev_dispatch_keyboard(NativeEvdevInput *input, const NativeEvdevDevice *device,
                                    const struct input_event *raw) {
    if (raw->type != EV_KEY) {
        return false;
    }
    atomic_fetch_add(&input->event_count, 1u);
    NativeEvent event = {.time_us = evdev_event_time(raw), .kind = NATIVE_EVENT_KEYBOARD};
    event.data.keyboard = (NativeKeyboardEv){
        .code = (uint16_t)raw->code, .down = raw->value != 0, .from_remote = device->is_remote,
    };
    native_event_queue_push(&input->queue, &event);
    return true;
}

static bool evdev_dispatch(NativeEvdevInput *input, const NativeEvdevDevice *device, const struct input_event *raw) {
    switch (raw->type) {
    case EV_REL:
        /* Relative motion/wheel is always pointer data. */
        return device->is_mouse ? evdev_dispatch_mouse(input, raw) : false;
    case EV_KEY:
        /* Pointer buttons (BTN_LEFT..BTN_TASK) are mouse events; every other EV_KEY is
         * a typing key. Both share the same ordered queue. */
        if (raw->code >= BTN_LEFT && raw->code <= BTN_TASK) {
            return device->is_mouse ? evdev_dispatch_mouse(input, raw) : false;
        }
        /* Remote navigation keys can arrive on mouse-only nodes. */
        bool remote_confirm = device->is_remote &&
                              (raw->code == KEY_ENTER || raw->code == KEY_KPENTER || raw->code == KEY_OK);
        if ((raw->code >= KEY_RED && raw->code <= KEY_CHANNELDOWN) || remote_confirm) {
            return evdev_dispatch_keyboard(input, device, raw);
        }
        return device->is_keyboard ? evdev_dispatch_keyboard(input, device, raw) : false;
    default:
        return false;
    }
}

static void evdev_notify_main(NativeEvdevInput *input) {
    if (!input || input->wake_fd < 0) {
        return;
    }
    uint64_t one = 1;
    if (write(input->wake_fd, &one, sizeof(one)) < 0 && errno != EAGAIN) {
        clog(cLogLevelWarning, "failed to signal input eventfd: %s", strerror(errno));
    }
}

static NativeEvdevDrainResult evdev_drain_sync(NativeEvdevInput *input, NativeEvdevDevice *device) {
    NativeEvdevDrainResult result = {0};
    for (;;) {
        struct input_event raw;
        int ret = libevdev_next_event(device->dev, LIBEVDEV_READ_FLAG_SYNC, &raw);
        if (ret == LIBEVDEV_READ_STATUS_SYNC) {
            result.pushed = evdev_dispatch(input, device, &raw) || result.pushed;
        } else if (ret == -EAGAIN) {
            break;
        } else {
            clog(cLogLevelWarning, "libevdev sync failed on fd %d: %s", device->fd, strerror(-ret));
            result.remove = true;
            break;
        }
    }
    return result;
}

static NativeEvdevDrainResult evdev_drain_device(NativeEvdevInput *input, NativeEvdevDevice *device,
                                                uint64_t before_us) {
    NativeEvdevDrainResult result = {0};
    for (;;) {
        struct input_event raw;
        int ret = libevdev_next_event(device->dev, LIBEVDEV_READ_FLAG_NORMAL, &raw);
        if (ret == LIBEVDEV_READ_STATUS_SUCCESS) {
            result.pushed = evdev_dispatch(input, device, &raw) || result.pushed;
            /* Keep the first newer event queued but unpublished. Do not chase a
             * continuously busy mouse while holding the consumer mutex. */
            if (evdev_event_time(&raw) >= before_us) {
                result.pending = true;
                break;
            }
        } else if (ret == LIBEVDEV_READ_STATUS_SYNC) {
            NativeEvdevDrainResult sync_result = evdev_drain_sync(input, device);
            result.pushed = sync_result.pushed || result.pushed;
            if (sync_result.remove) {
                result.remove = true;
                break;
            }
        } else if (ret == -EAGAIN) {
            break;
        } else {
            clog(cLogLevelWarning, "libevdev read failed on fd %d: %s", device->fd, strerror(-ret));
            result.remove = true;
            break;
        }
    }
    return result;
}

static void evdev_close_device(NativeEvdevDevice *device) {
    if (!device) {
        return;
    }
    if (device->dev) {
        int ret = libevdev_grab(device->dev, LIBEVDEV_UNGRAB);
        if (ret < 0 && ret != -ENODEV) {
            clog(cLogLevelWarning, "failed to ungrab fd %d: %s", device->fd, strerror(-ret));
        }
        libevdev_free(device->dev);
        device->dev = NULL;
    }
    if (device->fd >= 0) {
        close(device->fd);
        device->fd = -1;
    }
}

static void evdev_update_active_flags(NativeEvdevInput *input, const NativeEvdevBackend *backend) {
    bool mouse_active = false;
    bool keyboard_active = false;
    if (backend) {
        for (int i = 0; i < backend->ndevices; i++) {
            if (backend->devices[i].is_mouse) {
                mouse_active = true;
            }
            if (backend->devices[i].is_keyboard) {
                keyboard_active = true;
            }
        }
    }
    atomic_store(&input->mouse_active, mouse_active);
    atomic_store(&input->keyboard_active, keyboard_active);
}

static const char *evdev_device_kind_name(const NativeEvdevDevice *device) {
    if (device->is_mouse && device->is_keyboard) {
        return "keyboard+mouse";
    }
    if (device->is_mouse) {
        return "mouse";
    }
    if (device->is_keyboard) {
        return "keyboard";
    }
    return "unknown";
}

static void evdev_remove_device(NativeEvdevInput *input, NativeEvdevBackend *backend, int index) {
    if (!input || !backend || index < 0 || index >= backend->ndevices) {
        return;
    }
    NativeEvdevDevice removed = backend->devices[index];
    clog(cLogLevelNotice, "removing %s fd %d after disconnect/error", evdev_device_kind_name(&removed),
         removed.fd);
    evdev_close_device(&removed);
    for (int i = index; i + 1 < backend->ndevices; i++) {
        backend->devices[i] = backend->devices[i + 1];
    }
    backend->ndevices--;
    backend->devices[backend->ndevices].fd = -1;
    backend->devices[backend->ndevices].dev = NULL;
    evdev_update_active_flags(input, backend);
}

/* Release devices without freeing backend/inotify state; stop frees it after joining. */
static void evdev_release_devices(NativeEvdevInput *input, NativeEvdevBackend *backend) {
    for (int i = 0; i < backend->ndevices; i++) {
        evdev_close_device(&backend->devices[i]);
    }
    backend->ndevices = 0;
    evdev_update_active_flags(input, backend);
}

static void evdev_close_backend(NativeEvdevBackend *backend) {
    if (!backend) {
        return;
    }
    for (int i = 0; i < backend->ndevices; i++) {
        evdev_close_device(&backend->devices[i]);
    }
    backend->ndevices = 0;
    if (backend->inotify_fd >= 0) {
        close(backend->inotify_fd);
        backend->inotify_fd = -1;
    }
    free(backend);
}

static bool evdev_backend_has_path(const NativeEvdevBackend *backend, const char *path) {
    for (int i = 0; i < backend->ndevices; i++) {
        if (strcmp(backend->devices[i].path, path) == 0) {
            return true;
        }
    }
    return false;
}

static bool evdev_add_device(NativeEvdevInput *input, NativeEvdevBackend *backend, const char *path) {
    /* Skip before open: a duplicate EVIOCGRAB fails with EBUSY. */
    if (backend->ndevices >= NATIVE_EVDEV_MAX_DEVICES || evdev_backend_has_path(backend, path)) {
        return false;
    }

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        if (errno != ENXIO && errno != EACCES) {
            clog(cLogLevelWarning, "failed to open %s: %s", path, strerror(errno));
        }
        return false;
    }

    /* Configure the fd before libevdev reads any events: all timestamps and
     * publication cutoffs must use one clock unaffected by wall-clock changes. */
    int clock_id = CLOCK_MONOTONIC;
    if (ioctl(fd, EVIOCSCLOCKID, &clock_id) < 0) {
        clog(cLogLevelWarning, "cannot select monotonic input clock for %s: %s", path, strerror(errno));
        close(fd);
        return false;
    }
    struct libevdev *dev = NULL;
    int ret = libevdev_new_from_fd(fd, &dev);
    if (ret < 0) {
        close(fd);
        return false;
    }

    bool is_keyboard = evdev_is_keyboard(dev);
    bool is_mouse = evdev_is_mouse(dev);
    if (!is_keyboard && !is_mouse) {
        libevdev_free(dev);
        close(fd);
        return false;
    }

    NativeEvdevDevice *device = &backend->devices[backend->ndevices];
    device->fd = fd;
    device->dev = dev;
    device->is_mouse = is_mouse;
    device->is_keyboard = is_keyboard;
    /* webOS exposes the Magic Remote through virtual nodes named "Smart Remote RCU
     * Input" / "LGE Network Input" (network-remote app buttons ride the latter). */
    const char *dev_name = libevdev_get_name(dev);
    device->is_remote = dev_name && (strstr(dev_name, "RCU") || strstr(dev_name, "LGE Network"));
    (void)snprintf(device->path, sizeof(device->path), "%s", path);
    const char *kind_name = evdev_device_kind_name(device);

    ret = libevdev_grab(dev, LIBEVDEV_GRAB);
    if (ret < 0) {
        clog(cLogLevelWarning, "failed to grab %s %s: %s (skipping device)", kind_name, path,
             strerror(-ret));
        libevdev_free(dev);
        close(fd);
        device->dev = NULL;
        device->fd = -1;
        return false;
    } else {
        clog(cLogLevelInfo, "grabbed %s %s (%s)", kind_name, path, libevdev_get_name(dev));
    }

    backend->ndevices++;
    if (is_mouse) {
        atomic_store(&input->mouse_active, true);
    }
    if (is_keyboard) {
        atomic_store(&input->keyboard_active, true);
    }
    return true;
}

struct evdev_open_context {
    NativeEvdevInput *input;
    NativeEvdevBackend *backend;
};

static bool evdev_open_visit(const char *path, void *user) {
    struct evdev_open_context *ctx = (struct evdev_open_context *)user;
    (void)evdev_add_device(ctx->input, ctx->backend, path);
    return ctx->backend->ndevices >= NATIVE_EVDEV_MAX_DEVICES; /* stop once the table is full */
}

static NativeEvdevBackend *evdev_open_backend(NativeEvdevInput *input) {
    NativeEvdevBackend *backend = calloc(1, sizeof(*backend));
    if (!backend) {
        return NULL;
    }
    for (int i = 0; i < NATIVE_EVDEV_MAX_DEVICES; i++) {
        backend->devices[i].fd = -1;
    }

    backend->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (backend->inotify_fd >= 0 && inotify_add_watch(backend->inotify_fd, "/dev/input", IN_CREATE | IN_ATTRIB) < 0) {
        clog(cLogLevelWarning, "inotify watch on /dev/input failed (%s); hotplug add disabled",
             strerror(errno));
        close(backend->inotify_fd);
        backend->inotify_fd = -1;
    }

    struct evdev_open_context ctx = {input, backend};
    (void)evdev_for_each_input_node(evdev_open_visit, &ctx);

    if (backend->ndevices == 0 && backend->inotify_fd < 0) {
        evdev_close_backend(backend);
        return NULL;
    }
    return backend;
}

/* Rescan all nodes: unplug/replug may renumber them. Returns whether the set changed. */
static bool evdev_handle_hotplug(NativeEvdevInput *input, NativeEvdevBackend *backend) {
    if (backend->inotify_fd < 0) {
        return false;
    }
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    while (read(backend->inotify_fd, buf, sizeof(buf)) > 0) {
        /* contents intentionally ignored; the rescan below is the source of truth */
    }

    int before = backend->ndevices;
    struct evdev_open_context ctx = {input, backend};
    (void)evdev_for_each_input_node(evdev_open_visit, &ctx);
    return backend->ndevices != before;
}

static void *evdev_thread(void *arg) {
    NativeEvdevInput *input = (NativeEvdevInput *)arg;
    NativeEvdevBackend *backend = (NativeEvdevBackend *)input->backend;
    /* +2 for the stop eventfd and the /dev/input inotify fd. */
    struct pollfd fds[NATIVE_EVDEV_MAX_DEVICES + 2];
    int poll_failures = 0;
    bool queued = false;

    while (atomic_load(&input->running)) {
        fds[0].fd = input->stop_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = backend->inotify_fd; /* a negative fd is ignored by poll() */
        fds[1].events = POLLIN;
        fds[1].revents = 0;
        for (int i = 0; i < backend->ndevices; i++) {
            fds[i + 2].fd = backend->devices[i].fd;
            fds[i + 2].events = POLLIN;
            fds[i + 2].revents = 0;
        }

        /* A newer event may already be buffered in libevdev or the shared
         * queue, with no kernel fd readiness left to wake the next sweep. */
        int ret = poll(fds, (nfds_t)(backend->ndevices + 2), queued ? 1 : -1);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            clog(cLogLevelWarning, "poll failed: %s", strerror(errno));
            if (++poll_failures >= NATIVE_EVDEV_MAX_POLL_FAILURES) {
                clog(cLogLevelWarning, "too many consecutive poll failures; stopping evdev reader");
                /* Hand the devices back to the compositor before quitting, otherwise they stay
                 * EVIOCGRAB-captured and the SDL fallback would have nothing to read. */
                evdev_release_devices(input, backend);
                atomic_store(&input->running, false);
                break;
            }
            native_sleep_ms(10u);
            continue;
        }
        poll_failures = 0;
        if (fds[0].revents & POLLIN) {
            uint64_t value;
            while (read(input->stop_fd, &value, sizeof(value)) == (ssize_t)sizeof(value)) {
            }
            break;
        }

        bool pushed = false;
        bool changed = false;
        bool read_pending = false;
        struct timespec sweep_start;
        clock_gettime(CLOCK_MONOTONIC, &sweep_start);
        uint64_t before_us = (uint64_t)sweep_start.tv_sec * 1000000u +
                             (uint64_t)sweep_start.tv_nsec / 1000u;
        pthread_mutex_lock(&input->lock);
        /* Handle disconnects first so the hotplug rescan below sees an accurate device set
         * (e.g. an unplug/replug that reuses the same eventN node). */
        for (int i = backend->ndevices - 1; i >= 0; i--) {
            /* Read every nonblocking fd, including devices which became ready
             * after poll returned, before publishing this sweep. */
            NativeEvdevDrainResult drain = evdev_drain_device(input, &backend->devices[i], before_us);
            pushed = drain.pushed || pushed;
            read_pending = drain.pending || read_pending;
            if (drain.remove || (fds[i + 2].revents & (POLLERR | POLLHUP | POLLNVAL))) {
                evdev_remove_device(input, backend, i);
                changed = true;
            }
        }
        native_event_queue_publish_before(&input->queue, before_us);
        queued = read_pending || input->queue.count != 0;
        bool ready = native_event_queue_next_kind(&input->queue) != NATIVE_EVENT_NONE;
        pthread_mutex_unlock(&input->lock);
        /* Then pick up devices connected since the last scan. */
        if (fds[1].revents & POLLIN) {
            changed = evdev_handle_hotplug(input, backend) || changed;
        }
        /* Wake the main loop on new input and on a device-set change so it re-reads the
         * mouse/keyboard active flags and starts (or stops) draining promptly. */
        if (pushed || changed || ready) {
            evdev_notify_main(input);
        }
    }
    return NULL;
}

bool native_evdev_input_start(NativeEvdevInput *input) {
    if (!input || input->started) {
        return input && input->started;
    }
    memset(input, 0, sizeof(*input));
    input->wake_fd = -1;
    input->stop_fd = -1;
    native_event_queue_publish_before(&input->queue, 0);
    pthread_mutex_init(&input->lock, NULL);
    input->lock_initialized = true;
    atomic_init(&input->running, false);
    atomic_init(&input->mouse_active, false);
    atomic_init(&input->keyboard_active, false);
    atomic_init(&input->event_count, 0u);

    input->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    input->stop_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (input->wake_fd < 0 || input->stop_fd < 0) {
        clog(cLogLevelWarning, "eventfd() failed; evdev input is unavailable: %s", strerror(errno));
        native_evdev_input_stop(input);
        return false;
    }

    NativeEvdevBackend *backend = evdev_open_backend(input);
    if (!backend) {
        clog(cLogLevelInfo, "no USB mouse/keyboard devices found for evdev grab");
        native_evdev_input_stop(input);
        return false;
    }
    input->backend = backend;

    atomic_store(&input->running, true);
    if (pthread_create(&input->thread, NULL, evdev_thread, input) != 0) {
        clog(cLogLevelWarning, "failed to start evdev reader thread");
        atomic_store(&input->running, false);
        native_evdev_input_stop(input);
        return false;
    }
    input->started = true;
    clog(cLogLevelInfo, "libevdev reader started (%d device(s), mouse=%d keyboard=%d)",
         backend->ndevices, atomic_load(&input->mouse_active) ? 1 : 0,
         atomic_load(&input->keyboard_active) ? 1 : 0);
    return true;
}

void native_evdev_input_stop(NativeEvdevInput *input) {
    native_evdev_input_stop_with_buttons(input, NULL);
}

void native_evdev_input_stop_with_buttons(NativeEvdevInput *input, uint8_t *buttons) {
    if (!input) {
        return;
    }
    if (!input->lock_initialized) {
        /* App is zero-initialized, so wake_fd/stop_fd are 0 until start() sets them to -1.
         * A stop before start must not close fd 0 or whatever resource reused it. */
        return;
    }
    if (input->started) {
        atomic_store(&input->running, false);
        if (input->stop_fd >= 0) {
            uint64_t one = 1;
            (void)write(input->stop_fd, &one, sizeof(one));
        }
        pthread_join(input->thread, NULL);
    }

    if (input->backend) {
        NativeEvdevBackend *backend = input->backend;
        if (buttons) {
            uint8_t held = 0;
            bool has_mouse = false;
            /* Query kernel button state after join/ungrab; queued releases may never have reached SDL. */
            for (int i = 0; i < backend->ndevices; i++) {
                NativeEvdevDevice *device = &backend->devices[i];
                if (!device->is_mouse) {
                    continue;
                }
                has_mouse = true;
                (void)libevdev_grab(device->dev, LIBEVDEV_UNGRAB);
                unsigned char keys[(KEY_MAX + 8u) / 8u] = {0};
                if (ioctl(device->fd, EVIOCGKEY(sizeof(keys)), keys) < 0) {
                    clog_limited(cLogLevelWarning, 2, 5000,
                                 "cannot read mouse buttons during input handoff: %s", strerror(errno));
                    continue;
                }
                for (unsigned code = BTN_LEFT; code <= BTN_TASK; code++) {
                    uint8_t button = evdev_mouse_button((uint16_t)code);
                    if (button >= 1 && button <= SDL_BUTTON_X2 && (keys[code / 8u] & (1u << (code % 8u)))) {
                        held |= (uint8_t)(1u << (button - 1u));
                    }
                }
            }
            if (has_mouse) {
                *buttons = held;
            }
        }
        evdev_close_backend((NativeEvdevBackend *)input->backend);
        input->backend = NULL;
    }
    if (input->wake_fd >= 0) {
        close(input->wake_fd);
        input->wake_fd = -1;
    }
    if (input->stop_fd >= 0) {
        close(input->stop_fd);
        input->stop_fd = -1;
    }
    if (input->lock_initialized) {
        pthread_mutex_destroy(&input->lock);
        input->lock_initialized = false;
    }
    input->started = false;
    atomic_store(&input->mouse_active, false);
    atomic_store(&input->keyboard_active, false);
}

bool native_evdev_input_active(const NativeEvdevInput *input) {
    return input && input->started && atomic_load(&input->running);
}

NativeEventKind native_evdev_input_next_kind(NativeEvdevInput *input) {
    if (!input || !input->started) {
        return NATIVE_EVENT_NONE;
    }
    pthread_mutex_lock(&input->lock);
    NativeEventKind kind = native_event_queue_next_kind(&input->queue);
    pthread_mutex_unlock(&input->lock);
    return kind;
}

bool native_evdev_input_take_reset(NativeEvdevInput *input) {
    if (!input || !input->started) {
        return false;
    }
    pthread_mutex_lock(&input->lock);
    bool reset = native_event_queue_take_reset(&input->queue);
    pthread_mutex_unlock(&input->lock);
    return reset;
}

bool native_evdev_input_mouse_active(NativeEvdevInput *input) {
    return input && input->started &&
           (atomic_load(&input->mouse_active) || native_evdev_input_next_kind(input) == NATIVE_EVENT_MOUSE);
}

bool native_evdev_input_keyboard_active(NativeEvdevInput *input) {
    return input && input->started &&
           (atomic_load(&input->keyboard_active) || native_evdev_input_next_kind(input) == NATIVE_EVENT_KEYBOARD);
}

int native_evdev_input_wake_fd(const NativeEvdevInput *input) {
    if (!native_evdev_input_active(input) || input->wake_fd < 0) {
        return -1;
    }
    return input->wake_fd;
}

void native_evdev_input_clear_wake(NativeEvdevInput *input) {
    if (!input || input->wake_fd < 0) {
        return;
    }
    uint64_t value;
    while (read(input->wake_fd, &value, sizeof(value)) == (ssize_t)sizeof(value)) {
    }
}

size_t native_evdev_input_pop_mouse_batch(NativeEvdevInput *input, NativeMouseEv *out, size_t cap) {
    if (!input || !input->started || !out || cap == 0) {
        return 0;
    }
    size_t count = 0;
    pthread_mutex_lock(&input->lock);
    NativeEvent event;
    while (count < cap && native_event_queue_pop(&input->queue, NATIVE_EVENT_MOUSE, &event)) {
        out[count++] = event.data.mouse;
    }
    pthread_mutex_unlock(&input->lock);
    return count;
}

size_t native_evdev_input_pop_keyboard_batch(NativeEvdevInput *input, NativeKeyboardEv *out, size_t cap) {
    if (!input || !input->started || !out || cap == 0) {
        return 0;
    }
    size_t count = 0;
    pthread_mutex_lock(&input->lock);
    NativeEvent event;
    while (count < cap && native_event_queue_pop(&input->queue, NATIVE_EVENT_KEYBOARD, &event)) {
        out[count++] = event.data.keyboard;
    }
    pthread_mutex_unlock(&input->lock);
    return count;
}
