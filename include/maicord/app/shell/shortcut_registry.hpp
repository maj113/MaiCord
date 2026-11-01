#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

// One table of the built-in keyboard shortcuts MaiCord implements, the
// precedence that resolves chords shared between focus scopes, display text,
// the key recorder and the user's custom (voice) keybinds. Input handlers ask
// the registry instead of comparing keys, so the shortcuts overlay and the
// Keybinds settings page list exactly what the window routes.
enum ShortcutModifier : u8 {
    shortcut_ctrl = 1u << 0,
    shortcut_alt = 1u << 1,
    shortcut_shift = 1u << 2,
    shortcut_modifier_mask = 7,
};
// Win32 virtual-key code plus modifiers; key zero is unbound.
struct ShortcutChord {
    u8 key, modifiers;
};
constexpr bool shortcut_chord_equal(ShortcutChord a, ShortcutChord b) noexcept {
    return a.key && a.key == b.key && a.modifiers == b.modifiers;
}
// The chord a key-down forms with the modifier keys currently held.
ShortcutChord shortcut_chord_from_keyboard(u32 key) noexcept;

enum class ShortcutGroup : u8 {
    navigation,
    chat,
    messages,
    voice,
    misc,
    count
};
enum class ShortcutAction : u8 {
    none,
    server_previous,
    server_next,
    channel_previous,
    channel_next,
    unread_previous,
    unread_next,
    mention_previous,
    mention_next,
    quick_switcher,
    mark_server_read,
    mark_channel_read,
    toggle_pins,
    toggle_inbox,
    toggle_members,
    emoji_picker,
    gif_picker,
    sticker_picker,
    upload_file,
    edit_last_message,
    message_reply,
    message_edit,
    message_forward,
    message_react,
    save_attachment,
    toggle_mute,
    toggle_deafen,
    search,
    shortcuts_help,
    open_settings,
    downloads,
    count
};

// What holds keyboard focus in the main window.
enum class ShortcutFocus : u8 {
    content,  // Panes and lists without a caret.
    message,  // A keyboard-focused message.
    composer, // The message composer's text.
    text      // Any other text field (search, picker and switcher queries).
};
// Where a shortcut applies. Resolution tries the most specific scope first:
// message, empty composer, content, then global.
enum class ShortcutScope : u8 {
    global,         // Everywhere in the window, text fields included.
    content,        // Not while another text field owns the caret; the composer is allowed.
    empty_composer, // The composer with no text and no edit in progress.
    message         // A keyboard-focused message.
};
struct ShortcutContext {
    ShortcutFocus focus;
    bool composer_empty;
};
struct ShortcutDefinition {
    ShortcutAction action;
    ShortcutGroup group;
    ShortcutScope scope;
    Utf8View label;
    ShortcutChord chords[2]; // Second is an optional alternate.
};

const ShortcutDefinition* shortcut_definition(ShortcutAction) noexcept;
Utf8View shortcut_group_label(ShortcutGroup) noexcept;
bool shortcut_scope_active(ShortcutScope, ShortcutContext) noexcept;
// Actions bound to the chord that apply in the context, most specific first.
// The caller runs them in order until one handles the key.
u32 shortcut_resolve(ShortcutContext, ShortcutChord, Slice<ShortcutAction> output) noexcept;
ShortcutAction shortcut_first(ShortcutContext, ShortcutChord) noexcept;
// True when the chord is one of the action's bindings (scope not considered).
bool shortcut_matches(ShortcutAction, ShortcutChord) noexcept;
// Any built-in definition uses the chord.
const ShortcutDefinition* shortcut_built_in(ShortcutChord) noexcept;

// Display text. Keys use US names ("K", "↑", "Esc", "Page Up", "Num +");
// chords join modifiers and key with "+" ("Ctrl+Shift+M"). Return bytes written.
u32 shortcut_key_label(u8 key, Slice<char> output) noexcept;
Utf8View shortcut_modifier_label(u8 modifier) noexcept;
u32 shortcut_chord_label(ShortcutChord, Slice<char> output) noexcept;

// Overlay and settings listing: group headers, then one row per definition,
// with previous/next pairs that share a label merged into one row. An empty
// query lists everything; otherwise every query word must prefix a word of the
// label, group or chord text.
struct ShortcutListingRow {
    ShortcutGroup group;
    bool header;
    Utf8View label;
    ShortcutChord chords[2];
};
u32 shortcut_listing(Utf8View query, Slice<ShortcutListingRow> output) noexcept;

