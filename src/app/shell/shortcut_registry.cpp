#include "maicord/app/shell/shortcut_registry.hpp"
#include "maicord/app/account/account_profile.hpp"
#include "maicord/app/net/file_persistence.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/os_text.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/platform/window/key.hpp"
#include "maicord/platform/window/window.hpp"

namespace maicord {
namespace {
using A = ShortcutAction;
using G = ShortcutGroup;
using S = ShortcutScope;
constexpr u8 C = shortcut_ctrl, L = shortcut_alt, H = shortcut_shift;

static constexpr ShortcutDefinition kDefinitions[]{
    {A::server_previous, G::navigation, S::global, {{key::up, C | L}, {}}},
    {A::server_next, G::navigation, S::global, {{key::down, C | L}, {}}},
    {A::channel_previous, G::navigation, S::global, {{key::up, L}, {}}},
    {A::channel_next, G::navigation, S::global, {{key::down, L}, {}}},
    {A::unread_previous, G::navigation, S::global, {{key::up, L | H}, {}}},
    {A::unread_next, G::navigation, S::global, {{key::down, L | H}, {}}},
    {A::mention_previous, G::navigation, S::global, {{key::up, C | L | H}, {}}},
    {A::mention_next, G::navigation, S::global, {{key::down, C | L | H}, {}}},
    {A::quick_switcher, G::navigation, S::global, {{'K', C}, {}}},
    {A::tab_next, G::navigation, S::global, {{key::tab, C}, {key::page_down, C}}},
    {A::tab_previous, G::navigation, S::global, {{key::tab, C | H}, {key::page_up, C}}},
    {A::tab_close, G::navigation, S::global, {{'W', C}, {}}},
    {A::tab_new, G::navigation, S::global, {{'T', C}, {}}},
    {A::tab_first, G::navigation, S::global, {{'1', C}, {}}},
    {A::tab_second, G::navigation, S::global, {{'2', C}, {}}},
    {A::tab_third, G::navigation, S::global, {{'3', C}, {}}},
    {A::tab_fourth, G::navigation, S::global, {{'4', C}, {}}},
    {A::tab_fifth, G::navigation, S::global, {{'5', C}, {}}},
    {A::tab_sixth, G::navigation, S::global, {{'6', C}, {}}},
    {A::tab_seventh, G::navigation, S::global, {{'7', C}, {}}},
    {A::tab_eighth, G::navigation, S::global, {{'8', C}, {}}},
    {A::tab_last, G::navigation, S::global, {{'9', C}, {}}},
    {A::mark_server_read, G::chat, S::content, {{key::escape, H}, {}}},
    {A::mark_channel_read, G::chat, S::content, {{key::escape, 0}, {}}},
    {A::toggle_pins, G::chat, S::global, {{'P', C}, {}}},
    {A::toggle_inbox, G::chat, S::global, {{'I', C}, {}}},
    {A::toggle_members, G::chat, S::global, {{'U', C}, {}}},
    {A::emoji_picker, G::chat, S::global, {{'E', C}, {}}},
    {A::gif_picker, G::chat, S::global, {{'G', C}, {}}},
    {A::sticker_picker, G::chat, S::global, {{'S', C}, {}}},
    {A::upload_file, G::chat, S::global, {{'U', C | H}, {}}},
    {A::edit_last_message, G::chat, S::empty_composer, {{key::up, 0}, {}}},
    {A::message_reply, G::messages, S::message, {{'R', 0}, {}}},
    {A::message_edit, G::messages, S::message, {{'E', 0}, {}}},
    {A::message_forward, G::messages, S::message, {{'F', 0}, {}}},
    {A::message_react, G::messages, S::message, {{key::equals, 0}, {key::add, 0}}},
    {A::save_attachment, G::messages, S::message, {{'S', C}, {}}},
    {A::toggle_mute, G::voice, S::global, {{'M', C | H}, {}}},
    {A::toggle_deafen, G::voice, S::global, {{'D', C | H}, {}}},
    {A::search, G::misc, S::global, {{'F', C}, {'F', C | H}}},
    {A::shortcuts_help, G::misc, S::global, {{key::slash, C}, {}}},
    {A::open_settings, G::misc, S::global, {{key::comma, C}, {}}},
    {A::downloads, G::misc, S::global, {{'J', C}, {}}},
    {A::zoom_in, G::misc, S::global, {{key::equals, C}, {key::add, C}}},
    {A::zoom_out, G::misc, S::global, {{key::minus, C}, {key::subtract, C}}},
    {A::zoom_reset, G::misc, S::global, {{'0', C}, {}}},
};
constexpr u32 kDefinitionCount = sizeof(kDefinitions) / sizeof(kDefinitions[0]);
static_assert(kDefinitionCount == static_cast<u32>(ShortcutAction::count) - 1);
// The definitions' labels, one name each in the same order (a previous/next pair repeats its label and lists as one row).
constexpr char kLabels[] =
    "Navigate between servers\0" "Navigate between servers\0" "Navigate between channels\0"
    "Navigate between channels\0" "Navigate between unread channels\0" "Navigate between unread channels\0"
    "Navigate between unread mentions\0" "Navigate between unread mentions\0" "Toggle quick switcher\0" "Next tab\0"
    "Previous tab\0" "Close tab\0" "Pin this channel as a tab\0" "Go to tab 1\0" "Go to tab 2\0" "Go to tab 3\0"
    "Go to tab 4\0" "Go to tab 5\0" "Go to tab 6\0" "Go to tab 7\0" "Go to tab 8\0" "Go to the last tab\0"
    "Mark server read\0" "Mark channel read\0" "Toggle pinned messages\0" "Toggle inbox\0" "Toggle member list\0"
    "Toggle emoji picker\0" "Toggle GIF picker\0" "Toggle sticker picker\0" "Upload a file\0"
    "Edit your last message\0" "Reply\0" "Edit message\0" "Forward message\0" "Add reaction\0" "Save attachment\0"
    "Toggle mute\0" "Toggle deafen\0" "Search\0" "Show keyboard shortcuts\0" "Open user settings\0" "Show downloads\0"
    "Zoom in\0" "Zoom out\0" "Reset zoom\0";
static_assert(name_list_count(kLabels) == kDefinitionCount);

constexpr char kGroupLabels[] = "Navigation\0Chat\0Messages\0Voice\0Miscellaneous\0";
static_assert(name_list_count(kGroupLabels) == static_cast<u32>(ShortcutGroup::count));

constexpr bool binds(const ShortcutDefinition& definition, ShortcutChord chord) noexcept {
    return shortcut_chord_equal(definition.chords[0], chord) || shortcut_chord_equal(definition.chords[1], chord);
}
// Definitions follow action order, every chord is bound once per
// scope and groups stay contiguous so listings show each header once.
constexpr bool definitions_valid() noexcept {
    for (u32 i = 0; i < kDefinitionCount; ++i) {
        const auto& a = kDefinitions[i];
        if (a.action != static_cast<ShortcutAction>(i + 1) || !a.chords[0].key ||
            a.group >= ShortcutGroup::count || a.scope > ShortcutScope::message ||
            ((a.chords[0].modifiers | a.chords[1].modifiers) & ~shortcut_modifier_mask) ||
            shortcut_chord_equal(a.chords[0], a.chords[1]))
            return false;
        if (i && a.group < kDefinitions[i - 1].group)
            return false;
        for (u32 j = i + 1; j < kDefinitionCount; ++j)
            for (const auto chord : a.chords)
                if (chord.key && a.scope == kDefinitions[j].scope && binds(kDefinitions[j], chord))
                    return false;
    }
    return true;
}
static_assert(definitions_valid());
u32 put(const Slice<char>& output, u32 at, const Utf8View& text) noexcept {
    if (!output.data || at + text.bytes > output.count)
        return at;
    copy_bytes(output.data + at, text.data, text.bytes);
    return at + text.bytes;
}
u32 put_char(const Slice<char>& output, u32 at, char value) noexcept {
    if (!output.data || at >= output.count)
        return at;
    output.data[at] = value;
    return at + 1;
}
u32 put_decimal(Slice<char> output, u32 at, u32 value) noexcept {
    if (value >= 10)
        at = put_char(output, at, static_cast<char>('0' + value / 10));
    return put_char(output, at, static_cast<char>('0' + value % 10));
}
// The fixed key names, one blob: each entry is the key's code byte, its text and a NUL, a second NUL ends the list.
// A switch of Utf8View literals spends code, a pointer and a length per name; this spends only the text.
#define KEY_NAME(code, text) code text "\0"
constexpr char kKeyNames[] =
    KEY_NAME("\x26", "\xE2\x86\x91")  // up
    KEY_NAME("\x28", "\xE2\x86\x93")  // down
    KEY_NAME("\x25", "\xE2\x86\x90")  // left
    KEY_NAME("\x27", "\xE2\x86\x92")  // right
    KEY_NAME("\x1B", "Esc")
    KEY_NAME("\x0D", "Enter")
    KEY_NAME("\x09", "Tab")
    KEY_NAME("\x20", "Space")
    KEY_NAME("\x08", "Backspace")
    KEY_NAME("\x2E", "Delete")
    KEY_NAME("\x2D", "Insert")
    KEY_NAME("\x24", "Home")
    KEY_NAME("\x23", "End")
    KEY_NAME("\x21", "Page Up")
    KEY_NAME("\x22", "Page Down")
    KEY_NAME("\x13", "Pause")
    KEY_NAME("\x91", "Scroll Lock")
    KEY_NAME("\x14", "Caps Lock")
    KEY_NAME("\x90", "Num Lock")
    KEY_NAME("\x2C", "Print Screen")
    KEY_NAME("\x5D", "Menu")
    KEY_NAME("\x6A", "Num *")
    KEY_NAME("\x6B", "Num +")
    KEY_NAME("\x6D", "Num -")
    KEY_NAME("\x6E", "Num .")
    KEY_NAME("\x6F", "Num /")
    KEY_NAME("\xBA", ";")
    KEY_NAME("\xBB", "+")
    KEY_NAME("\xBC", ",")
    KEY_NAME("\xBD", "-")
    KEY_NAME("\xBE", ".")
    KEY_NAME("\xBF", "/")
    KEY_NAME("\xC0", "`")
    KEY_NAME("\xDB", "[")
    KEY_NAME("\xDC", "\\")
    KEY_NAME("\xDD", "]")
    KEY_NAME("\xDE", "'")
    KEY_NAME("\x10", "Shift")
    KEY_NAME("\xA0", "Left Shift")
    KEY_NAME("\xA1", "Right Shift")
    KEY_NAME("\x11", "Ctrl")
    KEY_NAME("\xA2", "Left Ctrl")
    KEY_NAME("\xA3", "Right Ctrl")
    KEY_NAME("\x12", "Alt")
    KEY_NAME("\xA4", "Left Alt")
    KEY_NAME("\xA5", "Right Alt")
    KEY_NAME("\x01", "Left mouse")
    KEY_NAME("\x02", "Right mouse")
    KEY_NAME("\x04", "Middle mouse")
    KEY_NAME("\x05", "Mouse 4")
    KEY_NAME("\x06", "Mouse 5");
#undef KEY_NAME
Utf8View named_key(u8 key) noexcept {
    if (key == key::left_super)
        return MAICORD_OS_TEXT("Left Windows", "Left Super");
    if (key == key::right_super)
        return MAICORD_OS_TEXT("Right Windows", "Right Super");
    for (const char* at = kKeyNames; *at;) {
        const u32 bytes = c_string_length(at + 1);
        if (static_cast<u8>(*at) == key)
            return {at + 1, bytes};
        at += bytes + 2;
    }
    return {};
}

// Word-prefix matching, ASCII case-insensitive (the settings search rule).
bool word_byte(char value) noexcept {
    return ascii_is_alnum(value) ||
           (static_cast<u8>(value) & 0x80) || value == '/' || value == ',' || value == '\\';
}
bool word_prefixes(Utf8View word, Utf8View text) noexcept {
    for (u32 i = 0; i < text.bytes;) {
        while (i < text.bytes && !word_byte(text.data[i]))
            ++i;
        u32 j = 0;
        while (j < word.bytes && i + j < text.bytes && ascii_lower(text.data[i + j]) == ascii_lower(word.data[j]))
            ++j;
        if (j == word.bytes && word.bytes)
            return true;
        while (i < text.bytes && word_byte(text.data[i]))
            ++i;
    }
    return false;
}
// "+" separates the words of chord text ("Ctrl+K"); key symbols are words.
bool query_matches(Utf8View query, const Utf8View* texts, u32 count) noexcept {
    u32 words = 0;
    for (u32 i = 0; i < query.bytes;) {
        while (i < query.bytes && query.data[i] == ' ')
            ++i;
        const u32 start = i;
        while (i < query.bytes && query.data[i] != ' ')
            ++i;
        if (i == start)
            continue;
        ++words;
        const Utf8View word{query.data + start, i - start};
        bool found = false;
        for (u32 t = 0; t < count && !found; ++t)
            found = word_prefixes(word, texts[t]);
        if (!found)
            return false;
    }
    return words != 0;
}

static constexpr u8 kMagic[8]{'M', 'A', 'I', 'K', 'E', 'Y', 'S', '1'};
constexpr u32 kChecksumOffset = 24;
u64 checksum(const ConstSlice<u8>& record) noexcept {
    const u64 hash = hash64_bytes(kHash64Seed, record.data, kChecksumOffset);
    return hash64_bytes(hash, record.data + kCustomKeybindsHeaderBytes, record.count - kCustomKeybindsHeaderBytes);
}
bool standalone_key(u8 key) noexcept {
    return (key >= key::f1 && key <= key::f24) || key == key::pause || key == key::scroll_lock;
}
// Chords every text field or Windows itself relies on; a global keybind there
// would break typing or system commands.
bool system_chord(ShortcutChord chord) noexcept {
    const u8 key = chord.key, modifiers = chord.modifiers;
    const bool alt = (modifiers & shortcut_alt) != 0, ctrl = (modifiers & shortcut_ctrl) != 0;
    if (alt && !ctrl && (key == key::f4 || key == key::tab || key == key::space || key == key::escape))
        return true;
    if (ctrl && !alt && (key == 'A' || key == 'C' || key == 'V' || key == 'X' || key == 'Z' || key == 'Y'))
        return true;
    const bool editing = key == key::left || key == key::right || key == key::up || key == key::down || key == key::home ||
                         key == key::end || key == key::page_up || key == key::page_down || key == key::backspace || key == key::del ||
                         key == key::insert || key == key::tab || key == key::enter || key == key::escape || key == key::space;
    return editing && !alt;
}
bool user_path(wchar_t* path, bool create) noexcept {
    return data_path(path, kAccountProfileMaximumPathCharacters, L"keybinds.bin", create).ok();
}
} // namespace

static_assert(static_cast<u8>(shortcut_ctrl) == mod_ctrl && static_cast<u8>(shortcut_alt) == mod_alt &&
              static_cast<u8>(shortcut_shift) == mod_shift);

ShortcutChord shortcut_chord_from_keyboard(u32 key) noexcept {
    const auto modifiers = static_cast<u8>(window_modifiers(nullptr) & (mod_ctrl | mod_alt | mod_shift));
    return {key <= 0xff ? static_cast<u8>(key) : u8{0}, modifiers};
}

const ShortcutDefinition* shortcut_definition(ShortcutAction action) noexcept {
    for (const auto& definition : kDefinitions)
        if (definition.action == action)
            return &definition;
    return nullptr;
}
Utf8View shortcut_group_label(ShortcutGroup group) noexcept {
    return group < ShortcutGroup::count ? name_list_at(kGroupLabels, static_cast<u32>(group)) : Utf8View{};
}

bool shortcut_scope_active(ShortcutScope scope, ShortcutContext context) noexcept {
    switch (scope) {
    case ShortcutScope::global:
        return true;
    case ShortcutScope::content:
        return context.focus != ShortcutFocus::text;
    case ShortcutScope::empty_composer:
        return context.focus == ShortcutFocus::composer && context.composer_empty;
    case ShortcutScope::message:
        return context.focus == ShortcutFocus::message;
    }
    return false;
}

u32 shortcut_resolve(ShortcutContext context, ShortcutChord chord, Slice<ShortcutAction> output) noexcept {
    static constexpr ShortcutScope order[]{ShortcutScope::message, ShortcutScope::empty_composer, ShortcutScope::content,
                                    ShortcutScope::global};
    u32 count = 0;
    if (!chord.key || !output.data)
        return 0;
    for (const auto scope : order) {
        if (!shortcut_scope_active(scope, context))
            continue;
        for (const auto& definition : kDefinitions)
            if (definition.scope == scope && binds(definition, chord) && count < output.count)
                output[count++] = definition.action;
    }
    return count;
}
ShortcutAction shortcut_first(ShortcutContext context, ShortcutChord chord) noexcept {
    ShortcutAction action = ShortcutAction::none;
    shortcut_resolve(context, chord, {&action, 1});
    return action;
}
bool shortcut_matches(ShortcutAction action, ShortcutChord chord) noexcept {
    const auto* definition = shortcut_definition(action);
    return definition && binds(*definition, chord);
}
const ShortcutDefinition* shortcut_built_in(ShortcutChord chord) noexcept {
    for (const auto& definition : kDefinitions)
        if (binds(definition, chord))
            return &definition;
    return nullptr;
}

u32 shortcut_key_label(u8 key, const Slice<char>& output) noexcept {
    if (!key || !output.data)
        return 0;
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
        return put_char(output, 0, static_cast<char>(key));
    if (key >= key::f1 && key <= key::f24)
        return put_decimal(output, put_char(output, 0, 'F'), key - key::f1 + 1u);
    if (key >= key::numpad0 && key <= key::numpad9)
        return put_char(output, put(output, 0, utf8_literal("Num ")), static_cast<char>('0' + key - key::numpad0));
    const Utf8View name = named_key(key);
    if (name.bytes)
        return put(output, 0, name);
    static constexpr char hex[] = "0123456789ABCDEF";
    u32 at = put(output, 0, utf8_literal("Key 0x"));
    at = put_char(output, at, hex[key >> 4]);
    return put_char(output, at, hex[key & 15]);
}
Utf8View shortcut_modifier_label(u8 modifier) noexcept {
    switch (modifier) {
    case shortcut_ctrl: return utf8_literal("Ctrl");
    case shortcut_alt: return utf8_literal("Alt");
    case shortcut_shift: return utf8_literal("Shift");
    default: return {};
    }
}
u32 shortcut_chord_label(ShortcutChord chord, Slice<char> output) noexcept {
    if (!chord.key || !output.data)
        return 0;
    u32 at = 0;
    // Ctrl, Alt, Shift: the bits in display order.
    for (u8 modifier = shortcut_ctrl; modifier <= shortcut_shift; modifier <<= 1)
        if (chord.modifiers & modifier)
            at = put_char(output, put(output, at, shortcut_modifier_label(modifier)), '+');
    char key[24]{};
    const u32 bytes = shortcut_key_label(chord.key, {key, sizeof(key)});
    return put(output, at, {key, bytes});
}

u32 shortcut_listing(Utf8View query, Slice<ShortcutListingRow> output) noexcept {
    if (!output.data)
        return 0;
    bool filtered = false;
    for (u32 i = 0; i < query.bytes && !filtered; ++i)
        filtered = query.data[i] != ' ';
    u32 count = 0;
    ShortcutGroup headed = ShortcutGroup::count;
    for (u32 i = 0; i < kDefinitionCount && count < output.count; ++i) {
        const auto& definition = kDefinitions[i];
        const Utf8View label = name_list_at(kLabels, i);
        ShortcutListingRow row{definition.group, false, {definition.chords[0], definition.chords[1]}, label};
        // Previous/next pairs share their label and one row.
        if (i + 1 < kDefinitionCount && !definition.chords[1].key &&
            utf8_equal(name_list_at(kLabels, i + 1), label)) {
            row.chords[1] = kDefinitions[i + 1].chords[0];
            ++i;
        }
        if (filtered) {
            char first[48]{}, second[48]{};
            const Utf8View texts[]{row.label, shortcut_group_label(row.group),
                                   {first, shortcut_chord_label(row.chords[0], {first, sizeof(first)})},
                                   {second, shortcut_chord_label(row.chords[1], {second, sizeof(second)})}};
            if (!query_matches(query, texts, 4))
                continue;
        }
        if (headed != row.group) {
            if (count + 2 > output.count)
                break;
            headed = row.group;
            output[count++] = {row.group, true, {}, shortcut_group_label(row.group)};
        }
        output[count++] = row;
    }
    return count;
}

void shortcut_recorder_start(ShortcutRecorder* recorder, ShortcutRecordMode mode) noexcept {
    if (recorder)
        *recorder = {mode, true};
}
bool shortcut_key_is_modifier(u32 key) noexcept {
    return key == key::shift || key == key::ctrl || key == key::alt || key == key::left_shift || key == key::right_shift ||
           key == key::left_ctrl || key == key::right_ctrl || key == key::left_alt || key == key::right_alt || key == key::left_super ||
           key == key::right_super;
}
ShortcutRecordStep shortcut_recorder_input(ShortcutRecorder* recorder, bool down, u32 key, u8 modifiers,
                                           ShortcutChord* recorded) noexcept {
    if (!recorder->active)
        return ShortcutRecordStep::ignored;
    if (!down || !key || key > 0xfe)
        return ShortcutRecordStep::pending;
    modifiers &= shortcut_modifier_mask;
    if (!modifiers && key == key::escape) {
        recorder->active = false;
        return ShortcutRecordStep::cancelled;
    }
    if (!modifiers && (key == key::backspace || key == key::del)) {
        recorder->active = false;
        return ShortcutRecordStep::cleared;
    }
    if (recorder->mode == ShortcutRecordMode::chord && shortcut_key_is_modifier(key))
        return ShortcutRecordStep::pending;
    if (recorded)
        *recorded = {static_cast<u8>(key), recorder->mode == ShortcutRecordMode::chord ? modifiers : u8{0}};
    recorder->active = false;
    return ShortcutRecordStep::recorded;
}

static_assert(name_list_count(kCustomKeybindActionLabels) == static_cast<u32>(CustomKeybindAction::count));
Utf8View custom_keybind_action_label(CustomKeybindAction action) noexcept {
    return name_list_at(kCustomKeybindActionLabels, static_cast<u32>(action));
}
Utf8View custom_keybind_error_text(CustomKeybindError error) noexcept {
    switch (error) {
    case CustomKeybindError::invalid: return utf8_literal("That key can't be used for a keybind.");
    case CustomKeybindError::needs_modifier:
        return utf8_literal("Add Ctrl or Alt, or use a function key, so typing doesn't trigger it.");
    case CustomKeybindError::reserved: return utf8_literal("A built-in shortcut already uses those keys.");
    case CustomKeybindError::system: return MAICORD_OS_TEXT("Typing or Windows already uses those keys.",
                                                            "Typing or your desktop already uses those keys.");
    case CustomKeybindError::duplicate: return utf8_literal("Another keybind already uses those keys.");
    case CustomKeybindError::full: return utf8_literal("You've reached the keybind limit.");
    default: return {};
    }
}
CustomKeybindError custom_keybind_check(const CustomKeybinds& keybinds, CustomKeybind keybind) noexcept {
    if (keybind.action == CustomKeybindAction::push_to_talk || keybind.action >= CustomKeybindAction::count)
        return CustomKeybindError::invalid;
    const auto chord = keybind.chord;
    if (!chord.key || chord.key == 0xff || (chord.modifiers & ~shortcut_modifier_mask) ||
        shortcut_key_is_modifier(chord.key) || chord.key < key::backspace)
        return CustomKeybindError::invalid;
    if (!(chord.modifiers & (shortcut_ctrl | shortcut_alt)) && !standalone_key(chord.key))
        return CustomKeybindError::needs_modifier;
    if (shortcut_built_in(chord))
        return CustomKeybindError::reserved;
    if (system_chord(chord))
        return CustomKeybindError::system;
    if (custom_keybinds_find(keybinds, chord) < kCustomKeybindMaximum)
        return CustomKeybindError::duplicate;
    if (keybinds.count >= kCustomKeybindMaximum)
        return CustomKeybindError::full;
    return CustomKeybindError::none;
}
bool custom_keybinds_add(CustomKeybinds* keybinds, CustomKeybind keybind, CustomKeybindError* error) noexcept {
    const auto result = keybinds ? custom_keybind_check(*keybinds, keybind) : CustomKeybindError::invalid;
    if (error)
        *error = result;
    if (result != CustomKeybindError::none)
        return false;
    keybinds->items[keybinds->count++] = keybind;
    return true;
}
bool custom_keybinds_remove(CustomKeybinds* keybinds, u32 index) noexcept {
    if (index >= keybinds->count || keybinds->count > kCustomKeybindMaximum)
        return false;
    for (u32 i = index + 1; i < keybinds->count; ++i)
        keybinds->items[i - 1] = keybinds->items[i];
    reset_value(&keybinds->items[--keybinds->count]);
    return true;
}
u32 custom_keybinds_find(const CustomKeybinds& keybinds, ShortcutChord chord) noexcept {
    for (u32 i = 0; i < keybinds.count && i < kCustomKeybindMaximum; ++i)
        if (shortcut_chord_equal(keybinds.items[i].chord, chord))
            return i;
    return kCustomKeybindMaximum;
}
bool custom_keybinds_valid(const CustomKeybinds& keybinds) noexcept {
    if (keybinds.count > kCustomKeybindMaximum)
        return false;
    CustomKeybinds rebuilt{};
    for (u32 i = 0; i < keybinds.count; ++i)
        if (!custom_keybinds_add(&rebuilt, keybinds.items[i]))
            return false;
    return true;
}

u32 custom_keybinds_encode(const CustomKeybinds& keybinds, Slice<u8> output) noexcept {
    const u32 bytes = kCustomKeybindsHeaderBytes + keybinds.count * kCustomKeybindBytes;
    if (!output.data || !custom_keybinds_valid(keybinds) || output.count < bytes)
        return 0;
    clear_bytes(output.data, bytes);
    u8* r = output.data;
    copy_bytes(r, kMagic, 8);
    byte_store_le32(r + 8, kCustomKeybindsVersion);
    byte_store_le32(r + 12, bytes);
    byte_store_le16(r + 16, static_cast<u16>(keybinds.count));
    for (u32 i = 0; i < keybinds.count; ++i) {
        u8* entry = r + kCustomKeybindsHeaderBytes + i * kCustomKeybindBytes;
        entry[0] = static_cast<u8>(keybinds.items[i].action);
        entry[1] = keybinds.items[i].chord.key;
        entry[2] = keybinds.items[i].chord.modifiers;
    }
    byte_store_le64(r + kChecksumOffset, checksum({r, bytes}));
    return bytes;
}
bool custom_keybinds_decode(ConstSlice<u8> record, CustomKeybinds* keybinds) noexcept {
    const u8* r = record.data;
    if (!keybinds || !r || record.count < kCustomKeybindsHeaderBytes || record.count > kCustomKeybindsMaximumBytes ||
        !bytes_equal(r, kMagic, 8) || byte_load_le32(r + 8) != kCustomKeybindsVersion ||
        byte_load_le32(r + 12) != record.count || byte_load_le64(r + kChecksumOffset) != checksum(record))
        return false;
    const u32 count = byte_load_le16(r + 16);
    if (count > kCustomKeybindMaximum || kCustomKeybindsHeaderBytes + count * kCustomKeybindBytes != record.count)
        return false;
    for (u32 i = 18; i < kChecksumOffset; ++i)
        if (r[i])
            return false;
    CustomKeybinds decoded{};
    for (u32 i = 0; i < count; ++i) {
        const u8* entry = r + kCustomKeybindsHeaderBytes + i * kCustomKeybindBytes;
        if (entry[3] || entry[0] >= static_cast<u8>(CustomKeybindAction::count) ||
            !custom_keybinds_add(&decoded, {static_cast<CustomKeybindAction>(entry[0]), {entry[1], entry[2]}}))
            return false;
    }
    *keybinds = decoded;
    return true;
}
bool custom_keybinds_load_file(const wchar_t* path, CustomKeybinds* keybinds) noexcept {
    if (!path || !keybinds)
        return false;
    u8 record[kCustomKeybindsMaximumBytes]{};
    const FileReadResult read = file_persistence_read_file(path, {record, sizeof(record)});
    return read.ok() && custom_keybinds_decode({record, read.bytes}, keybinds);
}
bool custom_keybinds_save_file(const wchar_t* path, const CustomKeybinds& keybinds) noexcept {
    u8 record[kCustomKeybindsMaximumBytes];
    const u32 bytes = custom_keybinds_encode(keybinds, {record, sizeof(record)});
    const u32 length = path ? wide_length(path, kAccountProfileMaximumPathCharacters) : 0;
    return bytes && length && file_persistence_write_atomic(path, length, {record, bytes}).ok();
}
bool custom_keybinds_load(CustomKeybinds* keybinds) noexcept {
    wchar_t path[kAccountProfileMaximumPathCharacters]{};
    return user_path(path, false) && custom_keybinds_load_file(path, keybinds);
}
bool custom_keybinds_save(const CustomKeybinds& keybinds) noexcept {
    wchar_t path[kAccountProfileMaximumPathCharacters]{};
    return user_path(path, true) && custom_keybinds_save_file(path, keybinds);
}

} // namespace maicord
