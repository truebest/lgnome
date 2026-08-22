#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "camera_v4l2.h"
#include "camera_v4l2_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/v4l2-controls.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "clog.h"

clog_define(g_native_log_camera, cLogLevelInfo, cLogFlags_Default, "camera.v4l2", NULL);

_Static_assert(NATIVE_CAMERA_FRAME_RATE_MAX >= NATIVE_CAMERA_MAX_FPS,
               "frame-rate enumeration must retain every supported whole FPS");

bool native_camera_v4l2_format_is_native_h264(uint32_t pixel_format, uint32_t format_flags, bool single_plane) {
    return single_plane && pixel_format == V4L2_PIX_FMT_H264 && (format_flags & V4L2_FMT_FLAG_EMULATED) == 0u;
}

bool native_camera_v4l2_mode_is_exact(uint32_t requested_pixel_format, uint32_t requested_width,
                                      uint32_t requested_height, uint32_t selected_pixel_format,
                                      uint32_t selected_width, uint32_t selected_height) {
    return requested_width != 0u && requested_height != 0u && selected_pixel_format == requested_pixel_format &&
           selected_width == requested_width && selected_height == requested_height;
}

bool native_camera_v4l2_insert_frame_rate(uint16_t *rates, size_t capacity, size_t *count, uint32_t fps) {
    if (!rates || !count || capacity == 0u || *count > capacity || fps == 0u || fps > NATIVE_CAMERA_MAX_FPS) {
        return false;
    }
    for (size_t i = 0u; i < *count; i++) {
        if (rates[i] == fps) {
            return true;
        }
    }

    size_t position = *count;
    while (position > 0u && rates[position - 1u] < (uint16_t)fps) {
        position--;
    }
    if (position >= capacity) {
        return false;
    }
    size_t tail = *count < capacity ? *count : capacity - 1u;
    for (size_t move = tail; move > position; move--) {
        rates[move] = rates[move - 1u];
    }
    rates[position] = (uint16_t)fps;
    if (*count < capacity) {
        (*count)++;
    }
    return true;
}

bool native_camera_v4l2_insert_frame_size(uint16_t (*sizes)[2], size_t capacity, size_t *count, uint32_t width,
                                          uint32_t height) {
    if (!sizes || !count || capacity == 0u || *count > capacity) {
        return false;
    }
    /* Odd geometry is refused by the capture open, so never offer it. */
    if (width < NATIVE_CAMERA_MIN_WIDTH || width > NATIVE_CAMERA_MAX_WIDTH || (width & 1u) != 0u ||
        height < NATIVE_CAMERA_MIN_HEIGHT || height > NATIVE_CAMERA_MAX_HEIGHT || (height & 1u) != 0u) {
        return false;
    }
    for (size_t i = 0u; i < *count; i++) {
        if (sizes[i][0] == width && sizes[i][1] == height) {
            return true;
        }
    }

    uint32_t area = width * height;
    size_t position = *count;
    while (position > 0u && (uint32_t)sizes[position - 1u][0] * sizes[position - 1u][1] < area) {
        position--;
    }
    if (position >= capacity) {
        return false;
    }
    size_t tail = *count < capacity ? *count : capacity - 1u;
    for (size_t move = tail; move > position; move--) {
        sizes[move][0] = sizes[move - 1u][0];
        sizes[move][1] = sizes[move - 1u][1];
    }
    sizes[position][0] = (uint16_t)width;
    sizes[position][1] = (uint16_t)height;
    if (*count < capacity) {
        (*count)++;
    }
    return true;
}

bool native_camera_v4l2_import_frame(const uint8_t *mapped_data, size_t mapped_length, size_t bytesused,
                                     size_t minimum_bytes, bool compressed, uint32_t index, uint32_t sequence,
                                     uint32_t flags, int64_t timestamp_sec, int64_t timestamp_usec,
                                     NativeCameraFrame *frame) {
    if (!mapped_data || !frame || bytesused == 0u || bytesused > mapped_length ||
        (compressed && bytesused > NATIVE_CAMERA_H264_MAX_AU_BYTES) || (!compressed && bytesused < minimum_bytes) ||
        timestamp_sec < 0 || timestamp_usec < 0 || timestamp_usec >= 1000000 ||
        (uint64_t)timestamp_sec > (UINT64_MAX - (uint64_t)timestamp_usec) / 1000000u) {
        return false;
    }
    memset(frame, 0, sizeof(*frame));
    frame->data = mapped_data;
    frame->len = bytesused;
    frame->timestamp_us = (uint64_t)timestamp_sec * 1000000u + (uint64_t)timestamp_usec;
    frame->index = index;
    frame->sequence = sequence;
    frame->flags = flags;
    frame->held = true;
    return true;
}

