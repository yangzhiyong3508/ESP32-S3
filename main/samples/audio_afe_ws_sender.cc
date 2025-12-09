#include "audio_uploader.h"
#include "audio_service.h"
#include "boards/common/wifi_connect.h"
#include <esp_log.h>

#define TAG "AFE_WS_SENDER"

static bool ws_ready = false;

// 延迟到 WiFi 已连接后再去初始化 WebSocket，避免在 netif 未就绪时崩溃
extern "C" void audio_afe_ws_sender_init(void) {
    if (ws_ready) {
        return;
    }
    if (!wifi_is_connected()) {
        return;
    }
    audio_uploader_init();
    ws_ready = true;
    ESP_LOGI(TAG, "AFE WebSocket sender initialized after WiFi up");
}

// 发送降噪/回声消除后的语音流
void audio_afe_ws_send(const int16_t *data, int samples) {
    audio_afe_ws_sender_init();
    if (!ws_ready) {
        return; // WiFi 尚未连上，丢弃
    }
    audio_uploader_send(data, samples);
}

// 挂载到 AudioService 的 AFE输出回调
void audio_afe_ws_hook(AudioService* service) {
    service->SetAfeOutputCallback([](std::vector<int16_t>&& pcm) {
        audio_afe_ws_send(pcm.data(), pcm.size());
    });
}

// 用法：
// 1. 在 app 初始化时调用 audio_afe_ws_sender_init();
// 2. 在 AudioService 初始化后调用 audio_afe_ws_hook(&audio_service);
