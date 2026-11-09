// Screen and window capture on Linux (include/maicord/platform/capture/screen_capture.hpp): the xdg-desktop-portal
// ScreenCast interface over a session-bus connection of the capture thread's own, then PipeWire (pipewire.cpp).
//
// CreateSession, SelectSources (the kinds asked for, one source, the cursor drawn in or hidden where the portal lists
// that mode: version 2), Start (the portal shows its own picker; a cancel ends the capture as denied), then
// OpenPipeWireRemote gives the fd the stream's node is reached through. The portal session lives as long as that
// connection; Session.Closed (the user stopped sharing from the desktop) ends the capture as closed.
#include "maicord/platform/capture/screen_capture.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/core/time.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/platform/dbus/bus.hpp"
#include "maicord/platform/dbus/portal.hpp"
#include "pipewire_stream.hpp"

namespace maicord {
namespace {
using namespace dbus;

constexpr const char* kPortalName = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char* kScreenCast = "org.freedesktop.portal.ScreenCast";
constexpr u32 kCallTimeoutMs = 10'000;
constexpr u32 kCursorHidden = 1, kCursorEmbedded = 2;
constexpr u32 kResponseFailed = 2, kResponseStopped = 3;
// How long the stream may take to reach the portal's node (link and negotiation) before the capture counts as failed.
constexpr u64 kLinkTimeoutMs = 10'000;

enum class Step : u8 { create, select, start };

struct Runtime {
    CaptureOptions options;
    CaptureSink sink;
    Thread* thread;
    Waitable stop, stream_ended;
    char parent[128];
    Connection* connection;
    PortalRequest request;
    Step step;
    bool responded, session_closed;
    u32 code;
    char session[200];
    u32 node;
    pipewire::Consumer* consumer;
    u64 link_deadline_ms;
};

u64 now_ms() noexcept {
    return clock_monotonic_milliseconds();
}

// Serves the connection until `*done`, the stop event, `also`, the connection's end or `abandon`; true for `*done`.
bool serve_until(Runtime& r, const volatile bool* done, Waitable also = nullptr,
                 bool (*abandon)(const Runtime&) noexcept = nullptr) noexcept {
    while (!*done) {
        Waitable waitables[]{r.stop, connection_waitable(r.connection), also};
        const u32 timeout = minimum(connection_wait_ms(r.connection, now_ms()), 250u);
        const u32 waited = wait_any(waitables, also ? 3u : 2u, timeout);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_FAILED || waited == WAIT_OBJECT_0 + 2)
            return false;
        connection_process(r.connection, now_ms(), waited == WAIT_OBJECT_0 + 1);
        if (!connection_alive(r.connection) || (abandon && abandon(r)))
            return false;
    }
    return true;
}

// Request.Response's results for the step in flight: the session handle, or the first stream's node.
void on_response(void* context, u32 code, Reader* results) noexcept {
    auto& r = *static_cast<Runtime*>(context);
    r.code = code;
    r.responded = true;
    if (code != 0 || !reader_enter_array(results))
        return;
    while (reader_more(*results)) {
        const char* key = nullptr;
        const char* signature = nullptr;
        if (!reader_enter_struct(results) || !reader_string(results, &key, nullptr) ||
            !reader_enter_variant(results, &signature))
            break;
        if (r.step == Step::create && c_string_equal(key, "session_handle") && c_string_equal(signature, "s")) {
            const char* handle = nullptr;
            u32 length = 0;
            if (reader_string(results, &handle, &length) && length < sizeof(r.session))
                copy_bytes(r.session, handle, length + 1);
        } else if (r.step == Step::start && c_string_equal(key, "streams") && reader_enter_array(results)) {
            // a(ua{sv}): the node id of each stream; one source was asked for.
            if (reader_more(*results) && reader_enter_struct(results)) {
                reader_u32(results, &r.node);
                reader_leave(results);
            }
            reader_leave(results);
        }
        if (!reader_leave(results) || !reader_leave(results))
            break;
    }
}

void on_session_closed(void* context, const Message&) noexcept {
    static_cast<Runtime*>(context)->session_closed = true;
}

// Starts a request, letting `options` add to the call's a{sv} after the handle token. Returns the response code,
// kResponseFailed when the call failed, kResponseStopped when stop came first.
template <class Options>
u32 request(Runtime& r, Step step, const char* member, bool with_session, const char* text, Options options) noexcept {
    r.step = step;
    r.responded = false;
    r.code = kResponseFailed;
    if (!portal_request_begin(&r.request, r.connection, on_response, &r))
        return kResponseFailed;
    Builder body;
    builder_init(&body);
    if (with_session)
        builder_object_path(&body, r.session);
    if (text)
        builder_string(&body, text);
    builder_open_array(&body, "{sv}");
    portal_request_append_token(&body, r.request);
    options(&body);
    builder_close_array(&body);
    Message reply{};
    const Status status =
        connection_call_sync(r.connection, {kPortalName, kPortalPath, kScreenCast, member}, &body, kCallTimeoutMs, &reply);
    builder_release(&body);
    if (status != Status::ok) {
        if (status == Status::error_reply) {
            log_write(LogLevel::warning, "capture", reply.error_name ? reply.error_name : "portal_error");
            message_release(&reply);
        }
        portal_request_end(&r.request);
        return kResponseFailed;
    }
    Reader reader;
    message_reader(reply, &reader);
    const char* handle = nullptr;
    if (reader_string(&reader, &handle, nullptr))
        portal_request_adopt(&r.request, handle);
    message_release(&reply);
    if (!serve_until(r, &r.responded)) {
        portal_request_close(&r.request);
        portal_request_end(&r.request);
        return kResponseStopped;
    }
    return r.code;
}

// A u property of the ScreenCast interface; 0 when it is missing.
u32 screencast_property(Connection* connection, const char* name) noexcept {
    Builder body;
    builder_init(&body);
    builder_string(&body, kScreenCast);
    builder_string(&body, name);
    Message reply{};
    const Status status = connection_call_sync(
        connection, {kPortalName, kPortalPath, "org.freedesktop.DBus.Properties", "Get"}, &body, kCallTimeoutMs, &reply);
    builder_release(&body);
    u32 value = 0;
    if (status == Status::ok) {
        Reader reader;
        message_reader(reply, &reader);
        const char* signature = nullptr;
        if (reader_enter_variant(&reader, &signature) && c_string_equal(signature, "u"))
            reader_u32(&reader, &value);
    }
    if (status == Status::ok || status == Status::error_reply)
        message_release(&reply);
    return value;
}

// OpenPipeWireRemote: the fd of the PipeWire remote the session's streams are reached through, owned by the caller.
int open_remote(Runtime& r) noexcept {
    Builder body;
    builder_init(&body);
    builder_object_path(&body, r.session);
    builder_open_array(&body, "{sv}");
    builder_close_array(&body);
    Message reply{};
    const Status status = connection_call_sync(r.connection, {kPortalName, kPortalPath, kScreenCast, "OpenPipeWireRemote"},
                                               &body, kCallTimeoutMs, &reply);
    builder_release(&body);
    int fd = -1;
    if (status == Status::ok) {
        Reader reader;
        message_reader(reply, &reader);
        if (reader_unix_fd(&reader, &fd))
            for (u32 i = 0; i < reply.fd_count; ++i)
                if (reply.fds[i] == fd)
                    reply.fds[i] = -1; // ours now: message_release must not close it
    }
    if (status == Status::ok || status == Status::error_reply)
        message_release(&reply);
    return fd;
}

void close_session(Runtime& r) noexcept {
    if (r.session[0])
        connection_call(r.connection, {kPortalName, r.session, "org.freedesktop.portal.Session", "Close"}, nullptr, 0,
                        nullptr, nullptr);
}

CaptureEnd run(Runtime& r) noexcept {
    r.connection = connection_open_session();
    if (!r.connection)
        return CaptureEnd::failed;
    const u32 version = screencast_property(r.connection, "version");
    if (!version) {
        log_write(LogLevel::warning, "capture", "screencast_portal_missing");
        return CaptureEnd::unsupported;
    }
    u32 code = request(r, Step::create, "CreateSession", false, nullptr, [&](Builder* options) noexcept {
        builder_dict_string(options, "session_handle_token", r.request.token);
    });
    if (code == kResponseStopped)
        return CaptureEnd::stopped;
    if (code != 0 || !r.session[0])
        return code == 1 ? CaptureEnd::denied : CaptureEnd::failed;
    const SignalMatch closed{kPortalName, r.session, "org.freedesktop.portal.Session", "Closed", nullptr};
    connection_subscribe(r.connection, closed, on_session_closed, &r);

    const u32 available_types = screencast_property(r.connection, "AvailableSourceTypes");
    const u32 wanted_types = (r.options.kinds & (1u << static_cast<u8>(CaptureSourceKind::screen)) ? 1u : 0u) |
                             (r.options.kinds & (1u << static_cast<u8>(CaptureSourceKind::window)) ? 2u : 0u);
    const u32 types = available_types ? (wanted_types ? wanted_types : 3u) & available_types : 0u;
    const u32 cursor_modes = version >= 2 ? screencast_property(r.connection, "AvailableCursorModes") : 0;
    const u32 cursor_mode = r.options.cursor && (cursor_modes & kCursorEmbedded) ? kCursorEmbedded
                            : (cursor_modes & kCursorHidden)                      ? kCursorHidden
                                                                                  : 0;
    code = request(r, Step::select, "SelectSources", true, nullptr, [&](Builder* options) noexcept {
        if (types) {
            builder_open_dict_entry(options, "types", "u");
            builder_u32(options, types);
            builder_close_dict_entry(options);
        }
        builder_dict_bool(options, "multiple", false);
        if (cursor_mode) {
            builder_open_dict_entry(options, "cursor_mode", "u");
            builder_u32(options, cursor_mode);
            builder_close_dict_entry(options);
        }
    });
    if (code == kResponseStopped)
        return CaptureEnd::stopped;
    if (code != 0)
        return code == 1 ? CaptureEnd::denied : CaptureEnd::failed;
    code = request(r, Step::start, "Start", true, r.parent, [](Builder*) noexcept {});
    if (code == kResponseStopped)
        return CaptureEnd::stopped;
    if (code != 0)
        return code == 1 ? CaptureEnd::denied : CaptureEnd::failed;
    if (!r.node)
        return CaptureEnd::failed;
    const int fd = open_remote(r);
    if (fd < 0)
        return CaptureEnd::failed;
    // The cursor is in the pixels when the portal draws it, or when it could not be asked to hide it.
    const bool cursor = cursor_mode == kCursorEmbedded || (!cursor_mode && r.options.cursor);
    r.consumer = pipewire::consumer_open(fd, r.node, cursor, r.options.max_fps, r.sink, r.stream_ended);
    if (!r.consumer)
        return CaptureEnd::failed;
    r.link_deadline_ms = now_ms() + kLinkTimeoutMs;
    const auto unlinked = [](const Runtime& runtime) noexcept {
        return !pipewire::consumer_linked(runtime.consumer) && now_ms() >= runtime.link_deadline_ms;
    };
    const bool stopped =
        !serve_until(r, &r.session_closed, r.stream_ended, unlinked) && wait_one(r.stop, 0) == WAIT_OBJECT_0;
    const pipewire::StreamEnd end = pipewire::consumer_end(r.consumer);
    pipewire::consumer_close(r.consumer);
    r.consumer = nullptr;
    if (stopped)
        return CaptureEnd::stopped;
    if (r.session_closed || end == pipewire::StreamEnd::closed)
        return CaptureEnd::closed;
    return end == pipewire::StreamEnd::unsupported ? CaptureEnd::unsupported : CaptureEnd::failed;
}

u32 capture_thread(void* parameter) noexcept {
    auto& r = *static_cast<Runtime*>(parameter);
    const CaptureEnd end = run(r);
    if (r.connection) {
        close_session(r);
        connection_process(r.connection, now_ms(), false); // flushes the Close
        connection_close(r.connection);
        r.connection = nullptr;
    }
    if (end != CaptureEnd::stopped && wait_one(r.stop, 0) != WAIT_OBJECT_0)
        r.sink.ended(r.sink.context, end);
    return 0;
}

void free_runtime(Runtime* r) noexcept {
    if (r->thread)
        thread_close(r->thread);
    if (r->stop)
        waitable_close(r->stop);
    if (r->stream_ended)
        waitable_close(r->stream_ended);
    pages_release(r, sizeof(Runtime));
}

} // namespace

