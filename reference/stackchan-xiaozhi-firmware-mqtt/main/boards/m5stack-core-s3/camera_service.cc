#include "camera_service.h"
#include "motion_manager.h"
#include "push_mqtt_manager.h"
#include "config.h"
#include "display/lvgl_display/jpg/image_to_jpeg.h"
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#define TAG "CameraService"

CameraService::~CameraService() = default;

bool CameraService::Initialize(const Config& config) {
    config_ = config;

    static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
        .data_width = CAM_CTLR_DATA_WIDTH_8,
        .data_io = {
            [0] = CAMERA_PIN_D0,
            [1] = CAMERA_PIN_D1,
            [2] = CAMERA_PIN_D2,
            [3] = CAMERA_PIN_D3,
            [4] = CAMERA_PIN_D4,
            [5] = CAMERA_PIN_D5,
            [6] = CAMERA_PIN_D6,
            [7] = CAMERA_PIN_D7,
        },
        .vsync_io = CAMERA_PIN_VSYNC,
        .de_io = CAMERA_PIN_HREF,
        .pclk_io = CAMERA_PIN_PCLK,
        .xclk_io = CAMERA_PIN_XCLK,
    };

    esp_video_init_sccb_config_t sccb_config = {
        .init_sccb = false,
        .i2c_handle = config_.i2c_bus,
        .freq = 100000,
    };

    esp_video_init_dvp_config_t dvp_config = {
        .sccb_config = sccb_config,
        .reset_pin = CAMERA_PIN_RESET,
        .pwdn_pin = CAMERA_PIN_PWDN,
        .dvp_pin = dvp_pin_config,
        .xclk_freq = XCLK_FREQ_HZ,
    };

    esp_video_init_config_t video_config = {
        .dvp = &dvp_config,
    };

    video_ = std::make_unique<EspVideo>(video_config);
    if (video_) {
        video_->SetHMirror(true);
    }
    return IsOk();
}

bool CameraService::IsOk() const {
    return video_ && video_->IsOk();
}

EspVideo* CameraService::GetVideoDevice() {
    return video_.get();
}

void CameraService::SnapPhoto() {
    if (!config_.push_mqtt || !config_.push_mqtt->IsConnected()) {
        ESP_LOGE(TAG, "snap: push mqtt not ready");
        return;
    }
    if (!video_ || !video_->IsOk()) {
        ESP_LOGE(TAG, "snap: camera not ready");
        return;
    }
    if (config_.face_tracker) {
        config_.face_tracker->Pause(false);
    }
    if (!video_->Capture() || video_->FrameData() == nullptr || video_->FrameLen() == 0) {
        ESP_LOGE(TAG, "snap: capture failed");
        if (config_.face_tracker) config_.face_tracker->Resume();
        return;
    }
    uint16_t w = video_->FrameWidth();
    uint16_t h = video_->FrameHeight();
    // 头报文: [0x01][w:2][h:2]
    uint8_t hdr[5] = {0x01, (uint8_t)(w >> 8), (uint8_t)(w & 0xFF),
                      (uint8_t)(h >> 8), (uint8_t)(h & 0xFF)};
    config_.push_mqtt->Publish(config_.push_mqtt->PhotoTopic(), hdr, sizeof(hdr), 1, 0);  // QoS1: 防丢块

    struct SnapCtx {
        PushMqttManager* mqtt = nullptr;
        uint16_t seq = 0;
        uint32_t total = 0;
        uint8_t* chunk = nullptr;  // PSRAM 缓冲(不占用 MQTT 任务栈)
    } ctx;
    ctx.mqtt = config_.push_mqtt;
    ctx.chunk = (uint8_t*)heap_caps_malloc(7003, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx.chunk == nullptr) {
        ESP_LOGE(TAG, "snap: no psram for chunk buffer");
        if (config_.face_tracker) config_.face_tracker->Resume();
        return;
    }
    bool ok = image_to_jpeg_cb(
        const_cast<uint8_t*>(video_->FrameData()), video_->FrameLen(), w, h,
        video_->FrameFormat(), 80,
        [](void* arg, size_t index, const void* data, size_t len) -> size_t {
            auto* c = static_cast<SnapCtx*>(arg);
            const uint8_t* p = (const uint8_t*)data;
            size_t off = 0;
            while (off < len) {
                size_t n = len - off > 7000 ? 7000 : len - off;
                c->chunk[0] = 0x02;
                c->chunk[1] = (uint8_t)(c->seq >> 8);
                c->chunk[2] = (uint8_t)(c->seq & 0xFF);
                memcpy(c->chunk + 3, p + off, n);
                c->mqtt->Publish(c->mqtt->PhotoTopic(), c->chunk, 3 + n, 1, 0);  // QoS1
                c->seq++;
                c->total += (uint32_t)n;
                off += n;
                vTaskDelay(pdMS_TO_TICKS(20));  // 节流: 防 outbox 突发溢出丢块
            }
            return len;
        },
        &ctx);
    heap_caps_free(ctx.chunk);
    // 结束报文: [0x03][total:4] —— 网关据此校验重组完整性
    uint8_t end[5] = {0x03, (uint8_t)(ctx.total >> 24), (uint8_t)(ctx.total >> 16),
                      (uint8_t)(ctx.total >> 8), (uint8_t)(ctx.total & 0xFF)};
    config_.push_mqtt->Publish(config_.push_mqtt->PhotoTopic(), end, sizeof(end), 1, 0);  // QoS1
    ESP_LOGI(TAG, "snap done: %ux%u jpeg_ok=%d chunks=%u",
             w, h, ok ? 1 : 0, (unsigned)ctx.seq);
    if (config_.face_tracker) {
        config_.face_tracker->Resume();
    }
}
