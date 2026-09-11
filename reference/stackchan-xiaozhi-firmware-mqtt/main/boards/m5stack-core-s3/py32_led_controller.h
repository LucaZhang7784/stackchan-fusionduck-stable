#pragma once
#include <stdint.h>
#include <stddef.h>
#include <driver/i2c_master.h>
#include "device_state.h"
#include <string>
class Py32LedController {
public:
 struct Config { i2c_master_bus_handle_t bus{}; void* lock_ctx{}; bool (*try_lock)(void*, uint32_t){}; void (*unlock)(void*){}; };
 bool Initialize(const Config&);
 bool SetLedFrame(const uint16_t*, size_t);
 bool TurnOff();
 bool IsReady() const { return ready_; }
 void UpdateEmotion(const std::string& emotion, DeviceState state);
 void UpdateDeviceState(DeviceState state);
 void SetManualColor(uint16_t rgb565);
 void ClearManual();
private:
 bool ReadReg(uint8_t, uint8_t*); bool SetRegBit(uint8_t, uint8_t, bool); bool WriteRegBlock(uint8_t, const uint8_t*, size_t);
 Config config_{}; i2c_master_dev_handle_t dev_{}; bool ready_=false;
 bool led_manual_ = false;
 uint16_t manual_color_ = 0;
 std::string last_state_eff_;
};
