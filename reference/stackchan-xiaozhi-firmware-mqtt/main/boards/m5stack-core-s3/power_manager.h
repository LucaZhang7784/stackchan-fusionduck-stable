#pragma once

#include <stdint.h>

#include <driver/i2c_master.h>
#include <esp_timer.h>

class PowerManager {
public:
    using TryLock = bool (*)(void* context);
    using Unlock = void (*)(void* context);
    using LowBatterySink = void (*)(void* context, int level);

    struct Config {
        i2c_master_bus_handle_t bus = nullptr;
        void* context = nullptr;
        TryLock try_lock = nullptr;
        Unlock unlock = nullptr;
        LowBatterySink post_low_battery = nullptr;
    };

    PowerManager() = default;
    ~PowerManager();

    bool Init(const Config& config);
    void Deinit();
    bool EnableServoRail();
    bool ReadBattery(int& level, bool& charging, bool& discharging);
    void SetBacklightBrightness(uint8_t brightness);

private:
    static void BatteryTimer(void* context);
    bool TryLockBus() const;
    void UnlockBus() const;
    bool ReadRegister(i2c_master_dev_handle_t device, uint8_t reg, uint8_t* value);
    bool WriteRegister(i2c_master_dev_handle_t device, uint8_t reg, uint8_t value);
    bool SetRegisterBit(i2c_master_dev_handle_t device, uint8_t reg, uint8_t bit);

    Config config_{};
    i2c_master_dev_handle_t axp_device_ = nullptr;
    esp_timer_handle_t battery_timer_ = nullptr;
    bool low_battery_warned_ = false;
    bool initialized_ = false;
};
