#pragma once
#include "maicord/app/packages/mcfx.hpp"
#include "maicord/core/base.hpp"
#include "maicord/render/effects/background_effect.hpp"

namespace maicord {
struct UiSettingsControl;
}

// Package background effects on the host side: the library of installed effects (.mcfx packages, see
// src/app/packages/packages.hpp), the saved choice, the user's global and per-effect settings and knob values,
// the picker rows after the built-in effects, the per-frame selection for the renderer, and the renderer's
// verdicts (too heavy, overloaded, removed with the device). No package code runs here: a package is data, and
// the renderer builds a pixel shader from its bytecode.
//
// The library reads %LOCALAPPDATA%\MaiCord\effects\*.mcfx and the development folder: %MAICORD_EFFECT_DEV% when
// set, else %LOCALAPPDATA%\MaiCord\effects\dev (entries there are `developer`).
namespace maicord::app_detail {

inline constexpr u32 kEffectLibraryCapacity = 64;

// Frame step, before the background clock: reloads nothing, selects the saved effect into the renderer (cheap
// when nothing changed) and acts on the renderer's verdict.
void application_effects_synchronize() noexcept;
// The background effect picker: appends the library by name after the built-in effects and selects the saved one.
// A control that is not the built-in list, or a library without entries, is left as is.
void application_effects_project(UiSettingsControl*) noexcept;
// A package choice is saved here (true, *result = saved); a built-in choice clears the package choice and
// returns false for the caller to write.
bool application_effects_activate(u32 choice, bool* result) noexcept;

// Window input arrived (the idle clock).
void application_effects_note_input() noexcept;
// A package effect is selected. Its clock then runs under its own frame rate and pause rules instead of the
// built-in effect's.
bool application_effects_selected() noexcept;
// The clock may run: not paused by the not-focused row or by five minutes without input.
bool application_effects_motion_allowed() noexcept;
// Milliseconds between the selected effect's frames.
u32 application_effects_interval_ms() noexcept;
// mc_ambient (interface 2: @uses clock, appearance). The selected package's inputs are sampled and eased in
// application_effects_synchronize. While one still eases toward a new value, the frame it is due next (else 0): the
// background clock asks this while the effect itself is paused or idle, so a theme switch still glides into place.
u64 application_effects_transition_wake() noexcept;
// Windows' app mode (AppsUseLightTheme under HKCU\...\Themes\Personalize; 0 is dark). The window procedure reads it
// again on WM_SETTINGCHANGE "ImmersiveColorSet" and hands it here; the appearance input then eases to it.
bool application_effects_read_system_dark() noexcept;
void application_effects_set_system_dark(bool dark) noexcept;

// The user's settings (Settings > Appearance, and the effect's own page). Setters validate, persist
// and repaint; they return false for a value out of range or a failed save.
EffectUserSettings application_effects_user_settings() noexcept;
bool application_effects_set_user_settings(const EffectUserSettings&) noexcept;
EffectShaderSettings application_effects_shader_settings(u64 identity) noexcept;
bool application_effects_set_shader_settings(const EffectShaderSettings&) noexcept;

// ---- The library ------------------------------------------------------------------------------------------
struct EffectInfo {
    u64 identity; // a hash of the whole file; the saved choice and the refusals are keyed by it
    char slug[kMcfxSlugBytes + 1], name[kMcfxNameBytes + 1], version[kMcfxVersionBytes + 1],
        license[kMcfxLicenseBytes + 1], authors[kMcfxAuthorsBytes + 1];
    u32 bytes;
    u8 knob_count;
    bool selected;  // the saved choice (a theme's effect is not "selected")
    bool developer; // from the development folder
};
u32 application_effects_count() noexcept;
u64 application_effects_identity(u32 index) noexcept;
// Valid until the library rescans (a frame after application_packages_library_changed).
const EffectInfo* application_effects_info(u32 index) noexcept;
// Reloads the folders now.
void application_effects_reload() noexcept;
bool application_effects_select(u32 index) noexcept;
// No package effect (the theme's, else the built-in effect applies).
bool application_effects_select_none() noexcept;
i32 application_effects_selected_index() noexcept;

// The package of one library entry, for its knob page: one at a time, valid until the next open, a rescan or
// the removal of the file.
const McfxPackage* application_effects_open(u32 index) noexcept;
// The open effect's knob values (natural units; a colour reads rgb). Set validates against the schema and applies
// live; commit true also saves (call it on release, not on every drag step).
bool application_effects_knob_get(u32 knob, McfxKnobValue* value) noexcept;
bool application_effects_knob_set(u32 knob, const McfxKnobValue& value, bool commit) noexcept;
// Back to the package's defaults (and the theme's values when the theme supplies the effect).
bool application_effects_knobs_reset() noexcept;
// The measured cost on this GPU, for the effect that is drawing now (known once it has been probed).
enum class EffectCostClass : u8 { light, medium, heavy };
struct EffectCost {
    bool known;
    bool refused; // too heavy even at the lowest resolution
    float milliseconds;
    EffectCostClass level;
};
EffectCost application_effects_cost(u64 identity) noexcept;
// Deletes a library entry's file, its knob values and any refusal; the selection falls back.
bool application_effects_remove(const char* slug, u32 slug_bytes) noexcept;
// A package that stopped being wanted (removed, revoked): if it is the saved choice or the theme's, the choice
// is cleared. Keeps the files.
void application_effects_deactivate(const char* slug, u32 slug_bytes) noexcept;
// Library rescan requested from any thread.
void application_effects_library_changed() noexcept;

// A trial: the candidate (bytes owned by the caller, parsed in `package`) draws as the window background at the
// Low budget until the trial ends, whatever else is chosen. false: a trial is already running.
bool application_effects_trial_begin(const ConstSlice<u8>& file, const McfxPackage& package) noexcept;
void application_effects_trial_end() noexcept;
// A trial the renderer turned down: the error is a verdict (rejected -> refused_shader semantics).
enum class EffectTrialState : u8 { none, drawing, rejected, refused };
EffectTrialState application_effects_trial_state() noexcept;
u64 application_effects_trial_identity() noexcept;
// The live preview of a knob page: while library entry `index` is set (~0u: none), it is the effect that draws.
// The window's own effect draws as it is; another entry draws in its place with the knob values and settings its
// page edits, at the Low budget, and a verdict on it is not remembered (like a trial's). `running`: the page's tile
// is on screen, so the preview's clock runs under Settings while the window has focus.
void application_effects_preview(u32 index, bool running) noexcept;
// The preview of the entry `identity`: drawing once it is the effect drawing now (its tile shows it), none until
// then or when another entry is previewed, rejected and refused as for a trial.
EffectTrialState application_effects_preview_state(u64 identity) noexcept;
// The background clock may run under Settings: the preview draws and its tile is on screen in a focused window.
bool application_effects_preview_running() noexcept;
// Small persisted answers that live in the effects state file (the maicord:// question was answered).
bool application_effects_state_flag(u32 bit) noexcept;
bool application_effects_set_state_flag(u32 bit, bool on) noexcept;

} // namespace maicord::app_detail
