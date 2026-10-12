#include "maicord/app/shortcut_registry.hpp"
#include "maicord/app/account_profile.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"

#include <Windows.h>

namespace maicord {
namespace {
constexpr u8 kModifierOrder[]{shortcut_ctrl, shortcut_alt, shortcut_shift};
using A = ShortcutAction;
using G = ShortcutGroup;
using S = ShortcutScope;
constexpr u8 C = shortcut_ctrl, L = shortcut_alt, H = shortcut_shift;

constexpr ShortcutDefinition kDefinitions[]{
    {A::server_previous, G::navigation, S::global, utf8_literal("Navigate between servers"), {{VK_UP, C | L}, {}}},
    {A::server_next, G::navigation, S::global, utf8_literal("Navigate between servers"), {{VK_DOWN, C | L}, {}}},
    {A::channel_previous, G::navigation, S::global, utf8_literal("Navigate between channels"), {{VK_UP, L}, {}}},
    {A::channel_next, G::navigation, S::global, utf8_literal("Navigate between channels"), {{VK_DOWN, L}, {}}},
    {A::unread_previous, G::navigation, S::global, utf8_literal("Navigate between unread channels"),
     {{VK_UP, L | H}, {}}},
    {A::unread_next, G::navigation, S::global, utf8_literal("Navigate between unread channels"),
     {{VK_DOWN, L | H}, {}}},
    {A::mention_previous, G::navigation, S::global, utf8_literal("Navigate between unread mentions"),
     {{VK_UP, C | L | H}, {}}},
    {A::mention_next, G::navigation, S::global, utf8_literal("Navigate between unread mentions"),
     {{VK_DOWN, C | L | H}, {}}},
    {A::quick_switcher, G::navigation, S::global, utf8_literal("Toggle quick switcher"), {{'K', C}, {}}},
    {A::mark_server_read, G::chat, S::content, utf8_literal("Mark server read"), {{VK_ESCAPE, H}, {}}},
    {A::mark_channel_read, G::chat, S::content, utf8_literal("Mark channel read"), {{VK_ESCAPE, 0}, {}}},
    {A::toggle_pins, G::chat, S::global, utf8_literal("Toggle pinned messages"), {{'P', C}, {}}},
    {A::toggle_inbox, G::chat, S::global, utf8_literal("Toggle inbox"), {{'I', C}, {}}},
    {A::toggle_members, G::chat, S::global, utf8_literal("Toggle member list"), {{'U', C}, {}}},
    {A::emoji_picker, G::chat, S::global, utf8_literal("Toggle emoji picker"), {{'E', C}, {}}},
    {A::gif_picker, G::chat, S::global, utf8_literal("Toggle GIF picker"), {{'G', C}, {}}},
    {A::sticker_picker, G::chat, S::global, utf8_literal("Toggle sticker picker"), {{'S', C}, {}}},
    {A::upload_file, G::chat, S::global, utf8_literal("Upload a file"), {{'U', C | H}, {}}},
    {A::edit_last_message, G::chat, S::empty_composer, utf8_literal("Edit your last message"), {{VK_UP, 0}, {}}},
    {A::message_reply, G::messages, S::message, utf8_literal("Reply"), {{'R', 0}, {}}},
    {A::message_edit, G::messages, S::message, utf8_literal("Edit message"), {{'E', 0}, {}}},
    {A::message_forward, G::messages, S::message, utf8_literal("Forward message"), {{'F', 0}, {}}},
    {A::message_react, G::messages, S::message, utf8_literal("Add reaction"), {{VK_OEM_PLUS, 0}, {VK_ADD, 0}}},
    {A::save_attachment, G::messages, S::message, utf8_literal("Save attachment"), {{'S', C}, {}}},
    {A::toggle_mute, G::voice, S::global, utf8_literal("Toggle mute"), {{'M', C | H}, {}}},
    {A::toggle_deafen, G::voice, S::global, utf8_literal("Toggle deafen"), {{'D', C | H}, {}}},
    {A::search, G::misc, S::global, utf8_literal("Search"), {{'F', C}, {'F', C | H}}},
    {A::shortcuts_help, G::misc, S::global, utf8_literal("Show keyboard shortcuts"), {{VK_OEM_2, C}, {}}},
    {A::open_settings, G::misc, S::global, utf8_literal("Open user settings"), {{VK_OEM_COMMA, C}, {}}},
    {A::downloads, G::misc, S::global, utf8_literal("Show downloads"), {{'J', C}, {}}},
};
constexpr u32 kDefinitionCount = sizeof(kDefinitions) / sizeof(kDefinitions[0]);
static_assert(kDefinitionCount == static_cast<u32>(ShortcutAction::count) - 1);

constexpr Utf8View kGroupLabels[]{utf8_literal("Navigation"), utf8_literal("Chat"), utf8_literal("Messages"),
                                  utf8_literal("Voice"), utf8_literal("Miscellaneous")};
static_assert(sizeof(kGroupLabels) / sizeof(kGroupLabels[0]) == static_cast<u32>(ShortcutGroup::count));

constexpr bool binds(const ShortcutDefinition& definition, ShortcutChord chord) noexcept {
    return shortcut_chord_equal(definition.chords[0], chord) || shortcut_chord_equal(definition.chords[1], chord);
}
// Definitions follow action order with a label, every chord is bound once per
// scope and groups stay contiguous so listings show each header once.
constexpr bool definitions_valid() noexcept {
    for (u32 i = 0; i < kDefinitionCount; ++i) {
        const auto& a = kDefinitions[i];
        if (a.action != static_cast<ShortcutAction>(i + 1) || !a.label.bytes || !a.chords[0].key ||
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
u32 put(Slice<char> output, u32 at, Utf8View text) noexcept {
    if (!output.data || at + text.bytes > output.count)
        return at;
    copy_bytes(output.data + at, text.data, text.bytes);
    return at + text.bytes;
}
u32 put_char(Slice<char> output, u32 at, char value) noexcept {
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
Utf8View named_key(u8 key) noexcept {
    switch (key) {
    case VK_UP: return utf8_literal("\xE2\x86\x91");
    case VK_DOWN: return utf8_literal("\xE2\x86\x93");
    case VK_LEFT: return utf8_literal("\xE2\x86\x90");
    case VK_RIGHT: return utf8_literal("\xE2\x86\x92");
    case VK_ESCAPE: return utf8_literal("Esc");
    case VK_RETURN: return utf8_literal("Enter");
    case VK_TAB: return utf8_literal("Tab");
    case VK_SPACE: return utf8_literal("Space");
    case VK_BACK: return utf8_literal("Backspace");
    case VK_DELETE: return utf8_literal("Delete");
    case VK_INSERT: return utf8_literal("Insert");
    case VK_HOME: return utf8_literal("Home");
    case VK_END: return utf8_literal("End");
    case VK_PRIOR: return utf8_literal("Page Up");
    case VK_NEXT: return utf8_literal("Page Down");
    case VK_PAUSE: return utf8_literal("Pause");
    case VK_SCROLL: return utf8_literal("Scroll Lock");
    case VK_CAPITAL: return utf8_literal("Caps Lock");
    case VK_NUMLOCK: return utf8_literal("Num Lock");
    case VK_SNAPSHOT: return utf8_literal("Print Screen");
    case VK_APPS: return utf8_literal("Menu");
    case VK_MULTIPLY: return utf8_literal("Num *");
    case VK_ADD: return utf8_literal("Num +");
    case VK_SUBTRACT: return utf8_literal("Num -");
    case VK_DECIMAL: return utf8_literal("Num .");
    case VK_DIVIDE: return utf8_literal("Num /");
    case VK_OEM_1: return utf8_literal(";");
    case VK_OEM_PLUS: return utf8_literal("+");
    case VK_OEM_COMMA: return utf8_literal(",");
    case VK_OEM_MINUS: return utf8_literal("-");
    case VK_OEM_PERIOD: return utf8_literal(".");
    case VK_OEM_2: return utf8_literal("/");
    case VK_OEM_3: return utf8_literal("`");
    case VK_OEM_4: return utf8_literal("[");
    case VK_OEM_5: return utf8_literal("\\");
    case VK_OEM_6: return utf8_literal("]");
    case VK_OEM_7: return utf8_literal("'");
    case VK_SHIFT: return utf8_literal("Shift");
    case VK_LSHIFT: return utf8_literal("Left Shift");
    case VK_RSHIFT: return utf8_literal("Right Shift");
    case VK_CONTROL: return utf8_literal("Ctrl");
    case VK_LCONTROL: return utf8_literal("Left Ctrl");
    case VK_RCONTROL: return utf8_literal("Right Ctrl");
    case VK_MENU: return utf8_literal("Alt");
    case VK_LMENU: return utf8_literal("Left Alt");
    case VK_RMENU: return utf8_literal("Right Alt");
    case VK_LWIN: return utf8_literal("Left Windows");
    case VK_RWIN: return utf8_literal("Right Windows");
    case VK_LBUTTON: return utf8_literal("Left mouse");
    case VK_RBUTTON: return utf8_literal("Right mouse");
    case VK_MBUTTON: return utf8_literal("Middle mouse");
    case VK_XBUTTON1: return utf8_literal("Mouse 4");
    case VK_XBUTTON2: return utf8_literal("Mouse 5");
    default: return {};
    }
}

// Word-prefix matching, ASCII case-insensitive (the settings search rule).
bool word_byte(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
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

void store16(u8* output, u16 value) noexcept {
    output[0] = static_cast<u8>(value);
    output[1] = static_cast<u8>(value >> 8);
}
constexpr u8 kMagic[8]{'M', 'A', 'I', 'K', 'E', 'Y', 'S', '1'};
constexpr u32 kChecksumOffset = 24;
u64 checksum(ConstSlice<u8> record) noexcept {
    const u64 hash = hash64_bytes(kHash64Seed, record.data, kChecksumOffset);
    return hash64_bytes(hash, record.data + kCustomKeybindsHeaderBytes, record.count - kCustomKeybindsHeaderBytes);
}
bool standalone_key(u8 key) noexcept {
    return (key >= VK_F1 && key <= VK_F24) || key == VK_PAUSE || key == VK_SCROLL;
}
// Chords every text field or Windows itself relies on; a global keybind there
// would break typing or system commands.
bool system_chord(ShortcutChord chord) noexcept {
    const u8 key = chord.key, modifiers = chord.modifiers;
    const bool alt = (modifiers & shortcut_alt) != 0, ctrl = (modifiers & shortcut_ctrl) != 0;
    if (alt && !ctrl && (key == VK_F4 || key == VK_TAB || key == VK_SPACE || key == VK_ESCAPE))
        return true;
    if (ctrl && !alt && (key == 'A' || key == 'C' || key == 'V' || key == 'X' || key == 'Z' || key == 'Y'))
        return true;
    const bool editing = key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN || key == VK_HOME ||
                         key == VK_END || key == VK_PRIOR || key == VK_NEXT || key == VK_BACK || key == VK_DELETE ||
                         key == VK_INSERT || key == VK_TAB || key == VK_RETURN || key == VK_ESCAPE || key == VK_SPACE;
    return editing && !alt;
}
bool user_path(wchar_t* path, bool create) noexcept {
    return data_path(path, kAccountProfileMaximumPathCharacters, L"keybinds.bin", create).ok();
}
} // namespace

ShortcutChord shortcut_chord_from_keyboard(u32 key) noexcept {
    const u8 modifiers = static_cast<u8>(((GetKeyState(VK_CONTROL) & 0x8000) ? shortcut_ctrl : 0) |
                                         ((GetKeyState(VK_MENU) & 0x8000) ? shortcut_alt : 0) |
                                         ((GetKeyState(VK_SHIFT) & 0x8000) ? shortcut_shift : 0));
    return {key <= 0xff ? static_cast<u8>(key) : u8{0}, modifiers};
}

const ShortcutDefinition* shortcut_definition(ShortcutAction action) noexcept {
    for (const auto& definition : kDefinitions)
        if (definition.action == action)
            return &definition;
    return nullptr;
}
Utf8View shortcut_group_label(ShortcutGroup group) noexcept {
    return group < ShortcutGroup::count ? kGroupLabels[static_cast<u32>(group)] : Utf8View{};
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
    constexpr ShortcutScope order[]{ShortcutScope::message, ShortcutScope::empty_composer, ShortcutScope::content,
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

u32 shortcut_key_label(u8 key, Slice<char> output) noexcept {
    if (!key || !output.data)
        return 0;
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
        return put_char(output, 0, static_cast<char>(key));
    if (key >= VK_F1 && key <= VK_F24)
        return put_decimal(output, put_char(output, 0, 'F'), key - VK_F1 + 1u);
    if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
        return put_char(output, put(output, 0, utf8_literal("Num ")), static_cast<char>('0' + key - VK_NUMPAD0));
    const Utf8View name = named_key(key);
    if (name.bytes)
        return put(output, 0, name);
    constexpr char hex[] = "0123456789ABCDEF";
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
    for (const u8 modifier : kModifierOrder)
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
        ShortcutListingRow row{definition.group, false, definition.label, {definition.chords[0], definition.chords[1]}};
        // Previous/next pairs share their label and one row.
        if (i + 1 < kDefinitionCount && !definition.chords[1].key &&
            utf8_equal(kDefinitions[i + 1].label, definition.label)) {
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
            output[count++] = {row.group, true, shortcut_group_label(row.group), {}};
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
    return key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU || key == VK_LSHIFT || key == VK_RSHIFT ||
           key == VK_LCONTROL || key == VK_RCONTROL || key == VK_LMENU || key == VK_RMENU || key == VK_LWIN ||
           key == VK_RWIN;
}
ShortcutRecordStep shortcut_recorder_input(ShortcutRecorder* recorder, bool down, u32 key, u8 modifiers,
                                           ShortcutChord* recorded) noexcept {
    if (!recorder || !recorder->active)
        return ShortcutRecordStep::ignored;
    if (!down || !key || key > 0xfe)
        return ShortcutRecordStep::pending;
    modifiers &= shortcut_modifier_mask;
    if (!modifiers && key == VK_ESCAPE) {
        recorder->active = false;
        return ShortcutRecordStep::cancelled;
    }
    if (!modifiers && (key == VK_BACK || key == VK_DELETE)) {
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

Utf8View custom_keybind_action_label(CustomKeybindAction action) noexcept {
    switch (action) {
    case CustomKeybindAction::push_to_talk: return utf8_literal("Push to talk");
    case CustomKeybindAction::toggle_mute: return utf8_literal("Toggle mute");
    case CustomKeybindAction::toggle_deafen: return utf8_literal("Toggle deafen");
    default: return {};
    }
}
Utf8View custom_keybind_error_text(CustomKeybindError error) noexcept {
    switch (error) {
    case CustomKeybindError::invalid: return utf8_literal("That key can't be used for a keybind.");
    case CustomKeybindError::needs_modifier:
        return utf8_literal("Add Ctrl or Alt, or use a function key, so typing doesn't trigger it.");
    case CustomKeybindError::reserved: return utf8_literal("A built-in shortcut already uses those keys.");
    case CustomKeybindError::system: return utf8_literal("Typing or Windows already uses those keys.");
    case CustomKeybindError::duplicate: return utf8_literal("Another keybind already uses those keys.");
    case CustomKeybindError::full: return utf8_literal("You've reached the keybind limit.");
    default: return {};
    }
}
CustomKeybindError custom_keybind_check(const CustomKeybinds& keybinds, CustomKeybind keybind) noexcept {
    if (keybind.action != CustomKeybindAction::toggle_mute && keybind.action != CustomKeybindAction::toggle_deafen)
        return CustomKeybindError::invalid;
    const auto chord = keybind.chord;
    if (!chord.key || chord.key == 0xff || (chord.modifiers & ~shortcut_modifier_mask) ||
        shortcut_key_is_modifier(chord.key) || chord.key < VK_BACK)
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
Utf8View shortcut_plugin_chord_conflict(const CustomKeybinds& keybinds, ShortcutChord chord,
                                        u8 push_to_talk_key) noexcept {
    if (!chord.key || chord.key == 0xff || (chord.modifiers & ~shortcut_modifier_mask) ||
        shortcut_key_is_modifier(chord.key) || chord.key < VK_BACK || chord.key == VK_LWIN || chord.key == VK_RWIN ||
        chord.key == VK_APPS)
        return utf8_literal("a key that can't be used");
    if (!(chord.modifiers & (shortcut_ctrl | shortcut_alt)) && !(chord.key >= VK_F1 && chord.key <= VK_F24))
        return utf8_literal("a shortcut that needs Ctrl or Alt");
    if (const auto* built_in = shortcut_built_in(chord))
        return built_in->label;
    if (system_chord(chord) || (chord.key == VK_F10 && !(chord.modifiers & ~shortcut_shift)))
        return utf8_literal("a system shortcut");
    if (custom_keybinds_find(keybinds, chord) < kCustomKeybindMaximum)
        return utf8_literal("a custom keybind");
    if (push_to_talk_key && chord.key == push_to_talk_key)
        return custom_keybind_action_label(CustomKeybindAction::push_to_talk);
    return {};
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
    if (!keybinds || index >= keybinds->count || keybinds->count > kCustomKeybindMaximum)
        return false;
    for (u32 i = index + 1; i < keybinds->count; ++i)
        keybinds->items[i - 1] = keybinds->items[i];
    keybinds->items[--keybinds->count] = {};
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
    store16(r + 16, static_cast<u16>(keybinds.count));
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
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    u8 record[kCustomKeybindsMaximumBytes]{};
    LARGE_INTEGER size{};
    u32 native = 0;
    const bool read = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= sizeof(record) &&
                      file_persistence_read_complete(file, {record, static_cast<u32>(size.QuadPart)}, &native);
    CloseHandle(file);
    return read && custom_keybinds_decode({record, static_cast<u32>(size.QuadPart)}, keybinds);
}
bool custom_keybinds_save_file(const wchar_t* path, const CustomKeybinds& keybinds) noexcept {
    u8 record[kCustomKeybindsMaximumBytes]{};
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
