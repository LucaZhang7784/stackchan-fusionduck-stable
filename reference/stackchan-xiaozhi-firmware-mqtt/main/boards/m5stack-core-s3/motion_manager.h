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
public:
    static constexpr int DS_W = 40;
    static constexpr int DS_H = 30;
    void Start(EspVideo* camera, StackChanServo* servo);
    void Pause(bool resume_scan = true);
    void Resume();
    void SetManualLock(bool locked);
    bool IsManualLocked() const { return manual_lock_; }
    bool IsPaused() const { return paused_; }
    float GetYaw() const { return yaw_; }
    float GetPitch() const { return pitch_; }
private:
    static void TaskFunc(void* arg);
    void Track();
    EspVideo* camera_ = nullptr;
    StackChanServo* servo_ = nullptr;
    TaskHandle_t task_ = nullptr;
    volatile bool paused_ = false;
    volatile bool manual_lock_ = false;
    bool tracking_ = false;
    bool has_prev_ = false;
    int no_move_count_ = 0;
    float yaw_ = 0.0f;
    float pitch_ = 30.0f;
    float smooth_x_ = 0.0f;
    float smooth_y_ = 0.0f;
    uint8_t prev_frame_[DS_W * DS_H];
};
