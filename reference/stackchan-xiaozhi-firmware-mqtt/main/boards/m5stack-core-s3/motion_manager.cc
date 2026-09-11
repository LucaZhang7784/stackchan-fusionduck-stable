#include "motion_manager.h"

bool StackChanServo::Begin() {
    if (!bus_.begin(UART_NUM_1, 1000000, 6, 7)) { ESP_LOGE("Servo", "SCS bus begin failed"); return false; }
    ESP_LOGI("Servo", "SCS bus init OK on UART1 GPIO6/7 @1Mbps");
    MoveTo(0, 30, 1500);
    esp_timer_create_args_t args = {};
    args.callback = &StackChanServo::IdleScanCb; args.arg = this;
    args.dispatch_method = ESP_TIMER_TASK; args.name = "servo_idle"; args.skip_unhandled_events = true;
    esp_timer_create(&args, &idle_timer_); esp_timer_start_periodic(idle_timer_, kIdleScanIntervalUs);
    scan_running_ = true; return true;
}

void StackChanServo::MoveTo(int yaw_deg, int pitch_deg, int time_ms) {
    if (yaw_deg < -45) {
        yaw_deg = -45;
    } else if (yaw_deg > 45) {
        yaw_deg = 45;
    }
    if (pitch_deg < 5) {
        pitch_deg = 5;
    } else if (pitch_deg > 60) {
        pitch_deg = 60;
    }
    int yaw_pos = 460 + yaw_deg * 16 / 5, pitch_pos = 620 + pitch_deg * 16 / 5;
    ESP_LOGI("Servo", "MoveTo yaw=%d pitch=%d t=%d (caller=%p)", yaw_deg, pitch_deg, time_ms, __builtin_return_address(0));
    bus_.WritePos(1, yaw_pos, time_ms, 0); bus_.WritePos(2, pitch_pos, time_ms, 0);
    last_yaw_deg_ = yaw_deg; last_pitch_deg_ = pitch_deg;
}

void StackChanServo::PauseScan() { if (scan_running_ && idle_timer_) { esp_timer_stop(idle_timer_); scan_running_ = false; } }
void StackChanServo::ResumeScan() { if (!scan_running_ && idle_timer_) { esp_timer_start_periodic(idle_timer_, kIdleScanIntervalUs); scan_running_ = true; } }
void StackChanServo::IdleScanCb(void* arg) { auto* self = static_cast<StackChanServo*>(arg); self->MoveTo((rand()%51)-25, 25+(rand()%11), 1500); }

void StackChanServo::Nod() {
    if (anim_running_) return;
    // 用 servo 当前命令位置作 base —— 不用 tracker_->GetYaw()，因为后者只反映
    // 人脸追踪的目标值，不反映 LLM head.move 工具设的硬位置。这样 Nod 在
    // 当前位置上下点头，不会把头甩回 (0, 30)。
    anim_ctx_ = {this, GetCurrentYaw(), GetCurrentPitch()};
    anim_running_ = true;
    xTaskCreatePinnedToCore([](void* arg) {
        auto* c = static_cast<ServoAnimCtx*>(arg);
        auto* s = c->servo;
        int y = c->base_yaw, p = c->base_pitch;
        s->MoveTo(y, p - 10, 200);
        vTaskDelay(pdMS_TO_TICKS(250));
        s->MoveTo(y, p + 5, 200);
        vTaskDelay(pdMS_TO_TICKS(250));
        s->MoveTo(y, p - 8, 200);
        vTaskDelay(pdMS_TO_TICKS(250));
        s->MoveTo(y, p, 300);
        vTaskDelay(pdMS_TO_TICKS(300));
        s->anim_running_ = false;
        vTaskDelete(nullptr);
    }, "nod", 2048, &anim_ctx_, 2, nullptr, 1);
}

void StackChanServo::Shake() {
    if (anim_running_) return;
    anim_ctx_ = {this, GetCurrentYaw(), GetCurrentPitch()};
    anim_running_ = true;
    xTaskCreatePinnedToCore([](void* arg) {
        auto* c = static_cast<ServoAnimCtx*>(arg);
        auto* s = c->servo;
        int y = c->base_yaw, p = c->base_pitch;
        s->MoveTo(y - 15, p, 200);
        vTaskDelay(pdMS_TO_TICKS(250));
        s->MoveTo(y + 15, p, 200);
        vTaskDelay(pdMS_TO_TICKS(250));
        s->MoveTo(y - 10, p, 200);
        vTaskDelay(pdMS_TO_TICKS(250));
        s->MoveTo(y, p, 300);
        vTaskDelay(pdMS_TO_TICKS(300));
        s->anim_running_ = false;
        vTaskDelete(nullptr);
    }, "shake", 2048, &anim_ctx_, 2, nullptr, 1);
}

void StackChanServo::Tilt() {
    if (anim_running_) return;
    anim_ctx_ = {this, GetCurrentYaw(), GetCurrentPitch()};
    anim_running_ = true;
    xTaskCreatePinnedToCore([](void* arg) {
        auto* c = static_cast<ServoAnimCtx*>(arg);
        auto* s = c->servo;
        int y = c->base_yaw, p = c->base_pitch;
        s->MoveTo(y + 10, p - 10, 400);
        vTaskDelay(pdMS_TO_TICKS(1500));
        s->MoveTo(y, p, 500);
        vTaskDelay(pdMS_TO_TICKS(500));
        s->anim_running_ = false;
        vTaskDelete(nullptr);
    }, "tilt", 2048, &anim_ctx_, 2, nullptr, 1);
}

