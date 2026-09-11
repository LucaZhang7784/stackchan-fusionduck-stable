#include "py32_led_controller.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include "led_controller.h"
bool Py32LedController::Initialize(const Config& c){config_=c;if(!config_.bus)return false;i2c_device_config_t d{};d.dev_addr_length=I2C_ADDR_BIT_LEN_7;d.device_address=0x6F;d.scl_speed_hz=100000;if(i2c_master_bus_add_device(config_.bus,&d,&dev_)!=ESP_OK)return false;SetRegBit(0x04,0x20,true);SetRegBit(0x0A,0x20,true);SetRegBit(0x0C,0x20,false);SetRegBit(0x14,0x20,false);vTaskDelay(pdMS_TO_TICKS(50));uint8_t n=12;WriteRegBlock(0x24,&n,1);ready_=true;TurnOff();TurnOff();return true;}
bool Py32LedController::ReadReg(uint8_t r,uint8_t* o){if(!dev_||!o)return false;if(config_.try_lock&&!config_.try_lock(config_.lock_ctx,50))return false;auto e=i2c_master_transmit_receive(dev_,&r,1,o,1,200);if(config_.unlock)config_.unlock(config_.lock_ctx);return e==ESP_OK;}
bool Py32LedController::SetRegBit(uint8_t r,uint8_t m,bool on){uint8_t v=0;if(!ReadReg(r,&v))return false;if(on)v|=m;else v&=~m;return WriteRegBlock(r,&v,1);}
bool Py32LedController::WriteRegBlock(uint8_t r,const uint8_t* p,size_t n){if(!dev_||!p||n>79)return false;uint8_t b[80];b[0]=r;memcpy(b+1,p,n);for(int i=0;i<2;i++){if(config_.try_lock&&!config_.try_lock(config_.lock_ctx,50))continue;auto e=i2c_master_transmit(dev_,b,n+1,200);if(config_.unlock)config_.unlock(config_.lock_ctx);if(e==ESP_OK)return true;vTaskDelay(pdMS_TO_TICKS(5));}return false;}
bool Py32LedController::SetLedFrame(const uint16_t* p,size_t n){if(!ready_||!p||n==0)return false;if(n>12)n=12;uint8_t b[24]{};for(size_t i=0;i<n;i++){b[2*i]=p[i]&255;b[2*i+1]=p[i]>>8;}if(!WriteRegBlock(0x30,b,n*2))return false;uint8_t cur=0;ReadReg(0x24,&cur);uint8_t refresh=cur|0x40;return WriteRegBlock(0x24,&refresh,1);}
bool Py32LedController::TurnOff(){uint16_t z[12]{};return SetLedFrame(z,12);}
void Py32LedController::SetManualColor(uint16_t rgb565){manual_color_=rgb565;led_manual_=true;SetLedFrame(&manual_color_,1);}
void Py32LedController::ClearManual(){led_manual_=false;last_state_eff_.clear();}
void Py32LedController::UpdateEmotion(const std::string& emotion, DeviceState state){
 if(!ready_)return;
 bool active=state==kDeviceStateConnecting||state==kDeviceStateListening||state==kDeviceStateSpeaking;
 if(led_manual_&&!active)return;
 const char* e=emotion.empty()?"neutral":emotion.c_str();
 if(state==kDeviceStateConnecting)e="connecting";
 else if(state==kDeviceStateListening)e="listening";
 else if(state==kDeviceStateSpeaking)e="speaking";
 else if(state==kDeviceStateIdle)e="neutral";
 uint8_t r=0,g=0,b=0; LedController::ColorForEmotion(e,r,g,b);
 uint16_t color=LedController::Rgb888To565(r,g,b);
 uint16_t frame[12];
 for (auto& v : frame) {
  v = color;
 }
 SetLedFrame(frame,12);
}
void Py32LedController::UpdateDeviceState(DeviceState state){
 bool active=state==kDeviceStateConnecting||state==kDeviceStateListening||state==kDeviceStateSpeaking;
 if(led_manual_&&!active){ClearManual();}
 const char* e="neutral";
 if(state==kDeviceStateConnecting)e="connecting";
 else if(state==kDeviceStateListening)e="listening";
 else if(state==kDeviceStateSpeaking)e="speaking";
 if(last_state_eff_==e&&!(!led_manual_&&state==kDeviceStateIdle))return;
 last_state_eff_=e; UpdateEmotion(e,state);
}
