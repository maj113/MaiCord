#pragma once

#include "maicord/app/settings/client_theme_storage.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {
enum class ApplicationCustomThemeAction : u8 {
    import_file, // Themes > Import theme…: pick a file, add it to the installed themes and apply it
    preview,
    apply,
    cancel,
    retry_read,
    retry_save,
};
struct ApplicationCustomThemeView {
    Utf8View current, candidate, status;
    u64 revision;
    bool busy, candidate_ready, previewing, custom_active, save_failed;
    bool can_retry_read;
};
// Root resolves all renderer/style consumers before accepting a publication.
// A null document requests the current built-in preference projection.
bool application_custom_theme_publish(const ClientThemeDocument*) noexcept;
bool application_custom_theme_initialize() noexcept;
HANDLE application_custom_theme_notification() noexcept;
void application_custom_theme_poll() noexcept;
bool application_custom_theme_synchronize() noexcept;
bool application_custom_theme_release(u32 timeout_ms = 0) noexcept;
const ClientThemeDocument* application_custom_theme_document() noexcept;
ApplicationCustomThemeView application_custom_theme_view() noexcept;
bool application_custom_theme_action(ApplicationCustomThemeAction) noexcept;
void application_custom_theme_cancel_interaction() noexcept;
// Installed themes (%LOCALAPPDATA%\MaiCord\themes\*.json): catalog and link installs, imported files and files
// put there by hand. Scanned once per Settings lifetime and again after each change.
ConstSlice<ClientThemeCatalogEntry> application_custom_theme_installed() noexcept;
// The installed theme the active custom theme is (by name), or ~0u.
u32 application_custom_theme_installed_active() noexcept;
// Loads an installed theme and applies it once it has loaded; preview loads it as a trial instead ("Try it").
bool application_custom_theme_apply_installed(u32 index, bool preview) noexcept;
// Deletes an installed theme file; an active one first falls back to the last built-in theme.
bool application_custom_theme_remove_installed(u32 index) noexcept;
// A theme from the store's install card (not an installed file) becomes the candidate: the usual
// preview, apply and cancel actions then work from any settings page. The page that shows the card must count
// as a theme panel, or leaving Appearance cancels the interaction.
bool application_custom_theme_offer(const ClientThemeDocument&) noexcept;
// A theme file was added or removed: the installed list scans again.
void application_custom_theme_rescan() noexcept;
// Leaves the custom theme for the last built-in one (ClientPreferences::builtin_theme) and saves.
bool application_custom_theme_use_builtin() noexcept;
// Built-in preference changes (palettes, gradient, effect, Use built-in). They never change an active custom
// document. choose_builtin is true only for an explicit built-in palette selection.
bool application_custom_theme_preferences_changed(bool choose_builtin, bool persist) noexcept;
// The theme editor's background edit of the active custom document. The edit forks it: the copy is the active
// document itself with this background, renamed "<Name> (edited)", and its saves export it to the installed themes.
bool application_custom_theme_edit_background(const BackgroundTheme&) noexcept;
bool application_custom_theme_snapshot(ClientThemeStorageSnapshot*) noexcept;
bool application_custom_theme_restore(const ClientThemeStorageSnapshot&) noexcept;
} // namespace maicord::app_detail
