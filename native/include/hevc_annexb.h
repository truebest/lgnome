#ifndef LGNOME_HEVC_ANNEXB_H
#define LGNOME_HEVC_ANNEXB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum NativeHevcResult {
    NATIVE_HEVC_OK = 0,
    NATIVE_HEVC_INVALID = 1,
} NativeHevcResult;

typedef struct NativeHevcInfo {
    bool has_vps;
    bool has_sps;
    bool has_pps;
    bool has_irap;
    /* True once an IRAP is encountered after VPS, SPS and PPS in stream order. */
    bool can_seed_decoder;
    size_t nal_count;
} NativeHevcInfo;

/* The private RDPGFX HEVC extension carries Annex-B access units. */
NativeHevcResult native_hevc_scan_annexb(const uint8_t *data, size_t len, NativeHevcInfo *info);

#endif
