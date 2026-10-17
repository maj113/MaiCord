#pragma once
#include "maicord/platform/mp4_decoder.hpp"
#include "maicord/media/mp4_audio_demux.hpp"
#include "maicord/media/ogg_opus.hpp"

#include "maicord/core/deadline.hpp"
#include "maicord/core/win32_types.hpp"
#if defined(MAICORD_ACCOUNT_VIDEO_SESSION_PROBE)
#include "maicord/platform/media_audio.hpp"
#endif

namespace maicord {
struct MediaAudioStatus;
struct EncodedAudioMetadata;
struct AccountVideoIdentity {
    u64 account_serial{}, message_id{}, content_identity{}, session_epoch{};
    u32 message_revision{};
    u16 cache_generation{}, reserved{};
};
enum class AccountVideoError : u8 {
    none,
    invalid_input,
    stale,
    busy,
    memory,
    unsupported_audio,
    demux,
    decoder,
    timestamp,
    audio,
    event,
    clock,
    release
};
enum class AccountVideoCommand : u8 {
    play,
    pause,
    seek,
    volume,
    mute,
    stop
};
enum class AccountPlaybackKind : u8 {
    mp4,
    ogg_opus,
    mp3,
    wave_pcm
};
struct AccountVideoResult {
    AccountVideoError error{};
    u32 native_error{};
    bool admitted{}, completed{};
    bool ok() const noexcept {
        return error == AccountVideoError::none;
    }
};
struct AccountVideoStatus {
    AccountVideoIdentity identity{};
    u64 desired_command{}, observed_command{}, playback_epoch{}, frame_serial{};
    u64 rejected_command{};
    u32 rejected_native_error{};
    AccountVideoError rejected_error{};
    i64 position_100ns{}, duration_100ns{};
    u32 native_error{};
    AccountVideoError error{};
    float desired_volume{1}, applied_volume{};
    bool has_audio{}, playing{}, paused{true}, seeking{}, buffering{}, ended{}, muted{}, volume_valid{},
        frame_pending{}, clock_valid{}, release_pending{};
    AccountPlaybackKind kind{};
};
static_assert(sizeof(AccountVideoStatus) == 136);
struct AccountVideoService {
    Mp4DecodedFrame frame{};
    u64 playback_epoch{}, frame_serial{};
    u32 next_service_delay_ms{kInfiniteWaitMilliseconds};
    bool frame_ready{}, needs_service{}, interrupted{};
    // A progressive source has not delivered the next sample; wake on its arrival.
    bool data_pending{};
};
enum class AccountVideoFrameDue : u8 {
    invalid,
    expired,
    future,
    ready
};
struct AccountVideoAudioObservation {
    bool epoch_ready{}, playing{}, paused{}, clock_valid{}, volume_valid{}, ended{};
};
struct AccountVideoAudioProgress {
    u64 epoch{}, written_frames{}, submitted_frames{}, consumed_frames{};
};
AccountVideoAudioObservation account_video_audio_observation(const MediaAudioStatus&,
                                                             u64 playback_epoch) noexcept;
// A copied target frame requires acknowledgement. A future picture or video
// EOF has no copy to acknowledge and can make the audio seek ready directly.
bool account_video_audio_should_play(bool desired_play, bool seeking, bool video_seek_ready) noexcept;
// Native MFT may derive a block stamp by adding separately floored preceding
// stamp and PCM duration. Accept that decomposition, at most one tick
// below floor-of-sum; never substitute a timestamp or accumulate native drift.
bool account_video_audio_timestamp_matches(i64 native_timestamp, i64 exact_timestamp,
                                           i64 preceding_exact_timestamp, u32 preceding_frames,
                                           u32 sample_rate) noexcept;
// Native may round a fractional source time up or down. Both bounds must be
// supplied by the demuxer's original rational mapping, not a guessed offset.
bool account_video_audio_timestamp_within_bounds(i64 native_timestamp,
                                                 const Mp4AudioFrameTimeBounds&) noexcept;
AccountVideoFrameDue account_video_frame_due(i64 pts_100ns, u64 duration_100ns,
                                             i64 position_100ns) noexcept;
bool account_video_qpc_position(i64 origin_100ns, u64 anchor, u64 now, u64 frequency,
                                i64* position_100ns) noexcept;
// Codec worker only: runs on the caller's MTA thread with serialized
// commands, service and ack. The MP4 decoder or encoded audio source and the
// worker's event are borrowed until joined release. All formats share one
// command queue, audio transport and worker. The root validates
// account/message/cache identity before queueing and after the status/frame
// copy; this object rejects mismatched command/ack IDs.
struct AccountVideoSession {
    Mp4Decoder* video{};
    void* workspace{};
    HANDLE lane_event{};
    Mp4AudioDemuxer audio_metadata{};
    AccountVideoStatus status{};
    Mp4DecodedFrame held_frame{};
    u64 qpc_frequency{}, qpc_anchor{}, frame_counter{}, playback_epoch{1};
    i64 qpc_origin{}, seek_target{};
    u32 thread_id{};
    bool initialized{}, desired_play{}, held{}, published{}, video_eof{}, video_seek_ready{}, seeking{},
        audio_eof{};
    AccountVideoCommand command_kind{};
    bool video_started{}, audio_epoch_ready{};
    const EncodedAudioMetadata* encoded_metadata{};
    // Progressive MP4: a track waits for bytes (the clock holds), or a seek
    // waits for its IDR payload before the decoder cursor moves.
    bool video_pending{}, audio_pending{}, seek_pending{};
    i64 pending_seek_us{};
    // Optional worker-stop check for demux seek scans. Context is
    // borrowed until joined release; command/service cancellation stays separate.
    OggOpusCancelled source_cancelled{};
    void* source_cancel_context{};
};
inline bool account_video_identity_equal(const AccountVideoIdentity& a,
                                         const AccountVideoIdentity& b) noexcept {
    return a.account_serial && a.message_id && a.content_identity && a.session_epoch &&
           a.cache_generation && !a.reserved && b.account_serial && b.message_id && b.content_identity &&
           b.session_epoch && b.cache_generation && !b.reserved && a.account_serial == b.account_serial &&
           a.message_id == b.message_id && a.content_identity == b.content_identity &&
           a.session_epoch == b.session_epoch && a.message_revision == b.message_revision &&
           a.cache_generation == b.cache_generation;
}
// Metadata-only: no PCM workspace/MF/device/thread. Present malformed or
// unsupported AAC is an error, never silently classified as audio-less.
// A progressive decoder source needs the audio track's own availability reader.
AccountVideoResult account_video_session_initialize(AccountVideoSession*, Mp4Decoder*,
                                                    const AccountVideoIdentity&, HANDLE lane_event,
                                                    Mp4SourceAvailability audio_availability = {}) noexcept;
// Borrowed, validated source. Metadata only, no H.264, GPU or endpoint.
// The encoded source/demux must outlive joined session release.
AccountVideoResult account_video_session_initialize_audio(AccountVideoSession*, const EncodedAudioMetadata*,
                                                          const AccountVideoIdentity&,
                                                          HANDLE lane_event) noexcept;
// Commands are accepted separately from their completion. Seek requires
// the prior GPU copy acknowledgement; no old-frame lease is revoked.
AccountVideoResult account_video_session_control(AccountVideoSession*, const AccountVideoIdentity&,
                                                 u64 command_serial, AccountVideoCommand,
                                                 i64 seek_time_100ns = 0, float volume = 1,
                                                 bool muted = false) noexcept;
// Each call decodes at most 8 AAC/Opus packets or 4 video pictures and checks cancellation
// between native calls. Missing native PTS is terminal; no invented priming.
AccountVideoService account_video_session_service(AccountVideoSession*,
                                                  bool (*cancelled)(void*) noexcept = nullptr,
                                                  void* context = nullptr) noexcept;
bool account_video_session_acknowledge(AccountVideoSession*, const AccountVideoIdentity&,
                                       u64 playback_epoch, u64 frame_serial) noexcept;
AccountVideoStatus account_video_session_status(const AccountVideoSession*) noexcept;
// Codec worker snapshot only, no borrowed renderer pointer. Counters are returned
// only after the native transport observes this session's playback epoch.
AccountVideoAudioProgress account_video_session_audio_progress(const AccountVideoSession*) noexcept;
// The producer is the caller's worker, so it is already idle during release.
// Timeout/close failure keeps the workspace, AAC decoder and borrowed source/event.
AccountVideoResult account_video_session_release(AccountVideoSession*, u32 timeout_ms) noexcept;
u32 account_video_session_owned_bytes(const AccountVideoSession*) noexcept;
#if defined(MAICORD_ACCOUNT_VIDEO_SESSION_PROBE)
// Opt-in QA builds only; no cost in shipping builds.
struct AccountVideoTimestampDiagnostic {
    u64 source_frames{}, window_first{}, window_end{}, outputs{};
    i64 native_timestamp{}, expected_timestamp{};
    u32 block_frames{}, packet_index{}, packet_count{}, error_site{};
    bool stamped{}, mapped{};
};
AccountVideoTimestampDiagnostic account_video_session_timestamp_diagnostic(
    const AccountVideoSession*) noexcept;
struct AccountVideoProbeSnapshot {
    AccountVideoStatus session{};
    MediaAudioStatus audio{};
    u64 source_frames{}, queued_frames{}, wanted_frames{};
    u32 pending_frames{};
    bool desired_play{}, video_eof{}, held{}, published{}, seeking{}, audio_epoch_ready{};
    bool reading{}, draining{}, decoded_end{}, sealed{}, requested_play{};
};
AccountVideoProbeSnapshot account_video_session_probe_snapshot(const AccountVideoSession*) noexcept;
#endif
} // namespace maicord
