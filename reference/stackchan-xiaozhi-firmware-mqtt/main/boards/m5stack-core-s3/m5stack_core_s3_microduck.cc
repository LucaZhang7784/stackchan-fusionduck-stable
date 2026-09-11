#include "wifi_board.h"
#include "cores3_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "config.h"
#include "power_save_timer.h"
#include "i2c_device.h"
#include "mcp_server.h"
#include "system_info.h"

#include <esp_log.h>
#include <esp_wifi.h>
#include <atomic>
#include <array>
#include <driver/i2c_master.h>
#include <freertos/semphr.h>
#include <esp_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory>
#include "esp_video.h"
#include "i2c_bus.h"
#include "led_controller.h"
#include "motion_manager.h"
#include "confirm_manager.h"
#include "push_mqtt_config.h"
#include "push_mqtt_manager.h"
#include "power_manager.h"
#include "sensor_manager.h"
#include "robot_action_dispatcher.h"
#include "py32_led_controller.h"
#include "camera_service.h"
#include "touch_gesture_manager.h"
#include "push_message_controller.h"
#include "display_service.h"
#include "avatar_display.h"

#define TAG "M5StackCoreS3Board"

// 云链路主动推送 broker: 当前只走 EMQX 公共入口。
// 保留一个 URI，避免局域网/Tailscale/Funnel failover 反复切换造成在线状态抖动。

class CustomBacklight : public Backlight {
public:
    CustomBacklight(PowerManager* power_manager) : power_manager_(power_manager) {}

    void SetBrightnessImpl(uint8_t brightness) override {
        power_manager_->SetBacklightBrightness(target_brightness_);
        brightness_ = target_brightness_;
    }

private:
    PowerManager* power_manager_;
};

