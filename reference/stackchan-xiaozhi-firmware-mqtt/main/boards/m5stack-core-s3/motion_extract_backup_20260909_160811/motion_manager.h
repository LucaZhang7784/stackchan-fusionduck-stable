#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <sys/ioctl.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_video.h"
#include "SCSCL.h"

class StackChanServo;
struct ServoAnimCtx { StackChanServo* servo; int base_yaw; int base_pitch; };
class StackChanServo {
public:
    static constexpr uint64_t kIdleScanIntervalUs = 20000000;  // 待机摆头 20s 一次(统一常量)

    bool Begin();
    void MoveTo(int yaw_deg, int pitch_deg, int time_ms);
    void PauseScan();
    void ResumeScan();

    void Center() { MoveTo(0, 30, 600); }


    // 当前命令位置（最后一次 MoveTo 设的）— Nod/Shake 用这个作 base
    int GetCurrentYaw() const { return last_yaw_deg_; }
    int GetCurrentPitch() const { return last_pitch_deg_; }

    void Nod();
    void Shake();
    void Tilt();
    void TiltAsk();

    bool IsAnimating() const { return anim_running_; }

private:
    static void IdleScanCb(void* arg);

    SCSCL bus_;
    esp_timer_handle_t idle_timer_ = nullptr;
    bool scan_running_ = false;
    volatile bool anim_running_ = false;
    int last_yaw_deg_ = 0;
    int last_pitch_deg_ = 30;
    ServoAnimCtx anim_ctx_{};
};

class FaceTracker {
    static constexpr int DS_W = 40;
    static constexpr int DS_H = 30;
public:
    void Start(EspVideo* camera, StackChanServo* servo) {
        camera_ = camera;
        servo_ = servo;
        if (!camera_ || !servo_) return;

        memset(prev_frame_, 0, sizeof(prev_frame_));
        paused_ = true;
        xTaskCreatePinnedToCore(TaskFunc, "face_track", 4096, this, 1, &task_, 1);
        ESP_LOGI("FaceTrack", "Started (paused until conversation)");
    }

    void Pause(bool resume_scan = true) {
        if (!paused_) {
            paused_ = true;
            tracking_ = false;
            if (resume_scan) servo_->ResumeScan();
            ESP_LOGI("FaceTrack", "Paused (scan=%d)", resume_scan);
        }
    }

    void Resume() {
        // 当 manual_lock_ 被 LLM 工具锁住时，忽略状态机周期 Resume，
        // 否则 Application::Listening/Speaking 每次刷新都会自动 Resume，
        // 让用户"派蒙转 30 度"立刻被人脸追踪复位。
        if (manual_lock_) return;
        if (paused_) {
            paused_ = false;
            has_prev_ = false;
            servo_->PauseScan();
            ESP_LOGI("FaceTrack", "Resumed");
        }
    }

    // 手动锁：lock=true 后，外部 Resume() 无效（除非 lock=false 解锁再 Resume）。
    // LLM head.move/center 用这个保证位置不被自动 tracker 抢回去。
    void SetManualLock(bool locked) {
        manual_lock_ = locked;
        ESP_LOGI("FaceTrack", "Manual lock %s", locked ? "ENABLED" : "DISABLED");
    }

    bool IsManualLocked() const { return manual_lock_; }

    bool IsPaused() const { return paused_; }
    float GetYaw() const { return yaw_; }
    float GetPitch() const { return pitch_; }

private:
    static void TaskFunc(void* arg) {
        auto* self = static_cast<FaceTracker*>(arg);
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (self->servo_->IsAnimating()) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
            if (self->paused_ || !self->camera_->IsOk()) continue;
            self->Track();
        }
    }

    void Track() {
        uint8_t cur_frame[DS_W * DS_H];
        int sum_x = 0, sum_y = 0, count = 0;

        bool ok = camera_->PeekFrame([&](const uint8_t* data, size_t len, uint16_t w, uint16_t h) {
            if (w == 0 || h == 0) return;
            int sx = w / DS_W;
            int sy = h / DS_H;
            for (int dy = 0; dy < DS_H; dy++) {
                for (int dx = 0; dx < DS_W; dx++) {
                    int src_offset = (dy * sy * w + dx * sx) * 2;
                    if ((size_t)src_offset >= len) { cur_frame[dy * DS_W + dx] = 0; continue; }
                    cur_frame[dy * DS_W + dx] = data[src_offset];
                }
            }

            if (!has_prev_) return;

            for (int dy = 3; dy < DS_H - 1; dy++) {
                for (int dx = 1; dx < DS_W - 1; dx++) {
                    int idx = dy * DS_W + dx;
                    uint8_t brightness = cur_frame[idx];
                    if (brightness > 200) continue;
                    int diff = abs((int)cur_frame[idx] - (int)prev_frame_[idx]);
                    if (diff > 20) {
                        sum_x += dx;
                        sum_y += dy;
                        count++;
                    }
                }
            }
        });

        memcpy(prev_frame_, cur_frame, sizeof(prev_frame_));
        if (!has_prev_) { has_prev_ = true; return; }
        if (!ok) return;

        int total_pixels = (DS_W - 2) * (DS_H - 4);
        if (count < 3 || count > total_pixels / 3) {
            no_move_count_++;
            if (no_move_count_ > 6 && tracking_) {
                tracking_ = false;
            }
            return;
        }

        no_move_count_ = 0;
        if (!tracking_) {
            servo_->PauseScan();
            tracking_ = true;
        }

        float cx = (float)sum_x / count;
        float cy = (float)sum_y / count;
        float target_x = (cx - DS_W / 2.0f) / (DS_W / 2.0f);
        float target_y = (cy - DS_H / 2.0f) / (DS_H / 2.0f);

        smooth_x_ = smooth_x_ * 0.3f + target_x * 0.7f;
        smooth_y_ = smooth_y_ * 0.3f + target_y * 0.7f;

        if (fabsf(smooth_x_) < 0.03f) smooth_x_ = 0;
        if (fabsf(smooth_y_) < 0.03f) smooth_y_ = 0;

        yaw_ -= smooth_x_ * 6.0f;
        pitch_ -= smooth_y_ * 4.0f;
        if (yaw_ < -45) yaw_ = -45;
        if (yaw_ > 45) yaw_ = 45;
        if (pitch_ < 5) pitch_ = 5;
        if (pitch_ > 60) pitch_ = 60;

        servo_->MoveTo((int)yaw_, (int)pitch_, 150);
    }

    EspVideo* camera_ = nullptr;
    StackChanServo* servo_ = nullptr;
    TaskHandle_t task_ = nullptr;
    volatile bool paused_ = false;
    volatile bool manual_lock_ = false;  // LLM head 工具锁住时为 true
    bool tracking_ = false;
    bool has_prev_ = false;
    int no_move_count_ = 0;
    float yaw_ = 0.0f;
    float pitch_ = 30.0f;
    float smooth_x_ = 0.0f;
    float smooth_y_ = 0.0f;
    uint8_t prev_frame_[DS_W * DS_H];
};
