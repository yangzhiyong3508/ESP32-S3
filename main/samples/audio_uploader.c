#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_websocket_client.h"
#include "audio_uploader.h"

// ---------------- WebSocket 配置 ----------------
#define WEBSOCKET_URI   "ws://192.168.1.102:8080/esp32"
#define TAG             "ws_client"

static esp_websocket_client_handle_t ws_client = NULL;
static bool ws_connected = false;

// ---------------- WebSocket 事件 ----------------
static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                   int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ws_connected = true;
            ESP_LOGI(TAG, "WebSocket已连接");
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ws_connected = false;
            ESP_LOGI(TAG, "WebSocket已断开");
            break;
        case WEBSOCKET_EVENT_DATA:
            ESP_LOGI(TAG, "收到: %.*s", data->data_len, (char*)data->data_ptr);
            break;
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket错误");
            break;
    }
}

// ---------------- WebSocket 初始化 ----------------
static void websocket_init(void) {
    esp_websocket_client_config_t config = {
        .uri = WEBSOCKET_URI,
        .reconnect_timeout_ms = 10000,
        .network_timeout_ms = 10000,
        // 增大发送缓冲区，以应对网络抖动
        // 16000Hz * 2bytes * 1s = 32000 bytes
        // 设置为 32KB，大约可以缓存 1 秒的音频数据
        .buffer_size = 32768,
        // 启用 TCP keep-alive，避免长时间静默被对端关闭
        .keep_alive_enable = true,
        .keep_alive_idle = 5,      // 秒，空闲多久开始发保活
        .keep_alive_interval = 5,  // 秒，保活包间隔
        .keep_alive_count = 3,     // 尝试次数
    };
    ws_client = esp_websocket_client_init(&config);
    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY,
                                  websocket_event_handler, NULL);
    esp_websocket_client_start(ws_client);
}

// ---------------- 公共接口 ----------------
void audio_uploader_init(void) {
    websocket_init();
}

void audio_uploader_send(const int16_t *data, int samples) {
    // 非阻塞发送，掉线时直接丢弃避免阻塞音频链路
    if (!ws_connected || ws_client == NULL) {
        return;
    }
    
    // 发送 PCM 二进制流
    // 使用较短的超时时间(20ms)，避免阻塞音频线程太久
    // 由于缓冲区已增大，一般情况下不会阻塞
    esp_websocket_client_send_bin(ws_client, (const char*)data, samples * sizeof(int16_t), 20 / portTICK_PERIOD_MS);
}
