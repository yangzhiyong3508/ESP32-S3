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

// 使用简单发送队列，避免在音频回调里直接阻塞 socket
// 将帧拆小以减少一次写入的阻塞概率
#define MAX_FRAME_BYTES 512
#define SEND_QUEUE_LEN 32
// 发送速率限制（字节/秒），超出则短暂延时以平滑带宽
#define SEND_BUDGET_BYTES_PER_SEC 16000
// 当队列积压超过该阈值时，放弃当前帧剩余数据
#define SEND_DROP_THRESHOLD 24

// ---------------- WebSocket 配置 ----------------
#define WEBSOCKET_URI   "ws://192.168.1.106:8080/esp32"
#define TAG             "ws_client"

static esp_websocket_client_handle_t ws_client = NULL;
static bool ws_connected = false;
static QueueHandle_t send_queue = NULL;
static TaskHandle_t send_task_handle = NULL;
static int send_budget = SEND_BUDGET_BYTES_PER_SEC;
static int64_t budget_ts_us = 0;

// 下行数据回调（由上层注册）
static audio_uploader_binary_cb_t binary_cb = NULL;
static audio_uploader_text_cb_t text_cb = NULL;

typedef struct {
    size_t len;
    uint8_t* buf;
} audio_frame_t;

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
            // 清空发送队列，避免积压
            if (send_queue) {
                audio_frame_t frame;
                while (xQueueReceive(send_queue, &frame, 0) == pdTRUE) {
                    free(frame.buf);
                }
            }
            break;
        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == WS_TRANSPORT_OPCODES_BINARY) {
                if (binary_cb) {
                    binary_cb((const uint8_t*)data->data_ptr, data->data_len);
                } else {
                    ESP_LOGI(TAG, "收到二进制数据 len=%d", data->data_len);
                }
            } else {
                if (text_cb) {
                    text_cb((const char*)data->data_ptr, data->data_len);
                } else {
                    ESP_LOGI(TAG, "收到文本: %.*s", data->data_len, (char*)data->data_ptr);
                }
            }
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
        // 设置为 64KB，以适配拆分后的小块累计
        .buffer_size = 65536,
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

// 发送任务：串行发送队列中的 PCM 帧，失败时丢弃该帧并等待重连
static void audio_send_task(void* arg) {
    (void)arg;
    audio_frame_t frame;
    for (;;) {
        if (xQueueReceive(send_queue, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!ws_connected || ws_client == NULL) {
            free(frame.buf);
            continue;
        }

        int ret = esp_websocket_client_send_bin(ws_client, (const char*)frame.buf, frame.len, 500 / portTICK_PERIOD_MS);
        free(frame.buf);

        if (ret <= 0) {
            ESP_LOGW(TAG, "send failed, drop frame len=%d", (int)frame.len);
            // 等待下一次事件驱动的重连
        }

        // 适当让出 CPU，避免连续写填满对端窗口
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ---------------- 公共接口 ----------------
void audio_uploader_init(void) {
    if (send_queue == NULL) {
        send_queue = xQueueCreate(SEND_QUEUE_LEN, sizeof(audio_frame_t));
    }
    if (send_task_handle == NULL) {
        xTaskCreate(audio_send_task, "audio_send", 3072, NULL, 5, &send_task_handle);
    }
    websocket_init();
}

static void enqueue_bytes(const uint8_t* data, size_t len) {
    if (!ws_connected || ws_client == NULL || send_queue == NULL) {
        return; // 未连接直接丢弃
    }

    // 按最大帧拆分，减少单次写阻塞
    const uint8_t* ptr = (const uint8_t*)data;
    size_t remaining = len;
    while (remaining > 0) {
        size_t chunk = remaining > MAX_FRAME_BYTES ? MAX_FRAME_BYTES : remaining;

        // 简单令牌桶：每秒允许 SEND_BUDGET_BYTES_PER_SEC，超出则等待 10ms
        int64_t now = esp_timer_get_time();
        if (budget_ts_us == 0 || now - budget_ts_us >= 1000000) {
            budget_ts_us = now;
            send_budget = SEND_BUDGET_BYTES_PER_SEC;
        }
        if (send_budget <= 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (chunk > send_budget) {
            chunk = send_budget;
        }

        audio_frame_t frame = {0};
        frame.len = chunk;
        frame.buf = (uint8_t*)malloc(chunk);
        if (!frame.buf) {
            return; // 内存不足直接放弃剩余
        }
        memcpy(frame.buf, ptr, chunk);

        if (xQueueSend(send_queue, &frame, 0) != pdTRUE) {
            free(frame.buf); // 队列满则丢弃当前块
            return; // 避免积压，剩余也放弃
        }

        // 队列积压过高，主动丢弃剩余数据，防止 AFE 堵塞
        if (uxQueueMessagesWaiting(send_queue) > SEND_DROP_THRESHOLD) {
            return;
        }

        ptr += chunk;
        remaining -= chunk;
        send_budget -= chunk;
    }
}

void audio_uploader_send_bytes(const uint8_t *data, size_t len) {
    enqueue_bytes(data, len);
}

void audio_uploader_set_binary_cb(audio_uploader_binary_cb_t cb) {
    binary_cb = cb;
}

void audio_uploader_set_text_cb(audio_uploader_text_cb_t cb) {
    text_cb = cb;
}

void audio_uploader_send(const int16_t *data, int samples) {
    enqueue_bytes((const uint8_t*)data, samples * sizeof(int16_t));
}
