#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <array>
#include <atomic>
#include <esp_timer.h>

class PushMqttManager;
class RobotActionDispatcher;
class CameraService;
class AudioCodec;
class Display;
class AudioService;

class PushMessageController {
public:
    struct Dependencies {
        PushMqttManager* push_mqtt = nullptr;
        RobotActionDispatcher* dispatcher = nullptr;
        CameraService* camera = nullptr;
        AudioCodec* audio_codec = nullptr;
        AudioService* audio_service = nullptr;
        Display* display = nullptr;
        std::function<void(const uint8_t*, size_t, std::function<void()>)> push_pcm;
        std::function<void()> reset_decoder;
        std::function<void()> abort_speaking;
        std::function<int()> get_device_state;
        std::function<void(int)> set_device_state;
    };

    PushMessageController() = default;
    ~PushMessageController();
    void Initialize(const Dependencies& dependencies);
    void Interrupt();
    void OnDisconnected();
    static void OnMqttData(void* arg, const char* topic, size_t topic_len,
                           const uint8_t* data, size_t data_len);
    bool IsPushActive() const { return push_active_.load(); }

private:
    void HandleData(const char* topic, size_t topic_len,
                    const uint8_t* data, size_t data_len);
    static void WatchdogTimerCb(void* arg);
    static void FinishTimerCb(void* arg);
    void PublishPushPlaybackStart(const std::string& msg_uid);
    Dependencies dependencies_{};
    std::atomic<bool> push_active_{false};
    bool push_finishing_ = false;
    volatile int64_t finish_ready_ms_ = 0;
    volatile int64_t last_push_frame_ms_ = 0;
    esp_timer_handle_t push_finish_timer_ = nullptr;
    esp_timer_handle_t push_wd_timer_ = nullptr;
    std::string push_msg_uid_;
    bool push_play_start_pending_ = false;
    std::array<std::string, 32> control_request_cache_{};
    size_t control_request_cache_pos_ = 0;
};
