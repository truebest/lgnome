#ifndef GNOMECAST_NATIVE_RDP_VIDEO_H
#define GNOMECAST_NATIVE_RDP_VIDEO_H

#include <stddef.h>
#include <stdint.h>

typedef struct NativeSessionSlot NativeSessionSlot;

/* Single compressed-video ingest path; classifies raw AU bytes before routing. The
 * switch replay feeds cached AUs through it so it takes the same path as live input. */
void native_video_ingest_au(NativeSessionSlot *slot, const uint8_t *data, size_t len, uint64_t pts90k);

#endif
