#pragma once
#include "maicord/app/packages/link.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/app/packages/mcfx.hpp"
#include "maicord/app/settings/client_theme_document.hpp"
#include "../effects/effects.hpp"

// Themes and background effects as data packages (plugin design 16): validation, install from a link, a file or
// a drop, the "not reviewed" session, the installed-effect library and its knob values, and the maicord://
// handler. A package is data: nothing in one runs on the CPU. Catalog items arrive through package_validate and
// the files the catalog service writes; this module never talks to the catalog.
//
// Threads: package_validate is pure and runs on any thread. Everything named application_* is UI thread only,
// except application_packages_library_changed.
//
// Storage under %LOCALAPPDATA%\MaiCord:
//   effects\<slug>-<sha16>.mcfx          installed effect (sha16 = first 16 hex of the file's SHA-256)
//   effects\<slug>.knobs                 the user's knob values, kept across updates (by knob id)
//   effects\dev\*.mcfx                   Developer folder (%MAICORD_EFFECT_DEV% overrides); listed as `dev`
//   themes\<slug>.maicord-theme.json     installed theme (unreviewed ones: <name-slug>-<hash8>.maicord-theme.json)
namespace maicord::app_detail {

enum class PackageError : u8 {
    none,
    busy,           // another install session is open
    unsupported,    // a plugin or an unknown file type
    bad_file,       // unreadable
    unreachable,    // the host could not be reached, or answered with an error
    redirect,       // the host redirected somewhere else
    too_large,
    invalid,        // container, manifest or theme document refused
    refused_shader, // the bytecode is not a ps_5_0 shader, or the GPU's runtime refused it
    too_heavy,      // the selection probe: too heavy for this GPU
    capacity,       // the library is full
    conflict,       // the slug belongs to a catalog item
    storage,        // the file could not be written
};

// ---- Validation (pure; for the catalog worker and for tests) ------------------------------------------------
struct PackageInfo {
    PackageLinkKind kind; // theme or effect
    char slug[kPackageSlugMaximumBytes + 1]; // effect: the manifest's; theme: derived from the name
    char name[kClientThemeNameMaximumBytes + 1];
    u32 bytes;
};
// Checks a downloaded file: .mcfx container and manifest, or a theme document (64 KiB). Allocates its scratch.
PackageError package_validate(PackageLinkKind kind, ConstSlice<u8> bytes, PackageInfo* info) noexcept;
// A catalog file changed on disk (installed, updated, rolled back, removed): the library rescans on the next
// frame. Any thread.
void application_packages_library_changed() noexcept;

// ---- Link, file and drop installs (unreviewed) ------------------------------------------------------------
enum class PackagePhase : u8 { idle, working, ready, failed };

// What the "not reviewed" card shows. Valid while the phase is `ready`.
struct PackageCard {
    PackageLinkKind kind;
    char host[65];      // the source host of a link; "" for a file
    char slug[kPackageSlugMaximumBytes + 1];
    char name[kClientThemeNameMaximumBytes + 1];
    u32 bytes;
    bool replaces; // an unreviewed item with this slug is already installed
    // Effects: the package (credit, knobs, uses, PREVIEW png). Borrowed from the session.
    const McfxPackage* effect;
    // Themes.
    u32 swatches[4];       // 0xRRGGBB: background, surface, text, accent
    char effect_slug[kPackageSlugMaximumBytes + 1]; // the theme's background effect; "" for none
    bool effect_installed; // that effect is in the library
};
struct PackageStatus {
    PackagePhase phase;
    PackageError error; // when failed
    u64 revision;       // changes on every change
};
// One session at a time. A link is https or maicord://install?url=; a file is any path (a drop passes its first
// file). Fetching is capped at 1.5 MiB (64 KiB for a theme), https only, never off the host.
bool application_packages_begin_link(Utf8View url) noexcept;
bool application_packages_begin_file(ConstSlice<wchar_t> path) noexcept;
PackageStatus application_packages_status() noexcept;
const PackageCard* application_packages_card() noexcept;
// Starts a live preview of the candidate: an effect becomes the window background at the Low budget; a theme
// previews like Settings > Appearance does. Ends with cancel or commit.
bool application_packages_preview_start() noexcept;
bool application_packages_previewing() noexcept;
// The selection probe for the previewed effect: this GPU's frame time at the window size.
EffectCost application_packages_preview_cost() noexcept;
// Installs the candidate (the user clicked Install) and ends the session. Records it as unreviewed in the
// catalog state. Does not select or apply it.
PackageError application_packages_commit() noexcept;
void application_packages_cancel() noexcept;

// ---- maicord:// ---------------------------------------------------------------------------------------------
// A link the handler received (or Install from link parsed): a catalog form for the page to open on that item,
// taken once. A `url` form starts a session instead (see application_packages_status).
struct PackageDeepLink {
    PackageLinkKind kind;
    char slug[kPackageSlugMaximumBytes + 1];
};
bool application_packages_take_deep_link(PackageDeepLink* link) noexcept;
// Routes a maicord:// URL from the command line, WM_COPYDATA or the paste box. False: refused (logged without
// content). Queued until the account is ready.
bool application_packages_open_link(ConstSlice<wchar_t> url) noexcept;
// Frame tick: queued links.
void application_packages_tick(u64 now_ms) noexcept;
// The download worker's completion event and its poll (main loop); release at exit.
HANDLE application_packages_notification() noexcept;
void application_packages_poll() noexcept;
void application_packages_release() noexcept;
// Implemented by the store pages: opens Settings on the page for `kind` (and its detail view for a catalog
// link). Called from the tick.
bool application_store_open(PackageLinkKind kind) noexcept;

// Opt-in registration (HKCU\Software\Classes\maicord only). `asked` is false until the user answered once.
bool application_packages_protocol_asked() noexcept;
// "Turn on" registers; "Not now" only records that the question was answered.
bool application_packages_protocol_answer(bool enable) noexcept;

// Drag and drop of a file onto the window while the install dialog is open (the store pages enable it for
// that time). The first dropped file starts a session; every other drop goes to attachments.
void application_packages_drop_enable(bool wanted) noexcept;
bool application_packages_drop(HANDLE drop) noexcept;

// ---- Installed themes and effects --------------------------------------------------------------------------
bool application_packages_remove(PackageLinkKind kind, Utf8View slug) noexcept;
// Whatever is active from that item falls back (effect: the theme's, else None; theme: the last built-in one).
void application_packages_deactivate(PackageLinkKind kind, const Utf8View& slug) noexcept;

} // namespace maicord::app_detail
