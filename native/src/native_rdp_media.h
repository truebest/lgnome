#ifndef LGNOME_NATIVE_RDP_MEDIA_H
#define LGNOME_NATIVE_RDP_MEDIA_H

#include <stddef.h>
#include <stdint.h>

#include "media_backend.h"

typedef struct App App;

/* Shared media-pipeline helpers. Callers must hold app->video_lock. */
NativeMedia *native_ensure_media_locked(App *app);
void native_open_speculative_audio_locked(App *app);

/* NDL pump -> shared media feed bridge (ctx is App). */
void native_audio_pipeline_feed_cb(void *ctx, const int16_t *samples, size_t frames);

#endif
