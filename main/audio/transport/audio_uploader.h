#ifndef AUDIO_UPLOADER_H
#define AUDIO_UPLOADER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

void audio_uploader_init(void);
void audio_uploader_send(const int16_t *data, int samples);
// Send arbitrary bytes (used for Opus packets)
void audio_uploader_send_bytes(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif
