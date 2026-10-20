#include "hevc_annexb.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int expect_true(int condition, const char *name) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", name);
        return 1;
    }
    return 0;
}

/* One NAL per type, header only: the scanner reads the type out of the first header
 * byte and never parses the payload. */
static size_t build_annexb(uint8_t *dst, const uint8_t *nal_types, size_t nal_count) {
    size_t pos = 0;
    for (size_t i = 0; i < nal_count; i++) {
        dst[pos++] = 0;
        dst[pos++] = 0;
        dst[pos++] = 0;
        dst[pos++] = 1;
        dst[pos++] = (uint8_t)(nal_types[i] << 1); /* forbidden_zero + type, layer id 0 */
        dst[pos++] = 1;                            /* nuh_temporal_id_plus1 */
    }
    return pos;
}

static int expect_scan_case(const char *name, const uint8_t *nal_types, size_t nal_count, bool has_vps,
                            bool has_sps, bool has_pps, bool has_irap, bool can_seed_decoder) {
    uint8_t au[64];
    size_t len = build_annexb(au, nal_types, nal_count);
    NativeHevcInfo info;
    int failures = 0;

    failures += expect_true(native_hevc_scan_annexb(au, len, &info) == NATIVE_HEVC_OK, name);
    failures += expect_true(info.nal_count == nal_count, "nal count");
    failures += expect_true(info.has_vps == has_vps, "VPS flag");
    failures += expect_true(info.has_sps == has_sps, "SPS flag");
    failures += expect_true(info.has_pps == has_pps, "PPS flag");
    failures += expect_true(info.has_irap == has_irap, "IRAP flag");
    failures += expect_true(info.can_seed_decoder == can_seed_decoder, "seed verdict");

    if (failures) {
        fprintf(stderr, "  case: %s\n", name);
    }
    return failures;
}

int main(void) {
    int failures = 0;

    /* AUD, VPS, SPS, PPS, IDR_W_RADL. */
    const uint8_t seed[] = {35, 32, 33, 34, 19};
    failures += expect_scan_case("parameter sets then IDR", seed, sizeof(seed), true, true, true, true, true);

    /* CRA_NUT is an IRAP too, so it seeds the decoder just as well. */
    const uint8_t cra[] = {32, 33, 34, 21};
    failures += expect_scan_case("CRA counts as IRAP", cra, sizeof(cra), true, true, true, true, true);

    /* A delta picture alone: nothing to start from. */
    const uint8_t delta[] = {35, 1};
    failures += expect_scan_case("trailing picture only", delta, sizeof(delta), false, false, false, false, false);

    /* Parameter sets without a picture must not be mistaken for a seed: the decoder
     * would report success and then have nothing to decode. */
    const uint8_t config_only[] = {32, 33, 34};
    failures += expect_scan_case("parameter sets only", config_only, sizeof(config_only), true, true, true, false,
                                 false);

    /* An IRAP that arrives before its parameter sets cannot seed either. */
    const uint8_t idr_first[] = {19, 32, 33, 34};
    failures += expect_scan_case("IDR before its parameter sets", idr_first, sizeof(idr_first), true, true, true,
                                 true, false);

    /* H.264 bytes must not pass as HEVC: an AVC length-prefixed AU has no start code. */
    const uint8_t avc[] = {0x00, 0x00, 0x00, 0x02, 0x65, 0x88};
    NativeHevcInfo info;
    failures += expect_true(native_hevc_scan_annexb(avc, sizeof(avc), &info) == NATIVE_HEVC_INVALID,
                            "AVC framing rejected");

    /* Three-byte start codes are equally valid. */
    const uint8_t short_prefix[] = {0x00, 0x00, 0x01, (uint8_t)(19 << 1), 0x01};
    failures += expect_true(native_hevc_scan_annexb(short_prefix, sizeof(short_prefix), &info) == NATIVE_HEVC_OK,
                            "three-byte start code");
    failures += expect_true(info.has_irap, "three-byte start code IRAP flag");

    const uint8_t reserved[] = {32, 33, 34, 22, 23};
    failures += expect_scan_case("reserved IRAP types cannot seed", reserved, sizeof(reserved),
                                 true, true, true, false, false);
    const uint8_t malformed[][6] = {
        {0, 0, 0, 1, 0xa6, 1},
        {0, 0, 0, 1, 0x26, 0},
    };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); i++) {
        failures += expect_true(native_hevc_scan_annexb(malformed[i], sizeof(malformed[i]), &info) ==
                                    NATIVE_HEVC_INVALID, "malformed NAL header rejected");
    }
    failures += expect_true(native_hevc_scan_annexb(short_prefix, sizeof(short_prefix) - 1, &info) ==
                                NATIVE_HEVC_INVALID, "truncated NAL header rejected");

    failures += expect_true(native_hevc_scan_annexb(NULL, 0, &info) == NATIVE_HEVC_INVALID, "NULL data rejected");
    failures += expect_true(native_hevc_scan_annexb(avc, sizeof(avc), NULL) == NATIVE_HEVC_INVALID,
                            "NULL info rejected");

    if (failures) {
        fprintf(stderr, "FAIL hevc-annexb (%d)\n", failures);
        return 1;
    }
    printf("PASS hevc-annexb\n");
    return 0;
}
