#pragma once
// The capture pipeline's shared block, which send.cpp allocates behind VoiceSend::shared; the send test
// reads its packet queue indices.
#include "maicord/app/voice/send.hpp"
#include "maicord/core/win32_records.hpp"

namespace maicord {
struct VoiceSendShared {
    SrwLock control_lock;
    VoiceSendControl control;
    bool (*probe)(void*, u8) noexcept;
    void* probe_context;
    volatile LONG write, read, end_lost;
    VoiceSendPacket packets[kVoiceSendQueue];
    OpusEncoder encoder;
    VoiceFramer framer;
    VoiceActivity activity;
    VoicePushToTalk push_to_talk;
    VoiceTransmitter transmitter;
    u64 wake_us;
    i16 frame[kVoiceFrameSamples];
};
} // namespace maicord
