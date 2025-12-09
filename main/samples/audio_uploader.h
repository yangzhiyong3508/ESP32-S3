#ifndef AUDIO_UPLOADER_H
#define AUDIO_UPLOADER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

void audio_uploader_init(void);
void audio_uploader_send(const int16_t *data, int samples);

#ifdef __cplusplus
}
#endif

#endif
