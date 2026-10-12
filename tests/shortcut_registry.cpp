#include "maicord/app/shortcut_registry.hpp"
#include "maicord/app/navigation_shortcuts.hpp"
#include "maicord/ui/shortcuts_scene.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
constexpr u8 C = shortcut_ctrl, L = shortcut_alt, H = shortcut_shift;

bool resolves_list(ShortcutContext context, ShortcutChord chord, const ShortcutAction* expected, u32 expected_count) noexcept {
    ShortcutAction actions[4]{};
    const u32 count = shortcut_resolve(context, chord, {actions, 4});
    if (count != expected_count)
        return false;
    for (u32 i = 0; i < count; ++i)
        if (actions[i] != expected[i])
            return false;
    return true;
}
template <typename... Actions> bool resolves(ShortcutContext context, ShortcutChord chord, Actions... actions) noexcept {
    const ShortcutAction expected[]{ShortcutAction::none, actions...};
    return resolves_list(context, chord, expected + 1, sizeof...(Actions));
}
bool label_is(ShortcutChord chord, Utf8View expected) noexcept {
    char text[48]{};
    return utf8_equal({text, shortcut_chord_label(chord, {text, sizeof(text)})}, expected);
}

void test_precedence(Check check) noexcept {
    bool complete = true;
    for (u32 action = 1; action < static_cast<u32>(ShortcutAction::count); ++action)
        complete = complete && shortcut_definition(static_cast<ShortcutAction>(action));
    check(complete, "shortcut registry: every implemented action has a definition");
    const ShortcutContext content{ShortcutFocus::content, true}, message{ShortcutFocus::message, true},
        composer{ShortcutFocus::composer, true}, typed{ShortcutFocus::composer, false}, text{ShortcutFocus::text, true};
    using A = ShortcutAction;
    check(resolves(message, {'S', C}, A::save_attachment, A::sticker_picker) &&
              resolves(composer, {'S', C}, A::sticker_picker) && resolves(text, {'S', C}, A::sticker_picker),
          "shortcut precedence: Ctrl+S saves a focused message's attachment before the sticker picker");
    check(resolves(composer, {VK_UP, 0}, A::edit_last_message) && resolves(typed, {VK_UP, 0}) &&
              resolves(content, {VK_UP, 0}) && resolves(message, {VK_UP, 0}),
          "shortcut precedence: Up edits the last message only from an empty composer");
    check(resolves(content, {VK_ESCAPE, 0}, A::mark_channel_read) &&
              resolves(composer, {VK_ESCAPE, H}, A::mark_server_read) && resolves(text, {VK_ESCAPE, 0}) &&
              resolves(text, {VK_ESCAPE, H}),
          "shortcut precedence: Escape marks read outside other text fields");
    check(resolves(message, {'R', 0}, A::message_reply) && resolves(composer, {'R', 0}) &&
              resolves(content, {'E', 0}) && resolves(message, {'E', C}, A::emoji_picker) &&
              resolves(message, {VK_ADD, 0}, A::message_react),
          "shortcut precedence: plain letters act only on a focused message, never while typing");
    check(resolves(text, {VK_UP, L}, A::channel_previous) && resolves(text, {'K', C}, A::quick_switcher) &&
              resolves(typed, {'M', C | H}, A::toggle_mute) && resolves(content, {VK_OEM_2, C}, A::shortcuts_help) &&
              resolves(content, {'K', C | L}) && shortcut_matches(A::search, {'F', C | H}) &&
              !shortcut_matches(A::search, {'F', C | L}),
          "shortcut precedence: global chords apply in every focus and need their exact modifiers");
    check(navigation_shortcut(VK_UP, true, true, true).mode == NavigationMode::mention &&
              navigation_shortcut(VK_DOWN, true, true, false).mode == NavigationMode::guild &&
              navigation_shortcut('K', true, false, false).mode == NavigationMode::none,
          "shortcut registry: navigation chords come from the registry");
    check(label_is({'M', C | H}, utf8_literal("Ctrl+Shift+M")) && label_is({VK_OEM_2, C}, utf8_literal("Ctrl+/")) &&
              label_is({VK_UP, L}, utf8_literal("Alt+\xE2\x86\x91")) && label_is({VK_F12, 0}, utf8_literal("F12")) &&
              label_is({VK_NUMPAD5, C | L}, utf8_literal("Ctrl+Alt+Num 5")) &&
              label_is({0xE9, 0}, utf8_literal("Key 0xE9")) && label_is({}, {}),
          "shortcut labels: modifiers, arrows, function, numpad and unknown keys use the official wording");
}

