#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <linux/videodev2.h>

#include "camera_v4l2.h"
#include "camera_v4l2_internal.h"

/* A fake V4L2 node table for the two mode-enumeration walks. Each node answers
 * VIDIOC_ENUM_FRAMESIZES from its own size list and VIDIOC_ENUM_FRAMEINTERVALS
 * from the interval list of the size that matches the queried geometry, exactly
 * as a driver does — including refusing index 0 for a size it does not have. */

typedef struct FakeInterval {
    uint32_t type;
    uint32_t numerator;
    uint32_t denominator;
} FakeInterval;

typedef struct FakeSize {
    uint32_t type;
    uint32_t width;
    uint32_t height;
    const FakeInterval *intervals;
    size_t interval_count;
    /* Stepwise/continuous entries only: the top of the advertised range, with
     * `width`/`height` as its bottom. Last so discrete entries keep a short
     * initializer. */
    uint32_t max_width;
    uint32_t max_height;
} FakeSize;

typedef struct FakeNode {
    const char *path;
    const char *id;
    bool open_fails;
    const FakeSize *sizes;
    size_t size_count;
} FakeNode;

#define FAKE_FD_BASE 500

static const FakeNode *g_nodes;
static size_t g_node_count;
static int g_open_calls;
static int g_close_calls;

static size_t fake_enumerate(NativeCameraDeviceInfo *devices, size_t capacity) {
    size_t written = 0u;
    for (size_t i = 0u; i < g_node_count && written < capacity; i++) {
        memset(&devices[written], 0, sizeof(devices[written]));
        (void)snprintf(devices[written].path, sizeof(devices[written].path), "%s", g_nodes[i].path);
        (void)snprintf(devices[written].id, sizeof(devices[written].id), "%s", g_nodes[i].id);
        (void)snprintf(devices[written].name, sizeof(devices[written].name), "fake camera");
        written++;
    }
    return written;
}

static int fake_open(const char *path) {
    for (size_t i = 0u; i < g_node_count; i++) {
        if (strcmp(path, g_nodes[i].path) != 0) {
            continue;
        }
        if (g_nodes[i].open_fails) {
            return -1;
        }
        g_open_calls++;
        return FAKE_FD_BASE + (int)i;
    }
    return -1;
}

static void fake_close(int fd) {
    assert(fd >= FAKE_FD_BASE && (size_t)(fd - FAKE_FD_BASE) < g_node_count);
    g_close_calls++;
}

static int fake_ioctl(int fd, unsigned long request, void *argument) {
    assert(fd >= FAKE_FD_BASE && (size_t)(fd - FAKE_FD_BASE) < g_node_count);
    const FakeNode *node = &g_nodes[fd - FAKE_FD_BASE];

    if (request == VIDIOC_ENUM_FRAMESIZES) {
        struct v4l2_frmsizeenum *query = (struct v4l2_frmsizeenum *)argument;
        /* A walk that forgot the format would silently enumerate MJPEG modes. */
        assert(query->pixel_format == V4L2_PIX_FMT_H264);
        if (query->index >= node->size_count) {
            return -1;
        }
        const FakeSize *size = &node->sizes[query->index];
        query->type = size->type;
        if (size->type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            query->discrete.width = size->width;
            query->discrete.height = size->height;
        } else {
            /* The kernel overlays both shapes in one union, so a walk that reads
             * `discrete` without checking the type gets min_width/max_width. */
            query->stepwise.min_width = size->width;
            query->stepwise.max_width = size->max_width;
            query->stepwise.step_width = 2u;
            query->stepwise.min_height = size->height;
            query->stepwise.max_height = size->max_height;
            query->stepwise.step_height = 2u;
        }
        return 0;
    }

    if (request == VIDIOC_ENUM_FRAMEINTERVALS) {
        struct v4l2_frmivalenum *query = (struct v4l2_frmivalenum *)argument;
        assert(query->pixel_format == V4L2_PIX_FMT_H264);
        for (size_t i = 0u; i < node->size_count; i++) {
            const FakeSize *size = &node->sizes[i];
            if (size->width != query->width || size->height != query->height) {
                continue;
            }
            if (query->index >= size->interval_count) {
                return -1;
            }
            const FakeInterval *interval = &size->intervals[query->index];
            query->type = interval->type;
            query->discrete.numerator = interval->numerator;
            query->discrete.denominator = interval->denominator;
            return 0;
        }
        return -1;
    }

    return -1;
}

