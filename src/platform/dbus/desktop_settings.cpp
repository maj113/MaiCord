#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/platform/dbus/desktop_settings.hpp"
#include "maicord/platform/dbus/session.hpp"

namespace maicord::dbus {
namespace {

constexpr const char* kPortalName = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalPath = "/org/freedesktop/portal/desktop";
constexpr const char* kSettingsInterface = "org.freedesktop.portal.Settings";
constexpr u32 kReadTimeoutMs = 10000;

struct State {
    DesktopSettings value;
    volatile u32 sequence;  // odd while the value is being replaced
    bool initialized;
    bool hook_registered;
    bool read_finished;  // the first ReadAll was answered or failed
    bool portal_scheme_seen;
    DesktopSettingsHandler handler;
    void* handler_context;
};

State g;

bool equals_n(const char* a, u32 a_length, const char* b) noexcept {
    return a_length == c_string_length(b) && bytes_equal(a, b, a_length);
}

// The value of a settings entry as far as this module reads it.
struct Value {
    char type;  // the D-Bus type code of what was read, 0 for a type not read
    bool boolean;
    i64 integer;  // every integer type
    const char* text;
    u32 text_length;
};

// Reads the variant at the reader's position (unwrapping variants nested in it) into `value`.
bool read_value(Reader* r, Value* value) noexcept {
    *value = Value{};
    u32 entered = 0;
    while (reader_type(*r) == 'v' && entered < 4) {
        if (!reader_enter_variant(r, nullptr))
            return false;
        ++entered;
    }
    switch (reader_type(*r)) {
    case 'b':
        value->type = 'b';
        if (!reader_bool(r, &value->boolean))
            return false;
        break;
    case 'y': {
        u8 v;
        value->type = 'y';
        if (!reader_byte(r, &v))
            return false;
        value->integer = v;
        break;
    }
    case 'n': {
        i16 v;
        value->type = 'n';
        if (!reader_i16(r, &v))
            return false;
        value->integer = v;
        break;
    }
    case 'q': {
        u16 v;
        value->type = 'q';
        if (!reader_u16(r, &v))
            return false;
        value->integer = v;
        break;
    }
    case 'i': {
        i32 v;
        value->type = 'i';
        if (!reader_i32(r, &v))
            return false;
        value->integer = v;
        break;
    }
    case 'u': {
        u32 v;
        value->type = 'u';
        if (!reader_u32(r, &v))
            return false;
        value->integer = v;
        break;
    }
    case 'x':
        value->type = 'x';
        if (!reader_i64(r, &value->integer))
            return false;
        break;
    case 't': {
        u64 v;
        value->type = 't';
        if (!reader_u64(r, &v))
            return false;
        value->integer = v > 0x7FFFFFFFFFFFFFFFull ? 0x7FFFFFFFFFFFFFFFll : static_cast<i64>(v);
        break;
    }
    case 's':
        value->type = 's';
        if (!reader_string(r, &value->text, &value->text_length))
            return false;
        break;
    default:
        break;  // another type: leaving the variants skips it
    }
    while (entered--)
        if (!reader_leave(r))
            return false;
    return !r->failed;
}

// Stores `value`; whether it differs from what was there.
template <typename T> bool set_value(T* field, T value) noexcept {
    const bool changed = *field != value;
    *field = value;
    return changed;
}

bool set_text(char* field, u32 capacity, const char* text, u32 length) noexcept {
    if (length >= capacity)
        length = capacity - 1;
    char next[128];
    copy_bytes(next, text, length);
    next[length] = 0;
    const bool changed = !c_string_equal(field, next);
    copy_bytes(field, next, length + 1);
    return changed;
}

// The namespaces the settings portal is asked for (ReadAll's filter), in the order of Setting::space.
constexpr const char* kNamespaces[] = {"org.freedesktop.appearance", "org.gnome.desktop.interface",
                                       "org.gnome.desktop.peripherals.mouse", "org.kde.kdeglobals.KDE"};
enum : u8 { space_appearance, space_interface, space_mouse, space_kde };

// A setting that is one field of DesktopSettings, read as `kind` and, when it changes, raising `area`.
struct Setting {
    const char* key;
    u8 space;
    enum Kind : u8 { number, flag, boolean, text } kind;
    u16 area;
    u16 offset;  // of the field in DesktopSettings
    u16 low;     // number: the least accepted value; text: the field's capacity
    u16 high;    // number: the greatest
};

#define MAICORD_SETTING(space, key, kind, field, area, low, high) \
    Setting{key, space, Setting::kind, area, static_cast<u16>(__builtin_offsetof(DesktopSettings, field)), low, high}
constexpr Setting kSettings[] = {
    // reduced-motion: u, 1 for on
    MAICORD_SETTING(space_appearance, "reduced-motion", flag, reduced_motion, desktop_motion, 0, 1),
    MAICORD_SETTING(space_interface, "enable-animations", boolean, animations, desktop_motion, 0, 0),
    MAICORD_SETTING(space_interface, "cursor-blink", boolean, cursor_blink, desktop_caret, 0, 0),
    MAICORD_SETTING(space_interface, "cursor-blink-time", number, cursor_blink_ms, desktop_caret, 100, 20000),
    MAICORD_SETTING(space_interface, "cursor-size", number, cursor_size, desktop_cursor, 1, 512),
    MAICORD_SETTING(space_interface, "cursor-theme", text, cursor_theme, desktop_cursor, 0, 64),
    MAICORD_SETTING(space_interface, "font-name", text, ui_font, desktop_fonts, 0, 96),
    MAICORD_SETTING(space_interface, "monospace-font-name", text, monospace_font, desktop_fonts, 0, 96),
    MAICORD_SETTING(space_mouse, "double-click", number, double_click_ms, desktop_pointer, 50, 5000),
};
#undef MAICORD_SETTING

bool integer_in(const Value& v, i64 low, i64 high) noexcept {
    return (v.type == 'i' || v.type == 'u' || v.type == 'x' || v.type == 'n' || v.type == 'q' || v.type == 'y' ||
            v.type == 't') &&
           v.integer >= low && v.integer <= high;
}

// Applies a setting of kSettings to `s`; the areas that changed.
u32 apply_field(DesktopSettings* s, u32 space, const char* key, const Value& v) noexcept {
    for (const Setting& row : kSettings) {
        if (row.space != space || !c_string_equal(row.key, key))
            continue;
        u8* field = reinterpret_cast<u8*>(s) + row.offset;
        bool changed = false;
        switch (row.kind) {
        case Setting::number:
            if (integer_in(v, row.low, row.high))
                changed = set_value(reinterpret_cast<u32*>(field), static_cast<u32>(v.integer));
            break;
        case Setting::flag:
            if (integer_in(v, row.low, row.high))
                changed = set_value(reinterpret_cast<bool*>(field), v.integer == 1);
            break;
        case Setting::boolean:
            if (v.type == 'b')
                changed = set_value(reinterpret_cast<bool*>(field), v.boolean);
            break;
        case Setting::text:
            if (v.type == 's')
                changed = set_text(reinterpret_cast<char*>(field), row.high, v.text, v.text_length);
            break;
        }
        return changed ? row.area : 0;
    }
    return 0;
}

// Applies one setting to `s`; the areas that changed.
u32 apply(DesktopSettings* s, const char* ns, const char* key, const Value& v) noexcept {
    u32 space = 0;
    while (space < sizeof(kNamespaces) / sizeof(kNamespaces[0]) && !c_string_equal(ns, kNamespaces[space]))
        ++space;
    if (const u32 areas = apply_field(s, space, key, v))
        return areas;
    u32 areas = 0;
    // What is more than one field or reads another way.
    if (space == space_appearance) {
        if (c_string_equal(key, "color-scheme") && integer_in(v, 0, 2)) {
            g.portal_scheme_seen = true;
            const auto next = static_cast<ColorScheme>(v.integer);  // the portal's numbers are ColorScheme's
            if (s->color_scheme != next)
                areas |= desktop_scheme;
            s->color_scheme = next;
        }
    } else if (space == space_interface) {
        if (c_string_equal(key, "color-scheme") && v.type == 's' && !g.portal_scheme_seen) {
            const ColorScheme next = equals_n(v.text, v.text_length, "prefer-dark")    ? ColorScheme::dark
                                     : equals_n(v.text, v.text_length, "prefer-light") ? ColorScheme::light
                                                                                       : ColorScheme::no_preference;
            if (s->color_scheme != next)
                areas |= desktop_scheme;
            s->color_scheme = next;
        }
    } else if (space == space_kde) {
        if (c_string_equal(key, "DoubleClickInterval")) {
            i64 interval = 0;
            if (v.type == 's') {
                for (u32 i = 0; i < v.text_length && v.text[i] >= '0' && v.text[i] <= '9' && interval < 100000; ++i)
                    interval = interval * 10 + (v.text[i] - '0');
            } else if (integer_in(v, 0, 100000)) {
                interval = v.integer;
            }
            if (interval >= 50 && interval <= 5000 && set_value(&s->double_click_ms, static_cast<u32>(interval)))
                areas |= desktop_pointer;
        }
    }
    return areas;
}

void publish(const DesktopSettings& next) noexcept {
    __atomic_store_n(&g.sequence, g.sequence + 1, __ATOMIC_RELEASE);  // odd: being replaced
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    g.value = next;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    __atomic_store_n(&g.sequence, g.sequence + 1, __ATOMIC_RELEASE);
}

void announce(u32 areas) noexcept {
    if (areas && g.handler)
        g.handler(g.handler_context, areas);
}

// SettingChanged(s namespace, s key, v value)
void on_setting_changed(void*, const Message& signal) noexcept {
    Reader r;
    message_reader(signal, &r);
    const char* ns = nullptr;
    const char* key = nullptr;
    Value value;
    if (!reader_string(&r, &ns, nullptr) || !reader_string(&r, &key, nullptr) || !read_value(&r, &value))
        return;
    DesktopSettings next = g.value;
    const u32 areas = apply(&next, ns, key, value);
    if (areas) {
        publish(next);
        announce(areas);
    }
}

// ReadAll's reply: a{sa{sv}}
void on_read_all(void*, Status status, const Message* reply) noexcept {
    g.read_finished = true;
    if (status != Status::ok) {
        log_write(LogLevel::info, "dbus", reply_error_name(status, reply, "the settings portal did not answer"));
        return;
    }
    Reader r;
    message_reader(*reply, &r);
    DesktopSettings next = g.value;
    u32 areas = 0;
    bool ok = reader_enter_array(&r);
    while (ok && reader_more(r)) {
        const char* ns = nullptr;
        ok = reader_enter_struct(&r) && reader_string(&r, &ns, nullptr) && reader_enter_array(&r);
        while (ok && reader_more(r)) {
            const char* key = nullptr;
            Value value;
            ok = reader_enter_struct(&r) && reader_string(&r, &key, nullptr) && read_value(&r, &value);
            if (ok)
                areas |= apply(&next, ns, key, value);
            ok = ok && reader_leave(&r);
        }
        ok = ok && reader_leave(&r) && reader_leave(&r);
    }
    if (!ok)
        log_write(LogLevel::warning, "dbus", "the settings portal's ReadAll reply was malformed");
    if (!next.loaded) {
        next.loaded = true;
        areas |= desktop_all_areas;
    }
    publish(next);
    announce(areas);
}

void request_settings(Connection* c) noexcept {
    const SignalMatch match{kPortalName, kPortalPath, kSettingsInterface, "SettingChanged", nullptr};
    connection_subscribe(c, match, on_setting_changed, nullptr);
    Builder body;
    builder_init(&body);
    builder_open_array(&body, "s");
    for (const char* ns : kNamespaces)
        builder_string(&body, ns);
    builder_close_array(&body);
    g.read_finished = false;
    const CallTarget target{kPortalName, kPortalPath, kSettingsInterface, "ReadAll"};
    if (!connection_call(c, target, &body, kReadTimeoutMs, on_read_all, nullptr))
        g.read_finished = true;
    builder_release(&body);
}

void initialize() noexcept {
    if (g.initialized)
        return;
    g.initialized = true;
    desktop_settings_defaults(&g.value);
}

} // namespace

void desktop_settings_defaults(DesktopSettings* settings) noexcept {
    *settings = DesktopSettings{};
    settings->color_scheme = ColorScheme::no_preference;
    settings->animations = true;
    settings->cursor_blink = true;
    settings->cursor_blink_ms = 1200;
}

void desktop_settings_set_handler(DesktopSettingsHandler handler, void* context) noexcept {
    g.handler = handler;
    g.handler_context = context;
}

bool desktop_settings_start() noexcept {
    initialize();
    if (!g.hook_registered) {
        g.hook_registered = true;
        session_bus_add_hook(request_settings);
    }
    return session_bus_open();
}

bool desktop_settings_wait_loaded(u32 timeout_ms) noexcept {
    initialize();
    const volatile bool* finished = &g.read_finished;
    if (session_bus())
        session_bus_wait_for(finished, timeout_ms);
    return g.value.loaded;
}

bool desktop_settings_get(DesktopSettings* settings) noexcept {
    if (!g.initialized) {
        desktop_settings_defaults(settings);
        return false;
    }
    for (;;) {
        const u32 before = __atomic_load_n(&g.sequence, __ATOMIC_ACQUIRE);
        if (before & 1)
            continue;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        *settings = g.value;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (__atomic_load_n(&g.sequence, __ATOMIC_ACQUIRE) == before)
            return settings->loaded;
    }
}

} // namespace maicord::dbus
