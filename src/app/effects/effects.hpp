#pragma once
#include "maicord/core/base.hpp"
#include "maicord/render/effects/background_effect.hpp"

namespace maicord {
struct UiSettingsControl;
}

// Package background effects on the host side: the library of installed effects, the saved choice, the user's
// global and per-effect settings, the picker rows after the built-in effects, the per-frame selection for
// the renderer, and the renderer's verdicts (too heavy, overloaded, removed with the device). No package code
// runs here: a package is data, and the renderer builds a pixel shader from its bytecode.
//
// Until the package format and the installer exist, the library reads the development folder: for each
// <name>.effect (key=value lines: name, uses, blend, resolution, fps, knobs in thousandths) the compiled
// <name>.cso beside it. The folder is %MAICORD_EFFECT_DEV% when set, else %LOCALAPPDATA%\MaiCord\effects\dev.
namespace maicord::app_detail {

inline constexpr u32 kEffectLibraryCapacity = 16;

// Frame step, before the background clock: reloads nothing, selects the saved effect into the renderer (cheap
// when nothing changed) and acts on the renderer's verdict.
void application_effects_synchronize() noexcept;
// The background effect picker: appends the library by name after the built-ins and selects the saved one.
// False leaves the control as is.
bool application_effects_project(UiSettingsControl*) noexcept;
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

// The user's settings (Settings > Appearance, and the effect's own page). Setters validate, persist
// and repaint; they return false for a value out of range or a failed save.
EffectUserSettings application_effects_user_settings() noexcept;
bool application_effects_set_user_settings(const EffectUserSettings&) noexcept;
EffectShaderSettings application_effects_shader_settings(u64 identity) noexcept;
bool application_effects_set_shader_settings(const EffectShaderSettings&) noexcept;

// The library, for tests and the settings page.
u32 application_effects_count() noexcept;
u64 application_effects_identity(u32 index) noexcept;
// Reloads the development folder.
void application_effects_reload() noexcept;

} // namespace maicord::app_detail
