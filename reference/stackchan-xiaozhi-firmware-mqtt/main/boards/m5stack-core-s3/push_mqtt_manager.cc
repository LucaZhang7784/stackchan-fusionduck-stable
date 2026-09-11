#include "push_mqtt_manager.h"

#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>

#include <stdio.h>
#include <string.h>

namespace {
constexpr char kTag[] = "PushMqttManager";
constexpr int64_t kDebounceUs = 2 * 1000 * 1000;
constexpr int64_t kHeartbeatUs = 30 * 1000 * 1000;
constexpr int64_t kRecoveryPeriodUs = 10 * 1000 * 1000;
constexpr int64_t kInitialStartUs = 10 * 1000 * 1000;
constexpr int64_t kRecoveryOfflineMs = 90 * 1000;
}  // namespace

PushMqttManager::~PushMqttManager() { Deinit(); }

bool PushMqttManager::Configure(const Config& cfg) {
    if (configured_ || cfg.uris == nullptr || cfg.uri_count == 0) {
        ESP_LOGE(kTag, "Configure rejected: configured=%d uris=%p count=%u", configured_, cfg.uris, static_cast<unsigned>(cfg.uri_count));
        return false;
    }
    cfg_ = cfg;
    topics_ = PushMqttTopicBuilder(cfg.mac_address);
    status_topic_cache_ = topics_.Status();
    const esp_timer_create_args_t heartbeat_args = {
        .callback = &PushMqttManager::HeartbeatTimerCb, .arg = this,
        .dispatch_method = ESP_TIMER_TASK, .name = "push_heartbeat", .skip_unhandled_events = true};
    const esp_timer_create_args_t debounce_args = {
        .callback = &PushMqttManager::DebounceTimerCb, .arg = this,
        .dispatch_method = ESP_TIMER_TASK, .name = "push_wifi_debounce", .skip_unhandled_events = true};
    const esp_timer_create_args_t recovery_args = {
        .callback = &PushMqttManager::RecoveryTimerCb, .arg = this,
        .dispatch_method = ESP_TIMER_TASK, .name = "push_mqtt_recovery", .skip_unhandled_events = true};
    const esp_timer_create_args_t initial_args = {
        .callback = &PushMqttManager::InitialStartTimerCb, .arg = this,
        .dispatch_method = ESP_TIMER_TASK, .name = "push_mqtt_start", .skip_unhandled_events = true};
    const esp_timer_create_args_t* args[] = {&heartbeat_args, &debounce_args, &recovery_args, &initial_args};
    esp_timer_handle_t* handles[] = {&heartbeat_timer_, &debounce_timer_, &recovery_timer_, &initial_start_timer_};
    const char* names[] = {"heartbeat", "debounce", "recovery", "initial_start"};
    for (size_t i = 0; i < 4; ++i) {
        const esp_err_t ret = esp_timer_create(args[i], handles[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "Configure esp_timer_create(%s) failed: %s (0x%x)", names[i], esp_err_to_name(ret), ret);
            Deinit();
            return false;
        }
    }
    esp_err_t ret = esp_timer_start_periodic(heartbeat_timer_, kHeartbeatUs);
    if (ret != ESP_OK) ESP_LOGE(kTag, "Configure heartbeat start failed: %s (0x%x)", esp_err_to_name(ret), ret);
    ret = esp_timer_start_periodic(recovery_timer_, kRecoveryPeriodUs);
    if (ret != ESP_OK) ESP_LOGE(kTag, "Configure recovery start failed: %s (0x%x)", esp_err_to_name(ret), ret);
    ret = esp_timer_start_once(initial_start_timer_, kInitialStartUs);
    if (ret != ESP_OK) ESP_LOGE(kTag, "Configure initial start failed: %s (0x%x)", esp_err_to_name(ret), ret);
    wifi_ready_ = true;
    configured_ = true;
    return true;
}