CaptureSupport capture_support() noexcept {
    static i32 cached = -1;
    if (cached < 0)
        cached = pipewire::api() && dbus::portal_interface_version(kScreenCast, 500) ? 1 : 0;
    return {cached == 1, cached == 1, true, cached == 1};
}

u32 capture_sources(Slice<CaptureSource>) noexcept {
    return 0;
}

bool capture_thumbnail(GpuDevice*, const CaptureSource&, u32, u32, u8*, u32*, u32*) noexcept {
    return false;
}

ScreenCapture* capture_start(const CaptureOptions& options, const CaptureSink& sink) noexcept {
    if (!sink.frame || !sink.ended || !pipewire::api())
        return nullptr;
    auto* r = commit_pages<Runtime>();
    if (!r)
        return nullptr;
    copy_value(&r->options, options);
    copy_value(&r->sink, sink);
    const char* parent = dbus::portal_parent_window();
    copy_bytes(r->parent, parent, minimum(c_string_length(parent), static_cast<u32>(sizeof(r->parent) - 1)));
    r->stop = event_create(true, false);
    r->stream_ended = event_create(true, false);
    if (r->stop && r->stream_ended)
        r->thread = thread_start(capture_thread, r, 256 * 1024, nullptr);
    if (!r->thread) {
        free_runtime(r);
        return nullptr;
    }
    return reinterpret_cast<ScreenCapture*>(r);
}

void capture_stop(ScreenCapture* capture) noexcept {
    auto* r = reinterpret_cast<Runtime*>(capture);
    if (!r)
        return;
    event_set(r->stop);
    thread_wait(r->thread, INFINITE);
    free_runtime(r);
}

} // namespace maicord
