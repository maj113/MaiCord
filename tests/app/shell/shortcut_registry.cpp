#include "maicord/app/shell/shortcut_registry.hpp"
#include "maicord/app/shell/background_keybinds.hpp"
#include "maicord/app/shell/navigation_shortcuts.hpp"
#include "maicord/ui/shell/shortcuts_scene.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/platform/window/event.hpp"
#include "maicord/platform/window/key.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
constexpr u8 C = shortcut_ctrl, L = shortcut_alt, H = shortcut_shift;

constexpr bool single_bits() noexcept {
    constexpr u32 flags[] = {ev_repeat, ev_double_click, ev_system, ev_synthetic, ev_keyboard, ev_provisional};
    u32 seen = 0;
    for (const u32 flag : flags) {
        if (flag == 0 || (flag & (flag - 1)) != 0 || (seen & flag) != 0)
            return false;
        seen |= flag;
    }
    return seen <= 0xFF;
}
static_assert(single_bits());
static_assert(static_cast<u8>(PointerButton::left) == 0 && static_cast<u8>(PointerButton::forward) == 4);
static_assert(__is_trivially_copyable(WindowEvent) && __is_trivially_copyable(WindowEventResult));

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
    check(resolves(composer, {key::up, 0}, A::edit_last_message) && resolves(typed, {key::up, 0}) &&
              resolves(content, {key::up, 0}) && resolves(message, {key::up, 0}),
          "shortcut precedence: Up edits the last message only from an empty composer");
    check(resolves(content, {key::escape, 0}, A::mark_channel_read) &&
              resolves(composer, {key::escape, H}, A::mark_server_read) && resolves(text, {key::escape, 0}) &&
              resolves(text, {key::escape, H}),
          "shortcut precedence: Escape marks read outside other text fields");
    check(resolves(message, {'R', 0}, A::message_reply) && resolves(composer, {'R', 0}) &&
              resolves(content, {'E', 0}) && resolves(message, {'E', C}, A::emoji_picker) &&
              resolves(message, {key::add, 0}, A::message_react),
          "shortcut precedence: plain letters act only on a focused message, never while typing");
    check(resolves(text, {key::up, L}, A::channel_previous) && resolves(text, {'K', C}, A::quick_switcher) &&
              resolves(typed, {'M', C | H}, A::toggle_mute) && resolves(content, {key::slash, C}, A::shortcuts_help) &&
              resolves(content, {'K', C | L}) && shortcut_matches(A::search, {'F', C | H}) &&
              !shortcut_matches(A::search, {'F', C | L}),
          "shortcut precedence: global chords apply in every focus and need their exact modifiers");
    check(resolves(text, {key::tab, C}, A::tab_next) && resolves(composer, {key::page_down, C}, A::tab_next) &&
              resolves(content, {key::tab, C | H}, A::tab_previous) &&
              resolves(typed, {key::page_up, C}, A::tab_previous) && resolves(text, {'W', C}, A::tab_close) &&
              resolves(typed, {'T', C}, A::tab_new) && resolves(content, {'1', C}, A::tab_first) &&
              resolves(text, {'8', C}, A::tab_eighth) && resolves(message, {'9', C}, A::tab_last) &&
              resolves(content, {key::tab, 0}) && resolves(content, {'0', C}, A::zoom_reset) &&
              resolves(content, {'1', 0}) && shortcut_matches(A::tab_second, {'2', C}) &&
              !shortcut_matches(A::tab_second, {'2', C | L}),
          "shortcut registry: Ctrl+Tab, Ctrl+W, Ctrl+T and Ctrl+1..9 are the tab keys in every focus");
    check(label_is({key::tab, C | H}, utf8_literal("Ctrl+Shift+Tab")) &&
              label_is({key::page_down, C}, utf8_literal("Ctrl+Page Down")) && label_is({'9', C}, utf8_literal("Ctrl+9")),
          "shortcut labels: the tab keys read as Ctrl+Tab, Ctrl+Page Down and Ctrl+1..9");
    check(label_is({'M', C | H}, utf8_literal("Ctrl+Shift+M")) && label_is({key::slash, C}, utf8_literal("Ctrl+/")) &&
              label_is({key::up, L}, utf8_literal("Alt+\xE2\x86\x91")) && label_is({key::f12, 0}, utf8_literal("F12")) &&
              label_is({key::numpad5, C | L}, utf8_literal("Ctrl+Alt+Num 5")) &&
              label_is({0xE9, 0}, utf8_literal("Key 0xE9")) && label_is({}, {}),
          "shortcut labels: modifiers, arrows, function, numpad and unknown keys use the official wording");
}

