#include "application_state.hpp"
#include "application_settings.hpp"
#include "application_theme_editor.hpp"
#include "application_account_logout.hpp"
#include "maicord/app/client_theme_storage.hpp"
#include "maicord/platform/file_picker.hpp"
#include "maicord/core/win32_types.hpp"

// This fixture includes the shipping owner under isolated names. The parser,
// owned document/storage snapshots and page registry are real; picker/IO and
// renderer publication are typed admission/receipt boundaries. No file, dialog,
// renderer font or worker is opened by this fixture.
namespace maicord {
bool custom_fixture_io_create(ClientThemeIo**, const wchar_t* = nullptr) noexcept;
bool custom_fixture_io_load(ClientThemeIo*, u64*) noexcept;
bool custom_fixture_io_import(ClientThemeIo*, ConstSlice<wchar_t>, const FileSnapshot&,
                              const ClientThemeStorageSnapshot&, u64*) noexcept;
bool custom_fixture_io_save(ClientThemeIo*, const ClientThemeStorageSnapshot&, u64*) noexcept;
bool custom_fixture_io_catalog(ClientThemeIo*, u64*) noexcept;
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
UiPageRegistry* custom_fixture_pages() noexcept;
bool custom_fixture_appearance_selected() noexcept;
bool custom_fixture_settings_active() noexcept;
u64 custom_fixture_settings_lifetime() noexcept;
void custom_fixture_refresh_pages() noexcept;
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
#define application_custom_theme_revision custom_fixture_revision
#define application_custom_theme_view custom_fixture_view
#define application_custom_theme_action custom_fixture_action
#define application_custom_theme_cancel_interaction custom_fixture_cancel_interaction
#define application_custom_theme_preferences_changed custom_fixture_preferences_changed
#define application_custom_theme_snapshot custom_fixture_snapshot
#define application_custom_theme_restore custom_fixture_restore
#define application_custom_theme_catalog custom_fixture_catalog
#define application_custom_theme_select_installed custom_fixture_select_installed
#define application_settings_pages custom_fixture_pages
#define application_settings_appearance_selected custom_fixture_appearance_selected
#define application_settings_active custom_fixture_settings_active
#define application_settings_lifetime custom_fixture_settings_lifetime
#define application_settings_refresh_pages custom_fixture_refresh_pages
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
#include "../src/app/application_custom_theme.cpp"

namespace maicord {
namespace custom_theme_test {
using namespace app_detail;
struct Transport {
    ClientThemeIoCompletion completion{};
    ClientThemeStorageSnapshot requested{};
    FileSnapshot imported_identity{};
    u64 serial{}, generation{}, cancelled{};
    u32 loads{}, imports{}, saves{}, publications{}, frames{}, refreshes{}, preparations{};
    ClientThemeIoOperation operation{};
    bool ready{}, picker_ready{}, publication_failed{}, stop_failed{}, blocked{};
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
bool import_candidate(const ClientThemeDocument& document) noexcept {
    if (!application_custom_theme_action(ApplicationCustomThemeAction::import_file))
        return false;
    transport.picker_ready = true;
    application_custom_theme_poll();
    auto candidate = transport.requested;
    candidate.custom = document;
    candidate.custom_present = true;
    candidate.custom_active = false;
    receipt(ClientThemeStorageError::none, &candidate);
    return application_custom_theme_view().candidate_ready;
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
                              const ClientThemeStorageSnapshot& baseline, u64* generation) noexcept {
    auto& t = custom_theme_test::transport;
    if (selected.count != sizeof(custom_theme_test::path) / sizeof(wchar_t) - 1)
        return false;
    t.requested = baseline;
    t.imported_identity = identity;
    ++t.imports;
    t.operation = ClientThemeIoOperation::import_document;
    *generation = t.generation = ++t.serial;
    return true;
}
bool custom_fixture_io_catalog(ClientThemeIo*, u64*) noexcept {
    return false; // Catalog scans are covered by the theme storage tests.
}
bool custom_fixture_io_save(ClientThemeIo*, const ClientThemeStorageSnapshot& snapshot,
                            u64* generation) noexcept {
    auto& t = custom_theme_test::transport;
    t.requested = snapshot;
    ++t.saves;
    t.operation = ClientThemeIoOperation::save;
    *generation = t.generation = ++t.serial;
    return true;
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
UiPageRegistry* custom_fixture_pages() noexcept {
    return &custom_theme_test::registry;
}
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
void custom_fixture_refresh_pages() noexcept {
    ++custom_theme_test::transport.refreshes;
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
    check(parsed.ok() && import_candidate(document) && transport.imported_identity.identity.index == 41 &&
              transport.imported_identity.identity.last_write == 42 && !transport.custom_published &&
              transport.saves == 0,
          "custom theme owner: exact picker identity admits validated owning candidate without apply or "
          "write");
    const auto candidate = document;
    document.name[0] = 'X';
    check(application_custom_theme_action(ApplicationCustomThemeAction::preview) &&
              application_custom_theme_view().previewing &&
              client_theme_document_equal(*application_custom_theme_document(), candidate) &&
              transport.saves == 0,
          "custom theme owner: explicit preview owns every palette/font/metric token independently of "
          "import storage");
    application_custom_theme_cancel_interaction();
    check(!application_custom_theme_document() && !application_custom_theme_view().previewing &&
              g_app.preferences.theme == ClientTheme::dark && transport.saves == 0,
          "custom theme owner: Settings X Escape or page withdrawal restores the complete prior theme "
          "without saving");
    check(import_candidate(candidate), "custom theme owner: a new import can follow cancelled preview");
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
              transport.saves == 2,
          "custom theme owner: failed disk commit truthfully remains applied and explicit Try agains the "
          "same snapshot");
    receipt(ClientThemeStorageError::none);
    check(!application_custom_theme_view().save_failed,
          "custom theme owner: exact successful save receipt clears the unsaved state");
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
              application_custom_theme_document()->theme.accent.red == candidate.theme.accent.red,
          "custom theme owner: background edit preserves imported foreground typography and geometry");
    check(application_custom_theme_restore(baseline) &&
              client_theme_document_equal(*application_custom_theme_document(), candidate),
          "custom theme owner: gesture rollback restores the complete custom snapshot");
    g_app.preferences.theme = ClientTheme::amoled;
    g_app.preferences.output_volume = 37;
    check(application_custom_theme_preferences_changed(true, true) &&
              !application_custom_theme_document() && application_custom_theme_view().imported &&
              transport.requested.builtin_theme == ClientTheme::amoled,
          "custom theme owner: explicit built-in selection persists while retaining imported document");
    receipt(ClientThemeStorageError::none);
    check(
        application_custom_theme_action(ApplicationCustomThemeAction::restore_imported) &&
            application_custom_theme_document() && g_app.preferences.output_volume == 37,
        "custom theme owner: restoring imported theme leaves independent notification preferences intact");
    receipt(ClientThemeStorageError::none);
    const auto revision = application_custom_theme_revision();
    const auto publications = transport.publications, frames = transport.frames, saves = transport.saves;
    bool warm = true;
    for (u32 i = 0; i < 64; ++i)
        warm = application_custom_theme_synchronize() && warm;
    check(warm && application_custom_theme_revision() == revision &&
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
    check(!application_custom_theme_release() && application_custom_theme_revision() != 0 &&
              !application_custom_theme_action(ApplicationCustomThemeAction::import_file),
          "custom theme owner: failed worker join retains the owner and rejects new actions");
    transport.stop_failed = false;
    check(application_custom_theme_release() && application_custom_theme_revision() == 0,
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
