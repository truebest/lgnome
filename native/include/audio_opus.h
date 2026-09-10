#ifndef LGNOME_AUDIO_OPUS_H
#define LGNOME_AUDIO_OPUS_H

#include <stddef.h>
#include <stdint.h>

/* Per-session libopus decoder, called on the RDP worker before mixing. */

typedef struct NativeOpusDecoder NativeOpusDecoder;

/* Returns NULL when libopus is not compiled in or the decoder cannot be created. */
NativeOpusDecoder *native_opus_decoder_open(uint32_t sample_rate, uint16_t channels);

/* Decodes one Opus packet. Returns the number of frames decoded (>0) and points *pcm at
 * an internal interleaved S16 buffer valid until the next call; returns 0 for a packet
 * that should simply be skipped (corrupt/oversized — logged once per decoder). */
int native_opus_decoder_decode(NativeOpusDecoder *decoder, const uint8_t *data, size_t len, const int16_t **pcm);

void native_opus_decoder_close(NativeOpusDecoder *decoder);

#endif