static const NativeCameraV4l2TestNodes k_fake_nodes = {
    .enumerate = fake_enumerate,
    .open_node = fake_open,
    .node_ioctl = fake_ioctl,
    .close_node = fake_close,
};

static void install_nodes(const FakeNode *nodes, size_t count) {
    g_nodes = nodes;
    g_node_count = count;
    g_open_calls = 0;
    g_close_calls = 0;
}

/* One CyberTrack-H5-shaped node: H.264 sizes in driver order (not sorted), plus
 * the entries the walk has to drop. */
static const FakeInterval k_1080p_intervals[] = {
    {V4L2_FRMIVAL_TYPE_DISCRETE, 1u, 30u},
    {V4L2_FRMIVAL_TYPE_DISCRETE, 1u, 15u},
};
static const FakeInterval k_720p_intervals[] = {
    {V4L2_FRMIVAL_TYPE_DISCRETE, 1u, 30u},
    {V4L2_FRMIVAL_TYPE_DISCRETE, 2u, 30u},  /* 15 fps spelled 2/30 */
    {V4L2_FRMIVAL_TYPE_DISCRETE, 2u, 15u},  /* 7.5 fps: not a whole rate */
    {V4L2_FRMIVAL_TYPE_DISCRETE, 0u, 30u},  /* malformed */
    {V4L2_FRMIVAL_TYPE_STEPWISE, 1u, 30u},
};
static const FakeInterval k_480p_intervals[] = {
    {V4L2_FRMIVAL_TYPE_DISCRETE, 1u, 60u},  /* above the FPS envelope */
};
static const FakeSize k_camera_sizes[] = {
    {V4L2_FRMSIZE_TYPE_DISCRETE, 1280u, 720u, k_720p_intervals, 5u},
    {V4L2_FRMSIZE_TYPE_DISCRETE, 640u, 480u, k_480p_intervals, 1u},
    {V4L2_FRMSIZE_TYPE_DISCRETE, 1920u, 1080u, k_1080p_intervals, 2u},
    {V4L2_FRMSIZE_TYPE_DISCRETE, 320u, 240u, NULL, 0u},   /* below the lower bound */
    {V4L2_FRMSIZE_TYPE_DISCRETE, 1281u, 720u, NULL, 0u},  /* odd width */
    /* 640..1024 x 480..768 stepwise. Misread as a discrete pair that is
     * min_width x max_width = 640x1024, which the envelope would accept — so
     * dropping the type check shows up as a mode the camera cannot deliver. */
    {V4L2_FRMSIZE_TYPE_STEPWISE, 640u, 480u, NULL, 0u, 1024u, 768u},
};
static const FakeNode k_camera[] = {
    {"/dev/video0", "v4l-by-id:usb-CyberTrack_H5", false, k_camera_sizes, 6u},
};

