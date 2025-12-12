#include "audio_uploader.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_timer.h"

// ---------------- 配置 ----------------
#define WEBSOCKET_URI           "ws://192.168.1.105:8080/esp32"
#define TAG                     "WS_UPLOADER"

// 队列深度：Opus 60ms帧，1秒约16帧。设置50可以缓冲约3秒的网络抖动
#define SEND_QUEUE_LEN          50 
#define WS_SEND_TIMEOUT_MS      1000

// ---------------- 状态管理 ----------------
static esp_websocket_client_handle_t ws_client = NULL;
static QueueHandle_t send_queue = NULL;
static TaskHandle_t send_task_handle = NULL;

// 使用 volatile bool 避免多线程锁竞争，快速判断连接状态
static volatile bool is_connected = false;

// 回调函数
static audio_uploader_binary_cb_t binary_cb = NULL;
static audio_uploader_text_cb_t text_cb = NULL;

typedef struct {
    size_t len;
    uint8_t* buf; // 拥有所有权，需要在发送后 free
} queue_item_t;

// ---------------- WebSocket 事件处理 ----------------
static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket Connected!");
            is_connected = true;
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket Disconnected!");
            is_connected = false;
            break;

        case WEBSOCKET_EVENT_DATA:
            // 处理下行数据 (服务器发来的音频或指令)
            if (data->op_code == WS_TRANSPORT_OPCODES_BINARY) {
                if (binary_cb) binary_cb((const uint8_t*)data->data_ptr, data->data_len);
            } else if (data->op_code == WS_TRANSPORT_OPCODES_TEXT) {
                if (text_cb) text_cb((const char*)data->data_ptr, data->data_len);
                else ESP_LOGI(TAG, "Received Text: %.*s", data->data_len, (char*)data->data_ptr);
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket Error!");
            break;
    }
}

// ---------------- 发送任务 (消费者) ----------------
static void audio_send_task(void* arg) {
    queue_item_t item;
    
    while (true) {
        // 永久阻塞等待队列数据，避免 CPU 空转
        if (xQueueReceive(send_queue, &item, portMAX_DELAY) == pdTRUE) {
            
            // 再次检查连接状态
            if (is_connected && ws_client != NULL) {
                // 发送数据：注意这里不分片！Opus 包必须完整发送
                int ret = esp_websocket_client_send_bin(ws_client, (const char*)item.buf, item.len, pdMS_TO_TICKS(WS_SEND_TIMEOUT_MS));
                
                if (ret < 0) {
                    ESP_LOGE(TAG, "Send failed, ret=%d. Connection might be unstable.", ret);
                    // 发送失败通常意味着连接出问题了，暂时标记为断开，等待重连事件
                    // is_connected = false; // 可选：让事件回调去处理状态
                }
            } else {
                // 如果取出数据时发现断连了，静默丢弃，避免日志刷屏
                // 可以每隔 100 个包打印一次警告，这里为了清爽省略
            }

            // 无论发送成功与否，必须释放内存
            if (item.buf) {
                free(item.buf);
            }
        }
    }
}

// ---------------- 公共接口 ----------------

void audio_uploader_init(void) {
    // 1. 创建队列
    if (send_queue == NULL) {
        send_queue = xQueueCreate(SEND_QUEUE_LEN, sizeof(queue_item_t));
    }

    // 2. 初始化 WebSocket
    esp_websocket_client_config_t config = {
        .uri = WEBSOCKET_URI,
        .reconnect_timeout_ms = 5000,   // 缩短重连间隔
        .network_timeout_ms = 10000,    // 增加网络超时时间
        .buffer_size = 4096,            // 接收缓冲区
        .disable_auto_reconnect = false,
        .keep_alive_enable = true,
        .keep_alive_idle = 5,
        .keep_alive_interval = 5,
        .keep_alive_count = 3
    };

    ws_client = esp_websocket_client_init(&config);
    esp_websocket_register_events(ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    esp_websocket_client_start(ws_client);

    // 3. 创建发送任务
    if (send_task_handle == NULL) {
        // 优先级设置适中，不要太高抢占音频处理，也不要太低发不出去
        xTaskCreate(audio_send_task, "ws_send_task", 4096, NULL, 5, &send_task_handle);
    }
}

void audio_uploader_send_bytes(const uint8_t *data, size_t len) {
    // 1. 快速检查连接状态：如果断连，直接丢弃，不进队列，防止内存耗尽
    if (!is_connected || data == NULL || len == 0) {
        return;
    }

    // 2. 检查队列余量：如果队列太满（说明网络发不出去），丢弃最新的，保全实时性
    if (uxQueueSpacesAvailable(send_queue) < 5) {
        ESP_LOGW(TAG, "Queue full, dropping packet to reduce latency");
        return;
    }

    // 3. 分配内存并复制数据
    // 注意：这里必须 copy，因为上层 buffer (Opus payload) 马上会被复用或释放
    uint8_t* buf_copy = (uint8_t*)malloc(len);
    if (!buf_copy) {
        ESP_LOGE(TAG, "Malloc failed");
        return;
    }
    memcpy(buf_copy, data, len);

    queue_item_t item = {
        .len = len,
        .buf = buf_copy
    };

    // 4. 入队
    if (xQueueSend(send_queue, &item, 0) != pdTRUE) {
        // 极罕见情况：刚检查还有空间，现在满了
        free(buf_copy);
    }
}

// 兼容接口：如果还想发 PCM，封装一下即可
void audio_uploader_send(const int16_t *data, int samples) {
    audio_uploader_send_bytes((const uint8_t*)data, samples * sizeof(int16_t));
}

void audio_uploader_set_binary_cb(audio_uploader_binary_cb_t cb) {
    binary_cb = cb;
}

void audio_uploader_set_text_cb(audio_uploader_text_cb_t cb) {
    text_cb = cb;
}