void test_recorder(Check check) noexcept {
    ShortcutRecorder recorder{};
    ShortcutChord chord{};
    check(shortcut_recorder_input(&recorder, true, 'A', 0, &chord) == ShortcutRecordStep::ignored,
          "shortcut recorder: idle recorders ignore keys");
    shortcut_recorder_start(&recorder, ShortcutRecordMode::chord);
    const bool chord_ok = shortcut_recorder_input(&recorder, true, key::ctrl, C, &chord) == ShortcutRecordStep::pending &&
                          shortcut_recorder_input(&recorder, true, key::shift, C | H, &chord) == ShortcutRecordStep::pending &&
                          shortcut_recorder_input(&recorder, false, key::shift, C, &chord) == ShortcutRecordStep::pending &&
                          shortcut_recorder_input(&recorder, true, 'M', C | H | 0x80, &chord) == ShortcutRecordStep::recorded &&
                          chord.key == 'M' && chord.modifiers == (C | H) && !recorder.active;
    check(chord_ok, "shortcut recorder: chord mode waits past modifiers and records the combination");
    shortcut_recorder_start(&recorder, ShortcutRecordMode::chord);
    const bool cancel = shortcut_recorder_input(&recorder, true, key::escape, 0, &chord) == ShortcutRecordStep::cancelled &&
                        !recorder.active && chord.key == 'M';
    shortcut_recorder_start(&recorder, ShortcutRecordMode::chord);
    const bool escape_chord = shortcut_recorder_input(&recorder, true, key::escape, H, &chord) == ShortcutRecordStep::recorded &&
                              chord.key == key::escape && chord.modifiers == H;
    check(cancel && escape_chord, "shortcut recorder: Esc cancels, while Shift+Esc is a chord like any other");
    shortcut_recorder_start(&recorder, ShortcutRecordMode::single_key);
    const bool single = shortcut_recorder_input(&recorder, true, key::shift, H, &chord) == ShortcutRecordStep::recorded &&
                        chord.key == key::shift && chord.modifiers == 0;
    shortcut_recorder_start(&recorder, ShortcutRecordMode::single_key);
    const bool cleared = shortcut_recorder_input(&recorder, true, key::backspace, 0, &chord) == ShortcutRecordStep::cleared;
    shortcut_recorder_start(&recorder, ShortcutRecordMode::single_key);
    const bool mouse = shortcut_recorder_input(&recorder, true, key::xbutton1, 0, &chord) == ShortcutRecordStep::recorded &&
                       chord.key == key::xbutton1;
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
                       custom_keybinds_add(&keybinds, {K::toggle_deafen, {key::f9, 0}}) && keybinds.count == 2;
    check(added && rejects({K::toggle_deafen, {'M', C | L}}, E::duplicate) &&
              rejects({K::toggle_mute, {'M', C | H}}, E::reserved) && rejects({K::toggle_mute, {'M', H}}, E::needs_modifier) &&
              rejects({K::toggle_mute, {'V', C}}, E::system) && rejects({K::toggle_mute, {key::f4, L}}, E::system) &&
              rejects({K::toggle_mute, {key::left, C | H}}, E::system) &&
              custom_keybind_check(keybinds, {K::toggle_mute, {key::left, C | L}}) == E::none &&
              custom_keybind_check(keybinds, {K::toggle_streamer_mode, {'S', C | L}}) == E::none &&
              utf8_equal(custom_keybind_action_label(K::toggle_streamer_mode), utf8_literal("Toggle Streamer Mode")) &&
              rejects({K::toggle_mute, {'Q', 0}}, E::needs_modifier) && rejects({K::push_to_talk, {'Q', C}}, E::invalid) &&
              rejects({K::count, {'Q', C | L}}, E::invalid) &&
              rejects({K::toggle_mute, {key::shift, C}}, E::invalid) && rejects({K::toggle_mute, {}}, E::invalid) &&
              keybinds.count == 2,
          "custom keybinds: duplicates, built-in chords, typing keys and stored-elsewhere actions are refused");
    check(custom_keybinds_find(keybinds, {key::f9, 0}) == 1 && custom_keybinds_find(keybinds, {key::f9, C}) == kCustomKeybindMaximum,
          "custom keybinds: lookup matches exact chords");
    for (u32 key = key::f1; keybinds.count < kCustomKeybindMaximum; ++key)
        custom_keybinds_add(&keybinds, {K::toggle_mute, {static_cast<u8>(key), C}});
    check(rejects({K::toggle_mute, {key::f10, 0}}, E::full), "custom keybinds: the list holds eight keybinds");
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
              servers < all && rows[servers].chords[0].key == key::up && rows[servers].chords[1].key == key::down,
          "shortcut overlay listing: grouped rows, with previous/next pairs merged");
    // The Keybinds page lists the defaults in a bank of 48 rows (kKeybindsSettingsBankCapacity): none may be cut off.
    check(shortcut_listing({}, {rows, 48}) == all && shortcut_listing(utf8_literal("tab"), {rows, 64}) == 14,
          "shortcut overlay listing: the tab shortcuts fit the Keybinds page's bank and list under Navigation");
    const u32 mute = shortcut_listing(utf8_literal("MUTE"), {rows, 64});
    check(mute == 2 && rows[0].header && rows[0].group == ShortcutGroup::voice &&
              utf8_equal(rows[1].label, utf8_literal("Toggle mute")),
          "shortcut overlay listing: search matches labels and keeps the group heading");
    const u32 alt_shift = shortcut_listing(utf8_literal("alt shift"), {rows, 64});
    const bool quick = alt_shift == 3 && rows[1].chords[0].key == key::up && rows[1].chords[0].modifiers == (L | H) &&
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

