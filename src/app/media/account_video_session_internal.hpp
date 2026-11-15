#pragma once
// Shared by account_video_session.cpp and its parts (account_video_session_*.cpp); private to them.
#include "maicord/app/media/account_video_session.hpp"
#include "maicord/platform/decode/aac_decoder.hpp"
#include "maicord/platform/audio/media_audio.hpp"
#include "maicord/media/audio/opus_decoder.hpp"
#include "maicord/media/audio/encoded_audio.hpp"


namespace maicord {
namespace account_video_session_detail {}
using namespace account_video_session_detail;
namespace account_video_session_detail {

constexpr i64 kLimit = 0x7fffffffffffffffll;
// The decoder of a source whose packets are Opus, in the place of the AAC decoder. One packet goes in and its frames
// come out, as from the AAC decoder. It lives behind the Workspace, in the same pages.
struct OpusPackets {
    OpusDecoder decoder;
    i16 pcm16[kAacDecoderMaximumOutputBytes / 4];
    i64 time;
    bool pending;
};
// What every workspace starts with: the ring the transport plays from, the transport, and the PCM the session
// queued and still holds. The session reaches them the same way for every codec.
struct AudioQueue {
    float ring[8192 * 2];
    MediaAudio audio{};
    u64 source_frames{}, queued_frames{}, wanted_frames{};
    u32 pending_offset{}, pending_frames{}, allocation_bytes{};
    bool decoded_end{}, sealed{}, requested_play{};
};
struct Workspace : AudioQueue {
    float pcm[kAacDecoderMaximumOutputBytes / 4];
    Mp4AudioDemuxer demux{};
    Mp4AudioSampleWindow window{};
    Mp4AudioTimeline timeline{};
    u32 packet_index{}, packet_count{}, channels{}, frames_per_packet{};
    AacDecoder decoder{};
    u64 silence_frames{};
    u32 input_retries{}, preceding_frames{};
    bool reading{}, draining{}, seek_pending{};
    // Null for an AAC source.
    OpusPackets* opus{};
};
inline constexpr usize kWorkspaceOpusOffset = (sizeof(Workspace) + 63) & ~usize{63};

struct OpusWorkspace : AudioQueue {
    float pcm[kOpusDecoderMaximumFrames * 2];
    i16 pcm16[kOpusDecoderMaximumFrames * 2];
    u8 packet[kOggOpusMaximumPacketBytes];
    OggOpusDemuxer demux{};
    OpusDecoder decoder{};
    u64 first_frame{}, end_frame{};
};

struct PcmWorkspace : AudioQueue {
    float pcm[kMediaAudioMaximumWriteFrames * 2];
    u64 first_frame{};
};

struct Mp3Workspace : PcmWorkspace {
    Mp3Stream demux{};
    Mp3SeekPoint points[kMp3DefaultSeekPoints]{};
    Mp3SeekIndex index{};
    Mp3Decoder decoder{};
};

// Short enough that a call costs more than the test: inline in both session units.
inline bool pcm_backed(const AccountVideoSession& s) noexcept {
    return s.status.kind == AccountPlaybackKind::wave_pcm || s.status.kind == AccountPlaybackKind::mp3;
}
inline bool standalone(const AccountVideoSession& s) noexcept {
    return s.status.kind != AccountPlaybackKind::mp4;
}
inline MediaAudio* audio_owner(const AccountVideoSession& s) noexcept {
    return s.workspace ? &s.workspace->audio : nullptr;
}
bool thread(const AccountVideoSession& s) noexcept;
AccountVideoResult result(AccountVideoError error, u32 native = 0, bool admitted = false,
                          bool completed = false) noexcept;
AccountVideoResult refused(AccountVideoSession* s, const AccountVideoIdentity& id, u64 serial,
                           AccountVideoError error, u32 native = 0) noexcept;
void terminal(AccountVideoSession& s, AccountVideoError error, u32 native = 0) noexcept;
u32 delay(i64 duration) noexcept;
bool qpc(u64* now) noexcept;
bool data_pending(const AccountVideoSession& s) noexcept;
void set_pending(AccountVideoSession& s, bool* flag, bool value) noexcept;
bool live_window(AccountVideoSession& s) noexcept;
bool workspace(AccountVideoSession& s) noexcept;
bool drop_frame(AccountVideoSession& s) noexcept;
bool request_audio_state(AccountVideoSession& s) noexcept;
// Picture decoder only: seeks to the IDR at or before target; data_pending leaves seek_pending.
bool video_seek(AccountVideoSession& s, i64 target) noexcept;
bool reset(AccountVideoSession& s, i64 target) noexcept;
void refresh(AccountVideoSession& s) noexcept;
bool audio_service(AccountVideoSession& s, bool (*cancelled)(void*) noexcept, void* context,
                   bool* interrupted) noexcept;
bool pcm_service(AccountVideoSession& s, bool (*cancelled)(void*) noexcept, void* context,
                 bool* interrupted) noexcept;
bool opus_service(AccountVideoSession& s, bool (*cancelled)(void*) noexcept, void* context,
                  bool* interrupted) noexcept;

} // namespace account_video_session_detail
} // namespace maicord
