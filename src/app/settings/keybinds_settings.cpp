#include "keybinds_settings.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/name_list.hpp"
#include "../voice/voice.hpp"
#include "../shell/overlay_input.hpp"
#include "maicord/core/log.hpp"


namespace maicord::app_detail {
namespace {
constexpr u32 kDefaultRows = kKeybindsSettingsBankCapacity, kChipBytes = 40;
constexpr u32 kActions = static_cast<u32>(CustomKeybindAction::count);
constexpr u32 kKeybindEdit = 0, kKeybindDelete = 1, kNotEditing = ~0u;

struct Chips {
    char text[2][kChipBytes];
    Utf8View views[2];
};
struct KeybindsUi {
    CustomKeybinds keybinds;
    u32 revision;
    bool loaded, visible, drafting;
    CustomKeybindAction draft_action;
    // The keybind the draft replaces (an index, kKeybindsPushToTalkRow, or kNotEditing for a new one).
    u32 editing;
    CustomKeybindError error;
    ShortcutRecorder recorder;
    // Borrowed by projected rows until the next projection.
    Chips custom[kCustomKeybindMaximum], push_to_talk;
    char push_to_talk_text[kChipBytes + 64];
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
    reset_value(&ui.keybinds);
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
__forceinline Utf8View key_chip(Chips& chips, u8 key) noexcept {
    const u32 bytes = shortcut_key_label(key, {chips.text[0], kChipBytes});
    return chips.views[0] = {chips.text[0], bytes};
}
UiSettingsControl row(u32 index, const Utf8View& label, const Utf8View& description,
                      UiSettingsControlKind kind, const ConstSlice<Utf8View>& choices, u32 selected,
                      const Utf8View& action) noexcept {
    return {settings_list_control_id(SettingBinding::keybinds_custom, index), label, description, kind, true, false,
            choices, selected, action, nullptr};
}
void stop_draft() noexcept {
    ui.drafting = false;
    ui.recorder.active = false;
    ui.error = CustomKeybindError::none;
    ui.editing = kNotEditing;
}
void start_draft(CustomKeybindAction action, u32 editing) noexcept {
    ui.drafting = true;
    ui.draft_action = action;
    ui.editing = editing;
    ui.error = CustomKeybindError::none;
    ui.recorder.active = false;
}
__forceinline u32 custom_rows(Slice<UiSettingsControl> controls, Slice<UiSettingsControlPresentation> presentation) noexcept {
    ensure_loaded();
    u32 count = 0;
    static Utf8View actions[kActions], buttons[2];
    name_list_views(actions, kCustomKeybindActionLabels, kActions);
    // A set keybind's buttons: change its action or keys, or remove it.
    name_list_views(buttons, "Edit\0Delete", 2);
    const auto emit = [&](const UiSettingsControl& control, UiSettingsVariant variant) noexcept {
        if (count >= controls.count)
            return;
        controls[count] = control;
        presentation[count++] = {variant, 0, kUiSettingsNoDefault};
    };
    const auto& audio = application_voice_preferences().audio;
    // Each set keybind: its action, its keys, and Edit and Delete.
    if (audio.push_to_talk_key) {
        // The key, and while the input mode is voice activity, when it applies.
        const Utf8View key = key_chip(ui.push_to_talk, audio.push_to_talk_key);
        Utf8View text = key;
        if (audio.mode != VoiceInputMode::push_to_talk) {
            constexpr Utf8View note = utf8_literal(" \xC2\xB7 Used when your input mode is push to talk.");
            copy_bytes(ui.push_to_talk_text, key.data, key.bytes);
            text = {ui.push_to_talk_text,
                    key.bytes + utf8_copy_truncated(ui.push_to_talk_text + key.bytes,
                                                    sizeof(ui.push_to_talk_text) - key.bytes, note)};
        }
        emit(row(kKeybindsPushToTalkRow, actions[0], text, UiSettingsControlKind::buttons, {buttons, 2}, 0, {}),
             UiSettingsVariant::standard);
    }
    for (u32 i = 0; i < ui.keybinds.count; ++i) {
        const auto& keybind = ui.keybinds.items[i];
        emit(row(i, custom_keybind_action_label(keybind.action), chord_chip(ui.custom[i], 0, keybind.chord),
                 UiSettingsControlKind::buttons, {buttons, 2}, 0, {}),
             UiSettingsVariant::standard);
    }
    if (!ui.drafting)
        return count;
    const bool push_to_talk = ui.draft_action == CustomKeybindAction::push_to_talk;
    emit(row(kKeybindsDraftActionRow, utf8_literal("Action"),
             push_to_talk && audio.push_to_talk_key ? utf8_literal("Replaces your push to talk key.") : Utf8View{},
             UiSettingsControlKind::radio, {actions, kActions},
             static_cast<u32>(ui.draft_action), {}),
         UiSettingsVariant::standard);
    const Utf8View hint = ui.recorder.active  ? utf8_literal("Press the keys now. Esc cancels.")
                          : ui.error != CustomKeybindError::none ? custom_keybind_error_text(ui.error)
                          : push_to_talk        ? utf8_literal("Hold this key to talk.")
                                                : utf8_literal("Ctrl or Alt combinations work best.");
    emit(row(kKeybindsDraftRecordRow, utf8_literal("Keys"), hint, UiSettingsControlKind::action, {}, 0,
             ui.recorder.active ? utf8_literal("Press keys\xE2\x80\xA6") : utf8_literal("Record keybind")),
         ui.recorder.active ? UiSettingsVariant::standard : UiSettingsVariant::primary);
    emit(row(kKeybindsDraftCancelRow, ui.editing == kNotEditing ? utf8_literal("New keybind") : utf8_literal("Edit keybind"),
             utf8_literal("Nothing is saved until you record keys."), UiSettingsControlKind::action, {}, 0,
             utf8_literal("Cancel")),
         UiSettingsVariant::standard);
    return count;
}
__forceinline u32 default_rows(Slice<UiSettingsControl> controls, Slice<UiSettingsControlPresentation> presentation) noexcept {
    const u32 listed = shortcut_listing({}, {ui.listing, kDefaultRows});
    u32 count = 0;
    for (u32 i = 0; i < listed && count < controls.count; ++i) {
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
__forceinline void commit(ShortcutChord chord) noexcept {
    auto next = ui.keybinds;
    if (ui.editing < next.count)
        custom_keybinds_remove(&next, ui.editing);
    auto& preferences = application_voice_preferences();
    if (ui.draft_action == CustomKeybindAction::push_to_talk) {
        // Push to talk is the one key the voice preferences hold.
        preferences.audio.push_to_talk_key = chord.key;
    } else {
        CustomKeybindError error{};
        if (!custom_keybinds_add(&next, {ui.draft_action, chord}, &error)) {
            ui.error = error;
            return;
        }
        // Push to talk moved to another action.
        if (ui.editing == kKeybindsPushToTalkRow)
            preferences.audio.push_to_talk_key = 0;
    }
    if (ui.draft_action == CustomKeybindAction::push_to_talk || ui.editing == kKeybindsPushToTalkRow)
        application_voice_settings_changed(true);
    if (ui.editing < ui.keybinds.count || ui.draft_action != CustomKeybindAction::push_to_talk) {
        ui.keybinds = next;
        save();
    }
    stop_draft();
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

bool application_keybinds_settings_project(SettingBinding binding, UiSettingsControl* control) noexcept {
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
    start_draft(CustomKeybindAction::toggle_mute, kNotEditing);
    return true;
}

bool application_keybinds_settings_row(u64 control_id) noexcept {
    if (!settings_list_control(control_id))
        return false;
    const auto binding = settings_list_binding(control_id);
    return binding == SettingBinding::keybinds_custom || binding == SettingBinding::keybinds_defaults;
}

bool application_keybinds_settings_activate_row(u64 control_id, u32 choice) noexcept {
    if (!application_keybinds_settings_row(control_id))
        return false;
    ensure_loaded();
    if (settings_list_binding(control_id) != SettingBinding::keybinds_custom)
        return false; // Default shortcuts are read-only.
    const u32 index = settings_list_index(control_id);
    if (index < kCustomKeybindMaximum) {
        if (index >= ui.keybinds.count)
            return false;
        if (choice == kKeybindEdit) {
            start_draft(ui.keybinds.items[index].action, index);
            return true;
        }
        if (choice != kKeybindDelete || !custom_keybinds_remove(&ui.keybinds, index))
            return false;
        stop_draft();
        save();
        return true;
    }
    switch (index) {
    case kKeybindsPushToTalkRow:
        if (choice == kKeybindEdit) {
            start_draft(CustomKeybindAction::push_to_talk, kKeybindsPushToTalkRow);
            return true;
        }
        if (choice != kKeybindDelete)
            return false;
        stop_draft();
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
    if (application_alt_f4(kind, code))
        return false;
    const bool down = kind == UiInputKind::key_down || kind == UiInputKind::system_key_down;
    if (!down && kind != UiInputKind::key_up)
        return true; // Characters of the recorded keys never reach the page.
    const u8 modifiers = static_cast<u8>((application_control_down() ? shortcut_ctrl : 0) |
                                         (application_alt_down() ? shortcut_alt : 0) |
                                         (application_shift_down() ? shortcut_shift : 0));
    ShortcutChord chord{};
    const auto step = shortcut_recorder_input(&ui.recorder, down, code, modifiers, &chord);
    if (step == ShortcutRecordStep::recorded)
        commit(chord);
    else if (step == ShortcutRecordStep::cancelled || step == ShortcutRecordStep::cleared)
        ui.error = CustomKeybindError::none;
    return true;
}
} // namespace maicord::app_detail
