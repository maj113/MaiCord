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
struct Workspace {
    float ring[8192 * 2], pcm[kAacDecoderMaximumOutputBytes / 4];
    Mp4AudioDemuxer demux{};
    Mp4AudioSampleWindow window{};
    Mp4AudioTimeline timeline{};
    u32 packet_index{}, packet_count{}, channels{}, frames_per_packet{};
    AacDecoder decoder{};
    MediaAudio audio{};
    u64 source_frames{}, queued_frames{}, wanted_frames{}, silence_frames{};
    u32 pending_offset{}, pending_frames{}, input_retries{}, preceding_frames{};
    bool reading{}, draining{}, decoded_end{}, sealed{}, requested_play{}, seek_pending{};
};

struct OpusWorkspace {
    float ring[8192 * 2], pcm[kOpusDecoderMaximumFrames * 2];
    i16 pcm16[kOpusDecoderMaximumFrames * 2];
    u8 packet[kOggOpusMaximumPacketBytes];
    OggOpusDemuxer demux{};
    OpusDecoder decoder{};
    MediaAudio audio{};
    u64 first_frame{}, end_frame{}, source_frames{}, queued_frames{}, wanted_frames{};
    u32 pending_offset{}, pending_frames{}, allocation_bytes{};
    bool decoded_end{}, sealed{}, requested_play{};
};

struct PcmWorkspace {
    float ring[8192 * 2], pcm[kMediaAudioMaximumWriteFrames * 2];
    MediaAudio audio{};
    u64 first_frame{}, source_frames{}, queued_frames{}, wanted_frames{};
    u32 pending_offset{}, pending_frames{};
    bool decoded_end{}, sealed{}, requested_play{};
};

struct Mp3Workspace {
    PcmWorkspace pcm{};
    Mp3Stream demux{};
    Mp3SeekPoint points[kMp3DefaultSeekPoints]{};
    Mp3SeekIndex index{};
    Mp3Decoder decoder{};
    u32 allocation_bytes{};
};

PcmWorkspace& pcm_owner(AccountVideoSession& s) noexcept;
const PcmWorkspace& pcm_owner(const AccountVideoSession& s) noexcept;
bool pcm_backed(const AccountVideoSession& s) noexcept;
bool standalone(const AccountVideoSession& s) noexcept;
MediaAudio* audio_owner(AccountVideoSession& s) noexcept;
const MediaAudio* audio_owner(const AccountVideoSession& s) noexcept;
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
