#include "push_message_controller.h"
#include <esp_log.h>
#include <esp_wifi.h>
#include <string.h>
#include <stdio.h>
#include "application.h"
#include "push_mqtt_manager.h"
#include "push_packet_parser.h"
#include "device_control_secret.h"
#include "robot_action_dispatcher.h"
#include "camera_service.h"
#include "cores3_audio_codec.h"
#include "display/lcd_display.h"
#define TAG "PushMsgCtrl"
PushMessageController::~PushMessageController() {
    if (push_wd_timer_) {
        esp_timer_stop(push_wd_timer_);
        esp_timer_delete(push_wd_timer_);
        push_wd_timer_ = nullptr;
    }
    if (push_finish_timer_) {
        esp_timer_stop(push_finish_timer_);
        esp_timer_delete(push_finish_timer_);
        push_finish_timer_ = nullptr;
    }
}
void PushMessageController::Initialize(const Dependencies& dependencies) {
    dependencies_ = dependencies;
    // 1. 断流看门狗: 1s 周期检测，5s 无新帧且音频空闲则回退 Idle
    esp_timer_create_args_t wd_args = {};
    wd_args.callback = &PushMessageController::WatchdogTimerCb;
    wd_args.arg = this;
    wd_args.name = "push_mqtt_wd";
    if (esp_timer_create(&wd_args, &push_wd_timer_) == ESP_OK) {
        esp_timer_start_periodic(push_wd_timer_, 1000000);
    }
    // 2. 200ms STOP 轮询定时器: 队列排空 + 100ms DMA 静音放空
    esp_timer_create_args_t fin_args = {};
    fin_args.callback = &PushMessageController::FinishTimerCb;
    fin_args.arg = this;
    fin_args.name = "push_finish_poll";
    esp_timer_create(&fin_args, &push_finish_timer_);
}
void PushMessageController::Interrupt() {
    push_active_ = false;
    push_finishing_ = false;
    finish_ready_ms_ = 0;
    if (push_finish_timer_) {
        esp_timer_stop(push_finish_timer_);
    }
}
void PushMessageController::OnDisconnected() {
    Application::GetInstance().Schedule([this]() {
        if (!push_active_) return;
        Interrupt();
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateSpeaking) {
            app.SetDeviceState(kDeviceStateIdle);
        }
    });
}
void PushMessageController::OnMqttData(void* arg, const char* topic, size_t topic_len,
                                       const uint8_t* data, size_t data_len) {
    auto* self = static_cast<PushMessageController*>(arg);
    if (self) {
        self->HandleData(topic, topic_len, data, data_len);
    }
}
void PushMessageController::PublishPushPlaybackStart(const std::string& msg_uid) {
    if (msg_uid.empty() || !dependencies_.push_mqtt) return;
    const std::string payload = "{\"v\":1,\"id\":\"" + msg_uid
        + "\",\"status\":\"play_start\",\"kind\":\"audio\"}";
    dependencies_.push_mqtt->Publish(dependencies_.push_mqtt->AckTopic(), payload, 1, 0);
}
void PushMessageController::HandleData(const char* raw_topic, size_t topic_len,
                                       const uint8_t* raw_data, size_t raw_data_len) {
    if (!raw_topic || !raw_data || raw_data_len == 0 || !dependencies_.push_mqtt) return;
    const ByteView input{raw_data, raw_data_len};
    const std::string topic(raw_topic, topic_len);
    const std::string push_topic = dependencies_.push_mqtt->PushTopic();
    const std::string control_topic = dependencies_.push_mqtt->ControlTopic();
    if (topic != push_topic && topic != control_topic) return;
    // A. 控制报文处理
    if (topic == control_topic) {
        ParsedPushPacket control;
        if (!PushPacketParser::ParseControl(input, STACKCHAN_DEVICE_CONTROL_SECRET_HEX, &control)) return;
        const int volume = control.volume;
        char req[9] = {};
        for (int i = 0; i < 4; ++i) {
            snprintf(req + i * 2, 3, "%02x", control.request_id[i]);
        }
        bool duplicate = false;
        for (const auto& seen : control_request_cache_) duplicate |= (seen == req);
        if (duplicate) return;
        control_request_cache_[control_request_cache_pos_++ % control_request_cache_.size()] = req;
        Application::GetInstance().Schedule([this, volume, request_id = std::string(req)]() {
            auto* codec = dependencies_.audio_codec;
            const bool ok = codec != nullptr;
            if (ok) codec->SetOutputVolume(volume);
            char ack[128] = {};
            snprintf(ack, sizeof(ack), "{\"request_id\":\"%s\",\"ok\":%s,\"status\":\"%s\",\"volume\":%d}",
                     request_id.c_str(), ok ? "true" : "false", ok ? "applied" : "no_codec", ok ? codec->output_volume() : -1);
            dependencies_.push_mqtt->Publish(dependencies_.push_mqtt->ControlAckTopic(), std::string(ack), 1, 0);
        });
        return;
    }
    // B. 推送报文处理
    ParsedPushPacket packet;
    if (!PushPacketParser::ParsePush(input, &packet)) return;
    auto& app = Application::GetInstance();
    if (packet.kind == PushPacketKind::kStart) {
        const std::string msg_uid = packet.msg_uid.data
            ? std::string(reinterpret_cast<const char*>(packet.msg_uid.data), packet.msg_uid.size)
            : std::string();
        const std::string action = packet.action.data
            ? std::string(reinterpret_cast<const char*>(packet.action.data), packet.action.size)
            : std::string();
        const std::string text = packet.text.data
            ? std::string(reinterpret_cast<const char*>(packet.text.data), packet.text.size)
            : std::string();
        if (app.GetDeviceState() == kDeviceStateListening) {
            push_active_ = false;
            return;
        }
        push_active_ = true;
        push_msg_uid_ = msg_uid;
        push_play_start_pending_ = !msg_uid.empty();
        push_finishing_ = false;
        finish_ready_ms_ = 0;
        if (push_finish_timer_) esp_timer_stop(push_finish_timer_);
        last_push_frame_ms_ = esp_timer_get_time() / 1000;
        esp_wifi_set_ps(WIFI_PS_NONE);
        if (!msg_uid.empty() || !text.empty()) {
            const char* ack_payload = msg_uid.empty() ? text.c_str() : msg_uid.c_str();
            int ack_len = (int)(msg_uid.empty() ? text.size() : msg_uid.size());
            dependencies_.push_mqtt->Publish(dependencies_.push_mqtt->AckTopic(), ack_payload,
                                             static_cast<size_t>(ack_len), 0, 0);
        }
        if (app.GetDeviceState() == kDeviceStateSpeaking) {
            app.AbortSpeaking(kAbortReasonNone);
            if (dependencies_.audio_service) {
                dependencies_.audio_service->ResetDecoder();
            }
        }
        app.SetDeviceState(kDeviceStateSpeaking);
        if (dependencies_.dispatcher) {
            dependencies_.dispatcher->DispatchPushAction(action, text, msg_uid);
        }
        if (!text.empty()) {
            app.Schedule([display = dependencies_.display, text]() {
                if (display) display->SetChatMessage("assistant", text.c_str());
            });
        }
    } else if (packet.kind == PushPacketKind::kPcmBatch && push_active_) {
        if (app.GetDeviceState() != kDeviceStateSpeaking) return;
        last_push_frame_ms_ = esp_timer_get_time() / 1000;
        if (packet.pcm_truncated) {
            ESP_LOGE(TAG, "push pcm batch truncated: count=%u data_len=%u",
                     packet.frame_count, static_cast<unsigned>(raw_data_len));
        }
        std::function<void()> on_playback_start;
        if (push_play_start_pending_) {
            const std::string msg_uid = push_msg_uid_;
            on_playback_start = [this, msg_uid]() {
                Application::GetInstance().Schedule([this, msg_uid]() {
                    PublishPushPlaybackStart(msg_uid);
                });
            };
        }
        auto* as = dependencies_.audio_service ? dependencies_.audio_service : &app.GetAudioService();
        if (as->PushPcmToPlaybackQueue(packet.pcm, packet.pcm_bytes, std::move(on_playback_start))) {
            push_play_start_pending_ = false;
        }
    } else if (packet.kind == PushPacketKind::kStop) {
        push_active_ = false;
        push_finishing_ = true;
        finish_ready_ms_ = 0;
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        if (push_finish_timer_) esp_timer_start_periodic(push_finish_timer_, 200 * 1000);
    } else if (packet.kind == PushPacketKind::kSnap) {
        if (dependencies_.camera) {
            dependencies_.camera->SnapPhoto();
        }
    }
}
void PushMessageController::WatchdogTimerCb(void* arg) {
    auto* self = static_cast<PushMessageController*>(arg);
    if (!self || !self->push_active_) return;
    auto& app = Application::GetInstance();
    auto* as = self->dependencies_.audio_service ? self->dependencies_.audio_service : &app.GetAudioService();
    if (esp_timer_get_time() / 1000 - self->last_push_frame_ms_ > 5000 && as->IsIdle()) {
        self->push_active_ = false;
        self->push_finishing_ = false;
        self->finish_ready_ms_ = 0;
        if (self->push_finish_timer_) esp_timer_stop(self->push_finish_timer_);
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        if (app.GetDeviceState() == kDeviceStateSpeaking) {
            app.SetDeviceState(kDeviceStateIdle);
        }
    }
}
void PushMessageController::FinishTimerCb(void* arg) {
    auto* self = static_cast<PushMessageController*>(arg);
    if (!self) return;
    auto& app = Application::GetInstance();
    if (!self->push_finishing_) {
        if (self->push_finish_timer_) esp_timer_stop(self->push_finish_timer_);
        return;
    }
    if (app.GetDeviceState() != kDeviceStateSpeaking) {
        self->push_finishing_ = false;
        self->finish_ready_ms_ = 0;
        if (self->push_finish_timer_) esp_timer_stop(self->push_finish_timer_);
        return;
    }
    auto* as = self->dependencies_.audio_service ? self->dependencies_.audio_service : &app.GetAudioService();
    if (!as->IsIdle()) {
        self->finish_ready_ms_ = 0;
        return;
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (self->finish_ready_ms_ == 0) {
        self->finish_ready_ms_ = now;
        return;
    }
    if (now - self->finish_ready_ms_ < 100) return;
    self->push_finishing_ = false;
    self->finish_ready_ms_ = 0;
    if (self->push_finish_timer_) esp_timer_stop(self->push_finish_timer_);
    if (app.GetDeviceState() == kDeviceStateSpeaking) {
        app.SetDeviceState(kDeviceStateIdle);
    }
}
