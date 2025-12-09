#ifndef AUDIO_AFE_WS_SENDER_H
#define AUDIO_AFE_WS_SENDER_H

#include "audio_service.h"

#ifdef __cplusplus
extern "C" {
#endif

void audio_afe_ws_sender_init(void);

#ifdef __cplusplus
}
#endif

void audio_afe_ws_hook(AudioService* service);

#endif // AUDIO_AFE_WS_SENDER_H
