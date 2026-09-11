#pragma once

#include <stddef.h>
#include <stdint.h>

struct ByteView {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

enum class PushPacketKind : uint8_t {
    kInvalid,
    kStart,
    kPcmBatch,
    kStop,
    kSnap,
    kVolumeControl,
};

struct ParsedPushPacket {
    PushPacketKind kind = PushPacketKind::kInvalid;
    ByteView msg_uid;
    ByteView action;
    ByteView text;
    const uint8_t* pcm = nullptr;
    size_t pcm_bytes = 0;
    uint8_t frame_count = 0;
    bool pcm_truncated = false;
    uint8_t volume = 0;
    uint8_t request_id[4] = {};
};

class PushPacketParser {
public:
    static constexpr size_t kPcmFrameBytes = 960;

    static bool ParsePush(ByteView input, ParsedPushPacket* output);
    static bool ParseControl(ByteView input, const char* hmac_secret,
                             ParsedPushPacket* output);
};