static void test_frame_size_enumeration(void) {
    uint16_t sizes[NATIVE_CAMERA_FRAME_SIZE_MAX][2];
    size_t count = 12345u;

    install_nodes(k_camera, 1u);
    assert(native_camera_enumerate_frame_sizes(NULL, sizes, NATIVE_CAMERA_FRAME_SIZE_MAX, &count) ==
           NATIVE_CAMERA_RATES_LISTED);
    assert(count == 3u);
    assert(sizes[0][0] == 1920u && sizes[0][1] == 1080u);
    assert(sizes[1][0] == 1280u && sizes[1][1] == 720u);
    assert(sizes[2][0] == 640u && sizes[2][1] == 480u);
    assert(g_open_calls == 1 && g_close_calls == 1);

    /* An explicit ID selects the same node; an unknown one opens nothing at all,
     * which is UNKNOWN rather than "this camera has no H.264 sizes". */
    assert(native_camera_enumerate_frame_sizes("v4l-by-id:usb-CyberTrack_H5", sizes, NATIVE_CAMERA_FRAME_SIZE_MAX,
                                               &count) == NATIVE_CAMERA_RATES_LISTED);
    assert(count == 3u);
    count = 12345u;
    assert(native_camera_enumerate_frame_sizes("v4l-by-id:some-other-camera", sizes, NATIVE_CAMERA_FRAME_SIZE_MAX,
                                               &count) == NATIVE_CAMERA_RATES_UNKNOWN);
    assert(count == 0u);

    /* Capacity is a cap on what is kept, and what it keeps is the LARGEST sizes:
     * a caller that filters the result afterwards must not have been handed the
     * smallest-N prefix. */
    assert(native_camera_enumerate_frame_sizes(NULL, sizes, 2u, &count) == NATIVE_CAMERA_RATES_LISTED);
    assert(count == 2u);
    assert(sizes[0][0] == 1920u && sizes[1][0] == 1280u);

    assert(native_camera_enumerate_frame_sizes(NULL, NULL, NATIVE_CAMERA_FRAME_SIZE_MAX, &count) ==
           NATIVE_CAMERA_RATES_UNKNOWN);
    assert(count == 0u);
    assert(native_camera_enumerate_frame_sizes(NULL, sizes, 0u, &count) == NATIVE_CAMERA_RATES_UNKNOWN);
    assert(native_camera_enumerate_frame_sizes(NULL, sizes, NATIVE_CAMERA_FRAME_SIZE_MAX, NULL) ==
           NATIVE_CAMERA_RATES_LISTED);

    /* A camera that answers but offers nothing inside the envelope is a definite
     * "no", unlike a camera that could not be reached. */
    static const FakeSize tiny_sizes[] = {
        {V4L2_FRMSIZE_TYPE_DISCRETE, 320u, 240u, NULL, 0u},
        {V4L2_FRMSIZE_TYPE_DISCRETE, 176u, 144u, NULL, 0u},
    };
    static const FakeNode tiny[] = {
        {"/dev/video0", "v4l2:tiny", false, tiny_sizes, 2u},
    };
    install_nodes(tiny, 1u);
    assert(native_camera_enumerate_frame_sizes(NULL, sizes, NATIVE_CAMERA_FRAME_SIZE_MAX, &count) ==
           NATIVE_CAMERA_RATES_UNSUPPORTED);
    assert(count == 0u);

    /* One camera can expose several capture nodes under the same fallback ID when
     * /dev/v4l/by-id is missing; the later node must not be hidden by the first,
     * and a node that cannot be opened must not hide the rest. */
    static const FakeSize first_sizes[] = {{V4L2_FRMSIZE_TYPE_DISCRETE, 640u, 480u, NULL, 0u}};
    static const FakeSize second_sizes[] = {{V4L2_FRMSIZE_TYPE_DISCRETE, 1920u, 1080u, NULL, 0u}};
    static const FakeNode split[] = {
        {"/dev/video0", "v4l2:usb-1-2:CyberTrack H5", false, first_sizes, 1u},
        {"/dev/video1", "v4l2:usb-1-2:CyberTrack H5", true, second_sizes, 1u},
        {"/dev/video2", "v4l2:usb-1-2:CyberTrack H5", false, second_sizes, 1u},
    };
    install_nodes(split, 3u);
    assert(native_camera_enumerate_frame_sizes("v4l2:usb-1-2:CyberTrack H5", sizes, NATIVE_CAMERA_FRAME_SIZE_MAX,
                                               &count) == NATIVE_CAMERA_RATES_LISTED);
    assert(count == 2u);
    assert(sizes[0][0] == 1920u && sizes[1][0] == 640u);
    assert(g_open_calls == 2 && g_close_calls == 2);

    /* Every node that opened is closed again. */
    static const FakeNode unreachable[] = {
        {"/dev/video0", "v4l2:gone", true, first_sizes, 1u},
    };
    install_nodes(unreachable, 1u);
    assert(native_camera_enumerate_frame_sizes(NULL, sizes, NATIVE_CAMERA_FRAME_SIZE_MAX, &count) ==
           NATIVE_CAMERA_RATES_UNKNOWN);
    assert(g_open_calls == 0 && g_close_calls == 0);
}

