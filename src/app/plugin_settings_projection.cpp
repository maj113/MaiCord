#include "maicord/app/plugin_settings_projection.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
using Error = NativePluginSettingsError;
using Prefix = NativePluginSettingsPage;
bool range(const void* data, usize bytes) noexcept {
    return !bytes || (data && reinterpret_cast<usize>(data) <= ~usize{0} - bytes);
}
bool overlaps(const void* a, usize an, const void* b, usize bn) noexcept {
    const auto av = reinterpret_cast<usize>(a), bv = reinterpret_cast<usize>(b);
    return an && bn && (av <= bv ? bv - av < an : av - bv < bn);
}
}
NativePluginSettingsError native_plugin_settings_snapshot(const void* context,
    NativePluginSettingsSnapshot* output) noexcept {
    if (!output || !range(context, sizeof(Prefix)) ||
        overlaps(context, sizeof(Prefix), output, sizeof(*output))) return Error::invalid_argument;
    Prefix prefix{};
    copy_bytes(&prefix, context, sizeof(prefix));
    if (prefix.format != kNativePluginSettingsFormatHost) return Error::unsupported;
    NativePluginSettingsSnapshot candidate{prefix.title, prefix.description, prefix.controls, prefix.revision};
    if (candidate.controls.count > kNativePluginSettingsMaximumControls ||
        !range(candidate.controls.data, static_cast<usize>(candidate.controls.count) * sizeof(NativePluginSettingsControl)))
        return Error::invalid_argument;
    *output = candidate;
    return Error::none;
}
NativePluginSettingsError native_plugin_settings_control(const NativePluginSettingsSnapshot& snapshot,
    u32 index, NativePluginSettingsControl* output) noexcept {
    if (!output || index >= snapshot.controls.count ||
        snapshot.controls.count > kNativePluginSettingsMaximumControls ||
        !range(snapshot.controls.data, static_cast<usize>(snapshot.controls.count) * sizeof(NativePluginSettingsControl)))
        return Error::invalid_argument;
    NativePluginSettingsControl candidate{};
    // The stride belongs to this host DTO, never sizeof a native widget.
    copy_bytes(&candidate, reinterpret_cast<const u8*>(snapshot.controls.data) +
        static_cast<usize>(index) * sizeof(NativePluginSettingsControl), sizeof(candidate));
    if ((candidate.kind > NativePluginSettingsKind::action && candidate.kind != NativePluginSettingsKind::slider &&
         candidate.kind != NativePluginSettingsKind::category && candidate.kind != NativePluginSettingsKind::notice) ||
        candidate.editor) return Error::unsupported;
    if (!candidate.id) return Error::invalid_argument;
    if (candidate.kind == NativePluginSettingsKind::slider &&
        (candidate.choices.count < 2 || candidate.choices.count > kNativePluginSettingsMaximumChoices ||
        !range(candidate.choices.data, static_cast<usize>(candidate.choices.count) * sizeof(Utf8View)) ||
        candidate.selected_choice >= candidate.choices.count)) return Error::invalid_argument;
    if (candidate.kind == NativePluginSettingsKind::choice &&
        (!candidate.choices.count || candidate.choices.count > kNativePluginSettingsMaximumChoices ||
        !range(candidate.choices.data, static_cast<usize>(candidate.choices.count) * sizeof(Utf8View)) ||
        candidate.selected_choice >= candidate.choices.count)) return Error::invalid_argument;
    *output = candidate;
    return Error::none;
}
NativePluginSettingsError native_plugin_settings_project(const NativePluginSettingsSnapshot& snapshot,
    Slice<UiSettingsControl> controls, UiSettingsPageContent* output) noexcept {
    if (!output || !range(controls.data, static_cast<usize>(controls.count) * sizeof(UiSettingsControl)) ||
        overlaps(output, sizeof(*output), controls.data, static_cast<usize>(controls.count) * sizeof(UiSettingsControl)) ||
        overlaps(snapshot.controls.data, static_cast<usize>(snapshot.controls.count) * sizeof(NativePluginSettingsControl),
            controls.data, static_cast<usize>(controls.count) * sizeof(UiSettingsControl)) ||
        overlaps(snapshot.controls.data, static_cast<usize>(snapshot.controls.count) * sizeof(NativePluginSettingsControl),
            output, sizeof(*output)) ||
        overlaps(&snapshot, sizeof(snapshot), controls.data, static_cast<usize>(controls.count) * sizeof(UiSettingsControl)) ||
        overlaps(&snapshot, sizeof(snapshot), output, sizeof(*output))) return Error::invalid_argument;
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
