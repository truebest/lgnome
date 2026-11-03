#ifndef LGNOME_NATIVE_QOS_H
#define LGNOME_NATIVE_QOS_H
#include <stdint.h>
#include <string.h>

/* Upper-bound histogram, 250 us buckets; the overflow bucket reports the observed max. */
typedef struct NativeQosSamples {
    uint32_t bins[513];
    uint32_t count;
    uint64_t max_us;
} NativeQosSamples;

static inline void native_qos_record(NativeQosSamples *samples, uint64_t us) {
    unsigned bucket = us >= 128000 ? 512 : (unsigned)((us + 249) / 250);
    samples->bins[bucket]++;
    samples->count++;
    if (us > samples->max_us) samples->max_us = us;
}
static inline uint64_t native_qos_p95(const NativeQosSamples *samples) {
    uint64_t target = ((uint64_t)samples->count * 95 + 99) / 100;
    uint64_t count = 0;
    for (unsigned i = 0; i < 512; i++) {
        count += samples->bins[i];
        if (count >= target) return i * 250u;
    }
    return samples->max_us;
}
static inline void native_qos_clear(NativeQosSamples *samples) {
    memset(samples, 0, sizeof(*samples));
}
#endif