static uint8_t native_camera_clamp_byte(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

static void native_camera_yuv_pixel(uint8_t y, int u, int v, uint8_t *rgba) {
    int c = (int)y - 16;
    if (c < 0) {
        c = 0;
    }
    rgba[0] = native_camera_clamp_byte((298 * c + 409 * v + 128) >> 8);
    rgba[1] = native_camera_clamp_byte((298 * c - 100 * u - 208 * v + 128) >> 8);
    rgba[2] = native_camera_clamp_byte((298 * c + 516 * u + 128) >> 8);
    rgba[3] = 255;
}

bool native_camera_yuyv_to_rgba(const uint8_t *source, size_t source_len, uint32_t width, uint32_t height,
                                uint32_t source_pitch, uint8_t *rgba, size_t rgba_len) {
    if (!source || !rgba || width == 0 || height == 0 || (width & 1u) != 0 || source_pitch < width * 2u) {
        return false;
    }
    if ((size_t)height > SIZE_MAX / (size_t)source_pitch || (size_t)width > SIZE_MAX / 4u ||
        (size_t)height > SIZE_MAX / ((size_t)width * 4u)) {
        return false;
    }
    size_t required_source = (size_t)height * source_pitch;
    size_t required_rgba = (size_t)width * height * 4u;
    if (source_len < required_source || rgba_len < required_rgba) {
        return false;
    }

    for (uint32_t row = 0; row < height; row++) {
        const uint8_t *src = source + (size_t)row * source_pitch;
        uint8_t *dst = rgba + (size_t)row * width * 4u;
        for (uint32_t column = 0; column < width; column += 2u) {
            int u = (int)src[1] - 128;
            int v = (int)src[3] - 128;
            native_camera_yuv_pixel(src[0], u, v, dst);
            native_camera_yuv_pixel(src[2], u, v, dst + 4);
            src += 4;
            dst += 8;
        }
    }
    return true;
}

#define NATIVE_CAMERA_BUFFER_COUNT 4u
#define NATIVE_CAMERA_SCAN_LIMIT 32u

typedef enum NativeCameraCaptureFormat {
    NATIVE_CAMERA_CAPTURE_H264,
    NATIVE_CAMERA_CAPTURE_YUYV_PREVIEW,
} NativeCameraCaptureFormat;

typedef struct NativeCameraBuffer {
    void *data;
    size_t length;
} NativeCameraBuffer;

struct NativeCameraCapture {
    int fd;
    char path[NATIVE_CAMERA_DEVICE_PATH_MAX];
    char id[NATIVE_CAMERA_DEVICE_ID_MAX];
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t source_pitch;
    size_t minimum_frame_bytes;
    bool compressed;
    NativeCameraBuffer buffers[NATIVE_CAMERA_BUFFER_COUNT];
    uint32_t buffer_count;
    bool streaming;
};

static int native_camera_ioctl(int fd, unsigned long request, void *argument) {
    int result;
    do {
        result = ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

static void native_camera_fourcc(char text[5], uint32_t format) {
    text[0] = (char)(format & 0xffu);
    text[1] = (char)((format >> 8) & 0xffu);
    text[2] = (char)((format >> 16) & 0xffu);
    text[3] = (char)((format >> 24) & 0xffu);
    text[4] = '\0';
}

static void native_camera_capture_reset(NativeCameraCapture *capture) {
    if (!capture) {
        return;
    }
    if (capture->streaming && capture->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (native_camera_ioctl(capture->fd, VIDIOC_STREAMOFF, &type) != 0) {
            clog(cLogLevelWarning, "VIDIOC_STREAMOFF failed for %s: %s", capture->path, strerror(errno));
        }
        capture->streaming = false;
    }
    for (uint32_t i = 0; i < capture->buffer_count; i++) {
        if (capture->buffers[i].data && capture->buffers[i].data != MAP_FAILED) {
            (void)munmap(capture->buffers[i].data, capture->buffers[i].length);
        }
        capture->buffers[i].data = NULL;
        capture->buffers[i].length = 0;
    }
    capture->buffer_count = 0;
    if (capture->fd >= 0) {
        close(capture->fd);
        capture->fd = -1;
    }
}

static uint32_t native_camera_capabilities(const struct v4l2_capability *capability) {
    return (capability->capabilities & V4L2_CAP_DEVICE_CAPS) ? capability->device_caps : capability->capabilities;
}

static bool native_camera_single_plane_streaming(uint32_t capabilities) {
    return (capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0u && (capabilities & V4L2_CAP_STREAMING) != 0u;
}

static bool native_camera_fd_has_native_h264(int fd) {
    for (uint32_t index = 0u;; index++) {
        struct v4l2_fmtdesc description;
        memset(&description, 0, sizeof(description));
        description.index = index;
        description.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (native_camera_ioctl(fd, VIDIOC_ENUM_FMT, &description) != 0) {
            break;
        }
        if (native_camera_v4l2_format_is_native_h264(description.pixelformat, description.flags, true)) {
            return true;
        }
    }
    return false;
}

static void native_camera_set_control_best_effort(int fd, uint32_t id, int32_t value, const char *name,
                                                  const char *path) {
    struct v4l2_control control;
    memset(&control, 0, sizeof(control));
    control.id = id;
    control.value = value;
    if (native_camera_ioctl(fd, VIDIOC_S_CTRL, &control) != 0) {
        clog(cLogLevelDebug, "%s does not accept best-effort H.264 %s=%d: %s", path, name, value, strerror(errno));
    }
}

static uint32_t native_camera_target_bitrate(uint32_t width, uint32_t height, uint32_t fps) {
    uint64_t bitrate = (uint64_t)width * height * fps * 130u / 1000u;
    if (bitrate < 300000u) {
        return 300000u;
    }
    /* Above the largest offered mode the heuristic would become the limiter rather
     * than a guard, so the ceiling clears 1080p at the top of the FPS envelope. */
    if (bitrate > 6000000u) {
        return 6000000u;
    }
    return (uint32_t)bitrate;
}

static void native_camera_configure_h264_controls(NativeCameraCapture *capture) {
    uint32_t i_period = capture->fps <= UINT32_MAX / 2u ? capture->fps * 2u : capture->fps;
#ifdef V4L2_CID_MPEG_VIDEO_BITRATE
    native_camera_set_control_best_effort(capture->fd, V4L2_CID_MPEG_VIDEO_BITRATE,
                                          (int32_t)native_camera_target_bitrate(capture->width, capture->height,
                                                                                capture->fps),
                                          "bitrate", capture->path);
#endif
#ifdef V4L2_CID_MPEG_VIDEO_GOP_SIZE
    native_camera_set_control_best_effort(capture->fd, V4L2_CID_MPEG_VIDEO_GOP_SIZE, (int32_t)i_period, "GOP size",
                                          capture->path);
#endif
#ifdef V4L2_CID_MPEG_VIDEO_H264_I_PERIOD
    native_camera_set_control_best_effort(capture->fd, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, (int32_t)i_period,
                                          "I-period", capture->path);
#endif
#ifdef V4L2_CID_MPEG_VIDEO_HEADER_MODE
    native_camera_set_control_best_effort(capture->fd, V4L2_CID_MPEG_VIDEO_HEADER_MODE,
                                          V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME, "joined headers",
                                          capture->path);
#endif
#ifdef V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER
    native_camera_set_control_best_effort(capture->fd, V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER, 1,
                                          "repeated headers", capture->path);
#endif
}

static bool native_camera_configure_capture(NativeCameraCapture *capture, const char *path, const char *device_id,
                                            uint32_t requested_width, uint32_t requested_height,
                                            uint32_t requested_fps, NativeCameraCaptureFormat capture_format) {
    memset(capture, 0, sizeof(*capture));
    capture->fd = -1;
    (void)snprintf(capture->path, sizeof(capture->path), "%s", path);
    (void)snprintf(capture->id, sizeof(capture->id), "%s", device_id ? device_id : "");

    capture->fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (capture->fd < 0) {
        clog(cLogLevelDebug, "cannot open %s: %s", path, strerror(errno));
        return false;
    }

    struct v4l2_capability capability;
    memset(&capability, 0, sizeof(capability));
    if (native_camera_ioctl(capture->fd, VIDIOC_QUERYCAP, &capability) != 0) {
        clog(cLogLevelDebug, "VIDIOC_QUERYCAP failed for %s: %s", path, strerror(errno));
        native_camera_capture_reset(capture);
        return false;
    }
    uint32_t caps = native_camera_capabilities(&capability);
    if (!native_camera_single_plane_streaming(caps)) {
        clog(cLogLevelDebug, "%s is not a streaming video-capture node (caps=0x%x)", path, caps);
        native_camera_capture_reset(capture);
        return false;
    }
    if (capture_format == NATIVE_CAMERA_CAPTURE_H264 && !native_camera_fd_has_native_h264(capture->fd)) {
        clog(cLogLevelDebug, "%s does not expose non-emulated single-plane H.264", path);
        native_camera_capture_reset(capture);
        return false;
    }

    struct v4l2_format format;
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = requested_width;
    format.fmt.pix.height = requested_height;
    uint32_t requested_pixel_format = capture_format == NATIVE_CAMERA_CAPTURE_H264 ? V4L2_PIX_FMT_H264
                                                                                   : V4L2_PIX_FMT_YUYV;
    format.fmt.pix.pixelformat = requested_pixel_format;
    format.fmt.pix.field = V4L2_FIELD_ANY;
    if (native_camera_ioctl(capture->fd, VIDIOC_S_FMT, &format) != 0) {
        char requested[5];
        native_camera_fourcc(requested, requested_pixel_format);
        clog(cLogLevelDebug, "VIDIOC_S_FMT(%s) failed for %s: %s", requested, path, strerror(errno));
        native_camera_capture_reset(capture);
        return false;
    }
    if (!native_camera_v4l2_mode_is_exact(requested_pixel_format, requested_width, requested_height,
                                          format.fmt.pix.pixelformat, format.fmt.pix.width, format.fmt.pix.height)) {
        char selected[5];
        native_camera_fourcc(selected, format.fmt.pix.pixelformat);
        char requested[5];
        native_camera_fourcc(requested, requested_pixel_format);
        clog(cLogLevelDebug, "%s cannot provide exact %s %ux%u (driver selected %s %ux%u)", path, requested,
             requested_width, requested_height, selected, format.fmt.pix.width, format.fmt.pix.height);
        native_camera_capture_reset(capture);
        return false;
    }
    capture->width = format.fmt.pix.width;
    capture->height = format.fmt.pix.height;
    capture->compressed = capture_format == NATIVE_CAMERA_CAPTURE_H264;
    if (capture->compressed) {
        if (format.fmt.pix.sizeimage == 0u) {
            clog(cLogLevelDebug, "%s returned a zero H.264 buffer size", path);
            native_camera_capture_reset(capture);
            return false;
        }
        capture->source_pitch = 0u;
        capture->minimum_frame_bytes = 0u;
    } else {
        capture->source_pitch = format.fmt.pix.bytesperline;
        if (capture->source_pitch < capture->width * 2u) {
            capture->source_pitch = capture->width * 2u;
        }
        if ((size_t)capture->height > SIZE_MAX / capture->source_pitch) {
            native_camera_capture_reset(capture);
            return false;
        }
        capture->minimum_frame_bytes = (size_t)capture->source_pitch * capture->height;
    }

    struct v4l2_streamparm stream;
    memset(&stream, 0, sizeof(stream));
    stream.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    stream.parm.capture.timeperframe.numerator = 1;
    stream.parm.capture.timeperframe.denominator = requested_fps;
    if (native_camera_ioctl(capture->fd, VIDIOC_S_PARM, &stream) != 0) {
        clog(cLogLevelDebug, "VIDIOC_S_PARM failed for %s: %s", path, strerror(errno));
        native_camera_capture_reset(capture);
        return false;
    } else if (stream.parm.capture.timeperframe.numerator == 0 || stream.parm.capture.timeperframe.denominator == 0 ||
               (uint64_t)stream.parm.capture.timeperframe.denominator !=
                   (uint64_t)requested_fps * stream.parm.capture.timeperframe.numerator) {
        clog(cLogLevelDebug, "%s cannot provide exact %u fps (driver selected %u/%u seconds)", path, requested_fps,
             stream.parm.capture.timeperframe.numerator, stream.parm.capture.timeperframe.denominator);
        native_camera_capture_reset(capture);
        return false;
    }
    capture->fps = requested_fps;
    if (capture->compressed) {
        native_camera_configure_h264_controls(capture);
    }

    struct v4l2_requestbuffers request;
    memset(&request, 0, sizeof(request));
    request.count = NATIVE_CAMERA_BUFFER_COUNT;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (native_camera_ioctl(capture->fd, VIDIOC_REQBUFS, &request) != 0 || request.count == 0) {
        clog(cLogLevelDebug, "VIDIOC_REQBUFS failed for %s: %s", path, strerror(errno));
        native_camera_capture_reset(capture);
        return false;
    }
    capture->buffer_count = request.count < NATIVE_CAMERA_BUFFER_COUNT ? request.count : NATIVE_CAMERA_BUFFER_COUNT;

    for (uint32_t i = 0; i < capture->buffer_count; i++) {
        struct v4l2_buffer buffer;
        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        if (native_camera_ioctl(capture->fd, VIDIOC_QUERYBUF, &buffer) != 0) {
            clog(cLogLevelDebug, "VIDIOC_QUERYBUF(%u) failed for %s: %s", i, path, strerror(errno));
            native_camera_capture_reset(capture);
            return false;
        }
        capture->buffers[i].length = buffer.length;
        capture->buffers[i].data = mmap(NULL, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, capture->fd,
                                        buffer.m.offset);
        if (capture->buffers[i].data == MAP_FAILED) {
            clog(cLogLevelDebug, "mmap buffer %u failed for %s: %s", i, path, strerror(errno));
            native_camera_capture_reset(capture);
            return false;
        }
        if (native_camera_ioctl(capture->fd, VIDIOC_QBUF, &buffer) != 0) {
            clog(cLogLevelDebug, "VIDIOC_QBUF(%u) failed for %s: %s", i, path, strerror(errno));
            native_camera_capture_reset(capture);
            return false;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (native_camera_ioctl(capture->fd, VIDIOC_STREAMON, &type) != 0) {
        clog(cLogLevelDebug, "VIDIOC_STREAMON failed for %s: %s", path, strerror(errno));
        native_camera_capture_reset(capture);
        return false;
    }
    capture->streaming = true;

    char card[sizeof(capability.card) + 1u];
    memcpy(card, capability.card, sizeof(capability.card));
    card[sizeof(capability.card)] = '\0';
    clog(cLogLevelNotice, "opened camera %s (%s), %s %ux%u at target %u fps", path, card,
         capture->compressed ? "native H.264" : "YUYV preview", capture->width, capture->height,
         (unsigned)requested_fps);
    return true;
}

static void native_camera_device_stable_id(const char *path, const struct v4l2_capability *caps, char *out,
                                           size_t out_cap) {
    struct stat camera_stat;
    if (stat(path, &camera_stat) == 0) {
        glob_t links;
        memset(&links, 0, sizeof(links));
        if (glob("/dev/v4l/by-id/*", 0, NULL, &links) == 0) {
            for (size_t i = 0; i < links.gl_pathc; i++) {
                struct stat link_stat;
                if (stat(links.gl_pathv[i], &link_stat) == 0 && link_stat.st_rdev == camera_stat.st_rdev) {
                    const char *base = strrchr(links.gl_pathv[i], '/');
                    (void)snprintf(out, out_cap, "v4l-by-id:%s", base ? base + 1 : links.gl_pathv[i]);
                    globfree(&links);
                    return;
                }
            }
        }
        globfree(&links);
    }

    char bus[sizeof(caps->bus_info) + 1u];
    char card[sizeof(caps->card) + 1u];
    memcpy(bus, caps->bus_info, sizeof(caps->bus_info));
    bus[sizeof(caps->bus_info)] = '\0';
    memcpy(card, caps->card, sizeof(caps->card));
    card[sizeof(caps->card)] = '\0';
    (void)snprintf(out, out_cap, "v4l2:%s:%s", bus[0] ? bus : "unknown", card[0] ? card : "camera");
}

static bool native_camera_query_device(const char *path, NativeCameraDeviceInfo *info, bool require_native_h264) {
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    struct v4l2_capability caps;
    memset(&caps, 0, sizeof(caps));
    bool usable = native_camera_ioctl(fd, VIDIOC_QUERYCAP, &caps) == 0;
    if (usable) {
        uint32_t flags = native_camera_capabilities(&caps);
        usable = native_camera_single_plane_streaming(flags);
    }
    if (usable && require_native_h264) {
        usable = native_camera_fd_has_native_h264(fd);
    }
    close(fd);
    if (!usable) {
        return false;
    }

    if (info) {
        memset(info, 0, sizeof(*info));
        (void)snprintf(info->path, sizeof(info->path), "%s", path);
        memcpy(info->name, caps.card,
               sizeof(caps.card) < sizeof(info->name) - 1u ? sizeof(caps.card) : sizeof(info->name) - 1u);
        native_camera_device_stable_id(path, &caps, info->id, sizeof(info->id));
    }
    return true;
}

static size_t native_camera_enumerate_devices(NativeCameraDeviceInfo *devices, size_t capacity,
                                              bool require_native_h264) {
    size_t count = 0;
    for (unsigned index = 0; index < NATIVE_CAMERA_SCAN_LIMIT; index++) {
        char path[NATIVE_CAMERA_DEVICE_PATH_MAX];
        (void)snprintf(path, sizeof(path), "/dev/video%u", index);
        NativeCameraDeviceInfo info;
        if (!native_camera_query_device(path, &info, require_native_h264)) {
            continue;
        }
        if (!devices || count >= capacity) {
            continue;
        }
        devices[count++] = info;
    }
    return count;
}

#ifdef HELLOLG_CAMERA_V4L2_TESTING
static const NativeCameraV4l2TestNodes *g_test_nodes;

void native_camera_v4l2_set_test_nodes(const NativeCameraV4l2TestNodes *nodes) {
    g_test_nodes = nodes;
}
#endif

/* The mode-enumeration walks below reach the driver only through these three
 * calls, which the host tests replace wholesale. */
static int native_camera_open_node(const char *path) {
#ifdef HELLOLG_CAMERA_V4L2_TESTING
    if (g_test_nodes) {
        return g_test_nodes->open_node(path);
    }
#endif
    return open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
}

static int native_camera_node_ioctl(int fd, unsigned long request, void *argument) {
#ifdef HELLOLG_CAMERA_V4L2_TESTING
    if (g_test_nodes) {
        return g_test_nodes->node_ioctl(fd, request, argument);
    }
#endif
    return native_camera_ioctl(fd, request, argument);
}

static void native_camera_close_node(int fd) {
#ifdef HELLOLG_CAMERA_V4L2_TESTING
    if (g_test_nodes) {
        g_test_nodes->close_node(fd);
        return;
    }
#endif
    close(fd);
}

size_t native_camera_enumerate(NativeCameraDeviceInfo *devices, size_t capacity) {
#ifdef HELLOLG_CAMERA_V4L2_TESTING
    if (g_test_nodes) {
        return g_test_nodes->enumerate(devices, capacity);
    }
#endif
    return native_camera_enumerate_devices(devices, capacity, true);
}

NativeCameraRateQuery native_camera_enumerate_frame_rates(const char *device_id, uint32_t width, uint32_t height,
                                                          uint16_t *rates, size_t capacity, size_t *count) {
    if (count) {
        *count = 0;
    }
    if (!rates || capacity == 0u || width == 0u || height == 0u) {
        return NATIVE_CAMERA_RATES_UNKNOWN;
    }
    NativeCameraDeviceInfo devices[NATIVE_CAMERA_SCAN_LIMIT];
    size_t device_count = native_camera_enumerate(devices, sizeof(devices) / sizeof(devices[0]));
    if (device_count > sizeof(devices) / sizeof(devices[0])) {
        device_count = sizeof(devices) / sizeof(devices[0]);
    }
    bool automatic = !device_id || device_id[0] == '\0';
    size_t written = 0;
    bool any_node = false;
    bool any_has_interval = false;
    bool any_whole_rate = false;

    /* Capture open walks every candidate until one accepts the exact mode: Auto spans
     * all cameras, and an explicit ID can still match several nodes of one camera when
     * /dev/v4l/by-id is missing. Union the rates over the same set so a later candidate
     * cannot be hidden by an earlier one. */
    for (size_t i = 0; i < device_count; i++) {
        if (!automatic && strcmp(device_id, devices[i].id) != 0) {
            continue;
        }
        int fd = native_camera_open_node(devices[i].path);
        if (fd < 0) {
            continue;
        }
        any_node = true;
        /* A refusal at index 0 is the driver saying it has no H.264 mode at this
         * size; a refusal later is just the end of the list. */
        for (uint32_t index = 0;; index++) {
            struct v4l2_frmivalenum interval;
            memset(&interval, 0, sizeof(interval));
            interval.index = index;
            interval.pixel_format = V4L2_PIX_FMT_H264;
            interval.width = width;
            interval.height = height;
            if (native_camera_node_ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) != 0) {
                break;
            }
            any_has_interval = true;
            if (interval.type != V4L2_FRMIVAL_TYPE_DISCRETE || interval.discrete.numerator == 0u ||
                interval.discrete.denominator == 0u ||
                interval.discrete.denominator % interval.discrete.numerator != 0u) {
                continue;
            }
            uint32_t fps = interval.discrete.denominator / interval.discrete.numerator;
            if (fps == 0u) {
                continue;
            }
            any_whole_rate = true;
            (void)native_camera_v4l2_insert_frame_rate(rates, capacity, &written, fps);
        }
        native_camera_close_node(fd);
    }

    if (written > 0u) {
        if (count) {
            *count = written;
        }
        return NATIVE_CAMERA_RATES_LISTED;
    }
    if (any_whole_rate) {
        /* The size exists, but only above the app's supported FPS envelope. */
        return NATIVE_CAMERA_RATES_UNSUPPORTED;
    }
    if (any_has_interval) {
        /* The size exists but only with stepwise/fractional intervals we cannot offer. */
        return NATIVE_CAMERA_RATES_UNKNOWN;
    }
    return any_node ? NATIVE_CAMERA_RATES_UNSUPPORTED : NATIVE_CAMERA_RATES_UNKNOWN;
}

NativeCameraRateQuery native_camera_enumerate_frame_sizes(const char *device_id, uint16_t (*sizes)[2], size_t capacity,
                                                          size_t *count) {
    if (count) {
        *count = 0;
    }
    if (!sizes || capacity == 0u) {
        return NATIVE_CAMERA_RATES_UNKNOWN;
    }
    NativeCameraDeviceInfo devices[NATIVE_CAMERA_SCAN_LIMIT];
    size_t device_count = native_camera_enumerate(devices, sizeof(devices) / sizeof(devices[0]));
    if (device_count > sizeof(devices) / sizeof(devices[0])) {
        device_count = sizeof(devices) / sizeof(devices[0]);
    }
    bool automatic = !device_id || device_id[0] == '\0';
    size_t written = 0;
    bool any_node = false;
    bool any_discrete = false;

    /* Same candidate walk and union as the frame-rate query: Auto spans all cameras,
     * and an explicit ID can still match several nodes of one camera. */
    for (size_t i = 0; i < device_count; i++) {
        if (!automatic && strcmp(device_id, devices[i].id) != 0) {
            continue;
        }
        int fd = native_camera_open_node(devices[i].path);
        if (fd < 0) {
            continue;
        }
        any_node = true;
        for (uint32_t index = 0;; index++) {
            struct v4l2_frmsizeenum size;
            memset(&size, 0, sizeof(size));
            size.index = index;
            size.pixel_format = V4L2_PIX_FMT_H264;
            if (native_camera_node_ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) != 0) {
                break;
            }
            /* Stepwise/continuous ranges describe a space, not the exact modes the
             * capture open demands, so only discrete entries can be offered. */
            if (size.type != V4L2_FRMSIZE_TYPE_DISCRETE) {
                continue;
            }
            any_discrete = true;
            (void)native_camera_v4l2_insert_frame_size(sizes, capacity, &written, size.discrete.width,
                                                       size.discrete.height);
        }
        native_camera_close_node(fd);
    }

    if (written > 0u) {
        if (count) {
            *count = written;
        }
        return NATIVE_CAMERA_RATES_LISTED;
    }
    /* The camera answered but every size fell outside the envelope. */
    return any_discrete || any_node ? NATIVE_CAMERA_RATES_UNSUPPORTED : NATIVE_CAMERA_RATES_UNKNOWN;
}

static bool native_camera_capture_open_format(const char *device_id, uint32_t width, uint32_t height, uint32_t fps,
                                              NativeCameraCaptureFormat capture_format,
                                              NativeCameraCapture **out_capture) {
    if (!out_capture || width == 0 || height == 0 || fps == 0 || (width & 1u) != 0 || (height & 1u) != 0) {
        return false;
    }
    *out_capture = NULL;
    NativeCameraDeviceInfo devices[NATIVE_CAMERA_SCAN_LIMIT];
    bool h264 = capture_format == NATIVE_CAMERA_CAPTURE_H264;
    size_t count = native_camera_enumerate_devices(devices, sizeof(devices) / sizeof(devices[0]), h264);
    if (count > sizeof(devices) / sizeof(devices[0])) {
        count = sizeof(devices) / sizeof(devices[0]);
    }
    bool automatic = !device_id || device_id[0] == '\0';
    for (size_t i = 0; i < count; i++) {
        if (!automatic && strcmp(device_id, devices[i].id) != 0) {
            continue;
        }
        NativeCameraCapture *capture = (NativeCameraCapture *)calloc(1, sizeof(*capture));
        if (!capture) {
            return false;
        }
        if (native_camera_configure_capture(capture, devices[i].path, devices[i].id, width, height, fps,
                                            capture_format)) {
            *out_capture = capture;
            return true;
        }
        free(capture);
        /* A camera can expose multiple capture nodes with the same bus/card fallback
         * ID when /dev/v4l/by-id is unavailable. Try every matching node so an earlier
         * metadata/other-format endpoint cannot hide a later exact endpoint. */
    }
    clog(cLogLevelError, "no V4L2 camera accepted exact %s %ux%u@%u%s%s", h264 ? "native H.264" : "YUYV",
         width, height, fps,
         automatic ? "" : " for device ", automatic ? "" : device_id);
    return false;
}

bool native_camera_frame_rate_list_contains(const uint16_t *rates, size_t count, uint32_t fps) {
    if (!rates || fps == 0u || fps > UINT16_MAX) {
        return false;
    }
    for (size_t i = 0u; i < count; i++) {
        if (rates[i] == fps) {
            return true;
        }
    }
    return false;
}

bool native_camera_h264_mode_available(const char *device_id, uint32_t width, uint32_t height, uint32_t fps) {
    uint16_t rates[NATIVE_CAMERA_FRAME_RATE_MAX];
    size_t count = 0u;
    if (native_camera_enumerate_frame_rates(device_id, width, height, rates, NATIVE_CAMERA_FRAME_RATE_MAX, &count) !=
        NATIVE_CAMERA_RATES_LISTED) {
        return false;
    }
    return native_camera_frame_rate_list_contains(rates, count, fps);
}

bool native_camera_capture_open_h264(const char *device_id, uint32_t width, uint32_t height, uint32_t fps,
                                     NativeCameraCapture **out_capture) {
    return native_camera_capture_open_format(device_id, width, height, fps, NATIVE_CAMERA_CAPTURE_H264, out_capture);
}

bool native_camera_capture_open_yuyv_preview(uint32_t width, uint32_t height, uint32_t fps,
                                             NativeCameraCapture **out_capture) {
    return native_camera_capture_open_format(NULL, width, height, fps, NATIVE_CAMERA_CAPTURE_YUYV_PREVIEW,
                                             out_capture);
}

void native_camera_capture_close(NativeCameraCapture *capture) {
    if (!capture) {
        return;
    }
    native_camera_capture_reset(capture);
    free(capture);
}

uint32_t native_camera_capture_width(const NativeCameraCapture *capture) {
    return capture ? capture->width : 0;
}

uint32_t native_camera_capture_height(const NativeCameraCapture *capture) {
    return capture ? capture->height : 0;
}

uint32_t native_camera_capture_pitch(const NativeCameraCapture *capture) {
    return capture ? capture->source_pitch : 0;
}

static size_t native_camera_frame_size(const NativeCameraCapture *capture) {
    return capture ? capture->minimum_frame_bytes : 0u;
}

static void native_camera_queue_buffer(NativeCameraCapture *capture, uint32_t index) {
    struct v4l2_buffer buffer;
    memset(&buffer, 0, sizeof(buffer));
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    if (native_camera_ioctl(capture->fd, VIDIOC_QBUF, &buffer) != 0) {
        clog(cLogLevelWarning, "VIDIOC_QBUF(%u) failed for %s: %s", index, capture->path, strerror(errno));
    }
}

NativeCameraAcquireResult native_camera_capture_acquire(NativeCameraCapture *capture, int timeout_ms,
                                                        NativeCameraFrame *frame) {
    if (!capture || capture->fd < 0 || !frame) {
        return NATIVE_CAMERA_ACQUIRE_RECOVER;
    }
    memset(frame, 0, sizeof(*frame));
    size_t required = native_camera_frame_size(capture);
    if (!capture->compressed && required == 0u) {
        return NATIVE_CAMERA_ACQUIRE_RECOVER;
    }
    struct pollfd poll_fd = {
        .fd = capture->fd,
        .events = POLLIN | POLLPRI,
    };
    int poll_result;
    do {
        poll_result = poll(&poll_fd, 1, timeout_ms);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result == 0) {
        return NATIVE_CAMERA_ACQUIRE_TIMEOUT;
    }
    if (poll_result < 0 || (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return NATIVE_CAMERA_ACQUIRE_RECOVER;
    }

    struct v4l2_buffer buffer;
    memset(&buffer, 0, sizeof(buffer));
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (native_camera_ioctl(capture->fd, VIDIOC_DQBUF, &buffer) != 0) {
        return errno == EAGAIN ? NATIVE_CAMERA_ACQUIRE_TIMEOUT : NATIVE_CAMERA_ACQUIRE_RECOVER;
    }
    if (buffer.index >= capture->buffer_count) {
        clog_limited(cLogLevelWarning, 2, 5000, "%s returned out-of-range camera buffer index %u (count=%u)",
                     capture->path, buffer.index, capture->buffer_count);
        return NATIVE_CAMERA_ACQUIRE_RECOVER;
    }
    if (!native_camera_v4l2_import_frame((const uint8_t *)capture->buffers[buffer.index].data,
                                         capture->buffers[buffer.index].length, buffer.bytesused, required,
                                         capture->compressed, buffer.index, buffer.sequence, buffer.flags,
                                         buffer.timestamp.tv_sec, buffer.timestamp.tv_usec, frame)) {
        clog_limited(cLogLevelWarning, 2, 5000,
                     "%s returned invalid camera buffer metadata (index=%u bytesused=%u mapped=%zu flags=0x%x)",
                     capture->path, buffer.index, buffer.bytesused, capture->buffers[buffer.index].length,
                     buffer.flags);
        native_camera_queue_buffer(capture, buffer.index);
        return NATIVE_CAMERA_ACQUIRE_RECOVER;
    }
    return NATIVE_CAMERA_ACQUIRE_FRAME;
}

void native_camera_capture_release(NativeCameraCapture *capture, NativeCameraFrame *frame) {
    if (!capture || capture->fd < 0 || !frame || !frame->held) {
        return;
    }
    native_camera_queue_buffer(capture, frame->index);
    memset(frame, 0, sizeof(*frame));
}

void native_camera_capture_request_keyframe(NativeCameraCapture *capture) {
    if (!capture || capture->fd < 0 || !capture->compressed) {
        return;
    }
#ifdef V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME
    /* Best effort: a camera that ignores the button simply keeps its own GOP
     * cadence and the caller waits for the next natural IDR instead. */
    native_camera_set_control_best_effort(capture->fd, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1, "forced key frame",
                                          capture->path);
#endif
}
