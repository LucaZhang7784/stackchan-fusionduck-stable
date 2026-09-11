#pragma once
#include <memory>
#include <driver/i2c_master.h>
#include <sys/ioctl.h>
#include "esp_video.h"
class PushMqttManager;
class FaceTracker;
class CameraService {
public:
    struct Config { i2c_master_bus_handle_t i2c_bus{}; PushMqttManager* push_mqtt{}; FaceTracker* face_tracker{}; };
    CameraService() = default;
    ~CameraService();
    bool Initialize(const Config& config);
    bool IsOk() const;
    EspVideo* GetVideoDevice();
    void SnapPhoto();
private:
    Config config_{};
    std::unique_ptr<EspVideo> video_;
};
