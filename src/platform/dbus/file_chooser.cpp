#include "maicord/core/ascii.hpp"
#include "maicord/core/heap.hpp"
#include "maicord/core/log.hpp"
#include "maicord/platform/dbus/bus.hpp"
#include "maicord/platform/dbus/file_chooser.hpp"
#include "maicord/platform/dbus/portal.hpp"
#include "maicord/platform/dbus/session.hpp"

namespace maicord::dbus {

struct FileChooser {
    PortalRequest request;
    FileChooserState state;
    bool reply_pending;  // the method's reply has not come: the handler still holds this pointer
    bool released;       // released while the reply was pending: freed when it arrives
    u32 count;
    void (*settled)(void* context) noexcept;
    void* settled_context;
    char paths[kFileChooserMaximumFiles][kFileChooserPathBytes];
};

namespace {

constexpr const char* kPortalName = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char* kInterface = "org.freedesktop.portal.FileChooser";
constexpr u32 kReplyTimeoutMs = 30000;

void release_memory(FileChooser* chooser) noexcept {
    portal_request_end(&chooser->request);
    heap_free(chooser);
}

// The Response's results: {uris: as, ...}. The reader is at the `a{sv}`.
void read_uris(FileChooser* chooser, Reader* r) noexcept {
    if (!reader_enter_array(r)) {
        chooser->state = FileChooserState::failed;
        return;
    }
    bool found = false;
    while (reader_more(*r)) {
        const char* key = nullptr;
        const char* signature = nullptr;
        if (!reader_enter_struct(r) || !reader_string(r, &key, nullptr) || !reader_enter_variant(r, &signature))
            break;
        if (c_string_equal(key, "uris") && c_string_equal(signature, "as")) {
            found = true;
            if (reader_enter_array(r)) {
                while (reader_more(*r)) {
                    const char* uri = nullptr;
                    if (!reader_string(r, &uri, nullptr)) {
                        chooser->state = FileChooserState::failed;
                        return;
                    }
                    if (chooser->count == kFileChooserMaximumFiles) {
                        chooser->state = FileChooserState::capacity;
                        chooser->count = 0;
                        return;
                    }
                    if (!file_uri_to_path(uri, chooser->paths[chooser->count], kFileChooserPathBytes)) {
                        chooser->state = c_string_length(uri) >= kFileChooserPathBytes ? FileChooserState::capacity
                                                                                  : FileChooserState::failed;
                        chooser->count = 0;
                        return;
                    }
                    ++chooser->count;
                }
            }
        }
        if (!reader_leave(r) || !reader_leave(r))
            break;
    }
    chooser->state = found && chooser->count ? FileChooserState::done : FileChooserState::failed;
}

// Tells the owner, once, that the dialog is no longer pending.
void settle(FileChooser* chooser) noexcept {
    auto* callback = chooser->settled;
    chooser->settled = nullptr;
    if (callback && chooser->state != FileChooserState::pending)
        callback(chooser->settled_context);
}

void on_response(void* context, u32 code, Reader* results) noexcept {
    FileChooser* chooser = static_cast<FileChooser*>(context);
    if (chooser->released) {
        if (!chooser->reply_pending)
            release_memory(chooser);
        return;
    }
    if (code == 1) {
        chooser->state = FileChooserState::cancelled;
    } else if (code != 0) {
        chooser->state = FileChooserState::failed;
    } else {
        read_uris(chooser, results);
    }
    settle(chooser);
}

void on_reply(void* context, Status status, const Message* reply) noexcept {
    FileChooser* chooser = static_cast<FileChooser*>(context);
    chooser->reply_pending = false;
    if (chooser->released) {
        release_memory(chooser);
        return;
    }
    if (status != Status::ok) {
        log_write(LogLevel::trace, "file_chooser", reply_error_name(status, reply, "the call failed"));
        portal_request_end(&chooser->request);
        if (chooser->state == FileChooserState::pending) {
            chooser->state = FileChooserState::failed;
            settle(chooser);
        }
        return;
    }
    portal_request_adopt_reply(&chooser->request, *reply);
}

// a(sa(us)): each filter's name and its glob patterns (kind 0).
void write_filters(Builder* b, const FileFilter* filters, u32 count) noexcept {
    builder_open_array(b, "(sa(us))");
    for (u32 i = 0; i < count; ++i) {
        builder_open_struct(b, "(sa(us))");
        builder_string(b, filters[i].name);
        builder_open_array(b, "(us)");
        for (u32 j = 0; j < filters[i].pattern_count; ++j) {
            builder_open_struct(b, "(us)");
            builder_u32(b, 0);
            builder_string(b, filters[i].patterns[j]);
            builder_close_struct(b);
        }
        builder_close_array(b);
        builder_close_struct(b);
    }
    builder_close_array(b);
}

FileChooser* start(const FileChooserOptions& options, bool save) noexcept {
    if (!session_bus_open())
        return nullptr;
    Connection* c = session_bus();
    if (!c)
        return nullptr;
    FileChooser* chooser = static_cast<FileChooser*>(heap_allocate(sizeof(FileChooser), true));
    if (!chooser)
        return nullptr;
    chooser->state = FileChooserState::pending;
    chooser->settled = options.settled;
    chooser->settled_context = options.settled_context;
    if (!portal_request_begin(&chooser->request, c, on_response, chooser)) {
        heap_free(chooser);
        return nullptr;
    }
    Builder body;
    builder_init(&body);
    builder_string(&body, options.parent ? options.parent : portal_parent_window());
    builder_string(&body, options.title ? options.title : (save ? "Save File" : "Open File"));
    builder_open_array(&body, "{sv}");
    portal_request_append_token(&body, chooser->request);
    builder_dict_bool(&body, "modal", true);
    if (!save) {
        builder_dict_bool(&body, "multiple", options.multiple);
        builder_dict_bool(&body, "directory", options.directory);
    }
    if (options.accept_label)
        builder_dict_string(&body, "accept_label", options.accept_label);
    if (save && options.current_name)
        builder_dict_string(&body, "current_name", options.current_name);
    if (options.filter_count && options.filters) {
        builder_open_dict_entry(&body, "filters", "a(sa(us))");
        write_filters(&body, options.filters, options.filter_count);
        builder_close_dict_entry(&body);
    }
    if (options.current_folder) {
        // current_folder is a byte string with its NUL.
        builder_open_dict_entry(&body, "current_folder", "ay");
        builder_byte_array(&body, reinterpret_cast<const u8*>(options.current_folder),
                           c_string_length(options.current_folder) + 1);
        builder_close_dict_entry(&body);
    }
    builder_close_array(&body);
    const CallTarget target{kPortalName, kPortalPath, kInterface, save ? "SaveFile" : "OpenFile"};
    chooser->reply_pending = true;
    const bool sent = connection_call(c, target, &body, kReplyTimeoutMs, on_reply, chooser);
    builder_release(&body);
    if (!sent) {
        chooser->reply_pending = false;
        release_memory(chooser);
        return nullptr;
    }
    return chooser;
}

} // namespace

bool file_uri_to_path(const char* uri, char* path, u32 capacity) noexcept {
    const char scheme[] = "file://";
    for (u32 i = 0; i < sizeof(scheme) - 1; ++i)
        if (!uri[i] || (uri[i] | 0x20) != scheme[i])
            return false;
    const char* rest = uri + sizeof(scheme) - 1;
    if (*rest != '/') {
        const char host[] = "localhost";
        for (u32 i = 0; i < sizeof(host) - 1; ++i)
            if ((rest[i] | 0x20) != host[i])
                return false;
        rest += sizeof(host) - 1;
        if (*rest != '/')
            return false;
    }
    u32 length = 0;
    for (const char* p = rest; *p; ++p) {
        char c = *p;
        if (c == '%') {
            const int high = ascii_hex_value(p[1]);
            const int low = high >= 0 ? ascii_hex_value(p[2]) : -1;
            if (high < 0 || low < 0)
                return false;
            c = static_cast<char>(high * 16 + low);
            p += 2;
            if (c == 0)
                return false;
        }
        if (length + 1 >= capacity)
            return false;
        path[length++] = c;
    }
    path[length] = 0;
    return length != 0;
}

FileChooser* file_chooser_open(const FileChooserOptions& options) noexcept {
    return start(options, false);
}

FileChooser* file_chooser_save(const FileChooserOptions& options) noexcept {
    return start(options, true);
}

FileChooserState file_chooser_state(const FileChooser* chooser) noexcept {
    return chooser->state;
}

u32 file_chooser_count(const FileChooser* chooser) noexcept {
    return chooser->state == FileChooserState::done ? chooser->count : 0;
}

const char* file_chooser_path(const FileChooser* chooser, u32 index) noexcept {
    return chooser->state == FileChooserState::done && index < chooser->count ? chooser->paths[index] : nullptr;
}

void file_chooser_cancel(FileChooser* chooser) noexcept {
    if (chooser->state != FileChooserState::pending)
        return;
    portal_request_close(&chooser->request);
    portal_request_end(&chooser->request);
    chooser->state = FileChooserState::cancelled;
}

void file_chooser_release(FileChooser* chooser) noexcept {
    if (!chooser)
        return;
    file_chooser_cancel(chooser);
    if (chooser->reply_pending) {
        chooser->released = true;  // the reply handler frees it
        return;
    }
    release_memory(chooser);
}

} // namespace maicord::dbus
