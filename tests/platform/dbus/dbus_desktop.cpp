#include "maicord/core/shared_library.hpp"
#include "maicord/platform/dbus/file_chooser.hpp"
#include "maicord/platform/dbus/global_shortcuts.hpp"
#include "maicord/platform/dbus/instance.hpp"
#include "maicord/platform/dbus/launcher_entry.hpp"
#include "maicord/platform/dbus/tray.hpp"
#include "mock_control.hpp"

// The desktop integration over the session bus (tools/test_dbus.sh): the StatusNotifierItem tray with its dbusmenu, the
// GlobalShortcuts portal, single instance with org.freedesktop.Application, the file chooser portal and the launcher entry
// badge, against the python mock whose libdbus is the other end of every exchange (it calls our exported objects, decodes
// what we send, and logs it in a canonical type-tagged form). Runs:
//
//   (no argument)           the mock bus
//   --bare                  a bus without any service: every client degrades silently
//   --socket                the single-instance socket backend (MAICORD_INSTANCE_BACKEND=socket); needs no bus
//   --secondary KEY URL     a second launch: forwards URL to the instance KEY and exits (0 forwarded)
//
// Without MAICORD_DBUS_TEST_BUS naming the run's bus (and DBUS_SESSION_BUS_ADDRESS = MAICORD_DBUS_TEST_ADDRESS) the bus
// runs skip (77): they never touch the bus of the user's session.

