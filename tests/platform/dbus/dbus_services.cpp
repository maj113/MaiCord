#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/platform/dbus/connection.hpp"
#include "maicord/platform/dbus/desktop_settings.hpp"
#include "maicord/platform/dbus/notifications.hpp"
#include "maicord/platform/dbus/open_uri.hpp"
#include "maicord/platform/dbus/portal.hpp"
#include "maicord/platform/dbus/secret_service.hpp"
#include "maicord/platform/dbus/session.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"
#include "../../support/test_output.hpp"

// The desktop services over the session bus (tools/test_dbus.sh): the settings portal, notifications (the daemon and the
// portal), OpenURI with its xdg-open fallback, the Secret Service and the portal probes, each against the python mock
// services, whose libdbus decodes every request (the mock logs what it received in a canonical, type-tagged form that
// the expectations below spell out). Four runs share this program:
//
//   (no argument)  the mock bus: settings, direct notifications, open-uri, secrets, probes
//   --portal       the mock bus with MAICORD_NOTIFICATION_BACKEND=portal: the portal notification backend
//   --plain        the mock bus with a notification daemon that does not read markup in the body
//   --bare         a bus without any service: every client reports its absence, open-uri falls back to xdg-open
//
// Without MAICORD_DBUS_TEST_BUS naming the run's bus (and DBUS_SESSION_BUS_ADDRESS = MAICORD_DBUS_TEST_ADDRESS) the test
// skips (77): it never touches the bus of the user's session.

