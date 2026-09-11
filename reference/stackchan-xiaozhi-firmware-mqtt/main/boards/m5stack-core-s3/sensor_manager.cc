#include "sensor_manager.h"

#include <algorithm>
#include <math.h>
#include <string.h>

#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>

#include "bmi270_api.h"

namespace {
constexpr const char* kTag = "SensorManager";
constexpr uint8_t kBmiAddress = 0x69;
constexpr uint8_t kSi12tAddress = 0x68;
}  // namespace

extern "C" {
int8_t bmi270_init(struct bmi2_dev* dev);
extern const uint8_t bmi270_config_file[];
}

SensorManager::~SensorManager() {
    Deinit();
}

bool SensorManager::TryLockBus() const {
    return config_.try_lock != nullptr && config_.try_lock(config_.context);
}

void SensorManager::UnlockBus() const {
    if (config_.unlock != nullptr) config_.unlock(config_.context);
}

bool SensorManager::Init(const Config& config) {
    Deinit();
    if (config.bus == nullptr || config.try_lock == nullptr || config.unlock == nullptr ||
        config.post_event == nullptr || config.is_motion_suppressed == nullptr) {
        ESP_LOGE(kTag, "invalid sensor manager config");
        return false;
    }
    config_ = config;
    running_.store(true);
    const bool bmi_ok = StartBmi270();
    const bool si12t_ok = StartSi12t();
    return bmi_ok || si12t_ok;
}

bool SensorManager::StartBmi270() {
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = kBmiAddress,
        .scl_speed_hz = 400000,
    };
    if (!TryLockBus()) return false;
    const esp_err_t add_result = i2c_master_bus_add_device(config_.bus, &device_config, &bmi_device_);
    UnlockBus();
    if (add_result != ESP_OK) {
        ESP_LOGW(kTag, "BMI270 add device failed: %s", esp_err_to_name(add_result));
        return false;
    }

    bmi_dev_ = {};
    bmi_dev_.intf = BMI2_I2C_INTF;
    bmi_dev_.intf_ptr = this;
    bmi_dev_.read = BmiRead;
    bmi_dev_.write = BmiWrite;
    bmi_dev_.delay_us = BmiDelayUs;
    bmi_dev_.read_write_len = 256;
    bmi_dev_.config_file_ptr = bmi270_config_file;
    const int8_t init_result = bmi270_init(&bmi_dev_);
    if (init_result != BMI2_OK) {
        ESP_LOGW(kTag, "BMI270 init failed: %d", init_result);
        if (TryLockBus()) {
            i2c_master_bus_rm_device(bmi_device_);
            UnlockBus();
        }
        bmi_device_ = nullptr;
        return false;
    }
    const uint8_t sensors[] = {BMI2_ACCEL};
    const int8_t enable_result = bmi270_sensor_enable(sensors, 1, &bmi_dev_);
    if (enable_result != BMI2_OK) {
        ESP_LOGW(kTag, "BMI270 accel enable failed: %d", enable_result);
        if (TryLockBus()) {
            i2c_master_bus_rm_device(bmi_device_);
            UnlockBus();
        }
        bmi_device_ = nullptr;
        return false;
    }
    motion_exited_.store(false);
    if (xTaskCreatePinnedToCore(MotionTask, "motion", 4096, this, 1, &motion_task_, 1) != pdPASS) {
        motion_exited_.store(true);
        if (TryLockBus()) {
            i2c_master_bus_rm_device(bmi_device_);
            UnlockBus();
        }
        bmi_device_ = nullptr;
        ESP_LOGE(kTag, "motion task creation failed");
        return false;
    }
    bmi_ready_ = true;
    ESP_LOGI(kTag, "BMI270 ready at 0x%02X", kBmiAddress);
    return true;
}

bool SensorManager::StartSi12t() {
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = kSi12tAddress,
        .scl_speed_hz = 100000,
    };
    if (!TryLockBus()) return false;
    const esp_err_t add_result = i2c_master_bus_add_device(config_.bus, &device_config, &si12t_device_);
    UnlockBus();
    if (add_result != ESP_OK) {
        ESP_LOGW(kTag, "SI12T add device failed: %s", esp_err_to_name(add_result));
        return false;
    }

    const uint8_t setup[][2] = {
        {0x0A, 0x00}, {0x0C, 0x00}, {0x0E, 0x00}, {0x0B, 0x00},
        {0x0D, 0x00}, {0x0F, 0x00}, {0x09, 0x0F},
    };
    for (const auto& write : setup) Si12tWrite(write[0], write[1]);
    vTaskDelay(pdMS_TO_TICKS(10));
    Si12tWrite(0x09, 0x07);
    Si12tWrite(0x08, 0x22);
    for (uint8_t reg = 0x02; reg <= 0x07; ++reg) Si12tWrite(reg, 0xCC);

    si12t_exited_.store(false);
    if (xTaskCreatePinnedToCore(Si12tTask, "si12t", 3072, this, 1, &si12t_task_, 1) != pdPASS) {
        si12t_exited_.store(true);
        if (TryLockBus()) {
            i2c_master_bus_rm_device(si12t_device_);
            UnlockBus();
        }
        si12t_device_ = nullptr;
        ESP_LOGE(kTag, "SI12T task creation failed");
        return false;
    }
    si12t_ready_ = true;
    ESP_LOGI(kTag, "SI12T ready at 0x%02X", kSi12tAddress);
    return true;
}