// Background keybinds over a model of the main loop: one turn each `sample_ms`, the F9 key down from `from` to `to`
// (with Ctrl when `ctrl`), the window in the foreground from `front_from` to `front_to`. Returns how many times each
// of the two keybinds acted (F9 in the low byte, Ctrl+F9 in the next), with `samples` times one sample in each turn.
bool g_keys[256];
u32 background_run(u32 sample_ms, u32 from, u32 to, bool ctrl, u32 front_from, u32 front_to, u32 samples) noexcept {
    CustomKeybinds keybinds{};
    keybinds.count = 2;
    keybinds.items[0] = {CustomKeybindAction::toggle_mute, {key::f9, 0}};
    keybinds.items[1] = {CustomKeybindAction::toggle_deafen, {key::f9, C}};
    BackgroundKeybinds state{};
    u32 acted = 0;
    for (u32 now = 0; now <= 3'000; now += sample_ms) {
        g_keys[key::f9] = now >= from && now < to;
        g_keys[key::ctrl] = ctrl && g_keys[key::f9];
        const bool active = !(now >= front_from && now < front_to);
        for (u32 sample = 0; sample < samples; ++sample) {
            const u32 fired =
                background_keybinds_sample(&state, keybinds, 1, active, [](Key key) noexcept { return g_keys[key]; });
            acted += (fired & 1) + ((fired >> 1 & 1) << 8);
        }
    }
    g_keys[key::f9] = g_keys[key::ctrl] = false;
    return acted;
}
void test_background(Check check) noexcept {
    // The defect that the 20 ms loop turn fixes: with one sample for each account pump (about one a second in a
    // call), a press of 100 ms between two pumps did nothing.
    check(background_run(1'000, 110, 210, false, 0, 0, 1) == 0, "background keybinds: a sample each second loses a 100 ms press");
    check(background_run(20, 110, 210, false, 0, 0, 1) == 1, "background keybinds: a sample each 20 ms runs a 100 ms press one time");
    check(background_run(20, 110, 210, false, 0, 0, 3) == 1,
          "background keybinds: more samples of one press (a pump turn and a wake turn) run it one time");
    check(background_run(20, 110, 2'500, false, 0, 0, 1) == 1, "background keybinds: a held key runs one time, with no repeat");
    check(background_run(20, 110, 2'500, false, 500, 900, 1) == 1,
          "background keybinds: the window in the foreground during the press adds no action when it leaves again");
    check(background_run(20, 0, 400, false, 0, 0, 1) == 0, "background keybinds: a key down before the first sample never acts");
    check(background_run(20, 110, 210, true, 0, 0, 1) == 0x100, "background keybinds: the modifiers choose the chord");
    check(background_run(20, 110, 210, false, 0, 3'001, 1) == 0, "background keybinds: no sample acts when the conditions do not hold");
    BackgroundKeybinds state{};
    state.armed = true;
    const CustomKeybinds none{};
    check(background_keybinds_sample(&state, none, 1, false, [](Key) noexcept { return true; }) == 0 && !state.armed,
          "background keybinds: conditions that do not hold disarm the wake");
}
} // namespace

void test_shortcut_registry(Check check) noexcept {
    test_precedence(check);
    test_recorder(check);
    test_custom(check);
    test_listing(check);
    test_background(check);
}
} // namespace maicord