static void test_frame_rate_enumeration(void) {
    uint16_t rates[NATIVE_CAMERA_FRAME_RATE_MAX];
    size_t count = 12345u;

    install_nodes(k_camera, 1u);
    assert(native_camera_enumerate_frame_rates(NULL, 1280u, 720u, rates, NATIVE_CAMERA_FRAME_RATE_MAX, &count) ==
           NATIVE_CAMERA_RATES_LISTED);
    assert(count == 2u && rates[0] == 30u && rates[1] == 15u);
    assert(g_open_calls == 1 && g_close_calls == 1);

    assert(native_camera_enumerate_frame_rates(NULL, 1920u, 1080u, rates, NATIVE_CAMERA_FRAME_RATE_MAX, &count) ==
           NATIVE_CAMERA_RATES_LISTED);
    assert(count == 2u && rates[0] == 30u && rates[1] == 15u);

    /* The size exists but only above the FPS envelope: UNSUPPORTED, not UNKNOWN. */
    assert(native_camera_enumerate_frame_rates(NULL, 640u, 480u, rates, NATIVE_CAMERA_FRAME_RATE_MAX, &count) ==
           NATIVE_CAMERA_RATES_UNSUPPORTED);
    assert(count == 0u);

    /* A size the driver refuses at index 0 is the driver saying it has no H.264
     * mode there — the node answered, so this is UNSUPPORTED too. */
    assert(native_camera_enumerate_frame_rates(NULL, 800u, 600u, rates, NATIVE_CAMERA_FRAME_RATE_MAX, &count) ==
           NATIVE_CAMERA_RATES_UNSUPPORTED);

    assert(native_camera_enumerate_frame_rates("v4l-by-id:some-other-camera", 1280u, 720u, rates,
                                               NATIVE_CAMERA_FRAME_RATE_MAX, &count) ==
           NATIVE_CAMERA_RATES_UNKNOWN);
    assert(count == 0u);

    assert(native_camera_enumerate_frame_rates(NULL, 0u, 720u, rates, NATIVE_CAMERA_FRAME_RATE_MAX, &count) ==
           NATIVE_CAMERA_RATES_UNKNOWN);
    assert(native_camera_enumerate_frame_rates(NULL, 1280u, 0u, rates, NATIVE_CAMERA_FRAME_RATE_MAX, &count) ==
           NATIVE_CAMERA_RATES_UNKNOWN);
    assert(native_camera_enumerate_frame_rates(NULL, 1280u, 720u, NULL, NATIVE_CAMERA_FRAME_RATE_MAX, &count) ==
           NATIVE_CAMERA_RATES_UNKNOWN);
    assert(native_camera_enumerate_frame_rates(NULL, 1280u, 720u, rates, 0u, &count) ==
           NATIVE_CAMERA_RATES_UNKNOWN);
    assert(count == 0u);

    /* Stepwise intervals describe a space, not the exact modes the capture open
     * demands: nothing can be offered and nothing can be concluded. */
    static const FakeInterval stepwise_intervals[] = {
        {V4L2_FRMIVAL_TYPE_STEPWISE, 1u, 30u},
        {V4L2_FRMIVAL_TYPE_CONTINUOUS, 1u, 30u},
    };
    static const FakeSize stepwise_sizes[] = {
        {V4L2_FRMSIZE_TYPE_DISCRETE, 800u, 600u, stepwise_intervals, 2u},
    };
    static const FakeNode stepwise[] = {
        {"/dev/video0", "v4l2:stepwise", false, stepwise_sizes, 1u},
    };
    install_nodes(stepwise, 1u);
    assert(native_camera_enumerate_frame_rates(NULL, 800u, 600u, rates, NATIVE_CAMERA_FRAME_RATE_MAX, &count) ==
           NATIVE_CAMERA_RATES_UNKNOWN);
    assert(count == 0u);
    /* The same node still offers the size itself. */
    uint16_t sizes[NATIVE_CAMERA_FRAME_SIZE_MAX][2];
    size_t size_count = 0u;
    assert(native_camera_enumerate_frame_sizes(NULL, sizes, NATIVE_CAMERA_FRAME_SIZE_MAX, &size_count) ==
           NATIVE_CAMERA_RATES_LISTED);
    assert(size_count == 1u && sizes[0][0] == 800u && sizes[0][1] == 600u);

    /* Rates union across a camera's nodes exactly like sizes do. */
    static const FakeInterval slow_intervals[] = {{V4L2_FRMIVAL_TYPE_DISCRETE, 1u, 10u}};
    static const FakeInterval fast_intervals[] = {{V4L2_FRMIVAL_TYPE_DISCRETE, 1u, 30u}};
    static const FakeSize slow_sizes[] = {{V4L2_FRMSIZE_TYPE_DISCRETE, 1280u, 720u, slow_intervals, 1u}};
    static const FakeSize fast_sizes[] = {{V4L2_FRMSIZE_TYPE_DISCRETE, 1280u, 720u, fast_intervals, 1u}};
    static const FakeNode split[] = {
        {"/dev/video0", "v4l2:split", false, slow_sizes, 1u},
        {"/dev/video2", "v4l2:split", false, fast_sizes, 1u},
    };
    install_nodes(split, 2u);
    assert(native_camera_enumerate_frame_rates("v4l2:split", 1280u, 720u, rates, NATIVE_CAMERA_FRAME_RATE_MAX,
                                               &count) == NATIVE_CAMERA_RATES_LISTED);
    assert(count == 2u && rates[0] == 30u && rates[1] == 10u);
    assert(g_open_calls == 2 && g_close_calls == 2);
}

