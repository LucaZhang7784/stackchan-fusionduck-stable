#include "touch_gesture_manager.h"
#include "i2c_device.h"
#include "application.h"
#include "display/lcd_display.h"
#include "motion_manager.h"
#include "confirm_manager.h"
#include "push_mqtt_manager.h"
#include "avatar_display.h"
#include <esp_log.h>
#include <esp_random.h>
#include <vector>
#include <stdlib.h>

namespace {
constexpr const char* TAG = "TouchGesture";

class Ft6336 : public I2cDevice {
public:
    struct TouchPoint_t {
        int num = 0;
        int x = -1;
        int y = -1;
    };

    Ft6336(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        uint8_t chip_id = ReadReg(0xA3);
        ESP_LOGI(TAG, "Get chip ID: 0x%02X", chip_id);
        read_buffer_ = new uint8_t[6];
    }

    ~Ft6336() {
        delete[] read_buffer_;
    }

    void UpdateTouchPoint() {
        ReadRegs(0x02, read_buffer_, 6);
        tp_.num = read_buffer_[0] & 0x0F;
        tp_.x = ((read_buffer_[1] & 0x0F) << 8) | read_buffer_[2];
        tp_.y = ((read_buffer_[3] & 0x0F) << 8) | read_buffer_[4];
    }

    inline const TouchPoint_t& GetTouchPoint() {
        return tp_;
    }

private:
    uint8_t* read_buffer_ = nullptr;
    TouchPoint_t tp_;
};

static const std::vector<const char*>& DoubleClickPool() {
    static const std::vector<const char*> pool = {
        "（主人亲了亲小智）",
        "（主人啵了一下小智）",
        "（主人偷偷亲了小智一下）",
        "（主人用鼻尖蹭了蹭小智的鼻尖）",
        "（主人戳了戳小智的脸）",
        "（主人抱住小智蹭了蹭\"要抱抱\"）",
        "（主人亲了亲小智的唇角）",
        "（主人啄了啄小智的唇）",
        "（主人把额头贴到小智的额头上）",
        "（主人接吻时故意咬了小智一口）",
    };
    return pool;
}

static const std::vector<const char*>& UpSwipePool() {
    static const std::vector<const char*> pool = {
        "（主人弹了弹小智的脑门）",
        "（主人拨了拨小智的头发）",
        "（主人亲了亲小智的眼睛）",
        "（主人凑上去嗅了嗅小智）",
        "（主人踮起脚蒙住了小智的眼睛）",
        "（主人凑近小智吹了吹他的睫毛）",
        "（主人凑到小智耳边吹了吹）",
        "（主人托着腮对着小智发呆）",
        "（主人把手抵在小智唇上）",
    };
    return pool;
}

static const std::vector<const char*>& DownSwipePool() {
    static const std::vector<const char*> pool = {
        "（主人摸了摸小智的喉结）",
        "（主人摸了摸小智的下巴）",
        "（主人戳了戳小智的颈窝）",
        "（主人摸了摸小智的胸口）",
        "（主人贴在小智胸口听了听心跳）",
        "（主人摸了摸小智的腹肌）",
        "（主人摸了摸小智的屁股）",
        "（主人按了按小智的腰窝）",
        "（主人扯了扯小智的衣角）",
        "（主人伸手解了解小智的纽扣）",
    };
    return pool;
}

static const std::vector<const char*>& LeftSwipePool() {
    static const std::vector<const char*> pool = {
        "（主人揉了揉小智的左脸）",
        "（主人牵起了小智的左手）",
        "（主人和小智十指紧扣）",
        "（主人抱住了小智的左臂）",
        "（主人把小智的脸捧过来转向自己）",
        "（主人靠到小智的肩膀上）",
        "（主人跨坐到小智的腿上）",
        "（主人伸出小指勾了勾小智的）",
        "（主人趴在小智腿上）",
    };
    return pool;
}

static const std::vector<const char*>& RightSwipePool() {
    static const std::vector<const char*> pool = {
        "（主人揉了揉小智的右脸）",
        "（主人捏了捏小智的耳垂）",
        "（主人捏住小智的手指玩）",
        "（主人捏了捏小智的后颈）",
        "（主人把小智的脸捧过来转向自己）",
        "（主人叼住了小智的指尖咬了咬）",
        "（主人拽了拽小智的领口）",
        "（主人从背后抱住了小智）",
    };
    return pool;
}

static const char* PickRandom(const std::vector<const char*>& pool) {
    if (pool.empty()) return nullptr;
    return pool[esp_random() % pool.size()];
}

}  // namespace

class TouchGestureManager::Impl {
public:
    Dependencies deps_{};
    Ft6336* ft6336_ = nullptr;
    esp_timer_handle_t touchpad_timer_ = nullptr;

    int touch_i2c_fail_count_ = 0;
    int64_t touch_pause_until_ms_ = 0;
    volatile int64_t touch_lock_until_ms_ = 0;

