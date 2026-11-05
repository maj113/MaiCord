#pragma once

#include "maicord/app/update/service.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/ui/shell/caption_bar.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "../settings/catalog.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {

// Exe-only build inputs (entry.cpp): repository, publisher
// key, the installed version and its source commit compiled from CMake.
UpdateServiceConfig application_update_build_config() noexcept;
// The first seven hex digits of the build's commit; empty when unknown.
Utf8View application_update_build_commit() noexcept;
#if !defined(_WIN32)
// "1.2.3", or "1.2.4-dev.5+abcdef0" for a development build: what `MaiCord --version` prints. 0 when it does not fit.
u32 application_update_build_version_text(Slice<char> output) noexcept;
#endif

// installing: the download is verified and staged; nothing is installed
// before the user chooses Restart to update.
enum class ApplicationUpdateState : u8 {
    disabled, idle, checking, downloading, installing, up_to_date, ready, restarting, error, rate_limited,
};
inline constexpr u32 kApplicationUpdatesCommitChars = 7;
struct ApplicationUpdatesView {
    ApplicationUpdateState state;
    UpdateChannel channel;
    UpdateCheckInterval interval;
    UpdateServiceError error;
    u64 own_version, ready_version;
    u64 received, total;
    // The newest verified offer on the channel (zero when none): its size,
    // and from its changelog the release or build date and commit.
    u64 offer_version, offer_bytes, offer_date;
    // Bumped whenever the published changelog changes.
    u64 changelog_revision;
    // The last check that reached a verified manifest (unix seconds, zero
    // until one has).
    u64 checked_unix;
    char own_commit[kApplicationUpdatesCommitChars], offer_commit[kApplicationUpdatesCommitChars];
};

// Settings rows for one view. Text lives in the caller's scratch until the
// next projection. Returns false when the row is hidden. meter receives the
// row's progress in thousandths, kUiSettingsIndeterminateMeter, or
// kUiSettingsNoMeter. now_unix dates "Last checked".
struct ApplicationUpdatesText {
    char label[96], description[256], detail[96], chip_text[96];
    Utf8View chips[4];
};
bool application_updates_project_view(const ApplicationUpdatesView&, u64 now_unix, SettingBinding, ApplicationUpdatesText*,
    UiSettingsControl*, UiSettingsControlPresentation*, u16* meter = nullptr) noexcept;

// The "What's new" rows that follow the updates.changelog heading. A stable
// changelog lists one disclosure row per release (the newest relevant one open
// with its notes as a markdown row, older ones folded behind "N older
// releases"); a preview changelog lists one plain row per commit with its
// build and date, the installed build marked by a divider. The notes' text is
// copied only for open releases, and the page owns it until it is rebuilt.
inline constexpr u32 kApplicationUpdatesChangelogRows = 36;
// The low byte of a row's control id (settings_list_control_id(update_changelog, ..)):
// a disclosure row is its entry's index, the notes of entry k are 0x80 + k.
inline constexpr u32 kApplicationUpdatesRowNotes = 0x80, kApplicationUpdatesRowOlder = 0xf0,
    kApplicationUpdatesRowMore = 0xf1, kApplicationUpdatesRowInstalled = 0xf2, kApplicationUpdatesRowNote = 0xf3;
inline constexpr u32 kApplicationUpdatesVisibleReleases = 3, kApplicationUpdatesVisibleCommits = 10;
// Room for the release notes address: https://github.com/<owner>/<repo>/releases/tag/v<X.Y.Z>.
inline constexpr u32 kApplicationUpdatesReleaseNotesBytes = 192;
struct ApplicationUpdatesChangelogRow {
    Utf8View label, description, detail;
    UiSettingsControlKind kind;
    UiSettingsVariant tone;
    u8 more, id;
    bool checked;
};
struct ApplicationUpdatesChangelogRows {
    // What the rows were built from.
    u64 built_revision, built_own, built_offer, built_stamp;
    u32 count;
    bool built;
    Utf8View note;
    // The user's disclosure choices for entries of one changelog version:
    // flips against the default (the newest relevant release is open), the
    // notes shown unclipped, and the expanders. stamp counts changes.
    u64 state_version, stamp;
    u64 flipped[2], unclipped[2];
    bool older_open, more_open;
    ApplicationUpdatesChangelogRow rows[kApplicationUpdatesChangelogRows];
    TextBank bank;
    char text[kUpdateChangelogMaximumBytes + 8192];
};
// Rebuilds the rows for the view's own build and offer from the changelog.
void application_updates_changelog_build(const ApplicationUpdatesView&, const UpdateChangelog&,
    ApplicationUpdatesChangelogRows*) noexcept;
