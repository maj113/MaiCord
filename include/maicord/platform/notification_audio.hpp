#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/core/win32_types.hpp"
#if defined(MAICORD_NOTIFICATION_AUDIO_FIXTURE)
#include <mmdeviceapi.h>
#endif

namespace maicord {
inline constexpr u32 kNotificationAudioRate=44100;
inline constexpr u32 kNotificationAudioAlternateRate=48000;
inline constexpr u32 kNotificationAudioQueueCapacity=32;
inline constexpr u32 kNotificationAudioVoiceCapacity=64;
// Storage maximum covers the higher native rate; clip validation still limits
// each descriptor to ten seconds at its own supported source rate.
inline constexpr u32 kNotificationAudioMixChunkFrames=512;
// Borrowed, read-only signed 16-bit interleaved stereo at 44100 or 48000 Hz. count is the
// sample count, twice the frame count. The bank/context must outlive a successful worker release.
struct NotificationAudioClip {ConstSlice<i16> samples{};u32 sample_rate{kNotificationAudioRate};};
using NotificationAudioResolve=bool(*)(void*,u32 clip_id,NotificationAudioClip*) noexcept;
using NotificationAudioGain=bool(*)(void*,float nominal_gain,float* effective_gain) noexcept;
struct NotificationAudioRequest {
    u64 epoch{};
    u32 clip_id{};
    float gain{};
    // Optional predecoded clip. Otherwise the clip is resolved on the audio worker only.
    NotificationAudioClip clip{};
};
enum class NotificationAudioError : u8 {
    none,invalid_input,stale_epoch,full_queue,full_voices,memory,event,thread,
    stopped,join_pending,wait,resolve,device,stream,timeline_exhausted
};
struct NotificationAudioResult {
    NotificationAudioError error{};u32 native_error{};
    bool ok() const noexcept {return error==NotificationAudioError::none;}
};
struct NotificationAudioConfig {
    NotificationAudioResolve resolve{};void* context{};
    HWND wake_window{};u32 wake_message{};
    u64 initial_epoch{1};bool enabled{true};
    // Called on the worker only, after the clip is resolved and loaded. The voice
    // gain is fixed from then on. Null uses request.gain directly. Uses the same
    // borrowed context, with the same lifetime, as resolve.
    NotificationAudioGain gain{};
};
struct NotificationAudioStatus {
    u64 desired_epoch{},observed_epoch{},accepted{},completed{},canceled{},failed{},switches{},device_open_attempts{},stream_open_attempts{};
    u32 queued{},outstanding{},active{},native_error{},wake_error{};
    // Last preparation/transport failure, kept until the next epoch.
    NotificationAudioError error{};
    bool enabled{},worker_started{},stream_open{},stopping{},worker_exited{};
};
struct NotificationAudio {
    NotificationAudioConfig config{};void* runtime{};
    u64 epoch{};bool initialized{},enabled{};
};
// Call on the UI thread only. Only records the configuration; no allocation, worker, COM or
// endpoint open happens until a request is accepted. The resolver runs on the worker,
// must not block for long and must not call back into this object. PCM is never copied.
NotificationAudioResult notification_audio_initialize(NotificationAudio*,const NotificationAudioConfig&) noexcept;
// Hint for producer backpressure; submit checks again atomically. Counts queued and
// active voices (max 64) and queue headers (max 32). needed=2 supports dual routes.
bool notification_audio_can_accept(const NotificationAudio*,u32 needed=1) noexcept;
NotificationAudioResult notification_audio_submit(NotificationAudio*,const NotificationAudioRequest&) noexcept;
// Every mute/account cancellation needs a new, larger, nonzero epoch.
// Clears queued descriptors immediately and wakes the worker to stop and reset its
// buffered stream before it accepts the new epoch. Returning from reset does not
// mean the PCM is no longer in use.
NotificationAudioResult notification_audio_reset(NotificationAudio*,u64 new_epoch,bool enabled) noexcept;
NotificationAudioStatus notification_audio_status(const NotificationAudio*) noexcept;
// Status changes coalesce into one posted WM_APP. Call status after that wake;
// it clears the coalescing bit. No polling or timer.
// A successful release/join means the bank/context is no longer in use. On timeout or
// error the runtime, thread and handles stay attached and release can be retried.
// Never calls TerminateThread; reset/submit are rejected once stopping begins.
NotificationAudioResult notification_audio_release(NotificationAudio*,u32 timeout_ms) noexcept;
// Counts the attached thread/event handles, including a callback's duplicate while
// attached. A disarmed external COM callback can keep its own duplicate handle and
// endpoint ID; neither refers to this object or the PCM.
u32 notification_audio_owned_handles(const NotificationAudio*) noexcept;

// Offline source-rate mixer. No allocation/COM/device/thread.
// The caller serializes these helpers. Voice descriptors stay valid even after
// submission until played_rate() confirms padding consumption; this permits rewind
// on a default-device switch. Padding does not give the exact audible position.
struct NotificationAudioVoice {NotificationAudioClip clip{};u64 start{};float gain{};};
struct NotificationAudioMixer {
    NotificationAudioVoice voices[kNotificationAudioVoiceCapacity]{};
    // submitted/consumed count 44100 Hz frames; the _48000 pair is the separate
    // 48000 Hz frame timeline. All voices share one limit of 64 descriptors.
    u64 epoch{},submitted{},consumed{},submitted_48000{},consumed_48000{};u32 count{};
};
struct NotificationAudioMixResult {NotificationAudioError error{};u32 frames{};bool nonzero{};};
bool notification_audio_clip_valid(NotificationAudioClip) noexcept;
bool notification_audio_gain_valid(float) noexcept;
// Offline helpers shared with the worker/callback; no owner/device operations.
// owned_id is a terminated string with owned_chars characters. The event ID is
// OS-provided and terminated; comparison reads at most that length plus the
// terminator and treats endpoint IDs as opaque, case-sensitive values.
bool notification_audio_endpoint_matches(const wchar_t* event_id,const wchar_t* owned_id,usize owned_chars) noexcept;
// The caller serializes status access. A late Stop/Reset failure from an old stream
// must not change the current epoch's status. Returns true only for the current epoch.
bool notification_audio_record_stream_failure(NotificationAudioStatus*,u64 closed_epoch,u32 native_error) noexcept;
// Worker preparation (or tests with offline callbacks). Resolves the clip first,
// then reads the effective gain. Never call asset callbacks on the UI thread.
NotificationAudioResult notification_audio_prepare_request(const NotificationAudioConfig&,
    const NotificationAudioRequest&,NotificationAudioClip*,float*) noexcept;
void notification_audio_mixer_reset(NotificationAudioMixer*,u64 epoch) noexcept;
NotificationAudioResult notification_audio_mixer_add(NotificationAudioMixer*,u64 epoch,NotificationAudioClip,float gain) noexcept;
// Mixes at most 512 frames per call, summing gain and saturating. Output must not
// alias the mixer or any voice PCM. Invalid input changes nothing.
// No software interpolation/resampling: WASAPI handles hardware conversion.
u32 notification_audio_mixer_voices_rate(const NotificationAudioMixer&,u32 sample_rate) noexcept;
u32 notification_audio_mixer_needed_rate(const NotificationAudioMixer&,u32 sample_rate) noexcept;
NotificationAudioMixResult notification_audio_mixer_render_rate(NotificationAudioMixer*,Slice<i16>,u32 sample_rate) noexcept;
// padding is source-format frames, per IAudioClient documentation.
// Returns the number of finished voices; invalid/backwards padding leaves state unchanged.
u32 notification_audio_mixer_played_rate(NotificationAudioMixer*,u32 padding,u32 sample_rate) noexcept;
void notification_audio_mixer_rewind_rate(NotificationAudioMixer*,u32 sample_rate) noexcept;
#if defined(MAICORD_NOTIFICATION_AUDIO_FIXTURE)
// Test builds only. Install the hooks before the audio object is created; keep the
// hooks/context unchanged until every worker has joined and the callback reference is released.
// With no hook installed, COM creation fails; a real endpoint is never opened.
struct NotificationAudioFixtureHooks {
    void* context{};
    HRESULT(*initialize)(void*) noexcept;
    void(*uninitialize)(void*) noexcept;
    HRESULT(*create)(void*,IMMDeviceEnumerator**) noexcept;
    BOOL(*post_wake)(void*,HWND,u32) noexcept;
    void(*before_wait)(void*,u32 handle_count) noexcept;
};
void notification_audio_fixture_hooks(const NotificationAudioFixtureHooks*) noexcept;
// For inspection only, while the caller still holds a fixture callback reference.
HANDLE notification_audio_fixture_callback_event(IMMNotificationClient*) noexcept;
#endif
} // namespace maicord
