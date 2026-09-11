#include "push_packet_parser.h"

#include <algorithm>
#include <string.h>

#include "push_mqtt_config.h"

namespace {
constexpr size_t kControlBodyBytes = 20;
constexpr size_t kControlPacketBytes = 36;

const uint8_t* FindNul(const uint8_t* data, size_t length) {
    return static_cast<const uint8_t*>(memchr(data, 0, length));
}
}  // namespace

bool PushPacketParser::ParsePush(ByteView input, ParsedPushPacket* output) {
    if (output == nullptr || input.data == nullptr || input.size == 0) return false;
    *output = {};
    switch (input.data[0]) {
    case 1: {
        output->kind = PushPacketKind::kStart;
        const uint8_t* body = input.data + 1;
        const size_t body_size = input.size - 1;
        const uint8_t* first = FindNul(body, body_size);
        if (first == nullptr) {
            output->text = {body, body_size};  // Legacy text-only START.
            return true;
        }
        output->msg_uid = {body, static_cast<size_t>(first - body)};
        const uint8_t* rest = first + 1;
        const size_t rest_size = body_size - output->msg_uid.size - 1;
        const uint8_t* second = FindNul(rest, rest_size);
        if (second == nullptr) {
            output->text = {rest, rest_size};  // Legacy uid + text START.
            return true;
        }
        output->action = {rest, static_cast<size_t>(second - rest)};
        output->text = {second + 1, rest_size - output->action.size - 1};
        return true;
    }
    case 2: {
        if (input.size < 2) return false;
        output->kind = PushPacketKind::kPcmBatch;
        const size_t available = input.size - 2;
        const size_t requested = input.data[1];
        const size_t complete = std::min(requested, available / kPcmFrameBytes);
        output->frame_count = static_cast<uint8_t>(complete);
        output->pcm = input.data + 2;
        output->pcm_bytes = complete * kPcmFrameBytes;
        output->pcm_truncated = requested != complete;
        return true;
    }
    case 3:
        output->kind = PushPacketKind::kStop;
        return true;
    case 4:
        output->kind = PushPacketKind::kSnap;
        return true;
    default:
        return false;
    }
}

bool PushPacketParser::ParseControl(ByteView input, const char* hmac_secret,
                                    ParsedPushPacket* output) {
    if (output == nullptr || input.data == nullptr || input.size != kControlPacketBytes ||
        input.data[0] != 5 || input.data[1] != 1 || input.data[2] != 1 ||
        !VerifyPushControlHmac(input.data, kControlBodyBytes, input.data + kControlBodyBytes,
                               16, hmac_secret)) {
        return false;
    }
    *output = {};
    output->kind = PushPacketKind::kVolumeControl;
    output->volume = std::min<uint8_t>(input.data[11], 100);
    memcpy(output->request_id, input.data + 3, sizeof(output->request_id));
    return true;
}
