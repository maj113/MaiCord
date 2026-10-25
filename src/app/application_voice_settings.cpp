#include "application_settings.hpp"
#include "application_state.hpp"
#include "application_voice.hpp"
#include "voice_settings_binding.hpp"
#include "maicord/app/shortcut_registry.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/platform/voice_audio_capture.hpp"

namespace maicord::app_detail {
namespace {
constexpr u32 kDevices = 16, kMeters = 64;
struct DeviceList {
    VoiceAudioDevice devices[kDevices];
    Utf8View choices[kDevices + 1];
    char default_label[kVoiceAudioDeviceNameBytes + 16];
    u32 count;
};
struct VoiceSettingsUi {
    DeviceList inputs, outputs;
    u16 meters[kMeters];
    u32 meter_index;
    u8 meter_level, threshold;
    bool visible, enumerated;
    ShortcutRecorder recorder; // The shared key recorder, in single-key mode.
    char key_name[64];
};
VoiceSettingsUi page{};

void enumerate(DeviceList& list, bool capture) noexcept {
    list.count = voice_audio_devices(capture, {list.devices, kDevices});
    // "Default (name)" follows the Windows default; specific devices follow.
    static constexpr char prefix[] = "Default";
    u32 at = 0;
    for (u32 i = 0; prefix[i]; ++i)
        list.default_label[at++] = prefix[i];
    for (u32 i = 0; i < list.count; ++i)
        if (list.devices[i].is_default) {
            list.default_label[at++] = ' ';
            list.default_label[at++] = '(';
            copy_bytes(list.default_label + at, list.devices[i].name, list.devices[i].name_bytes);
            at += list.devices[i].name_bytes;
            list.default_label[at++] = ')';
            break;
        }
    list.choices[0] = {list.default_label, at};
    for (u32 i = 0; i < list.count; ++i)
        list.choices[i + 1] = {list.devices[i].name, list.devices[i].name_bytes};
}
bool same_id(const wchar_t* a, const wchar_t* b) noexcept {
    u32 i = 0;
    while (i + 1 < kVoiceDeviceIdCharacters && a[i] && a[i] == b[i])
        ++i;
    return a[i] == b[i];
}
u32 selected_device(const DeviceList& list, const wchar_t* id) noexcept {
    if (!id[0])
        return 0;
    for (u32 i = 0; i < list.count; ++i)
        if (same_id(list.devices[i].id, id))
            return i + 1;
    return 0;
}
Utf8View key_name(u8 key) noexcept {
    return {page.key_name, shortcut_key_label(key, {page.key_name, sizeof(page.key_name)})};
}
SettingBinding binding_of(u64 control_id) noexcept {
    UiSettingsSchemaLocation location{};
    const auto* node = settings_control_location(control_id, &location) ? settings_catalog_node(location) : nullptr;
    return node && node->binding() < static_cast<u32>(SettingBinding::count)
               ? static_cast<SettingBinding>(node->binding())
               : SettingBinding::none;
}
} // namespace

bool application_voice_settings_project(SettingBinding binding, UiSettingsControl* control,
                                        UiSettingsControlPresentation* presentation) noexcept {
    const auto& p = application_voice_preferences();
    const auto& a = p.audio;
    const bool push_to_talk = a.mode == VoiceInputMode::push_to_talk;
    SettingValue value{};
    if (voice_settings_read(p, binding, &value)) {
        control->checked = value.checked;
        control->selected_choice = value.choice;
    }
    switch (binding) {
    case SettingBinding::voice_input_device:
    case SettingBinding::voice_output_device: {
        // Search results can show these rows before the page was opened.
        if (!page.enumerated) {
            enumerate(page.inputs, true);
            enumerate(page.outputs, false);
            page.enumerated = true;
        }
        const bool input = binding == SettingBinding::voice_input_device;
        const auto& list = input ? page.inputs : page.outputs;
        control->choices = {list.choices, list.count + 1};
        control->selected_choice = selected_device(list, input ? a.input_device : a.output_device);
        return true;
    }
    case SettingBinding::voice_auto_sensitivity:
        return !push_to_talk;
    case SettingBinding::voice_sensitivity: {
        // Automatic mode shows (and locks) the threshold it chose.
        const auto status = application_voice_status();
        control->enabled = !a.auto_sensitivity;
        if (a.auto_sensitivity && status.capture == VoiceCaptureState::capturing && status.threshold)
            control->selected_choice = status.threshold;
        page.threshold = static_cast<u8>(control->selected_choice);
        if (presentation)
            presentation->default_choice = kUiSettingsNoDefault;
        return !push_to_talk;
    }
    case SettingBinding::voice_push_to_talk_key:
        control->action_label = page.recorder.active     ? utf8_literal("Press a key…")
                                : a.push_to_talk_key ? key_name(a.push_to_talk_key)
                                                     : utf8_literal("Record shortcut");
        if (page.recorder.active)
            control->description = utf8_literal("Press Esc to cancel or Backspace to clear.");
        else if (!a.push_to_talk_key)
            control->description = utf8_literal("Choose a key to hold while you talk.");
        return push_to_talk;
    case SettingBinding::voice_push_to_talk_release:
        return push_to_talk;
    default:
        return true;
    }
}

bool application_voice_settings_activate(SettingBinding binding, bool checked, u32 choice, bool commit) noexcept {
    auto& p = application_voice_preferences();
    auto next = p;
    switch (binding) {
    case SettingBinding::voice_input_device:
    case SettingBinding::voice_output_device: {
        const bool input = binding == SettingBinding::voice_input_device;
        const auto& list = input ? page.inputs : page.outputs;
        wchar_t* id = input ? next.audio.input_device : next.audio.output_device;
        if (choice > list.count)
            return false;
        clear_bytes(id, kVoiceDeviceIdCharacters * sizeof(wchar_t));
        if (choice)
            copy_bytes(id, list.devices[choice - 1].id, kVoiceDeviceIdCharacters * sizeof(wchar_t));
        break;
    }
    case SettingBinding::voice_push_to_talk_key:
        if (page.recorder.active)
            page.recorder.active = false;
        else
            shortcut_recorder_start(&page.recorder, ShortcutRecordMode::single_key);
        return true;
    case SettingBinding::voice_input_mode:
        // The Push to Talk switch picks the input mode.
        choice = static_cast<u32>(checked ? VoiceInputMode::push_to_talk : VoiceInputMode::voice_activity);
        if (!voice_settings_write(&next, binding, {checked, choice}))
            return false;
        break;
    case SettingBinding::voice_sensitivity:
        if (p.audio.auto_sensitivity)
            return false;
        [[fallthrough]];
    default:
        if (!voice_settings_write(&next, binding, {checked, choice}))
            return false;
        break;
    }
    if (!voice_preferences_valid(next))
        return false;
    p = next;
    application_voice_settings_changed(commit);
    return true;
}

void application_voice_settings_visible(bool visible) noexcept {
    if (page.visible == visible)
        return;
    page.visible = visible;
    page.recorder.active = false;
    // Re-read endpoints each time the page opens (devices come and go).
    page.enumerated = false;
    application_voice_set_monitor(visible);
}

const u16* application_voice_settings_meters(ConstSlice<UiSettingsControl> controls) noexcept {
    page.meter_index = kMeters;
    if (controls.count > kMeters)
        return nullptr;
    const auto status = application_voice_status();
    for (u32 i = 0; i < controls.count; ++i) {
        page.meters[i] = kUiSettingsNoMeter;
        if (binding_of(controls[i].id) == SettingBinding::voice_sensitivity) {
            page.meter_index = i;
            page.meter_level = status.capture == VoiceCaptureState::capturing ? status.level : 0;
            page.meters[i] = page.meter_level;
        }
    }
    return page.meters;
}

bool application_voice_settings_recording() noexcept {
    return page.visible && page.recorder.active;
}

bool application_voice_settings_capture(UiInputKind kind, u32 code) noexcept {
    if (!page.recorder.active || !page.visible)
        return false;
    ShortcutChord chord{};
    const auto step = shortcut_recorder_input(
        &page.recorder, kind == UiInputKind::key_down || kind == UiInputKind::system_key_down, code, 0, &chord);
    if (step == ShortcutRecordStep::recorded || step == ShortcutRecordStep::cleared) {
        application_voice_preferences().audio.push_to_talk_key = step == ShortcutRecordStep::recorded ? chord.key : 0;
        application_voice_settings_changed(true);
    }
    return true;
}

void application_voice_settings_tick() noexcept {
    if (!page.visible)
        return;
    const auto status = application_voice_status();
    const u8 level = status.capture == VoiceCaptureState::capturing ? status.level : 0;
    if (page.meter_index < kMeters && level != page.meter_level) {
        page.meter_level = level;
        page.meters[page.meter_index] = level;
        g_app.renderer.retained_presentation.valid = false; // The meter is not in the presentation stamp.
        application_request_frame();
    }
    const auto& a = application_voice_preferences().audio;
    if (a.auto_sensitivity && a.mode == VoiceInputMode::voice_activity && status.threshold &&
        status.threshold != page.threshold)
        application_settings_refresh_pages();
}
} // namespace maicord::app_detail
