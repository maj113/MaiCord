#pragma once

#include "maicord/core/base.hpp"
#include "maicord/os/linux/error.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/platform/audio/endpoint_watch.hpp"
#include "maicord/platform/audio/pulse/abi.hpp"

// The Linux audio transport over libpulse (PulseAudio, pipewire-pulse, WSLg), shared by the voice output and
// capture, the media player and the notification sounds.
//
// One Session is one connection (a libpulse threaded mainloop and context) owned by one audio worker thread.
// libpulse runs its callbacks on the mainloop's own thread; they only record state and signal the session's wake
// Waitable (the "event"), the way WASAPI event callbacks wake a render thread, so every worker is the same loop
// as on Windows: wait for stop, command and event, then fill or drain under the mainloop lock.
//
// Nothing here fails hard when there is no sound server: api() is null without libpulse.so.0, session_open
// reports why it could not connect, and the workers keep their "unavailable" state and retry.
namespace maicord::pulse {

// native_error values of the audio status fields: libpulse's own error is kErrorPulse | PA_ERR_*.
inline constexpr u32 kErrorPulse = kErrorNative | 0x1000u;
// libpulse.so.0 is not installed or lacks a function this build needs.
inline constexpr u32 kErrorLibraryMissing = kErrorPulse | 0xFF0u;
// A wait for the server passed its limit.
inline constexpr u32 kErrorServerTimeout = kErrorPulse | 0xFF1u;
// The caller's stop event fired during a wait.
inline constexpr u32 kErrorStopped = kErrorPulse | 0xFF2u;

// libpulse, loaded on first use from any thread. Null when it is not installed (the reason is logged once).
const Api* api() noexcept;

struct Session {
    ThreadedMainloop* loop;
    Context* context;
    // The worker's wake Waitable, borrowed: signaled by every callback below.
    Waitable event;
    // PA_CONTEXT_* as of the last state callback; readable without the lock.
    volatile LONG context_state;
    // kAudioEndpoint* bits accumulated by the subscription until session_take_dirty.
    volatile LONG dirty;
    // Which facilities of the subscription count (kSubscribeSink, kSubscribeSource) besides the server's default.
    u32 watch_mask;
};

// Connects to the sound server (never spawning one) and waits until the context is ready, the stop waitable
// (may be null) is signaled or timeout_ms passes. With `watch` the session subscribes to the device list of
// `flow` and to default-device changes. False with *native_error set; the session is then closed.
bool session_open(Session* session, Waitable event, const char* client_name, AudioFlow flow, bool watch, Waitable stop,
                  u32 timeout_ms, u32* native_error) noexcept;
// Disconnects and frees everything; every Track of the session must be closed first. Safe on a closed session.
void session_close(Session* session) noexcept;
// True while the context is connected and ready.
bool session_ready(const Session& session) noexcept;
// The change bits since the last call.
LONG session_take_dirty(Session* session) noexcept;

// Holds the mainloop lock for a scope.
struct Lock {
    explicit Lock(const Session& session) noexcept;
    ~Lock();
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    const Session& session;
};

// One stream of a Session. state is PA_STREAM_* as of the last state callback.
struct Track {
    Session* session;
    Stream* stream;
    // Channels of the stream's sample spec (a volume request needs it).
    u8 channels;
    volatile LONG state;
    // 1 once a drain requested with track_drain has played out.
    volatile LONG drained;
    // The pending drain, cancelled by track_close.
    Operation* drain_operation;
};

struct TrackConfig {
    // The stream's description in mixers ("MaiCord voice") and its media role ("phone", "music", "event").
    const char* name;
    const char* role;
    SampleSpec spec;
    // A sink or source name; null selects the server's default and lets the server follow default changes.
    const char* device;
    BufferAttr attributes;
    u32 flags;
    // Initial linear volume (1 = unity) of a playback stream, applied so the server's stream-restore database
    // cannot override it; negative leaves the server's choice.
    float volume;
};

bool track_open_playback(Session* session, Track* track, const TrackConfig& config, u32* native_error) noexcept;
bool track_open_record(Session* session, Track* track, const TrackConfig& config, u32* native_error) noexcept;
// Waits until the stream is ready (true), failed (false), stop was signaled or timeout_ms passed.
bool track_wait_ready(Track* track, Waitable stop, u32 timeout_ms, u32* native_error) noexcept;
// Connecting or ready.
bool track_alive(const Track& track) noexcept;
void track_close(Track* track) noexcept;

// Bytes the server asks for now.
usize track_writable(Track* track) noexcept;
bool track_write(Track* track, const void* data, usize bytes) noexcept;
// The next captured fragment: *data is null for a hole of *bytes. Every successful peek with *bytes != 0 is
// followed by track_drop.
bool track_peek(Track* track, const void** data, usize* bytes) noexcept;
void track_drop(Track* track) noexcept;
bool track_cork(Track* track, bool paused) noexcept;
// The audible playback position in microseconds since the stream started; false until the server has reported
// timing. The position does not advance while the stream is corked or has nothing to play.
bool track_time(Track* track, u64* microseconds) noexcept;
// Sets the stream's volume (linear, 1 = unity); false when the server did not accept the request.
bool track_set_volume(Track* track, float linear) noexcept;
// Asks the server to play out everything written so far; track.drained becomes 1 when it has. Some sinks (a PulseAudio
// null sink) never answer, so a caller that must finish gives it a grace period (timing.hpp: kDrainGraceMs).
bool track_drain(Track* track) noexcept;
// The buffer attributes the server granted the stream (it may adjust the requested ones); false before the stream is
// ready.
bool track_buffer_attributes(Track* track, BufferAttr* granted) noexcept;

// One entry of the server's device list: a sink for AudioFlow::render, a source for capture.
struct DeviceInfo {
    const char* name;        // The ID: stable, ASCII.
    const char* description; // UTF-8, for people.
    bool monitor;            // A source that records a sink's output.
};
using DeviceVisitor = void (*)(void* context, const DeviceInfo& device) noexcept;
// Asks the server for the default device name and then the list, calling `visitor` for each device on libpulse's
// mainloop thread (it must not block; the call returns after the last entry, so the caller reads what the visitor
// stored without a lock). `default_name` receives the default device's name (empty when there is none). The session
// must have been opened with the event the call waits on (device_list consumes it); the waits end at the stop
// waitable (may be null) or timeout_ms for the whole call. False on failure, timeout or stop.
bool device_list(Session* session, AudioFlow flow, Waitable stop, u32 timeout_ms, DeviceVisitor visitor,
                 void* context, char* default_name, u32 default_capacity) noexcept;
// True when `name` is a device of the flow the server knows (a lookup through device_list).
bool device_exists(Session* session, AudioFlow flow, const char* name, Waitable stop) noexcept;

// UTF-16 endpoint ID <-> UTF-8 device name. The IDs of PulseAudio devices are ASCII; false for anything else or
// when it does not fit.
bool device_name_from_id(const wchar_t* id, char* name, u32 capacity) noexcept;
bool device_id_from_name(const char* name, wchar_t* id, u32 capacity) noexcept;

} // namespace maicord::pulse
