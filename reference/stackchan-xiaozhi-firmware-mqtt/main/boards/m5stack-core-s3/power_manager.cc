#include "power_manager.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
constexpr const char* kTag = "PowerManager";
constexpr uint8_t kAxpAddress = 0x34;
constexpr uint8_t kPy32Address = 0x6F;
}  // namespace

PowerManager::~PowerManager() {
    Deinit();
}

bool PowerManager::TryLockBus() const {
    return config_.try_lock != nullptr && config_.try_lock(config_.context);
}

void PowerManager::UnlockBus() const {
    if (config_.unlock != nullptr) config_.unlock(config_.context);
}

bool PowerManager::Init(const Config& config) {
    Deinit();
    if (config.bus == nullptr || config.try_lock == nullptr || config.unlock == nullptr ||
        config.post_low_battery == nullptr) {
        ESP_LOGE(kTag, "invalid power manager config");
        return false;
    }
    config_ = config;
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = kAxpAddress,
        .scl_speed_hz = 400000,
    };
    if (!TryLockBus()) return false;
    const esp_err_t add_result = i2c_master_bus_add_device(config_.bus, &device_config, &axp_device_);
    UnlockBus();
    if (add_result != ESP_OK) {
        ESP_LOGE(kTag, "AXP2101 add device failed: %s", esp_err_to_name(add_result));
        axp_device_ = nullptr;
        return false;
    }

    uint8_t value = 0;
    if (ReadRegister(axp_device_, 0x90, &value)) WriteRegister(axp_device_, 0x90, value | 0b10110100);
    WriteRegister(axp_device_, 0x99, 0b11110 - 5);
    WriteRegister(axp_device_, 0x97, 0b11110 - 2);
    WriteRegister(axp_device_, 0x69, 0b00110101);
    WriteRegister(axp_device_, 0x30, 0b111111);
    WriteRegister(axp_device_, 0x90, 0xBF);
    WriteRegister(axp_device_, 0x94, 33 - 5);
    WriteRegister(axp_device_, 0x95, 33 - 5);

    esp_timer_create_args_t args = {};
    args.callback = BatteryTimer;
    args.arg = this;
    args.name = "batt_check";
    args.dispatch_method = ESP_TIMER_TASK;
    args.skip_unhandled_events = true;
    if (esp_timer_create(&args, &battery_timer_) != ESP_OK ||
        esp_timer_start_periodic(battery_timer_, 60 * 1000 * 1000) != ESP_OK) {
        ESP_LOGE(kTag, "battery timer initialization failed");
        Deinit();
        return false;
    }
    initialized_ = true;
    ESP_LOGI(kTag, "AXP2101 ready at 0x%02X", kAxpAddress);
    return true;
}

void PowerManager::Deinit() {
    if (battery_timer_ != nullptr) {
        esp_timer_stop(battery_timer_);
        esp_timer_delete(battery_timer_);
        battery_timer_ = nullptr;
    }
    if (axp_device_ != nullptr && config_.bus != nullptr && TryLockBus()) {
        i2c_master_bus_rm_device(axp_device_);
        UnlockBus();
    }
    axp_device_ = nullptr;
    initialized_ = false;
    low_battery_warned_ = false;
    config_ = {};
}

bool PowerManager::ReadRegister(i2c_master_dev_handle_t device, uint8_t reg, uint8_t* value) {
    if (device == nullptr || value == nullptr || !TryLockBus()) return false;
    const esp_err_t result = i2c_master_transmit_receive(device, &reg, 1, value, 1, 200);
    UnlockBus();
    return result == ESP_OK;
}

bool PowerManager::WriteRegister(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value) {
    if (device == nullptr || !TryLockBus()) return false;
    const uint8_t bytes[2] = {reg, value};
    const esp_err_t result = i2c_master_transmit(device, bytes, sizeof(bytes), 200);
    UnlockBus();
    return result == ESP_OK;
}

bool PowerManager::SetRegisterBit(i2c_master_dev_handle_t device, uint8_t reg, uint8_t bit) {
    uint8_t value = 0;
    return ReadRegister(device, reg, &value) && WriteRegister(device, reg, value | bit);
}

bool PowerManager::EnableServoRail() {
    if (!initialized_) return false;
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = kPy32Address,
        .scl_speed_hz = 100000,
        .scl_wait_us = 0,
        .flags = {.disable_ack_check = 0},
    };
    i2c_master_dev_handle_t device = nullptr;
    if (!TryLockBus()) return false;
    const esp_err_t add_result = i2c_master_bus_add_device(config_.bus, &device_config, &device);
    UnlockBus();
    if (add_result != ESP_OK) {
        ESP_LOGE(kTag, "PY32 add temporary device failed: %s", esp_err_to_name(add_result));
        return false;
    }

    bool enabled = false;
    for (int attempt = 0; attempt < 10 && !enabled; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(200));
        uint8_t version = 0;
        if (ReadRegister(device, 0x02, &version) && version != 0 && version != 0xFF) {
            enabled = SetRegisterBit(device, 0x03, 0x01) && SetRegisterBit(device, 0x09, 0x01) &&
                      SetRegisterBit(device, 0x05, 0x01);
            ESP_LOGI(kTag, "PY32 version=%u servo rail=%s", version, enabled ? "enabled" : "failed");
        }
    }

    // The handle is deliberately temporary: LED initialization owns its own
    // persistent 0x6F handle and must never compete with this handshake handle.
    if (TryLockBus()) {
        i2c_master_bus_rm_device(device);
        UnlockBus();
    } else {
        ESP_LOGE(kTag, "PY32 temporary handle could not be removed");
        enabled = false;
    }
    return enabled;
}

bool PowerManager::ReadBattery(int& level, bool& charging, bool& discharging) {
    level = 0;
    charging = false;
    discharging = false;
    if (!initialized_) return false;
    uint8_t status = 0;
    uint8_t battery = 0;
    if (!ReadRegister(axp_device_, 0x01, &status) || !ReadRegister(axp_device_, 0xA4, &battery)) return false;
    const int direction = (status & 0b01100000) >> 5;
    charging = direction == 1;
    discharging = direction == 2;
    level = battery;
    return true;
}

void PowerManager::SetBacklightBrightness(uint8_t brightness) {
    if (!initialized_) return;
    WriteRegister(axp_device_, 0x99, static_cast<uint8_t>((brightness + 641) >> 5));
}

void PowerManager::BatteryTimer(void* context) {
    auto* self = static_cast<PowerManager*>(context);
    if (self == nullptr || !self->initialized_) return;
    int level = 0;
    bool charging = false;
    bool discharging = false;
    if (!self->ReadBattery(level, charging, discharging)) return;
    if (level > 0 && level <= 15 && !charging && !self->low_battery_warned_) {
        self->low_battery_warned_ = true;
        self->config_.post_low_battery(self->config_.context, level);
    } else if (level > 20 || charging) {
        self->low_battery_warned_ = false;
    }
}
