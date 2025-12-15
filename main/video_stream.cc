#include "video_stream.h"
#include "board.h"
#include "audio/audio_codec.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "VideoStream"

static esp_websocket_client_handle_t client = nullptr;
static bool is_connected = false;

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WEBSOCKET_EVENT_CONNECTED");
            is_connected = true;
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WEBSOCKET_EVENT_DISCONNECTED");
            is_connected = false;
            Board::GetInstance().GetDisplay()->ShowNotification("视频连接断开", 2000);
            break;
        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == WS_TRANSPORT_OPCODES_TEXT && data->data_len > 0 && data->data_ptr) {
                std::string volume_str(data->data_ptr, data->data_len);
                int volume = atoi(volume_str.c_str());
                ESP_LOGI(TAG, "Received volume from server: %d", volume);
                auto codec = Board::GetInstance().GetAudioCodec();
                if (codec) {
                    codec->SetOutputVolume(volume);
                }
            }
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WEBSOCKET_EVENT_ERROR");
            Board::GetInstance().GetDisplay()->ShowNotification("视频连接错误", 2000);
            break;
    }
}

static void video_stream_task(void *pvParameters) {
    Camera* camera = Board::GetInstance().GetCamera();
    if (!camera) {
        ESP_LOGE(TAG, "Camera not found");
        vTaskDelete(NULL);
        return;
    }

    // 自适应帧率控制变量
    const int MIN_DELAY_MS = 50;    // ~20 FPS
    const int MAX_DELAY_MS = 200;   // ~5 FPS
    int current_delay = MIN_DELAY_MS;
    int error_count = 0;
    const int ERROR_THRESHOLD = 3;

    while (1) {
        // 使用 API 检查连接状态
        if (esp_websocket_client_is_connected(client) && is_connected) {
            if (camera->Capture()) {
                size_t len = 0;
                const uint8_t* data = camera->GetFrameJpeg(&len);
                if (data && len > 0) {
                    int ret = esp_websocket_client_send_bin(client, (const char*)data, len, pdMS_TO_TICKS(500));
                    if (ret < 0) {
                         // 仅在非连续错误时打印错误日志，避免刷屏
                         if (error_count == 0) {
                            ESP_LOGE(TAG, "Failed to send video frame, ret=%d", ret);
                         }
                         error_count++;

                         // 如果连续出错，降低帧率
                         if (error_count > ERROR_THRESHOLD) {
                             current_delay = std::min(current_delay * 2, MAX_DELAY_MS);
                             ESP_LOGW(TAG, "High error rate, decreasing FPS. Delay: %d ms", current_delay);
                             error_count = 0; // 重置计数
                         }
                         
                         // 如果发送失败且API显示未连接，强制等待更久
                         if (!esp_websocket_client_is_connected(client)) {
                             ESP_LOGW(TAG, "Connection lost detected during send, pausing...");
                             continue;
                         }
                    } else {
                         // 发送成功，逐渐恢复帧率
                         if (current_delay > MIN_DELAY_MS) {
                             current_delay = std::max(current_delay - 10, MIN_DELAY_MS);
                         }
                         error_count = 0;
                    }
                }
            }
        } else {
             // 未连接时，等待较长时间
             // 断连时重置延迟
             current_delay = MIN_DELAY_MS;
             continue;
        }
        vTaskDelay(pdMS_TO_TICKS(current_delay)); 
    }
}

void start_video_stream(const std::string& url) {
    esp_websocket_client_config_t websocket_cfg = {};
    websocket_cfg.uri = url.c_str();
    websocket_cfg.reconnect_timeout_ms = 10000;
    websocket_cfg.network_timeout_ms = 20000;
    websocket_cfg.buffer_size = 20 * 1024; // 20KB RX buffer
    websocket_cfg.disable_auto_reconnect = false;

    client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);

    esp_websocket_client_start(client);

    xTaskCreate(video_stream_task, "video_stream", 4096, NULL, 2, NULL);
}
