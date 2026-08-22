#ifndef GNOMECAST_CAMERA_V4L2_INTERNAL_H
#define GNOMECAST_CAMERA_V4L2_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "camera_v4l2.h"

/* Pure boundary predicates shared by the V4L2 implementation and host tests.
 * Kernel constants are passed in by the caller so the public camera header does
 * not expose Linux-only types. */
bool native_camera_v4l2_format_is_native_h264(uint32_t pixel_format, uint32_t format_flags, bool single_plane);
bool native_camera_v4l2_mode_is_exact(uint32_t requested_pixel_format, uint32_t requested_width,
                                      uint32_t requested_height, uint32_t selected_pixel_format,
                                      uint32_t selected_width, uint32_t selected_height);

/* Retains one whole frame rate inside the supported config envelope, sorted
 * largest first and without duplicates. Returns whether the rate is retained. */
bool native_camera_v4l2_insert_frame_rate(uint16_t *rates, size_t capacity, size_t *count, uint32_t fps);

/* Same for one {width, height} pair, sorted by descending pixel area. Odd sizes
 * are rejected because the capture open refuses them. Returns whether the size
 * is retained. */
bool native_camera_v4l2_insert_frame_size(uint16_t (*sizes)[2], size_t capacity, size_t *count, uint32_t width,
                                          uint32_t height);

/* Imports the metadata returned by VIDIOC_DQBUF without trusting bytesused or
 * timeval fields. `compressed` selects the 4 MiB camera-AU limit; uncompressed
 * preview frames instead require `minimum_bytes`. */
bool native_camera_v4l2_import_frame(const uint8_t *mapped_data, size_t mapped_length, size_t bytesused,
                                     size_t minimum_bytes, bool compressed, uint32_t index, uint32_t sequence,
                                     uint32_t flags, int64_t timestamp_sec, int64_t timestamp_usec,
                                     NativeCameraFrame *frame);

#ifdef HELLOLG_CAMERA_V4L2_TESTING
/* Host-test seam for the two mode-enumeration walks. The device table and these
 * three node calls are the only kernel contact those walks make, so a fake
 * covers candidate matching, descriptor filtering and the tri-state verdict
 * without a camera. `request` stays an unsigned long for the same reason the
 * predicates above take plain integers: no Linux type leaks into the header.
 * Production builds compile the hook out entirely. */
typedef struct NativeCameraV4l2TestNodes {
    size_t (*enumerate)(NativeCameraDeviceInfo *devices, size_t capacity);
    int (*open_node)(const char *path);
    int (*node_ioctl)(int fd, unsigned long request, void *argument);
    void (*close_node)(int fd);
} NativeCameraV4l2TestNodes;

/* NULL restores the real V4L2 calls. */
void native_camera_v4l2_set_test_nodes(const NativeCameraV4l2TestNodes *nodes);
#endif

#endif
