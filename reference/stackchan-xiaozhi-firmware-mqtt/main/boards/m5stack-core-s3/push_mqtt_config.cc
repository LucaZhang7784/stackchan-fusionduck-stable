#include "push_mqtt_config.h"

#include <string.h>
#include <mbedtls/md.h>

const char* const kPushMqttUris[] = {"mqtt://broker-cn.emqx.io:1883"};
const size_t kPushMqttUriCount = sizeof(kPushMqttUris) / sizeof(kPushMqttUris[0]);

PushMqttTopicBuilder::PushMqttTopicBuilder(const std::string& mac)
    : base_("stackchan/" + mac + "/") {}
std::string PushMqttTopicBuilder::Base(const char* suffix) const { return base_ + suffix; }
std::string PushMqttTopicBuilder::Push() const { return Base("push"); }
std::string PushMqttTopicBuilder::Ack() const { return Base("ack"); }
std::string PushMqttTopicBuilder::Status() const { return Base("status"); }
std::string PushMqttTopicBuilder::Heartbeat() const { return Base("heartbeat"); }
std::string PushMqttTopicBuilder::Diag() const { return Base("diag"); }
std::string PushMqttTopicBuilder::Photo() const { return Base("photo"); }
std::string PushMqttTopicBuilder::Confirm() const { return Base("confirm"); }
std::string PushMqttTopicBuilder::Control() const { return Base("control"); }
std::string PushMqttTopicBuilder::ControlAck() const { return Base("control_ack"); }

bool VerifyPushControlHmac(const uint8_t* packet, size_t body_len,
                           const uint8_t* expected, size_t expected_len,
                           const char* secret) {
    if (!packet || !expected || !secret || expected_len != 16) return false;
    uint8_t digest[32] = {};
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md_hmac(md, reinterpret_cast<const unsigned char*>(secret), strlen(secret),
                               packet, body_len, digest) != 0) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < expected_len; ++i) {
        diff |= digest[i] ^ expected[i];
    }
    return diff == 0;
}
