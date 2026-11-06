// libpulse behind the Session and Track of pulse.hpp: the dlopen, the threaded mainloop, the contexts and streams.
#include "pulse.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/shared_library.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/core/test_hook.hpp"

namespace maicord::pulse {
namespace {

// 0: not tried, 1: loading, 2: loaded, 3: not available.
volatile LONG g_state;
SharedLibrary g_library;
Api g_api;

LONG load_state() noexcept {
    return InterlockedCompareExchange(&g_state, 0, 0);
}

void log_unavailable(const char* what, const char* name) noexcept {
    char message[160];
    u32 length = 0;
    const char* const parts[] = {what, name};
    for (const char* part : parts)
        for (; part && *part && length + 1 < sizeof(message); ++part)
            message[length++] = *part;
    message[length] = 0;
    log_write(LogLevel::warning, "audio", message);
}

bool load() noexcept {
    // A test can name another soname (the no-libpulse path); users and operators have MAICORD_LIBRARY_PATH.
    const char* soname = test_hook("MAICORD_PULSE_LIBRARY");
    if (!soname || !*soname)
        soname = "libpulse.so.0";
    if (!shared_library_open(&g_library, soname)) {
        log_unavailable("libpulse.so.0 is not installed: audio is unavailable", "");
        return false;
    }
    SharedLibraryResolver resolve{g_library, nullptr};
    Api api{};
#define MAICORD_PULSE_RESOLVE(ret, name, args) resolve(&api.name, #name);
    MAICORD_PULSE_FUNCTIONS(MAICORD_PULSE_RESOLVE)
#undef MAICORD_PULSE_RESOLVE
    if (const char* missing = resolve.missing) {
        log_unavailable("libpulse.so.0: no symbol ", missing);
        shared_library_close(&g_library);
        return false;
    }
    g_api = api;
    return true;
}

// The error to report for the last failed call on a context.
u32 context_error(const Session& session) noexcept {
    return session.context ? kErrorPulse | static_cast<u32>(g_api.pa_context_errno(session.context)) : kErrorPulse;
}

void set_error(u32* destination, u32 error) noexcept {
    if (destination)
        *destination = error;
}

// ---- callbacks: libpulse's mainloop thread; they record state and signal the worker, nothing else ----

void context_state_changed(Context* context, void* userdata) noexcept {
    auto* session = static_cast<Session*>(userdata);
    InterlockedExchange(&session->context_state, g_api.pa_context_get_state(context));
    event_set(session->event);
}

void subscription_changed(Context*, u32 event, u32, void* userdata) noexcept {
    auto* session = static_cast<Session*>(userdata);
    const u32 facility = event & kEventFacilityMask;
    const u32 type = event & (kEventTypeMask);
    LONG bits = 0;
    if (facility == kEventServer)
        bits = kAudioEndpointDefaultChanged;
    else if (type == kEventNew || type == kEventRemove) {
        // Changes of an existing device (volume, mute, port) are not topology and would only wake the worker.
        if ((facility == kEventSink && (session->watch_mask & kSubscribeSink)) ||
            (facility == kEventSource && (session->watch_mask & kSubscribeSource)))
            bits = kAudioEndpointTopologyChanged;
    }
    if (!bits)
        return;
    InterlockedOr(&session->dirty, bits);
    event_set(session->event);
}

void track_state_changed(Stream* stream, void* userdata) noexcept {
    auto* track = static_cast<Track*>(userdata);
    InterlockedExchange(&track->state, g_api.pa_stream_get_state(stream));
    event_set(track->session->event);
}

void track_requested(Stream*, usize, void* userdata) noexcept {
    event_set(static_cast<Track*>(userdata)->session->event);
}

void track_underflowed(Stream*, void* userdata) noexcept {
    InterlockedIncrement(&static_cast<Track*>(userdata)->underflows);
}

void track_drained(Stream*, i32, void* userdata) noexcept {
    auto* track = static_cast<Track*>(userdata);
    InterlockedExchange(&track->drained, 1);
    event_set(track->session->event);
}

// Waits for `done()` to be true on the session event, the stop waitable or the deadline; the caller's wait is the
// only consumer of the event.
template <typename Done> u32 wait_for(Waitable event, Waitable stop, u64 deadline_ms, Done done) noexcept {
    for (;;) {
        if (done())
            return 0;
        const u64 now = clock_monotonic_milliseconds();
        if (now >= deadline_ms)
            return kErrorServerTimeout;
        const u32 remaining = static_cast<u32>(minimum<u64>(deadline_ms - now, 0x7FFFFFFFu));
        Waitable waitables[2]{event, stop};
        const u32 waited = wait_any(waitables, stop ? 2u : 1u, remaining);
        if (waited == WAIT_OBJECT_0 + 1)
            return kErrorStopped;
        if (waited == WAIT_FAILED)
            return kErrorServerTimeout;
    }
}

} // namespace

const Api* api() noexcept {
    for (;;) {
        switch (load_state()) {
        case 2:
            return &g_api;
        case 3:
            return nullptr;
        case 1:
            thread_sleep(1);
            continue;
        default:
            if (InterlockedCompareExchange(&g_state, 1, 0) != 0)
                continue;
            InterlockedExchange(&g_state, load() ? 2 : 3);
        }
    }
}

Lock::Lock(const Session& owner) noexcept : session(owner) {
    g_api.pa_threaded_mainloop_lock(session.loop);
}

Lock::~Lock() {
    g_api.pa_threaded_mainloop_unlock(session.loop);
}

bool session_open(Session* session, Waitable event, const char* client_name, AudioFlow flow, bool watch,
                  Waitable stop, u32 timeout_ms, u32* native_error) noexcept {
    *session = {};
    session->event = event;
    session->watch_mask = flow == AudioFlow::render ? kSubscribeSink : kSubscribeSource;
    if (!api()) {
        set_error(native_error, kErrorLibraryMissing);
        return false;
    }
    session->loop = g_api.pa_threaded_mainloop_new();
    Proplist* properties = g_api.pa_proplist_new();
    if (session->loop && properties) {
        g_api.pa_proplist_sets(properties, "application.name", client_name);
        g_api.pa_proplist_sets(properties, "application.process.binary", "maicord");
        session->context = g_api.pa_context_new_with_proplist(g_api.pa_threaded_mainloop_get_api(session->loop),
                                                              client_name, properties);
    }
    if (properties)
        g_api.pa_proplist_free(properties);
    if (!session->context || g_api.pa_threaded_mainloop_start(session->loop) < 0) {
        set_error(native_error, ERROR_NOT_ENOUGH_MEMORY);
        session_close(session);
        return false;
    }
    bool connecting = false;
    {
        Lock lock(*session);
        g_api.pa_context_set_state_callback(session->context, context_state_changed, session);
        connecting = g_api.pa_context_connect(session->context, nullptr, kContextNoAutospawn, nullptr) >= 0;
    }
    if (!connecting) {
        u32 error = 0;
        {
            Lock lock(*session);
            error = context_error(*session);
        }
        set_error(native_error, error);
        session_close(session);
        return false;
    }
    const u64 deadline = clock_monotonic_milliseconds() + timeout_ms;
    const u32 waited = wait_for(event, stop, deadline, [&]() noexcept {
        const LONG state = InterlockedCompareExchange(&session->context_state, 0, 0);
        return state == kContextReady || state == kContextFailed || state == kContextTerminated;
    });
    const LONG state = InterlockedCompareExchange(&session->context_state, 0, 0);
    if (waited || state != kContextReady) {
        u32 error = waited;
        if (!error) {
            Lock lock(*session);
            error = context_error(*session);
        }
        set_error(native_error, error);
        session_close(session);
        return false;
    }
    if (watch) {
        Lock lock(*session);
        g_api.pa_context_set_subscribe_callback(session->context, subscription_changed, session);
        Operation* operation = g_api.pa_context_subscribe(
            session->context, session->watch_mask | kSubscribeServer, nullptr, nullptr);
        if (operation)
            g_api.pa_operation_unref(operation);
    }
    return true;
}

void session_close(Session* session) noexcept {
    if (!session->loop)
        return;
    if (session->context) {
        Lock lock(*session);
        g_api.pa_context_set_state_callback(session->context, nullptr, nullptr);
        g_api.pa_context_set_subscribe_callback(session->context, nullptr, nullptr);
        g_api.pa_context_disconnect(session->context);
        g_api.pa_context_unref(session->context);
        session->context = nullptr;
    }
    g_api.pa_threaded_mainloop_stop(session->loop);
    g_api.pa_threaded_mainloop_free(session->loop);
    session->loop = nullptr;
    InterlockedExchange(&session->context_state, kContextTerminated);
}

bool session_ready(const Session& session) noexcept {
    return session.loop && InterlockedCompareExchange(const_cast<volatile LONG*>(&session.context_state), 0, 0) ==
                               kContextReady;
}

LONG session_take_dirty(Session* session) noexcept {
    return InterlockedExchange(&session->dirty, 0);
}

namespace {

bool track_open(Session* session, Track* track, const TrackConfig& config, bool playback,
                u32* native_error) noexcept {
    *track = {};
    track->session = session;
    track->channels = config.spec.channels;
    InterlockedExchange(&track->state, kStreamCreating);
    Proplist* properties = g_api.pa_proplist_new();
    if (properties) {
        g_api.pa_proplist_sets(properties, "media.role", config.role);
        g_api.pa_proplist_sets(properties, "application.process.binary", "maicord");
    }
    Lock lock(*session);
    track->stream = g_api.pa_stream_new_with_proplist(session->context, config.name, &config.spec, nullptr, properties);
    if (properties)
        g_api.pa_proplist_free(properties);
    if (!track->stream) {
        set_error(native_error, context_error(*session));
        return false;
    }
    g_api.pa_stream_set_state_callback(track->stream, track_state_changed, track);
    if (playback) {
        g_api.pa_stream_set_write_callback(track->stream, track_requested, track);
        g_api.pa_stream_set_underflow_callback(track->stream, track_underflowed, track);
    } else {
        g_api.pa_stream_set_read_callback(track->stream, track_requested, track);
        g_api.pa_stream_set_overflow_callback(track->stream, track_underflowed, track);
    }
    CVolume volume{};
    const CVolume* initial = nullptr;
    if (playback && config.volume >= 0) {
        g_api.pa_cvolume_set(&volume, config.spec.channels, g_api.pa_sw_volume_from_linear(config.volume));
        initial = &volume;
    }
    const i32 connected = playback ? g_api.pa_stream_connect_playback(track->stream, config.device, &config.attributes,
                                                                      config.flags, initial, nullptr)
                                   : g_api.pa_stream_connect_record(track->stream, config.device, &config.attributes,
                                                                    config.flags);
    if (connected < 0) {
        set_error(native_error, context_error(*session));
        g_api.pa_stream_set_state_callback(track->stream, nullptr, nullptr);
        g_api.pa_stream_unref(track->stream);
        track->stream = nullptr;
        return false;
    }
    return true;
}

} // namespace

bool track_open_playback(Session* session, Track* track, const TrackConfig& config, u32* native_error) noexcept {
    return track_open(session, track, config, true, native_error);
}

bool track_open_record(Session* session, Track* track, const TrackConfig& config, u32* native_error) noexcept {
    return track_open(session, track, config, false, native_error);
}

bool track_wait_ready(Track* track, Waitable stop, u32 timeout_ms, u32* native_error) noexcept {
    const u64 deadline = clock_monotonic_milliseconds() + timeout_ms;
    const u32 waited = wait_for(track->session->event, stop, deadline, [&]() noexcept {
        const LONG state = InterlockedCompareExchange(&track->state, 0, 0);
        return state == kStreamReady || state == kStreamFailed || state == kStreamTerminated;
    });
    if (!waited && InterlockedCompareExchange(&track->state, 0, 0) == kStreamReady)
        return true;
    u32 error = waited;
    if (!error) {
        Lock lock(*track->session);
        error = context_error(*track->session);
    }
    set_error(native_error, error);
    return false;
}

bool track_alive(const Track& track) noexcept {
    const LONG state = InterlockedCompareExchange(const_cast<volatile LONG*>(&track.state), 0, 0);
    return track.stream && (state == kStreamCreating || state == kStreamReady);
}

void track_close(Track* track) noexcept {
    if (!track->stream)
        return;
    {
        Lock lock(*track->session);
        // A pending drain must not call back into a track that is closed (or reused for a later stream).
        if (track->drain_operation) {
            g_api.pa_operation_cancel(track->drain_operation);
            g_api.pa_operation_unref(track->drain_operation);
            track->drain_operation = nullptr;
        }
        g_api.pa_stream_set_state_callback(track->stream, nullptr, nullptr);
        g_api.pa_stream_set_write_callback(track->stream, nullptr, nullptr);
        g_api.pa_stream_set_read_callback(track->stream, nullptr, nullptr);
        g_api.pa_stream_set_underflow_callback(track->stream, nullptr, nullptr);
        g_api.pa_stream_set_overflow_callback(track->stream, nullptr, nullptr);
        if (track_alive(*track))
            g_api.pa_stream_disconnect(track->stream);
        g_api.pa_stream_unref(track->stream);
    }
    track->stream = nullptr;
    InterlockedExchange(&track->state, kStreamTerminated);
}

usize track_writable(Track* track) noexcept {
    Lock lock(*track->session);
    const usize bytes = g_api.pa_stream_writable_size(track->stream);
    return bytes == ~usize{0} ? 0 : bytes;
}

bool track_write(Track* track, const void* data, usize bytes) noexcept {
    Lock lock(*track->session);
    return g_api.pa_stream_write(track->stream, data, bytes, nullptr, 0, kSeekRelative) == 0;
}

bool track_peek(Track* track, const void** data, usize* bytes) noexcept {
    Lock lock(*track->session);
    *data = nullptr;
    *bytes = 0;
    return g_api.pa_stream_peek(track->stream, data, bytes) == 0;
}

void track_drop(Track* track) noexcept {
    Lock lock(*track->session);
    g_api.pa_stream_drop(track->stream);
}

bool track_cork(Track* track, bool paused) noexcept {
    Lock lock(*track->session);
    Operation* operation = g_api.pa_stream_cork(track->stream, paused ? 1 : 0, nullptr, nullptr);
    if (!operation)
        return false;
    g_api.pa_operation_unref(operation);
    return true;
}

bool track_time(Track* track, u64* microseconds) noexcept {
    Lock lock(*track->session);
    return g_api.pa_stream_get_time(track->stream, microseconds) == 0;
}

bool track_set_volume(Track* track, float linear) noexcept {
    Lock lock(*track->session);
    const u32 index = g_api.pa_stream_get_index(track->stream);
    if (index == kInvalid)
        return false;
    CVolume volume{};
    g_api.pa_cvolume_set(&volume, track->channels, g_api.pa_sw_volume_from_linear(linear));
    Operation* operation = g_api.pa_context_set_sink_input_volume(track->session->context, index, &volume, nullptr,
                                                                  nullptr);
    if (!operation)
        return false;
    g_api.pa_operation_unref(operation);
    return true;
}

bool track_drain(Track* track) noexcept {
    Lock lock(*track->session);
    if (track->drain_operation) {
        g_api.pa_operation_cancel(track->drain_operation);
        g_api.pa_operation_unref(track->drain_operation);
        track->drain_operation = nullptr;
    }
    InterlockedExchange(&track->drained, 0);
    track->drain_operation = g_api.pa_stream_drain(track->stream, track_drained, track);
    return track->drain_operation != nullptr;
}

bool track_buffer_attributes(Track* track, BufferAttr* granted) noexcept {
    Lock lock(*track->session);
    const BufferAttr* attributes = g_api.pa_stream_get_buffer_attr(track->stream);
    if (!attributes)
        return false;
    *granted = *attributes;
    return true;
}

namespace {

struct ListState {
    DeviceVisitor visitor;
    void* context;
    char* default_name;
    u32 default_capacity;
    Waitable event;
    volatile LONG stage; // 0 waiting for the server info, 1 waiting for the list, 2 done
    AudioFlow flow;
};

void copy_name(char* destination, u32 capacity, const char* name) noexcept {
    u32 length = 0;
    for (; name && name[length] && length + 1 < capacity; ++length)
        destination[length] = name[length];
    if (capacity)
        destination[length] = 0;
}

void server_info(Context*, const ServerInfo* info, void* userdata) noexcept {
    auto* list = static_cast<ListState*>(userdata);
    if (info && list->default_name && list->default_capacity)
        copy_name(list->default_name, list->default_capacity,
                  list->flow == AudioFlow::render ? info->default_sink_name : info->default_source_name);
    InterlockedExchange(&list->stage, 1);
    event_set(list->event);
}

void sink_info(Context*, const SinkInfo* info, i32 end_of_list, void* userdata) noexcept {
    auto* list = static_cast<ListState*>(userdata);
    if (end_of_list) {
        InterlockedExchange(&list->stage, 2);
        event_set(list->event);
        return;
    }
    if (info && info->name && list->visitor)
        list->visitor(list->context, {info->name, info->description ? info->description : info->name, false});
}

void source_info(Context*, const SourceInfo* info, i32 end_of_list, void* userdata) noexcept {
    auto* list = static_cast<ListState*>(userdata);
    if (end_of_list) {
        InterlockedExchange(&list->stage, 2);
        event_set(list->event);
        return;
    }
    if (info && info->name && list->visitor)
        list->visitor(list->context, {info->name, info->description ? info->description : info->name,
                                      info->monitor_of_sink != kInvalid});
}

} // namespace

bool device_list(Session* session, AudioFlow flow, Waitable stop, u32 timeout_ms, DeviceVisitor visitor, void* context,
                 char* default_name, u32 default_capacity) noexcept {
    ListState list{visitor, context, default_name, default_capacity, session->event, 0, flow};
    if (default_name && default_capacity)
        default_name[0] = 0;
    const u64 deadline = clock_monotonic_milliseconds() + timeout_ms;
    Operation* operation = nullptr;
    // An operation still pending when this returns would call back into `list`, which is on this stack: cancel it.
    auto finish = [&](bool completed) noexcept {
        if (operation) {
            Lock lock(*session);
            g_api.pa_operation_cancel(operation);
            g_api.pa_operation_unref(operation);
            operation = nullptr;
        }
        return completed;
    };
    {
        Lock lock(*session);
        operation = g_api.pa_context_get_server_info(session->context, server_info, &list);
    }
    if (!operation)
        return false;
    if (wait_for(session->event, stop, deadline, [&]() noexcept { return InterlockedCompareExchange(&list.stage, 0, 0) >= 1; }))
        return finish(false);
    finish(true);
    {
        Lock lock(*session);
        operation = flow == AudioFlow::render
                        ? g_api.pa_context_get_sink_info_list(session->context, sink_info, &list)
                        : g_api.pa_context_get_source_info_list(session->context, source_info, &list);
    }
    if (!operation)
        return false;
    // The visitor runs on the mainloop thread, so it needs no lock of its own; the wait ends after the last entry.
    const bool listed = wait_for(session->event, stop, deadline, [&]() noexcept {
                            return InterlockedCompareExchange(&list.stage, 0, 0) >= 2;
                        }) == 0;
    return finish(listed);
}

namespace {

struct Lookup {
    const char* name;
    bool found;
};

void match(void* context, const DeviceInfo& device) noexcept {
    auto* lookup = static_cast<Lookup*>(context);
    u32 i = 0;
    while (lookup->name[i] && lookup->name[i] == device.name[i])
        ++i;
    if (!lookup->name[i] && !device.name[i])
        lookup->found = true;
}

} // namespace

bool device_exists(Session* session, AudioFlow flow, const char* name, Waitable stop) noexcept {
    Lookup lookup{name, false};
    return name && *name && device_list(session, flow, stop, 3000, match, &lookup, nullptr, 0) && lookup.found;
}

bool device_name_from_id(const wchar_t* id, char* name, u32 capacity) noexcept {
    u32 length = 0;
    for (; id && id[length]; ++length) {
        if (length + 1 >= capacity || id[length] >= 0x80)
            return false;
        name[length] = static_cast<char>(id[length]);
    }
    if (!capacity)
        return false;
    name[length] = 0;
    return true;
}

bool device_id_from_name(const char* name, wchar_t* id, u32 capacity) noexcept {
    u32 length = 0;
    for (; name && name[length]; ++length) {
        if (length + 1 >= capacity || static_cast<u8>(name[length]) >= 0x80)
            return false;
        id[length] = static_cast<wchar_t>(name[length]);
    }
    if (!capacity)
        return false;
    id[length] = 0;
    return true;
}

} // namespace maicord::pulse