// The heading row's inline note (false when there is nothing to list), and the
// rows after it.
bool application_updates_changelog_heading(const ApplicationUpdatesChangelogRows&, UiSettingsControl*) noexcept;
u32 application_updates_changelog_project(const ApplicationUpdatesChangelogRows&, Slice<UiSettingsControl>,
    Slice<UiSettingsControlPresentation>) noexcept;
// A press on a row: opens or closes a release, the notes, or a list. False
// when the id names no row.
bool application_updates_changelog_toggle(ApplicationUpdatesChangelogRows*, u32 row_id) noexcept;
constexpr bool application_updates_changelog_row(u64 control_id) noexcept {
    return control_id >= settings_list_control_id(SettingBinding::update_changelog, 0) &&
           control_id <= settings_list_control_id(SettingBinding::update_changelog, 0xff);
}
// The release notes address for the view: a stable release's tag page on the
// repository, otherwise its releases page; 0 without a repository. Opens through
// the external-link confirmation.
u32 application_updates_release_notes_url(const ApplicationUpdatesView&, Utf8View repository, Slice<char> output) noexcept;

// UI thread. A background worker checks at the chosen interval (jitter,
// backoff, server retry times) and on demand; nothing here blocks on the
// network. commit: application_update_build_commit(). Also deletes the
// executable a previous swap replaced and a stage an earlier session left.
bool application_updates_initialize(const UpdateServiceConfig&, Utf8View commit = {}) noexcept;
// The first frame was presented: the first check follows within seconds. If
// this never comes (a window hidden at startup), it runs 1 to 3 minutes after
// launch.
void application_updates_ready() noexcept;
// What an exit does with a verified update. Only Restart to update installs
// it, on a normal exit, and starts MaiCord again; any other exit keeps the
// running version, and the next start stages the kept package again.
struct ApplicationUpdatesExit { bool install, relaunch; };
constexpr ApplicationUpdatesExit application_updates_exit(bool normal_exit, bool restart_requested) noexcept {
    return {normal_exit && restart_requested, restart_requested};
}
// Stops the worker, then applies application_updates_exit: after Restart to
// update and a normal exit (the frame loop ended with exit code 0) it swaps
// in the executable it verified and staged in this process, if any.
void application_updates_release(bool normal_exit) noexcept;
// The last step of an exit after Restart to update: starts the executable
// path, which a successful swap has pointed at the new version.
void application_updates_relaunch() noexcept;
HANDLE application_updates_notification() noexcept;
// Refreshes Settings on change. True once Restart asked this window to close.
bool application_updates_poll() noexcept;
bool application_updates_project(SettingBinding, UiSettingsControl*, UiSettingsControlPresentation*,
    u16* meter = nullptr) noexcept;
// The Updates page's changelog rows after its heading, and the status card's
// progress meter. The rows are built when the page asks for them, from the
// changelog the worker published; the page's visibility frees them.
u32 application_updates_settings_list(Slice<UiSettingsControl>, Slice<UiSettingsControlPresentation>) noexcept;
u16 application_updates_meter() noexcept;
void application_updates_visible(bool visible) noexcept;
// A press on a What's new row. False when nothing changed.
bool application_updates_activate_row(u64 control_id) noexcept;
// The release notes address (application_updates_release_notes_url) for the
// running view and repository; 0 when updates aren't configured.
u32 application_updates_release_notes(const Slice<char>& output) noexcept;
// Row storage for the Updates page (null before initialization).
inline constexpr u32 kApplicationUpdatesSettingsRows = 48;
struct ApplicationUpdatesSettingsBank {
    UiSettingsControl controls[kApplicationUpdatesSettingsRows];
    UiSettingsControlPresentation presentation[kApplicationUpdatesSettingsRows];
    u16 meters[kApplicationUpdatesSettingsRows];
};
ApplicationUpdatesSettingsBank* application_updates_settings_bank() noexcept;
bool application_updates_activate(SettingBinding, u32 choice) noexcept;
// The view as of the last poll.
ApplicationUpdatesView application_updates_view() noexcept;
// Restart to update: installs a ready update (a normal exit swaps it in, then
// relaunches); false in any other state.
bool application_updates_restart() noexcept;

// The caption's update badge: shown while an update downloads, is verified or
// waits for Restart to update, and after an update that failed to download or
// stage. A click installs a ready update and otherwise opens the Updates page.
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