bool PushMqttManager::StartNetwork() {
    if (!configured_ || cfg_.uris == nullptr || cfg_.uri_count == 0) {
        ESP_LOGE(kTag, "StartNetwork rejected: not configured");
        return false;
    }
    if (!event_handlers_registered_) {
        esp_err_t ret = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, WifiEventCb, this);
        if (ret != ESP_OK) { ESP_LOGE(kTag, "Failed to register WIFI_EVENT: %s (0x%x)", esp_err_to_name(ret), ret); return false; }
        ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, WifiEventCb, this);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "Failed to register IP_EVENT: %s (0x%x)", esp_err_to_name(ret), ret);
            esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, WifiEventCb);
            return false;
        }
        event_handlers_registered_ = true;
    }
    if (client_ == nullptr) {
        esp_mqtt_client_config_t mqtt_cfg = {};
        mqtt_cfg.broker.address.uri = cfg_.uris[uri_idx_.load() % cfg_.uri_count];
        mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        mqtt_cfg.buffer.size = 2048;
        mqtt_cfg.session.keepalive = 15;
        mqtt_cfg.session.disable_clean_session = false;
        mqtt_cfg.session.last_will.topic = status_topic_cache_.c_str();
        mqtt_cfg.session.last_will.msg = "offline";
        mqtt_cfg.session.last_will.qos = 1;
        mqtt_cfg.session.last_will.retain = 1;
        mqtt_cfg.network.timeout_ms = 5000;
        mqtt_cfg.network.reconnect_timeout_ms = 2000;
        client_ = esp_mqtt_client_init(&mqtt_cfg);
        if (client_ == nullptr) { ESP_LOGE(kTag, "esp_mqtt_client_init failed"); return false; }
        esp_err_t ret = esp_mqtt_client_register_event(client_, MQTT_EVENT_ANY, MqttEventCb, this);
        if (ret != ESP_OK) {
            ESP_LOGE(kTag, "Failed to register MQTT_EVENT: %s (0x%x)", esp_err_to_name(ret), ret);
            esp_mqtt_client_destroy(client_); client_ = nullptr; return false;
        }
    }
    ESP_LOGI(kTag, "StartNetwork complete; starting URI index %d", uri_idx_.load());
    StartFromCurrentUri();
    return true;
}

void PushMqttManager::Deinit() {
    if (event_handlers_registered_) {
        esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, WifiEventCb);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, WifiEventCb);
        event_handlers_registered_ = false;
    }
    for (auto* timer : {&heartbeat_timer_, &debounce_timer_, &recovery_timer_, &initial_start_timer_}) {
        if (*timer != nullptr) { esp_timer_stop(*timer); esp_timer_delete(*timer); *timer = nullptr; }
    }
    if (client_ != nullptr) { esp_mqtt_client_stop(client_); esp_mqtt_client_destroy(client_); client_ = nullptr; }
    connected_ = false;
    started_ = false;
    wifi_ready_ = false;
    failover_scheduled_ = false;
    recovery_scheduled_ = false;
    offline_since_ms_ = 0;
    status_topic_cache_.clear();
    configured_ = false;
}

bool PushMqttManager::Publish(const std::string& topic, const void* data, size_t len, int qos, int retain) {
    return client_ != nullptr && connected_.load() && esp_mqtt_client_publish(client_, topic.c_str(),
        static_cast<const char*>(data), static_cast<int>(len), qos, retain) >= 0;
}
bool PushMqttManager::Publish(const std::string& topic, const std::string& payload, int qos, int retain) {
    return Publish(topic, payload.data(), payload.size(), qos, retain);
}
std::string PushMqttManager::PushTopic() const { return topics_.Push(); }
std::string PushMqttManager::AckTopic() const { return topics_.Ack(); }
std::string PushMqttManager::StatusTopic() const { return topics_.Status(); }
std::string PushMqttManager::HeartbeatTopic() const { return topics_.Heartbeat(); }
std::string PushMqttManager::DiagTopic() const { return topics_.Diag(); }
std::string PushMqttManager::PhotoTopic() const { return topics_.Photo(); }
std::string PushMqttManager::ConfirmTopic() const { return topics_.Confirm(); }
std::string PushMqttManager::ControlTopic() const { return topics_.Control(); }
std::string PushMqttManager::ControlAckTopic() const { return topics_.ControlAck(); }