void StackChanServo::TiltAsk() {
    // Phase 8.1: question 广播 -> 头偏转 15°(保持 1.2s 再回正), 表示"需要确认"
    if (anim_running_) return;
    anim_ctx_ = {this, GetCurrentYaw(), GetCurrentPitch()};
    anim_running_ = true;
    xTaskCreatePinnedToCore([](void* arg) {
        auto* c = static_cast<ServoAnimCtx*>(arg);
        auto* s = c->servo;
        int y = c->base_yaw, p = c->base_pitch;
        s->MoveTo(y + 15, p, 400);
        vTaskDelay(pdMS_TO_TICKS(1200));
        s->MoveTo(y, p, 500);
        vTaskDelay(pdMS_TO_TICKS(500));
        s->anim_running_ = false;
        vTaskDelete(nullptr);
    }, "tiltask", 2048, &anim_ctx_, 2, nullptr, 1);
}

void FaceTracker::Start(EspVideo* camera, StackChanServo* servo) {
    camera_ = camera;
    servo_ = servo;
    if (!camera_ || !servo_) return;
    memset(prev_frame_, 0, sizeof(prev_frame_));
    paused_ = true;
    xTaskCreatePinnedToCore(TaskFunc, "face_track", 4096, this, 1, &task_, 1);
    ESP_LOGI("FaceTrack", "Started (paused until conversation)");
}

void FaceTracker::Pause(bool resume_scan) {
    if (!paused_) {
        paused_ = true;
        tracking_ = false;
        if (resume_scan && servo_) servo_->ResumeScan();
        ESP_LOGI("FaceTrack", "Paused (scan=%d)", resume_scan);
    }
}

void FaceTracker::Resume() {
    if (manual_lock_ || !servo_) return;
    if (paused_) {
        paused_ = false;
        has_prev_ = false;
        servo_->PauseScan();
        ESP_LOGI("FaceTrack", "Resumed");
    }
}

void FaceTracker::SetManualLock(bool locked) {
    manual_lock_ = locked;
    ESP_LOGI("FaceTrack", "Manual lock %s", locked ? "ENABLED" : "DISABLED");
}

void FaceTracker::TaskFunc(void* arg) {
    auto* self = static_cast<FaceTracker*>(arg);
    if (!self || !self->camera_ || !self->servo_) {
        ESP_LOGE("FaceTrack", "Invalid tracker context, task exiting");
        vTaskDelete(nullptr);
        return;
    }
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (self->servo_->IsAnimating()) continue;
        if (self->paused_ || !self->camera_->IsOk()) continue;
        self->Track();
    }
}

void FaceTracker::Track() {
    uint8_t cur_frame[DS_W * DS_H] = {};
    int sum_x = 0, sum_y = 0, count = 0;
    bool ok = camera_->PeekFrame([&](const uint8_t* data, size_t len, uint16_t w, uint16_t h) {
        if (w == 0 || h == 0) return;
        int sx = w / DS_W, sy = h / DS_H;
        for (int dy = 0; dy < DS_H; dy++) {
            for (int dx = 0; dx < DS_W; dx++) {
                int src_offset = (dy * sy * w + dx * sx) * 2;
                if (src_offset < 0 || static_cast<size_t>(src_offset) >= len) continue;
                cur_frame[dy * DS_W + dx] = data[src_offset];
            }
        }
        if (!has_prev_) return;
        for (int dy = 3; dy < DS_H - 1; dy++) {
            for (int dx = 1; dx < DS_W - 1; dx++) {
                int idx = dy * DS_W + dx;
                if (cur_frame[idx] > 200) continue;
                int diff = abs(static_cast<int>(cur_frame[idx]) - static_cast<int>(prev_frame_[idx]));
                if (diff > 20) { sum_x += dx; sum_y += dy; count++; }
            }
        }
    });
    memcpy(prev_frame_, cur_frame, sizeof(prev_frame_));
    if (!has_prev_) { has_prev_ = true; return; }
    if (!ok) return;
    int total_pixels = (DS_W - 2) * (DS_H - 4);
    if (count < 3 || count > total_pixels / 3) {
        no_move_count_++;
        if (no_move_count_ > 6) tracking_ = false;
        return;
    }
    no_move_count_ = 0;
    if (!tracking_) { servo_->PauseScan(); tracking_ = true; }
    float cx = static_cast<float>(sum_x) / count;
    float cy = static_cast<float>(sum_y) / count;
    float target_x = (cx - DS_W / 2.0f) / (DS_W / 2.0f);
    float target_y = (cy - DS_H / 2.0f) / (DS_H / 2.0f);
    smooth_x_ = smooth_x_ * 0.3f + target_x * 0.7f;
    smooth_y_ = smooth_y_ * 0.3f + target_y * 0.7f;
    if (fabsf(smooth_x_) < 0.03f) smooth_x_ = 0;
    if (fabsf(smooth_y_) < 0.03f) smooth_y_ = 0;
    yaw_ -= smooth_x_ * 6.0f;
    pitch_ -= smooth_y_ * 4.0f;
    if (yaw_ < -45) yaw_ = -45;
    else if (yaw_ > 45) yaw_ = 45;
    if (pitch_ < 5) pitch_ = 5;
    else if (pitch_ > 60) pitch_ = 60;
    servo_->MoveTo(static_cast<int>(yaw_), static_cast<int>(pitch_), 150);
}
