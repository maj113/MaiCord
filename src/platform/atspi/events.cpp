#include "server_internal.hpp"

#include "maicord/core/memory.hpp"

namespace maicord::atspi {
namespace {

using namespace dbus;

constexpr const char kObject[] = "org.a11y.atspi.Event.Object";
constexpr const char kWindow[] = "org.a11y.atspi.Event.Window";
constexpr const char kFocusEvent[] = "org.a11y.atspi.Event.Focus";

// The any_data variant of an event.
struct Payload {
    enum Kind : u8 { kind_zero, kind_text, kind_reference, kind_no_reference, kind_real };
    Kind kind;
    Utf8View text;
    u64 key;
    double number;
};

constexpr Payload kZero{Payload::kind_zero, {}, 0, 0};

Payload text_payload(Utf8View text) noexcept {
    return {Payload::kind_text, text, 0, 0};
}

Payload reference_payload(u64 key) noexcept {
    return {Payload::kind_reference, {}, key, 0};
}

// Every event is org.a11y.atspi.Event.<class> / <Name>(siiva{sv}): detail, two numbers, the payload, and no properties.
void emit(const char* path, const char* interface, const char* member, const char* detail, i32 detail1, i32 detail2,
          const Payload& payload) noexcept {
    Server& s = g_server;
    if (!s.bus || !s.embedded)
        return;
    Builder b;
    builder_init(&b);
    builder_string(&b, detail);
    builder_i32(&b, detail1);
    builder_i32(&b, detail2);
    switch (payload.kind) {
    case Payload::kind_text:
        builder_open_variant(&b, "s");
        builder_string(&b, payload.text.bytes ? payload.text.data : "", payload.text.bytes);
        builder_close_variant(&b);
        break;
    case Payload::kind_reference:
        builder_open_variant(&b, "(so)");
        append_node_reference(&b, s.endpoint, payload.key);
        builder_close_variant(&b);
        break;
    case Payload::kind_no_reference:
        builder_open_variant(&b, "(so)");
        append_reference(&b, "", kNullPath);
        builder_close_variant(&b);
        break;
    case Payload::kind_real:
        builder_open_variant(&b, "d");
        builder_double(&b, payload.number);
        builder_close_variant(&b);
        break;
    default:
        builder_open_variant(&b, "i");
        builder_i32(&b, 0);
        builder_close_variant(&b);
        break;
    }
    builder_open_array(&b, "{sv}");
    builder_close_array(&b);
    connection_emit(s.bus, path, interface, member, &b);
    builder_release(&b);
}

void state_event(const UiAccessNode& node, const char* state, bool value) noexcept {
    char path[kPathBytes];
    path_for_key(node.key, path);
    emit(path, kObject, "StateChanged", state, value ? 1 : 0, 0, kZero);
}

// A text a screen reader may read out: never a password's.
bool text_node(const UiAccessNode& node) noexcept {
    return !(node.state & ui_access_password) && (interfaces_for(node) & interface_text);
}

u32 character_boundary_down(const char* text, u32 bytes) noexcept {
    while (bytes && (static_cast<u8>(text[bytes]) & 0xC0u) == 0x80u)
        --bytes;
    return bytes;
}

// Whether a cut at `offset` would split a character.
bool starts_inside(Utf8View text, u32 offset) noexcept {
    return offset < text.bytes && (static_cast<u8>(text.data[offset]) & 0xC0u) == 0x80u;
}

// What an edit did to a text, as the deletion and insertion a screen reader speaks.
void text_change_events(const char* path, Utf8View before, Utf8View after) noexcept {
    u32 prefix = 0;
    const u32 shorter = minimum(before.bytes, after.bytes);
    while (prefix < shorter && before.data[prefix] == after.data[prefix])
        ++prefix;
    // Both texts must be cut on a character boundary.
    while (prefix && (starts_inside(before, prefix) || starts_inside(after, prefix)))
        --prefix;
    u32 suffix = 0;
    while (suffix < shorter - prefix && before.data[before.bytes - 1 - suffix] == after.data[after.bytes - 1 - suffix])
        ++suffix;
    while (suffix && (starts_inside(before, before.bytes - suffix) || starts_inside(after, after.bytes - suffix)))
        --suffix;
    const Utf8View removed{before.data + prefix, before.bytes - prefix - suffix};
    const Utf8View added{after.data + prefix, after.bytes - prefix - suffix};
    const i32 offset = static_cast<i32>(text_character_offset(after, prefix));
    if (removed.bytes)
        emit(path, kObject, "TextChanged", "delete", offset, static_cast<i32>(text_character_count(removed)),
             text_payload(removed));
    if (added.bytes)
        emit(path, kObject, "TextChanged", "insert", offset, static_cast<i32>(text_character_count(added)),
             text_payload(added));
}

void children_event(const UiAccessTree& tree, u32 node) noexcept {
    const UiAccessNode& parent = tree.nodes[node];
    char path[kPathBytes];
    path_for_key(parent.key, path);
    if (parent.child_count)
        emit(path, kObject, "ChildrenChanged", "add", static_cast<i32>(parent.child_count - 1), 0,
             reference_payload(tree.nodes[parent.last_child].key));
    else
        emit(path, kObject, "ChildrenChanged", "remove", 0, 0, {Payload::kind_no_reference, {}, 0, 0});
}

void property_event(const UiAccessTree& tree, const UiAccessPropertyChange& change) noexcept {
    const UiAccessNode& node = tree.nodes[change.node];
    char path[kPathBytes];
    path_for_key(node.key, path);
    const u16 changed = static_cast<u16>(change.old_state ^ node.state);
    switch (change.property) {
    case UiAccessProperty::toggle:
        if (changed & ui_access_checked) {
            state_event(node, "checked", (node.state & ui_access_checked) != 0);
            if (node.role == UiAccessRole::toggle_button)
                state_event(node, "pressed", (node.state & ui_access_checked) != 0);
        }
        if (changed & ui_access_mixed)
            state_event(node, "indeterminate", (node.state & ui_access_mixed) != 0);
        break;
    case UiAccessProperty::selected:
        state_event(node, "selected", (node.state & ui_access_selected) != 0);
        if (node.parent != kUiAccessNone) {
            char parent[kPathBytes];
            path_for_key(tree.nodes[node.parent].key, parent);
            emit(parent, kObject, "SelectionChanged", "", 0, 0, kZero);
        }
        break;
    case UiAccessProperty::expanded:
        if (changed & ui_access_expanded)
            state_event(node, "expanded", (node.state & ui_access_expanded) != 0);
        if (changed & ui_access_collapsed)
            state_event(node, "collapsed", (node.state & ui_access_collapsed) != 0);
        break;
    case UiAccessProperty::name: {
        const Utf8View name = ui_access_name(tree, change.node);
        emit(path, kObject, "PropertyChange", "accessible-name", 0, 0, text_payload(name));
        // A live region is read out as it changes.
        if ((node.state & ui_access_live) && name.bytes)
            emit(path, kObject, "Announcement", "", 1, 0, text_payload(name));
        break;
    }
    case UiAccessProperty::range:
        emit(path, kObject, "PropertyChange", "accessible-value", 0, 0,
             {Payload::kind_real, {}, 0, static_cast<double>(node.range.value)});
        break;
    case UiAccessProperty::value:
        // Only what the user edits is spoken as an edit.
        if (text_node(node) && (node.state & ui_access_focused)) {
            Server& s = g_server;
            const Utf8View before = s.text_key == node.key ? Utf8View{s.text, s.text_bytes} : Utf8View{};
            text_change_events(path, before, ui_access_value(tree, change.node));
        }
        break;
    case UiAccessProperty::text_selection:
        if (text_node(node)) {
            const Utf8View value = ui_access_value(tree, change.node);
            emit(path, kObject, "TextCaretMoved", "", static_cast<i32>(text_character_offset(value, node.placement.selection_caret)),
                 0, kZero);
            emit(path, kObject, "TextSelectionChanged", "", 0, 0, kZero);
        }
        break;
    }
}

// Remembers the focused text so the next edit can be told apart from what was there.
void remember_text(const UiAccessTree& tree) noexcept {
    Server& s = g_server;
    s.text_key = 0;
    s.text_bytes = 0;
    if (tree.focus == kUiAccessNone || !text_node(tree.nodes[tree.focus]))
        return;
    const Utf8View value = ui_access_value(tree, tree.focus);
    u32 bytes = minimum<u32>(value.bytes, sizeof(s.text));
    if (bytes < value.bytes)
        bytes = character_boundary_down(value.data, bytes);
    copy_bytes(s.text, value.data, bytes);
    s.text_key = tree.nodes[tree.focus].key;
    s.text_bytes = bytes;
}

} // namespace

void events_reset() noexcept {
    Server& s = g_server;
    s.focus_key = 0;
    s.active = false;
    s.text_key = 0;
    s.text_bytes = 0;
}

void server_publish(const UiAccessTree& tree, const UiAccessEventPlan& plan) noexcept {
    Server& s = g_server;
    if (!s.bus || !s.embedded || !tree.count)
        return;
    const HostCallbacks& host = s.endpoint.host;
    char window[kPathBytes];
    path_for_key(tree.nodes[0].key, window);

    const bool active = host.window_active(host.context);
    if (active != s.active) {
        s.active = active;
        emit(window, kWindow, active ? "Activate" : "Deactivate", "", 0, 0, kZero);
        emit(window, kObject, "StateChanged", "active", active ? 1 : 0, 0, kZero);
    }
    if (plan.structure_root)
        children_event(tree, 0);
    else
        for (u32 i = 0; i < plan.structure_count; ++i)
            children_event(tree, plan.structures[i]);
    for (u32 i = 0; i < plan.property_count; ++i)
        property_event(tree, plan.properties[i]);

    // Focus follows the active window: on Wayland the compositor activates the window that holds the keyboard focus.
    const u64 focus_key = active && tree.focus != kUiAccessNone ? tree.nodes[tree.focus].key : 0;
    if (focus_key != s.focus_key) {
        const u32 old_node = s.focus_key ? ui_access_find(tree, s.focus_key) : kUiAccessNone;
        if (old_node != kUiAccessNone)
            state_event(tree.nodes[old_node], "focused", false);
        if (focus_key) {
            state_event(tree.nodes[tree.focus], "focused", true);
            char path[kPathBytes];
            path_for_key(focus_key, path);
            emit(path, kFocusEvent, "Focus", "", 0, 0, kZero);
        }
        s.focus_key = focus_key;
    }
    remember_text(tree);
}

void server_announce(u64 key, Utf8View text, bool assertive) noexcept {
    Server& s = g_server;
    if (!s.bus || !s.embedded || !text.bytes)
        return;
    char path[kPathBytes];
    path_for_key(key ? key : ui_access_key(0, 0), path);
    emit(path, kObject, "Announcement", "", assertive ? 2 : 1, 0, text_payload(text));
}

} // namespace maicord::atspi