void PushMqttManager::StartFromCurrentUri() {
    if (client_ == nullptr || connected_.load() || !wifi_ready_.load()) return;
    const size_t index = static_cast<size_t>(uri_idx_.load()) % cfg_.uri_count;
    if (started_.load()) {
        esp_mqtt_client_reconnect(client_);
        return;
    }
    if (esp_mqtt_client_set_uri(client_, cfg_.uris[index]) != ESP_OK) {
        ESP_LOGW(kTag, "push MQTT URI reset failed");
        return;
    }
    const esp_err_t result = esp_mqtt_client_start(client_);
    if (result == ESP_OK) {
        started_ = true;
    } else {
        ESP_LOGW(kTag, "push MQTT start failed: %s", esp_err_to_name(result));
    }
}

void PushMqttManager::ScheduleWifiDebouncedReconnect() {
    if (debounce_timer_ == nullptr || connected_.load()) return;
    esp_timer_stop(debounce_timer_);
    esp_timer_start_once(debounce_timer_, kDebounceUs);
}

void PushMqttManager::PublishDiag(const char* state, const char* reason, int64_t duration_ms) {
    if (!connected_.load()) return;
    if (duration_ms < 0) duration_ms = 0;
    char payload[224] = {};
    const int length = snprintf(payload, sizeof(payload),
        "{\"v\":1,\"state\":\"%s\",\"reason\":\"%s\",\"offline_ms\":%lu,\"uri\":%d}",
        state ? state : "", reason ? reason : "",
        static_cast<unsigned long>(duration_ms > UINT32_MAX ? UINT32_MAX : duration_ms), uri_idx_.load());
    if (length <= 0 || length >= static_cast<int>(sizeof(payload))) return;
    Publish(DiagTopic(), payload, static_cast<size_t>(length), 1, 0);
}

void PushMqttManager::MqttEventCb(void* arg, esp_event_base_t, int32_t event_id, void* data) {
    auto* self = static_cast<PushMqttManager*>(arg);
    if (self == nullptr) return;
    auto* event = static_cast<esp_mqtt_event_handle_t>(data);
    if (event_id == MQTT_EVENT_CONNECTED) {
        self->fail_count_ = 0;
        self->connected_ = true;
        self->recovery_scheduled_ = false;
        self->offline_since_ms_ = 0;
        const std::string push_topic = self->PushTopic();
        const std::string control_topic = self->ControlTopic();
        esp_mqtt_client_subscribe(self->client_, push_topic.c_str(), 1);
        esp_mqtt_client_subscribe(self->client_, control_topic.c_str(), 1);
        self->Publish(self->StatusTopic(), std::string("online"), 1, 1);
        if (self->last_offline_pending_) {
            self->PublishDiag("recovered", self->last_offline_reason_.c_str(),
                              esp_timer_get_time() / 1000 - self->last_offline_since_ms_);
            self->last_offline_pending_ = false;
        }
        ESP_LOGI(kTag, "connected via %s", self->cfg_.uris[self->uri_idx_.load() % self->cfg_.uri_count]);
        return;
    }
    if (event_id == MQTT_EVENT_ERROR) {
        if (event != nullptr && event->error_handle != nullptr) {
            ESP_LOGW(kTag, "MQTT error: type=%d tls=%d sock=%d", event->error_handle->error_type,
                     event->error_handle->esp_tls_last_esp_err, event->error_handle->esp_transport_sock_errno);
        }
        return;
    }
    if (event_id == MQTT_EVENT_DISCONNECTED) {
        self->connected_ = false;
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (self->offline_since_ms_.exchange(now_ms) == 0) {
            self->last_offline_since_ms_ = now_ms;
            self->last_offline_pending_ = true;
            self->last_offline_reason_ = "mqtt_disconnected";
        }
        if (self->cfg_.on_disconnected != nullptr) self->cfg_.on_disconnected(self->cfg_.user_ctx);
        if (self->fail_count_.fetch_add(1) + 1 >= 3 && !self->failover_scheduled_.exchange(true)) {
            self->fail_count_ = 0;
            self->uri_idx_ = (self->uri_idx_.load() + 1) % self->cfg_.uri_count;
            auto restart = [self]() {
                if (!self->connected_.load() && self->client_ != nullptr) {
                    if (self->started_.load()) {
                        const esp_err_t stopped = esp_mqtt_client_stop(self->client_);
                        if (stopped == ESP_OK || stopped == ESP_ERR_INVALID_STATE) self->started_ = false;
                    }
                    self->StartFromCurrentUri();
                }
                self->failover_scheduled_ = false;
            };
            if (self->cfg_.scheduler != nullptr) self->cfg_.scheduler(self->cfg_.user_ctx, std::move(restart));
            else restart();
        }
        return;
    }
    if (event_id == MQTT_EVENT_DATA && event != nullptr && event->current_data_offset == 0 &&
        event->data != nullptr && event->data_len > 0 && self->cfg_.on_data != nullptr) {
        self->cfg_.on_data(self->cfg_.user_ctx, event->topic, event->topic_len,
                           reinterpret_cast<const uint8_t*>(event->data), event->data_len);
    }
}

