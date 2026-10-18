#include "application_keybinds_settings.hpp"
#include "application_settings.hpp"
#include "application_state.hpp"
#include "application_voice.hpp"
#include "maicord/core/memory.hpp"

namespace maicord::app_detail {
namespace {
using Check = void (*)(bool, const char*) noexcept;
constexpr UiPageKey kKeybindsPage{kSettingsNativeOwner, static_cast<u64>(SettingsPanel::keybinds) + 1};
constexpr u8 C = shortcut_ctrl, L = shortcut_alt, H = shortcut_shift;

const UiSettingsPageContent* content() noexcept {
    const auto* pages = application_settings_pages();
    const auto* page = pages ? ui_page_registry_find(*pages, kKeybindsPage) : nullptr;
    return page ? static_cast<const UiSettingsPageContent*>(page->context) : nullptr;
}
const UiSettingsControl* control(u64 id) noexcept {
    const auto* page = content();
    for (u32 i = 0; page && i < page->controls.count; ++i)
        if (page->controls[i].id == id)
            return &page->controls[i];
    return nullptr;
}
const UiSettingsControl* labeled(Utf8View label) noexcept {
    const auto* page = content();
    for (u32 i = 0; page && i < page->controls.count; ++i)
        if (utf8_equal(page->controls[i].label, label))
            return &page->controls[i];
    return nullptr;
}
u64 custom(u32 index) noexcept {
    return settings_list_control_id(SettingBinding::keybinds_custom, index);
}
u64 node(SettingBinding binding) noexcept {
    UiSettingsSchemaLocation location{};
    return settings_catalog_binding(binding, &location) ? settings_control_id(location.panel, location.node) : 0;
}
bool press(u64 id, u32 key = VK_RETURN) noexcept {
    auto* scene = const_cast<UiSettingsScene*>(g_app.renderer.settings_scene);
    if (!scene || !ui_settings_scene_focus(scene, {UiSettingsTargetKind::control, kKeybindsPage, id}))
        return false;
    const bool consumed = application_settings_input(UiInputKind::key_down, key, false);
    application_settings_input(UiInputKind::key_up, key, false);
    return consumed && application_settings_synchronize();
}
// Keys typed into the recorder with thread-local modifier state; nothing is
// sent to the desktop session.
bool type(u32 key, u8 modifiers) noexcept {
    BYTE keys[256]{};
    keys[VK_CONTROL] = (modifiers & C) ? 0x80 : 0;
    keys[VK_MENU] = (modifiers & L) ? 0x80 : 0;
    keys[VK_SHIFT] = (modifiers & H) ? 0x80 : 0;
    if (!SetKeyboardState(keys))
        return false;
    const bool held = ((GetKeyState(VK_CONTROL) & 0x8000) != 0) == ((modifiers & C) != 0) &&
                      ((GetKeyState(VK_MENU) & 0x8000) != 0) == ((modifiers & L) != 0);
    const bool consumed = held && application_settings_input(UiInputKind::key_down, key, false) &&
                          application_settings_input(UiInputKind::key_up, key, false);
    BYTE clear[256]{};
    SetKeyboardState(clear);
    return consumed && application_settings_synchronize();
}
bool chip(const UiSettingsControl* row, Utf8View text) noexcept {
    return row && row->choices.count && utf8_equal(row->choices[0], text);
}
} // namespace

void test_application_keybinds_settings_runtime(Check check) noexcept {
    // keybinds.bin goes to a private LOCALAPPDATA for the duration of the test.
    wchar_t previous[1024]{}, root[MAX_PATH]{}, file[MAX_PATH + 64]{}, folder[MAX_PATH + 32]{};
    const DWORD previous_length = GetEnvironmentVariableW(L"LOCALAPPDATA", previous, 1024);
    const DWORD length = GetTempPathW(MAX_PATH, root);
    constexpr wchar_t leaf[] = L"maicord-keybinds-runtime";
    for (u32 i = 0; length && i < sizeof(leaf) / sizeof(wchar_t); ++i)
        root[length + i] = leaf[i];
    if (!length || (!CreateDirectoryW(root, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) ||
        !SetEnvironmentVariableW(L"LOCALAPPDATA", root)) {
        check(false, "keybinds settings: private LOCALAPPDATA fixture");
        return;
    }
    auto* pages = application_settings_pages();
    const bool opened = pages && ui_page_registry_select(pages, kKeybindsPage).ok() && application_settings_synchronize() &&
                        content() && content()->controls.count > 30;
    const auto* quick = labeled(utf8_literal("Toggle quick switcher"));
    const auto* servers = labeled(utf8_literal("Navigate between servers"));
    check(opened && control(node(SettingBinding::keybinds_add)) && labeled(utf8_literal("Custom Keybinds")) &&
              labeled(utf8_literal("Default Keybinds")) && labeled(utf8_literal("Navigation")) &&
              labeled(utf8_literal("Voice")) && chip(quick, utf8_literal("Ctrl+K")) && !quick->action_label.bytes &&
              servers && servers->choices.count == 2,
          "keybinds settings: the page lists custom keybinds and every default shortcut group");

    check(press(node(SettingBinding::keybinds_add)) && !control(node(SettingBinding::keybinds_add)) &&
              control(custom(kKeybindsDraftActionRow)) &&
              control(custom(kKeybindsDraftActionRow))->selected_choice ==
                  static_cast<u32>(CustomKeybindAction::toggle_mute) &&
              press(custom(kKeybindsDraftRecordRow)) && application_keybinds_settings_recording(),
          "keybinds settings: Add opens a draft with an action choice and the key recorder");
    const bool typed = type('M', C | L);
    const auto& keybinds = application_custom_keybinds();
    CustomKeybinds stored{};
    check(typed && keybinds.count == 1 && keybinds.items[0].action == CustomKeybindAction::toggle_mute &&
              keybinds.items[0].chord.key == 'M' && keybinds.items[0].chord.modifiers == (C | L) &&
              !application_keybinds_settings_recording() && chip(control(custom(0)), utf8_literal("Ctrl+Alt+M")) &&
              custom_keybinds_load(&stored) && stored.count == 1 && control(node(SettingBinding::keybinds_add)),
          "keybinds settings: a recorded chord becomes a saved keybind row");

    const bool reserved = press(node(SettingBinding::keybinds_add)) && press(custom(kKeybindsDraftRecordRow)) &&
                          type('M', C | H) && keybinds.count == 1 && control(custom(kKeybindsDraftRecordRow)) &&
                          utf8_equal(control(custom(kKeybindsDraftRecordRow))->description,
                                     custom_keybind_error_text(CustomKeybindError::reserved));
    check(reserved && press(custom(kKeybindsDraftCancelRow)) && !control(custom(kKeybindsDraftRecordRow)),
          "keybinds settings: built-in chords are refused in place and Cancel discards the draft");

    auto& voice = application_voice_preferences();
    voice.audio.push_to_talk_key = 0;
    const bool ptt = press(node(SettingBinding::keybinds_add)) && press(custom(kKeybindsDraftActionRow), VK_LEFT) &&
                     control(custom(kKeybindsDraftActionRow))->selected_choice == 0 &&
                     press(custom(kKeybindsDraftRecordRow)) && type(VK_ESCAPE, 0) &&
                     !application_keybinds_settings_recording() && control(custom(kKeybindsDraftRecordRow)) &&
                     press(custom(kKeybindsDraftRecordRow)) && type('V', 0);
    check(ptt && voice.audio.push_to_talk_key == 'V' && chip(control(custom(kKeybindsPushToTalkRow)), utf8_literal("V")) &&
              keybinds.count == 1,
          "keybinds settings: push to talk records one key into the voice preferences; Esc only stops recording");

    check(press(custom(0)) && keybinds.count == 0 && custom_keybinds_load(&stored) && stored.count == 0 &&
              press(custom(kKeybindsPushToTalkRow)) && !voice.audio.push_to_talk_key &&
              !control(custom(kKeybindsPushToTalkRow)) && control(settings_list_control_id(SettingBinding::keybinds_defaults, 1)) &&
              !application_keybinds_settings_activate_row(settings_list_control_id(SettingBinding::keybinds_defaults, 1), 0) &&
              keybinds.count == 0,
          "keybinds settings: Delete removes keybinds; default shortcuts stay read-only");

    // Restore the environment and remove the fixture files.
    for (u32 i = 0; root[i]; ++i)
        folder[i] = file[i] = root[i];
    constexpr wchar_t app[] = L"\\MaiCord", bin[] = L"\\MaiCord\\keybinds.bin";
    u32 at = 0;
    while (root[at])
        ++at;
    for (u32 i = 0; i < sizeof(bin) / sizeof(wchar_t); ++i)
        file[at + i] = bin[i];
    for (u32 i = 0; i < sizeof(app) / sizeof(wchar_t); ++i)
        folder[at + i] = app[i];
    DeleteFileW(file);
    RemoveDirectoryW(folder);
    RemoveDirectoryW(root);
    SetEnvironmentVariableW(L"LOCALAPPDATA", previous_length && previous_length < 1024 ? previous : nullptr);
}
} // namespace maicord::app_detail

namespace maicord::app_detail {
// application_shell_input.cpp opens the server header popout.
bool application_guild_menu_toggle(bool) noexcept {
    return false;
}
} // namespace maicord::app_detail
