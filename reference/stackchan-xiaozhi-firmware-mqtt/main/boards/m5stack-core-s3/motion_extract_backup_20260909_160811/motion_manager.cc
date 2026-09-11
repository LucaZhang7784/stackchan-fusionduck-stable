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