void PushMqttManager::WifiEventCb(void* arg, esp_event_base_t base, int32_t event_id, void*) {
    auto* self = static_cast<PushMqttManager*>(arg);
    if (self == nullptr) return;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        self->wifi_ready_ = false;
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        self->wifi_ready_ = true;
        self->ScheduleWifiDebouncedReconnect();
    }
}

void PushMqttManager::HeartbeatTimerCb(void* arg) {
    auto* self = static_cast<PushMqttManager*>(arg);
    if (self == nullptr || !self->connected_.load()) return;
    char payload[96] = {};
    const uint32_t seq = self->heartbeat_seq_.fetch_add(1) + 1;
    snprintf(payload, sizeof(payload), "{\"seq\":%lu,\"uptime_s\":%lu}",
             static_cast<unsigned long>(seq), static_cast<unsigned long>(esp_timer_get_time() / 1000000));
    self->Publish(self->HeartbeatTopic(), payload, strlen(payload), 0, 0);
}

void PushMqttManager::DebounceTimerCb(void* arg) {
    auto* self = static_cast<PushMqttManager*>(arg);
    if (self == nullptr || self->connected_.load() || !self->wifi_ready_.load()) return;
    auto restart = [self]() { self->StartFromCurrentUri(); };
    if (self->cfg_.scheduler != nullptr) self->cfg_.scheduler(self->cfg_.user_ctx, std::move(restart));
    else restart();
}

void PushMqttManager::RecoveryTimerCb(void* arg) {
    auto* self = static_cast<PushMqttManager*>(arg);
    if (self == nullptr || !self->wifi_ready_.load() || self->connected_.load()) return;
    const int64_t offline_since = self->offline_since_ms_.load();
    if (offline_since == 0 || esp_timer_get_time() / 1000 - offline_since < kRecoveryOfflineMs ||
        self->recovery_scheduled_.exchange(true)) return;
    auto restart = [self]() {
        if (!self->connected_.load() && self->client_ != nullptr) {
            self->uri_idx_ = (self->uri_idx_.load() + 1) % self->cfg_.uri_count;
            if (self->started_.load()) {
                const esp_err_t stopped = esp_mqtt_client_stop(self->client_);
                if (stopped == ESP_OK || stopped == ESP_ERR_INVALID_STATE) self->started_ = false;
            }
            self->StartFromCurrentUri();
        }
        self->recovery_scheduled_ = false;
    };
    if (self->cfg_.scheduler != nullptr) self->cfg_.scheduler(self->cfg_.user_ctx, std::move(restart));
    else restart();
}

void PushMqttManager::InitialStartTimerCb(void* arg) {
    auto* self = static_cast<PushMqttManager*>(arg);
    if (self == nullptr) return;
    auto start = [self]() { self->StartNetwork(); };
    if (self->cfg_.scheduler != nullptr) self->cfg_.scheduler(self->cfg_.user_ctx, std::move(start));
    else start();
}