class Aw9523 : public I2cDevice {
public:
    // Exanpd IO Init
    Aw9523(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : I2cDevice(i2c_bus, addr) {
        WriteReg(0x02, 0b00000111);  // P0
        WriteReg(0x03, 0b10001111);  // P1
        WriteReg(0x04, 0b00011000);  // CONFIG_P0
        WriteReg(0x05, 0b00001100);  // CONFIG_P1
        WriteReg(0x11, 0b00010000);  // GCR P0 port is Push-Pull mode.
        WriteReg(0x12, 0b11111111);  // LEDMODE_P0
        WriteReg(0x13, 0b11111111);  // LEDMODE_P1
    }

    void ResetIli9342() {
        ESP_LOGI(TAG, "Reset IlI9342");
        WriteReg(0x03, 0b10000001);
        vTaskDelay(pdMS_TO_TICKS(20));
        WriteReg(0x03, 0b10000011);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
};

class M5StackCoreS3Board;

// 状态直驱 LED: 每次设备状态变化(connecting/listening/speaking/idle)由
// Application 调 led->OnStateChanged(), 直接写 PY32 灯环, 不依赖 avatar/emotion 路径。
class StateLed : public Led {
public:
    explicit StateLed(Py32LedController* ctrl) : ctrl_(ctrl) {}
    void OnStateChanged() override;
private:
    Py32LedController* ctrl_;
};

class M5StackCoreS3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    // Phase 7.1: I2C 共享总线互斥锁(触屏/音频/AXP/PY32 同总线), FreeRTOS Mutex 带优先级继承
    SemaphoreHandle_t i2c_bus_mutex_ = nullptr;
    PowerManager power_manager_;
    SensorManager sensor_manager_;
    TouchGestureManager touch_gesture_mgr_;
    Aw9523* aw9523_;
    LcdDisplay* display_;
    CameraService camera_service_;
    StackChanServo servo_;
    FaceTracker face_tracker_;
    PowerSaveTimer* power_save_timer_;
    bool servo_power_ready_ = false;
    // ---- PY32 持久 I2C 设备句柄（控 LED + 其他扩展）----
    Py32LedController py32_led_ctrl_;
    StateLed state_led_{&py32_led_ctrl_};

    bool LockI2cBus() {
        if (i2c_bus_mutex_ == nullptr) return true;  // 未初始化时不做互斥
        return xSemaphoreTake(i2c_bus_mutex_, pdMS_TO_TICKS(50)) == pdTRUE;
    }
    void UnlockI2cBus() {
        if (i2c_bus_mutex_ != nullptr) xSemaphoreGive(i2c_bus_mutex_);
    }

    static bool TrySharedI2cLock(void* context) {
        auto* self = static_cast<M5StackCoreS3Board*>(context);
        return self != nullptr && self->LockI2cBus();
    }

    static void UnlockSharedI2c(void* context) {
        auto* self = static_cast<M5StackCoreS3Board*>(context);
        if (self != nullptr) self->UnlockI2cBus();
    }

    static bool IsSensorMotionSuppressed(void*) {
        return Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking;
    }

    static void PostSensorEvent(void* context, SensorEvent event) {
        auto* self = static_cast<M5StackCoreS3Board*>(context);
        if (self == nullptr) return;
        Application::GetInstance().Schedule([self, event]() {
            self->HandleSensorEvent(event);
        });
    }

    static void PostLowBattery(void* context, int level) {
        auto* self = static_cast<M5StackCoreS3Board*>(context);
        if (self == nullptr) return;
        Application::GetInstance().Schedule([self, level]() {
            self->HandleLowBattery(level);
        });
    }

    void InitializePowerManager() {
        PowerManager::Config config{};
        config.bus = i2c_bus_;
        config.context = this;
        config.try_lock = TrySharedI2cLock;
        config.unlock = UnlockSharedI2c;
        config.post_low_battery = PostLowBattery;
        if (!power_manager_.Init(config)) ESP_LOGE(TAG, "power manager init failed");
    }

    void InitializeSensors() {
        SensorManager::Config config{};
        config.bus = i2c_bus_;
        config.context = this;
        config.try_lock = TrySharedI2cLock;
        config.unlock = UnlockSharedI2c;
        config.post_event = PostSensorEvent;
        config.is_motion_suppressed = IsSensorMotionSuppressed;
        if (!sensor_manager_.Init(config)) ESP_LOGW(TAG, "no optional sensor initialized");
    }

    void HandleLowBattery(int level) {
        if (action_dispatcher_) action_dispatcher_->HandleLowBattery(level);
    }

    void HandleSensorEvent(SensorEvent event) {
        if (action_dispatcher_) action_dispatcher_->HandleSensorEvent(event);
    }
    bool servo_ok_ = false;

    // ---- 云链路主动推送: 板级第二条 MQTT (独立 esp_mqtt_client, 订阅 stackchan/{mac}/push) ----
    PushMqttManager push_mqtt_mgr_;
    PushMessageController push_msg_ctrl_;

    static void SchedulePushTask(void*, std::function<void()> task) {
        Application::GetInstance().Schedule(std::move(task));
    }

    static void OnPushMqttDisconnected(void* arg) {
        auto* self = static_cast<M5StackCoreS3Board*>(arg);
        if (self != nullptr) self->push_msg_ctrl_.OnDisconnected();
    }

    // ---- Phase 9-B: 触屏审批浮层(LVGL) ----
    ConfirmManager confirm_mgr_;
    std::unique_ptr<RobotActionDispatcher> action_dispatcher_;

    // MQTT 推送帧: [0]=type(1=start,2=pcm帧,3=stop,4=snap), [1..]=payload。
    // 连接生命周期由 PushMqttManager 处理；板级只解析已完整到达的下行数据。
    static void OnConfirmTimeoutStatic(void* arg) {
        auto* self = static_cast<M5StackCoreS3Board*>(arg);
        // 15s 超时: 必须经主任务销毁, 严禁在定时器回调里直接碰 LVGL
        Application::GetInstance().Schedule([self]() {
            self->confirm_mgr_.Dismiss(self->display_);
        });
    }
    void InitializePushMqtt() {
        action_dispatcher_ = std::make_unique<RobotActionDispatcher>();
        RobotActionDispatcher::Dependencies action_deps;
        action_deps.servo = &servo_;
        action_deps.face_tracker = &face_tracker_;
        action_deps.display = display_;
        action_deps.confirm_mgr = &confirm_mgr_;
        action_deps.push_mqtt = &push_mqtt_mgr_;
        action_deps.schedule = [](std::function<void()> task) {
            Application::GetInstance().Schedule(std::move(task));
        };
        action_dispatcher_->Initialize(action_deps);
        PushMessageController::Dependencies push_deps;
        push_deps.push_mqtt = &push_mqtt_mgr_;
        push_deps.dispatcher = action_dispatcher_.get();
        push_deps.camera = &camera_service_;
        push_deps.audio_service = &Application::GetInstance().GetAudioService();
        push_deps.audio_codec = GetAudioCodec();
        push_deps.display = display_;
        push_msg_ctrl_.Initialize(push_deps);
        PushMqttManager::Config config = {};
        config.mac_address = SystemInfo::GetMacAddress();
        config.uris = kPushMqttUris;
        config.uri_count = kPushMqttUriCount;
        config.user_ctx = &push_msg_ctrl_;
        config.on_data = &PushMessageController::OnMqttData;
        config.on_disconnected = &M5StackCoreS3Board::OnPushMqttDisconnected;
        config.scheduler = &M5StackCoreS3Board::SchedulePushTask;
        if (!push_mqtt_mgr_.Configure(config)) {
            ESP_LOGE(TAG, "push MQTT manager init failed");
        }
    }
    void InitializePy32LedDevice() {
        Py32LedController::Config ctrl_cfg;
        ctrl_cfg.bus = i2c_bus_;
        ctrl_cfg.lock_ctx = this;
        ctrl_cfg.try_lock = [](void* ctx, uint32_t) {
            return static_cast<M5StackCoreS3Board*>(ctx)->LockI2cBus();
        };
        ctrl_cfg.unlock = [](void* ctx) {
            static_cast<M5StackCoreS3Board*>(ctx)->UnlockI2cBus();
        };
        if (!py32_led_ctrl_.Initialize(ctrl_cfg)) {
            ESP_LOGW(TAG, "PY32 LED controller init failed");
        }
    }

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(-1, -1, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(0);
            servo_.PauseScan();
            py32_led_ctrl_.TurnOff();
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
            servo_.ResumeScan();
            if (py32_led_ctrl_.IsReady()) {
                uint16_t neutral = LedController::Rgb888To565(60, 35, 10);
                uint16_t colors[12];
                for (int i = 0; i < 12; i++) colors[i] = neutral;
                py32_led_ctrl_.SetLedFrame(colors, 12);
            }
        });
        power_save_timer_->OnShutdownRequest([this]() {
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeAw9523() {
        ESP_LOGI(TAG, "Init AW9523");
        aw9523_ = new Aw9523(i2c_bus_, 0x58);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

public:
    M5StackCoreS3Board() : confirm_mgr_(&M5StackCoreS3Board::OnConfirmTimeoutStatic, this) {
        i2c_bus_mutex_ = xSemaphoreCreateMutex();
        if (i2c_bus_mutex_ == nullptr) {
            ESP_LOGE(TAG, "i2c_bus_mutex_ create failed");
        }
        InitializePowerSaveTimer();
        InitializeI2c();
        InitializePowerManager();
        InitializeAw9523();

        servo_power_ready_ = power_manager_.EnableServoRail();
        if (servo_power_ready_) {
            vTaskDelay(pdMS_TO_TICKS(200));
            servo_ok_ = servo_.Begin();
            InitializePy32LedDevice();
        }

        DisplayService::Config disp_cfg;
        disp_cfg.reset_panel = [this]() { if (aw9523_) aw9523_->ResetIli9342(); };
        display_ = DisplayService::InitializeDisplay(disp_cfg);
        CameraService::Config cam_cfg;
        cam_cfg.i2c_bus = i2c_bus_;
        cam_cfg.push_mqtt = &push_mqtt_mgr_;
        cam_cfg.face_tracker = &face_tracker_;
        camera_service_.Initialize(cam_cfg);
        auto* avatar_display = static_cast<M5StackAvatarDisplay*>(display_);
        if (servo_ok_) {
            avatar_display->SetServo(&servo_);
        }
        if (camera_service_.IsOk() && servo_ok_) {
            face_tracker_.Start(camera_service_.GetVideoDevice(), &servo_);
            avatar_display->SetFaceTracker(&face_tracker_);
        }
        avatar_display->SetLedUpdater([this](const char* emotion) {
            py32_led_ctrl_.UpdateEmotion(emotion ? emotion : "",
                                         Application::GetInstance().GetDeviceState());
        });
        TouchGestureManager::Dependencies touch_deps;
        touch_deps.i2c_bus = i2c_bus_;
        touch_deps.bus_lock_ctx = this;
        touch_deps.lock_bus = [](void* ctx) -> bool {
            return static_cast<M5StackCoreS3Board*>(ctx)->LockI2cBus();
        };
        touch_deps.unlock_bus = [](void* ctx) {
            static_cast<M5StackCoreS3Board*>(ctx)->UnlockI2cBus();
        };
        touch_deps.display = display_;
        touch_deps.servo = &servo_;
        touch_deps.confirm_mgr = &confirm_mgr_;
        touch_deps.push_mqtt = &push_mqtt_mgr_;
        touch_deps.wifi_ctx = this;
        touch_deps.enter_wifi_config = [](void* ctx) {
            static_cast<M5StackCoreS3Board*>(ctx)->EnterWifiConfigMode();
        };
        touch_deps.push_ctx = this;
        touch_deps.on_push_interrupt = [](void* ctx) {
            auto* self = static_cast<M5StackCoreS3Board*>(ctx);
            if (self != nullptr) self->push_msg_ctrl_.Interrupt();
        };
        touch_gesture_mgr_.Initialize(touch_deps);
        InitializeSensors();
        InitializePushMqtt();
        // LED 状态色周期刷新(500ms): 即使状态事件偶发丢失, 灯环也跟随状态
        esp_timer_create_args_t led_state_args = {};
        led_state_args.callback = [](void* arg) {
            static_cast<M5StackCoreS3Board*>(arg)->py32_led_ctrl_.UpdateDeviceState(Application::GetInstance().GetDeviceState());
        };
        led_state_args.arg = this;
        led_state_args.name = "led_state";
        esp_timer_handle_t led_state_timer = nullptr;
        esp_timer_create(&led_state_args, &led_state_timer);
        esp_timer_start_periodic(led_state_timer, 500000);

        GetBacklight()->RestoreBrightness();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static CoreS3AudioCodec audio_codec(i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_AW88298_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Camera* GetCamera() override {
        return camera_service_.GetVideoDevice();
    }

    virtual Led* GetLed() override {
        return &state_led_;
    }

    

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        return power_manager_.ReadBattery(level, charging, discharging);
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    virtual Backlight *GetBacklight() override {
        static CustomBacklight backlight(&power_manager_);
        return &backlight;
    }
};

void StateLed::OnStateChanged() {
    if (ctrl_) ctrl_->UpdateDeviceState(Application::GetInstance().GetDeviceState());
}

DECLARE_BOARD(M5StackCoreS3Board);
