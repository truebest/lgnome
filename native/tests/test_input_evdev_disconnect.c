/* Run the real device-removal and event-queue path; replace only libevdev I/O. */
#include "../src/input_evdev.c"
#include <assert.h>

int libevdev_grab(struct libevdev *dev, enum libevdev_grab_mode mode) { (void)dev; (void)mode; return 0; }
void libevdev_free(struct libevdev *dev) { (void)dev; }
int libevdev_next_event(struct libevdev *dev, unsigned flags, struct input_event *event) {
    (void)dev; (void)flags; (void)event; return -EAGAIN;
}
int libevdev_new_from_fd(int fd, struct libevdev **dev) { (void)fd; (void)dev; return -ENODEV; }
int libevdev_has_event_code(const struct libevdev *dev, unsigned type, unsigned code) {
    (void)dev; (void)type; (void)code; return 0;
}
const char *libevdev_get_name(const struct libevdev *dev) { (void)dev; return "test"; }
int poll(struct pollfd *fds, nfds_t count, int timeout) {
    (void)fds; (void)count; (void)timeout; errno=EIO; return -1;
}
int nanosleep(const struct timespec *request, struct timespec *remaining) {
    (void)request; (void)remaining; return 0;
}

static void test_disconnect(bool mouse) {
    NativeEvdevInput input = {0};
    NativeEvdevBackend backend = {0};
    backend.ndevices = 2;
    backend.devices[0] = (NativeEvdevDevice){.fd = -1, .is_mouse = mouse, .is_keyboard = !mouse};
    backend.devices[1] = (NativeEvdevDevice){.fd = -1, .is_mouse = true};
    atomic_init(&input.mouse_active, true);
    atomic_init(&input.keyboard_active, !mouse);
    struct input_event down = {.type = EV_KEY, .code = mouse ? BTN_LEFT : KEY_LEFTSHIFT, .value = 1};
    assert(evdev_dispatch(&input, &backend.devices[0], &down));
    NativeEvent emitted;
    NativeEventKind kind = mouse ? NATIVE_EVENT_MOUSE : NATIVE_EVENT_KEYBOARD;
    assert(native_event_queue_pop(&input.queue, kind, &emitted));
    assert(mouse ? emitted.data.mouse.down : emitted.data.keyboard.down);
    /* Other-device events cannot overtake the release-all after the disconnect. */
    down.code = BTN_LEFT;
    assert(evdev_dispatch(&input, &backend.devices[1], &down));
    evdev_remove_device(&input, &backend, 0);
    assert(backend.ndevices == 1);
    assert(native_event_queue_next_kind(&input.queue) == NATIVE_EVENT_RESET);
    assert(native_event_queue_take_reset(&input.queue));
    assert(native_event_queue_next_kind(&input.queue) == NATIVE_EVENT_NONE);
    assert(evdev_dispatch(&input, &backend.devices[0], &down));
    assert(native_event_queue_pop(&input.queue, NATIVE_EVENT_MOUSE, &emitted));
}

static void test_terminal_poll_failure_publishes_reset(void) {
    NativeEvdevInput input = {.started=true, .stop_fd=-1};
    NativeEvdevBackend backend = {.ndevices=1, .inotify_fd=-1};
    backend.devices[0]=(NativeEvdevDevice){.fd=-1, .is_keyboard=true};
    input.backend=&backend;
    input.wake_fd=eventfd(0,EFD_NONBLOCK);
    assert(input.wake_fd>=0 && pthread_mutex_init(&input.lock,NULL)==0);
    atomic_init(&input.running,true); atomic_init(&input.keyboard_active,true);
    struct input_event down={.type=EV_KEY, .code=KEY_LEFTSHIFT, .value=1};
    assert(evdev_dispatch(&input,&backend.devices[0],&down));
    NativeEvent emitted; assert(native_event_queue_pop(&input.queue,NATIVE_EVENT_KEYBOARD,&emitted));
    evdev_thread(&input);
    assert(!native_evdev_input_active(&input) && backend.ndevices==0);
    assert(native_evdev_input_next_kind(&input)==NATIVE_EVENT_RESET);
    assert(native_evdev_input_take_reset(&input));
    uint64_t wake=0; assert(read(input.wake_fd,&wake,sizeof(wake))==sizeof(wake) && wake==1);
    close(input.wake_fd); assert(pthread_mutex_destroy(&input.lock)==0);
}

int main(void) {
    test_disconnect(false);
    test_disconnect(true);
    test_terminal_poll_failure_publishes_reset();
    return 0;
}
