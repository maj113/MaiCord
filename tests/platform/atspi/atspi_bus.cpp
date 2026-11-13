#include "maicord/core/time.hpp"
#include "maicord/platform/atspi/server.hpp"
#include "maicord/platform/dbus/bus.hpp"
#include "maicord/platform/dbus/session.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"
#include "../../support/test_output.hpp"

// The AT-SPI server's life on a bus (tools/test_dbus.sh, the bare private bus): it stays disconnected while org.a11y.Status
// says nobody wants accessibility, asks org.a11y.Bus for the accessibility bus when IsEnabled turns on, registers its root
// with the registry (Socket.Embed), answers a client, sends the events a snapshot difference calls for, leaves when the
// flags go off, and embeds again when the registry comes back. This program plays org.a11y.Bus and the registry itself, on
// the same private bus the server's session connection uses (the "accessibility bus" is that bus), and a screen reader
// that calls and listens on a third connection; the process pumps all of them. Without MAICORD_DBUS_TEST_BUS naming the
// run's bus (and DBUS_SESSION_BUS_ADDRESS = MAICORD_DBUS_TEST_ADDRESS) the test skips (77).

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

// The window the server publishes ------------------------------------------------------------------------------------------

UiAccessNode g_nodes[16];
char g_text[1024];
u32 g_slots[32];
u32 g_keys[32];
UiAccessTree g_tree;
u32 g_entry = kUiAccessNone;
bool g_active = true;
u32 g_acts;

constexpr u8 kSurface = 7;

u32 add(u32 parent, u64 identity, UiAccessRole role, const char* name, UiRect rectangle, u16 state, u16 actions,
        const char* value) noexcept {
    UiAccessNodeDesc desc{};
    desc.key = ui_access_key(kSurface, identity);
    desc.rectangle = rectangle;
    desc.role = role;
    u32 length = 0;
    while (name[length])
        ++length;
    desc.name = {name, length};
    length = 0;
    while (value && value[length])
        ++length;
    desc.value = {value, length};
    desc.state = state;
    desc.actions = actions;
    desc.surface = kSurface;
    return ui_access_add(&g_tree, parent, desc);
}

bool build_window() noexcept {
    if (!ui_access_tree_initialize(&g_tree, {g_nodes, 16}, {g_text, sizeof(g_text)}, {g_slots, 32}, {g_keys, 32}) ||
        !ui_access_tree_reset(&g_tree, utf8_literal("MaiCord"), {0, 0, 400, 300}))
        return false;
    const u32 dialog = add(0, 1, UiAccessRole::dialog, "Log in", {20, 20, 380, 280}, 0, 0, nullptr);
    g_entry = add(dialog, 2, UiAccessRole::edit, "Email", {30, 40, 370, 70}, ui_access_focusable | ui_access_focused,
                  ui_access_action_value | ui_access_action_text, "mai");
    add(dialog, 3, UiAccessRole::button, "Continue", {30, 90, 370, 120}, ui_access_focusable, ui_access_action_invoke,
        nullptr);
    return ui_access_tree_finish(&g_tree);
}

const UiAccessTree* host_tree(void*) noexcept {
    return &g_tree;
}

bool host_act(void*, u64, UiAccessAction, double, Utf8View) noexcept {
    ++g_acts;
    return true;
}

float host_content_top(void*) noexcept {
    return 0.0f;
}

bool host_window_active(void*) noexcept {
    return g_active;
}

const atspi::HostCallbacks kHost{nullptr, host_tree, host_act, host_content_top, host_window_active, nullptr};

// The fake services ---------------------------------------------------------------------------------------------------------

Connection* g_services;  // owns org.a11y.Bus and org.a11y.atspi.Registry
Connection* g_client;    // the screen reader
bool g_enabled;
bool g_screen_reader;
u32 g_status_reads;
u32 g_address_calls;
u32 g_embeds;
char g_app_name[64];
char g_app_path[96];

bool services_call(void*, Connection* c, const Message& call) noexcept {
    if (!call.path || !call.interface || !call.member)
        return false;
    Builder body;
    builder_init(&body);
    bool handled = true;
    if (text_equal(call.interface, "org.freedesktop.DBus.Properties") && text_equal(call.member, "GetAll")) {
        ++g_status_reads;
        builder_open_array(&body, "{sv}");
        builder_open_entry(&body);
        builder_string(&body, "IsEnabled");
        builder_open_variant(&body, "b");
        builder_bool(&body, g_enabled);
        builder_close_variant(&body);
        builder_close_entry(&body);
        builder_open_entry(&body);
        builder_string(&body, "ScreenReaderEnabled");
        builder_open_variant(&body, "b");
        builder_bool(&body, g_screen_reader);
        builder_close_variant(&body);
        builder_close_entry(&body);
        builder_close_array(&body);
    } else if (text_equal(call.interface, "org.a11y.Bus") && text_equal(call.member, "GetAddress")) {
        ++g_address_calls;
        builder_string(&body, environment_get("DBUS_SESSION_BUS_ADDRESS"));
    } else if (text_equal(call.interface, "org.a11y.atspi.Socket") && text_equal(call.member, "Embed")) {
        Reader r;
        message_reader(call, &r);
        const char* name = nullptr;
        const char* path = nullptr;
        if (reader_enter_struct(&r) && reader_string(&r, &name, nullptr) && reader_string(&r, &path, nullptr)) {
            u32 n = 0;
            while (name[n] && n < sizeof(g_app_name) - 1)
                g_app_name[n] = name[n], ++n;
            g_app_name[n] = 0;
            n = 0;
            while (path[n] && n < sizeof(g_app_path) - 1)
                g_app_path[n] = path[n], ++n;
            g_app_path[n] = 0;
        }
        ++g_embeds;
        builder_open_struct(&body, "(so)");
        builder_string(&body, connection_unique_name(g_services));
        builder_object_path(&body, "/org/a11y/atspi/accessible/root");
        builder_close_struct(&body);
    } else {
        handled = false;
    }
    if (handled)
        connection_reply(c, call, &body);
    builder_release(&body);
    return handled;
}

// org.a11y.Status changed: what the desktop does when the switch or a screen reader turns accessibility on or off.
void announce_status() noexcept {
    Builder body;
    builder_init(&body);
    builder_string(&body, "org.a11y.Status");
    builder_open_array(&body, "{sv}");
    builder_close_array(&body);
    builder_open_array(&body, "s");
    builder_close_array(&body);
    connection_emit(g_services, "/org/a11y/bus", "org.freedesktop.DBus.Properties", "PropertiesChanged", &body);
    builder_release(&body);
}

// The screen reader's view -------------------------------------------------------------------------------------------------

struct Event {
    char member[32];
    char detail[32];
    i32 detail1, detail2;
    char text[64];
};
Event g_events[32];
u32 g_event_count;

void on_event(void*, const Message& signal) noexcept {
    if (g_event_count >= 32 || !signal.member)
        return;
    Event& e = g_events[g_event_count];
    e = Event{};
    u32 n = 0;
    while (signal.member[n] && n < sizeof(e.member) - 1)
        e.member[n] = signal.member[n], ++n;
    Reader r;
    message_reader(signal, &r);
    const char* detail = nullptr;
    if (!reader_string(&r, &detail, nullptr) || !reader_i32(&r, &e.detail1) || !reader_i32(&r, &e.detail2))
        return;
    n = 0;
    while (detail[n] && n < sizeof(e.detail) - 1)
        e.detail[n] = detail[n], ++n;
    const char* signature = nullptr;
    if (reader_enter_variant(&r, &signature) && signature && text_equal(signature, "s")) {
        const char* text = nullptr;
        if (reader_string(&r, &text, nullptr)) {
            n = 0;
            while (text[n] && n < sizeof(e.text) - 1)
                e.text[n] = text[n], ++n;
        }
    }
    ++g_event_count;
}

const Event* find_event(const char* member, const char* detail) noexcept {
    for (u32 i = 0; i < g_event_count; ++i)
        if (text_equal(g_events[i].member, member) && (!detail || text_equal(g_events[i].detail, detail)))
            return &g_events[i];
    return nullptr;
}

u32 g_children_reply;
void on_children(void*, Status status, const Message* reply) noexcept {
    if (status != Status::ok || !reply)
        return;
    Reader r;
    message_reader(*reply, &r);
    if (reader_enter_array(&r))
        while (reader_more(r) && reader_skip(&r))
            ++g_children_reply;
}

bool g_action_answered, g_action_result;
void on_action(void*, Status status, const Message* reply) noexcept {
    if (status != Status::ok || !reply)
        return;
    Reader r;
    message_reader(*reply, &r);
    g_action_answered = reader_bool(&r, &g_action_result);
}

// Pumping -------------------------------------------------------------------------------------------------------------------

void step(u32 milliseconds) noexcept {
    const TimeSpec nap{0, static_cast<i64>(milliseconds) * 1000000};
    sys_ppoll(nullptr, 0, &nap);
    const u64 now = clock_monotonic_milliseconds();
    if (g_services)
        connection_process(g_services, now);
    if (g_client)
        connection_process(g_client, now);
    session_bus_process(now);  // the session connection and, through the extra, the accessibility connection
}

template <typename Done> bool pump(Done done, u32 milliseconds) noexcept {
    const u64 deadline = clock_monotonic_milliseconds() + milliseconds;
    while (!done()) {
        if (clock_monotonic_milliseconds() >= deadline)
            return false;
        step(5);
    }
    return true;
}

void settle(u32 milliseconds) noexcept {
    const u64 until = clock_monotonic_milliseconds() + milliseconds;
    while (clock_monotonic_milliseconds() < until)
        step(5);
}

Waitable loop_waitable() noexcept {
    return atspi::server_waitable();
}
u32 loop_wait_ms(u64 now) noexcept {
    return atspi::server_wait_ms(now);
}
void loop_process(u64 now, bool readable) noexcept {
    atspi::server_process(now, readable);
}

bool skip_unless(const char* kind) noexcept {
    const char* bus = environment_get("MAICORD_DBUS_TEST_BUS");
    const char* address = environment_get("MAICORD_DBUS_TEST_ADDRESS");
    const char* session = environment_get("DBUS_SESSION_BUS_ADDRESS");
    return bus && address && session && text_equal(bus, kind) && text_equal(address, session);
}

bool connected() noexcept {
    return atspi::server_connected();
}
bool embedded_once() noexcept {
    return g_embeds >= 1;
}
bool disconnected() noexcept {
    return !atspi::server_connected();
}
bool embedded_twice() noexcept {
    return g_embeds >= 2 && atspi::server_connected();
}
bool embedded_thrice() noexcept {
    return g_embeds >= 3 && atspi::server_connected();
}
bool queried() noexcept {
    return atspi::server_queried();
}
bool got_children() noexcept {
    return g_children_reply != 0;
}
bool got_action() noexcept {
    return g_action_answered;
}
bool got_events() noexcept {
    return find_event("TextChanged", "insert") && find_event("Announcement", "");
}

