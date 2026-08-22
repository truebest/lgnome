#ifndef GNOMECAST_CAMERA_V4L2_H
#define GNOMECAST_CAMERA_V4L2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NATIVE_CAMERA_DEVICE_ID_MAX 512u
#define NATIVE_CAMERA_DEVICE_NAME_MAX 128u
#define NATIVE_CAMERA_DEVICE_PATH_MAX 64u

/* Accepted RDPECAM geometry. The offered modes come from the camera itself; these
 * are sanity bounds for untrusted config, not a curated list. The lower bounds keep
 * thumbnail-sized modes out of the drawer; the upper bounds only have to sit above
 * anything a real camera reports, and are checked before any buffer is sized. */
#define NATIVE_CAMERA_MIN_WIDTH 640u
#define NATIVE_CAMERA_MAX_WIDTH 1920u
#define NATIVE_CAMERA_MIN_HEIGHT 480u
#define NATIVE_CAMERA_MAX_HEIGHT 1080u
#define NATIVE_CAMERA_MAX_FPS 30u
/* The rate list must hold every accepted whole rate: native_camera_h264_mode_available()
 * answers from it, so a truncated tail would report a mode the camera really has as
 * unavailable and block capture. The size list is enumeration scratch, sized well past
 * the in-envelope sizes any camera reports so that callers filter the whole list rather
 * than a largest-N prefix; the drawer applies its own, smaller display cap afterwards. */
#define NATIVE_CAMERA_FRAME_RATE_MAX NATIVE_CAMERA_MAX_FPS
#define NATIVE_CAMERA_FRAME_SIZE_MAX 32u
#define NATIVE_CAMERA_H264_MAX_AU_BYTES ((size_t)4u * 1024u * 1024u)

typedef struct NativeCameraDeviceInfo {
    char id[NATIVE_CAMERA_DEVICE_ID_MAX];
    char name[NATIVE_CAMERA_DEVICE_NAME_MAX];
    char path[NATIVE_CAMERA_DEVICE_PATH_MAX];
} NativeCameraDeviceInfo;

typedef struct NativeCameraCapture NativeCameraCapture;

/* One capture buffer borrowed from the driver. `data` points straight at the
 * mmap'd V4L2 buffer, so it stays valid only until the matching release. */
typedef struct NativeCameraFrame {
    const uint8_t *data;
    size_t len;
    uint64_t timestamp_us;
    uint32_t index;
    uint32_t sequence;
    uint32_t flags;
    bool held;
} NativeCameraFrame;

typedef enum NativeCameraAcquireResult {
    NATIVE_CAMERA_ACQUIRE_RECOVER = -1,
    NATIVE_CAMERA_ACQUIRE_TIMEOUT = 0,
    NATIVE_CAMERA_ACQUIRE_FRAME = 1,
} NativeCameraAcquireResult;

/* Convert one packed V4L2 YUYV frame to SDL-friendly RGBA bytes. Exposed so the
 * color conversion and its stride/bounds handling can be covered on host builds. */
bool native_camera_yuyv_to_rgba(const uint8_t *source, size_t source_len,
                                uint32_t width, uint32_t height,
                                uint32_t source_pitch, uint8_t *rgba,
                                size_t rgba_len);

/* Device enumeration never starts streaming and returns only single-plane capture
 * nodes with a non-emulated V4L2_PIX_FMT_H264 format. Empty device_id in the H.264
 * capture open means Auto (the first compatible node). Explicit IDs never fall back
 * to a different camera. */
/* Returns the number of entries WRITTEN (never more than capacity). */
size_t native_camera_enumerate(NativeCameraDeviceInfo *devices, size_t capacity);

typedef enum NativeCameraRateQuery {
    /* The device is absent, cannot be opened, or only advertises stepwise
     * intervals. Callers must not conclude anything about the size. */
    NATIVE_CAMERA_RATES_UNKNOWN = 0,
    /* The device answered and has no native-H.264 mode at this size. */
    NATIVE_CAMERA_RATES_UNSUPPORTED,
    /* Discrete rates were written to the caller's array, largest first. */
    NATIVE_CAMERA_RATES_LISTED,
} NativeCameraRateQuery;

/* Whole native-H.264 frame rates inside the supported FPS envelope that the
 * device offers at this exact size. */
NativeCameraRateQuery native_camera_enumerate_frame_rates(const char *device_id,
                                                          uint32_t width,
                                                          uint32_t height,
                                                          uint16_t *rates,
                                                          size_t capacity,
                                                          size_t *count);

/* Discrete native-H.264 sizes the device offers, largest area first. Written as
 * {width, height} pairs. Sizes outside the envelope, odd ones the capture open
 * would refuse, and duplicates across a camera's nodes are dropped. */
NativeCameraRateQuery native_camera_enumerate_frame_sizes(const char *device_id,
                                                          uint16_t (*sizes)[2],
                                                          size_t capacity,
                                                          size_t *count);
/* Exact-rate predicate shared by the mode UI and capture compatibility probe. */
bool native_camera_frame_rate_list_contains(const uint16_t *rates, size_t count,
                                            uint32_t fps);
bool native_camera_h264_mode_available(const char *device_id, uint32_t width,
                                       uint32_t height, uint32_t fps);
bool native_camera_capture_open_h264(const char *device_id, uint32_t width,
                                     uint32_t height, uint32_t fps,
                                     NativeCameraCapture **out_capture);
/* Launch-only local diagnostic. This is deliberately YUYV-only and is not an
 * RDPECAM compatibility probe. */
bool native_camera_capture_open_yuyv_preview(uint32_t width, uint32_t height,
                                             uint32_t fps,
                                             NativeCameraCapture **out_capture);
void native_camera_capture_close(NativeCameraCapture *capture);
uint32_t native_camera_capture_width(const NativeCameraCapture *capture);
uint32_t native_camera_capture_height(const NativeCameraCapture *capture);
uint32_t native_camera_capture_pitch(const NativeCameraCapture *capture);
/* Borrows the next frame without copying it. timeout_ms < 0 waits indefinitely.
 * Every successful acquire must be paired with a release, which returns the
 * buffer to the driver's queue; holding one starves the queue by that buffer. */
NativeCameraAcquireResult native_camera_capture_acquire(NativeCameraCapture *capture,
                                                        int timeout_ms,
                                                        NativeCameraFrame *frame);
void native_camera_capture_release(NativeCameraCapture *capture,
                                   NativeCameraFrame *frame);

/* Best-effort request for an immediate IDR, used after a dropped-buffer gap so
 * the decoder seed returns in about one frame instead of a whole GOP. Cameras
 * that ignore the control keep their own cadence; no-op on non-H.264 captures. */
void native_camera_capture_request_keyframe(NativeCameraCapture *capture);

#endif
