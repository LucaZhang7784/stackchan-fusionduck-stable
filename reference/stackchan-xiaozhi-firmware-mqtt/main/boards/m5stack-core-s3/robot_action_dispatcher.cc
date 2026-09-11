#include "robot_action_dispatcher.h"
#include "motion_manager.h"
#include "confirm_manager.h"
#include "mcp_server.h"
#include "led_controller.h"
#include "display/lcd_display.h"
#include <esp_log.h>

namespace {
constexpr const char* TAG = "ActionDispatch";
}

RobotActionDispatcher::~RobotActionDispatcher() {
    if (behavior_timer_) {
        esp_timer_stop(behavior_timer_);
        esp_timer_delete(behavior_timer_);
        behavior_timer_ = nullptr;
    }
}

bool RobotActionDispatcher::Initialize(const Dependencies& deps) {
    deps_ = deps;
    if (behavior_timer_) return true;
    esp_timer_create_args_t args{};
    args.callback = &RobotActionDispatcher::BehaviorTimerCb;
    args.arg = this;
    args.name = "behavior_idle";
    esp_err_t err = esp_timer_create(&args, &behavior_timer_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "behavior timer create failed: %s", esp_err_to_name(err));
        return false;
    }
    RegisterFaceMcpTools();
    RegisterLedMcpTools();
    RegisterServoMcpTools();
    return true;
}

void RobotActionDispatcher::DispatchPushAction(const std::string& action,
                                                const std::string& text,
                                                const std::string& msg_uid) {
    if (action == "question") {
        SetBehaviorState(BehaviorState::kAttention);
        if (deps_.schedule && deps_.confirm_mgr && deps_.display) {
            auto* cm = deps_.confirm_mgr;
            auto* display = deps_.display;
            deps_.schedule([cm, display, text, msg_uid]() {
                cm->Show(display, text, msg_uid);
            });
        }
    } else if (action == "progress") {
        SetBehaviorState(BehaviorState::kBusy);
    } else if (action == "done" || action == "error") {
        SetBehaviorState(BehaviorState::kCelebrate);
    }
}

void RobotActionDispatcher::SetBehaviorState(BehaviorState state) {
    state_ = state;
    if (behavior_timer_) esp_timer_stop(behavior_timer_);
    if (deps_.display && deps_.schedule) {
        const char* emotion = "neutral";
        if (state == BehaviorState::kCelebrate) emotion = "happy";
        else if (state == BehaviorState::kAttention) emotion = "thinking";
        else if (state == BehaviorState::kBusy) emotion = "loading";
        else if (state == BehaviorState::kSleep) emotion = "sleepy";
        auto* display = deps_.display;
        deps_.schedule([display, emotion]() { display->SetEmotion(emotion); });
    }
    if (deps_.servo) {
        if (state == BehaviorState::kCelebrate) deps_.servo->Nod();
        else if (state == BehaviorState::kAttention) deps_.servo->TiltAsk();
    }
    if (behavior_timer_ && state != BehaviorState::kSleep) {
        esp_timer_start_once(behavior_timer_, 8ULL * 1000ULL * 1000ULL);
    }
}

void RobotActionDispatcher::BehaviorTimerCb(void* arg) {
    auto* self = static_cast<RobotActionDispatcher*>(arg);
    if (self) self->SetBehaviorState(BehaviorState::kIdle);
}

void RobotActionDispatcher::HandleSensorEvent(SensorEvent event) {
    if (!deps_.servo || !deps_.display || !deps_.schedule) return;
    if (event == SensorEvent::kHeadTouch) {
        deps_.schedule([d = deps_.display]() { d->SetEmotion("loving"); });
    } else if (event == SensorEvent::kShake) {
        deps_.schedule([d = deps_.display]() { d->SetEmotion("shocked"); });
        deps_.servo->Shake();
    } else if (event == SensorEvent::kLift) {
        deps_.schedule([d = deps_.display]() { d->SetEmotion("surprised"); });
    }
}

void RobotActionDispatcher::HandleLowBattery(int level) {
    if (deps_.display && deps_.schedule) {
        auto* d = deps_.display;
        deps_.schedule([d]() { d->SetEmotion("sad"); });
    }
    ESP_LOGW(TAG, "low battery level=%d", level);
}

void RobotActionDispatcher::RegisterFaceMcpTools() {
    McpServer::GetInstance().AddTool("self.face.expression", "Set facial expression.",
        PropertyList({Property("emotion", kPropertyTypeString)}),
        [this](const PropertyList& p) -> ReturnValue {
            const auto e = p["emotion"].value<std::string>();
            if (deps_.display) deps_.display->SetEmotion(e.c_str());
            return true;
        });
}

void RobotActionDispatcher::RegisterLedMcpTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool("self.led.set_color", "Set LED RGB color.",
        PropertyList({Property("r", kPropertyTypeInteger, 0, 255), Property("g", kPropertyTypeInteger, 0, 255), Property("b", kPropertyTypeInteger, 0, 255)}),
        [this](const PropertyList& p) -> ReturnValue {
            uint16_t c = LedController::Rgb888To565(p["r"].value<int>(), p["g"].value<int>(), p["b"].value<int>());
            uint16_t frame[12]; for (auto& v : frame) v = c;
            if (deps_.set_led_frame) deps_.set_led_frame(frame, 12);
            return true;
        });
    mcp.AddTool("self.led.turn_off", "Turn LED off.", PropertyList(),
        [this](const PropertyList&) -> ReturnValue { uint16_t frame[12]{}; if (deps_.set_led_frame) deps_.set_led_frame(frame, 12); return true; });
    mcp.AddTool("self.led.auto", "Return LED to automatic mode.", PropertyList(),
        [](const PropertyList&) -> ReturnValue { return true; });
}

void RobotActionDispatcher::RegisterServoMcpTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool("self.head.move", "Move head to yaw/pitch.",
        PropertyList({Property("yaw", kPropertyTypeInteger, -45, 45), Property("pitch", kPropertyTypeInteger, 5, 60)}),
        [this](const PropertyList& p) -> ReturnValue {
            if (deps_.face_tracker) { deps_.face_tracker->SetManualLock(true); deps_.face_tracker->Pause(false); }
            if (deps_.servo) { deps_.servo->PauseScan(); deps_.servo->MoveTo(p["yaw"].value<int>(), p["pitch"].value<int>(), 800); }
            return true;
        });
    mcp.AddTool("self.head.center", "Center head and resume tracking.", PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            if (deps_.servo) { deps_.servo->PauseScan(); deps_.servo->Center(); }
            if (deps_.face_tracker) { deps_.face_tracker->SetManualLock(false); deps_.face_tracker->Resume(); }
            if (deps_.servo) deps_.servo->ResumeScan();
            return true;
        });
    mcp.AddTool("self.head.nod", "Nod head.", PropertyList(), [this](const PropertyList&) -> ReturnValue { if (deps_.servo) deps_.servo->Nod(); return true; });
    mcp.AddTool("self.head.shake", "Shake head.", PropertyList(), [this](const PropertyList&) -> ReturnValue { if (deps_.servo) deps_.servo->Shake(); return true; });
}
