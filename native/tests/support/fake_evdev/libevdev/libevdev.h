#ifndef LGNOME_TEST_LIBEVDEV_H
#define LGNOME_TEST_LIBEVDEV_H
#include <linux/input.h>
struct libevdev { int unused; };
enum libevdev_grab_mode { LIBEVDEV_GRAB = 3, LIBEVDEV_UNGRAB = 4 };
#define LIBEVDEV_READ_FLAG_SYNC 1u
#define LIBEVDEV_READ_FLAG_NORMAL 2u
#define LIBEVDEV_READ_STATUS_SUCCESS 0
#define LIBEVDEV_READ_STATUS_SYNC 1
int libevdev_next_event(struct libevdev *dev, unsigned flags, struct input_event *event);
int libevdev_grab(struct libevdev *dev, enum libevdev_grab_mode mode);
void libevdev_free(struct libevdev *dev);
int libevdev_new_from_fd(int fd, struct libevdev **dev);
int libevdev_has_event_code(const struct libevdev *dev, unsigned type, unsigned code);
const char *libevdev_get_name(const struct libevdev *dev);
#endif
