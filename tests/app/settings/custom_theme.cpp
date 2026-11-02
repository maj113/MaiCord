#include "shell/state.hpp"
#include "settings/settings.hpp"
#include "settings/theme_editor.hpp"
#include "account/application_account_logout.hpp"
#include "maicord/app/settings/client_theme_storage.hpp"
#include "maicord/platform/desktop/file_picker.hpp"
#include "maicord/core/win32_types.hpp"

// This fixture includes the shipping owner under isolated names. The parser,
// owned document/storage snapshots and page registry are real; picker/IO and
// renderer publication are typed admission/receipt boundaries. No file, dialog,
// renderer font or worker is opened by this fixture.
namespace maicord {
bool custom_fixture_io_create(ClientThemeIo**, const wchar_t* = nullptr) noexcept;
bool custom_fixture_io_load(ClientThemeIo*, u64*) noexcept;
bool custom_fixture_io_import(ClientThemeIo*, ConstSlice<wchar_t>, const FileSnapshot&,
                              const ClientThemeStorageSnapshot&, bool, u64*) noexcept;
bool custom_fixture_io_save(ClientThemeIo*, const ClientThemeStorageSnapshot&, bool, u64*) noexcept;
bool custom_fixture_io_catalog(ClientThemeIo*, u64*) noexcept;
ClientThemeStorageResult custom_fixture_catalog_directory(Slice<wchar_t>) noexcept;
ClientThemeStorageResult custom_fixture_catalog_remove(const ClientThemeCatalogEntry&) noexcept;
HANDLE custom_fixture_io_event(const ClientThemeIo*) noexcept;
bool custom_fixture_io_poll(ClientThemeIo*, ClientThemeIoCompletion*, bool*) noexcept;
bool custom_fixture_io_pending(ClientThemeIo*) noexcept;
void custom_fixture_io_cancel(ClientThemeIo*, u64) noexcept;
bool custom_fixture_io_stop(ClientThemeIo**, u32) noexcept;
NativeFilePickerError custom_fixture_picker_start(NativeFilePicker*, HWND, u32, Utf8View) noexcept;
NativeFilePickerResult custom_fixture_picker_status(const NativeFilePicker*) noexcept;
ConstSlice<wchar_t> custom_fixture_picker_path(const NativeFilePicker*, u32) noexcept;
bool custom_fixture_picker_snapshot(const NativeFilePicker*, u32, FileSnapshot*) noexcept;
void custom_fixture_picker_cancel(NativeFilePicker*) noexcept;
bool custom_fixture_picker_release(NativeFilePicker*, u32) noexcept;
namespace app_detail {
bool custom_fixture_appearance_selected() noexcept;
bool custom_fixture_settings_active() noexcept;
u64 custom_fixture_settings_lifetime() noexcept;
bool custom_fixture_editor_active() noexcept;
bool custom_fixture_logout_blocked() noexcept;
void custom_fixture_preferences_apply() noexcept;
bool custom_fixture_request_frame() noexcept;
} // namespace app_detail
} // namespace maicord
#define application_custom_theme_publish custom_fixture_publish
#define application_custom_theme_initialize custom_fixture_initialize
#define application_custom_theme_notification custom_fixture_notification
#define application_custom_theme_poll custom_fixture_poll
#define application_custom_theme_synchronize custom_fixture_synchronize
#define application_custom_theme_release custom_fixture_release
#define application_custom_theme_document custom_fixture_document
#define application_custom_theme_view custom_fixture_view
#define application_custom_theme_action custom_fixture_action
#define application_custom_theme_cancel_interaction custom_fixture_cancel_interaction
#define application_custom_theme_preferences_changed custom_fixture_preferences_changed
#define application_custom_theme_snapshot custom_fixture_snapshot
#define application_custom_theme_restore custom_fixture_restore
#define application_custom_theme_installed custom_fixture_installed
#define application_custom_theme_installed_active custom_fixture_installed_active
#define application_custom_theme_apply_installed custom_fixture_apply_installed
#define application_custom_theme_remove_installed custom_fixture_remove_installed
#define application_custom_theme_offer custom_fixture_offer
#define application_custom_theme_rescan custom_fixture_rescan
#define application_custom_theme_use_builtin custom_fixture_use_builtin
#define client_theme_catalog_directory custom_fixture_catalog_directory
#define client_theme_catalog_remove custom_fixture_catalog_remove
#define application_settings_appearance_selected custom_fixture_appearance_selected
#define application_settings_active custom_fixture_settings_active
#define application_settings_lifetime custom_fixture_settings_lifetime
#define application_theme_editor_active custom_fixture_editor_active
#define application_account_logout_blocked custom_fixture_logout_blocked
#define application_preferences_apply custom_fixture_preferences_apply
#define application_request_frame custom_fixture_request_frame
#define client_theme_io_create custom_fixture_io_create
#define client_theme_io_submit_load custom_fixture_io_load
#define client_theme_io_submit_import custom_fixture_io_import
#define client_theme_io_submit_save custom_fixture_io_save
#define client_theme_io_submit_catalog custom_fixture_io_catalog
#define client_theme_io_completion_event custom_fixture_io_event
#define client_theme_io_poll custom_fixture_io_poll
#define client_theme_io_completion_pending custom_fixture_io_pending
#define client_theme_io_cancel custom_fixture_io_cancel
#define client_theme_io_stop custom_fixture_io_stop
#define native_file_picker_start custom_fixture_picker_start
#define native_file_picker_status custom_fixture_picker_status
#define native_file_picker_path custom_fixture_picker_path
#define native_file_picker_snapshot custom_fixture_picker_snapshot
#define native_file_picker_cancel custom_fixture_picker_cancel
#define native_file_picker_release custom_fixture_picker_release
#include "../../../src/app/settings/custom_theme.cpp"

