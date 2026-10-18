#pragma once
#include "maicord/app/gateway_voice_control.hpp"
#include "maicord/app/voice_connection.hpp"
#include "maicord/app/voice_playback.hpp"
#include "maicord/app/voice_transmit.hpp"
#include "maicord/app/voice_video_sink.hpp"
#include "maicord/app/voice_audio_settings.hpp"
#include "maicord/platform/voice_audio_output.hpp"

namespace maicord {
inline constexpr u32 kVoiceReceiveDatagramBytes = 2048;
enum class VoiceReceiveResult : u8 {
    queued,
    ignored, // RTCP, non-Opus payload, non-audio source or no playback.
    lost,    // DAVE not ready/rejected or jitter refusal: the gap is concealed.
    invalid,
    video // Handed to the video sink (RTP core; DAVE runs per assembled frame).
};
// DAVE stage for one transport-opened RTP payload. Production binds
// voice_connection_dave_decrypt; tests substitute a passthrough session.
using VoiceReceiveDecrypt = DaveMediaResult (*)(void* context, const VoiceReceivedPacket& packet,
                                                ConstSlice<u8> frame, Slice<u8> scratch,
                                                Slice<u8> output) noexcept;
struct VoiceReceiveStats {
    u64 lost, queued;
};
// Worker-owned receive path: transport plaintext -> RTP payload -> DAVE ->
// per-SSRC jitter buffer; an audio thread renders the mix to WASAPI. The UDP
// buffers are lent to the connection's media transport until it drains, so
// this object must live as long as the connection.
struct VoiceReceive {
    VoicePlayback playback;
    VoiceVideoSink video; // Camera/screen sources; frames go to video.frames.
    VoiceAudioOutput output;
    VoiceReceiveStats stats;
    VoiceConnection* connection;
    u64 media_revision, speaking_deadline_ms;
    wchar_t output_device[kVoiceDeviceIdCharacters]; // Endpoint the running output opened.
    // One datagram slice per concurrently posted read (video keyframe bursts).
    u8 receive_buffer[kVoiceUdpMediaReceiveSlots * kVoiceReceiveDatagramBytes];
    u8 send_buffer[kVoiceReceiveDatagramBytes];
    u8 dave_scratch[kVoiceReceiveDatagramBytes];
    u8 dave_output[kVoiceReceiveDatagramBytes];
};
VoiceReceiveResult voice_receive_packet(VoiceReceive* receive, const VoiceReceivedPacket& packet,
                                        ConstSlice<u8> plaintext, VoiceReceiveDecrypt decrypt, void* context,
                                        u64 now_us) noexcept;
// After voice_connection_drive: attaches the media transport once Session
// Description keys are current, runs audio output while the call lives and
// applies self/server deafen.
void voice_receive_drive(VoiceReceive* receive, VoiceConnection* connection, const GatewayVoiceControl& voice,
                         u64 now_ms) noexcept;
// Joins the audio thread and frees playback memory. Transport buffers stay
// owned by this object.
void voice_receive_stop(VoiceReceive* receive) noexcept;
// Sorted speaking users; also schedules the wake-up when the set next changes.
u32 voice_receive_speaking(VoiceReceive* receive, Slice<u64> users, u64 now_ms) noexcept;
u64 voice_receive_deadline(const VoiceReceive& receive) noexcept;
bool voice_receive_set_user(VoiceReceive* receive, u64 user_id, u32 gain_percent, bool muted) noexcept;
// Output endpoint and volume; a changed endpoint reopens running output.
void voice_receive_configure(VoiceReceive* receive, const VoiceAudioSettings& settings) noexcept;
} // namespace maicord
