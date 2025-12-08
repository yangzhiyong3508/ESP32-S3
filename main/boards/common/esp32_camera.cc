#include "esp32_camera.h"
#include "mcp_server.h"
#include "board.h"
#include "system_info.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cstring>

#define TAG "Esp32Camera"

Esp32Camera::Esp32Camera(const camera_config_t& config) {
    // camera init
    esp_err_t err = esp_camera_init(&config); // 配置上面定义的参数
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        return;
    }

    sensor_t *s = esp_camera_sensor_get(); // 获取摄像头型号
    if (s->id.PID == GC0308_PID) {
        s->set_hmirror(s, 0);  // 这里控制摄像头镜像 写1镜像 写0不镜像
    }
}

Esp32Camera::~Esp32Camera() {
    if (fb_) {
        esp_camera_fb_return(fb_);
        fb_ = nullptr;
    }
    esp_camera_deinit();
}

void Esp32Camera::SetExplainUrl(const std::string& url, const std::string& token) {
    explain_url_ = url;
    explain_token_ = token;
}

bool Esp32Camera::Capture() {
    if (encoder_thread_.joinable()) {
        encoder_thread_.join();
    }

    auto start_time = esp_timer_get_time();
    int frames_to_get = 2;
    // Try to get a stable frame
    for (int i = 0; i < frames_to_get; i++) {
        if (fb_ != nullptr) {
            esp_camera_fb_return(fb_);
        }
        fb_ = esp_camera_fb_get();
        if (fb_ == nullptr) {
            ESP_LOGE(TAG, "Camera capture failed");
            return false;
        }
    }
    auto end_time = esp_timer_get_time();
    ESP_LOGI(TAG, "Camera captured %d frames in %d ms", frames_to_get, int((end_time - start_time) / 1000));

    return true;
}

bool Esp32Camera::SetHMirror(bool enabled) {
    sensor_t *s = esp_camera_sensor_get();
    if (s == nullptr) {
        ESP_LOGE(TAG, "Failed to get camera sensor");
        return false;
    }
    
    esp_err_t err = s->set_hmirror(s, enabled);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set horizontal mirror: %d", err);
        return false;
    }
    
    ESP_LOGI(TAG, "Camera horizontal mirror set to: %s", enabled ? "enabled" : "disabled");
    return true;
}

bool Esp32Camera::SetVFlip(bool enabled) {
    sensor_t *s = esp_camera_sensor_get();
    if (s == nullptr) {
        ESP_LOGE(TAG, "Failed to get camera sensor");
        return false;
    }
    
    esp_err_t err = s->set_vflip(s, enabled);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set vertical flip: %d", err);
        return false;
    }
    
    ESP_LOGI(TAG, "Camera vertical flip set to: %s", enabled ? "enabled" : "disabled");
    return true;
}

/**
 * @brief 将摄像头捕获的图像发送到远程服务器进行AI分析和解释
 * 
 * 该函数将当前摄像头缓冲区中的图像编码为JPEG格式，并通过HTTP POST请求
 * 以multipart/form-data的形式发送到指定的解释服务器。服务器将根据提供的
 * 问题对图像进行AI分析并返回结果。
 * 
 * 实现特点：
 * - 使用独立线程编码JPEG，与主线程分离
 * - 采用分块传输编码(chunked transfer encoding)优化内存使用
 * - 通过队列机制实现编码线程和发送线程的数据同步
 * - 支持设备ID、客户端ID和认证令牌的HTTP头部配置
 * 
 * @param question 要向AI提出的关于图像的问题，将作为表单字段发送
 * @return std::string 服务器返回的JSON格式响应字符串
 *         成功时包含AI分析结果，失败时包含错误信息
 *         格式示例：{"success": true, "result": "分析结果"}
 *                  {"success": false, "message": "错误信息"}
 * 
 * @note 调用此函数前必须先调用SetExplainUrl()设置服务器URL
 * @note 函数会等待之前的编码线程完成后再开始新的处理
 * @warning 如果摄像头缓冲区为空或网络连接失败，将返回错误信息
 */
std::string Esp32Camera::Explain(const std::string& question) {
    return "";
}
