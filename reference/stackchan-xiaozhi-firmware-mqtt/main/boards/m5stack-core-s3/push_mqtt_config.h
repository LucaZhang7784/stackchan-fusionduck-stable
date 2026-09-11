#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

class PushMqttTopicBuilder {
public:
    explicit PushMqttTopicBuilder(const std::string& mac);
    std::string Push() const;
    std::string Ack() const;
    std::string Status() const;
    std::string Heartbeat() const;
    std::string Diag() const;
    std::string Photo() const;
    std::string Confirm() const;
    std::string Control() const;
    std::string ControlAck() const;
private:
    std::string Base(const char* suffix) const;
    std::string base_;
};

extern const char* const kPushMqttUris[];
extern const size_t kPushMqttUriCount;

bool VerifyPushControlHmac(const uint8_t* packet, size_t body_len,
                           const uint8_t* expected, size_t expected_len,
                           const char* secret);