void test_recorder(Check check) noexcept {
    ShortcutRecorder recorder{};
    ShortcutChord chord{};
    check(shortcut_recorder_input(&recorder, true, 'A', 0, &chord) == ShortcutRecordStep::ignored,
          "shortcut recorder: idle recorders ignore keys");
    shortcut_recorder_start(&recorder, ShortcutRecordMode::chord);
    const bool chord_ok = shortcut_recorder_input(&recorder, true, VK_CONTROL, C, &chord) == ShortcutRecordStep::pending &&
                          shortcut_recorder_input(&recorder, true, VK_SHIFT, C | H, &chord) == ShortcutRecordStep::pending &&
                          shortcut_recorder_input(&recorder, false, VK_SHIFT, C, &chord) == ShortcutRecordStep::pending &&
                          shortcut_recorder_input(&recorder, true, 'M', C | H | 0x80, &chord) == ShortcutRecordStep::recorded &&
                          chord.key == 'M' && chord.modifiers == (C | H) && !recorder.active;
    check(chord_ok, "shortcut recorder: chord mode waits past modifiers and records the combination");
    shortcut_recorder_start(&recorder, ShortcutRecordMode::chord);
    const bool cancel = shortcut_recorder_input(&recorder, true, VK_ESCAPE, 0, &chord) == ShortcutRecordStep::cancelled &&
                        !recorder.active && chord.key == 'M';
    shortcut_recorder_start(&recorder, ShortcutRecordMode::chord);
    const bool escape_chord = shortcut_recorder_input(&recorder, true, VK_ESCAPE, H, &chord) == ShortcutRecordStep::recorded &&
                              chord.key == VK_ESCAPE && chord.modifiers == H;
    check(cancel && escape_chord, "shortcut recorder: Esc cancels, while Shift+Esc is a chord like any other");
    shortcut_recorder_start(&recorder, ShortcutRecordMode::single_key);
    const bool single = shortcut_recorder_input(&recorder, true, VK_SHIFT, H, &chord) == ShortcutRecordStep::recorded &&
                        chord.key == VK_SHIFT && chord.modifiers == 0;
    shortcut_recorder_start(&recorder, ShortcutRecordMode::single_key);
    const bool cleared = shortcut_recorder_input(&recorder, true, VK_BACK, 0, &chord) == ShortcutRecordStep::cleared;
    shortcut_recorder_start(&recorder, ShortcutRecordMode::single_key);
    const bool mouse = shortcut_recorder_input(&recorder, true, VK_XBUTTON1, 0, &chord) == ShortcutRecordStep::recorded &&
                       chord.key == VK_XBUTTON1;
    check(single && cleared && mouse,
          "shortcut recorder: push-to-talk mode takes the first key or mouse button; Backspace clears");
}

