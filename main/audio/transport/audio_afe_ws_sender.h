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

// 挂载 AFE 输出回调（PCM）
void audio_afe_ws_hook(AudioService* service);

// 附加发送回调：将 Opus 编码后的数据通过 WebSocket 发送
void audio_afe_ws_attach_send_callbacks(AudioService* service, AudioServiceCallbacks& callbacks);

#endif // AUDIO_AFE_WS_SENDER_H
