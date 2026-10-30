#pragma once
#include "maicord/app/gateway_voice_control.hpp"
#include "maicord/app/voice_connection.hpp"
#include "maicord/app/voice_transmit.hpp"
#include "maicord/media/opus_encoder.hpp"
#include "maicord/platform/voice_audio_capture.hpp"

#include "maicord/core/win32_records.hpp"

namespace maicord {
// Microphone -> voice server. The capture thread frames, meters, gates (voice
// activity or push-to-talk) and Opus-encodes 20 ms frames into a queue; the
// network worker drains it in order: Speaking (op 5), DAVE, RTP and the
// transport. While DAVE cannot encrypt (joining, recovery) frames are dropped.
inline constexpr u32 kVoiceSendQueue = 16;
inline constexpr u32 kVoiceSendDatagramBytes = kVoiceRtpHeaderBytes + kOpusEncoderMaximumPacketBytes + 32;
inline constexpr u32 kVoiceSpeakingMicrophone = 1;
struct VoiceSendPacket {
    u64 frame; // Capture frame index: the RTP timestamp is base + frame * 960.
    u16 bytes;
    VoiceTransmitAction action;
    u8 opus[kOpusEncoderMaximumPacketBytes];
};
// Worker -> capture thread, read once per frame under the shared lock.
struct VoiceSendControl {
    u16 gain_percent, release_ms;
    VoiceInputMode mode;
    u8 sensitivity, key;
    bool automatic, send;
};
struct VoiceSendStatusCells {
    volatile LONG level, threshold, transmitting, announced, round_trip;
};
struct VoiceSendShared;
// Lives in the account worker runtime. UI-thread entry points: configure and
// status. Everything else is worker-only.
struct VoiceSend {
    VoiceAudioCapture capture;
    VoiceSendShared* shared;
    SrwLock lock; // Guards settings/revision (UI writes) and capture_visible (status reads).
    VoiceAudioSettings settings;
    u32 revision, applied_revision;
    bool capture_visible;
    VoiceSendStatusCells cells;
    volatile LONG64 sent_packets;
    // Worker-owned.
    VoiceAudioSettings applied;
    bool capture_processing, speaking_on, speaking_off_pending, configured;
    wchar_t capture_device[kVoiceDeviceIdCharacters];
    u64 session;
    u32 timestamp_base;
    u16 sequence;
    u64 dropped_packets;
    NetworkLoop* loop;
    NetworkSignal* signal;
    u8 datagram[kVoiceSendDatagramBytes];
};
// Media sink for flush: production binds the voice connection; tests bind a
// loopback transport and a synthetic DAVE sealer. session changes (a new
// Ready) restart RTP sequence/timestamp at random values.
struct VoiceSendTransport {
    void* context;
    u64 session;
    u32 ssrc;
    bool (*ready)(void* context) noexcept;
    DaveSealResult (*seal)(void* context, ConstSlice<u8> opus, Slice<u8> output) noexcept;
    VoiceUdpMediaSend (*send)(void* context, ConstSlice<u8> packet) noexcept;
    VoiceSocketSend (*speaking)(void* context, u32 flags) noexcept;
};

// UI thread: the latest settings win; the worker applies them on its next drive.
void voice_send_configure(VoiceSend* send, const VoiceAudioSettings& settings) noexcept;
VoiceAudioStatus voice_send_status(VoiceSend* send) noexcept;

// Worker: starts/stops the microphone for the call state (in call, unmuted,
// or the settings monitor), applies settings and flushes queued frames.
// media_ready: the call's media transport is attached for the current Ready.
void voice_send_drive(VoiceSend* send, VoiceConnection* connection, const GatewayVoiceControl& voice,
                      bool media_ready, NetworkLoop* loop, NetworkSignal* signal) noexcept;
void voice_send_stop(VoiceSend* send) noexcept;
// Worker needs drive calls (capture running or wanted).
bool voice_send_active(const VoiceSend& send) noexcept;
// The local user is transmitting voice right now.
bool voice_send_speaking(const VoiceSend& send) noexcept;
// Sends queued frames in order until the transport is busy.
void voice_send_flush(VoiceSend* send, const VoiceSendTransport& transport) noexcept;

// Test seam: the capture pipeline without WASAPI. start allocates the shared
// block (encoder included); push/tick are the capture sink and tick.
bool voice_send_open(VoiceSend* send) noexcept;
void voice_send_close(VoiceSend* send) noexcept;
void voice_send_set_control(VoiceSend* send, const VoiceSendControl& control) noexcept;
void voice_send_push(void* send, ConstSlice<i16> mono, u64 now_us) noexcept;
u64 voice_send_tick(void* send, u64 now_us) noexcept;
// Overrides the push-to-talk key probe (GetAsyncKeyState) for tests.
void voice_send_set_key_probe(VoiceSend* send, bool (*probe)(void*, u8 key) noexcept, void* context) noexcept;
u32 voice_send_queued(const VoiceSend& send) noexcept;
} // namespace maicord
