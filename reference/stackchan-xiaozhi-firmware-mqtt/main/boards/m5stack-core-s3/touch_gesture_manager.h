#pragma once

#include <stdint.h>
#include <string>
#include <functional>
#include <memory>
#include <driver/i2c_master.h>
#include <esp_timer.h>

class Display;
class StackChanServo;
class ConfirmManager;
class PushMqttManager;

class TouchGestureManager {
public:
    using LockBusFunc = bool (*)(void*);
    using UnlockBusFunc = void (*)(void*);
    using WifiConfigFunc = void (*)(void*);
    using PushInterruptFunc = void (*)(void*);

    struct Dependencies {
        i2c_master_bus_handle_t i2c_bus = nullptr;
        void* bus_lock_ctx = nullptr;
        LockBusFunc lock_bus = nullptr;
        UnlockBusFunc unlock_bus = nullptr;
        Display* display = nullptr;
        StackChanServo* servo = nullptr;
        ConfirmManager* confirm_mgr = nullptr;
        PushMqttManager* push_mqtt = nullptr;
        void* wifi_ctx = nullptr;
        WifiConfigFunc enter_wifi_config = nullptr;
        void* push_ctx = nullptr;
        PushInterruptFunc on_push_interrupt = nullptr;
    };

    TouchGestureManager();
    ~TouchGestureManager();

    bool Initialize(const Dependencies& deps);
    void Poll();

private:
    static void MapTouchToScreen(int& x, int& y);

    class Impl;
    std::unique_ptr<Impl> impl_;
};