static void test_h264_mode_available(void) {
    install_nodes(k_camera, 1u);
    assert(native_camera_h264_mode_available(NULL, 1280u, 720u, 30u));
    assert(native_camera_h264_mode_available(NULL, 1280u, 720u, 15u));
    assert(native_camera_h264_mode_available(NULL, 1920u, 1080u, 30u));
    /* 7.5 fps was reported but is not a whole rate; 10 fps was never reported. */
    assert(!native_camera_h264_mode_available(NULL, 1280u, 720u, 7u));
    assert(!native_camera_h264_mode_available(NULL, 1280u, 720u, 10u));
    /* Reported only at 60 fps, which the app cannot request. */
    assert(!native_camera_h264_mode_available(NULL, 640u, 480u, 60u));
    assert(!native_camera_h264_mode_available(NULL, 640u, 480u, 30u));
    assert(!native_camera_h264_mode_available(NULL, 800u, 600u, 30u));
    assert(!native_camera_h264_mode_available("v4l-by-id:some-other-camera", 1280u, 720u, 30u));
}

int main(void) {
    assert(native_camera_v4l2_format_is_native_h264(V4L2_PIX_FMT_H264, 0u, true));
    assert(!native_camera_v4l2_format_is_native_h264(V4L2_PIX_FMT_H264, V4L2_FMT_FLAG_EMULATED, true));
    assert(!native_camera_v4l2_format_is_native_h264(V4L2_PIX_FMT_H264, 0u, false));
    assert(!native_camera_v4l2_format_is_native_h264(V4L2_PIX_FMT_H264_NO_SC, 0u, true));
    assert(!native_camera_v4l2_format_is_native_h264(V4L2_PIX_FMT_YUYV, 0u, true));
    assert(!native_camera_v4l2_format_is_native_h264(V4L2_PIX_FMT_MJPEG, 0u, true));

    assert(native_camera_v4l2_mode_is_exact(V4L2_PIX_FMT_H264, 1280u, 720u, V4L2_PIX_FMT_H264, 1280u, 720u));
    assert(!native_camera_v4l2_mode_is_exact(V4L2_PIX_FMT_H264, 1280u, 720u, V4L2_PIX_FMT_H264, 640u, 480u));
    assert(!native_camera_v4l2_mode_is_exact(V4L2_PIX_FMT_H264, 1280u, 720u, V4L2_PIX_FMT_H264_NO_SC, 1280u,
                                              720u));

    const uint16_t custom_rates[] = {12u};
    assert(native_camera_frame_rate_list_contains(custom_rates, 1u, 12u));
    assert(!native_camera_frame_rate_list_contains(custom_rates, 1u, 5u));
    assert(!native_camera_frame_rate_list_contains(custom_rates, 1u, 10u));
    assert(!native_camera_frame_rate_list_contains(custom_rates, 1u, 15u));
    assert(!native_camera_frame_rate_list_contains(NULL, 0u, 12u));
    assert(!native_camera_frame_rate_list_contains(custom_rates, 1u, UINT16_MAX + 1u));

    /* Rates arrive from the device in driver order; the helper sorts them largest
     * first, drops duplicates, and refuses anything outside the envelope. */
    uint16_t retained_rates[NATIVE_CAMERA_FRAME_RATE_MAX] = {0u};
    size_t retained_count = 0u;
    for (uint32_t fps = 60u; fps > NATIVE_CAMERA_MAX_FPS; fps--) {
        assert(!native_camera_v4l2_insert_frame_rate(retained_rates, NATIVE_CAMERA_FRAME_RATE_MAX, &retained_count,
                                                     fps));
    }
    assert(!native_camera_v4l2_insert_frame_rate(retained_rates, NATIVE_CAMERA_FRAME_RATE_MAX, &retained_count, 0u));
    assert(retained_count == 0u);
    static const uint32_t reported_rates[] = {10u, 30u, 7u, 15u, 10u, 12u};
    for (size_t i = 0u; i < sizeof(reported_rates) / sizeof(reported_rates[0]); i++) {
        assert(native_camera_v4l2_insert_frame_rate(retained_rates, NATIVE_CAMERA_FRAME_RATE_MAX, &retained_count,
                                                    reported_rates[i]));
    }
    static const uint16_t expected_rates[] = {30u, 15u, 12u, 10u, 7u};
    assert(retained_count == sizeof(expected_rates) / sizeof(expected_rates[0]));
    for (size_t i = 0u; i < retained_count; i++) {
        assert(retained_rates[i] == expected_rates[i]);
    }
    /* Rates the old preset list could never offer are now retained verbatim. */
    assert(native_camera_frame_rate_list_contains(retained_rates, retained_count, 12u));
    assert(native_camera_frame_rate_list_contains(retained_rates, retained_count, 7u));
    assert(!native_camera_frame_rate_list_contains(retained_rates, retained_count, 5u));
    size_t before_duplicate = retained_count;
    assert(native_camera_v4l2_insert_frame_rate(retained_rates, NATIVE_CAMERA_FRAME_RATE_MAX, &retained_count, 10u));
    assert(retained_count == before_duplicate);

    /* Sizes sort by descending area, reject odd or out-of-envelope geometry the
     * capture open would refuse, and clamp at capacity by losing the smallest. */
    uint16_t retained_sizes[NATIVE_CAMERA_FRAME_SIZE_MAX][2] = {{0u, 0u}};
    size_t size_count = 0u;
    assert(!native_camera_v4l2_insert_frame_size(retained_sizes, NATIVE_CAMERA_FRAME_SIZE_MAX, &size_count, 320u,
                                                 240u));
    assert(!native_camera_v4l2_insert_frame_size(retained_sizes, NATIVE_CAMERA_FRAME_SIZE_MAX, &size_count, 3840u,
                                                 2160u));
    assert(!native_camera_v4l2_insert_frame_size(retained_sizes, NATIVE_CAMERA_FRAME_SIZE_MAX, &size_count, 641u,
                                                 480u));
    assert(!native_camera_v4l2_insert_frame_size(retained_sizes, NATIVE_CAMERA_FRAME_SIZE_MAX, &size_count, 640u,
                                                 481u));
    assert(size_count == 0u);
    static const uint16_t reported_sizes[][2] = {{1280u, 720u}, {640u, 480u},  {1920u, 1080u},
                                                 {960u, 540u},  {1280u, 720u}, {176u, 144u}};
    for (size_t i = 0u; i < sizeof(reported_sizes) / sizeof(reported_sizes[0]); i++) {
        (void)native_camera_v4l2_insert_frame_size(retained_sizes, NATIVE_CAMERA_FRAME_SIZE_MAX, &size_count,
                                                   reported_sizes[i][0], reported_sizes[i][1]);
    }
    static const uint16_t expected_sizes[][2] = {{1920u, 1080u}, {1280u, 720u}, {960u, 540u}, {640u, 480u}};
    assert(size_count == sizeof(expected_sizes) / sizeof(expected_sizes[0]));
    for (size_t i = 0u; i < size_count; i++) {
        assert(retained_sizes[i][0] == expected_sizes[i][0] && retained_sizes[i][1] == expected_sizes[i][1]);
    }
    uint16_t full_sizes[2][2] = {{0u, 0u}};
    size_t full_count = 0u;
    assert(native_camera_v4l2_insert_frame_size(full_sizes, 2u, &full_count, 640u, 480u));
    assert(native_camera_v4l2_insert_frame_size(full_sizes, 2u, &full_count, 1920u, 1080u));
    assert(full_count == 2u);
    /* A larger size displaces the smallest retained one; one smaller than every
     * retained size has nowhere to go and is refused outright. */
    assert(native_camera_v4l2_insert_frame_size(full_sizes, 2u, &full_count, 800u, 600u));
    assert(full_count == 2u && full_sizes[0][0] == 1920u && full_sizes[1][0] == 800u);
    assert(!native_camera_v4l2_insert_frame_size(full_sizes, 2u, &full_count, 640u, 480u));
    assert(full_count == 2u && full_sizes[1][0] == 800u);

    uint8_t mapped[16] = {0};
    NativeCameraFrame imported;
    assert(native_camera_v4l2_import_frame(mapped, sizeof(mapped), 9u, 0u, true, 2u, 77u,
                                            V4L2_BUF_FLAG_ERROR | V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC, 12, 345,
                                            &imported));
    assert(imported.data == mapped && imported.len == 9u && imported.index == 2u && imported.sequence == 77u);
    assert(imported.flags == (V4L2_BUF_FLAG_ERROR | V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC));
    assert(imported.timestamp_us == 12000345u && imported.held);
    assert(!native_camera_v4l2_import_frame(mapped, sizeof(mapped), 0u, 0u, true, 0u, 0u, 0u, 0, 0,
                                             &imported));
    assert(!native_camera_v4l2_import_frame(mapped, sizeof(mapped), sizeof(mapped) + 1u, 0u, true, 0u, 0u, 0u,
                                             0, 0, &imported));
    assert(!native_camera_v4l2_import_frame(mapped, NATIVE_CAMERA_H264_MAX_AU_BYTES + 1u,
                                             NATIVE_CAMERA_H264_MAX_AU_BYTES + 1u, 0u, true, 0u, 0u, 0u, 0, 0,
                                             &imported));
    assert(!native_camera_v4l2_import_frame(mapped, sizeof(mapped), 7u, 8u, false, 0u, 0u, 0u, 0, 0,
                                             &imported));
    assert(!native_camera_v4l2_import_frame(mapped, sizeof(mapped), 8u, 8u, false, 0u, 0u, 0u, -1, 0,
                                             &imported));

    const uint8_t black_white_yuyv[] = {
        16, 128, 235, 128,
    };
    uint8_t rgba[8] = {0};
    assert(native_camera_yuyv_to_rgba(black_white_yuyv,
                                       sizeof(black_white_yuyv), 2, 1, 4,
                                       rgba, sizeof(rgba)));
    assert(rgba[0] == 0 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255);
    assert(rgba[4] == 255 && rgba[5] == 255 && rgba[6] == 255 &&
           rgba[7] == 255);

    const uint8_t padded[] = {
        81, 90, 145, 240, 0, 0, 0, 0,
        81, 90, 145, 240, 0, 0, 0, 0,
    };
    uint8_t two_rows[16];
    memset(two_rows, 0, sizeof(two_rows));
    assert(native_camera_yuyv_to_rgba(padded, sizeof(padded), 2, 2, 8,
                                       two_rows, sizeof(two_rows)));
    assert(memcmp(two_rows, two_rows + 8, 8) == 0);

    assert(!native_camera_yuyv_to_rgba(padded, sizeof(padded), 1, 2, 8,
                                        two_rows, sizeof(two_rows)));
    assert(!native_camera_yuyv_to_rgba(padded, 7, 2, 1, 8, two_rows,
                                        sizeof(two_rows)));
    assert(!native_camera_yuyv_to_rgba(padded, sizeof(padded), 2, 2, 8,
                                        two_rows, sizeof(two_rows) - 1));

    /* The mode-enumeration walks run against a fake node table; everything above
     * is pure and needs no device at all. */
    native_camera_v4l2_set_test_nodes(&k_fake_nodes);
    test_frame_size_enumeration();
    test_frame_rate_enumeration();
    test_h264_mode_available();
    native_camera_v4l2_set_test_nodes(NULL);

    return 0;
}
