#pragma once

#include <atomic>
#include <stdint.h>

#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "bmi2.h"

enum class SensorEvent : uint8_t {
    kShake,
    kLift,
    kHeadTouch,
};

class SensorManager {
public:
    using TryLock = bool (*)(void* context);
    using Unlock = void (*)(void* context);
    using EventSink = void (*)(void* context, SensorEvent event);
    using ActivityGate = bool (*)(void* context);

    struct Config {
        i2c_master_bus_handle_t bus = nullptr;
        void* context = nullptr;
        TryLock try_lock = nullptr;
        Unlock unlock = nullptr;
        EventSink post_event = nullptr;
        ActivityGate is_motion_suppressed = nullptr;
    };

    SensorManager() = default;
    ~SensorManager();

    bool Init(const Config& config);
    void Deinit();
    bool IsBmiReady() const { return bmi_ready_; }
    bool IsSi12tReady() const { return si12t_ready_; }

private:
    static int8_t BmiRead(uint8_t reg, uint8_t* data, uint32_t len, void* context);
    static int8_t BmiWrite(uint8_t reg, const uint8_t* data, uint32_t len, void* context);
    static void BmiDelayUs(uint32_t period_us, void* context);
    static void MotionTask(void* context);
    static void Si12tTask(void* context);

    bool TryLockBus() const;
    void UnlockBus() const;
    bool StartBmi270();
    bool StartSi12t();
    void MotionLoop();
    void Si12tLoop();
    bool Si12tWrite(uint8_t reg, uint8_t value);
    bool Si12tRead(uint8_t reg, uint8_t* value);
    bool WaitForExit(std::atomic<bool>& exited);

    Config config_{};
    i2c_master_dev_handle_t bmi_device_ = nullptr;
    i2c_master_dev_handle_t si12t_device_ = nullptr;
    bmi2_dev bmi_dev_{};
    TaskHandle_t motion_task_ = nullptr;
    TaskHandle_t si12t_task_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> motion_exited_{true};
    std::atomic<bool> si12t_exited_{true};
    bool bmi_ready_ = false;
    bool si12t_ready_ = false;
    int64_t last_motion_trigger_us_ = 0;
    uint8_t bmi_write_buffer_[8300]{};
};
