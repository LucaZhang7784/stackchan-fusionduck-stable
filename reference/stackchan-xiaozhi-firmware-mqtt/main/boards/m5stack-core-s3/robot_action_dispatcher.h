#pragma once

#include <functional>
#include <string>
#include <esp_timer.h>
#include "sensor_manager.h"

class StackChanServo;
class FaceTracker;
class LcdDisplay;
class ConfirmManager;
class PushMqttManager;

enum class BehaviorState { kIdle, kCelebrate, kAttention, kBusy, kSleep };

class RobotActionDispatcher {
public:
    struct Dependencies {
        StackChanServo* servo = nullptr;
        FaceTracker* face_tracker = nullptr;
        LcdDisplay* display = nullptr;
        ConfirmManager* confirm_mgr = nullptr;
        PushMqttManager* push_mqtt = nullptr;
        std::function<void(const uint16_t*, size_t)> set_led_frame;
        std::function<void(std::function<void()>)> schedule;
    };

    RobotActionDispatcher() = default;
    ~RobotActionDispatcher();
    bool Initialize(const Dependencies& deps);
    void DispatchPushAction(const std::string& action, const std::string& text,
                            const std::string& msg_uid);
    void SetBehaviorState(BehaviorState state);
    void HandleSensorEvent(SensorEvent event);
    void HandleLowBattery(int level);

private:
    static void BehaviorTimerCb(void* arg);
    void RegisterFaceMcpTools();
    void RegisterLedMcpTools();
    void RegisterServoMcpTools();
    Dependencies deps_;
    esp_timer_handle_t behavior_timer_ = nullptr;
    BehaviorState state_ = BehaviorState::kIdle;
};
