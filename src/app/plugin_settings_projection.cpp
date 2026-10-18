#include "maicord/app/plugin_settings_projection.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
using Error = NativePluginSettingsError;
using Prefix = NativePluginSettingsPage;
bool controls_valid(ConstSlice<NativePluginSettingsControl> controls) noexcept {
    return controls.count <= kNativePluginSettingsMaximumControls &&
        byte_range_valid(controls.data, static_cast<usize>(controls.count) * sizeof(NativePluginSettingsControl));
}
}
NativePluginSettingsError native_plugin_settings_snapshot(const void* context,
    NativePluginSettingsSnapshot* output) noexcept {
    if (!output || !byte_range_valid(context, sizeof(Prefix)) ||
        byte_ranges_overlap(context, sizeof(Prefix), output, sizeof(*output)))
        return Error::invalid_argument;
    Prefix prefix{};
    copy_bytes(&prefix, context, sizeof(prefix));
    if (prefix.format != kNativePluginSettingsFormatHost) return Error::unsupported;
    NativePluginSettingsSnapshot candidate{prefix.title, prefix.description, prefix.controls, prefix.revision};
    if (!controls_valid(candidate.controls)) return Error::invalid_argument;
    *output = candidate;
    return Error::none;
}
NativePluginSettingsError native_plugin_settings_control(const NativePluginSettingsSnapshot& snapshot,
    u32 index, NativePluginSettingsControl* output) noexcept {
    if (!output || index >= snapshot.controls.count || !controls_valid(snapshot.controls))
        return Error::invalid_argument;
    NativePluginSettingsControl candidate{};
    // The stride belongs to this host DTO, never sizeof a native widget.
    copy_bytes(&candidate, reinterpret_cast<const u8*>(snapshot.controls.data) +
        static_cast<usize>(index) * sizeof(NativePluginSettingsControl), sizeof(candidate));
    if ((candidate.kind > NativePluginSettingsKind::action && candidate.kind != NativePluginSettingsKind::slider &&
         candidate.kind != NativePluginSettingsKind::category && candidate.kind != NativePluginSettingsKind::notice) ||
        candidate.editor) return Error::unsupported;
    if (!candidate.id) return Error::invalid_argument;
    if (candidate.kind == NativePluginSettingsKind::slider || candidate.kind == NativePluginSettingsKind::choice) {
        const u32 minimum_choices = candidate.kind == NativePluginSettingsKind::slider ? 2 : 1;
        if (candidate.choices.count < minimum_choices || candidate.choices.count > kNativePluginSettingsMaximumChoices ||
            !byte_range_valid(candidate.choices.data, static_cast<usize>(candidate.choices.count) * sizeof(Utf8View)) ||
            candidate.selected_choice >= candidate.choices.count) return Error::invalid_argument;
    }
    *output = candidate;
    return Error::none;
}
NativePluginSettingsError native_plugin_settings_project(const NativePluginSettingsSnapshot& snapshot,
    Slice<UiSettingsControl> controls, UiSettingsPageContent* output) noexcept {
    const usize output_bytes = static_cast<usize>(controls.count) * sizeof(UiSettingsControl);
    const usize source_bytes = static_cast<usize>(snapshot.controls.count) * sizeof(NativePluginSettingsControl);
    if (!output || !byte_range_valid(controls.data, output_bytes) ||
        byte_ranges_overlap(output, sizeof(*output), controls.data, output_bytes) ||
        byte_ranges_overlap(snapshot.controls.data, source_bytes, controls.data, output_bytes) ||
        byte_ranges_overlap(snapshot.controls.data, source_bytes, output, sizeof(*output)) ||
        byte_ranges_overlap(&snapshot, sizeof(snapshot), controls.data, output_bytes) ||
        byte_ranges_overlap(&snapshot, sizeof(snapshot), output, sizeof(*output))) return Error::invalid_argument;
    if (controls.count < snapshot.controls.count) return Error::capacity;
    if (!snapshot.title.bytes || !utf8_validate(snapshot.title).valid ||
        !utf8_validate(snapshot.description).valid)
        return Error::invalid_argument;
    for (u32 i = 0; i < snapshot.controls.count; ++i) {
        NativePluginSettingsControl control{};
        const auto read = native_plugin_settings_control(snapshot, i, &control);
        if (read != Error::none) return read;
        if ((!control.label.bytes && control.kind != NativePluginSettingsKind::notice) ||
            !utf8_validate(control.label).valid || !utf8_validate(control.description).valid ||
            !utf8_validate(control.action_label).valid ||
            (control.kind == NativePluginSettingsKind::action &&
                (!control.action_label.bytes || !utf8_validate(control.action_label).valid))) return Error::invalid_argument;
        if (control.kind == NativePluginSettingsKind::choice)
            for (u32 j = 0; j < control.choices.count; ++j)
                if (!control.choices[j].bytes || !utf8_validate(control.choices[j]).valid) return Error::invalid_argument;
    }
    for (u32 i = 0; i < snapshot.controls.count; ++i) {
        NativePluginSettingsControl control{};
        native_plugin_settings_control(snapshot, i, &control);
        controls[i] = {control.id, control.label, control.description,
            static_cast<UiSettingsControlKind>(control.kind), control.enabled, control.checked,
            control.choices, control.selected_choice, control.action_label, nullptr};
    }
    *output = {snapshot.title, snapshot.description, {controls.data, snapshot.controls.count},
        snapshot.revision, kUiSettingsPageFormat, nullptr, nullptr, {}};
    return Error::none;
}
} // namespace maicord
