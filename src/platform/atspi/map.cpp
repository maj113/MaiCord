#include "maicord/core/slice.hpp"
#include "maicord/platform/atspi/map.hpp"

namespace maicord::atspi {
namespace {

constexpr const char kPathPrefix[] = "/org/a11y/atspi/accessible/";
constexpr u32 kPrefixBytes = sizeof(kPathPrefix) - 1;

void set_state(u32 (&words)[2], State state) noexcept {
    words[static_cast<u32>(state) >> 5] |= 1u << (static_cast<u32>(state) & 31u);
}

bool edit_role(UiAccessRole role) noexcept {
    return role == UiAccessRole::edit || role == UiAccessRole::combo_box;
}

bool text_node(const UiAccessNode& node) noexcept {
    return (node.actions & ui_access_action_text) || (edit_role(node.role) && (node.actions & ui_access_action_value));
}

bool editable_node(const UiAccessNode& node) noexcept {
    return edit_role(node.role) && (node.actions & ui_access_action_value) && !(node.state & ui_access_read_only);
}

bool selection_container(const UiAccessNode& node) noexcept {
    return node.role == UiAccessRole::list || node.role == UiAccessRole::tree || node.role == UiAccessRole::tab_list ||
           (node.role == UiAccessRole::group && (node.actions & ui_access_action_select));
}

bool checkable_node(const UiAccessNode& node) noexcept {
    return (node.actions & ui_access_action_toggle) || (node.state & ui_access_toggleable) ||
           node.role == UiAccessRole::check_box || node.role == UiAccessRole::radio_button ||
           node.role == UiAccessRole::toggle_button;
}

bool expandable_node(const UiAccessNode& node) noexcept {
    return (node.actions & ui_access_action_expand) || (node.state & (ui_access_expanded | ui_access_collapsed));
}

u32 hex_value(char c) noexcept {
    if (c >= '0' && c <= '9')
        return static_cast<u32>(c - '0');
    if (c >= 'a' && c <= 'f')
        return static_cast<u32>(c - 'a' + 10);
    return 16;
}

bool continuation(char c) noexcept {
    return (static_cast<u8>(c) & 0xC0u) == 0x80u;
}

} // namespace

u32 role_for(const UiAccessNode& node) noexcept {
    switch (node.role) {
    case UiAccessRole::window:
        return role_frame;
    case UiAccessRole::pane:
        return role_panel;
    case UiAccessRole::group:
        return role_grouping;
    case UiAccessRole::dialog:
        return role_dialog;
    case UiAccessRole::button:
        return role_push_button;
    case UiAccessRole::toggle_button:
        return role_toggle_button;
    case UiAccessRole::check_box:
        return role_check_box;
    case UiAccessRole::radio_button:
        return role_radio_button;
    case UiAccessRole::combo_box:
        return role_combo_box;
    case UiAccessRole::list:
        return role_list;
    case UiAccessRole::list_item:
        return role_list_item;
    case UiAccessRole::tree:
        return role_tree;
    case UiAccessRole::tab_list:
        return role_page_tab_list;
    case UiAccessRole::tab:
        return role_page_tab;
    case UiAccessRole::menu:
        return role_menu;
    case UiAccessRole::menu_item:
        return role_menu_item;
    case UiAccessRole::text:
        return role_label;
    case UiAccessRole::header:
        return role_heading;
    case UiAccessRole::edit:
        if (node.state & ui_access_password)
            return role_password_text;
        return (node.state & ui_access_multiline) ? role_text : role_entry;
    case UiAccessRole::slider:
        return role_slider;
    case UiAccessRole::image:
        return role_image;
    case UiAccessRole::link:
        return role_link;
    case UiAccessRole::status:
        return role_status_bar;
    default:
        return role_filler;
    }
}

const char* role_name(u32 role) noexcept {
    switch (role) {
    case role_check_box: return "check box";
    case role_combo_box: return "combo box";
    case role_dialog: return "dialog";
    case role_filler: return "filler";
    case role_frame: return "frame";
    case role_image: return "image";
    case role_label: return "label";
    case role_list: return "list";
    case role_list_item: return "list item";
    case role_menu: return "menu";
    case role_menu_item: return "menu item";
    case role_page_tab: return "page tab";
    case role_page_tab_list: return "page tab list";
    case role_panel: return "panel";
    case role_password_text: return "password text";
    case role_push_button: return "push button";
    case role_radio_button: return "radio button";
    case role_slider: return "slider";
    case role_status_bar: return "status bar";
    case role_text: return "text";
    case role_toggle_button: return "toggle button";
    case role_tree: return "tree";
    case role_application: return "application";
    case role_entry: return "entry";
    case role_heading: return "heading";
    case role_link: return "link";
    case role_grouping: return "grouping";
    default: return "unknown";
    }
}

void states_for(const UiAccessNode& node, bool window_active, u32 words[2]) noexcept {
    u32 set[2]{};
    const u16 state = node.state;
    if (!(state & ui_access_disabled)) {
        set_state(set, state_enabled);
        set_state(set, state_sensitive);
    }
    if (!(state & ui_access_offscreen)) {
        set_state(set, state_showing);
        set_state(set, state_visible);
    }
    if ((state & (ui_access_focusable | ui_access_focused)) || (node.actions & ui_access_action_focus))
        set_state(set, state_focusable);
    if ((state & ui_access_focused) && window_active)
        set_state(set, state_focused);
    // The window is active, and so is the dialog it shows (Orca counts dialogs that lack ACTIVE as "unfocused").
    if ((node.role == UiAccessRole::window || (node.role == UiAccessRole::dialog && !(state & ui_access_offscreen))) &&
        window_active)
        set_state(set, state_active);
    if ((state & ui_access_selectable) || (node.actions & ui_access_action_select))
        set_state(set, state_selectable);
    if (state & ui_access_selected)
        set_state(set, state_selected);
    if (checkable_node(node))
        set_state(set, state_checkable);
    if (state & ui_access_checked) {
        set_state(set, state_checked);
        if (node.role == UiAccessRole::toggle_button)
            set_state(set, state_pressed);
    }
    if (state & ui_access_mixed)
        set_state(set, state_indeterminate);
    if (expandable_node(node))
        set_state(set, state_expandable);
    if (state & ui_access_expanded)
        set_state(set, state_expanded);
    if (state & ui_access_collapsed)
        set_state(set, state_collapsed);
    if (edit_role(node.role)) {
        if (editable_node(node))
            set_state(set, state_editable);
        if (state & ui_access_read_only)
            set_state(set, state_read_only);
        set_state(set, (state & ui_access_multiline) ? state_multi_line : state_single_line);
    }
    if (node.role == UiAccessRole::combo_box)
        set_state(set, state_has_popup);
    if (state & ui_access_modal)
        set_state(set, state_modal);
    words[0] = set[0];
    words[1] = set[1];
}

u32 interfaces_for(const UiAccessNode& node) noexcept {
    u32 interfaces = interface_accessible | interface_component;
    ActionEntry entries[kActionMaximum];
    if (actions_for(node, entries))
        interfaces |= interface_action;
    if (text_node(node))
        interfaces |= interface_text;
    if (editable_node(node))
        interfaces |= interface_editable_text;
    if (node.actions & ui_access_action_range)
        interfaces |= interface_value;
    if (selection_container(node))
        interfaces |= interface_selection;
    return interfaces;
}

const char* interface_name(u32 bit) noexcept {
    switch (bit) {
    case interface_accessible: return "org.a11y.atspi.Accessible";
    case interface_component: return "org.a11y.atspi.Component";
    case interface_action: return "org.a11y.atspi.Action";
    case interface_text: return "org.a11y.atspi.Text";
    case interface_editable_text: return "org.a11y.atspi.EditableText";
    case interface_value: return "org.a11y.atspi.Value";
    case interface_selection: return "org.a11y.atspi.Selection";
    default: return nullptr;
    }
}

u32 actions_for(const UiAccessNode& node, ActionEntry (&entries)[kActionMaximum]) noexcept {
    u32 count = 0;
    if (node.actions & ui_access_action_invoke)
        entries[count++] = {"click", "Activates the control", UiAccessAction::invoke};
    if (node.actions & ui_access_action_toggle)
        entries[count++] = {"toggle", "Switches the control on or off", UiAccessAction::toggle};
    if (node.actions & ui_access_action_select)
        entries[count++] = {"select", "Selects the item", UiAccessAction::select};
    if (expandable_node(node)) {
        if (node.state & ui_access_expanded)
            entries[count++] = {"collapse", "Collapses the item", UiAccessAction::collapse};
        else
            entries[count++] = {"expand", "Expands the item", UiAccessAction::expand};
    }
    return count;
}

void path_for_key(u64 key, char (&path)[kPathBytes]) noexcept {
    for (u32 i = 0; i < kPrefixBytes; ++i)
        path[i] = kPathPrefix[i];
    for (u32 digit = 0; digit < 16; ++digit)
        path[kPrefixBytes + digit] = "0123456789abcdef"[(key >> (60 - 4 * digit)) & 15u];
    path[kPrefixBytes + 16] = 0;
}

bool key_from_path(const char* path, u64* key) noexcept {
    if (!path)
        return false;
    for (u32 i = 0; i < kPrefixBytes; ++i)
        if (path[i] != kPathPrefix[i])
            return false;
    u64 value = 0;
    for (u32 digit = 0; digit < 16; ++digit) {
        const u32 nibble = hex_value(path[kPrefixBytes + digit]);
        if (nibble > 15)
            return false;
        value = (value << 4) | nibble;
    }
    if (path[kPrefixBytes + 16] != 0)
        return false;
    *key = value;
    return true;
}

bool is_root_path(const char* path) noexcept {
    return path && c_string_equal(path, kRootPath);
}

u32 text_character_count(Utf8View text) noexcept {
    u32 count = 0;
    for (u32 i = 0; i < text.bytes; ++i)
        count += continuation(text.data[i]) ? 0u : 1u;
    return count;
}

u32 text_byte_offset(Utf8View text, u32 characters) noexcept {
    u32 seen = 0;
    for (u32 i = 0; i < text.bytes; ++i) {
        if (continuation(text.data[i]))
            continue;
        if (seen == characters)
            return i;
        ++seen;
    }
    return text.bytes;
}

u32 text_character_offset(Utf8View text, u32 bytes) noexcept {
    return text_character_count({text.data, bytes < text.bytes ? bytes : text.bytes});
}

u32 text_scalar_at(Utf8View text, u32 byte_offset) noexcept {
    if (byte_offset >= text.bytes)
        return 0;
    const u8 lead = static_cast<u8>(text.data[byte_offset]);
    u32 length = 1;
    u32 value = lead;
    if (lead >= 0xF0u) {
        length = 4;
        value = lead & 0x07u;
    } else if (lead >= 0xE0u) {
        length = 3;
        value = lead & 0x0Fu;
    } else if (lead >= 0xC0u) {
        length = 2;
        value = lead & 0x1Fu;
    }
    for (u32 i = 1; i < length; ++i) {
        if (byte_offset + i >= text.bytes || !continuation(text.data[byte_offset + i]))
            return 0xFFFDu;
        value = (value << 6) | (static_cast<u8>(text.data[byte_offset + i]) & 0x3Fu);
    }
    return value;
}

void text_span(Utf8View text, u32 byte_offset, TextGranularity granularity, i32 direction, u32* start,
               u32* end) noexcept {
    UiAccessTextUnit unit = UiAccessTextUnit::character;
    switch (granularity) {
    case TextGranularity::word:
        unit = UiAccessTextUnit::word;
        break;
    case TextGranularity::sentence:
    case TextGranularity::line:
        unit = UiAccessTextUnit::line;
        break;
    case TextGranularity::paragraph:
        unit = UiAccessTextUnit::paragraph;
        break;
    default:
        break;
    }
    u32 position = ui_access_text_align(text, byte_offset);
    *start = *end = text.bytes;
    if (!text.bytes)
        return;
    if (position >= text.bytes) {
        // Past the last character: nothing at or after it, the last unit before it.
        if (direction < 0) {
            u32 last = text.bytes - 1;
            ui_access_text_expand(text, unit, &last, end);
            *start = last;
        }
        return;
    }
    u32 first = position, last = position;
    ui_access_text_expand(text, unit, &first, &last);
    if (direction == 0) {
        *start = first;
        *end = last;
    } else if (direction < 0) {
        if (first == 0) {
            *start = *end = 0;
            return;
        }
        u32 before = first - 1;
        u32 before_end = first;
        ui_access_text_expand(text, unit, &before, &before_end);
        *start = before;
        *end = before_end;
    } else if (last < text.bytes) {
        u32 after = last;
        u32 after_end = last;
        ui_access_text_expand(text, unit, &after, &after_end);
        *start = after;
        *end = after_end;
    }
}

} // namespace maicord::atspi
