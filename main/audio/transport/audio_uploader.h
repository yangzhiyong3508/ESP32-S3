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

// ---------------- Downlink callbacks ----------------
typedef void (*audio_uploader_binary_cb_t)(const uint8_t *data, size_t len);
typedef void (*audio_uploader_text_cb_t)(const char *data, size_t len);

// Register callbacks for data received from server
void audio_uploader_set_binary_cb(audio_uploader_binary_cb_t cb);
void audio_uploader_set_text_cb(audio_uploader_text_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif
