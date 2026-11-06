#include "maicord/core/memory.hpp"
#include "maicord/platform/atspi/accessible.hpp"
#include "maicord/platform/atspi/map.hpp"
#include "../../support/test_output.hpp"

// The AT-SPI presentation of the accessibility snapshot without a bus: the role, state, interface and action mapping, the
// object paths, the text offsets, and the answers to the method calls a screen reader makes on a synthetic window (the
// login form, a list, a slider), parsed back from the reply bodies.

namespace maicord {
namespace {

using namespace atspi;
using dbus::Builder;
using dbus::Message;
using dbus::MessageHeader;
using dbus::MessageType;
using dbus::Reader;

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

bool text_equal(const char* a, const char* b) noexcept {
    while (*a && *a == *b)
        ++a, ++b;
    return *a == *b;
}

// The synthetic window -------------------------------------------------------------------------------------------------

constexpr u8 kSurface = 5;

UiAccessNode g_nodes[64];
char g_text[8192];
u32 g_slots[128];
u32 g_keys[128];
UiAccessTree g_tree;

u64 key_of(u64 identity) noexcept {
    return ui_access_key(kSurface, identity);
}

enum : u64 {
    id_dialog = 1,
    id_heading,
    id_email,
    id_password,
    id_submit,
    id_register,
    id_remember,
    id_volume,
    id_qr,
    id_servers,
    id_row_a,
    id_row_b,
    id_row_c,
    id_folder,
    id_link,
    id_notes,
};

u32 add(u32 parent, u64 identity, UiAccessRole role, const char* name, UiRect rectangle, u16 state = 0, u16 actions = 0,
        const char* value = nullptr) noexcept {
    UiAccessNodeDesc desc{};
    desc.key = key_of(identity);
    desc.item = identity;
    desc.rectangle = rectangle;
    desc.role = role;
    u32 length = 0;
    while (name[length])
        ++length;
    desc.name = {name, length};
    if (value) {
        length = 0;
        while (value[length])
            ++length;
        desc.value = {value, length};
    }
    desc.state = state;
    desc.actions = actions;
    desc.surface = kSurface;
    return ui_access_add(&g_tree, parent, desc);
}

bool build_window() noexcept {
    if (!ui_access_tree_initialize(&g_tree, {g_nodes, 64}, {g_text, sizeof(g_text)}, {g_slots, 128}, {g_keys, 128}) ||
        !ui_access_tree_reset(&g_tree, utf8_literal("MaiCord"), {0, -32, 400, 600}))
        return false;
    const u32 dialog = add(0, id_dialog, UiAccessRole::dialog, "Log in", {50, 50, 350, 500}, ui_access_modal);
    add(dialog, id_heading, UiAccessRole::header, "Welcome back", {60, 60, 340, 90});
    const u32 email = add(dialog, id_email, UiAccessRole::edit, "Email or phone number", {60, 100, 340, 130},
                          ui_access_focusable | ui_access_focused,
                          ui_access_action_value | ui_access_action_text | ui_access_action_focus, "h\xC3\xA9llo w\xC3\xB6rld");
    g_tree.nodes[email].placement.selection_anchor = 1;  // after "h"
    g_tree.nodes[email].placement.selection_caret = 5;   // after "h\xC3\xA9ll"
    add(dialog, id_password, UiAccessRole::edit, "Password", {60, 140, 340, 170}, ui_access_password,
        ui_access_action_value | ui_access_action_focus, "hunter2");
    add(dialog, id_submit, UiAccessRole::button, "Log in", {60, 180, 340, 210}, ui_access_focusable,
        ui_access_action_invoke | ui_access_action_focus);
    add(dialog, id_register, UiAccessRole::button, "Register", {60, 220, 340, 250}, ui_access_disabled,
        ui_access_action_invoke);
    add(dialog, id_remember, UiAccessRole::check_box, "Remember me", {60, 260, 340, 280},
        static_cast<u16>(ui_access_checked | ui_access_toggleable), ui_access_action_toggle);
    const u32 volume = add(dialog, id_volume, UiAccessRole::slider, "Volume", {60, 290, 340, 310}, 0,
                           ui_access_action_range);
    g_tree.nodes[volume].range = {2, 0, 4, 1, 1};
    add(dialog, id_qr, UiAccessRole::image, "Log in with QR code", {60, 320, 200, 460}, 0, 0);
    const u32 list = add(dialog, id_servers, UiAccessRole::list, "Servers", {220, 320, 340, 460});
    add(list, id_row_a, UiAccessRole::list_item, "Alpha", {220, 320, 340, 340}, ui_access_selectable,
        ui_access_action_select | ui_access_action_invoke);
    add(list, id_row_b, UiAccessRole::list_item, "Beta", {220, 340, 340, 360},
        static_cast<u16>(ui_access_selectable | ui_access_selected), ui_access_action_select | ui_access_action_invoke);
    add(list, id_row_c, UiAccessRole::list_item, "Gamma", {220, 360, 340, 380},
        static_cast<u16>(ui_access_selectable | ui_access_offscreen), ui_access_action_select);
    add(list, id_folder, UiAccessRole::list_item, "Folder", {220, 380, 340, 400}, ui_access_collapsed,
        ui_access_action_expand | ui_access_action_invoke);
    add(dialog, id_link, UiAccessRole::link, "Forgot your password?", {60, 470, 340, 490}, 0, ui_access_action_invoke);
    add(0, id_notes, UiAccessRole::edit, "Notes", {0, 500, 400, 580}, static_cast<u16>(ui_access_multiline),
        ui_access_action_value | ui_access_action_text, "one\ntwo");
    return ui_access_tree_finish(&g_tree);
}

// The host: the tree above, an action log, the window's state.
struct ActLog {
    u64 key;
    UiAccessAction action;
    double number;
    char text[64];
    u32 text_bytes;
    u32 calls;
};
ActLog g_act;
bool g_active = true;

const UiAccessTree* host_tree(void*) noexcept {
    return &g_tree;
}

bool host_act(void*, u64 key, UiAccessAction action, double number, Utf8View text) noexcept {
    g_act.key = key;
    g_act.action = action;
    g_act.number = number;
    g_act.text_bytes = minimum<u32>(text.bytes, sizeof(g_act.text));
    copy_bytes(g_act.text, text.data, g_act.text_bytes);
    ++g_act.calls;
    return true;
}

float host_content_top(void*) noexcept {
    return 32.0f;
}

bool host_window_active(void*) noexcept {
    return g_active;
}

Endpoint g_endpoint;

void init_endpoint() noexcept {
    g_endpoint = Endpoint{};
    g_endpoint.host = {nullptr, host_tree, host_act, host_content_top, host_window_active, nullptr};
    const char* name = ":1.77";
    copy_bytes(g_endpoint.bus_name, name, 6);
    const char* registry = ":1.2";
    copy_bytes(g_endpoint.registry_sender, registry, 5);
}

// Asking ----------------------------------------------------------------------------------------------------------------

struct Answer {
    Outcome outcome;
    Failure failure;
    Builder body;
    Reader reader;
};

// A call to `path`; `args` may be null. The answer's body is read with `reader`; release it afterwards.
Answer ask(const char* path, const char* interface, const char* member, const Builder* args = nullptr,
           const char* sender = nullptr) noexcept {
    Answer answer{};
    Builder none;
    builder_init(&none);
    MessageHeader header{};
    header.type = MessageType::method_call;
    header.serial = 9;
    header.path = path;
    header.interface = interface;
    header.member = member;
    u8* block = nullptr;
    u32 bytes = 0;
    Message call;
    const bool built = dbus::message_serialize(header, args ? *args : none, &block, &bytes) &&
                       dbus::message_parse(&call, block, bytes);
    check(built, "the test call serialises and parses");
    builder_init(&answer.body);
    if (built) {
        call.sender = sender;
        answer.outcome = answer_call(&g_endpoint, call, &answer.body, &answer.failure);
        dbus::message_release(&call);
    }
    builder_release(&none);
    dbus::reader_init(&answer.reader, answer.body.data, answer.body.size, answer.body.signature, nullptr, 0);
    return answer;
}

void release(Answer* answer) noexcept {
    builder_release(&answer->body);
}

char g_path[kPathBytes];

const char* path_of(u64 identity) noexcept {
    path_for_key(key_of(identity), g_path);
    return g_path;
}

bool ok(const Answer& answer) noexcept {
    return answer.outcome == Outcome::replied;
}

// Reads a variant's i32/u32/bool/string/double (for Properties.Get).
bool variant_i32(Answer* answer, i32* value) noexcept {
    const char* signature = nullptr;
    return dbus::reader_enter_variant(&answer->reader, &signature) && signature && text_equal(signature, "i") &&
           dbus::reader_i32(&answer->reader, value);
}

bool variant_double(Answer* answer, double* value) noexcept {
    const char* signature = nullptr;
    return dbus::reader_enter_variant(&answer->reader, &signature) && signature && text_equal(signature, "d") &&
           dbus::reader_double(&answer->reader, value);
}

bool variant_text(Answer* answer, const char** text) noexcept {
    const char* signature = nullptr;
    return dbus::reader_enter_variant(&answer->reader, &signature) && signature && text_equal(signature, "s") &&
           dbus::reader_string(&answer->reader, text, nullptr);
}

Answer property(const char* path, const char* interface, const char* name) noexcept {
    Builder args;
    builder_init(&args);
    builder_string(&args, interface);
    builder_string(&args, name);
    Answer answer = ask(path, "org.freedesktop.DBus.Properties", "Get", &args);
    builder_release(&args);
    return answer;
}

i32 integer_property(const char* path, const char* interface, const char* name) noexcept {
    Answer answer = property(path, interface, name);
    i32 value = -12345;
    if (ok(answer))
        variant_i32(&answer, &value);
    release(&answer);
    return value;
}

bool text_property(const char* path, const char* interface, const char* name, const char* expected) noexcept {
    Answer answer = property(path, interface, name);
    const char* text = nullptr;
    const bool matched = ok(answer) && variant_text(&answer, &text) && text_equal(text, expected);
    release(&answer);
    return matched;
}

// Calls with integer arguments.
Answer ask_ints(const char* path, const char* interface, const char* member, i32 a, i32 b = 0, i32 c = 0, i32 d = 0,
                u32 count = 1) noexcept {
    Builder args;
    builder_init(&args);
    const i32 values[4]{a, b, c, d};
    for (u32 i = 0; i < count; ++i)
        builder_i32(&args, values[i]);
    Answer answer = ask(path, interface, member, &args);
    builder_release(&args);
    return answer;
}

Answer ask_u32(const char* path, const char* interface, const char* member, u32 value) noexcept {
    Builder args;
    builder_init(&args);
    builder_u32(&args, value);
    Answer answer = ask(path, interface, member, &args);
    builder_release(&args);
    return answer;
}

Answer ask_with_coordinates(const char* path, const char* interface, const char* member, i32 x, i32 y, u32 coordinates) noexcept {
    Builder args;
    builder_init(&args);
    builder_i32(&args, x);
    builder_i32(&args, y);
    builder_u32(&args, coordinates);
    Answer answer = ask(path, interface, member, &args);
    builder_release(&args);
    return answer;
}

bool boolean_reply(Answer* answer, bool expected) noexcept {
    bool value = !expected;
    return ok(*answer) && dbus::reader_bool(&answer->reader, &value) && value == expected;
}

// (so) of a reply: true when it is `path` on the test's bus name.
bool reference_is(Answer* answer, const char* path) noexcept {
    const char* name = nullptr;
    const char* object = nullptr;
    return dbus::reader_enter_struct(&answer->reader) && dbus::reader_string(&answer->reader, &name, nullptr) &&
           dbus::reader_string(&answer->reader, &object, nullptr) && text_equal(object, path) &&
           (text_equal(name, ":1.77") || text_equal(path, kNullPath));
}

constexpr const char kAccessible[] = "org.a11y.atspi.Accessible";
constexpr const char kComponent[] = "org.a11y.atspi.Component";
constexpr const char kAction[] = "org.a11y.atspi.Action";
constexpr const char kText[] = "org.a11y.atspi.Text";
constexpr const char kEditable[] = "org.a11y.atspi.EditableText";
constexpr const char kValue[] = "org.a11y.atspi.Value";
constexpr const char kSelection[] = "org.a11y.atspi.Selection";

// Tests ---------------------------------------------------------------------------------------------------------------

UiAccessNode make(UiAccessRole role, u16 state = 0, u16 actions = 0) noexcept {
    UiAccessNode node{};
    node.role = role;
    node.state = state;
    node.actions = actions;
    return node;
}

bool has_state(const UiAccessNode& node, State state, bool active = true) noexcept {
    u32 words[2];
    states_for(node, active, words);
    return (words[static_cast<u32>(state) >> 5] >> (static_cast<u32>(state) & 31u)) & 1u;
}

void test_roles() noexcept {
    check(role_for(make(UiAccessRole::window)) == role_frame, "the window is a frame");
    check(role_for(make(UiAccessRole::button)) == role_push_button, "a button is a push button");
    check(role_for(make(UiAccessRole::toggle_button)) == role_toggle_button, "a toggle button");
    check(role_for(make(UiAccessRole::check_box)) == role_check_box, "a check box");
    check(role_for(make(UiAccessRole::radio_button)) == role_radio_button, "a radio button");
    check(role_for(make(UiAccessRole::combo_box)) == role_combo_box, "a combo box");
    check(role_for(make(UiAccessRole::list)) == role_list && role_for(make(UiAccessRole::list_item)) == role_list_item,
          "lists and their items");
    check(role_for(make(UiAccessRole::tab_list)) == role_page_tab_list && role_for(make(UiAccessRole::tab)) == role_page_tab,
          "tab lists and tabs");
    check(role_for(make(UiAccessRole::menu)) == role_menu && role_for(make(UiAccessRole::menu_item)) == role_menu_item,
          "menus and their items");
    check(role_for(make(UiAccessRole::text)) == role_label && role_for(make(UiAccessRole::header)) == role_heading,
          "static text is a label and a header a heading");
    check(role_for(make(UiAccessRole::slider)) == role_slider && role_for(make(UiAccessRole::image)) == role_image &&
              role_for(make(UiAccessRole::link)) == role_link && role_for(make(UiAccessRole::status)) == role_status_bar,
          "slider, image, link and status");
    check(role_for(make(UiAccessRole::dialog)) == role_dialog && role_for(make(UiAccessRole::pane)) == role_panel &&
              role_for(make(UiAccessRole::group)) == role_grouping && role_for(make(UiAccessRole::tree)) == role_tree,
          "dialog, pane, group and tree");
    check(role_for(make(UiAccessRole::edit)) == role_entry, "a single-line edit is an entry");
    check(role_for(make(UiAccessRole::edit, ui_access_multiline)) == role_text, "a multi-line edit is text");
    check(role_for(make(UiAccessRole::edit, ui_access_password)) == role_password_text, "a password edit is password text");
    check(text_equal(role_name(role_push_button), "push button") && text_equal(role_name(role_page_tab_list), "page tab list") &&
              text_equal(role_name(role_entry), "entry") && text_equal(role_name(role_application), "application") &&
              text_equal(role_name(9999), "unknown"),
          "role names");
}

void test_states() noexcept {
    const UiAccessNode plain = make(UiAccessRole::button, ui_access_focusable, ui_access_action_invoke);
    check(has_state(plain, state_enabled) && has_state(plain, state_sensitive) && has_state(plain, state_showing) &&
              has_state(plain, state_visible) && has_state(plain, state_focusable) && !has_state(plain, state_focused),
          "an ordinary button is enabled, showing and focusable");
    check(!has_state(make(UiAccessRole::button, ui_access_disabled), state_enabled), "a disabled node is not enabled");
    check(!has_state(make(UiAccessRole::button, ui_access_offscreen), state_showing), "an offscreen node is not showing");
    const UiAccessNode focused = make(UiAccessRole::edit, ui_access_focused);
    check(has_state(focused, state_focused, true) && !has_state(focused, state_focused, false) &&
              has_state(focused, state_focusable),
          "focus is reported only while the window is active");
    check(has_state(make(UiAccessRole::window), state_active, true) && !has_state(make(UiAccessRole::window), state_active, false),
          "the window is active while it is the active window");
    const UiAccessNode checked = make(UiAccessRole::check_box, ui_access_checked, ui_access_action_toggle);
    check(has_state(checked, state_checked) && has_state(checked, state_checkable) && !has_state(checked, state_pressed),
          "a checked box is checked and checkable");
    check(has_state(make(UiAccessRole::toggle_button, ui_access_checked, ui_access_action_toggle), state_pressed),
          "a checked toggle button is pressed");
    check(has_state(make(UiAccessRole::check_box, ui_access_mixed), state_indeterminate), "mixed is indeterminate");
    check(has_state(make(UiAccessRole::list_item, ui_access_selected | ui_access_selectable), state_selected) &&
              has_state(make(UiAccessRole::list_item, ui_access_selectable), state_selectable),
          "selection");
    check(has_state(make(UiAccessRole::list_item, ui_access_collapsed, ui_access_action_expand), state_collapsed) &&
              has_state(make(UiAccessRole::list_item, ui_access_collapsed, ui_access_action_expand), state_expandable) &&
              has_state(make(UiAccessRole::list_item, ui_access_expanded, ui_access_action_expand), state_expanded),
          "expansion");
    const UiAccessNode edit = make(UiAccessRole::edit, 0, ui_access_action_value | ui_access_action_text);
    check(has_state(edit, state_editable) && has_state(edit, state_single_line) && !has_state(edit, state_read_only),
          "an edit is editable and single line");
    check(!has_state(make(UiAccessRole::edit, ui_access_read_only, ui_access_action_value), state_editable) &&
              has_state(make(UiAccessRole::edit, ui_access_read_only, ui_access_action_value), state_read_only),
          "a read-only edit is not editable");
    check(has_state(make(UiAccessRole::edit, ui_access_multiline, ui_access_action_value), state_multi_line),
          "a multi-line edit");
    check(has_state(make(UiAccessRole::dialog, ui_access_modal), state_modal), "a modal dialog");
    check(has_state(make(UiAccessRole::dialog), state_active, true) && !has_state(make(UiAccessRole::dialog), state_active, false) &&
              !has_state(make(UiAccessRole::dialog, ui_access_offscreen), state_active, true),
          "a showing dialog is active with its window");
    check(has_state(make(UiAccessRole::combo_box), state_has_popup), "a combo box has a popup");
}

void test_interfaces_and_actions() noexcept {
    const auto interfaces = [](UiAccessRole role, u16 state, u16 actions) noexcept {
        return interfaces_for(make(role, state, actions));
    };
    check(interfaces(UiAccessRole::pane, 0, 0) == (interface_accessible | interface_component),
          "every node has Accessible and Component");
    check(interfaces(UiAccessRole::button, 0, ui_access_action_invoke) & interface_action, "a button has Action");
    check(!(interfaces(UiAccessRole::button, 0, ui_access_action_focus) & interface_action),
          "focusing alone is not an AT-SPI action (Component.GrabFocus covers it)");
    const u32 edit = interfaces(UiAccessRole::edit, 0, ui_access_action_value | ui_access_action_text);
    check((edit & interface_text) && (edit & interface_editable_text), "an edit has Text and EditableText");
    check((interfaces(UiAccessRole::edit, ui_access_read_only, ui_access_action_value | ui_access_action_text) &
           interface_editable_text) == 0,
          "a read-only edit is not editable text");
    check((interfaces(UiAccessRole::text, 0, ui_access_action_text) & interface_text) &&
              !(interfaces(UiAccessRole::text, 0, ui_access_action_text) & interface_editable_text),
          "a text pattern node has Text only");
    check(interfaces(UiAccessRole::slider, 0, ui_access_action_range) & interface_value, "a range has Value");
    check(interfaces(UiAccessRole::list, 0, 0) & interface_selection, "a list has Selection");
    check(interfaces(UiAccessRole::group, 0, ui_access_action_select) & interface_selection &&
              !(interfaces(UiAccessRole::group, 0, 0) & interface_selection),
          "a selecting group has Selection");
    check(text_equal(interface_name(interface_text), "org.a11y.atspi.Text") && !interface_name(1u << 20),
          "interface names");

    ActionEntry entries[kActionMaximum];
    check(actions_for(make(UiAccessRole::list_item, 0, ui_access_action_select | ui_access_action_invoke), entries) == 2 &&
              text_equal(entries[0].name, "click") && entries[0].action == UiAccessAction::invoke &&
              text_equal(entries[1].name, "select") && entries[1].action == UiAccessAction::select,
          "invoke comes first, then select");
    check(actions_for(make(UiAccessRole::check_box, 0, ui_access_action_toggle), entries) == 1 &&
              text_equal(entries[0].name, "toggle") && entries[0].action == UiAccessAction::toggle,
          "a check box toggles");
    check(actions_for(make(UiAccessRole::list_item, ui_access_collapsed, ui_access_action_expand), entries) == 1 &&
              text_equal(entries[0].name, "expand") && entries[0].action == UiAccessAction::expand,
          "a collapsed item expands");
    check(actions_for(make(UiAccessRole::list_item, ui_access_expanded, 0), entries) == 1 &&
              text_equal(entries[0].name, "collapse") && entries[0].action == UiAccessAction::collapse,
          "an expanded item collapses, even when only its state says it can");
    check(actions_for(make(UiAccessRole::pane), entries) == 0, "a pane has no actions");
}

void test_paths_and_text() noexcept {
    char path[kPathBytes];
    path_for_key(0x0123456789abcdefull, path);
    u64 key = 0;
    check(text_equal(path, "/org/a11y/atspi/accessible/0123456789abcdef") && key_from_path(path, &key) &&
              key == 0x0123456789abcdefull,
          "a node path round trips its key");
    path_for_key(0, path);
    check(key_from_path(path, &key) && key == 0, "key 0 is a node, not the root");
    check(!key_from_path(kRootPath, &key) && is_root_path(kRootPath) && !is_root_path(path), "the root path");
    check(!key_from_path("/org/a11y/atspi/accessible/0123", &key) &&
              !key_from_path("/org/a11y/atspi/accessible/0123456789abcdeg", &key) &&
              !key_from_path("/org/a11y/atspi/accessible/0123456789abcdef0", &key) && !key_from_path("/x", &key) &&
              !key_from_path(nullptr, &key),
          "malformed paths are refused");

    const Utf8View text = utf8_literal("h\xC3\xA9llo w\xF0\x9F\x98\x80rld");
    check(text_character_count(text) == 11 && text_byte_offset(text, 2) == 3 && text_byte_offset(text, 7) == 8 &&
              text_byte_offset(text, 99) == text.bytes && text_character_offset(text, 3) == 2 &&
              text_character_offset(text, 4) == 3 && text_character_offset(text, text.bytes) == 11,
          "character and byte offsets");
    check(text_scalar_at(text, 1) == 0xE9 && text_scalar_at(text, 8) == 0x1F600 && text_scalar_at(text, text.bytes) == 0 &&
              text_scalar_at(text, 0) == 'h',
          "scalar values");

    const Utf8View lines = utf8_literal("hello world\nsecond line");
    u32 start = 0, end = 0;
    text_span(lines, 7, TextGranularity::word, 0, &start, &end);
    check(start == 6 && end == 12, "the word around an offset includes the space after it");
    text_span(lines, 7, TextGranularity::word, -1, &start, &end);
    check(start == 0 && end == 6, "the word before");
    text_span(lines, 7, TextGranularity::word, 1, &start, &end);
    check(start == 12 && end == 19, "the word after");
    text_span(lines, 3, TextGranularity::line, 0, &start, &end);
    check(start == 0 && end == 12, "a line ends after its line feed");
    text_span(lines, 3, TextGranularity::paragraph, 1, &start, &end);
    check(start == 12 && end == 23, "the next paragraph");
    text_span(lines, 3, TextGranularity::character, 0, &start, &end);
    check(start == 3 && end == 4, "one character");
    text_span(lines, 23, TextGranularity::character, 0, &start, &end);
    check(start == 23 && end == 23, "nothing at the end");
    text_span(lines, 23, TextGranularity::character, -1, &start, &end);
    check(start == 22 && end == 23, "the character before the end");
    text_span(lines, 0, TextGranularity::word, -1, &start, &end);
    check(start == 0 && end == 0, "nothing before the start");
    text_span({}, 0, TextGranularity::word, 0, &start, &end);
    check(start == 0 && end == 0, "an empty text has empty spans");
}

void test_application() noexcept {
    Answer name = property(kRootPath, kAccessible, "Name");
    const char* text = nullptr;
    check(ok(name) && variant_text(&name, &text) && text_equal(text, "MaiCord"), "the application is named MaiCord");
    release(&name);
    check(integer_property(kRootPath, kAccessible, "ChildCount") == 1, "the application has the window as its child");
    Answer role = ask(kRootPath, kAccessible, "GetRole");
    u32 value = 0;
    check(ok(role) && dbus::reader_u32(&role.reader, &value) && value == role_application, "the application's role");
    release(&role);
    Answer children = ask(kRootPath, kAccessible, "GetChildren");
    check(ok(children) && dbus::reader_enter_array(&children.reader) && reference_is(&children, "/org/a11y/atspi/accessible/" "0000000000000000") == false,
          "the application's child is the window node");
    release(&children);
    char window[kPathBytes];
    path_for_key(g_tree.nodes[0].key, window);
    Answer child = ask_ints(kRootPath, kAccessible, "GetChildAtIndex", 0);
    check(ok(child) && reference_is(&child, window), "GetChildAtIndex(0) is the window");
    release(&child);
    child = ask_ints(kRootPath, kAccessible, "GetChildAtIndex", 1);
    check(ok(child) && reference_is(&child, kNullPath), "no second child");
    release(&child);
    Answer parent = property(window, kAccessible, "Parent");
    const char* signature = nullptr;
    const char* object = nullptr;
    check(ok(parent) && dbus::reader_enter_variant(&parent.reader, &signature) && text_equal(signature, "(so)") &&
              dbus::reader_enter_struct(&parent.reader) && dbus::reader_string(&parent.reader, &object, nullptr) &&
              dbus::reader_string(&parent.reader, &object, nullptr) && text_equal(object, kRootPath),
          "the window's parent is the application");
    release(&parent);
    check(role_for(g_tree.nodes[0]) == role_frame, "the window node's role");

    // Application.Id is set by the registry.
    Builder args;
    builder_init(&args);
    builder_string(&args, "org.a11y.atspi.Application");
    builder_string(&args, "Id");
    builder_open_variant(&args, "i");
    builder_i32(&args, 42);
    builder_close_variant(&args);
    Answer set = ask(kRootPath, "org.freedesktop.DBus.Properties", "Set", &args);
    builder_release(&args);
    check(ok(set) && g_endpoint.id == 42 && integer_property(kRootPath, "org.a11y.atspi.Application", "Id") == 42,
          "the registry sets the application's id");
    release(&set);
    check(text_property(kRootPath, "org.a11y.atspi.Application", "ToolkitName", "MaiCord"), "the toolkit's name");
    Answer interfaces = ask(kRootPath, kAccessible, "GetInterfaces");
    u32 found = 0;
    const char* entry = nullptr;
    if (ok(interfaces) && dbus::reader_enter_array(&interfaces.reader))
        while (dbus::reader_more(interfaces.reader) && dbus::reader_string(&interfaces.reader, &entry, nullptr))
            found += text_equal(entry, "org.a11y.atspi.Application") || text_equal(entry, kAccessible) ? 1 : 0;
    check(found == 2, "the application answers Accessible and Application");
    release(&interfaces);
}

void test_nodes() noexcept {
    const char* dialog = path_of(id_dialog);
    char dialog_path[kPathBytes];
    for (u32 i = 0; i < kPathBytes; ++i)
        dialog_path[i] = dialog[i];
    check(text_property(dialog_path, kAccessible, "Name", "Log in"), "a node's name");
    Answer role = ask(dialog_path, kAccessible, "GetRole");
    u32 value = 0;
    check(ok(role) && dbus::reader_u32(&role.reader, &value) && value == role_dialog, "a dialog's role");
    release(&role);
    check(integer_property(dialog_path, kAccessible, "ChildCount") == 10, "the dialog's children");

    const char* email = path_of(id_email);
    char email_path[kPathBytes];
    for (u32 i = 0; i < kPathBytes; ++i)
        email_path[i] = email[i];
    Answer state = ask(email_path, kAccessible, "GetState");
    u32 low = 0, high = 0;
    check(ok(state) && dbus::reader_enter_array(&state.reader) && dbus::reader_u32(&state.reader, &low) &&
              dbus::reader_u32(&state.reader, &high) && ((low >> state_focused) & 1) && ((low >> state_editable) & 1) &&
              ((low >> state_enabled) & 1) && ((low >> state_single_line) & 1),
          "the focused email field's states");
    release(&state);
    g_active = false;
    state = ask(email_path, kAccessible, "GetState");
    check(ok(state) && dbus::reader_enter_array(&state.reader) && dbus::reader_u32(&state.reader, &low) &&
              !((low >> state_focused) & 1),
          "an inactive window reports nothing focused");
    release(&state);
    g_active = true;
    Answer role_name_reply = ask(email_path, kAccessible, "GetRoleName");
    const char* text = nullptr;
    check(ok(role_name_reply) && dbus::reader_string(&role_name_reply.reader, &text, nullptr) && text_equal(text, "entry"),
          "the role name");
    release(&role_name_reply);

    // Text.
    check(integer_property(email_path, kText, "CharacterCount") == 11, "the email's length in characters");
    check(integer_property(email_path, kText, "CaretOffset") == 4, "the caret is after four characters");
    Answer got = ask_ints(email_path, kText, "GetText", 1, 6, 0, 0, 2);
    check(ok(got) && dbus::reader_string(&got.reader, &text, nullptr) && text_equal(text, "\xC3\xA9llo "), "GetText by characters");
    release(&got);
    got = ask_ints(email_path, kText, "GetText", 6, -1, 0, 0, 2);
    check(ok(got) && dbus::reader_string(&got.reader, &text, nullptr) && text_equal(text, "w\xC3\xB6rld"), "GetText to the end");
    release(&got);
    got = ask_ints(email_path, kText, "GetNSelections", 0, 0, 0, 0, 0);
    i32 number = 0;
    check(ok(got) && dbus::reader_i32(&got.reader, &number) && number == 1, "one selection");
    release(&got);
    got = ask_ints(email_path, kText, "GetSelection", 0);
    i32 first = 0, last = 0;
    check(ok(got) && dbus::reader_i32(&got.reader, &first) && dbus::reader_i32(&got.reader, &last) && first == 1 && last == 4,
          "the selection in characters");
    release(&got);
    got = ask_ints(email_path, kText, "GetCharacterAtOffset", 1);
    check(ok(got) && dbus::reader_i32(&got.reader, &number) && number == 0xE9, "the character at an offset");
    release(&got);
    {
        Builder args;
        builder_init(&args);
        builder_i32(&args, 7);
        builder_u32(&args, 1);  // word granularity
        got = ask(email_path, kText, "GetStringAtOffset", &args);
        builder_release(&args);
        const char* word = nullptr;
        check(ok(got) && dbus::reader_string(&got.reader, &word, nullptr) &&
                  dbus::reader_i32(&got.reader, &first) && dbus::reader_i32(&got.reader, &last) &&
                  text_equal(word, "w\xC3\xB6rld") && first == 6 && last == 11,
              "GetStringAtOffset by word");
        release(&got);
    }
    g_act = {};
    got = ask_ints(email_path, kText, "SetSelection", 0, 2, 4, 0, 3);
    check(boolean_reply(&got, true) && g_act.calls == 1 && g_act.action == UiAccessAction::select_text &&
              g_act.key == key_of(id_email) && g_act.number == ui_access_text_selection_pack(3, 5),
          "SetSelection asks for the byte range of the characters");
    release(&got);
    g_act = {};
    got = ask_ints(email_path, kText, "SetCaretOffset", 5);
    check(boolean_reply(&got, true) && g_act.action == UiAccessAction::select_text &&
              g_act.number == ui_access_text_selection_pack(6, 6),
          "SetCaretOffset collapses the selection");
    release(&got);

    // EditableText.
    g_act = {};
    {
        Builder args;
        builder_init(&args);
        builder_string(&args, "new text");
        got = ask(email_path, kEditable, "SetTextContents", &args);
        builder_release(&args);
        check(boolean_reply(&got, true) && g_act.action == UiAccessAction::set_value && g_act.text_bytes == 8 &&
                  bytes_equal(g_act.text, "new text", 8),
              "SetTextContents sets the value");
        release(&got);
    }
    g_act = {};
    {
        Builder args;
        builder_init(&args);
        builder_i32(&args, 5);
        builder_string(&args, "XY");
        builder_i32(&args, 2);
        got = ask(email_path, kEditable, "InsertText", &args);
        builder_release(&args);
        const char expected[] = "h\xC3\xA9llo" "XY w\xC3\xB6rld";
        check(boolean_reply(&got, true) && g_act.action == UiAccessAction::set_value &&
                  g_act.text_bytes == sizeof(expected) - 1 && bytes_equal(g_act.text, expected, sizeof(expected) - 1),
              "InsertText inserts at a character offset");
        release(&got);
    }
    g_act = {};
    got = ask_ints(email_path, kEditable, "DeleteText", 0, 2, 0, 0, 2);
    {
        const char expected[] = "llo w\xC3\xB6rld";
        check(boolean_reply(&got, true) && g_act.text_bytes == sizeof(expected) - 1 &&
                  bytes_equal(g_act.text, expected, sizeof(expected) - 1),
              "DeleteText removes a range of characters");
    }
    release(&got);

    // A password: no text, but it can be set.
    const char* password = path_of(id_password);
    char password_path[kPathBytes];
    for (u32 i = 0; i < kPathBytes; ++i)
        password_path[i] = password[i];
    role = ask(password_path, kAccessible, "GetRole");
    check(ok(role) && dbus::reader_u32(&role.reader, &value) && value == role_password_text, "a password field's role");
    release(&role);
    check(integer_property(password_path, kText, "CharacterCount") == 0, "a password's text is never exposed");
    got = ask_ints(password_path, kText, "GetText", 0, -1, 0, 0, 2);
    check(ok(got) && dbus::reader_string(&got.reader, &text, nullptr) && text_equal(text, ""), "GetText of a password is empty");
    release(&got);
    g_act = {};
    {
        Builder args;
        builder_init(&args);
        builder_string(&args, "secret");
        got = ask(password_path, kEditable, "SetTextContents", &args);
        builder_release(&args);
        check(boolean_reply(&got, true) && g_act.action == UiAccessAction::set_value && g_act.key == key_of(id_password),
              "a password can be set");
        release(&got);
    }

    // The multi-line notes.
    const char* notes = path_of(id_notes);
    char notes_path[kPathBytes];
    for (u32 i = 0; i < kPathBytes; ++i)
        notes_path[i] = notes[i];
    role = ask(notes_path, kAccessible, "GetRole");
    check(ok(role) && dbus::reader_u32(&role.reader, &value) && value == role_text, "a multi-line edit is text");
    release(&role);
}

void test_actions() noexcept {
    char submit[kPathBytes];
    path_for_key(key_of(id_submit), submit);
    check(integer_property(submit, kAction, "NActions") == 1, "a button has one action");
    Answer got = ask_ints(submit, kAction, "GetName", 0);
    const char* text = nullptr;
    check(ok(got) && dbus::reader_string(&got.reader, &text, nullptr) && text_equal(text, "click"), "the action's name");
    release(&got);
    got = ask(submit, kAction, "GetActions");
    const char* name = nullptr;
    check(ok(got) && dbus::reader_enter_array(&got.reader) && dbus::reader_enter_struct(&got.reader) &&
              dbus::reader_string(&got.reader, &name, nullptr) && text_equal(name, "click"),
          "GetActions lists it");
    release(&got);
    g_act = {};
    got = ask_ints(submit, kAction, "DoAction", 0);
    check(boolean_reply(&got, true) && g_act.calls == 1 && g_act.key == key_of(id_submit) && g_act.action == UiAccessAction::invoke,
          "DoAction invokes the button");
    release(&got);
    g_act = {};
    got = ask_ints(submit, kAction, "DoAction", 1);
    check(boolean_reply(&got, false) && g_act.calls == 0, "there is no second action");
    release(&got);
    g_act = {};
    got = ask_ints(submit, kComponent, "GrabFocus", 0, 0, 0, 0, 0);
    check(boolean_reply(&got, true) && g_act.action == UiAccessAction::focus, "GrabFocus asks for focus");
    release(&got);

    char remember[kPathBytes];
    path_for_key(key_of(id_remember), remember);
    got = ask_ints(remember, kAction, "GetName", 0);
    check(ok(got) && dbus::reader_string(&got.reader, &text, nullptr) && text_equal(text, "toggle"), "a check box toggles");
    release(&got);
    Answer state = ask(remember, kAccessible, "GetState");
    u32 low = 0;
    u32 high = 0;
    check(ok(state) && dbus::reader_enter_array(&state.reader) && dbus::reader_u32(&state.reader, &low) &&
              dbus::reader_u32(&state.reader, &high) && ((low >> state_checked) & 1) &&
              ((high >> (state_checkable - 32)) & 1),
          "the check box is checked");
    release(&state);

    char folder[kPathBytes];
    path_for_key(key_of(id_folder), folder);
    got = ask_ints(folder, kAction, "GetName", 1);
    check(ok(got) && dbus::reader_string(&got.reader, &text, nullptr) && text_equal(text, "select") == false,
          "the folder's second action is not select");
    release(&got);
    g_act = {};
    got = ask_ints(folder, kAction, "DoAction", 1);
    check(boolean_reply(&got, true) && g_act.action == UiAccessAction::expand, "a collapsed folder expands");
    release(&got);

    // Not an interface of this node.
    got = ask(path_of(id_heading), kAction, "GetActions");
    check(got.outcome == Outcome::unhandled, "a heading has no Action interface");
    release(&got);
}

void test_value_and_selection() noexcept {
    char volume[kPathBytes];
    path_for_key(key_of(id_volume), volume);
    Answer got = property(volume, kValue, "MaximumValue");
    double number = 0;
    check(ok(got) && variant_double(&got, &number) && number == 4.0, "a slider's maximum");
    release(&got);
    got = property(volume, kValue, "CurrentValue");
    check(ok(got) && variant_double(&got, &number) && number == 2.0, "a slider's value");
    release(&got);
    got = property(volume, kValue, "MinimumIncrement");
    check(ok(got) && variant_double(&got, &number) && number == 1.0, "a slider's step");
    release(&got);
    Builder args;
    builder_init(&args);
    builder_string(&args, kValue);
    builder_string(&args, "CurrentValue");
    builder_open_variant(&args, "d");
    builder_double(&args, 3.0);
    builder_close_variant(&args);
    g_act = {};
    got = ask(volume, "org.freedesktop.DBus.Properties", "Set", &args);
    builder_release(&args);
    check(ok(got) && g_act.action == UiAccessAction::set_range && g_act.number == 3.0 && g_act.key == key_of(id_volume),
          "setting the value asks for a range change");
    release(&got);

    char list[kPathBytes];
    path_for_key(key_of(id_servers), list);
    check(integer_property(list, kSelection, "NSelectedChildren") == 1, "one server is selected");
    char beta[kPathBytes];
    path_for_key(key_of(id_row_b), beta);
    got = ask_ints(list, kSelection, "GetSelectedChild", 0);
    check(ok(got) && reference_is(&got, beta), "the selected child");
    release(&got);
    got = ask_ints(list, kSelection, "IsChildSelected", 1);
    check(boolean_reply(&got, true), "the second child is selected");
    release(&got);
    got = ask_ints(list, kSelection, "IsChildSelected", 0);
    check(boolean_reply(&got, false), "the first is not");
    release(&got);
    g_act = {};
    got = ask_ints(list, kSelection, "SelectChild", 2);
    check(boolean_reply(&got, true) && g_act.action == UiAccessAction::select && g_act.key == key_of(id_row_c),
          "SelectChild selects through the child's own action");
    release(&got);
    got = ask_ints(list, kAccessible, "GetChildAtIndex", 3);
    char folder[kPathBytes];
    path_for_key(key_of(id_folder), folder);
    check(ok(got) && reference_is(&got, folder), "children in order");
    release(&got);
    char gamma[kPathBytes];
    path_for_key(key_of(id_row_c), gamma);
    got = ask(gamma, kAccessible, "GetIndexInParent");
    i32 index = -1;
    check(ok(got) && dbus::reader_i32(&got.reader, &index) && index == 2, "the index in the parent");
    release(&got);
    got = ask(gamma, kAccessible, "GetState");
    u32 low = 0;
    check(ok(got) && dbus::reader_enter_array(&got.reader) && dbus::reader_u32(&got.reader, &low) &&
              !((low >> state_showing) & 1) && ((low >> state_selectable) & 1),
          "an offscreen item is not showing");
    release(&got);
}

void test_geometry() noexcept {
    char dialog[kPathBytes];
    path_for_key(key_of(id_dialog), dialog);
    Answer got = ask_u32(dialog, kComponent, "GetExtents", 0);
    i32 x = 0, y = 0, width = 0, height = 0;
    Reader* reader = &got.reader;
    check(ok(got) && dbus::reader_enter_struct(reader) && dbus::reader_i32(reader, &x) && dbus::reader_i32(reader, &y) &&
              dbus::reader_i32(reader, &width) && dbus::reader_i32(reader, &height) && x == 50 && y == 82 && width == 300 &&
              height == 450,
          "window extents add the caption above the snapshot's origin");
    release(&got);
    char email[kPathBytes];
    path_for_key(key_of(id_email), email);
    got = ask_u32(email, kComponent, "GetExtents", 2);
    reader = &got.reader;
    check(ok(got) && dbus::reader_enter_struct(reader) && dbus::reader_i32(reader, &x) && dbus::reader_i32(reader, &y) &&
              x == 10 && y == 50,
          "parent-relative extents");
    release(&got);
    got = ask_ints(email, kComponent, "GetSize", 0, 0, 0, 0, 0);
    check(ok(got) && dbus::reader_i32(&got.reader, &width) && dbus::reader_i32(&got.reader, &height) && width == 280 && height == 30,
          "size");
    release(&got);
    got = ask_with_coordinates(email, kComponent, "Contains", 100, 140, 1);
    check(boolean_reply(&got, true), "a point inside");
    release(&got);
    got = ask_with_coordinates(email, kComponent, "Contains", 100, 100, 1);
    check(boolean_reply(&got, false), "a point outside");
    release(&got);
    got = ask_with_coordinates(dialog, kComponent, "GetAccessibleAtPoint", 100, 140, 1);
    check(ok(got) && reference_is(&got, email), "the deepest node at a point");
    release(&got);
    got = ask_with_coordinates(email, kComponent, "GetAccessibleAtPoint", 70, 100, 1);
    check(ok(got) && reference_is(&got, kNullPath), "a point on a sibling is not this node's");
    release(&got);
    got = ask(dialog, kComponent, "GetLayer");
    u32 layer = 0;
    check(ok(got) && dbus::reader_u32(&got.reader, &layer) && layer == 3, "widgets lie in the widget layer");
    release(&got);
}

void test_errors_and_queries() noexcept {
    char gone[kPathBytes];
    path_for_key(0xDEADBEEFull, gone);
    Answer got = ask(gone, kAccessible, "GetRole");
    check(got.outcome == Outcome::failed && text_equal(got.failure.name, "org.freedesktop.DBus.Error.UnknownObject"),
          "a vanished node is an unknown object");
    release(&got);
    got = ask("/org/a11y/atspi/null", kAccessible, "GetRole");
    check(got.outcome == Outcome::unhandled, "other paths are not ours");
    release(&got);
    got = property(path_of(id_dialog), kAccessible, "NoSuchProperty");
    check(got.outcome == Outcome::failed && text_equal(got.failure.name, "org.freedesktop.DBus.Error.InvalidArgs"),
          "an unknown property");
    release(&got);
    got = ask(path_of(id_dialog), kAccessible, "NoSuchMethod");
    check(got.outcome == Outcome::unhandled, "an unknown method");
    release(&got);
    got = ask("/org/a11y/atspi/cache", "org.a11y.atspi.Cache", "GetItems");
    check(ok(got) && dbus::reader_enter_array(&got.reader) && !dbus::reader_more(got.reader),
          "the legacy cache query is answered with an empty cache");
    release(&got);
    {
        // Text extents are four out arguments; without text geometry from the host they are the node's own.
        Builder args;
        builder_init(&args);
        builder_i32(&args, 0);
        builder_i32(&args, 3);
        builder_u32(&args, 1);
        got = ask(path_of(id_email), kText, "GetRangeExtents", &args);
        builder_release(&args);
        i32 x = 0, y = 0, width = 0, height = 0;
        check(ok(got) && dbus::reader_i32(&got.reader, &x) && dbus::reader_i32(&got.reader, &y) &&
                  dbus::reader_i32(&got.reader, &width) && dbus::reader_i32(&got.reader, &height) && x == 60 && y == 132 &&
                  width == 280 && height == 30,
              "range extents of a text without geometry are the node's");
        release(&got);
    }
    got = ask(path_of(id_dialog), kAccessible, "GetChildAtIndex");
    check(got.outcome == Outcome::failed, "a call without its arguments is refused");
    release(&got);
    got = ask(path_of(id_dialog), kText, "GetText");
    check(got.outcome == Outcome::unhandled, "a dialog has no Text");
    release(&got);

    g_endpoint.queried = false;
    got = ask(kRootPath, kAccessible, "GetRole", nullptr, ":1.2");
    check(!g_endpoint.queried, "the registry's own calls are not a client asking");
    release(&got);
    got = ask(kRootPath, kAccessible, "GetRole", nullptr, ":1.9");
    check(g_endpoint.queried, "another caller is a client");
    release(&got);

    // Attributes.
    got = ask(path_of(id_email), kAccessible, "GetAttributes");
    const char* name = nullptr;
    const char* value = nullptr;
    bool toolkit = false;
    if (ok(got) && dbus::reader_enter_array(&got.reader))
        while (dbus::reader_more(got.reader) && dbus::reader_enter_struct(&got.reader) &&
               dbus::reader_string(&got.reader, &name, nullptr) && dbus::reader_string(&got.reader, &value, nullptr) &&
               dbus::reader_leave(&got.reader))
            toolkit = toolkit || (text_equal(name, "toolkit") && text_equal(value, "MaiCord"));
    check(toolkit, "the toolkit attribute");
    release(&got);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_atspi_map_entry) {
    using namespace maicord;
    check(build_window(), "the synthetic window builds");
    init_endpoint();
    test_roles();
    test_states();
    test_interfaces_and_actions();
    test_paths_and_text();
    test_application();
    test_nodes();
    test_actions();
    test_value_and_selection();
    test_geometry();
    test_errors_and_queries();
    test_exit(g_failures, "atspi_map");
}
