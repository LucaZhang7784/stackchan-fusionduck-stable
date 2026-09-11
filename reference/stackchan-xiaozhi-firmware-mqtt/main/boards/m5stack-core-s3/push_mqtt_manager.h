#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <functional>
#include <mqtt_client.h>
#include <string>

#include "push_mqtt_config.h"

class PushMqttManager {
public:
    using DataHandler = void (*)(void*, const char*, size_t, const uint8_t*, size_t);
    using DisconnectHandler = void (*)(void*);
    using TaskScheduler = void (*)(void*, std::function<void()>);
    struct Config {
        std::string mac_address;
        const char* const* uris = nullptr;
        size_t uri_count = 0;
        void* user_ctx = nullptr;
        DataHandler on_data = nullptr;
        DisconnectHandler on_disconnected = nullptr;
        TaskScheduler scheduler = nullptr;
    };

    PushMqttManager() = default;
    ~PushMqttManager();
    bool Configure(const Config& cfg);
    bool StartNetwork();
    void Deinit();
    bool Publish(const std::string& topic, const void* data, size_t len, int qos = 1, int retain = 0);
    bool Publish(const std::string& topic, const std::string& payload, int qos = 1, int retain = 0);
    bool IsConnected() const { return connected_.load(); }
    void ScheduleWifiDebouncedReconnect();
    std::string PushTopic() const;
    std::string AckTopic() const;
    std::string StatusTopic() const;
    std::string HeartbeatTopic() const;
    std::string DiagTopic() const;
    std::string PhotoTopic() const;
    std::string ConfirmTopic() const;
    std::string ControlTopic() const;
    std::string ControlAckTopic() const;

private:
    static void MqttEventCb(void*, esp_event_base_t, int32_t, void*);
    static void WifiEventCb(void*, esp_event_base_t, int32_t, void*);
    static void HeartbeatTimerCb(void*);
    static void DebounceTimerCb(void*);
    static void RecoveryTimerCb(void*);
    static void InitialStartTimerCb(void*);
    void StartFromCurrentUri();
    void PublishDiag(const char*, const char*, int64_t);
    Config cfg_;
    PushMqttTopicBuilder topics_{""};
    esp_mqtt_client_handle_t client_ = nullptr;
    esp_timer_handle_t heartbeat_timer_ = nullptr, debounce_timer_ = nullptr;
    esp_timer_handle_t recovery_timer_ = nullptr, initial_start_timer_ = nullptr;
    std::atomic<bool> started_{false}, connected_{false}, wifi_ready_{true};
    std::atomic<bool> failover_scheduled_{false}, recovery_scheduled_{false};
    std::atomic<int> uri_idx_{0}, fail_count_{0};
    std::atomic<uint32_t> heartbeat_seq_{0};
    std::atomic<int64_t> offline_since_ms_{0};
    bool configured_ = false;
    bool event_handlers_registered_ = false;
    std::string status_topic_cache_;
    int64_t last_offline_since_ms_ = 0;
    bool last_offline_pending_ = false;
    std::string last_offline_reason_;
};