namespace maicord {
namespace custom_theme_test {
using namespace app_detail;
struct Transport {
    ClientThemeIoCompletion completion{};
    ClientThemeStorageSnapshot requested{};
    FileSnapshot imported_identity{};
    u64 serial{}, generation{}, cancelled{}, catalog_generation{};
    u32 loads{}, imports{}, saves{}, publications{}, frames{}, preparations{}, catalogs{}, removals{};
    ClientThemeIoOperation operation{};
    bool ready{}, picker_ready{}, publication_failed{}, stop_failed{}, blocked{}, install{}, exported{}, scans{};
    bool settings_active{true};
    u64 settings_lifetime{1};
    bool custom_published{};
} transport;
UiPage pages[2]{};
UiPageRegistry registry{};
constexpr wchar_t path[] = LR"(D:\native-fixture\original-theme.json)";
constexpr FileSnapshot selected_identity{{41, 42, 43, 0}, 400};
void receipt(ClientThemeStorageError error, const ClientThemeStorageSnapshot* snapshot = nullptr) noexcept {
    transport.completion = {};
    transport.completion.generation = transport.generation;
    transport.completion.operation = transport.operation;
    transport.completion.result.error = error;
    transport.completion.snapshot = snapshot ? *snapshot : transport.requested;
    transport.ready = true;
    application_custom_theme_poll();
}
// The store's install card offers a document: the candidate the preview, apply and cancel actions work on.
bool offer_candidate(const ClientThemeDocument& document) noexcept {
    return application_custom_theme_offer(document) && application_custom_theme_view().candidate_ready;
}
// The installed theme scan completes with these entries.
void catalog_receipt(const ClientThemeCatalog& catalog) noexcept {
    transport.completion = {};
    transport.completion.generation = transport.catalog_generation;
    transport.completion.operation = ClientThemeIoOperation::catalog;
    transport.completion.catalog = catalog;
    transport.ready = true;
    application_custom_theme_poll();
}
} // namespace custom_theme_test
bool custom_fixture_io_create(ClientThemeIo** io, const wchar_t*) noexcept {
    *io = reinterpret_cast<ClientThemeIo*>(&custom_theme_test::transport);
    return true;
}
bool custom_fixture_io_load(ClientThemeIo*, u64* generation) noexcept {
    auto& t = custom_theme_test::transport;
    ++t.loads;
    t.operation = ClientThemeIoOperation::load;
    *generation = t.generation = ++t.serial;
    return true;
}
bool custom_fixture_io_import(ClientThemeIo*, ConstSlice<wchar_t> selected, const FileSnapshot& identity,
                              const ClientThemeStorageSnapshot& baseline, bool install, u64* generation) noexcept {
    auto& t = custom_theme_test::transport;
    if (selected.count != sizeof(custom_theme_test::path) / sizeof(wchar_t) - 1)
        return false;
    t.requested = baseline;
    t.imported_identity = identity;
    t.install = install;
    ++t.imports;
    t.operation = ClientThemeIoOperation::import_document;
    *generation = t.generation = ++t.serial;
    return true;
}
// Scans answer only while a test asks for them, through catalog_receipt and their own generation.
bool custom_fixture_io_catalog(ClientThemeIo*, u64* generation) noexcept {
    auto& t = custom_theme_test::transport;
    if (!t.scans)
        return false;
    ++t.catalogs;
    *generation = t.catalog_generation = ++t.serial;
    return true;
}
bool custom_fixture_io_save(ClientThemeIo*, const ClientThemeStorageSnapshot& snapshot, bool export_document,
                            u64* generation) noexcept {
    auto& t = custom_theme_test::transport;
    t.requested = snapshot;
    t.exported = export_document;
    ++t.saves;
    t.operation = ClientThemeIoOperation::save;
    *generation = t.generation = ++t.serial;
    return true;
}
// The installed themes live in the picker fixture's folder, so their paths are the picked file's.
ClientThemeStorageResult custom_fixture_catalog_directory(Slice<wchar_t> output) noexcept {
    constexpr wchar_t directory[] = LR"(D:\native-fixture)";
    copy_bytes(output.data, directory, sizeof(directory));
    return {};
}
ClientThemeStorageResult custom_fixture_catalog_remove(const ClientThemeCatalogEntry&) noexcept {
    ++custom_theme_test::transport.removals;
    return {};
}
HANDLE custom_fixture_io_event(const ClientThemeIo*) noexcept {
    return nullptr;
}
bool custom_fixture_io_poll(ClientThemeIo*, ClientThemeIoCompletion* out, bool* ready) noexcept {
    auto& t = custom_theme_test::transport;
    *ready = t.ready;
    if (t.ready) {
        *out = t.completion;
        t.ready = false;
    }
    return true;
}
bool custom_fixture_io_pending(ClientThemeIo*) noexcept {
    return custom_theme_test::transport.ready;
}
void custom_fixture_io_cancel(ClientThemeIo*, u64 generation) noexcept {
    custom_theme_test::transport.cancelled = generation;
}
bool custom_fixture_io_stop(ClientThemeIo** io, u32) noexcept {
    if (custom_theme_test::transport.stop_failed)
        return false;
    *io = nullptr;
    return true;
}
NativeFilePickerError custom_fixture_picker_start(NativeFilePicker* picker, HWND, u32 count,
                                                  Utf8View) noexcept {
    if (count != 1)
        return NativeFilePickerError::capacity;
    picker->runtime = &custom_theme_test::transport;
    custom_theme_test::transport.picker_ready = false;
    return NativeFilePickerError::none;
}
NativeFilePickerResult custom_fixture_picker_status(const NativeFilePicker*) noexcept {
    return {1, 0, NativeFilePickerError::none,
            custom_theme_test::transport.picker_ready ? NativeFilePickerPhase::complete
                                                      : NativeFilePickerPhase::showing};
}
ConstSlice<wchar_t> custom_fixture_picker_path(const NativeFilePicker*, u32) noexcept {
    return {custom_theme_test::path, sizeof(custom_theme_test::path) / sizeof(wchar_t) - 1};
}
bool custom_fixture_picker_snapshot(const NativeFilePicker*, u32, FileSnapshot* snapshot) noexcept {
    *snapshot = custom_theme_test::selected_identity;
    return true;
}
void custom_fixture_picker_cancel(NativeFilePicker*) noexcept {
    custom_theme_test::transport.picker_ready = true;
}
bool custom_fixture_picker_release(NativeFilePicker* picker, u32) noexcept {
    if (picker->runtime && !custom_theme_test::transport.picker_ready)
        return false;
    *picker = {};
    return true;
}
namespace app_detail {
bool custom_fixture_settings_active() noexcept {
    return custom_theme_test::transport.settings_active;
}
// The fixture registry names Appearance {1, 1}.
bool custom_fixture_appearance_selected() noexcept {
    const auto* page = ui_page_registry_selected(custom_theme_test::registry);
    return page && page->key.owner_id == 1 && page->key.page_id == 1;
}
u64 custom_fixture_settings_lifetime() noexcept {
    return custom_theme_test::transport.settings_lifetime;
}
bool custom_fixture_editor_active() noexcept {
    return false;
}
bool custom_fixture_logout_blocked() noexcept {
    return custom_theme_test::transport.blocked;
}
void custom_fixture_preferences_apply() noexcept {
}
bool custom_fixture_request_frame() noexcept {
    ++custom_theme_test::transport.frames;
    return true;
}
bool custom_fixture_publish(const ClientThemeDocument* document) noexcept {
    auto& t = custom_theme_test::transport;
    ++t.preparations;
    if (t.publication_failed)
        return false;
    ++t.publications;
    t.custom_published = document != nullptr;
    return true;
}
} // namespace app_detail
void test_application_custom_theme(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace app_detail;
    using namespace custom_theme_test;
    const auto prior_preferences = g_app.preferences;
    const auto prior_scope = g_app.plugin_account_scope_serial;
    const auto prior_profile = g_app.account.status.profile_id;
    const auto prior_ui = g_app.account_ui;
    const auto prior_login = g_app.login.visible;
    const auto prior_renderer_ready = g_app.renderer_ready;
    const auto prior_glyph_ready = g_app.renderer.glyph_text.ready;
    transport = {};
    transport.settings_active = true;
    transport.settings_lifetime = 1;
    ui_page_registry_initialize(&registry, {pages, 2});
    ui_page_registry_register(&registry, {{1, 1}, utf8_literal("Appearance"), nullptr});
    ui_page_registry_register(&registry, {{1, 2}, utf8_literal("Accessibility"), nullptr});
    g_app.preferences = client_preferences_defaults();
    g_app.plugin_account_scope_serial = 10;
    g_app.account.status.profile_id = 20;
    // The production owner checks UI identity presence, but never dereferences
    // this fixture boundary; account/model behavior belongs to their fixtures.
    g_app.account_ui = reinterpret_cast<AccountUiRuntime*>(&transport);
    g_app.login.visible = false;
    check(application_custom_theme_initialize() && transport.loads == 1 &&
              application_custom_theme_view().busy,
          "custom theme owner: startup queues one bounded asynchronous store load");
    receipt(ClientThemeStorageError::missing);
    check(!application_custom_theme_document() && !application_custom_theme_view().busy &&
              g_app.preferences.theme == ClientTheme::dark,
          "custom theme owner: absent store preserves legacy built-in preferences");
    u32 scratch[2048]{};
    ClientThemeDocument document{};
    const auto parsed = client_theme_document_parse(
        utf8_literal(
            R"({"version":"2.0","name":"Original theme","base":"light","colors":{"accent":[0.2,0.4,0.6,1]},"typography":{"body_size":18},"metrics":{"control_height":44}})"),
        client_theme_document_defaults(), {scratch, 2048}, &document);
    check(parsed.ok(),
          "custom theme owner: original document fixture has valid palette typography and metrics");
    ClientThemeStorageSnapshot saved{};
    client_theme_storage_capture(&saved, g_app.preferences, document, true, true);
    application_custom_theme_release();
    g_app.renderer_ready = g_app.renderer.glyph_text.ready = true;
    transport.publication_failed = true;
    application_custom_theme_initialize();
    receipt(ClientThemeStorageError::none, &saved);
    check(!application_custom_theme_view().busy && !application_custom_theme_view().candidate_ready &&
              !application_custom_theme_document() && g_app.preferences.theme == ClientTheme::dark &&
              utf8_equal(application_custom_theme_view().status,
                         utf8_literal("Couldn't load your saved theme. Using your current theme.")),
          "custom theme owner: permanent startup resource failure settles on active baseline with truthful "
          "feedback");
    const auto rejected_preparations = transport.preparations;
    const auto rejected_frames = transport.frames;
    bool settled = true;
    for (u32 i = 0; i < 64; ++i)
        settled = application_custom_theme_synchronize() && settled;
    check(settled && transport.preparations == rejected_preparations && transport.frames == rejected_frames,
          "custom theme owner: 64 frames after saved-theme resource failure do not repeat preparation or "
          "request frames");
    application_custom_theme_release();
    g_app.renderer_ready = g_app.renderer.glyph_text.ready = false;
    application_custom_theme_initialize();
    receipt(ClientThemeStorageError::none, &saved);
    const auto deferred_preparations = transport.preparations;
    check(application_custom_theme_view().busy && application_custom_theme_synchronize() &&
              transport.preparations == deferred_preparations,
          "custom theme owner: absent renderer readiness defers owned load without invoking preparation");
    g_app.renderer_ready = g_app.renderer.glyph_text.ready = true;
    check(application_custom_theme_synchronize() && !application_custom_theme_view().busy &&
              !application_custom_theme_view().candidate_ready && !application_custom_theme_document() &&
              transport.preparations == deferred_preparations + 1,
          "custom theme owner: deferred startup failure settles once readiness exists without fatal frame "
          "failure");
    application_custom_theme_release();
    transport.publication_failed = false;
    application_custom_theme_initialize();
    receipt(ClientThemeStorageError::missing);
    // A built-in choice before any custom theme is remembered apart from the custom theme's base.
    g_app.preferences.theme = ClientTheme::sunset;
    check(application_custom_theme_preferences_changed(true, true) &&
              g_app.preferences.builtin_theme == ClientTheme::sunset,
          "custom theme owner: choosing a built-in theme remembers it as the last built-in theme");
    receipt(ClientThemeStorageError::none);
    const u32 saves_before_offer = transport.saves;
    check(parsed.ok() && offer_candidate(document) && !transport.custom_published &&
              transport.saves == saves_before_offer,
          "custom theme owner: an offered document is a candidate without apply or write");
    const auto candidate = document;
    document.name[0] = 'X';
    check(application_custom_theme_action(ApplicationCustomThemeAction::preview) &&
              application_custom_theme_view().previewing &&
              client_theme_document_equal(*application_custom_theme_document(), candidate) &&
              transport.saves == saves_before_offer,
          "custom theme owner: explicit preview owns every palette/font/metric token independently of "
          "the offered document");
    application_custom_theme_cancel_interaction();
    check(!application_custom_theme_document() && !application_custom_theme_view().previewing &&
              g_app.preferences.theme == ClientTheme::sunset && transport.saves == saves_before_offer,
          "custom theme owner: Settings X Escape or page withdrawal restores the complete prior theme "
          "without saving");
    check(offer_candidate(candidate), "custom theme owner: a new offer can follow a cancelled preview");
    transport.publication_failed = true;
    const auto preference_before = g_app.preferences;
    check(!application_custom_theme_action(ApplicationCustomThemeAction::preview) &&
              !application_custom_theme_document() &&
              client_preferences_equal(preference_before, g_app.preferences) &&
              application_custom_theme_view().candidate_ready &&
              utf8_equal(application_custom_theme_view().status,
                         utf8_literal("Couldn't preview this theme. Try again.")),
          "custom theme owner: failed preview preparation preserves palette selector and candidate with "
          "visible explicit retry feedback");
    transport.publication_failed = false;
    check(application_custom_theme_action(ApplicationCustomThemeAction::preview) &&
              application_custom_theme_action(ApplicationCustomThemeAction::apply) &&
              transport.requested.custom_active && transport.requested.custom_present &&
              client_theme_document_equal(transport.requested.custom, candidate),
          "custom theme owner: Apply queues document and selection in one atomic persistence snapshot");
    receipt(ClientThemeStorageError::commit);
    check(application_custom_theme_document() && application_custom_theme_view().save_failed &&
              application_custom_theme_action(ApplicationCustomThemeAction::retry_save) &&
              transport.saves == saves_before_offer + 2,
          "custom theme owner: failed disk commit truthfully remains applied and explicit Try agains the "
          "same snapshot");
    receipt(ClientThemeStorageError::none);
    check(!application_custom_theme_view().save_failed && g_app.preferences.theme == ClientTheme::light &&
              g_app.preferences.builtin_theme == ClientTheme::sunset,
          "custom theme owner: an applied custom theme borrows its base and keeps the last built-in theme");
    const auto stable_document = *application_custom_theme_document();
    g_app.preferences.output_volume = 53;
    const auto writes_before_independent_edit = transport.saves;
    check(application_custom_theme_preferences_changed(false, true) &&
              client_theme_document_equal(*application_custom_theme_document(), stable_document) &&
              transport.saves == writes_before_independent_edit,
          "custom theme owner: unrelated preference edit neither flattens imported background nor rewrites "
          "theme store");
    ClientThemeStorageSnapshot baseline{};
    application_custom_theme_snapshot(&baseline);
    g_app.preferences.gradient = background_theme_defaults();
    g_app.preferences.gradient.angle_degrees = 212;
    g_app.preferences.theme = ClientTheme::custom_light_gradient;
    check(application_custom_theme_preferences_changed(false, false) &&
              application_custom_theme_document()->theme.background.angle_degrees == 212 &&
              ui_theme_typography_equal(application_custom_theme_document()->typography,
                                        candidate.typography) &&
              ui_theme_metrics_equal(application_custom_theme_document()->metrics, candidate.metrics) &&
              application_custom_theme_document()->theme.accent.red == candidate.theme.accent.red &&
              utf8_equal(application_custom_theme_view().current, utf8_literal("Original theme (edited)")),
          "custom theme owner: a background edit forks the theme into an edited copy and keeps its foreground "
          "typography and geometry");
    check(application_custom_theme_preferences_changed(false, true) && transport.exported &&
              transport.requested.custom_active &&
              utf8_equal({transport.requested.custom.name, transport.requested.custom.name_bytes},
                         utf8_literal("Original theme (edited)")),
          "custom theme owner: saving the edited copy also exports it to the installed themes");
    receipt(ClientThemeStorageError::none);
    check(application_custom_theme_restore(baseline) &&
              client_theme_document_equal(*application_custom_theme_document(), candidate),
          "custom theme owner: gesture rollback restores the complete custom snapshot");
    // A restart: the preferences record and the theme store load back.
    check(application_custom_theme_preferences_changed(false, true) && !transport.exported,
          "custom theme owner: the restored original saves without exporting a copy");
    receipt(ClientThemeStorageError::none);
    const auto stored = transport.requested;
    u8 record[kClientPreferencesRecordBytes]{};
    ClientPreferences reloaded = client_preferences_defaults();
    const bool record_ok = client_preferences_encode(g_app.preferences, {record, sizeof(record)}).ok() &&
                           client_preferences_decode({record, sizeof(record)}, &reloaded).ok();
    application_custom_theme_release();
    g_app.preferences = reloaded;
    application_custom_theme_initialize();
    receipt(ClientThemeStorageError::none, &stored);
    check(record_ok && application_custom_theme_document() && g_app.preferences.builtin_theme == ClientTheme::sunset,
          "custom theme owner: after a restart the custom theme is back and the last built-in theme is still known");
    g_app.preferences.output_volume = 37;
    check(application_custom_theme_use_builtin() && !application_custom_theme_document() &&
              g_app.preferences.theme == ClientTheme::sunset && transport.requested.builtin_theme == ClientTheme::sunset &&
              !transport.requested.custom_active && g_app.preferences.output_volume == 37,
          "custom theme owner: Use built-in restores the last built-in theme, not Dark, and saves it");
    receipt(ClientThemeStorageError::none);
    // Installed themes: Apply applies once the file loads, Try it previews, Remove falls back to the built-in.
    static ClientThemeCatalog installed{};
    installed.count = 1;
    auto& entry = installed.entries[0];
    copy_bytes(entry.name, candidate.name, candidate.name_bytes);
    entry.name_bytes = candidate.name_bytes;
    constexpr wchar_t file[] = L"original-theme.json";
    copy_bytes(entry.file, file, sizeof(file));
    entry.file_units = sizeof(file) / sizeof(wchar_t) - 1;
    entry.identity = selected_identity;
    transport.scans = true;
    application_custom_theme_installed();
    catalog_receipt(installed);
    transport.scans = false;
    check(transport.catalogs == 1 && application_custom_theme_installed().count == 1 &&
              application_custom_theme_installed_active() == ~0u,
          "custom theme owner: the installed themes come from one scan per Settings lifetime");
    const u32 imports_before_apply = transport.imports;
    check(application_custom_theme_apply_installed(0, false) && transport.imports == imports_before_apply + 1 &&
              !transport.install,
          "custom theme owner: Apply loads the installed file in place");
    auto loaded = transport.requested;
    loaded.custom = candidate;
    loaded.custom_present = true;
    loaded.custom_active = false;
    receipt(ClientThemeStorageError::none, &loaded);
    check(application_custom_theme_document() && !application_custom_theme_view().previewing &&
              !application_custom_theme_view().candidate_ready && transport.operation == ClientThemeIoOperation::save &&
              transport.requested.custom_active && application_custom_theme_installed_active() == 0,
          "custom theme owner: Apply makes the installed theme active as soon as it loads, with no preview state");
    receipt(ClientThemeStorageError::none);
    check(application_custom_theme_remove_installed(0) && transport.removals == 1 &&
              !application_custom_theme_document() && g_app.preferences.theme == ClientTheme::sunset &&
              transport.requested.builtin_theme == ClientTheme::sunset && !transport.requested.custom_active,
          "custom theme owner: removing the active installed theme falls back to the last built-in theme");
    receipt(ClientThemeStorageError::none);
    check(application_custom_theme_apply_installed(0, true), "custom theme owner: Try it loads the installed file");
    receipt(ClientThemeStorageError::none, &loaded);
    check(application_custom_theme_view().previewing && application_custom_theme_document(),
          "custom theme owner: Try it previews the installed theme until it is kept or reverted");
    application_custom_theme_cancel_interaction();
    check(!application_custom_theme_document() && g_app.preferences.theme == ClientTheme::sunset,
          "custom theme owner: reverting a trial restores the built-in theme");
    // Themes > Import theme…: the picked file joins the installed themes and is applied.
    check(application_custom_theme_action(ApplicationCustomThemeAction::import_file),
          "custom theme owner: Import opens the file picker");
    transport.picker_ready = true;
    application_custom_theme_poll();
    check(transport.install && transport.imported_identity.identity.index == 41 &&
              transport.imported_identity.identity.last_write == 42,
          "custom theme owner: the picked file is read by its exact identity and installed");
    auto picked = transport.requested;
    picked.custom = candidate;
    picked.custom_present = true;
    picked.custom_active = false;
    receipt(ClientThemeStorageError::none, &picked);
    check(application_custom_theme_document() &&
              client_theme_document_equal(*application_custom_theme_document(), candidate) &&
              !application_custom_theme_view().previewing && !application_custom_theme_view().candidate_ready &&
              transport.operation == ClientThemeIoOperation::save && transport.requested.custom_active,
          "custom theme owner: Import from Themes applies the theme with no preview state");
    receipt(ClientThemeStorageError::none);
    const auto revision = application_custom_theme_view().revision;
    const auto publications = transport.publications, frames = transport.frames, saves = transport.saves;
    bool warm = true;
    for (u32 i = 0; i < 64; ++i)
        warm = application_custom_theme_synchronize() && warm;
    check(warm && application_custom_theme_view().revision == revision &&
              transport.publications == publications && transport.frames == frames &&
              transport.saves == saves,
          "custom theme owner: 64 unchanged synchronizations add no publication repaint or IO admission");
    check(application_custom_theme_action(ApplicationCustomThemeAction::import_file),
          "custom theme owner: invalid document receipt fixture starts a fresh explicit import");
    transport.picker_ready = true;
    application_custom_theme_poll();
    transport.completion = {};
    transport.completion.generation = transport.generation;
    transport.completion.operation = ClientThemeIoOperation::import_document;
    transport.completion.result.error = ClientThemeStorageError::document;
    transport.completion.result.document_offset = 17;
    transport.ready = true;
    application_custom_theme_poll();
    check(!application_custom_theme_view().candidate_ready && application_custom_theme_document() &&
              utf8_equal(
                  application_custom_theme_view().status,
                  utf8_literal("Theme error at byte 17.")),
          "custom theme owner: parser failure retains active snapshot and reports the exact source offset");
    check(application_custom_theme_action(ApplicationCustomThemeAction::import_file),
          "custom theme owner: pending source withdrawal fixture starts picker");
    transport.picker_ready = true;
    application_custom_theme_poll();
    const auto retired = transport.generation;
    ++g_app.plugin_account_scope_serial;
    application_custom_theme_synchronize();
    auto stale = transport.requested;
    stale.custom = candidate;
    stale.custom_present = true;
    receipt(ClientThemeStorageError::none, &stale);
    check(transport.cancelled == retired && !application_custom_theme_view().candidate_ready &&
              application_custom_theme_document(),
          "custom theme owner: retired account-scope import receipt cannot replace the user-global applied "
          "snapshot");
    check(application_custom_theme_action(ApplicationCustomThemeAction::import_file),
          "custom theme owner: read failure fixture captures fresh authority");
    transport.picker_ready = true;
    application_custom_theme_poll();
    receipt(ClientThemeStorageError::read);
    check(application_custom_theme_view().can_retry_read &&
              application_custom_theme_action(ApplicationCustomThemeAction::retry_read),
          "custom theme owner: read error preserves copied path and exact source identity for explicit "
          "retry");
    ++transport.settings_lifetime;
    application_custom_theme_synchronize();
    receipt(ClientThemeStorageError::none, &stale);
    check(!application_custom_theme_view().candidate_ready,
          "custom theme owner: settings lifetime replacement rejects held import completion");
    transport.blocked = true;
    check(!application_custom_theme_action(ApplicationCustomThemeAction::import_file),
          "custom theme owner: durable logout admission blocks new native picker actions");
    transport.blocked = false;
    transport.stop_failed = true;
    check(!application_custom_theme_release() && application_custom_theme_view().revision != 0 &&
              !application_custom_theme_action(ApplicationCustomThemeAction::import_file),
          "custom theme owner: failed worker join retains the owner and rejects new actions");
    transport.stop_failed = false;
    check(application_custom_theme_release() && application_custom_theme_view().revision == 0,
          "custom theme owner: joined shutdown releases all retained snapshots");
    g_app.preferences = prior_preferences;
    g_app.plugin_account_scope_serial = prior_scope;
    g_app.account.status.profile_id = prior_profile;
    g_app.account_ui = prior_ui;
    g_app.login.visible = prior_login;
    g_app.renderer_ready = prior_renderer_ready;
    g_app.renderer.glyph_text.ready = prior_glyph_ready;
}
} // namespace maicord