// Key recorder shared by the push-to-talk shortcut and custom keybinds.
// Single-key mode takes the first key pressed (modifier keys included); chord
// mode waits for a non-modifier key and records the modifiers held with it.
// Esc cancels and Backspace or Delete clears, both without modifiers.
enum class ShortcutRecordMode : u8 {
    single_key,
    chord
};
enum class ShortcutRecordStep : u8 {
    ignored,
    pending,
    recorded,
    cancelled,
    cleared
};
struct ShortcutRecorder {
    ShortcutRecordMode mode;
    bool active;
};
void shortcut_recorder_start(ShortcutRecorder*, ShortcutRecordMode) noexcept;
ShortcutRecordStep shortcut_recorder_input(ShortcutRecorder*, bool down, u32 key, u8 modifiers,
                                           ShortcutChord* recorded) noexcept;
bool shortcut_key_is_modifier(u32 key) noexcept;

// Custom keybinds: user chords for voice actions that also work while the
// window is in the background during a call. Push to talk lives in the voice
// preferences (one key held by the capture thread); the others persist in
// %LOCALAPPDATA%\MaiCord\keybinds.bin.
enum class CustomKeybindAction : u8 {
    push_to_talk,
    toggle_mute,
    toggle_deafen,
    count
};
inline constexpr u32 kCustomKeybindMaximum = 8;
inline constexpr u32 kCustomKeybindsVersion = 1;
inline constexpr u32 kCustomKeybindsHeaderBytes = 32, kCustomKeybindBytes = 4;
inline constexpr u32 kCustomKeybindsMaximumBytes = kCustomKeybindsHeaderBytes + kCustomKeybindMaximum * kCustomKeybindBytes;
struct CustomKeybind {
    CustomKeybindAction action;
    ShortcutChord chord;
};
struct CustomKeybinds {
    u32 count;
    CustomKeybind items[kCustomKeybindMaximum];
};
enum class CustomKeybindError : u8 {
    none,
    invalid,        // Unbound key, unknown modifier or an action stored elsewhere.
    needs_modifier, // Would fire while typing: add Ctrl or Alt, or use a function key.
    reserved,       // A built-in shortcut already uses the chord.
    system,         // Text editing or Windows uses the chord (Ctrl+V, Alt+F4, Shift+arrows).
    duplicate,      // Another custom keybind uses the chord.
    full
};
Utf8View custom_keybind_action_label(CustomKeybindAction) noexcept;
Utf8View custom_keybind_error_text(CustomKeybindError) noexcept;
CustomKeybindError custom_keybind_check(const CustomKeybinds&, CustomKeybind) noexcept;
bool custom_keybinds_add(CustomKeybinds*, CustomKeybind, CustomKeybindError* error = nullptr) noexcept;
bool custom_keybinds_remove(CustomKeybinds*, u32 index) noexcept;
// Index of the keybind using the chord, or kCustomKeybindMaximum.
u32 custom_keybinds_find(const CustomKeybinds&, ShortcutChord) noexcept;
bool custom_keybinds_valid(const CustomKeybinds&) noexcept;
// Little-endian record with a checksum; decode leaves the output untouched on failure.
u32 custom_keybinds_encode(const CustomKeybinds&, Slice<u8> output) noexcept;
bool custom_keybinds_decode(ConstSlice<u8> record, CustomKeybinds*) noexcept;
bool custom_keybinds_load_file(const wchar_t* path, CustomKeybinds*) noexcept;
bool custom_keybinds_save_file(const wchar_t* path, const CustomKeybinds&) noexcept;
bool custom_keybinds_load(CustomKeybinds*) noexcept;
bool custom_keybinds_save(const CustomKeybinds&) noexcept;

// Plugin keybinds: why a plugin chord can't be used (a built-in action's label,
// "a system shortcut", "a custom keybind", "Push to talk", ...), or empty when
// it is free. Chords need Ctrl or Alt unless the key is F1-F24.
// push_to_talk_key is the bound push-to-talk key, or zero.
Utf8View shortcut_plugin_chord_conflict(const CustomKeybinds&, ShortcutChord, u8 push_to_talk_key) noexcept;

} // namespace maicord
