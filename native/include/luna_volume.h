#ifndef LGNOME_LUNA_VOLUME_H
#define LGNOME_LUNA_VOLUME_H

#include <stdatomic.h>
#include <stdbool.h>

/* System volume over in-process LS2, independent of app mix gains.
 * See docs/native-runbook.md, System volume, for platform limitations. */

typedef struct NativeLunaVolume {
    void *impl; /* transport state (LS2 loop thread on webOS; NULL on host builds) */
    /* Results (atomics: read from the SDL render path every frame). */
    atomic_int volume_pct; /* 0..100; -1 = unknown/unavailable */
    atomic_bool muted;
    atomic_bool available;  /* the bus answered at least once */
    atomic_uint reply_seq;  /* bumps on every parsed reply — freshness fence for baselines */
} NativeLunaVolume;

/* Starts the LS2 thread; callers poll via refresh. Unavailable without Luna. */
bool native_luna_volume_start(NativeLunaVolume *lv);
void native_luna_volume_stop(NativeLunaVolume *lv);

/* Queue an asynchronous getVolume ahead of the next poll (e.g. on overlay open). */
void native_luna_volume_refresh(NativeLunaVolume *lv);

/* Queue an asynchronous setVolume of the newest value (0..100, clamped); updates the
 * cached volume optimistically so the UI tracks the knob immediately. */
void native_luna_volume_set(NativeLunaVolume *lv, int pct);

/* Cached system volume: 0..100, or -1 while unknown/unavailable. */
int native_luna_volume_cached(const NativeLunaVolume *lv);
bool native_luna_volume_available(const NativeLunaVolume *lv);

/* Record reply_seq and wait for it to advance before trusting a new baseline. */
unsigned native_luna_volume_reply_seq(const NativeLunaVolume *lv);

/* Pure reply parser (unit-tested): extracts "volume" and "muteStatus" from a Luna JSON
 * reply. Returns false when the reply carries no usable volume (returnValue false,
 * malformed, or the field is absent). */
bool native_luna_volume_parse(const char *json, int *volume, bool *muted);

#endif
