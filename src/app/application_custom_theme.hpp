#pragma once

#include "maicord/app/client_theme_storage.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {
enum class ApplicationCustomThemeAction : u8 {
    import_file,
    preview,
    apply,
    cancel,
    restore_imported,
    retry_read,
    retry_save,
};
struct ApplicationCustomThemeView {
    Utf8View current, candidate, status;
    u64 revision;
    bool busy, candidate_ready, previewing, imported, custom_active, save_failed;
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
u64 application_custom_theme_revision() noexcept;
ApplicationCustomThemeView application_custom_theme_view() noexcept;
bool application_custom_theme_action(ApplicationCustomThemeAction) noexcept;
void application_custom_theme_cancel_interaction() noexcept;
// Names of installed themes (%LOCALAPPDATA%\MaiCord\themes\*.json), rescanned
// once per Settings lifetime. Selecting one imports it as the candidate.
ConstSlice<Utf8View> application_custom_theme_catalog() noexcept;
bool application_custom_theme_select_installed(u32 index) noexcept;
// Existing background edits retain every custom foreground/font/layout token.
// choose_builtin is true only for an explicit built-in palette selection.
bool application_custom_theme_preferences_changed(bool choose_builtin = false,
                                                  bool persist = false) noexcept;
bool application_custom_theme_snapshot(ClientThemeStorageSnapshot*) noexcept;
bool application_custom_theme_restore(const ClientThemeStorageSnapshot&, bool persist = false) noexcept;
u32 application_custom_theme_owner_bytes() noexcept;
} // namespace maicord::app_detail