void SensorManager::Deinit() {
    if (!running_.exchange(false)) return;
    if (motion_task_ != nullptr) xTaskNotifyGive(motion_task_);
    if (si12t_task_ != nullptr) xTaskNotifyGive(si12t_task_);
    const bool motion_stopped = WaitForExit(motion_exited_);
    const bool si12t_stopped = WaitForExit(si12t_exited_);
    if (!motion_stopped || !si12t_stopped) {
        // Never detach an I2C device while its task may still be inside a transfer.
        // The owning board has static lifetime; retain the handles for diagnosis.
        ESP_LOGE(kTag, "sensor shutdown incomplete; retaining I2C handles");
        return;
    }
    if (bmi_device_ != nullptr && TryLockBus()) {
        i2c_master_bus_rm_device(bmi_device_);
        UnlockBus();
    }
    if (si12t_device_ != nullptr && TryLockBus()) {
        i2c_master_bus_rm_device(si12t_device_);
        UnlockBus();
    }
    bmi_device_ = nullptr;
    si12t_device_ = nullptr;
    motion_task_ = nullptr;
    si12t_task_ = nullptr;
    bmi_ready_ = false;
    si12t_ready_ = false;
    config_ = {};
}

bool SensorManager::WaitForExit(std::atomic<bool>& exited) {
    for (int attempt = 0; attempt < 25 && !exited.load(); ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!exited.load()) {
        ESP_LOGW(kTag, "sensor task did not exit within 250ms");
        return false;
    }
    return true;
}

int8_t SensorManager::BmiRead(uint8_t reg, uint8_t* data, uint32_t len, void* context) {
    auto* self = static_cast<SensorManager*>(context);
    if (self == nullptr || self->bmi_device_ == nullptr || data == nullptr || !self->TryLockBus()) return -1;
    const esp_err_t result = i2c_master_transmit_receive(self->bmi_device_, &reg, 1, data, len, 200);
    self->UnlockBus();
    return result == ESP_OK ? 0 : -1;
}

int8_t SensorManager::BmiWrite(uint8_t reg, const uint8_t* data, uint32_t len, void* context) {
    auto* self = static_cast<SensorManager*>(context);
    if (self == nullptr || self->bmi_device_ == nullptr || data == nullptr ||
        len + 1 > sizeof(self->bmi_write_buffer_) || !self->TryLockBus()) return -1;
    self->bmi_write_buffer_[0] = reg;
    memcpy(self->bmi_write_buffer_ + 1, data, len);
    const esp_err_t result = i2c_master_transmit(self->bmi_device_, self->bmi_write_buffer_, len + 1, 500);
    self->UnlockBus();
    return result == ESP_OK ? 0 : -1;
}

void SensorManager::BmiDelayUs(uint32_t period_us, void*) {
    if (period_us < 1000) {
        esp_rom_delay_us(period_us);
    } else {
        vTaskDelay(pdMS_TO_TICKS((period_us + 999) / 1000));
    }
}

bool SensorManager::Si12tWrite(uint8_t reg, uint8_t value) {
    if (si12t_device_ == nullptr || !TryLockBus()) return false;
    const uint8_t bytes[2] = {reg, value};
    const esp_err_t result = i2c_master_transmit(si12t_device_, bytes, sizeof(bytes), 200);
    UnlockBus();
    return result == ESP_OK;
}

bool SensorManager::Si12tRead(uint8_t reg, uint8_t* value) {
    if (si12t_device_ == nullptr || value == nullptr || !TryLockBus()) return false;
    const esp_err_t result = i2c_master_transmit_receive(si12t_device_, &reg, 1, value, 1, 200);
    UnlockBus();
    return result == ESP_OK;
}

