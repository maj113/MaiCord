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

CaptionButton button_named(const char* token, u32 length) noexcept {
    if (equals_n(token, length, "close"))
        return caption_close;
    if (equals_n(token, length, "minimize"))
        return caption_minimize;
    if (equals_n(token, length, "maximize"))
        return caption_maximize;
    if (equals_n(token, length, "appmenu") || equals_n(token, length, "icon"))
        return caption_menu;
    return caption_none;
}

bool seen(const CaptionButtons& buttons, u8 button) noexcept {
    for (u32 i = 0; i < buttons.left_count; ++i)
        if (buttons.left[i] == button)
            return true;
    for (u32 i = 0; i < buttons.right_count; ++i)
        if (buttons.right[i] == button)
            return true;
    return false;
}

// The value of a settings entry as far as this module reads it.
struct Value {
    char type;  // the D-Bus type code of what was read, 0 for a type not read
    bool boolean;
    i64 integer;  // every integer type
    double real;
    const char* text;
    u32 text_length;
    double rgb[3];
    bool is_rgb;
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
    case 'd':
        value->type = 'd';
        if (!reader_double(r, &value->real))
            return false;
        break;
    case 's':
        value->type = 's';
        if (!reader_string(r, &value->text, &value->text_length))
            return false;
        break;
    case '(': {
        // (ddd): the accent colour.
        if (!reader_enter_struct(r))
            return false;
        bool ok = true;
        for (u32 i = 0; i < 3 && ok; ++i)
            ok = reader_type(*r) == 'd' && reader_double(r, &value->rgb[i]);
        value->is_rgb = ok;
        value->type = '(';
        if (!reader_leave(r))
            return false;
        break;
    }
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

ColorScheme scheme_of_number(i64 number) noexcept {
    return number == 1 ? ColorScheme::dark : number == 2 ? ColorScheme::light : ColorScheme::no_preference;
}

// Applies one setting to `s`; the areas that changed.
u32 apply(State* state, DesktopSettings* s, const char* ns, const char* key, const Value& v) noexcept {
    u32 areas = 0;
    const auto integer_in = [&](i64 low, i64 high) { return (v.type == 'i' || v.type == 'u' || v.type == 'x' || v.type == 'n' || v.type == 'q' || v.type == 'y' || v.type == 't') && v.integer >= low && v.integer <= high; };
    if (c_string_equal(ns, "org.freedesktop.appearance")) {
        if (c_string_equal(key, "color-scheme") && integer_in(0, 2)) {
            state->portal_scheme_seen = true;
            const ColorScheme next = scheme_of_number(v.integer);
            if (s->color_scheme != next)
                areas |= desktop_scheme;
            s->color_scheme = next;
        } else if (c_string_equal(key, "accent-color")) {
            const bool valid = v.is_rgb && v.rgb[0] >= 0 && v.rgb[0] <= 1 && v.rgb[1] >= 0 && v.rgb[1] <= 1 &&
                               v.rgb[2] >= 0 && v.rgb[2] <= 1;
            const float r = valid ? static_cast<float>(v.rgb[0]) : 0;
            const float g2 = valid ? static_cast<float>(v.rgb[1]) : 0;
            const float b = valid ? static_cast<float>(v.rgb[2]) : 0;
            if (s->accent_valid != valid || s->accent[0] != r || s->accent[1] != g2 || s->accent[2] != b)
                areas |= desktop_accent;
            s->accent_valid = valid;
            s->accent[0] = r;
            s->accent[1] = g2;
            s->accent[2] = b;
        } else if (c_string_equal(key, "contrast") && integer_in(0, 1)) {
            if (set_value(&s->more_contrast, v.integer == 1))
                areas |= desktop_contrast;
        } else if (c_string_equal(key, "reduced-motion") && integer_in(0, 1)) {
            if (set_value(&s->reduced_motion, v.integer == 1))
                areas |= desktop_motion;
        }
    } else if (c_string_equal(ns, "org.gnome.desktop.interface")) {
        if (c_string_equal(key, "enable-animations") && v.type == 'b') {
            if (set_value(&s->animations, v.boolean))
                areas |= desktop_motion;
        } else if (c_string_equal(key, "cursor-blink") && v.type == 'b') {
            if (set_value(&s->cursor_blink, v.boolean))
                areas |= desktop_caret;
        } else if (c_string_equal(key, "cursor-blink-time") && integer_in(100, 20000)) {
            if (set_value(&s->cursor_blink_ms, static_cast<u32>(v.integer)))
                areas |= desktop_caret;
        } else if (c_string_equal(key, "cursor-size") && integer_in(1, 512)) {
            if (set_value(&s->cursor_size, static_cast<u32>(v.integer)))
                areas |= desktop_cursor;
        } else if (c_string_equal(key, "cursor-theme") && v.type == 's') {
            if (set_text(s->cursor_theme, sizeof(s->cursor_theme), v.text, v.text_length))
                areas |= desktop_cursor;
        } else if (c_string_equal(key, "font-name") && v.type == 's') {
            if (set_text(s->ui_font, sizeof(s->ui_font), v.text, v.text_length))
                areas |= desktop_fonts;
        } else if (c_string_equal(key, "document-font-name") && v.type == 's') {
            if (set_text(s->document_font, sizeof(s->document_font), v.text, v.text_length))
                areas |= desktop_fonts;
        } else if (c_string_equal(key, "monospace-font-name") && v.type == 's') {
            if (set_text(s->monospace_font, sizeof(s->monospace_font), v.text, v.text_length))
                areas |= desktop_fonts;
        } else if (c_string_equal(key, "text-scaling-factor") && v.type == 'd' && v.real >= 0.5 && v.real <= 3.0) {
            const float next = static_cast<float>(v.real);
            if (s->text_scale != next)
                areas |= desktop_fonts;
            s->text_scale = next;
        } else if (c_string_equal(key, "color-scheme") && v.type == 's' && !state->portal_scheme_seen) {
            const ColorScheme next = equals_n(v.text, v.text_length, "prefer-dark")    ? ColorScheme::dark
                                     : equals_n(v.text, v.text_length, "prefer-light") ? ColorScheme::light
                                                                                       : ColorScheme::no_preference;
            if (s->color_scheme != next)
                areas |= desktop_scheme;
            s->color_scheme = next;
        }
    } else if (c_string_equal(ns, "org.gnome.desktop.wm.preferences")) {
        if (c_string_equal(key, "button-layout") && v.type == 's') {
            char layout[128];
            const u32 length = v.text_length < sizeof(layout) - 1 ? v.text_length : sizeof(layout) - 1;
            copy_bytes(layout, v.text, length);
            layout[length] = 0;
            CaptionButtons parsed{};
            const bool known = caption_buttons_parse(layout, &parsed);
            if (s->caption_known != known || !bytes_equal(&s->caption, &parsed, sizeof(parsed)))
                areas |= desktop_caption;
            s->caption_known = known;
            s->caption = parsed;
        } else if (c_string_equal(key, "titlebar-font") && v.type == 's') {
            if (set_text(s->titlebar_font, sizeof(s->titlebar_font), v.text, v.text_length))
                areas |= desktop_fonts;
        }
    } else if (c_string_equal(ns, "org.gnome.desktop.peripherals.mouse")) {
        if (c_string_equal(key, "double-click") && integer_in(50, 5000)) {
            if (set_value(&s->double_click_ms, static_cast<u32>(v.integer)))
                areas |= desktop_pointer;
        }
    } else if (c_string_equal(ns, "org.kde.kdeglobals.KDE")) {
        if (c_string_equal(key, "DoubleClickInterval")) {
            i64 interval = 0;
            if (v.type == 's') {
                for (u32 i = 0; i < v.text_length && v.text[i] >= '0' && v.text[i] <= '9' && interval < 100000; ++i)
                    interval = interval * 10 + (v.text[i] - '0');
            } else if (integer_in(0, 100000)) {
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
    const u32 areas = apply(&g, &next, ns, key, value);
    if (areas) {
        publish(next);
        announce(areas);
    }
}

// ReadAll's reply: a{sa{sv}}
void on_read_all(void*, Status status, const Message* reply) noexcept {
    g.read_finished = true;
    if (status != Status::ok) {
        log_write(LogLevel::info, "dbus",
                  status == Status::error_reply && reply->error_name ? reply->error_name
                                                                     : "the settings portal did not answer");
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
                areas |= apply(&g, &next, ns, key, value);
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
    builder_string(&body, "org.freedesktop.appearance");
    builder_string(&body, "org.gnome.desktop.interface");
    builder_string(&body, "org.gnome.desktop.wm.preferences");
    builder_string(&body, "org.gnome.desktop.peripherals.mouse");
    builder_string(&body, "org.kde.kdeglobals.KDE");
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

bool caption_buttons_parse(const char* layout, CaptionButtons* buttons) noexcept {
    *buttons = CaptionButtons{};
    if (!layout || !*layout)
        return false;
    bool right = false;
    const char* cursor = layout;
    while (*cursor) {
        if (*cursor == ':') {
            right = true;
            ++cursor;
            continue;
        }
        if (*cursor == ',' || *cursor == ' ') {
            ++cursor;
            continue;
        }
        const char* start = cursor;
        while (*cursor && *cursor != ',' && *cursor != ':' && *cursor != ' ')
            ++cursor;
        const CaptionButton button = button_named(start, static_cast<u32>(cursor - start));
        if (button == caption_none || seen(*buttons, button))
            continue;
        if (right && buttons->right_count < 4)
            buttons->right[buttons->right_count++] = button;
        else if (!right && buttons->left_count < 4)
            buttons->left[buttons->left_count++] = button;
    }
    return true;
}

void desktop_settings_defaults(DesktopSettings* settings) noexcept {
    *settings = DesktopSettings{};
    settings->color_scheme = ColorScheme::no_preference;
    settings->animations = true;
    settings->cursor_blink = true;
    settings->cursor_blink_ms = 1200;
    settings->text_scale = 1.0f;
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