void run() noexcept {
    check(build_window(), "the synthetic window builds");
    g_services = connection_open_address(environment_get("MAICORD_DBUS_TEST_ADDRESS"));
    g_client = connection_open_address(environment_get("MAICORD_DBUS_TEST_ADDRESS"));
    check(g_services && g_client, "the services and the client connect");
    if (!g_services || !g_client)
        return;
    connection_set_method_handler(g_services, services_call, nullptr);
    check(connection_request_name(g_services, "org.a11y.Bus", 3000) &&
              connection_request_name(g_services, "org.a11y.atspi.Registry", 3000),
          "the fake org.a11y.Bus and registry own their names");
    connection_subscribe(g_client, SignalMatch{nullptr, nullptr, "org.a11y.atspi.Event.Object", nullptr, nullptr},
                         on_event, nullptr);
    connection_subscribe(g_client, SignalMatch{nullptr, nullptr, "org.a11y.atspi.Event.Window", nullptr, nullptr},
                         on_event, nullptr);
    connection_subscribe(g_client, SignalMatch{nullptr, nullptr, "org.a11y.atspi.Event.Focus", nullptr, nullptr},
                         on_event, nullptr);
    session_bus_add_extra({loop_waitable, loop_wait_ms, loop_process});

    // 1. Nobody wants accessibility: the server reads the status and stays away.
    check(atspi::server_start(kHost), "the server starts");
    pump([] { return g_status_reads >= 1; }, 3000);
    settle(300);
    check(g_status_reads >= 1 && g_address_calls == 0 && !connected() && !atspi::server_waitable(),
          "with accessibility off the server asks no address and holds no connection");

    // 2. The desktop's switch turns on: the accessibility bus, then the registry.
    g_enabled = true;
    announce_status();
    check(pump(embedded_once, 3000), "turning IsEnabled on registers the application with the registry");
    check(g_address_calls == 1 && pump(connected, 1000) && atspi::server_waitable() != nullptr,
          "it asked org.a11y.Bus for the address once and connected");
    check(g_app_name[0] == ':' && text_equal(g_app_path, "/org/a11y/atspi/accessible/root"),
          "the plug it embeds is its connection's root");
    check(!atspi::server_queried(), "the registry's own calls do not count as a client");

    // 3. A client calls: the server answers, and only then is it worth sending events.
    {
        const CallTarget target{g_app_name, "/org/a11y/atspi/accessible/root", "org.a11y.atspi.Accessible", "GetChildren"};
        check(connection_call(g_client, target, nullptr, 3000, on_children, nullptr), "the client calls GetChildren");
        check(pump(got_children, 3000) && g_children_reply == 1, "the application root has the window as its one child");
        check(pump(queried, 1000), "a client's call makes the server worth publishing to");
        // An action reaches the host: DoAction on the button.
        char button[atspi::kPathBytes];
        atspi::path_for_key(ui_access_key(kSurface, 3), button);
        Builder args;
        builder_init(&args);
        builder_i32(&args, 0);
        const CallTarget act{g_app_name, button, "org.a11y.atspi.Action", "DoAction"};
        check(connection_call(g_client, act, &args, 3000, on_action, nullptr), "the client calls DoAction on the button");
        builder_release(&args);
        check(pump(got_action, 3000) && g_action_result && g_acts == 1, "the action reaches the host and is accepted");
    }

    // 4. The events a snapshot difference calls for.
    {
        UiAccessEventPlan plan{};
        plan.focus_node = g_entry;
        plan.focus_changed = true;
        plan.property_count = 1;
        plan.properties[0] = {g_entry, UiAccessProperty::value, 0, 0};
        atspi::server_publish(g_tree, plan);
        atspi::server_announce(0, utf8_literal("Reconnecting"), false);
        check(pump(got_events, 3000), "the text insertion and the announcement arrive");
        const Event* insert = find_event("TextChanged", "insert");
        check(insert && insert->detail1 == 0 && insert->detail2 == 3 && text_equal(insert->text, "mai"),
              "the insertion carries the offset, the length and the text");
        const Event* announcement = find_event("Announcement", "");
        check(announcement && announcement->detail1 == 1 && text_equal(announcement->text, "Reconnecting"),
              "a polite announcement carries its text");
        check(find_event("Activate", nullptr) != nullptr && find_event("StateChanged", "active") != nullptr,
              "the window's activation is told on the first publish");
        const Event* focused = find_event("StateChanged", "focused");
        check(focused && focused->detail1 == 1, "the focus is told");
        check(find_event("Focus", nullptr) != nullptr, "the legacy focus event follows");
        // The window deactivates: that is told, and the focus leaves with it.
        g_event_count = 0;
        UiAccessEventPlan later{};
        later.focus_node = kUiAccessNone;
        g_active = false;
        atspi::server_publish(g_tree, later);
        pump([] { return find_event("Deactivate", nullptr) != nullptr; }, 3000);
        check(find_event("Deactivate", nullptr) != nullptr && find_event("StateChanged", "focused") != nullptr,
              "deactivating the window is told, and the focus leaves with it");
        g_active = true;
    }

    // 5. Accessibility off again: the server leaves the bus.
    g_enabled = false;
    announce_status();
    check(pump(disconnected, 3000) && atspi::server_waitable() == nullptr,
          "turning accessibility off closes the connection");
    settle(200);
    check(g_embeds == 1, "nothing registers while it is off");

    // 6. A screen reader starts (ScreenReaderEnabled alone is enough); the registry restarts later.
    g_screen_reader = true;
    announce_status();
    check(pump(embedded_twice, 3000), "ScreenReaderEnabled brings the application back");
    check(!atspi::server_queried(), "a new connection starts unasked");
    check(connection_release_name(g_services, "org.a11y.atspi.Registry", 3000), "the registry leaves the bus");
    check(pump(disconnected, 3000), "the server notices the registry's departure (it is no longer registered)");
    check(connection_request_name(g_services, "org.a11y.atspi.Registry", 3000), "the registry comes back");
    check(pump(embedded_thrice, 3000), "it embeds again with the new registry");

    atspi::server_stop();
    check(!connected() && atspi::server_waitable() == nullptr, "stopping disconnects");
    connection_close(g_client);
    connection_close(g_services);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_atspi_bus_entry) {
    using namespace maicord;
    if (!skip_unless("bare")) {
        test_write_line("SKIP atspi_bus (the private bus is not set up: tools/test_dbus.sh)");
        test_process_exit(77);
    }
    run();
    test_exit(g_failures, "atspi_bus");
}