void SensorManager::MotionTask(void* context) {
    auto* self = static_cast<SensorManager*>(context);
    if (self != nullptr) self->MotionLoop();
    if (self != nullptr) {
        self->motion_task_ = nullptr;
        self->motion_exited_.store(true);
    }
    vTaskDelete(nullptr);
}

void SensorManager::Si12tTask(void* context) {
    auto* self = static_cast<SensorManager*>(context);
    if (self != nullptr) self->Si12tLoop();
    if (self != nullptr) {
        self->si12t_task_ = nullptr;
        self->si12t_exited_.store(true);
    }
    vTaskDelete(nullptr);
}

void SensorManager::MotionLoop() {
    constexpr float kMotionThreshold = 0.3f;
    constexpr int kShakePeaksToTrigger = 2;
    constexpr int64_t kShakeWindowUs = 1000 * 1000;
    constexpr int kLiftSamplesToTrigger = 5;
    constexpr int kStillSamplesToRearm = 50;
    constexpr int64_t kGlobalCooldownUs = 5 * 60 * 1000 * 1000LL;
    int lift_count = 0;
    int still_count = 0;
    bool armed = true;
    int64_t shake_peak_times[8] = {};
    int shake_index = 0;
    float last_ax = 0.0f;
    float last_ay = 0.0f;
    float last_az = 0.0f;
    bool last_valid = false;

    while (running_.load()) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        if (!running_.load() || !bmi_ready_) break;
        bmi2_sens_data accel{};
        if (bmi2_get_sensor_data(&accel, &bmi_dev_) != BMI2_OK) continue;
        const float ax = static_cast<float>(accel.acc.x) / 4096.0f;
        const float ay = static_cast<float>(accel.acc.y) / 4096.0f;
        const float az = static_cast<float>(accel.acc.z) / 4096.0f;
        const float magnitude = sqrtf(ax * ax + ay * ay + az * az);
        float delta = 0.0f;
        if (last_valid) {
            const float dx = ax - last_ax;
            const float dy = ay - last_ay;
            const float dz = az - last_az;
            delta = sqrtf(dx * dx + dy * dy + dz * dz);
        }
        last_ax = ax;
        last_ay = ay;
        last_az = az;
        last_valid = true;
        const bool moving = delta > kMotionThreshold || fabsf(magnitude - 1.0f) > kMotionThreshold;
        const int64_t now = esp_timer_get_time();
        if (!moving) {
            if (++still_count >= kStillSamplesToRearm) armed = true;
            lift_count = 0;
            memset(shake_peak_times, 0, sizeof(shake_peak_times));
            continue;
        }
        still_count = 0;
        if (config_.is_motion_suppressed(config_.context) || !armed ||
            (last_motion_trigger_us_ != 0 && now - last_motion_trigger_us_ < kGlobalCooldownUs)) continue;
        shake_peak_times[shake_index++ % 8] = now;
        int peaks = 0;
        for (const int64_t timestamp : shake_peak_times) {
            if (timestamp > 0 && now - timestamp < kShakeWindowUs) ++peaks;
        }
        if (peaks >= kShakePeaksToTrigger) {
            armed = false;
            lift_count = 0;
            last_motion_trigger_us_ = now;
            memset(shake_peak_times, 0, sizeof(shake_peak_times));
            config_.post_event(config_.context, SensorEvent::kShake);
            continue;
        }
        if (++lift_count >= kLiftSamplesToTrigger) {
            armed = false;
            lift_count = 0;
            last_motion_trigger_us_ = now;
            config_.post_event(config_.context, SensorEvent::kLift);
        }
    }
}

void SensorManager::Si12tLoop() {
    for (int elapsed = 0; elapsed < 120 && running_.load(); ++elapsed) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
    uint8_t previous = 0;
    Si12tRead(0x10, &previous);
    int64_t last_touch_time = 0;
    uint8_t stable[3] = {};
    constexpr int64_t kCooldownUs = 5000000;
    while (running_.load()) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        if (!running_.load() || !si12t_ready_) break;
        uint8_t state = 0;
        if (!Si12tRead(0x10, &state)) continue;
        const int64_t now = esp_timer_get_time();
        for (int zone = 0; zone < 3; ++zone) {
            const uint8_t current = (state >> (zone * 2)) & 0x03;
            stable[zone] = current != 0 ? std::min<uint8_t>(stable[zone] + 1, 3) : 0;
        }
        for (int zone = 0; zone < 3; ++zone) {
            if (stable[zone] == 3 && now - last_touch_time > kCooldownUs) {
                last_touch_time = now;
                stable[zone] = 0;
                config_.post_event(config_.context, SensorEvent::kHeadTouch);
                break;
            }
        }
        previous = state;
        (void)previous;
    }
}