void test_custom(Check check) noexcept {
    CustomKeybinds keybinds{};
    CustomKeybindError error{};
    using E = CustomKeybindError;
    using K = CustomKeybindAction;
    const auto rejects = [&](CustomKeybind keybind, E expected) noexcept {
        return !custom_keybinds_add(&keybinds, keybind, &error) && error == expected;
    };
    const bool added = custom_keybinds_add(&keybinds, {K::toggle_mute, {'M', C | L}}, &error) && error == E::none &&
                       custom_keybinds_add(&keybinds, {K::toggle_deafen, {VK_F9, 0}}) && keybinds.count == 2;
    check(added && rejects({K::toggle_deafen, {'M', C | L}}, E::duplicate) &&
              rejects({K::toggle_mute, {'M', C | H}}, E::reserved) && rejects({K::toggle_mute, {'M', H}}, E::needs_modifier) &&
              rejects({K::toggle_mute, {'V', C}}, E::system) && rejects({K::toggle_mute, {VK_F4, L}}, E::system) &&
              rejects({K::toggle_mute, {VK_LEFT, C | H}}, E::system) &&
              custom_keybind_check(keybinds, {K::toggle_mute, {VK_LEFT, C | L}}) == E::none &&
              rejects({K::toggle_mute, {'Q', 0}}, E::needs_modifier) && rejects({K::push_to_talk, {'Q', C}}, E::invalid) &&
              rejects({K::toggle_mute, {VK_SHIFT, C}}, E::invalid) && rejects({K::toggle_mute, {}}, E::invalid) &&
              keybinds.count == 2,
          "custom keybinds: duplicates, built-in chords, typing keys and stored-elsewhere actions are refused");
    check(custom_keybinds_find(keybinds, {VK_F9, 0}) == 1 && custom_keybinds_find(keybinds, {VK_F9, C}) == kCustomKeybindMaximum,
          "custom keybinds: lookup matches exact chords");
    const auto conflict = [&](ShortcutChord chord, Utf8View expected, u8 ptt = 0) noexcept {
        return utf8_equal(shortcut_plugin_chord_conflict(keybinds, chord, ptt), expected);
    };
    check(conflict({'M', C | H}, utf8_literal("Toggle mute")) && conflict({'C', C}, utf8_literal("a system shortcut")) &&
              conflict({VK_F4, L}, utf8_literal("a system shortcut")) &&
              conflict({VK_F10, 0}, utf8_literal("a system shortcut")) &&
              conflict({'M', C | L}, utf8_literal("a custom keybind")) &&
              conflict({VK_F9, 0}, utf8_literal("a custom keybind")) &&
              conflict({'K', H}, utf8_literal("a shortcut that needs Ctrl or Alt")) &&
              conflict({VK_PAUSE, 0}, utf8_literal("a shortcut that needs Ctrl or Alt")) &&
              conflict({VK_LWIN, C}, utf8_literal("a key that can't be used")) &&
              conflict({VK_CONTROL, C}, utf8_literal("a key that can't be used")) &&
              conflict({VK_F13, 0}, utf8_literal("Push to talk"), VK_F13) && conflict({VK_F13, 0}, {}) &&
              conflict({'K', C | H}, {}) && conflict({'P', C | L | H}, {}),
          "plugin keybinds: built-in, system, custom and push-to-talk chords are refused with a label; "
          "chords need Ctrl or Alt unless F1-F24");
    for (u32 key = VK_F1; keybinds.count < kCustomKeybindMaximum; ++key)
        custom_keybinds_add(&keybinds, {K::toggle_mute, {static_cast<u8>(key), C}});
    check(rejects({K::toggle_mute, {VK_F10, 0}}, E::full), "custom keybinds: the list holds eight keybinds");
    const CustomKeybind second = keybinds.items[2];
    check(custom_keybinds_remove(&keybinds, 1) && keybinds.count == kCustomKeybindMaximum - 1 &&
              keybinds.items[1].chord.key == second.chord.key && !custom_keybinds_remove(&keybinds, 9),
          "custom keybinds: removal keeps the order of the rest");

    static u8 record[kCustomKeybindsMaximumBytes];
    const u32 bytes = custom_keybinds_encode(keybinds, {record, sizeof(record)});
    CustomKeybinds decoded{};
    check(bytes == kCustomKeybindsHeaderBytes + keybinds.count * kCustomKeybindBytes &&
              custom_keybinds_decode({record, bytes}, &decoded) && bytes_equal(&decoded, &keybinds, sizeof(keybinds)),
          "custom keybinds persistence: the versioned record round-trips");
    decoded = {};
    decoded.count = 1;
    record[bytes - 2] ^= 1;
    const bool corrupt = !custom_keybinds_decode({record, bytes}, &decoded);
    record[bytes - 2] ^= 1;
    record[8] = 2; // Another format version is never migrated.
    const bool version = !custom_keybinds_decode({record, bytes}, &decoded);
    record[8] = 1;
    check(corrupt && version && !custom_keybinds_decode({record, bytes - 1}, &decoded) && decoded.count == 1 &&
              !custom_keybinds_decode({}, &decoded),
          "custom keybinds persistence: corrupt, other-version or truncated records leave the output untouched");
    CustomKeybinds invalid{};
    invalid.count = 1;
    invalid.items[0] = {K::toggle_mute, {'M', C | H}};
    check(!custom_keybinds_encode(invalid, {record, sizeof(record)}), "custom keybinds persistence: invalid sets are not written");

    wchar_t path[MAX_PATH + 32]{};
    const DWORD length = GetTempPathW(MAX_PATH, path);
    constexpr wchar_t name[] = L"maicord-keybinds-test.bin";
    for (u32 i = 0; length && i < sizeof(name) / sizeof(wchar_t); ++i)
        path[length + i] = name[i];
    CustomKeybinds loaded{};
    const bool file = length && custom_keybinds_save_file(path, keybinds) && custom_keybinds_load_file(path, &loaded) &&
                      bytes_equal(&loaded, &keybinds, sizeof(keybinds));
    DeleteFileW(path);
    check(file && !custom_keybinds_load_file(path, &loaded), "custom keybinds persistence: the file round-trips atomically");
}

