#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {
inline constexpr u32 kMediaAudioMinimumRingFrames=2048;
inline constexpr u32 kMediaAudioMaximumRingFrames=16384;
inline constexpr u32 kMediaAudioMaximumWriteFrames=2048;
enum class MediaAudioError:u8 {none,invalid_input,stale_epoch,capacity,memory,event,thread,device,stream,clock,stopped,join_pending,wait,release,timeline};
struct MediaAudioResult {MediaAudioError error{};u32 native_error{};bool ok()const noexcept{return error==MediaAudioError::none;}};
// Ring storage belongs to the caller: aligned, interleaved, finite float32 at the
// native source rate, mono or stereo. The caller must serialize the pure helpers
// below; the live transport guards its ring with a short metadata/copy lock.
struct MediaAudioRing {
    Slice<float> samples{};u64 epoch{},written{},submitted{},consumed{};
    u32 channels{},sample_rate{};bool ended{};
};
bool media_audio_ring_initialize(MediaAudioRing*,Slice<float>,u32 rate,u32 channels,u64 epoch) noexcept;
u32 media_audio_ring_free(const MediaAudioRing&) noexcept;
MediaAudioResult media_audio_ring_write(MediaAudioRing*,u64 epoch,ConstSlice<float>) noexcept;
// Copies up to capacity available frames, applying validated gain. Advances submitted
// only; the data stays in the ring until confirmed native padding consumption.
u32 media_audio_ring_render(MediaAudioRing*,Slice<float>,float gain) noexcept;
// True when padding and confirmed consumption both show the submitted native
// queue is empty, even if the producer has written later, unsubmitted PCM.
bool media_audio_ring_native_queue_empty(const MediaAudioRing&,u32 padding) noexcept;
void media_audio_ring_rewind(MediaAudioRing*) noexcept;
bool media_audio_gain_valid(float) noexcept;
// Overflow-checked conversion of clock units to source time; no clock unit is
// assumed to be a sample. GetPosition and GetFrequency share native units.
bool media_audio_clock_time(u64 position,u64 frequency,u64* time_100ns) noexcept;
// Converts native clock units straight to source frames with one floor, not via 100 ns.
// This keeps the sample endpoint correct even when its 100 ns time is fractional.
// Products and sums are overflow-checked; invalid input/overflow leaves output unchanged.
bool media_audio_clock_frames(u64 position,u64 frequency,u32 source_rate,u64* frames) noexcept;
struct MediaAudioConfig {
    Slice<float> pcm{};u32 sample_rate{},channels{};u64 initial_epoch{1};
    i64 source_origin_100ns{};float volume{1.0f};HWND wake_window{};u32 wake_message{};
    // Borrowed decode-worker event. Set only when rendering consumes data and frees
    // refill room. The caller resets and coalesces it until release.
    HANDLE refill_event{};
    // Optional borrowed event signaled on state transitions, not on each hardware
    // clock tick. The caller resets it until release.
    HANDLE status_event{};
};
struct MediaAudioStatus {
    u64 desired_epoch{},observed_epoch{},written_frames{},submitted_frames{},consumed_frames{};
    i64 presentation_time_100ns{};u64 clock_frequency{};
    u32 free_frames{},owned_handles{},native_error{},wake_error{};MediaAudioError error{};
    float desired_volume{},applied_volume{};bool volume_valid{};
    bool playing{},paused{},buffering{},ended{},stream_open{},worker_started{},worker_exited{},clock_valid{};
};
struct MediaAudio {MediaAudioConfig config{};MediaAudioRing ring{};void* runtime{};u64 epoch{};bool initialized{};};
// Only records the configuration: no allocation, COM, endpoint, worker or ring write.
MediaAudioResult media_audio_initialize(MediaAudio*,const MediaAudioConfig&) noexcept;
// Playback starts one render worker. Pause/volume/reset only change the
// requested state; the status reports what the native stream has actually done.
MediaAudioResult media_audio_play(MediaAudio*) noexcept;
MediaAudioResult media_audio_pause(MediaAudio*) noexcept;
MediaAudioResult media_audio_volume(MediaAudio*,float) noexcept;
// Write/end may run on one decode producer once play has created the
// runtime. Controls stay on the owning thread. Before play, the caller serializes
// metadata writes; before release, stop and join the producer, then the
// renderer. Never race creating/freeing owner.runtime with producer calls.
MediaAudioResult media_audio_write(MediaAudio*,u64 epoch,ConstSlice<float>) noexcept;
MediaAudioResult media_audio_end(MediaAudio*,u64 epoch) noexcept;
// The epoch must increase on every reset. The old buffered stream is stopped and
// reset before the new epoch is observed. Returning from reset does not mean the
// borrowed ring is no longer in use.
MediaAudioResult media_audio_reset(MediaAudio*,u64 epoch,i64 origin_100ns) noexcept;
MediaAudioStatus media_audio_status(const MediaAudio*) noexcept;
// A successful join means the PCM/context is no longer in use. On timeout or handle
// failure the runtime stays attached. No idle polling, forced termination or device open in default tests.
MediaAudioResult media_audio_release(MediaAudio*,u32 timeout_ms) noexcept;
} // namespace maicord
