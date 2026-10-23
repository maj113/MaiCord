#pragma once

#include "maicord/app/update_service.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/ui/caption_bar.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "settings_catalog.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {

// Exe-only build inputs (application_update_entry.cpp): repository, publisher
// key, the installed version and its source commit compiled from CMake.
UpdateServiceConfig application_update_build_config() noexcept;
// The first seven hex digits of the build's commit; empty when unknown.
Utf8View application_update_build_commit() noexcept;
// Child process: confirm startup once the account module API is admitted.
void application_updates_confirm_startup() noexcept;

enum class ApplicationUpdateState : u8 {
    disabled, idle, checking, downloading, installing, up_to_date, ready, restarting, error, rate_limited,
};
inline constexpr u32 kApplicationUpdatesCommitChars = 7;
struct ApplicationUpdatesView {
    ApplicationUpdateState state;
    UpdateChannel channel;
    UpdateCheckInterval interval;
    UpdateServiceError error;
    u64 own_version, ready_version, rollback_version;
    u64 received, total;
    // The newest verified offer on the channel (zero when none): its size,
    // and from its changelog the release or build date and commit.
    u64 offer_version, offer_bytes, offer_date;
    // Bumped whenever the published changelog changes.
    u64 changelog_revision;
    char own_commit[kApplicationUpdatesCommitChars], offer_commit[kApplicationUpdatesCommitChars];
};
// "0.3.4-dev.16+581fd44" for the view's offered build.
u32 application_updates_offer_label(const ApplicationUpdatesView&, Slice<char> output) noexcept;

// Settings rows for one view. Text lives in the caller's scratch until the
// next projection. Returns false when the row is hidden. meter receives the
// row's progress in thousandths, or kUiSettingsNoMeter.
struct ApplicationUpdatesText {
    char label[96], description[256], chip_text[96];
    Utf8View chips[4];
};
bool application_updates_project_view(const ApplicationUpdatesView&, SettingBinding, ApplicationUpdatesText*,
    UiSettingsControl*, UiSettingsControlPresentation*, u16* meter = nullptr) noexcept;

// The "What's new" rows that follow the updates.changelog heading: one notice
// per newer stable release (its notes as display text), or one table row per
// newer preview commit with its date, then a note when changes are missing.
inline constexpr u32 kApplicationUpdatesChangelogRows = 36;
struct ApplicationUpdatesChangelogRow {
    Utf8View label, description;
    Utf8View column[1];
    bool commit;
};
struct ApplicationUpdatesChangelogRows {
    u64 built_revision, built_own, built_offer;
    u32 count;
    bool preview, built;
    Utf8View heading;
    ApplicationUpdatesChangelogRow rows[kApplicationUpdatesChangelogRows];
    TextBank bank;
    char text[kUpdateChangelogMaximumBytes + 8192];
};
// Rebuilds rows from the changelog entries newer than the view's own build.
void application_updates_changelog_build(const ApplicationUpdatesView&, const UpdateChangelog&,
    ApplicationUpdatesChangelogRows*) noexcept;
// The heading row (false when there is nothing to list) and the rows after it,
// with their table decorations.
bool application_updates_changelog_heading(const ApplicationUpdatesChangelogRows&, UiSettingsControl*,
    UiSettingsListItem*) noexcept;
u32 application_updates_changelog_project(const ApplicationUpdatesChangelogRows&, Slice<UiSettingsControl>,
    Slice<UiSettingsControlPresentation>) noexcept;
UiSettingsListItem application_updates_changelog_item(const ApplicationUpdatesChangelogRows&, u32 row) noexcept;
constexpr bool application_updates_changelog_row(u64 control_id) noexcept {
    return control_id >= settings_list_control_id(SettingBinding::update_changelog, 0) &&
           control_id <= settings_list_control_id(SettingBinding::update_changelog, 0xff);
}

// UI thread. A background worker checks at the chosen interval (jitter,
// backoff, server retry times) and on demand; nothing here blocks on the
// network. commit: application_update_build_commit().
bool application_updates_initialize(const UpdateServiceConfig&, Utf8View commit = {}) noexcept;
// The first frame was presented: the first check follows within seconds. If
// this never comes (a window hidden at startup), it runs 1 to 3 minutes after
// launch.
void application_updates_ready() noexcept;
void application_updates_release() noexcept;
HANDLE application_updates_notification() noexcept;
// Refreshes Settings on change. True once a new version confirmed startup
// and this window should close.
bool application_updates_poll() noexcept;
bool application_updates_project(SettingBinding, UiSettingsControl*, UiSettingsControlPresentation*,
    u16* meter = nullptr) noexcept;
// The Updates page's changelog rows after its heading, their table
// decorations by control id (heading: the updates.changelog row) and the
// available update's progress meter.
u32 application_updates_settings_list(Slice<UiSettingsControl>, Slice<UiSettingsControlPresentation>) noexcept;
UiSettingsListItem application_updates_list_item(u64 control_id, bool heading) noexcept;
u16 application_updates_meter() noexcept;
// Row storage for the Updates page (null before initialization).
inline constexpr u32 kApplicationUpdatesSettingsRows = 48;
struct ApplicationUpdatesSettingsBank {
    UiSettingsControl controls[kApplicationUpdatesSettingsRows];
    UiSettingsControlPresentation presentation[kApplicationUpdatesSettingsRows];
    UiSettingsListItem items[kApplicationUpdatesSettingsRows];
    u16 meters[kApplicationUpdatesSettingsRows];
};
ApplicationUpdatesSettingsBank* application_updates_settings_bank() noexcept;
bool application_updates_activate(SettingBinding, u32 choice) noexcept;
// The view as of the last poll.
ApplicationUpdatesView application_updates_view() noexcept;
// Restarts into a ready update; false in any other state.
bool application_updates_restart() noexcept;

// The caption's update badge: shown while an update downloads, installs or
// waits for a restart, and after an update that failed to download or
// install. A click restarts a ready update and otherwise opens the Updates
// page.
constexpr UiCaptionUpdate application_update_badge(const ApplicationUpdatesView& view) noexcept {
    switch (view.state) {
    case ApplicationUpdateState::downloading:
        return UiCaptionUpdate::downloading;
    case ApplicationUpdateState::installing:
        return UiCaptionUpdate::installing;
    case ApplicationUpdateState::ready:
    case ApplicationUpdateState::restarting:
        return UiCaptionUpdate::ready;
    case ApplicationUpdateState::error:
        return view.offer_version > view.own_version ? UiCaptionUpdate::failed : UiCaptionUpdate::hidden;
    default:
        return UiCaptionUpdate::hidden;
    }
}
constexpr float application_update_progress(const ApplicationUpdatesView& view) noexcept {
    return view.total
               ? static_cast<float>(minimum(view.received, view.total)) / static_cast<float>(view.total)
               : 0;
}
constexpr bool application_update_badge_restarts(ApplicationUpdateState state) noexcept {
    return state == ApplicationUpdateState::ready;
}

} // namespace maicord::app_detail