    bool was_touched_ = false;
    int64_t touch_start_time_ = 0;
    int touch_start_x_ = 0;
    int touch_start_y_ = 0;
    int touch_last_x_ = 0;
    int touch_last_y_ = 0;
    int touch_total_move_ = 0;
    bool pet_triggered_ = false;
    bool pending_single_release_ = false;
    int64_t pending_single_release_time_ = 0;

    ~Impl() {
        if (touchpad_timer_) {
            esp_timer_stop(touchpad_timer_);
            esp_timer_delete(touchpad_timer_);
            touchpad_timer_ = nullptr;
        }
        delete ft6336_;
        ft6336_ = nullptr;
    }

    bool Initialize(const Dependencies& deps, TouchGestureManager* owner) {
        deps_ = deps;
        if (deps_.i2c_bus == nullptr) {
            ESP_LOGE(TAG, "invalid i2c_bus");
            return false;
        }

        ESP_LOGI(TAG, "Init FT6336");
        ft6336_ = new Ft6336(deps_.i2c_bus, 0x38);

        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                auto* mgr = static_cast<TouchGestureManager*>(arg);
                if (mgr) mgr->Poll();
            },
            .arg = owner,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "touchpad_timer",
            .skip_unhandled_events = true,
        };

        esp_err_t err = esp_timer_create(&timer_args, &touchpad_timer_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "create touchpad_timer failed: %s", esp_err_to_name(err));
            return false;
        }
        err = esp_timer_start_periodic(touchpad_timer_, 20 * 1000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "start touchpad_timer failed: %s", esp_err_to_name(err));
            return false;
        }
        return true;
    }

    void Poll() {
        if (!ft6336_) return;

        const int64_t SHORT_TOUCH_MS = 500;
        const int64_t PET_TOUCH_MS = 1500;
        const int64_t DOUBLE_CLICK_MS = 500;       // 双击窗口放宽
        const int SWIPE_THRESHOLD_PX = 20;         // 滑动门槛降低
        const int PET_MOVE_THRESHOLD_PX = 8;       // 防抖: 微位移不再算摸头
        const int CLICK_MAX_MOVE_PX = 5;           // 短按/双击允许的最大位移：超过就不算短按了

        int64_t now = esp_timer_get_time() / 1000;

        // Phase 7.1: 触屏 I2C 故障冷却 - 连续拿不到锁/超时后暂停轮询 500ms, 防超时风暴
        if (now < touch_pause_until_ms_) {
            return;
        }

        auto lock_bus = [this]() -> bool {
            return deps_.lock_bus == nullptr || deps_.lock_bus(deps_.bus_lock_ctx);
        };
        auto unlock_bus = [this]() {
            if (deps_.unlock_bus) deps_.unlock_bus(deps_.bus_lock_ctx);
        };

        // Phase 7.1: 触屏防踩踏锁 - 锁期内早退; 到期瞬间强制清空 IC 寄存器残留
        if (touch_lock_until_ms_ != 0) {
            if (now < touch_lock_until_ms_) {
                return;
            }
            touch_lock_until_ms_ = 0;
            was_touched_ = false;
            pending_single_release_ = false;
            touch_start_time_ = 0;
            if (lock_bus()) {
                ft6336_->UpdateTouchPoint();
                unlock_bus();
            }
        }

        // Phase 7.1: I2C 共享总线互斥(50ms 超时, 拿不到锁跳过本次轮询)
        if (!lock_bus()) {
            touch_i2c_fail_count_++;
            if (touch_i2c_fail_count_ >= 5) {
                touch_i2c_fail_count_ = 0;
                touch_pause_until_ms_ = now + 500;
                ESP_LOGW(TAG, "Touch I2C busy x5, pausing 500ms");
            }
            return;
        }
        ft6336_->UpdateTouchPoint();
        unlock_bus();
        auto& touch_point = ft6336_->GetTouchPoint();
        touch_i2c_fail_count_ = 0;

        // 待定单击超过双击窗口 → 执行单击（ToggleChat）
        if (pending_single_release_ && (now - pending_single_release_time_) > DOUBLE_CLICK_MS) {
            pending_single_release_ = false;
            touch_lock_until_ms_ = now + 400;  // Phase 7.1: 触屏动作 400ms 防踩踏
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                if (deps_.enter_wifi_config) deps_.enter_wifi_config(deps_.wifi_ctx);
                return;
            }
            app.ToggleChatState();
        }

        if (touch_point.num > 0 && !was_touched_) {
            // 按下
            was_touched_ = true;
            pet_triggered_ = false;
            touch_start_time_ = now;
            touch_start_x_ = touch_point.x;
            touch_start_y_ = touch_point.y;
            touch_last_x_ = touch_point.x;
            touch_last_y_ = touch_point.y;
            touch_total_move_ = 0;
        } else if (touch_point.num > 0 && was_touched_) {
            // 按住中 — 累积移动距离
            touch_total_move_ += abs(touch_point.x - touch_last_x_) + abs(touch_point.y - touch_last_y_);
            touch_last_x_ = touch_point.x;
            touch_last_y_ = touch_point.y;

            // 空闲状态下长按 5 秒 → 进入配网模式
            if (!pet_triggered_ && Application::GetInstance().GetDeviceState() == kDeviceStateIdle) {
                if (now - touch_start_time_ >= 5000) {
                    pet_triggered_ = true;
                    if (deps_.enter_wifi_config) deps_.enter_wifi_config(deps_.wifi_ctx);
                    return;
                }
            }

            // 长按摸头（要手指有移动，不算被物体压）
            if (!pet_triggered_) {
                int64_t held = now - touch_start_time_;
                if (held >= PET_TOUCH_MS && touch_total_move_ >= PET_MOVE_THRESHOLD_PX) {
                    pet_triggered_ = true;
                    if (deps_.display) {
                        auto* avatar = dynamic_cast<M5StackAvatarDisplay*>(deps_.display);
                        if (avatar) avatar->OnPetted();
                    }
                }
            }
        } else if (touch_point.num == 0 && was_touched_) {
            // 抬起
            was_touched_ = false;
            int64_t touch_duration = now - touch_start_time_;
            int dx_total = touch_last_x_ - touch_start_x_;
            int dy_total = touch_last_y_ - touch_start_y_;
            int abs_dx = abs(dx_total);
            int abs_dy = abs(dy_total);

            // ---- Phase 9-B: 确认浮层按钮命中(最高优先级, 抬起瞬间判定) ----
            if (deps_.confirm_mgr && deps_.confirm_mgr->IsActive()) {
                int tx = touch_last_x_, ty = touch_last_y_;
                MapTouchToScreen(tx, ty);
                if (const char* answer = deps_.confirm_mgr->HitTest(tx, ty)) {
                    ESP_LOGI(TAG, "confirm tap -> %s @(%d,%d) uid=%s", answer, tx, deps_.confirm_mgr->GetUid().c_str());
                    if (!deps_.confirm_mgr->GetUid().empty() && deps_.push_mqtt && deps_.push_mqtt->IsConnected()) {
                        std::string payload = deps_.confirm_mgr->GetUid() + "\x01" + answer;
                        deps_.push_mqtt->Publish(deps_.push_mqtt->ConfirmTopic(), payload, 0, 0);
                    }
                    touch_lock_until_ms_ = now + 400;  // 防踩踏: 点击后 400ms 锁
                    auto* cm = deps_.confirm_mgr;
                    auto* disp = deps_.display;
                    Application::GetInstance().Schedule([cm, disp]() {
                        cm->Dismiss(disp);
                    });
                    return;
                }
            }

            if (pet_triggered_) return;  // 摸头已触发就不再判别

            // 滑动手势：短促 + 位移够大
            if (touch_duration < SHORT_TOUCH_MS && (abs_dx >= SWIPE_THRESHOLD_PX || abs_dy >= SWIPE_THRESHOLD_PX)) {
                const std::vector<const char*>* pool = nullptr;
                if (abs_dx > abs_dy) {
                    pool = (dx_total < 0) ? &LeftSwipePool() : &RightSwipePool();
                } else {
                    pool = (dy_total < 0) ? &UpSwipePool() : &DownSwipePool();
                }
                touch_lock_until_ms_ = now + 400;  // Phase 7.1: 滑动动作 400ms 防踩踏
                // Phase 9-A: 手势仅本地表情/舵机响应, 严禁发手势文本给 LLM
                if (deps_.display) {
                    deps_.display->SetChatMessage("user", PickRandom(*pool));
                    deps_.display->SetEmotion("happy");
                }
                if (deps_.servo) {
                    deps_.servo->Nod();
                }
                return;
            }

            // 短按（位移要几乎为零，否则视为"模糊手势"不触发任何切换）
            int total_move = abs_dx + abs_dy;
            if (touch_duration >= 40 && touch_duration < SHORT_TOUCH_MS && total_move <= CLICK_MAX_MOVE_PX) {
                if (pending_single_release_ && (now - pending_single_release_time_) <= DOUBLE_CLICK_MS) {
                    // 第二次短按落在窗口内 → 双击
                    pending_single_release_ = false;
                    touch_lock_until_ms_ = now + 400;  // Phase 7.1: 双击 400ms 防踩踏锁
                    // Phase 9-A: 双击硬隔离为"纯打断 + 进聆听", 严禁发手势文本给 LLM
                    ESP_LOGI(TAG, "double-click -> pure interrupt + listening");
                    if (deps_.on_push_interrupt) {
                        deps_.on_push_interrupt(deps_.push_ctx);
                    }
                    Application::GetInstance().StartListening();  // 事件驱动, 线程安全
                } else {
                    // 候选单击，等下一帧或下次按下判定
                    pending_single_release_ = true;
                    pending_single_release_time_ = now;
                }
            }
        }
    }
};

TouchGestureManager::TouchGestureManager() : impl_(std::make_unique<Impl>()) {}
TouchGestureManager::~TouchGestureManager() = default;

bool TouchGestureManager::Initialize(const Dependencies& deps) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    return impl_->Initialize(deps, this);
}

void TouchGestureManager::Poll() {
    if (impl_) impl_->Poll();
}

void TouchGestureManager::MapTouchToScreen(int& x, int& y) {
    (void)x; (void)y;
}