namespace maicord {
namespace {

using namespace mock;

constexpr const char kApplicationId[] = "io.github.maj113.MaiCord";

// Tray ---------------------------------------------------------------------------------------------------------------------

struct TrayRecord {
    TrayEventKind kind;
    i32 x, y;
    u32 command;
};

TrayRecord g_tray_events[16];
u32 g_tray_event_count;

void on_tray(void*, const TrayEvent& event) noexcept {
    if (g_tray_event_count < 16)
        g_tray_events[g_tray_event_count++] = TrayRecord{event.kind, event.x, event.y, event.command};
}

bool tray_registered() noexcept {
    return tray_available();
}

bool tray_unregistered() noexcept {
    return !tray_available();
}

const u32 kIconBig[4] = {0xFF102030u, 0x80FFFFFFu, 0x00000000u, 0xFFAABBCCu};  // 2x2
const u32 kIconSmall[1] = {0xFF010203u};                                       // 1x1

const TrayPixmap kIcons[2] = {{2, 2, kIconBig}, {1, 1, kIconSmall}};

const TrayMenuEntry kMenu[4] = {
    {"Open MaiCord", 1, false, false, false},
    {"Mute", 2, true, false, false},
    {"Deafen", 3, true, true, false},
    {"Quit MaiCord", 4, false, false, true},
};

TrayState tray_state(const TrayMenuEntry* menu, u32 menu_count, bool attention, const char* tooltip) noexcept {
    return TrayState{"MaiCord", tooltip, kIcons, 2, attention, menu, menu_count};
}

// Calls a method on the tray item (or its menu) through the mock and returns the result line, null on timeout.
const char* call_item(const char* tag, const char* path, const char* interface, const char* member, const char* signature,
                      void (*arguments)(Builder*) noexcept) noexcept {
    Builder b;
    invoke_begin(&b, tag, "@item", path, interface, member, signature);
    if (arguments)
        arguments(&b);
    invoke_end(&b);
    return wait_result(tag, 4000);
}

// "Result <tag> ok <rest>": the rest, or null.
const char* after_ok(const char* line) noexcept {
    if (!line)
        return nullptr;
    const char* ok = nullptr;
    for (u32 i = 0; line[i]; ++i)
        if (starts_with(line + i, " ok ")) {
            ok = line + i + 4;
            break;
        }
    return ok;
}

void args_none(Builder*) noexcept {}
void args_item_interface(Builder* b) noexcept {
    arg_string(b, "org.kde.StatusNotifierItem");
}
void args_get_status(Builder* b) noexcept {
    arg_string(b, "org.kde.StatusNotifierItem");
    arg_string(b, "Status");
}
void args_get_unknown(Builder* b) noexcept {
    arg_string(b, "org.kde.StatusNotifierItem");
    arg_string(b, "NoSuchProperty");
}
void args_point(Builder* b) noexcept {
    arg_i32(b, 10);
    arg_i32(b, 20);
}
void args_scroll(Builder* b) noexcept {
    arg_i32(b, 3);
    arg_string(b, "vertical");
}
void args_layout_full(Builder* b) noexcept {
    arg_i32(b, 0);
    arg_i32(b, -1);
    arg_string_array(b, nullptr, 0);
}
void args_layout_root(Builder* b) noexcept {
    arg_i32(b, 0);
    arg_i32(b, 0);
    arg_string_array(b, nullptr, 0);
}
void args_layout_bad(Builder* b) noexcept {
    arg_i32(b, 99);
    arg_i32(b, -1);
    arg_string_array(b, nullptr, 0);
}
void args_group(Builder* b) noexcept {
    const i32 ids[2] = {1, 4};
    arg_int_array(b, ids, 2);
    arg_string_array(b, nullptr, 0);
}
void args_property(Builder* b) noexcept {
    arg_i32(b, 2);
    arg_string(b, "label");
}
void args_event_quit(Builder* b) noexcept {
    arg_i32(b, 5);
    arg_string(b, "clicked");
    arg_i32(b, 0);
    arg_u32(b, 0);
}
void args_event_opened(Builder* b) noexcept {
    arg_i32(b, 1);
    arg_string(b, "opened");
    arg_i32(b, 0);
    arg_u32(b, 0);
}
void args_event_group(Builder* b) noexcept {
    arg_click_group(b, 3);
}
void args_about(Builder* b) noexcept {
    arg_i32(b, 0);
}

const char kLayoutItems[] =
    "[v((i:1,{s:enabled=v(b:1),s:label=v(s:Open MaiCord),s:visible=v(b:1)},[])),"
    "v((i:2,{s:enabled=v(b:1),s:label=v(s:Mute),s:toggle-state=v(i:0),s:toggle-type=v(s:checkmark),s:visible=v(b:1)},[])),"
    "v((i:3,{s:enabled=v(b:1),s:label=v(s:Deafen),s:toggle-state=v(i:1),s:toggle-type=v(s:checkmark),s:visible=v(b:1)},[])),"
    "v((i:4,{s:type=v(s:separator)},[])),"
    "v((i:5,{s:enabled=v(b:1),s:label=v(s:Quit MaiCord),s:visible=v(b:1)},[]))])";

// The result text after "ok u:<revision> ": the layout; *revision gets the number.
const char* layout_of(const char* line, u32* revision) noexcept {
    const char* rest = after_ok(line);
    if (!rest || !starts_with(rest, "u:"))
        return nullptr;
    rest += 2;
    u32 value = 0;
    while (*rest >= '0' && *rest <= '9')
        value = value * 10 + static_cast<u32>(*rest++ - '0');
    *revision = value;
    return *rest == ' ' ? rest + 1 : nullptr;
}

void test_tray() noexcept {
    clear_log();
    check(tray_start("maicord", on_tray, nullptr), "the tray item starts on the bus");
    tray_update(tray_state(kMenu, 4, false, "3 unread"));
    settle(300);
    check(!tray_available(), "without a watcher nothing is registered, and nothing fails");

    Builder b;
    builder_init(&b);
    builder_bool(&b, true);
    control("WatcherSet", &b);
    builder_release(&b);
    check(pump(tray_registered, 4000), "a watcher appears: the item registers");
    take_log();
    check(find_line("RegisterItem org.kde.StatusNotifierItem-") != nullptr && count_lines("RegisterItem") == 1,
          "RegisterStatusNotifierItem names the item's bus name");

    // The properties, as libdbus decoded them.
    const char* props = call_item("props", "/StatusNotifierItem", "org.freedesktop.DBus.Properties", "GetAll", "s",
                                  args_item_interface);
    check(props && contains(props, "s:Category=v(s:ApplicationStatus)") && contains(props, "s:Id=v(s:maicord)") &&
              contains(props, "s:Title=v(s:MaiCord)") && contains(props, "s:Status=v(s:Active)") &&
              contains(props, "s:Menu=v(o:/MenuBar)") && contains(props, "s:ItemIsMenu=v(b:0)") &&
              contains(props, "s:WindowId=v(u:0)") && contains(props, "s:IconName=v(s:)") &&
              contains(props, "s:OverlayIconPixmap=v([])"),
          "GetAll: category, id, title, status, menu, window id, icon names");
    check(props && contains(props, "s:IconPixmap=v([(i:2,i:2,ay:ff10203080ffffff00000000ffaabbcc),(i:1,i:1,ay:ff010203)])") &&
              contains(props, "s:AttentionIconPixmap=v([(i:2,i:2,"),
          "the icon pixmaps are ARGB32 in network order, one per size");
    check(props && contains(props, "s:ToolTip=v((s:,[],s:MaiCord,s:3 unread))"), "the tooltip (name, pixmaps, title, text)");
    const char* status = call_item("get", "/StatusNotifierItem", "org.freedesktop.DBus.Properties", "Get", "ss", args_get_status);
    check(status && contains(status, " ok v(s:Active)"), "Get of one property");
    const char* unknown = call_item("unknown", "/StatusNotifierItem", "org.freedesktop.DBus.Properties", "Get", "ss", args_get_unknown);
    check(unknown && contains(unknown, " error org.freedesktop.DBus.Error.UnknownProperty"), "an unknown property is an error");
    const char* introspect = call_item("introspect", "/StatusNotifierItem", "org.freedesktop.DBus.Introspectable", "Introspect", "", args_none);
    check(introspect && contains(introspect, "org.kde.StatusNotifierItem") && contains(introspect, "name=\"Activate\""),
          "Introspect describes the interface");

    // Clicks.
    g_tray_event_count = 0;
    call_item("activate", "/StatusNotifierItem", "org.kde.StatusNotifierItem", "Activate", "ii", args_point);
    call_item("secondary", "/StatusNotifierItem", "org.kde.StatusNotifierItem", "SecondaryActivate", "ii", args_point);
    call_item("context", "/StatusNotifierItem", "org.kde.StatusNotifierItem", "ContextMenu", "ii", args_point);
    const char* scrolled = call_item("scroll", "/StatusNotifierItem", "org.kde.StatusNotifierItem", "Scroll", "is", args_scroll);
    check(scrolled && contains(scrolled, " ok "), "Scroll is accepted");
    check(g_tray_event_count == 3 && g_tray_events[0].kind == TrayEventKind::activate && g_tray_events[0].x == 10 &&
              g_tray_events[0].y == 20 && g_tray_events[1].kind == TrayEventKind::secondary_activate &&
              g_tray_events[2].kind == TrayEventKind::context_menu && g_tray_events[2].x == 10,
          "activate, secondary activate and context menu reach the handler with the position");

    // The menu.
    u32 revision = 0;
    const char* layout_line = call_item("layout", "/MenuBar", "com.canonical.dbusmenu", "GetLayout", "iias", args_layout_full);
    const char* layout = layout_of(layout_line, &revision);
    char want[2048] = "(i:0,{s:children-display=v(s:submenu)},";
    copy_bytes(want + text_length(want), kLayoutItems, sizeof(kLayoutItems));
    u32 length = text_length(want);
    want[length] = 0;
    check(layout && text_equal(layout, want) && revision >= 1, "GetLayout: the root, then its five entries with their properties");
    if (layout && !text_equal(layout, want)) {
        test_write_text("  got:  ");
        test_write_line(layout);
        test_write_text("  want: ");
        test_write_line(want);
    }
    const char* root = call_item("root", "/MenuBar", "com.canonical.dbusmenu", "GetLayout", "iias", args_layout_root);
    const char* root_layout = layout_of(root, &revision);
    check(root_layout && text_equal(root_layout, "(i:0,{s:children-display=v(s:submenu)},[])"), "GetLayout with depth 0 has no children");
    const char* bad = call_item("badlayout", "/MenuBar", "com.canonical.dbusmenu", "GetLayout", "iias", args_layout_bad);
    check(bad && contains(bad, " error org.freedesktop.DBus.Error.InvalidArgs"), "GetLayout of an unknown item is an error");
    const char* group = call_item("group", "/MenuBar", "com.canonical.dbusmenu", "GetGroupProperties", "aias", args_group);
    check(group && contains(group, " ok [(i:1,{s:enabled=v(b:1),s:label=v(s:Open MaiCord),s:visible=v(b:1)}),(i:4,{s:type=v(s:separator)})]"),
          "GetGroupProperties");
    const char* property = call_item("property", "/MenuBar", "com.canonical.dbusmenu", "GetProperty", "is", args_property);
    check(property && contains(property, " ok v(s:Mute)"), "GetProperty");
    const char* about = call_item("about", "/MenuBar", "com.canonical.dbusmenu", "AboutToShow", "i", args_about);
    check(about && contains(about, " ok b:0"), "AboutToShow needs no update");
    const char* menu_props = call_item("menuprops", "/MenuBar", "org.freedesktop.DBus.Properties", "GetAll", "s", args_item_interface);
    check(menu_props && contains(menu_props, "ok {}") , "the menu's properties of another interface are empty");

    g_tray_event_count = 0;
    call_item("click", "/MenuBar", "com.canonical.dbusmenu", "Event", "isvu", args_event_quit);
    call_item("opened", "/MenuBar", "com.canonical.dbusmenu", "Event", "isvu", args_event_opened);
    const char* group_click = call_item("clickgroup", "/MenuBar", "com.canonical.dbusmenu", "EventGroup", "a(isvu)", args_event_group);
    check(group_click && contains(group_click, " ok []"), "EventGroup answers an empty error list");
    check(g_tray_event_count == 2 && g_tray_events[0].kind == TrayEventKind::command && g_tray_events[0].command == 4 &&
              g_tray_events[1].kind == TrayEventKind::command && g_tray_events[1].command == 3,
          "a click on the entry reports its command; \"opened\" reports nothing");

    // A change: signals, a new revision, the new text.
    clear_log();
    const TrayMenuEntry changed_menu[2] = {{"Open", 1, false, false, false}, {"Quit", 4, false, false, false}};
    tray_update(tray_state(changed_menu, 2, true, "no news"));
    settle(150);
    take_log();
    check(find_line("ItemSignal NewToolTip /StatusNotifierItem") != nullptr &&
              find_line("ItemSignal NewStatus /StatusNotifierItem s:NeedsAttention") != nullptr &&
              find_line("ItemSignal LayoutUpdated /MenuBar") != nullptr && find_line("ItemSignal NewIcon") == nullptr,
          "a changed tooltip, attention and menu raise their signals, an unchanged icon none");
    const char* status_now = call_item("status2", "/StatusNotifierItem", "org.freedesktop.DBus.Properties", "Get", "ss", args_get_status);
    check(status_now && contains(status_now, " ok v(s:NeedsAttention)"), "the status follows attention");
    u32 next_revision = 0;
    const char* layout_now = layout_of(call_item("layout2", "/MenuBar", "com.canonical.dbusmenu", "GetLayout", "iias", args_layout_full), &next_revision);
    check(layout_now && next_revision == revision + 1 && contains(layout_now, "s:label=v(s:Quit)") && !contains(layout_now, "Mute"),
          "the layout has the new entries under the next revision");
    clear_log();
    tray_update(tray_state(changed_menu, 2, true, "no news"));
    settle(100);
    take_log();
    check(find_line("ItemSignal") == nullptr, "an update that changes nothing sends nothing");

    // The watcher goes away and comes back.
    builder_init(&b);
    builder_bool(&b, false);
    control("WatcherSet", &b);
    builder_release(&b);
    check(pump(tray_unregistered, 4000), "the watcher leaves: the item is no longer registered");
    clear_log();
    builder_init(&b);
    builder_bool(&b, true);
    control("WatcherSet", &b);
    builder_release(&b);
    check(pump(tray_registered, 4000), "the watcher returns: the item registers again");
    take_log();
    check(count_lines("RegisterItem") == 1, "once");

    tray_stop();
    check(!tray_available(), "stopped");
    const char* gone = call_item("gone", "/StatusNotifierItem", "org.freedesktop.DBus.Properties", "Get", "ss", args_get_status);
    check(gone && contains(gone, " error org.freedesktop.DBus.Error.ServiceUnknown"), "the item's bus name is released");
    builder_init(&b);
    builder_bool(&b, false);
    control("WatcherSet", &b);
    builder_release(&b);
}

void test_tray_bare() noexcept {
    check(tray_start("maicord", on_tray, nullptr), "the tray starts on a bus with no watcher");
    tray_update(tray_state(kMenu, 4, false, "x"));
    settle(300);
    check(!tray_available(), "no watcher: unavailable, silently");
    tray_stop();
}

// Global shortcuts ----------------------------------------------------------------------------------------------------------

struct ShortcutRecord {
    char id[48];
    bool active;
    u64 timestamp;
};

ShortcutRecord g_shortcut_events[8];
u32 g_shortcut_event_count;

void on_shortcut(void*, const char* id, bool active, u64 timestamp) noexcept {
    if (g_shortcut_event_count < 8) {
        ShortcutRecord& r = g_shortcut_events[g_shortcut_event_count++];
        copy_bytes(r.id, id, text_length(id) + 1);
        r.active = active;
        r.timestamp = timestamp;
    }
}

bool shortcuts_ready() noexcept {
    return global_shortcuts_state() == GlobalShortcutsState::ready;
}

bool shortcuts_settled() noexcept {
    return global_shortcuts_state() != GlobalShortcutsState::starting;
}

bool shortcut_event_seen() noexcept {
    return g_shortcut_event_count != 0;
}

void send_shortcut_state(const char* id, bool active) noexcept {
    Builder b;
    builder_init(&b);
    builder_string(&b, id);
    builder_bool(&b, active);
    control("ShortcutState", &b);
    builder_release(&b);
}

void test_trigger_strings() noexcept {
    char text[64];
    check(global_shortcut_trigger('P', mod_ctrl | mod_alt, text, sizeof(text)) && text_equal(text, "CTRL+ALT+p"), "a letter with modifiers");
    check(global_shortcut_trigger(key::f5, 0, text, sizeof(text)) && text_equal(text, "F5"), "a function key");
    check(global_shortcut_trigger(key::f24, mod_shift | mod_super, text, sizeof(text)) && text_equal(text, "SHIFT+LOGO+F24"), "F24 with shift and super");
    check(global_shortcut_trigger(key::space, mod_ctrl, text, sizeof(text)) && text_equal(text, "CTRL+space"), "space");
    check(global_shortcut_trigger('7', 0, text, sizeof(text)) && text_equal(text, "7"), "a digit");
    check(global_shortcut_trigger(key::numpad3, 0, text, sizeof(text)) && text_equal(text, "KP_3"), "a keypad digit");
    check(global_shortcut_trigger(key::grave, 0, text, sizeof(text)) && text_equal(text, "grave"), "the grave key");
    check(global_shortcut_trigger(key::page_up, 0, text, sizeof(text)) && text_equal(text, "Prior"), "page up is Prior");
    check(global_shortcut_trigger(key::enter, mod_alt, text, sizeof(text)) && text_equal(text, "ALT+Return"), "enter is Return");
    check(!global_shortcut_trigger(key::rbutton, 0, text, sizeof(text)) && text[0] == 0, "a mouse button has no trigger");
    check(!global_shortcut_trigger(key::ctrl, 0, text, sizeof(text)) && !global_shortcut_trigger(key::left_shift, 0, text, sizeof(text)) &&
              !global_shortcut_trigger(0xFF, 0, text, sizeof(text)),
          "a bare modifier or an unknown code has none");
    check(!global_shortcut_trigger('A', mod_ctrl | mod_alt | mod_shift | mod_super, text, 8), "a trigger that does not fit is refused");
}

void test_shortcuts() noexcept {
    test_trigger_strings();
    clear_log();
    const GlobalShortcut shortcut{"maicord-hold-to-talk", "Push to talk", "CTRL+ALT+p"};
    g_shortcut_event_count = 0;
    check(global_shortcuts_start(&shortcut, 1, on_shortcut, nullptr), "a session starts");
    check(pump(shortcuts_ready, 4000), "the session is created and the shortcut bound");
    take_log();
    const char* create = find_line("CreateSession");
    const char* bind = find_line("BindShortcuts");
    check(create && contains(create, "s:handle_token=v(s:maicord_") && contains(create, "s:session_handle_token=v(s:maicord_session_"),
          "CreateSession carries both tokens");
    check(bind && contains(bind, "o:/org/freedesktop/portal/desktop/session/") && contains(bind, "[(s:maicord-hold-to-talk,{s:description=v(s:Push to talk),s:preferred_trigger=v(s:CTRL+ALT+p)})] s: {s:handle_token=v(s:maicord_"),
          "BindShortcuts: the session, the shortcut with description and preferred trigger, no parent");
    send_shortcut_state("maicord-hold-to-talk", true);
    check(pump(shortcut_event_seen, 3000) && g_shortcut_events[0].active && g_shortcut_events[0].timestamp == 12345 &&
              text_equal(g_shortcut_events[0].id, "maicord-hold-to-talk"),
          "Activated reaches the handler with the id and the timestamp");
    send_shortcut_state("maicord-hold-to-talk", false);
    check(pump([]() { return g_shortcut_event_count >= 2; }, 3000) && !g_shortcut_events[1].active, "and Deactivated");
    global_shortcuts_stop();
    take_log();
    check(find_line("Session.Close /org/freedesktop/portal/desktop/session/") != nullptr && global_shortcuts_state() == GlobalShortcutsState::unavailable,
          "stopping closes the portal session");
    send_shortcut_state("maicord-hold-to-talk", true);
    settle(150);
    check(g_shortcut_event_count == 2, "nothing arrives after the stop");

    // The user declines the binding dialog.
    set_mode("bind-cancel", true);
    check(global_shortcuts_start(&shortcut, 1, on_shortcut, nullptr) && pump(shortcuts_settled, 4000) &&
              global_shortcuts_state() == GlobalShortcutsState::unavailable,
          "a cancelled binding leaves the shortcut unavailable");
    global_shortcuts_stop();
    set_mode("bind-cancel", false);

    // A portal without the interface.
    set_mode("shortcuts-error", true);
    check(global_shortcuts_start(&shortcut, 1, on_shortcut, nullptr) && pump(shortcuts_settled, 4000) &&
              global_shortcuts_state() == GlobalShortcutsState::unavailable,
          "a portal without GlobalShortcuts: unavailable");
    global_shortcuts_stop();
    set_mode("shortcuts-error", false);
    check(!global_shortcuts_start(nullptr, 0, on_shortcut, nullptr) && !global_shortcuts_start(&shortcut, 9, on_shortcut, nullptr),
          "no shortcuts, or too many, are refused");
}

void test_shortcuts_bare() noexcept {
    const GlobalShortcut shortcut{"maicord-hold-to-talk", "Push to talk", ""};
    check(global_shortcuts_start(&shortcut, 1, on_shortcut, nullptr), "the session starts on the bus");
    check(pump(shortcuts_settled, 4000) && global_shortcuts_state() == GlobalShortcutsState::unavailable,
          "no portal: unavailable, silently");
    global_shortcuts_stop();
}

// File chooser --------------------------------------------------------------------------------------------------------------

void set_file_result(u32 code, const char* const* uris, u32 count) noexcept {
    Builder b;
    builder_init(&b);
    builder_u32(&b, code);
    builder_open_array(&b, "s");
    for (u32 i = 0; i < count; ++i)
        builder_string(&b, uris[i]);
    builder_close_array(&b);
    control("SetFileResult", &b);
    builder_release(&b);
}

FileChooser* g_chooser;

bool chooser_settled() noexcept {
    return file_chooser_state(g_chooser) != FileChooserState::pending;
}

void test_file_uris() noexcept {
    char path[64];
    check(file_uri_to_path("file:///tmp/a%20b.txt", path, sizeof(path)) && text_equal(path, "/tmp/a b.txt"), "a file URI");
    check(file_uri_to_path("file://localhost/x/%C3%A9", path, sizeof(path)) && text_equal(path, "/x/\xC3\xA9"), "localhost and a UTF-8 escape");
    check(file_uri_to_path("FILE:///up", path, sizeof(path)) && text_equal(path, "/up"), "the scheme is case-insensitive");
    check(!file_uri_to_path("http://x/y", path, sizeof(path)) && !file_uri_to_path("file://otherhost/x", path, sizeof(path)) &&
              !file_uri_to_path("file:///bad%zz", path, sizeof(path)) && !file_uri_to_path("file:///nul%00", path, sizeof(path)) &&
              !file_uri_to_path("file://", path, sizeof(path)) && !file_uri_to_path("/plain", path, sizeof(path)),
          "other schemes, hosts, bad escapes and NULs are refused");
    check(!file_uri_to_path("file:///0123456789", path, 8), "a path that does not fit is refused");
}

void test_file_chooser() noexcept {
    test_file_uris();
    const char* const images[2] = {"*.png", "*.jpg"};
    const char* const all[1] = {"*"};
    const FileFilter filters[2] = {{"Images", images, 2}, {"All files", all, 1}};

    const char* const chosen[2] = {"file:///tmp/a%20b.txt", "file:///home/x/%C3%A9.png"};
    set_file_result(0, chosen, 2);
    clear_log();
    FileChooserOptions options{};
    options.title = "Pick files";
    options.filters = filters;
    options.filter_count = 2;
    options.multiple = true;
    options.accept_label = "Attach";
    g_chooser = file_chooser_open(options);
    check(g_chooser != nullptr, "an open dialog starts");
    check(g_chooser && file_chooser_state(g_chooser) == FileChooserState::pending, "it is pending until the portal answers");
    check(g_chooser && pump(chooser_settled, 4000) && file_chooser_state(g_chooser) == FileChooserState::done, "it completes");
    check(g_chooser && file_chooser_count(g_chooser) == 2 && text_equal(file_chooser_path(g_chooser, 0), "/tmp/a b.txt") &&
              text_equal(file_chooser_path(g_chooser, 1), "/home/x/\xC3\xA9.png") && !file_chooser_path(g_chooser, 2),
          "the paths are decoded, in order");
    take_log();
    const char* open = find_line("OpenFile");
    check(open && contains(open, "OpenFile s: s:Pick files {") && contains(open, "s:multiple=v(b:1)") && contains(open, "s:directory=v(b:0)") &&
              contains(open, "s:modal=v(b:1)") && contains(open, "s:accept_label=v(s:Attach)") &&
              contains(open, "s:filters=v([(s:Images,[(u:0,s:*.png),(u:0,s:*.jpg)]),(s:All files,[(u:0,s:*)])])") &&
              contains(open, "s:handle_token=v(s:maicord_"),
          "OpenFile's title and options as libdbus decoded them: multiple, modal, label, filters, token");
    file_chooser_release(g_chooser);

    // Save.
    const char* const saved[1] = {"file:///tmp/out.png"};
    set_file_result(0, saved, 1);
    clear_log();
    FileChooserOptions save{};
    save.current_name = "out.png";
    save.current_folder = "/tmp";
    save.filters = filters;
    save.filter_count = 1;
    g_chooser = file_chooser_save(save);
    check(g_chooser && pump(chooser_settled, 4000) && file_chooser_state(g_chooser) == FileChooserState::done &&
              file_chooser_count(g_chooser) == 1 && text_equal(file_chooser_path(g_chooser, 0), "/tmp/out.png"),
          "a save dialog returns the destination");
    take_log();
    const char* save_line = find_line("SaveFile");
    check(save_line && contains(save_line, "SaveFile s: s:Save File {") && contains(save_line, "s:current_name=v(s:out.png)") &&
              contains(save_line, "s:current_folder=v(ay:2f746d7000)") && !contains(save_line, "s:multiple"),
          "SaveFile: the name, the folder as bytes with their NUL, no open-only options");
    file_chooser_release(g_chooser);

    // The user cancels in the dialog.
    set_file_result(1, nullptr, 0);
    FileChooserOptions plain{};
    g_chooser = file_chooser_open(plain);
    check(g_chooser && pump(chooser_settled, 4000) && file_chooser_state(g_chooser) == FileChooserState::cancelled &&
              file_chooser_count(g_chooser) == 0,
          "response 1 is cancelled");
    file_chooser_release(g_chooser);

    // Something that is not a file, and too many files.
    const char* const web[1] = {"https://example.org/x"};
    set_file_result(0, web, 1);
    g_chooser = file_chooser_open(plain);
    check(g_chooser && pump(chooser_settled, 4000) && file_chooser_state(g_chooser) == FileChooserState::failed, "a non-file URI fails");
    file_chooser_release(g_chooser);
    static const char* many[kFileChooserMaximumFiles + 1];
    for (u32 i = 0; i < kFileChooserMaximumFiles + 1; ++i)
        many[i] = "file:///tmp/f";
    set_file_result(0, many, kFileChooserMaximumFiles + 1);
    g_chooser = file_chooser_open(plain);
    check(g_chooser && pump(chooser_settled, 4000) && file_chooser_state(g_chooser) == FileChooserState::capacity &&
              file_chooser_count(g_chooser) == 0,
          "more than 64 files is capacity, nothing partial");
    file_chooser_release(g_chooser);
    set_file_result(0, many, kFileChooserMaximumFiles);
    g_chooser = file_chooser_open(plain);
    check(g_chooser && pump(chooser_settled, 4000) && file_chooser_state(g_chooser) == FileChooserState::done &&
              file_chooser_count(g_chooser) == kFileChooserMaximumFiles,
          "exactly 64 is fine");
    file_chooser_release(g_chooser);

    // Cancelling a dialog that is up closes the portal request; releasing it at once is safe.
    set_mode("chooser-hold", true);
    clear_log();
    g_chooser = file_chooser_open(plain);
    settle(150);
    check(g_chooser && file_chooser_state(g_chooser) == FileChooserState::pending, "a dialog the portal does not answer stays pending");
    if (g_chooser)
        file_chooser_cancel(g_chooser);
    take_log();
    check(g_chooser && file_chooser_state(g_chooser) == FileChooserState::cancelled && find_line("Request.Close /org/freedesktop/portal/desktop/request/") != nullptr,
          "cancel closes the request and reads as cancelled");
    file_chooser_release(g_chooser);
    g_chooser = file_chooser_open(plain);
    file_chooser_release(g_chooser);  // before the portal's reply: the handler frees it
    settle(300);
    set_mode("chooser-hold", false);
    settle(100);
    check(true, "a chooser released before the reply is freed without a crash");
}

void test_file_chooser_bare() noexcept {
    FileChooserOptions plain{};
    g_chooser = file_chooser_open(plain);
    check(g_chooser != nullptr, "the call is made");
    check(g_chooser && pump(chooser_settled, 4000) && file_chooser_state(g_chooser) == FileChooserState::failed,
          "no portal: failed");
    file_chooser_release(g_chooser);
}

// Launcher entry --------------------------------------------------------------------------------------------------------------

void test_launcher_entry() noexcept {
    clear_log();
    check(launcher_entry_update("io.github.maj113.MaiCord.desktop", LauncherEntryState{7, true, false}), "an update is sent");
    settle(150);
    take_log();
    const char* line = find_line("LauncherEntry");
    check(line && contains(line, "s:application://io.github.maj113.MaiCord.desktop {s:count-visible=v(b:1),s:count=v(x:7),s:urgent=v(b:0)} /com/canonical/unity/launcherentry/"),
          "the Update signal: the application URI, count, visibility and urgency as libdbus decoded them");
    if (!line)
        dump_log();
    clear_log();
    check(launcher_entry_update("io.github.maj113.MaiCord.desktop", LauncherEntryState{12345678901ll, true, true}), "a large count, urgent");
    settle(150);
    take_log();
    line = find_line("LauncherEntry");
    check(line && contains(line, "s:count-visible=v(b:1),s:count=v(x:12345678901),s:urgent=v(b:1)"), "64-bit counts and the urgent flag");
    clear_log();
    check(launcher_entry_update("io.github.maj113.MaiCord.desktop", LauncherEntryState{}), "cleared");
    settle(150);
    take_log();
    line = find_line("LauncherEntry");
    check(line && contains(line, "s:count-visible=v(b:0),s:count=v(x:0),s:urgent=v(b:0)"), "the badge is hidden again");
    check(!launcher_entry_update(nullptr, LauncherEntryState{}), "no desktop id is refused");
}

// Single instance --------------------------------------------------------------------------------------------------------------

LaunchRequest g_requests[8];
u32 g_request_count;

void on_launch(void*, const LaunchRequest& request) noexcept {
    if (g_request_count < 8)
        g_requests[g_request_count++] = request;
}

bool request_arrived() noexcept {
    return g_request_count != 0;
}

int spawn_self(const char* const* arguments) noexcept {
    static SharedLibrary libc;
    using Spawn = int (*)(int*, const char*, const void*, const void*, char* const*, char* const*);
    static Spawn spawn;
    if (!spawn && shared_library_open(&libc, "libc.so.6"))
        spawn = reinterpret_cast<Spawn>(shared_library_symbol(libc, "posix_spawn"));
    if (!spawn)
        return -1;
    int pid = 0;
    char* argv[8] = {};
    for (u32 i = 0; arguments[i] && i < 7; ++i)
        argv[i] = const_cast<char*>(arguments[i]);
    return spawn(&pid, process_startup().argv[0], nullptr, nullptr, argv, process_startup().environment) == 0 ? pid : -1;
}

// The child's exit code once it is done (WNOHANG while the session bus is pumped); -1 on timeout.
int g_child;
int g_child_status = -1;

bool child_done() noexcept {
    if (g_child_status >= 0)
        return true;
    int status = 0;
    const long waited = syscall4(61, g_child, reinterpret_cast<long>(&status), 1 /* WNOHANG */, 0);
    if (waited == g_child) {
        g_child_status = (status & 0x7F) ? 100 + (status & 0x7F) : (status >> 8) & 0xFF;
        return true;
    }
    return false;
}

void test_instance_names() noexcept {
    char name[160];
    char path[200];
    check(instance_bus_name(kApplicationId, nullptr, name, sizeof(name), path, sizeof(path)) && text_equal(name, kApplicationId) &&
              text_equal(path, "/io/github/maj113/MaiCord"),
          "the default profile is the application id, its path the id with slashes");
    char name_a[160], path_a[200], name_b[160], path_b[200], name_c[160], path_c[200];
    check(instance_bus_name(kApplicationId, "a", name_a, sizeof(name_a), path_a, sizeof(path_a)) &&
              instance_bus_name(kApplicationId, "b", name_b, sizeof(name_b), path_b, sizeof(path_b)) &&
              instance_bus_name(kApplicationId, "a", name_c, sizeof(name_c), path_c, sizeof(path_c)) &&
              starts_with(name_a, "io.github.maj113.MaiCord.p") && text_length(name_a) == text_length(kApplicationId) + 18 &&
              !text_equal(name_a, name_b) && text_equal(name_a, name_c) && !text_equal(path_a, path_b) && path_a[0] == '/',
          "another profile has a name and a path of its own, stable for a key");
    check(instance_bus_name(kApplicationId, "", name, sizeof(name), path, sizeof(path)) && text_equal(name, kApplicationId), "an empty key is the default");
    check(!instance_bus_name("nodots", nullptr, name, sizeof(name), path, sizeof(path)) &&
              !instance_bus_name("io..x", nullptr, name, sizeof(name), path, sizeof(path)) &&
              !instance_bus_name("io.9x", nullptr, name, sizeof(name), path, sizeof(path)) &&
              !instance_bus_name("io.x y", nullptr, name, sizeof(name), path, sizeof(path)) &&
              !instance_bus_name("io.x.", nullptr, name, sizeof(name), path, sizeof(path)) &&
              !instance_bus_name(nullptr, nullptr, name, sizeof(name), path, sizeof(path)) &&
              !instance_bus_name(kApplicationId, nullptr, name, 8, path, sizeof(path)),
          "invalid ids and small buffers are refused");
    char dashed_name[160], dashed_path[200];
    check(instance_bus_name("org.example.my-app", nullptr, dashed_name, sizeof(dashed_name), dashed_path, sizeof(dashed_path)) &&
              text_equal(dashed_path, "/org/example/my_app"),
          "a dash in the id becomes an underscore in the object path");
}

void unique_key(char (&key)[32], const char* prefix) noexcept {
    u32 length = text_length(prefix);
    copy_bytes(key, prefix, length);
    char digits[12];
    u32 count = 0;
    for (u32 n = process_id(); n || count == 0; n /= 10)
        digits[count++] = static_cast<char>('0' + n % 10);
    while (count)
        key[length++] = digits[--count];
    key[length] = 0;
}

// The primary on the bus: requests from the mock (as a launcher would make them) and from a second process.
void test_instance_bus() noexcept {
    test_instance_names();
    char key[32];
    unique_key(key, "bus");
    const InstanceConfig config{kApplicationId, key};
    LaunchRequest none{};
    check(instance_acquire(InstanceConfig{"nodots", nullptr}, none, 500) == InstanceResult::unguarded, "an invalid id is unguarded");
    check(instance_acquire(config, none, 2000) == InstanceResult::primary, "the first process becomes the primary");
    check(instance_acquire(config, none, 2000) == InstanceResult::primary, "asking again changes nothing");

    char name[160], path[200];
    instance_bus_name(kApplicationId, key, name, sizeof(name), path, sizeof(path));
    // A request that arrives before the handler is set waits for it.
    const char* const uris[1] = {"discord://-/invite/early"};
    Builder b;
    invoke_begin(&b, "early", name, path, "org.freedesktop.Application", "Open", "asa{sv}");
    arg_string_array(&b, uris, 1);
    arg_platform_data(&b, "token-early");
    invoke_end(&b);
    check(result_is("early", "ok "), "Open is answered");
    g_request_count = 0;
    instance_set_handler(on_launch, nullptr);
    check(g_request_count == 1 && g_requests[0].url_present && text_equal(g_requests[0].url, "discord://-/invite/early") &&
              text_equal(g_requests[0].activation_token, "token-early"),
          "the early request is delivered when the handler is set, with its activation token");

    g_request_count = 0;
    const char* const two[2] = {"discord://-/channels/1/2", "discord://-/invite/ignored"};
    invoke_begin(&b, "open", name, path, "org.freedesktop.Application", "Open", "asa{sv}");
    arg_string_array(&b, two, 2);
    arg_platform_data(&b, nullptr);
    invoke_end(&b);
    check(result_is("open", "ok ") && g_request_count == 1 && text_equal(g_requests[0].url, "discord://-/channels/1/2") &&
              g_requests[0].activation_token[0] == 0,
          "Open takes the first URI");
    g_request_count = 0;
    invoke_begin(&b, "activate", name, path, "org.freedesktop.Application", "Activate", "a{sv}");
    arg_platform_data(&b, "token-activate");
    invoke_end(&b);
    check(result_is("activate", "ok ") && g_request_count == 1 && !g_requests[0].url_present &&
              text_equal(g_requests[0].activation_token, "token-activate"),
          "Activate asks for the window, with no URL");
    g_request_count = 0;
    invoke_begin(&b, "action", name, path, "org.freedesktop.Application", "ActivateAction", "sava{sv}");
    arg_string(&b, "open");
    builder_open_variant(&b, "av");
    builder_open_array(&b, "v");
    builder_close_array(&b);
    builder_close_variant(&b);
    arg_platform_data(&b, nullptr);
    invoke_end(&b);
    check(result_is("action", "ok ") && g_request_count == 1 && !g_requests[0].url_present, "ActivateAction brings the window forward");
    invoke_begin(&b, "intro", name, path, "org.freedesktop.DBus.Introspectable", "Introspect", "");
    invoke_end(&b);
    const char* intro = wait_result("intro", 3000);
    check(intro && contains(intro, "org.freedesktop.Application") && contains(intro, "name=\"Open\""), "Introspect describes the application interface");

    // A second process forwards its link and exits.
    g_request_count = 0;
    const char* const arguments[] = {process_startup().argv[0], "--secondary", key, "discord://-/channels/9/8/7", nullptr};
    g_child = spawn_self(arguments);
    g_child_status = -1;
    check(g_child > 0, "a second launch starts");
    check(pump(request_arrived, 8000) && g_requests[0].url_present && text_equal(g_requests[0].url, "discord://-/channels/9/8/7"),
          "the second launch's link reaches the first instance");
    check(pump(child_done, 8000) && g_child_status == 0, "and the second launch exits as forwarded");

    // The same without a link: it asks the first instance to show itself.
    g_request_count = 0;
    const char* const show[] = {process_startup().argv[0], "--secondary", key, "", nullptr};
    g_child = spawn_self(show);
    g_child_status = -1;
    check(pump(request_arrived, 8000) && !g_requests[0].url_present, "a launch without a link activates the window");
    check(pump(child_done, 8000) && g_child_status == 0, "and exits");

    instance_release();
    invoke_begin(&b, "released", name, path, "org.freedesktop.DBus.Introspectable", "Introspect", "");
    invoke_end(&b);
    const char* released = wait_result("released", 3000);
    check(released && contains(released, " error org.freedesktop.DBus.Error.ServiceUnknown"), "releasing gives the name up");
}

// The socket backend: no bus needed.
void test_instance_socket() noexcept {
    char key[32];
    unique_key(key, "sock");
    const InstanceConfig config{kApplicationId, key};
    LaunchRequest none{};
    check(instance_acquire(config, none, 2000) == InstanceResult::primary, "the first process binds the socket");
    check(instance_waitable() != nullptr, "the socket has a waitable");
    g_request_count = 0;
    instance_set_handler(on_launch, nullptr);

    const char* const arguments[] = {process_startup().argv[0], "--secondary", key, "discord://-/invite/socket", nullptr};
    g_child = spawn_self(arguments);
    g_child_status = -1;
    check(g_child > 0, "a second launch starts");
    const u64 deadline = clock_monotonic_milliseconds() + 8000;
    while (clock_monotonic_milliseconds() < deadline && (g_request_count == 0 || g_child_status < 0)) {
        Waitable waitable = instance_waitable();
        wait_many(&waitable, 1, 50, 0);
        instance_process();
        child_done();
    }
    check(g_request_count == 1 && g_requests[0].url_present && text_equal(g_requests[0].url, "discord://-/invite/socket"),
          "the link arrives through the socket");
    check(g_child_status == 0, "the second launch exits as forwarded");

    // Raw clients: a request in the protocol, then garbage.
    SocketAddress address;
    instance_socket_address(kApplicationId, key, &address);
    const auto connect_raw = [&]() -> int {
        const long created = syscall3(41, 1, 1 | 0x80000, 0);
        if (syscall_failed(created))
            return -1;
        if (syscall_failed(syscall3(42, static_cast<int>(created), reinterpret_cast<long>(address.data), address.length))) {
            sys_close(static_cast<int>(created));
            return -1;
        }
        return static_cast<int>(created);
    };
    const auto exchange = [&](const u8* bytes, u32 count) -> int {
        const int fd = connect_raw();
        if (fd < 0)
            return -2;
        sys_write(fd, bytes, count);
        // The answer comes from the primary's processing: pump it while waiting.
        u8 answer = 0;
        long got = -kErrorAgain;
        for (u32 i = 0; i < 60 && got == -kErrorAgain; ++i) {
            instance_process();
            PollFd poll{fd, kPollIn, 0};
            const TimeSpec nap{0, 10 * 1000000};
            sys_ppoll(&poll, 1, &nap);
            got = sys_read(fd, &answer, 1);
        }
        sys_close(fd);
        return got == 1 ? answer : -1;
    };
    g_request_count = 0;
    u8 good[12 + 3 + 2] = {'M', 'C', 'I', '1', 2, 3, 0, 2, 0, 0, 0, 0, 'a', 'b', 'c', 't', 'k'};
    check(exchange(good, sizeof(good)) == 'K' && g_request_count == 1 && g_requests[0].url_bytes == 3 &&
              text_equal(g_requests[0].url, "abc") && text_equal(g_requests[0].activation_token, "tk"),
          "a request in the socket protocol is answered and delivered");
    g_request_count = 0;
    u8 bad_magic[12] = {'X', 'X', 'X', 'X', 2, 0, 0, 0, 0, 0, 0, 0};
    u8 huge_url[12] = {'M', 'C', 'I', '1', 2, 0xFF, 0xFF, 0, 0, 0, 0, 0};
    u8 truncated[14] = {'M', 'C', 'I', '1', 2, 20, 0, 0, 0, 0, 0, 0, 'a', 'b'};
    check(exchange(bad_magic, sizeof(bad_magic)) == -1 && exchange(huge_url, sizeof(huge_url)) == -1 &&
              exchange(truncated, sizeof(truncated)) == -1 && g_request_count == 0,
          "a wrong magic, an oversized length and a cut-off request are dropped without a reply or a delivery");
    check(exchange(good, sizeof(good)) == 'K', "and the instance still serves afterwards");

    // A request with no handler yet is kept.
    instance_release();
    check(instance_waitable() == nullptr, "released: no waitable");
    check(instance_acquire(config, none, 2000) == InstanceResult::primary, "the name is free again");
    instance_set_handler(nullptr, nullptr);
    g_request_count = 0;
    check(exchange(good, sizeof(good)) == 'K' && g_request_count == 0, "with no handler the request waits");
    instance_set_handler(on_launch, nullptr);
    check(g_request_count == 1 && text_equal(g_requests[0].url, "abc"), "and is delivered when the handler is set");
    instance_release();
}

int run_secondary(const char* key, const char* url) noexcept {
    LaunchRequest request{};
    request.url_bytes = text_length(url);
    request.url_present = request.url_bytes != 0;
    copy_bytes(request.url, url, request.url_bytes + 1);
    const InstanceConfig config{kApplicationId, key};
    switch (instance_acquire(config, request, 8000)) {
    case InstanceResult::forwarded:
        return 0;
    case InstanceResult::primary:
        return 3;
    case InstanceResult::refused:
        return 4;
    default:
        return 5;
    }
}

bool bus_gate(const char* kind) noexcept {
    const char* bus = environment_get("MAICORD_DBUS_TEST_BUS");
    const char* address = environment_get("MAICORD_DBUS_TEST_ADDRESS");
    const char* session = environment_get("DBUS_SESSION_BUS_ADDRESS");
    return bus && address && session && text_equal(bus, kind) && text_equal(address, session);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_dbus_desktop_entry) {
    using namespace maicord;
    using namespace maicord::mock;
    const ProcessStartup& startup = process_startup();
    const char* mode = startup.argc > 1 ? startup.argv[1] : "";
    if (text_equal(mode, "--secondary") && startup.argc >= 4)
        test_process_exit(static_cast<u32>(run_secondary(startup.argv[2], startup.argv[3])));
    if (text_equal(mode, "--socket")) {
        if (!environment_get("MAICORD_INSTANCE_BACKEND") || !text_equal(environment_get("MAICORD_INSTANCE_BACKEND"), "socket")) {
            test_write_line("SKIP dbus_desktop --socket (MAICORD_INSTANCE_BACKEND=socket)");
            test_process_exit(77);
        }
        test_instance_socket();
        test_exit(g_failures, "dbus_desktop_socket");
    }
    const bool bare = text_equal(mode, "--bare");
    if (!bus_gate(bare ? "bare" : "mock")) {
        test_write_line("SKIP dbus_desktop (the private bus is not set up: tools/test_dbus.sh)");
        test_process_exit(77);
    }
    if (bare) {
        test_tray_bare();
        test_shortcuts_bare();
        test_file_chooser_bare();
        check(launcher_entry_update("io.github.maj113.MaiCord.desktop", LauncherEntryState{1, true, false}), "the launcher entry is sent on any bus");
        test_exit(g_failures, "dbus_desktop_bare");
    }
    g_control = connection_open_address(environment_get("MAICORD_DBUS_TEST_ADDRESS"));
    check(g_control != nullptr, "a control connection to the mock");
    if (!g_control)
        test_exit(g_failures, "dbus_desktop");
    control("Reset", nullptr);
    test_tray();
    test_shortcuts();
    test_file_chooser();
    test_launcher_entry();
    test_instance_bus();
    connection_close(g_control);
    session_bus_close();
    test_exit(g_failures, "dbus_desktop");
}