namespace maicord {
namespace {

using namespace dbus;

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

bool text_equal(const char* a, const char* b) noexcept {
    if (!a || !b)
        return a == b;
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

u32 text_length(const char* text) noexcept {
    u32 length = 0;
    while (text[length])
        ++length;
    return length;
}

bool starts_with(const char* text, const char* prefix) noexcept {
    while (*prefix && *text == *prefix)
        ++text, ++prefix;
    return *prefix == 0;
}

bool contains(const char* text, const char* needle) noexcept {
    const u32 n = text_length(needle);
    for (u32 i = 0; text[i]; ++i)
        if (starts_with(text + i, needle) && n)
            return true;
    return false;
}

// The control connection to the mock (its own, apart from the session bus the clients use).
Connection* g_control;

CallTarget mock(const char* member) noexcept {
    return CallTarget{"org.maicord.Mock", "/org/maicord/Mock", "org.maicord.Mock", member};
}

void control(const char* member, const Builder* body) noexcept {
    Message reply;
    const Status status = connection_call_sync(g_control, mock(member), body, 3000, &reply);
    check(status == Status::ok, member);
    if (status == Status::ok || status == Status::error_reply)
        message_release(&reply);
}

// What the mock logged since the last take.
char g_lines[24][1024];
u32 g_line_count;

void take_log() noexcept {
    g_line_count = 0;
    Message reply;
    if (connection_call_sync(g_control, mock("Take"), nullptr, 3000, &reply) != Status::ok)
        return;
    Reader r;
    message_reader(reply, &r);
    if (reader_enter_array(&r)) {
        while (reader_more(r) && g_line_count < 24) {
            const char* text = nullptr;
            u32 length = 0;
            if (!reader_string(&r, &text, &length))
                break;
            if (length >= sizeof(g_lines[0]))
                length = sizeof(g_lines[0]) - 1;
            copy_bytes(g_lines[g_line_count], text, length);
            g_lines[g_line_count][length] = 0;
            ++g_line_count;
        }
    }
    message_release(&reply);
}

const char* find_line(const char* prefix) noexcept {
    for (u32 i = 0; i < g_line_count; ++i)
        if (starts_with(g_lines[i], prefix))
            return g_lines[i];
    return nullptr;
}

// A request line the mock logged, compared with the expected text.
void expect_line(const char* prefix, const char* expected, const char* label) noexcept {
    const char* line = find_line(prefix);
    const bool ok = line && text_equal(line, expected);
    if (!ok) {
        test_write_text("  got:  ");
        test_write_line(line ? line : "(nothing logged)");
        test_write_text("  want: ");
        test_write_line(expected);
    }
    check(ok, label);
}

// Pumps the session bus until `done` or the time passes.
bool pump(bool (*done)(), u32 milliseconds) noexcept {
    const u64 deadline = clock_monotonic_milliseconds() + milliseconds;
    for (;;) {
        if (done())
            return true;
        const u64 now = clock_monotonic_milliseconds();
        if (now >= deadline)
            return false;
        Waitable waitable = session_bus_waitable();
        u32 wait = session_bus_wait_ms(now);
        const u32 left = static_cast<u32>(deadline - now);
        if (wait > left)
            wait = left;
        if (waitable)
            wait_many(&waitable, 1, wait, 0);
        else {
            const TimeSpec nap{static_cast<i64>(wait / 1000), static_cast<i64>(wait % 1000) * 1000000};
            sys_ppoll(nullptr, 0, &nap);
        }
        session_bus_process(clock_monotonic_milliseconds());
    }
}

void settle(u32 milliseconds) noexcept {
    const u64 until = clock_monotonic_milliseconds() + milliseconds;
    while (clock_monotonic_milliseconds() < until) {
        Waitable waitable = session_bus_waitable();
        if (waitable)
            wait_many(&waitable, 1, 20, 0);
        session_bus_process(clock_monotonic_milliseconds());
    }
}

// Settings -----------------------------------------------------------------------------------------------------------------

u32 g_setting_events;
u32 g_setting_areas;

void on_settings(void*, u32 areas) noexcept {
    ++g_setting_events;
    g_setting_areas |= areas;
}

bool setting_arrived() noexcept {
    return g_setting_events != 0;
}

bool near(float a, float b) noexcept {
    return a - b < 0.001f && b - a < 0.001f;
}

void set_setting_u32(const char* ns, const char* key, u32 value) noexcept {
    Builder b;
    builder_init(&b);
    builder_string(&b, ns);
    builder_string(&b, key);
    builder_open_variant(&b, "u");
    builder_u32(&b, value);
    builder_close_variant(&b);
    control("SetSetting", &b);
    builder_release(&b);
}

void set_setting_i32(const char* ns, const char* key, i32 value) noexcept {
    Builder b;
    builder_init(&b);
    builder_string(&b, ns);
    builder_string(&b, key);
    builder_open_variant(&b, "i");
    builder_i32(&b, value);
    builder_close_variant(&b);
    control("SetSetting", &b);
    builder_release(&b);
}

void set_setting_string(const char* ns, const char* key, const char* value) noexcept {
    Builder b;
    builder_init(&b);
    builder_string(&b, ns);
    builder_string(&b, key);
    builder_open_variant(&b, "s");
    builder_string(&b, value);
    builder_close_variant(&b);
    control("SetSetting", &b);
    builder_release(&b);
}

void set_setting_color(const char* ns, const char* key, double r, double g, double b2) noexcept {
    Builder b;
    builder_init(&b);
    builder_string(&b, ns);
    builder_string(&b, key);
    builder_open_variant(&b, "(ddd)");
    builder_open_struct(&b, "(ddd)");
    builder_double(&b, r);
    builder_double(&b, g);
    builder_double(&b, b2);
    builder_close_struct(&b);
    builder_close_variant(&b);
    control("SetSetting", &b);
    builder_release(&b);
}

void test_caption_parser() noexcept {
    CaptionButtons b;
    check(caption_buttons_parse("appmenu:minimize,maximize,close", &b) && b.left_count == 1 && b.left[0] == caption_menu &&
              b.right_count == 3 && b.right[0] == caption_minimize && b.right[1] == caption_maximize &&
              b.right[2] == caption_close,
          "the GNOME 3 layout");
    check(caption_buttons_parse("close,minimize,maximize:appmenu", &b) && b.left_count == 3 && b.left[0] == caption_close &&
              b.left[2] == caption_maximize && b.right_count == 1 && b.right[0] == caption_menu,
          "the macOS-like layout");
    check(caption_buttons_parse("close", &b) && b.left_count == 1 && b.right_count == 0, "no colon: everything on the left");
    check(caption_buttons_parse(":close", &b) && b.left_count == 0 && b.right_count == 1 && b.right[0] == caption_close,
          "nothing on the left");
    check(caption_buttons_parse("spacer,close,close,bogus:icon,minimize", &b) && b.left_count == 1 && b.right_count == 2 &&
              b.right[0] == caption_menu && b.right[1] == caption_minimize,
          "spacers, unknown names and repeats are skipped");
    check(!caption_buttons_parse("", &b) && !caption_buttons_parse(nullptr, &b), "an empty layout is unknown");
    check(caption_buttons_parse("close,minimize,maximize,appmenu,icon:close", &b) && b.left_count == 4, "at most four a side");
}

void test_settings() noexcept {
    desktop_settings_set_handler(on_settings, nullptr);
    check(desktop_settings_start(), "the settings client starts on the session bus");
    check(desktop_settings_wait_loaded(4000), "the portal's ReadAll answers");
    check(g_setting_events >= 1 && (g_setting_areas & desktop_all_areas) == desktop_all_areas, "the first load reports every area");

    DesktopSettings s;
    check(desktop_settings_get(&s) && s.loaded, "the settings are loaded");
    check(s.color_scheme == ColorScheme::dark, "color-scheme 1 is dark");
    check(s.accent_valid && near(s.accent[0], 0.25f) && near(s.accent[1], 0.5f) && near(s.accent[2], 0.75f), "accent colour");
    check(s.more_contrast && s.reduced_motion && !s.animations, "contrast, reduced motion and animations");
    check(s.cursor_blink && s.cursor_blink_ms == 1000 && s.cursor_size == 32 && text_equal(s.cursor_theme, "Bibata-Modern"),
          "cursor blink, size and theme");
    check(text_equal(s.ui_font, "Cantarell 11") && text_equal(s.monospace_font, "Source Code Pro 10") &&
              text_equal(s.document_font, "Sans 11") && text_equal(s.titlebar_font, "Cantarell Bold 11") &&
              near(s.text_scale, 1.25f),
          "fonts and text scale");
    check(s.double_click_ms == 350, "double-click time");
    check(s.caption_known && s.caption.left_count == 3 && s.caption.left[0] == caption_close && s.caption.right_count == 1 &&
              s.caption.right[0] == caption_menu,
          "caption buttons");

    take_log();
    expect_line("ReadAll", "ReadAll [s:org.freedesktop.appearance,s:org.gnome.desktop.interface,s:org.gnome.desktop.wm.preferences,s:org.gnome.desktop.peripherals.mouse,s:org.kde.kdeglobals.KDE]",
                "ReadAll names its namespaces (as libdbus decoded them)");

    // SettingChanged signals update the copy and report only the areas that changed.
    g_setting_events = 0;
    g_setting_areas = 0;
    set_setting_u32("org.freedesktop.appearance", "color-scheme", 2);
    check(pump(setting_arrived, 3000) && g_setting_areas == desktop_scheme, "a colour scheme change reports the scheme area");
    desktop_settings_get(&s);
    check(s.color_scheme == ColorScheme::light, "now light");

    g_setting_events = 0;
    g_setting_areas = 0;
    set_setting_i32("org.gnome.desktop.interface", "cursor-size", 48);
    check(pump(setting_arrived, 3000) && g_setting_areas == desktop_cursor, "a cursor size change reports the cursor area");
    desktop_settings_get(&s);
    check(s.cursor_size == 48, "cursor size 48");

    g_setting_events = 0;
    g_setting_areas = 0;
    set_setting_string("org.gnome.desktop.wm.preferences", "button-layout", "appmenu:close");
    check(pump(setting_arrived, 3000) && g_setting_areas == desktop_caption, "a button layout change reports the caption area");
    desktop_settings_get(&s);
    check(s.caption.left_count == 1 && s.caption.left[0] == caption_menu && s.caption.right_count == 1 &&
              s.caption.right[0] == caption_close,
          "the new layout");

    g_setting_events = 0;
    set_setting_u32("org.freedesktop.appearance", "color-scheme", 2);  // unchanged
    set_setting_string("org.example.unknown", "key", "value");           // not read
    set_setting_i32("org.gnome.desktop.interface", "cursor-size", 100000);  // out of range: ignored
    settle(300);
    desktop_settings_get(&s);
    check(g_setting_events == 0 && s.cursor_size == 48, "unchanged, unknown and out-of-range values report nothing");

    set_setting_color("org.freedesktop.appearance", "accent-color", 2.0, 0.0, 0.0);
    settle(200);
    desktop_settings_get(&s);
    check(!s.accent_valid, "an accent colour outside 0..1 means none");
    set_setting_color("org.freedesktop.appearance", "accent-color", 0.0, 1.0, 0.5);
    settle(200);
    desktop_settings_get(&s);
    check(s.accent_valid && near(s.accent[1], 1.0f), "and a valid one again");

    // GNOME's own colour-scheme is ignored while the portal's is published.
    set_setting_string("org.gnome.desktop.interface", "color-scheme", "prefer-dark");
    settle(200);
    desktop_settings_get(&s);
    check(s.color_scheme == ColorScheme::light, "the interface schema's scheme does not override the portal's");

    // A reconnect reads everything again.
    take_log();
    session_bus_close();
    check(session_bus_open(), "the session bus reopens");
    settle(300);
    take_log();
    check(find_line("ReadAll") != nullptr, "the open hook asks the portal for the settings again");
}

// Notifications ------------------------------------------------------------------------------------------------------------

struct Recorded {
    NotificationEventKind kind;
    u64 cookie;
    u32 id;
    char key[32];
    char token[32];
    bool has_token;
    u32 reason;
};

Recorded g_events[16];
u32 g_event_count;

void on_notification(void*, const NotificationEvent& event) noexcept {
    if (g_event_count >= 16)
        return;
    Recorded& r = g_events[g_event_count++];
    r = Recorded{};
    r.kind = event.kind;
    r.cookie = event.cookie;
    r.id = event.id;
    if (event.action_key) {
        u32 i = 0;
        for (; event.action_key[i] && i < sizeof(r.key) - 1; ++i)
            r.key[i] = event.action_key[i];
        r.key[i] = 0;
    }
    if (event.activation_token) {
        r.has_token = true;
        u32 i = 0;
        for (; event.activation_token[i] && i < sizeof(r.token) - 1; ++i)
            r.token[i] = event.activation_token[i];
        r.token[i] = 0;
    }
    r.reason = event.reason;
}

u32 g_events_seen;

bool event_arrived() noexcept {
    return g_event_count > g_events_seen;
}

// Waits for the next event and returns it (null on timeout).
const Recorded* next_event(u32 milliseconds = 3000) noexcept {
    if (!pump(event_arrived, milliseconds))
        return nullptr;
    return &g_events[g_events_seen++];
}

void test_notification_direct() noexcept {
    notifications_set_handler(on_notification, nullptr);
    check(notification_backend() == NotificationBackend::direct, "outside a sandbox the daemon is used directly");

    NotificationRequest bad{};
    check(!notification_show(bad), "a notification needs a summary");
    const NotificationAction reserved[1] = {{"default", "Nope"}};
    NotificationRequest bad_actions{};
    bad_actions.summary = "x";
    bad_actions.actions = reserved;
    bad_actions.action_count = 1;
    check(!notification_show(bad_actions), "the key \"default\" is the notification itself, not a button");

    const NotificationAction actions[1] = {{"reply", "Reply"}};
    NotificationRequest request{};
    request.summary = "Hello";
    request.body = "Body text";
    request.icon = "dialog-information";
    request.category = "im.received";
    request.desktop_entry = "maicord";
    request.actions = actions;
    request.action_count = 1;
    request.default_action = true;
    request.timeout_ms = 5000;
    request.urgency = NotificationUrgency::critical;
    request.silent = true;
    request.cookie = 77;
    take_log();
    check(notification_show(request), "a notification is sent");
    const Recorded* shown = next_event();
    check(shown && shown->kind == NotificationEventKind::shown && shown->cookie == 77 && shown->id > 0, "the daemon's id comes back");
    const u32 id = shown ? shown->id : 0;
    take_log();
    expect_line("Notify",
                "Notify s:MaiCord u:0 s:dialog-information s:Hello s:Body text [s:default,s:Open,s:reply,s:Reply] "
                "{s:category=v(s:im.received),s:desktop-entry=v(s:maicord),s:suppress-sound=v(b:1),s:urgency=v(y:2)} i:5000",
                "Notify's arguments as libdbus decoded them");

    // Signals: the token first, then the action.
    Builder b;
    builder_init(&b);
    builder_u32(&b, id);
    builder_string(&b, "token-123");
    control("NotifyToken", &b);
    builder_release(&b);
    builder_init(&b);
    builder_string(&b, "reply");
    builder_u32(&b, id);
    control("NotifyAction", &b);
    builder_release(&b);
    const Recorded* action = next_event();
    check(action && action->kind == NotificationEventKind::action && action->cookie == 77 && action->id == id &&
              text_equal(action->key, "reply") && action->has_token && text_equal(action->token, "token-123"),
          "an action with its activation token");
    builder_init(&b);
    builder_string(&b, "default");
    builder_u32(&b, id);
    control("NotifyAction", &b);
    builder_release(&b);
    action = next_event();
    check(action && action->kind == NotificationEventKind::action && text_equal(action->key, "default") && !action->has_token,
          "the token belongs to one activation");

    // Another application's notification is none of ours.
    builder_init(&b);
    builder_string(&b, "default");
    builder_u32(&b, 99999);
    control("NotifyAction", &b);
    builder_release(&b);
    settle(150);
    check(g_event_count == g_events_seen, "an action for an id this process never got is ignored");

    builder_init(&b);
    builder_u32(&b, id);
    builder_u32(&b, 2);
    control("NotifyClosed", &b);
    builder_release(&b);
    const Recorded* closed = next_event();
    check(closed && closed->kind == NotificationEventKind::closed && closed->cookie == 77 && closed->reason == 2,
          "the user's dismissal");
    builder_init(&b);
    builder_string(&b, "default");
    builder_u32(&b, id);
    control("NotifyAction", &b);
    builder_release(&b);
    settle(150);
    check(g_event_count == g_events_seen, "a closed notification takes no more actions");

    // Replace: the same id comes back and one entry serves both.
    NotificationRequest first{};
    first.summary = "One";
    first.timeout_ms = -1;
    first.cookie = 1;
    check(notification_show(first), "a plain notification");
    const Recorded* one = next_event();
    check(one && one->kind == NotificationEventKind::shown && one->cookie == 1, "shown");
    const u32 first_id = one ? one->id : 0;
    NotificationRequest replacement{};
    replacement.summary = "Two";
    replacement.timeout_ms = 0;
    replacement.replaces_id = first_id;
    replacement.cookie = 2;
    take_log();
    check(notification_show(replacement), "a replacing notification");
    const Recorded* two = next_event();
    check(two && two->kind == NotificationEventKind::shown && two->cookie == 2 && two->id == first_id, "the replacement keeps the id");
    take_log();
    char want[256];
    const char head[] = "Notify s:MaiCord u:";
    copy_bytes(want, head, sizeof(head) - 1);
    u32 length = sizeof(head) - 1;
    char digits[12];
    u32 count = 0;
    for (u32 n = first_id; n || count == 0; n /= 10)
        digits[count++] = static_cast<char>('0' + n % 10);
    while (count)
        want[length++] = digits[--count];
    const char tail[] = " s: s:Two s: [] {s:urgency=v(y:1)} i:0";
    copy_bytes(want + length, tail, sizeof(tail));
    expect_line("Notify", want, "the replacement names the notification it replaces");

    // Closing.
    check(notification_close(first_id), "closing a notification");
    const Recorded* closed_by_call = next_event();
    check(closed_by_call && closed_by_call->kind == NotificationEventKind::closed && closed_by_call->cookie == 2 &&
              closed_by_call->reason == 3,
          "the daemon reports the close");
    take_log();
    check(find_line("CloseNotification") != nullptr, "CloseNotification reached the daemon");

    // A refusing daemon.
    Builder mode;
    builder_init(&mode);
    builder_string(&mode, "notify-error");
    builder_bool(&mode, true);
    control("SetMode", &mode);
    builder_release(&mode);
    NotificationRequest refused{};
    refused.summary = "Refused";
    refused.cookie = 5;
    check(notification_show(refused), "the request is sent");
    const Recorded* failed = next_event();
    check(failed && failed->kind == NotificationEventKind::failed && failed->cookie == 5, "a refusal arrives as a failed event");
    builder_init(&mode);
    builder_string(&mode, "notify-error");
    builder_bool(&mode, false);
    control("SetMode", &mode);
    builder_release(&mode);

    // The text of a message is text: a daemon that reads markup gets &, < and > as entities (a link or an image tag in
    // a message stays inert), and the summary, which is plain, goes in as it is.
    NotificationRequest markup{};
    markup.summary = "R&D <team>";
    markup.body = "<a href=\"https://x\">a & b</a> > c \xC3\xA9";
    markup.icon = "/tmp/maicord avatar.png";
    markup.timeout_ms = -1;
    markup.cookie = 6;
    take_log();
    check(notification_show(markup), "a notification whose text holds markup characters");
    const Recorded* marked = next_event();
    check(marked && marked->kind == NotificationEventKind::shown && marked->cookie == 6, "shown");
    take_log();
    expect_line("Notify",
                "Notify s:MaiCord u:0 s:/tmp/maicord avatar.png s:R&D <team> "
                "s:&lt;a href=\"https://x\"&gt;a &amp; b&lt;/a&gt; &gt; c \xC3\xA9 [] {s:urgency=v(y:1)} i:-1",
                "the body is escaped for a daemon that reads markup, the avatar is its path, the summary is plain");
    // Text that is only too long is cut (the mock reports the length it received and whether it ends inside an entity;
    // it would refuse a body that is not UTF-8, so the cut is not inside a character either).
    static char big[24 * 1024];
    for (u32 i = 0; i < sizeof(big) - 1; i += 3) {
        big[i] = '<';
        big[i + 1] = static_cast<char>(0xC3);
        big[i + 2] = static_cast<char>(0xA9);
    }
    big[sizeof(big) - 1] = 0;
    NotificationRequest long_text{};
    long_text.summary = "Long";
    long_text.body = big;
    long_text.timeout_ms = -1;
    long_text.cookie = 7;
    take_log();
    check(notification_show(long_text), "a notification with a very long body");
    const Recorded* long_shown = next_event();
    check(long_shown && long_shown->kind == NotificationEventKind::shown && long_shown->cookie == 7,
          "it arrives, cut inside no character");
    take_log();
    const char* long_line = find_line("NotifyLong");
    check(long_line && starts_with(long_line, "NotifyLong 2047") && text_equal(long_line + text_length(long_line) - 2, " 1"),
          "the cut falls between whole entities");
}

// A daemon that does not read markup gets the body as it is.
void test_notification_plain() noexcept {
    notifications_set_handler(on_notification, nullptr);
    check(notification_backend() == NotificationBackend::direct, "outside a sandbox the daemon is used directly");
    Builder mode;
    builder_init(&mode);
    builder_string(&mode, "no-markup");
    builder_bool(&mode, true);
    control("SetMode", &mode);
    builder_release(&mode);
    NotificationRequest request{};
    request.summary = "R&D";
    request.body = "<b>a & b</b>";
    request.timeout_ms = -1;
    request.cookie = 31;
    take_log();
    check(notification_show(request), "a notification for a daemon without body-markup");
    const Recorded* shown = next_event();
    check(shown && shown->kind == NotificationEventKind::shown && shown->cookie == 31, "shown");
    take_log();
    expect_line("Notify", "Notify s:MaiCord u:0 s: s:R&D s:<b>a & b</b> [] {s:urgency=v(y:1)} i:-1",
                "the body stays plain text");
    builder_init(&mode);
    builder_string(&mode, "no-markup");
    builder_bool(&mode, false);
    control("SetMode", &mode);
    builder_release(&mode);
}

void test_notification_portal() noexcept {
    notifications_set_handler(on_notification, nullptr);
    check(notification_backend() == NotificationBackend::portal, "MAICORD_NOTIFICATION_BACKEND=portal selects the portal");
    const NotificationAction actions[1] = {{"reply", "Reply"}};
    NotificationRequest request{};
    request.summary = "Portal hello";
    request.body = "Body";
    request.icon = "mail-unread";
    request.actions = actions;
    request.action_count = 1;
    request.default_action = true;
    request.urgency = NotificationUrgency::critical;
    request.cookie = 9;
    take_log();
    check(notification_show(request), "a portal notification is sent");
    const Recorded* shown = next_event();
    check(shown && shown->kind == NotificationEventKind::shown && shown->cookie == 9 && shown->id == 1, "shown, with the local id");
    take_log();
    expect_line("AddNotification",
                "AddNotification s:1 {s:body=v(s:Body),s:buttons=v([{s:action=v(s:reply),s:label=v(s:Reply)}]),"
                "s:default-action=v(s:default),s:icon=v((s:themed,v([s:mail-unread]))),s:priority=v(s:urgent),"
                "s:title=v(s:Portal hello)}",
                "AddNotification's arguments as libdbus decoded them");

    Builder b;
    builder_init(&b);
    builder_string(&b, "1");
    builder_string(&b, "reply");
    control("PortalAction", &b);
    builder_release(&b);
    const Recorded* action = next_event();
    check(action && action->kind == NotificationEventKind::action && action->cookie == 9 && action->id == 1 &&
              text_equal(action->key, "reply"),
          "the portal's ActionInvoked");
    builder_init(&b);
    builder_string(&b, "default");
    builder_string(&b, "999");
    control("PortalAction", &b);
    builder_release(&b);
    settle(150);
    check(g_event_count == g_events_seen, "an action for another application's notification is ignored");

    check(notification_close(1), "RemoveNotification");
    const Recorded* closed = next_event();
    check(closed && closed->kind == NotificationEventKind::closed && closed->cookie == 9 && closed->reason == 3, "closed");
    take_log();
    expect_line("RemoveNotification", "RemoveNotification s:1", "RemoveNotification's argument");

    // An avatar is a file the portal's host may not see: it gets the file's bytes. A file that is not there is no icon.
    const char* directory = environment_get("MAICORD_DBUS_TEST_DIR");
    char avatar[512];
    u32 avatar_length = directory ? text_length(directory) : 0;
    if (avatar_length && avatar_length < 400) {
        copy_bytes(avatar, directory, avatar_length);
        const char name[] = "/avatar.png";
        copy_bytes(avatar + avatar_length, name, sizeof(name));
        const u8 image[7] = {0x89, 'P', 'N', 'G', 1, 2, 3};
        const long created = sys_openat(kAtCurrentDirectory, avatar,
                                        kOpenWriteOnly | kOpenCreate | kOpenTruncate | kOpenCloseOnExec, 0600);
        check(!syscall_failed(created), "the test makes an avatar file");
        if (!syscall_failed(created)) {
            sys_write(static_cast<int>(created), image, sizeof(image));
            sys_close(static_cast<int>(created));
        }
        NotificationRequest with_avatar{};
        with_avatar.summary = "Alice";
        with_avatar.icon = avatar;
        with_avatar.cookie = 10;
        take_log();
        check(notification_show(with_avatar), "a portal notification with an avatar");
        const Recorded* avatar_shown = next_event();
        check(avatar_shown && avatar_shown->kind == NotificationEventKind::shown && avatar_shown->cookie == 10, "shown");
        take_log();
        expect_line("AddNotification",
                    "AddNotification s:2 {s:icon=v((s:bytes,v([y:137,y:80,y:78,y:71,y:1,y:2,y:3]))),s:priority=v(s:normal),"
                    "s:title=v(s:Alice)}",
                    "the avatar goes to the portal as the bytes of the file");
        NotificationRequest missing{};
        missing.summary = "Bob";
        missing.icon = "/maicord-no-such-avatar.png";
        missing.cookie = 11;
        take_log();
        check(notification_show(missing), "a portal notification whose avatar file is missing");
        const Recorded* missing_shown = next_event();
        check(missing_shown && missing_shown->kind == NotificationEventKind::shown && missing_shown->cookie == 11, "shown");
        take_log();
        expect_line("AddNotification", "AddNotification s:3 {s:priority=v(s:normal),s:title=v(s:Bob)}",
                    "and goes without an icon");
    } else {
        test_write_line("SKIP the portal avatar (MAICORD_DBUS_TEST_DIR is not set)");
    }
}

// OpenURI ------------------------------------------------------------------------------------------------------------------

// The fake xdg-open appends its arguments to $MAICORD_DBUS_TEST_DIR/xdg-open.log.
bool read_spawn_log(char* text, u32 capacity) noexcept {
    const char* directory = environment_get("MAICORD_DBUS_TEST_DIR");
    if (!directory)
        return false;
    char path[512];
    u32 length = text_length(directory);
    if (length > 400)
        return false;
    copy_bytes(path, directory, length);
    const char name[] = "/xdg-open.log";
    copy_bytes(path + length, name, sizeof(name));
    const long opened = sys_openat(kAtCurrentDirectory, path, kOpenReadOnly | kOpenCloseOnExec);
    if (syscall_failed(opened))
        return false;
    const long got = sys_read(static_cast<int>(opened), text, capacity - 1);
    sys_close(static_cast<int>(opened));
    text[got > 0 ? got : 0] = 0;
    return got > 0;
}

bool spawn_log_has(const char* needle) noexcept {
    char text[2048];
    return read_spawn_log(text, sizeof(text)) && contains(text, needle);
}

bool spawn_log_has_refused() noexcept {
    return spawn_log_has("https://fallback.example/refused");
}

bool spawn_log_has_bare() noexcept {
    return spawn_log_has("https://fallback.example/bare");
}

void test_open_uri_validation() noexcept {
    check(open_uri_valid("https://example.org/") && open_uri_valid("mailto:a@b.c") && open_uri_valid("file:///tmp/x y") &&
              open_uri_valid("x-scheme+1.2:rest"),
          "ordinary URIs are accepted");
    check(!open_uri_valid(nullptr) && !open_uri_valid("") && !open_uri_valid("-flag") && !open_uri_valid("no-scheme") &&
              !open_uri_valid("1http://x") && !open_uri_valid("http//x") && !open_uri_valid("ht tp://x") &&
              !open_uri_valid("http://x\ny") && !open_uri_valid("http://x\x01"),
          "options, missing schemes and control characters are refused");
    char big[5000];
    for (u32 i = 0; i < sizeof(big) - 1; ++i)
        big[i] = 'a';
    big[0] = 'h';
    big[1] = ':';
    big[sizeof(big) - 1] = 0;
    check(!open_uri_valid(big), "a URI over 4095 bytes is refused");
    check(!open_uri("-flag", nullptr), "open_uri refuses what is not valid");
}

void test_open_uri_portal() noexcept {
    take_log();
    check(open_uri("https://example.org/a b?x=1", "token123"), "open_uri sends the request");
    settle(300);
    take_log();
    expect_line("OpenURI", "OpenURI s: s:https://example.org/a b?x=1 {s:activation_token=v(s:token123)}",
                "OpenURI's arguments as libdbus decoded them");
    check(open_uri("mailto:someone@example.org", nullptr), "a request without a token");
    settle(300);
    take_log();
    expect_line("OpenURI", "OpenURI s: s:mailto:someone@example.org {}", "no options without a token");

    // The portal refuses: the fallback starts xdg-open.
    Builder mode;
    builder_init(&mode);
    builder_string(&mode, "openuri-error");
    builder_bool(&mode, true);
    control("SetMode", &mode);
    builder_release(&mode);
    check(open_uri("https://fallback.example/refused", nullptr), "a request the portal will refuse");
    check(pump(spawn_log_has_refused, 4000), "the refusal starts xdg-open with the URI");
    builder_init(&mode);
    builder_string(&mode, "openuri-error");
    builder_bool(&mode, false);
    control("SetMode", &mode);
    builder_release(&mode);
}

// The Secret Service ------------------------------------------------------------------------------------------------------

void test_secrets() noexcept {
    const SecretAttribute attributes[2] = {{"application", "maicord"}, {"purpose", "test-key"}};
    u8 secret[64];
    u32 length = 99;
    control("Reset", nullptr);
    {
        const SecretStatus first = secret_lookup(attributes, 2, secret, sizeof(secret), &length);
        if (first != SecretStatus::not_found)
            test_write_line(secret_status_name(first));
        check(first == SecretStatus::not_found && length == 0, "an empty keyring has no such secret");
    }
    take_log();
    expect_line("OpenSession", "OpenSession s:plain v(s:)", "OpenSession asks for the plain algorithm");
    expect_line("SearchItems", "SearchItems {s:application=s:maicord,s:purpose=s:test-key}", "SearchItems' attributes");

    u8 key[32];
    for (u32 i = 0; i < 32; ++i)
        key[i] = static_cast<u8>(i * 7 + 1);
    check(secret_store("MaiCord test key", attributes, 2, key, 32) == SecretStatus::ok, "a secret is stored");
    take_log();
    const char* created = find_line("CreateItem");
    check(created && contains(created, "label=MaiCord test key") &&
              contains(created, "attrs={s:application=s:maicord,s:purpose=s:test-key}") &&
              contains(created, "secret=0108") && contains(created, "type=application/octet-stream") &&
              contains(created, "replace=1"),
          "CreateItem's label, attributes, value, content type and replace flag");
    check(find_line("ReadAlias default") != nullptr, "the default collection comes from ReadAlias");

    check(secret_lookup(attributes, 2, secret, sizeof(secret), &length) == SecretStatus::ok && length == 32 &&
              bytes_equal(secret, key, 32),
          "the stored secret reads back");
    take_log();
    check(find_line("GetSecret /org/freedesktop/secrets/collection/login/") != nullptr, "GetSecret on the item");

    u8 key2[32];
    for (u32 i = 0; i < 32; ++i)
        key2[i] = static_cast<u8>(255 - i);
    check(secret_store("MaiCord test key", attributes, 2, key2, 32) == SecretStatus::ok &&
              secret_lookup(attributes, 2, secret, sizeof(secret), &length) == SecretStatus::ok && length == 32 &&
              bytes_equal(secret, key2, 32),
          "storing again replaces the secret");

    u8 small[8];
    check(secret_lookup(attributes, 2, small, sizeof(small), &length) == SecretStatus::failed && length == 32,
          "a buffer that is too small reports the length and fails");

    const SecretAttribute other[2] = {{"application", "maicord"}, {"purpose", "other"}};
    check(secret_lookup(other, 2, secret, sizeof(secret), &length) == SecretStatus::not_found, "other attributes find nothing");
    const SecretAttribute subset[1] = {{"application", "maicord"}};
    check(secret_lookup(subset, 1, secret, sizeof(secret), &length) == SecretStatus::ok && length == 32,
          "an attribute subset matches");

    // A large secret.
    static u8 large[4096];
    static u8 back[4096];
    for (u32 i = 0; i < sizeof(large); ++i)
        large[i] = static_cast<u8>(i * 31);
    const SecretAttribute big[1] = {{"purpose", "large"}};
    check(secret_store("large", big, 1, large, sizeof(large)) == SecretStatus::ok &&
              secret_lookup(big, 1, back, sizeof(back), &length) == SecretStatus::ok && length == sizeof(large) &&
              bytes_equal(back, large, sizeof(large)),
          "a 4 KiB secret round-trips");

    // A locked keyring.
    Builder mode;
    builder_init(&mode);
    builder_string(&mode, "secret-locked");
    builder_bool(&mode, true);
    control("SetMode", &mode);
    builder_release(&mode);
    check(secret_lookup(attributes, 2, secret, sizeof(secret), &length) == SecretStatus::locked, "a locked keyring is reported");
    check(secret_store("x", attributes, 2, key, 32) == SecretStatus::locked, "storing into a locked keyring is reported");
    builder_init(&mode);
    builder_string(&mode, "secret-locked");
    builder_bool(&mode, false);
    control("SetMode", &mode);
    builder_release(&mode);

    builder_init(&mode);
    builder_string(&mode, "secret-create-error");
    builder_bool(&mode, true);
    control("SetMode", &mode);
    builder_release(&mode);
    check(secret_store("x", other, 2, key, 32) == SecretStatus::failed, "a refusing service is a failure");
    builder_init(&mode);
    builder_string(&mode, "secret-create-error");
    builder_bool(&mode, false);
    control("SetMode", &mode);
    builder_release(&mode);

    check(secret_erase(attributes, 2) == SecretStatus::ok, "erase");
    check(secret_lookup(attributes, 2, secret, sizeof(secret), &length) == SecretStatus::not_found &&
              secret_erase(attributes, 2) == SecretStatus::not_found,
          "the item is gone, and a second erase finds nothing");
    secret_erase(big, 1);
}

// Portal probes -----------------------------------------------------------------------------------------------------------

void test_probes() noexcept {
    check(!portal_sandboxed(), "the test runs outside a sandbox");
    check(portal_interface_version("org.freedesktop.portal.Settings", 2000) == 2, "the Settings portal's version");
    check(portal_interface_version("org.freedesktop.portal.Inhibit", 2000) == 3, "the Inhibit probe finds the portal");
    check(portal_interface_version("org.freedesktop.portal.NoSuchThing", 2000) == 0, "an unknown interface is version 0");
}

// A bus without any service --------------------------------------------------------------------------------------------

bool g_bare_failed;

void on_bare_notification(void*, const NotificationEvent& event) noexcept {
    if (event.kind == NotificationEventKind::failed)
        g_bare_failed = true;
}

bool bare_failed() noexcept {
    return g_bare_failed;
}

void test_bare() noexcept {
    check(desktop_settings_start(), "the bus itself is there");
    check(!desktop_settings_wait_loaded(2000), "no settings portal: nothing loads");
    DesktopSettings s;
    check(!desktop_settings_get(&s) && s.color_scheme == ColorScheme::no_preference && s.animations && s.text_scale == 1.0f,
          "the defaults stand");

    notifications_set_handler(on_bare_notification, nullptr);
    NotificationRequest request{};
    request.summary = "Nobody listens";
    request.cookie = 1;
    check(notification_show(request), "the request is sent");
    check(pump(bare_failed, 3000), "no notification daemon: a failed event");

    u8 secret[32];
    u32 length = 0;
    const SecretAttribute attributes[1] = {{"application", "maicord"}};
    check(secret_lookup(attributes, 1, secret, sizeof(secret), &length) == SecretStatus::unavailable &&
              secret_store("x", attributes, 1, secret, 32) == SecretStatus::unavailable &&
              secret_erase(attributes, 1) == SecretStatus::unavailable,
          "no Secret Service: unavailable");
    check(portal_interface_version("org.freedesktop.portal.Inhibit", 1000) == 0, "no portal: version 0");

    check(open_uri("https://fallback.example/bare", nullptr), "open_uri falls back to xdg-open at once");
    check(pump(spawn_log_has_bare, 4000), "xdg-open ran with the URI");
}

bool skip_unless(const char* kind) noexcept {
    const char* bus = environment_get("MAICORD_DBUS_TEST_BUS");
    const char* address = environment_get("MAICORD_DBUS_TEST_ADDRESS");
    const char* session = environment_get("DBUS_SESSION_BUS_ADDRESS");
    return bus && address && session && text_equal(bus, kind) && text_equal(address, session);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_dbus_services_entry) {
    using namespace maicord;
    const ProcessStartup& startup = process_startup();
    const char* mode = startup.argc > 1 ? startup.argv[1] : "";
    const bool bare = text_equal(mode, "--bare");
    const bool portal = text_equal(mode, "--portal");
    const bool plain = text_equal(mode, "--plain");
    if (!skip_unless(bare ? "bare" : "mock")) {
        test_write_line("SKIP dbus_services (the private bus is not set up: tools/test_dbus.sh)");
        test_process_exit(77);
    }
    if (bare) {
        test_bare();
        test_exit(g_failures, "dbus_services_bare");
    }
    g_control = connection_open_address(environment_get("MAICORD_DBUS_TEST_ADDRESS"));
    check(g_control != nullptr, "a control connection to the mock");
    if (!g_control)
        test_exit(g_failures, "dbus_services");
    control("Reset", nullptr);
    if (portal) {
        test_notification_portal();
        test_exit(g_failures, "dbus_services_portal");
    }
    if (plain) {
        test_notification_plain();
        test_exit(g_failures, "dbus_services_plain");
    }
    test_caption_parser();
    test_settings();
    test_notification_direct();
    test_open_uri_validation();
    test_open_uri_portal();
    test_secrets();
    test_probes();
    connection_close(g_control);
    session_bus_close();
    test_exit(g_failures, "dbus_services");
}
