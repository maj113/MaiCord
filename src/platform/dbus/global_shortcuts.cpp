#include "maicord/core/decimal.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/platform/dbus/bus.hpp"
#include "maicord/platform/dbus/global_shortcuts.hpp"
#include "maicord/platform/dbus/portal.hpp"
#include "maicord/platform/dbus/session.hpp"

namespace maicord::dbus {
namespace {

constexpr const char* kPortalName = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char* kInterface = "org.freedesktop.portal.GlobalShortcuts";
constexpr u32 kMaxShortcuts = 8;
constexpr u32 kCallTimeoutMs = 10000;

struct Spec {
    char id[48];
    char description[96];
    char trigger[64];
};

struct Shortcuts {
    bool started;
    bool hook_added;
    GlobalShortcutsState state;
    GlobalShortcutHandler handler;
    void* context;
    Spec specs[kMaxShortcuts];
    u32 spec_count;
    char session[200];
    PortalRequest request;
    u32 activated, deactivated;
    u32 session_counter;
};

Shortcuts g;

void fail(const char* why) noexcept {
    log_write(LogLevel::trace, "global_shortcuts", why);
    g.state = GlobalShortcutsState::unavailable;
    portal_request_end(&g.request);
}

void on_activated(void*, const Message& signal) noexcept {
    Reader r;
    message_reader(signal, &r);
    const char* session = nullptr;
    const char* id = nullptr;
    u64 timestamp = 0;
    if (!reader_string(&r, &session, nullptr) || !reader_string(&r, &id, nullptr) || !reader_u64(&r, &timestamp))
        return;
    // The bus's arg0 filter only matches strings, so a signal for another session is dropped here.
    if (!c_string_equal(session, g.session))
        return;
    const bool active = signal.member && signal.member[0] == 'A';  // Activated, not Deactivated
    if (g.handler)
        g.handler(g.context, id, active, timestamp);
}

void on_bound(void*, u32 code, Reader*) noexcept {
    if (code != 0) {
        fail("the shortcuts were not bound");
        return;
    }
    g.state = GlobalShortcutsState::ready;
}

// The reply of a method that answers through a Request: the handle to listen on, or the failure (`context` names the
// call for the log).
void on_request_reply(void* context, Status status, const Message* reply) noexcept {
    if (status != Status::ok) {
        // No portal, or one without this interface (an older one): the quiet outcome.
        fail(status == Status::error_reply && reply->error_name ? reply->error_name : static_cast<const char*>(context));
        return;
    }
    Reader r;
    message_reader(*reply, &r);
    const char* handle = nullptr;
    if (reader_string(&r, &handle, nullptr))
        portal_request_adopt(&g.request, handle);
}

void bind_shortcuts(Connection* c) noexcept {
    if (!portal_request_begin(&g.request, c, on_bound, nullptr)) {
        fail("no listener for the response");
        return;
    }
    Builder body;
    builder_init(&body);
    builder_object_path(&body, g.session);
    builder_open_array(&body, "(sa{sv})");
    for (u32 i = 0; i < g.spec_count; ++i) {
        builder_open_struct(&body, "(sa{sv})");
        builder_string(&body, g.specs[i].id);
        builder_open_array(&body, "{sv}");
        builder_dict_string(&body, "description", g.specs[i].description);
        if (g.specs[i].trigger[0])
            builder_dict_string(&body, "preferred_trigger", g.specs[i].trigger);
        builder_close_array(&body);
        builder_close_struct(&body);
    }
    builder_close_array(&body);
    builder_string(&body, portal_parent_window());
    builder_open_array(&body, "{sv}");
    portal_request_append_token(&body, g.request);
    builder_close_array(&body);
    const CallTarget target{kPortalName, kPortalPath, kInterface, "BindShortcuts"};
    if (!connection_call(c, target, &body, kCallTimeoutMs, on_request_reply, const_cast<char*>("BindShortcuts failed")))
        fail("BindShortcuts could not be sent");
    builder_release(&body);
}

// CreateSession's Response: results {session_handle: s}.
void on_session_created(void*, u32 code, Reader* results) noexcept {
    Connection* c = session_bus();
    if (code != 0 || !c) {
        fail("the portal refused the session");
        return;
    }
    const char* session = nullptr;
    if (reader_enter_array(results)) {
        while (reader_more(*results)) {
            const char* key = nullptr;
            if (!reader_enter_struct(results) || !reader_string(results, &key, nullptr))
                break;
            if (c_string_equal(key, "session_handle")) {
                const char* signature = nullptr;
                if (reader_enter_variant(results, &signature) && (signature[0] == 's' || signature[0] == 'o'))
                    reader_string(results, &session, nullptr);
                break;
            }
            if (!reader_skip(results) || !reader_leave(results))
                break;
        }
    }
    if (!session || c_string_length(session) >= sizeof(g.session)) {
        fail("the session response named no handle");
        return;
    }
    c_string_copy(g.session, sizeof(g.session), session);
    const SignalMatch activated{kPortalName, kPortalPath, kInterface, "Activated", nullptr};
    const SignalMatch deactivated{kPortalName, kPortalPath, kInterface, "Deactivated", nullptr};
    g.activated = connection_subscribe(c, activated, on_activated, nullptr);
    g.deactivated = connection_subscribe(c, deactivated, on_activated, nullptr);
    bind_shortcuts(c);
}

void create_session(Connection* c) noexcept {
    g.state = GlobalShortcutsState::starting;
    g.session[0] = 0;
    g.activated = g.deactivated = 0;
    if (!portal_request_begin(&g.request, c, on_session_created, nullptr)) {
        fail("no listener for the response");
        return;
    }
    Builder body;
    builder_init(&body);
    builder_open_array(&body, "{sv}");
    portal_request_append_token(&body, g.request);
    char token[32] = "maicord_session_";
    token[16 + decimal_write(token + 16, ++g.session_counter)] = 0;
    builder_dict_string(&body, "session_handle_token", token);
    builder_close_array(&body);
    const CallTarget target{kPortalName, kPortalPath, kInterface, "CreateSession"};
    if (!connection_call(c, target, &body, kCallTimeoutMs, on_request_reply, const_cast<char*>("CreateSession failed")))
        fail("CreateSession could not be sent");
    builder_release(&body);
}

void on_bus(Connection* c) noexcept {
    if (g.started)
        create_session(c);
}

// The keysym name of a Key for an XDG trigger string; null when there is none.
const char* keysym_of(Key key, char (&scratch)[8]) noexcept {
    if (key >= 'A' && key <= 'Z') {  // VK codes: the letters are upper case; 0x61 and up are the keypad and F keys
        scratch[0] = static_cast<char>(key | 0x20);
        scratch[1] = 0;
        return scratch;
    }
    if (key >= '0' && key <= '9') {
        scratch[0] = static_cast<char>(key);
        scratch[1] = 0;
        return scratch;
    }
    if (key >= key::f1 && key <= key::f24) {
        scratch[0] = 'F';
        scratch[1 + decimal_write(scratch + 1, static_cast<u32>(key - key::f1 + 1))] = 0;
        return scratch;
    }
    if (key >= key::numpad0 && key <= key::numpad9) {
        scratch[0] = 'K';
        scratch[1] = 'P';
        scratch[2] = '_';
        scratch[3] = static_cast<char>('0' + (key - key::numpad0));
        scratch[4] = 0;
        return scratch;
    }
    switch (key) {
    case key::backspace: return "BackSpace";
    case key::tab: return "Tab";
    case key::enter: return "Return";
    case key::pause: return "Pause";
    case key::caps_lock: return "Caps_Lock";
    case key::escape: return "Escape";
    case key::space: return "space";
    case key::page_up: return "Prior";
    case key::page_down: return "Next";
    case key::end: return "End";
    case key::home: return "Home";
    case key::left: return "Left";
    case key::up: return "Up";
    case key::right: return "Right";
    case key::down: return "Down";
    case key::print_screen: return "Print";
    case key::insert: return "Insert";
    case key::del: return "Delete";
    case key::menu: return "Menu";
    case key::multiply: return "KP_Multiply";
    case key::add: return "KP_Add";
    case key::subtract: return "KP_Subtract";
    case key::decimal: return "KP_Decimal";
    case key::divide: return "KP_Divide";
    case key::num_lock: return "Num_Lock";
    case key::scroll_lock: return "Scroll_Lock";
    case key::semicolon: return "semicolon";
    case key::equals: return "equal";
    case key::comma: return "comma";
    case key::minus: return "minus";
    case key::period: return "period";
    case key::slash: return "slash";
    case key::grave: return "grave";
    case key::left_bracket: return "bracketleft";
    case key::backslash: return "backslash";
    case key::right_bracket: return "bracketright";
    case key::quote: return "apostrophe";
    default: return nullptr;
    }
}

} // namespace

bool global_shortcut_trigger(Key key, u8 modifiers, char* out, u32 capacity) noexcept {
    if (capacity)
        out[0] = 0;
    char scratch[8];
    const char* name = keysym_of(key, scratch);
    if (!name)
        return false;
    char text[64];
    u32 length = 0;
    const auto append = [&](const char* part) {
        for (u32 i = 0; part[i]; ++i)
            text[length++] = part[i];
    };
    if (modifiers & mod_ctrl)
        append("CTRL+");
    if (modifiers & mod_alt)
        append("ALT+");
    if (modifiers & mod_shift)
        append("SHIFT+");
    if (modifiers & mod_super)
        append("LOGO+");
    append(name);
    text[length] = 0;
    if (length >= capacity)
        return false;
    copy_bytes(out, text, length + 1);
    return true;
}

bool global_shortcuts_start(const GlobalShortcut* shortcuts, u32 count, GlobalShortcutHandler handler,
                            void* context) noexcept {
    if (g.started || count == 0 || count > kMaxShortcuts || !shortcuts)
        return false;
    g.started = true;
    g.handler = handler;
    g.context = context;
    g.spec_count = count;
    for (u32 i = 0; i < count; ++i) {
        c_string_copy(g.specs[i].id, sizeof(g.specs[i].id), shortcuts[i].id);
        c_string_copy(g.specs[i].description, sizeof(g.specs[i].description), shortcuts[i].description);
        c_string_copy(g.specs[i].trigger, sizeof(g.specs[i].trigger), shortcuts[i].preferred_trigger);
    }
    if (!g.hook_added) {
        g.hook_added = true;
        session_bus_add_hook(on_bus);
    }
    return session_bus_open() && session_bus() != nullptr;
}

GlobalShortcutsState global_shortcuts_state() noexcept {
    return g.started ? g.state : GlobalShortcutsState::unavailable;
}

void global_shortcuts_stop() noexcept {
    if (!g.started)
        return;
    portal_request_end(&g.request);
    if (Connection* c = session_bus()) {
        connection_unsubscribe(c, g.activated);
        connection_unsubscribe(c, g.deactivated);
        if (g.session[0]) {
            const CallTarget target{kPortalName, g.session, "org.freedesktop.portal.Session", "Close"};
            connection_call(c, target, nullptr, 0, nullptr, nullptr);
        }
    }
    g.started = false;
    g.state = GlobalShortcutsState::unavailable;
    g.session[0] = 0;
}

} // namespace maicord::dbus
