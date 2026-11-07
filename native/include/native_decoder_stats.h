#ifndef LGNOME_NATIVE_DECODER_STATS_H
#define LGNOME_NATIVE_DECODER_STATS_H
#include <stdint.h>
#include <stdbool.h>

/* Live primary counter and five-second timing windows; not physical presentation. */
typedef struct NativeDecoderStats {
    bool available;
    uint64_t generation;
    uint32_t primary_total;
    uint32_t at_ms, window_ms, pictures, primary;
    uint64_t decode_p95_us, interval_p95_us, interval_max_us;
    uint32_t intervals, gaps, pending;
} NativeDecoderStats;

#endif
