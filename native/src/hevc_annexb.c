#include "hevc_annexb.h"

#include <string.h>

#define HEVC_NAL_UNIT_TYPE_IRAP_FIRST 16
#define HEVC_NAL_UNIT_TYPE_IRAP_LAST 21
#define HEVC_NAL_UNIT_TYPE_VPS 32
#define HEVC_NAL_UNIT_TYPE_SPS 33
#define HEVC_NAL_UNIT_TYPE_PPS 34

static bool annexb_prefix_at(const uint8_t *data, size_t len, size_t pos, size_t *prefix_len) {
    if (pos + 3 <= len && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1) {
        *prefix_len = 3;
        return true;
    }
    if (pos + 4 <= len && data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 0 && data[pos + 3] == 1) {
        *prefix_len = 4;
        return true;
    }
    return false;
}

static bool find_annexb_prefix(const uint8_t *data, size_t len, size_t pos, size_t *prefix_pos, size_t *prefix_len) {
    while (pos + 3 <= len) {
        if (annexb_prefix_at(data, len, pos, prefix_len)) {
            *prefix_pos = pos;
            return true;
        }
        pos++;
    }
    return false;
}

static void scan_nal_type(uint8_t nal_header, NativeHevcInfo *info) {
    uint8_t nal_type = (uint8_t)((nal_header >> 1) & 0x3f);

    if (nal_type == HEVC_NAL_UNIT_TYPE_VPS) {
        info->has_vps = true;
    } else if (nal_type == HEVC_NAL_UNIT_TYPE_SPS) {
        info->has_sps = true;
    } else if (nal_type == HEVC_NAL_UNIT_TYPE_PPS) {
        info->has_pps = true;
    } else if (nal_type >= HEVC_NAL_UNIT_TYPE_IRAP_FIRST && nal_type <= HEVC_NAL_UNIT_TYPE_IRAP_LAST) {
        info->has_irap = true;
        if (info->has_vps && info->has_sps && info->has_pps) {
            info->can_seed_decoder = true;
        }
    }
}

NativeHevcResult native_hevc_scan_annexb(const uint8_t *data, size_t len, NativeHevcInfo *info) {
    if (!data || !info) {
        return NATIVE_HEVC_INVALID;
    }

    memset(info, 0, sizeof(*info));

    size_t pos = 0;
    size_t prefix_pos = 0;
    size_t prefix_len = 0;
    while (find_annexb_prefix(data, len, pos, &prefix_pos, &prefix_len)) {
        size_t nal_start = prefix_pos + prefix_len;
        size_t next_prefix_pos = len;
        size_t next_prefix_len = 0;
        if (find_annexb_prefix(data, len, nal_start, &next_prefix_pos, &next_prefix_len)) {
            (void)next_prefix_len;
        }

        /* HEVC has a two-byte header with nonzero temporal_id_plus1. */
        if (next_prefix_pos - nal_start < 2 || (data[nal_start] & 0x80) != 0 ||
            (data[nal_start + 1] & 7) == 0) {
            return NATIVE_HEVC_INVALID;
        }
        scan_nal_type(data[nal_start], info);
        info->nal_count++;

        pos = next_prefix_pos;
    }

    if (!info->nal_count) {
        return NATIVE_HEVC_INVALID;
    }
    return NATIVE_HEVC_OK;
}
