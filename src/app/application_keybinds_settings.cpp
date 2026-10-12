#include "application_keybinds_settings.hpp"
#include "application_voice.hpp"
#include "maicord/core/log.hpp"

#include <Windows.h>

namespace maicord::app_detail {
namespace {
constexpr u32 kDefaultRows = 48, kChipBytes = 40;
constexpr Utf8View kActionChoices[]{utf8_literal("Push to talk"), utf8_literal("Toggle mute"),
                                    utf8_literal("Toggle deafen")};
static_assert(sizeof(kActionChoices) / sizeof(kActionChoices[0]) == static_cast<u32>(CustomKeybindAction::count));

struct Chips {
    char text[2][kChipBytes];
    Utf8View views[2];
};
struct KeybindsUi {
    CustomKeybinds keybinds;
    u32 revision;
    bool loaded, visible, drafting;
    CustomKeybindAction draft_action;
    CustomKeybindError error;
    ShortcutRecorder recorder;
    // Borrowed by projected rows until the next projection.
    Chips custom[kCustomKeybindMaximum], push_to_talk;
    Chips defaults[kDefaultRows];
    ShortcutListingRow listing[kDefaultRows];
    UiSettingsControl bank[kKeybindsSettingsBankCapacity];
    UiSettingsControlPresentation bank_presentation[kKeybindsSettingsBankCapacity];
};
KeybindsUi ui{};

void ensure_loaded() noexcept {
    if (ui.loaded)
        return;
    ui.loaded = true;
    ui.keybinds = {};
    custom_keybinds_load(&ui.keybinds);
    ++ui.revision;
}
void save() noexcept {
    ++ui.revision;
    if (!custom_keybinds_save(ui.keybinds))
        log_write(LogLevel::warning, "keybinds", "save_failed");
}
Utf8View chord_chip(Chips& chips, u32 slot, ShortcutChord chord) noexcept {
    const u32 bytes = shortcut_chord_label(chord, {chips.text[slot], kChipBytes});
    return chips.views[slot] = {chips.text[slot], bytes};
}
Utf8View key_chip(Chips& chips, u8 key) noexcept {
    const u32 bytes = shortcut_key_label(key, {chips.text[0], kChipBytes});
    return chips.views[0] = {chips.text[0], bytes};
}
UiSettingsControl row(u32 index, Utf8View label, Utf8View description, UiSettingsControlKind kind,
                      ConstSlice<Utf8View> choices, u32 selected, Utf8View action) noexcept {
    return {settings_list_control_id(SettingBinding::keybinds_custom, index), label, description, kind, true, false,
            choices, selected, action, nullptr};
}
void stop_draft() noexcept {
    ui.drafting = false;
    ui.recorder.active = false;
    ui.error = CustomKeybindError::none;
}
u32 custom_rows(Slice<UiSettingsControl> controls, Slice<UiSettingsControlPresentation> presentation) noexcept {
    ensure_loaded();
    u32 count = 0;
    const auto emit = [&](const UiSettingsControl& control, UiSettingsVariant variant) noexcept {
        if (count >= controls.count || count >= presentation.count)
            return;
        controls[count] = control;
        presentation[count++] = {variant, 0, kUiSettingsNoDefault};
    };
    const auto& audio = application_voice_preferences().audio;
    if (audio.push_to_talk_key) {
        key_chip(ui.push_to_talk, audio.push_to_talk_key);
        emit(row(kKeybindsPushToTalkRow, kActionChoices[0],
                 audio.mode == VoiceInputMode::push_to_talk ? Utf8View{}
                                                            : utf8_literal("Used when your input mode is push to talk."),
                 UiSettingsControlKind::action, {ui.push_to_talk.views, 1}, 0, utf8_literal("Delete")),
             UiSettingsVariant::critical);
    }
    for (u32 i = 0; i < ui.keybinds.count; ++i) {
        const auto& keybind = ui.keybinds.items[i];
        chord_chip(ui.custom[i], 0, keybind.chord);
        emit(row(i, custom_keybind_action_label(keybind.action), {}, UiSettingsControlKind::action,
                 {ui.custom[i].views, 1}, 0, utf8_literal("Delete")),
             UiSettingsVariant::critical);
    }
    if (!ui.drafting)
        return count;
    const bool push_to_talk = ui.draft_action == CustomKeybindAction::push_to_talk;
    emit(row(kKeybindsDraftActionRow, utf8_literal("Action"),
             push_to_talk && audio.push_to_talk_key ? utf8_literal("Replaces your push to talk key.") : Utf8View{},
             UiSettingsControlKind::radio, {kActionChoices, static_cast<u32>(CustomKeybindAction::count)},
             static_cast<u32>(ui.draft_action), {}),
         UiSettingsVariant::standard);
    const Utf8View hint = ui.recorder.active  ? utf8_literal("Press the keys now. Esc cancels.")
                          : ui.error != CustomKeybindError::none ? custom_keybind_error_text(ui.error)
                          : push_to_talk        ? utf8_literal("Hold this key to talk.")
                                                : utf8_literal("Ctrl or Alt combinations work best.");
    emit(row(kKeybindsDraftRecordRow, utf8_literal("Keys"), hint, UiSettingsControlKind::action, {}, 0,
             ui.recorder.active ? utf8_literal("Press keys\xE2\x80\xA6") : utf8_literal("Record keybind")),
         ui.recorder.active ? UiSettingsVariant::standard : UiSettingsVariant::primary);
    emit(row(kKeybindsDraftCancelRow, utf8_literal("New keybind"), utf8_literal("Nothing is saved until you record keys."),
             UiSettingsControlKind::action, {}, 0, utf8_literal("Cancel")),
         UiSettingsVariant::standard);
    return count;
}
u32 default_rows(Slice<UiSettingsControl> controls, Slice<UiSettingsControlPresentation> presentation) noexcept {
    const u32 listed = shortcut_listing({}, {ui.listing, kDefaultRows});
    u32 count = 0;
    for (u32 i = 0; i < listed && count < controls.count && count < presentation.count; ++i) {
        const auto& source = ui.listing[i];
        auto& chips = ui.defaults[i];
        u32 chip_count = 0;
        for (u32 chord = 0; chord < 2; ++chord)
            if (source.chords[chord].key)
                chord_chip(chips, chip_count++, source.chords[chord]);
        controls[count] = {settings_list_control_id(SettingBinding::keybinds_defaults, i), source.label, {},
                           source.header ? UiSettingsControlKind::category : UiSettingsControlKind::action, true,
                           false, source.header ? ConstSlice<Utf8View>{} : ConstSlice<Utf8View>{chips.views, chip_count},
                           0, {}, nullptr};
        presentation[count++] = {UiSettingsVariant::standard,
                                 static_cast<u8>(source.header ? 0 : ui_settings_presentation_compact),
                                 kUiSettingsNoDefault};
    }
    return count;
}
bool commit(ShortcutChord chord) noexcept {
    if (ui.draft_action == CustomKeybindAction::push_to_talk) {
        auto& preferences = application_voice_preferences();
        preferences.audio.push_to_talk_key = chord.key;
        application_voice_settings_changed(true);
        stop_draft();
        return true;
    }
    CustomKeybindError error{};
    if (!custom_keybinds_add(&ui.keybinds, {ui.draft_action, chord}, &error)) {
        ui.error = error;
        return true;
    }
    save();
    stop_draft();
    return true;
}
} // namespace

const CustomKeybinds& application_custom_keybinds() noexcept {
    ensure_loaded();
    return ui.keybinds;
}
u32 application_custom_keybinds_revision() noexcept {
    ensure_loaded();
    return ui.revision;
}

void application_keybinds_settings_bank(UiSettingsControl** controls,
                                        UiSettingsControlPresentation** presentation) noexcept {
    *controls = ui.bank;
    *presentation = ui.bank_presentation;
}

void application_keybinds_settings_visible(bool visible) noexcept {
    if (ui.visible == visible)
        return;
    ui.visible = visible;
    stop_draft();
}

bool application_keybinds_settings_project(SettingBinding binding, UiSettingsControl* control,
                                           UiSettingsControlPresentation*) noexcept {
    ensure_loaded();
    switch (binding) {
    case SettingBinding::keybinds_add:
        control->enabled = ui.keybinds.count < kCustomKeybindMaximum;
        return !ui.drafting;
    case SettingBinding::keybinds_defaults:
    case SettingBinding::keybinds_custom:
        return true;
    default:
        return false;
    }
}

u32 application_keybinds_settings_list(SettingBinding binding, Slice<UiSettingsControl> controls,
                                       Slice<UiSettingsControlPresentation> presentation) noexcept {
    if (!controls.data || !presentation.data)
        return 0;
    if (binding == SettingBinding::keybinds_custom)
        return custom_rows(controls, presentation);
    if (binding == SettingBinding::keybinds_defaults)
        return default_rows(controls, presentation);
    return 0;
}

bool application_keybinds_settings_activate(SettingBinding binding) noexcept {
    ensure_loaded();
    if (binding != SettingBinding::keybinds_add || ui.drafting || ui.keybinds.count >= kCustomKeybindMaximum)
        return false;
    ui.drafting = true;
    ui.draft_action = CustomKeybindAction::toggle_mute;
    ui.error = CustomKeybindError::none;
    ui.recorder.active = false;
    return true;
}

bool application_keybinds_settings_row(u64 control_id) noexcept {
    if (control_id < kSettingsListFirst || control_id >= kSettingsListFirst + 0x10000)
        return false;
    const auto binding = static_cast<SettingBinding>((control_id - kSettingsListFirst) >> 8);
    return binding == SettingBinding::keybinds_custom || binding == SettingBinding::keybinds_defaults;
}

bool application_keybinds_settings_activate_row(u64 control_id, u32 choice) noexcept {
    if (!application_keybinds_settings_row(control_id))
        return false;
    ensure_loaded();
    const auto binding = static_cast<SettingBinding>((control_id - kSettingsListFirst) >> 8);
    const u32 index = static_cast<u32>((control_id - kSettingsListFirst) & 0xff);
    if (binding != SettingBinding::keybinds_custom)
        return false; // Default shortcuts are read-only.
    if (index < kCustomKeybindMaximum) {
        if (!custom_keybinds_remove(&ui.keybinds, index))
            return false;
        save();
        return true;
    }
    switch (index) {
    case kKeybindsPushToTalkRow:
        application_voice_preferences().audio.push_to_talk_key = 0;
        application_voice_settings_changed(true);
        return true;
    case kKeybindsDraftActionRow:
        if (!ui.drafting || choice >= static_cast<u32>(CustomKeybindAction::count))
            return false;
        ui.draft_action = static_cast<CustomKeybindAction>(choice);
        ui.error = CustomKeybindError::none;
        ui.recorder.active = false;
        return true;
    case kKeybindsDraftRecordRow:
        if (!ui.drafting)
            return false;
        ui.error = CustomKeybindError::none;
        if (ui.recorder.active)
            ui.recorder.active = false;
        else
            shortcut_recorder_start(&ui.recorder, ui.draft_action == CustomKeybindAction::push_to_talk
                                                      ? ShortcutRecordMode::single_key
                                                      : ShortcutRecordMode::chord);
        return true;
    case kKeybindsDraftCancelRow:
        stop_draft();
        return true;
    default:
        return false;
    }
}

bool application_keybinds_settings_recording() noexcept {
    return ui.visible && ui.drafting && ui.recorder.active;
}

bool application_keybinds_settings_capture(UiInputKind kind, u32 code) noexcept {
    if (!application_keybinds_settings_recording())
        return false;
    // Alt+F4 still closes the window while recording.
    if (kind == UiInputKind::system_key_down && code == VK_F4 && (GetKeyState(VK_MENU) & 0x8000))
        return false;
    const bool down = kind == UiInputKind::key_down || kind == UiInputKind::system_key_down;
    if (!down && kind != UiInputKind::key_up)
        return true; // Characters of the recorded keys never reach the page.
    const u8 modifiers = static_cast<u8>(((GetKeyState(VK_CONTROL) & 0x8000) ? shortcut_ctrl : 0) |
                                         ((GetKeyState(VK_MENU) & 0x8000) ? shortcut_alt : 0) |
                                         ((GetKeyState(VK_SHIFT) & 0x8000) ? shortcut_shift : 0));
    ShortcutChord chord{};
    switch (shortcut_recorder_input(&ui.recorder, down, code, modifiers, &chord)) {
    case ShortcutRecordStep::recorded:
        return commit(chord);
    case ShortcutRecordStep::cancelled:
    case ShortcutRecordStep::cleared:
        ui.error = CustomKeybindError::none;
        return true;
    default:
        return true;
    }
}
} // namespace maicord::app_detail
