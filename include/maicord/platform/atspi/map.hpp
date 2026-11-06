#pragma once

#include "maicord/core/base.hpp"
#include "maicord/ui/accessibility/accessibility.hpp"

// How an accessibility snapshot node (ui/accessibility/accessibility.hpp) presents itself over AT-SPI (Linux, the
// org.a11y.atspi interfaces): its role, state set, interfaces, actions, object path and text offsets. Pure functions; the
// calls that answer the screen reader are in accessible.hpp, the connection and the events in server.hpp.
//
// The numbers are the AtspiRole and AtspiStateType values of at-spi2-core, which are the wire values.

namespace maicord::atspi {

enum Role : u32 {
    role_check_box = 7,
    role_combo_box = 11,
    role_dialog = 16,
    role_filler = 20,
    role_frame = 23,
    role_image = 27,
    role_label = 29,
    role_list = 31,
    role_list_item = 32,
    role_menu = 33,
    role_menu_item = 35,
    role_page_tab = 37,
    role_page_tab_list = 38,
    role_panel = 39,
    role_password_text = 40,
    role_push_button = 43,
    role_radio_button = 44,
    role_slider = 51,
    role_status_bar = 54,
    role_text = 61,
    role_toggle_button = 62,
    role_tree = 65,
    role_application = 75,
    role_entry = 79,
    role_heading = 83,
    role_link = 88,
    role_grouping = 99,
};

enum State : u32 {
    state_active = 1,
    state_checked = 4,
    state_collapsed = 5,
    state_editable = 7,
    state_enabled = 8,
    state_expandable = 9,
    state_expanded = 10,
    state_focusable = 11,
    state_focused = 12,
    state_modal = 16,
    state_multi_line = 17,
    state_pressed = 20,
    state_selectable = 22,
    state_selected = 23,
    state_sensitive = 24,
    state_showing = 25,
    state_single_line = 26,
    state_visible = 30,
    state_indeterminate = 32,
    state_checkable = 41,
    state_has_popup = 42,
    state_read_only = 43,
};

// The role of a node; the window node is a frame, and every edit is an entry, a text area or a password field.
u32 role_for(const UiAccessNode& node) noexcept;
// The English role name ("push button") AT-SPI clients show for a role; "unknown" for a number not listed here.
const char* role_name(u32 role) noexcept;

// The state set as the two 32-bit words of the wire (state k is bit k % 32 of word k / 32). `window_active` marks the
// window node as the active window and lets a focused node report focus.
void states_for(const UiAccessNode& node, bool window_active, u32 words[2]) noexcept;

enum Interface : u32 {
    interface_accessible = 1u << 0,
    interface_component = 1u << 1,
    interface_action = 1u << 2,
    interface_text = 1u << 3,
    interface_editable_text = 1u << 4,
    interface_value = 1u << 5,
    interface_selection = 1u << 6,
};
// The interfaces a node answers (always Accessible and Component).
u32 interfaces_for(const UiAccessNode& node) noexcept;
// Wire name of one Interface bit ("org.a11y.atspi.Text").
const char* interface_name(u32 bit) noexcept;

struct ActionEntry {
    const char* name;
    const char* description;
    UiAccessAction action;
};
inline constexpr u32 kActionMaximum = 4;
// The AT-SPI actions of a node, in the order a screen reader offers them (the first is the default); the count.
u32 actions_for(const UiAccessNode& node, ActionEntry (&entries)[kActionMaximum]) noexcept;

// Object paths: /org/a11y/atspi/accessible/<16 hex digits of the node key>; the application is .../root.
inline constexpr const char* kRootPath = "/org/a11y/atspi/accessible/root";
inline constexpr const char* kNullPath = "/org/a11y/atspi/null";
inline constexpr u32 kPathBytes = 48;
void path_for_key(u64 key, char (&path)[kPathBytes]) noexcept;
// False for the root and for anything that is not a node path.
bool key_from_path(const char* path, u64* key) noexcept;
bool is_root_path(const char* path) noexcept;

// Text offsets. AT-SPI counts Unicode scalars; the snapshot's values are UTF-8 and its selections are byte offsets.
u32 text_character_count(Utf8View text) noexcept;
// The byte offset of the character at index `characters` (the length when it is past the end).
u32 text_byte_offset(Utf8View text, u32 characters) noexcept;
// The index of the character at a byte offset: the characters that start before it (the snapshot's offsets are aligned).
u32 text_character_offset(Utf8View text, u32 bytes) noexcept;
// The scalar value starting at a byte offset; 0 at the end.
u32 text_scalar_at(Utf8View text, u32 byte_offset) noexcept;

// AtspiTextGranularity: character, word, sentence, line, paragraph.
enum class TextGranularity : u8 { character, word, sentence, line, paragraph };
// The span (byte offsets) of the unit around the character at `byte_offset` (`direction` 0), before it (-1) or after it
// (+1); empty at the ends. Words include the spaces after them; sentences are lines, as the snapshot has no sentences.
void text_span(Utf8View text, u32 byte_offset, TextGranularity granularity, i32 direction, u32* start,
               u32* end) noexcept;

} // namespace maicord::atspi