void test_listing(Check check) noexcept {
    ShortcutListingRow rows[64]{};
    const u32 all = shortcut_listing({}, {rows, 64});
    u32 headers = 0, servers = kUiShortcutsMaximumRows;
    for (u32 i = 0; i < all; ++i) {
        headers += rows[i].header;
        if (utf8_equal(rows[i].label, utf8_literal("Navigate between servers")))
            servers = i;
    }
    check(all > 20 && headers == static_cast<u32>(ShortcutGroup::count) && rows[0].header &&
              servers < all && rows[servers].chords[0].key == VK_UP && rows[servers].chords[1].key == VK_DOWN,
          "shortcut overlay listing: grouped rows, with previous/next pairs merged");
    const u32 mute = shortcut_listing(utf8_literal("MUTE"), {rows, 64});
    check(mute == 2 && rows[0].header && rows[0].group == ShortcutGroup::voice &&
              utf8_equal(rows[1].label, utf8_literal("Toggle mute")),
          "shortcut overlay listing: search matches labels and keeps the group heading");
    const u32 alt_shift = shortcut_listing(utf8_literal("alt shift"), {rows, 64});
    const bool quick = alt_shift == 3 && rows[1].chords[0].key == VK_UP && rows[1].chords[0].modifiers == (L | H) &&
                       rows[2].chords[0].modifiers == (C | L | H);
    const u32 voice = shortcut_listing(utf8_literal("voice"), {rows, 64});
    check(quick && voice == 3 && !shortcut_listing(utf8_literal("zzz"), {rows, 64}) &&
              shortcut_listing(utf8_literal("  "), {rows, 64}) == all && shortcut_listing({}, {rows, 1}) == 0,
          "shortcut overlay listing: chord text and group names are searchable; headings never dangle");

    static UiShortcutsRow scene_rows[kUiShortcutsMaximumRows];
    const u32 listed = shortcut_listing({}, {rows, 64});
    for (u32 i = 0; i < listed; ++i) {
        scene_rows[i] = {};
        scene_rows[i].label = rows[i].label;
        scene_rows[i].header = rows[i].header;
        if (!rows[i].header) {
            scene_rows[i].keys[0][0] = utf8_literal("Ctrl");
            scene_rows[i].keys[0][1] = utf8_literal("K");
            scene_rows[i].key_counts[0] = 2;
        }
    }
    UiShortcutsContent content{utf8_literal("Keyboard shortcuts"), {}, utf8_literal("Search shortcuts"), {},
                               {scene_rows, listed}, 1};
    static UiShortcutsScene scene;
    UiRect first{}, last{};
    const bool wide = ui_shortcuts_scene_update(&scene, &content, {0, 0, 1280, 800}) && scene.columns == 2 &&
                      ui_shortcuts_scene_row(scene, 0, &first) && ui_rect_contains(scene.body, first.left + 1, first.top + 1) &&
                      ui_shortcuts_scene_hit_test(scene, 2, 2) == UiShortcutsTarget::close &&
                      ui_shortcuts_scene_hit_test(scene, scene.search.left + 4, scene.search.top + 4) ==
                          UiShortcutsTarget::search;
    bool right_column = false;
    for (u32 i = 0; i < listed; ++i)
        right_column = right_column || scene.rows[i].left > 0;
    const bool narrow = ui_shortcuts_scene_update(&scene, &content, {0, 0, 520, 640}) && scene.columns == 1 &&
                        scene.content_height > ui_rect_height(scene.body) &&
                        !ui_shortcuts_scene_row(scene, listed - 1, &last) &&
                        ui_shortcuts_scene_scroll(&scene, 1e6f) && ui_shortcuts_scene_row(scene, listed - 1, &last) &&
                        last.bottom <= scene.body.bottom + .5f && !ui_shortcuts_scene_scroll(&scene, 10);
    content.revision = 2;
    content.rows = {scene_rows, 0};
    const bool empty = ui_shortcuts_scene_update(&scene, &content, {0, 0, 520, 640}) && scene.row_count == 0 &&
                       scene.scroll == 0;
    scene_rows[1].key_counts[0] = 5;
    content.rows = {scene_rows, listed};
    content.revision = 3;
    const bool refused = !ui_shortcuts_scene_update(&scene, &content, {0, 0, 520, 640});
    scene_rows[1].key_counts[0] = 2;
    check(wide && right_column && narrow && empty && refused,
          "shortcut overlay scene: two balanced columns when wide, one scrolling column when narrow");
    ui_shortcuts_scene_release(&scene);
}
} // namespace

void test_shortcut_registry(Check check) noexcept {
    test_precedence(check);
    test_recorder(check);
    test_custom(check);
    test_listing(check);
}
} // namespace maicord
