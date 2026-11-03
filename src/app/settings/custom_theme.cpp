#include "custom_theme.hpp"
#include "../shell/state.hpp"
#include "settings.hpp"
#include "theme_editor.hpp"
#include "../account/application_account_logout.hpp"
#include "maicord/app/settings/client_theme.hpp"
#include "maicord/platform/desktop/file_picker.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/decimal.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace {
struct CustomThemeOwner {
    ClientThemeStorageSnapshot applied, effective, candidate, save_request;
    ClientThemeIo* io;
    NativeFilePicker picker;
    FileSnapshot import_identity;
    wchar_t import_path[kClientThemeIoMaximumPathUnits];
    u32 import_units;
    u64 revision, load_generation, import_generation, save_generation;
    u64 settings_lifetime, account_scope, profile;
    Utf8View status;
    char diagnostic[192];
    bool selecting, picker_consumed, importing, loading, saving, save_failed;
    bool candidate_ready, previewing, read_failed, releasing, storage_dirty, offered;
    // What a finished import does: apply the theme, or preview it (Try it); install also copies the picked file
    // into the installed themes. fork_dirty: the active theme was edited into "<Name> (edited)" and its next save
    // exports it there.
    bool import_apply, import_preview, import_install, fork_dirty;
    // Installed themes; entries stay until the next scan completes.
    ClientThemeCatalog catalog;
    u64 catalog_generation, catalog_lifetime;
    bool catalog_scanning, catalog_wanted;
};
CustomThemeOwner* owner;
bool same(const ClientThemeStorageSnapshot& a, const ClientThemeStorageSnapshot& b) noexcept {
    if (a.builtin_theme != b.builtin_theme || a.custom_present != b.custom_present ||
        a.custom_active != b.custom_active || !background_theme_equal(a.gradient, b.gradient))
        return false;
    for (u32 i = 0; i < kBackgroundEffectProfileCount; ++i) {
        const auto x = a.effect_profiles[i], y = b.effect_profiles[i];
        if (x.speed != y.speed || x.strength != y.strength || x.density != y.density)
            return false;
    }
    return !a.custom_present || client_theme_document_equal(a.custom, b.custom);
}
void changed(CustomThemeOwner& o) noexcept {
    ++o.revision;
    application_request_frame();
}
CustomThemeOwner* ensure() noexcept {
    if (owner)
        return owner->releasing ? nullptr : owner;
    auto* o = static_cast<CustomThemeOwner*>(
        commit_pages(sizeof(CustomThemeOwner)));
    if (!o)
        return nullptr;
    const auto defaults = client_theme_document_defaults();
    if (!client_theme_storage_capture(&o->applied, g_app.preferences, defaults, false, false)) {
        VirtualFree(o, 0, MEM_RELEASE);
        return nullptr;
    }
    o->effective = o->applied;
    o->revision = 1;
    owner = o;
    return o;
}
bool io_ready(CustomThemeOwner& o) noexcept {
    return o.io || client_theme_io_create(&o.io);
}
// The IO owner replaces a queued request, so a scan only starts when no
// load, import or save is outstanding; otherwise it waits for the next poll.
void submit_catalog(CustomThemeOwner& o) noexcept {
    if (o.loading || o.importing || o.saving || o.catalog_scanning) {
        o.catalog_wanted = !o.catalog_scanning;
        return;
    }
    o.catalog_wanted = false;
    o.catalog_scanning = io_ready(o) && client_theme_io_submit_catalog(o.io, &o.catalog_generation);
}
// A load, import or save replaces a scan still queued: it scans again once that one is done.
void displace_catalog(CustomThemeOwner& o) noexcept {
    if (o.catalog_scanning) {
        o.catalog_scanning = false;
        o.catalog_wanted = true;
    }
}
bool interaction_current(const CustomThemeOwner& o) noexcept {
    return !o.releasing && g_app.account_ui && !g_app.login.visible &&
           !application_account_logout_blocked() && application_settings_active() &&
           (o.offered || application_settings_appearance_selected()) &&
           application_settings_lifetime() == o.settings_lifetime &&
           o.account_scope == g_app.plugin_account_scope_serial &&
           o.profile == g_app.account.status.profile_id;
}
bool capture_interaction(CustomThemeOwner& o) noexcept {
    o.settings_lifetime = application_settings_lifetime();
    o.account_scope = g_app.plugin_account_scope_serial;
    o.profile = g_app.account.status.profile_id;
    return interaction_current(o);
}
bool publish(CustomThemeOwner& o, const ClientThemeStorageSnapshot& snapshot) noexcept {
    if (same(snapshot, o.effective))
        return true;
    const auto prior = o.effective;
    const auto preferences = g_app.preferences;
    auto next = preferences;
    if (!client_theme_storage_apply_theme_fields(snapshot, &next))
        return false;
    // A built-in theme is remembered apart from any custom one, which only borrows its base.
    if (!snapshot.custom_active)
        next.builtin_theme = snapshot.builtin_theme;
    o.effective = snapshot;
    g_app.preferences = next;
    if (!application_custom_theme_publish(snapshot.custom_active ? &o.effective.custom : nullptr)) {
        o.effective = prior;
        g_app.preferences = preferences;
        return false;
    }
    application_preferences_apply();
    changed(o);
    return true;
}
// Outside a preview the published theme is the applied one; note whether it still has to be saved.
void adopt_effective(CustomThemeOwner& o) noexcept {
    if (o.previewing)
        return;
    o.storage_dirty = o.storage_dirty || !same(o.applied, o.effective);
    o.applied = o.effective;
}
constexpr Utf8View kEdited = utf8_literal(" (edited)");
bool forked(const ClientThemeDocument& d) noexcept {
    return d.name_bytes >= kEdited.bytes && bytes_equal(d.name + d.name_bytes - kEdited.bytes, kEdited.data, kEdited.bytes);
}
bool save(CustomThemeOwner& o) noexcept {
    if (o.previewing)
        return false;
    if (o.saving && same(o.save_request, o.applied))
        return true;
    // An edited copy of an installed theme joins the installed themes as its own file.
    const bool export_document = o.fork_dirty && o.applied.custom_present && forked(o.applied.custom);
    displace_catalog(o);
    if (!io_ready(o) || !client_theme_io_submit_save(o.io, o.applied, export_document, &o.save_generation)) {
        o.save_failed = true;
        o.status = utf8_literal("Theme applied, but couldn't be saved. Try again.");
        changed(o);
        return false;
    }
    if (export_document) {
        o.fork_dirty = false;
        o.catalog_wanted = true;
    }
    o.saving = true;
    o.save_request = o.applied;
    o.save_failed = false;
    o.status = utf8_literal("Saving theme…");
    changed(o);
    return true;
}
Utf8View builtin_name(ClientTheme theme) noexcept {
    static constexpr Utf8View names[]{utf8_literal("Dark"),   utf8_literal("AMOLED"),
                               utf8_literal("Aurora"), utf8_literal("Sunset"),
                               utf8_literal("Ocean"),  utf8_literal("Custom gradient"),
                               utf8_literal("Light"),  utf8_literal("Custom light gradient")};
    const auto index = static_cast<u32>(theme);
    return index < 8 ? names[index] : utf8_literal("Dark");
}
Utf8View document_name(const ClientThemeDocument& d) noexcept {
    return d.name_bytes ? Utf8View{d.name, d.name_bytes} : utf8_literal("Imported theme");
}
// "<Name> (edited)": the copy an edit makes of an installed theme, so the installed file stays as it was.
void fork(ClientThemeDocument& d) noexcept {
    if (forked(d))
        return;
    const Utf8View base = document_name(d);
    u32 kept = minimum<u32>(base.bytes, kClientThemeNameMaximumBytes - kEdited.bytes);
    while (kept && kept < base.bytes && (static_cast<u8>(base.data[kept]) & 0xC0) == 0x80)
        --kept;
    if (base.data != d.name)
        copy_bytes(d.name, base.data, kept);
    copy_bytes(d.name + kept, kEdited.data, kEdited.bytes);
    d.name_bytes = kept + kEdited.bytes;
    d.name[d.name_bytes] = 0;
}
bool same_name(const ClientThemeCatalogEntry& entry, const ClientThemeDocument& d) noexcept {
    const Utf8View name = document_name(d);
    return entry.name_bytes == name.bytes && bytes_equal(entry.name, name.data, name.bytes);
}
void activate_document(ClientThemeStorageSnapshot& snapshot) noexcept {
    snapshot.custom_active = true;
    snapshot.builtin_theme = snapshot.custom.base == ClientThemeBase::light    ? ClientTheme::light
                             : snapshot.custom.base == ClientThemeBase::amoled ? ClientTheme::amoled
                                                                               : ClientTheme::dark;
    if (background_theme_valid(snapshot.custom.theme.background))
        snapshot.gradient = snapshot.custom.theme.background;
    const auto index = background_effect_profile_index(snapshot.custom.theme.background.effect);
    if (index < kBackgroundEffectProfileCount)
        snapshot.effect_profiles[index] = background_effect_settings(snapshot.custom.theme.background);
}
void document_failure(CustomThemeOwner& o, u32 offset) noexcept {
    constexpr auto prefix = utf8_literal("Theme error at byte ");
    constexpr auto suffix = utf8_literal(".");
    copy_bytes(o.diagnostic, prefix.data, prefix.bytes);
    u32 count = prefix.bytes;
    count += decimal_write(o.diagnostic + count, offset);
    copy_bytes(o.diagnostic + count, suffix.data, suffix.bytes);
    count += suffix.bytes;
    o.status = {o.diagnostic, count};
}
// Contrast is advisory: the theme still previews and applies.
void ready_status(CustomThemeOwner& o, Utf8View prefix) noexcept {
    UiThemeContrastWarning warning{};
    if (!ui_theme_contrast_warnings(o.candidate.custom.theme, &warning, 1)) {
        o.status = prefix;
        return;
    }
    const auto tokens = ui_theme_token_descriptors();
    const auto& foreground = tokens[static_cast<u32>(warning.foreground)];
    const auto& background = tokens[static_cast<u32>(warning.background)];
    const u32 tenths = static_cast<u32>(warning.ratio * 10);
    char ratio[12]{};
    u32 digits = decimal_write(ratio, tenths / 10);
    ratio[digits++] = '.';
    ratio[digits++] = static_cast<char>('0' + tenths % 10);
    const Utf8View parts[]{prefix, utf8_literal(" Low contrast: "), foreground.group(), utf8_literal("."),
                           foreground.name(), utf8_literal(" on "), background.group(), utf8_literal("."),
                           background.name(), utf8_literal(" ("), {ratio, digits}, utf8_literal(":1).")};
    u32 used{};
    for (const auto part : parts) {
        if (used + part.bytes > sizeof(o.diagnostic)) {
            o.status = prefix;
            return;
        }
        copy_bytes(o.diagnostic + used, part.data, part.bytes);
        used += part.bytes;
    }
    o.status = {o.diagnostic, used};
}
bool renderer_resources_ready() noexcept {
    return g_app.renderer_ready && g_app.renderer.glyph_text.ready;
}
void saved_theme_resource_failure(CustomThemeOwner& o) noexcept {
    o.loading = o.candidate_ready = false;
    o.status = utf8_literal("Couldn't load your saved theme. Using your current theme.");
    changed(o);
}
bool start_import(CustomThemeOwner& o) noexcept {
    displace_catalog(o);
    if (!interaction_current(o) || !o.import_units || !io_ready(o) ||
        !client_theme_io_submit_import(o.io, {o.import_path, o.import_units}, o.import_identity, o.applied,
                                       o.import_install, &o.import_generation)) {
        o.read_failed = true;
        o.status = utf8_literal("Couldn't read the theme file. Try again or choose another file.");
        changed(o);
        return false;
    }
    o.importing = true;
    o.read_failed = false;
    o.status = utf8_literal("Loading theme…");
    changed(o);
    return true;
}
bool preview_candidate(CustomThemeOwner& o) noexcept {
    if (!o.candidate_ready)
        return false;
    activate_document(o.candidate);
    if (!publish(o, o.candidate)) {
        o.status = utf8_literal("Couldn't preview this theme. Try again.");
        changed(o);
        return false;
    }
    o.previewing = true;
    o.status = utf8_literal("Previewing theme. Apply to keep it.");
    changed(o);
    return true;
}
// Applies the loaded candidate directly: no preview state in between.
bool apply_candidate(CustomThemeOwner& o) noexcept {
    if (!o.candidate_ready)
        return false;
    activate_document(o.candidate);
    if (!publish(o, o.candidate)) {
        o.status = utf8_literal("Couldn't apply this theme. Try again.");
        changed(o);
        return false;
    }
    o.applied = o.effective;
    o.storage_dirty = true;
    o.previewing = o.candidate_ready = o.offered = false;
    return save(o);
}
void cancel(CustomThemeOwner& o) noexcept {
    if (o.selecting)
        native_file_picker_cancel(&o.picker);
    if (o.importing && o.io)
        client_theme_io_cancel(o.io, o.import_generation);
    o.selecting = o.importing = o.offered = false;
    o.import_generation = 0;
    if (o.previewing && publish(o, o.applied))
        o.previewing = false;
    o.candidate_ready = false;
    o.read_failed = false;
    o.import_units = 0;
    o.status = utf8_literal("Your applied theme is unchanged.");
    changed(o);
}
} // namespace

bool application_custom_theme_initialize() noexcept {
    auto* o = ensure();
    if (!o || !io_ready(*o) || o->loading || o->load_generation)
        return o && !o->releasing;
    if (!client_theme_io_submit_load(o->io, &o->load_generation))
        return false;
    o->loading = true;
    return true;
}
HANDLE application_custom_theme_notification() noexcept {
    const auto* o = owner;
    if (!o)
        return nullptr;
    if (o->picker.runtime)
        return o->picker_consumed ? o->picker.thread : o->picker.notification;
    return client_theme_io_completion_event(o->io);
}
const ClientThemeDocument* application_custom_theme_document() noexcept {
    return owner && owner->effective.custom_active ? &owner->effective.custom : nullptr;
}
ApplicationCustomThemeView application_custom_theme_view() noexcept {
    const auto* o = owner;
    if (!o)
        return {builtin_name(g_app.preferences.theme), {}, {}, 0};
    return {o->effective.custom_active ? document_name(o->effective.custom)
                                       : builtin_name(o->effective.builtin_theme),
            o->candidate_ready ? document_name(o->candidate.custom) : Utf8View{},
            o->status,
            o->revision,
            o->selecting || o->importing || o->loading,
            o->candidate_ready,
            o->previewing,
            o->effective.custom_active,
            o->save_failed,
            o->read_failed && o->import_units != 0};
}
bool application_custom_theme_action(ApplicationCustomThemeAction action) noexcept {
    auto* o = ensure();
    if (o && (o->candidate_ready || o->previewing || o->read_failed || o->importing || o->selecting) &&
        !interaction_current(*o)) {
        cancel(*o);
        return false;
    }
    if (!o || application_theme_editor_active() || !capture_interaction(*o))
        return false;
    if (action == ApplicationCustomThemeAction::cancel) {
        cancel(*o);
        return !o->previewing;
    }
    if (o->loading || o->selecting || o->importing)
        return false;
    switch (action) {
    case ApplicationCustomThemeAction::import_file:
        if (o->previewing && !publish(*o, o->applied))
            return false;
        o->previewing = o->candidate_ready = false;
        if (o->picker.runtime && !native_file_picker_release(&o->picker, 0))
            return false;
        o->import_apply = o->import_install = true;
        o->import_preview = false;
        o->picker_consumed = false;
        o->selecting =
            native_file_picker_start(&o->picker, g_app.window, 1, utf8_literal("Import a MaiCord theme")) ==
            NativeFilePickerError::none;
        o->status = o->selecting ? Utf8View{}
                                 : utf8_literal("Couldn't open the file picker. Try again.");
        changed(*o);
        return o->selecting;
    case ApplicationCustomThemeAction::preview:
        return preview_candidate(*o);
    case ApplicationCustomThemeAction::apply:
        if (!o->candidate_ready || !o->previewing)
            return false;
        o->applied = o->effective;
        o->storage_dirty = true;
        o->previewing = o->candidate_ready = o->offered = false;
        return save(*o);
    case ApplicationCustomThemeAction::retry_read:
        return o->read_failed && start_import(*o);
    case ApplicationCustomThemeAction::retry_save:
        return o->save_failed && save(*o);
    default:
        return false;
    }
}
ConstSlice<ClientThemeCatalogEntry> application_custom_theme_installed() noexcept {
    auto* o = owner;
    if (!o || o->releasing)
        return {};
    // One scan per Settings lifetime picks up files added while it was closed.
    if (o->catalog_lifetime != application_settings_lifetime()) {
        o->catalog_lifetime = application_settings_lifetime();
        submit_catalog(*o);
    }
    return {o->catalog.entries, o->catalog.count};
}
u32 application_custom_theme_installed_active() noexcept {
    const auto* o = owner;
    if (!o || o->releasing || !o->effective.custom_active)
        return ~0u;
    for (u32 i = 0; i < o->catalog.count; ++i)
        if (same_name(o->catalog.entries[i], o->effective.custom))
            return i;
    return ~0u;
}
bool application_custom_theme_apply_installed(u32 index, bool preview) noexcept {
    auto* o = ensure();
    if (!o || index >= o->catalog.count || application_theme_editor_active() || o->loading || o->selecting ||
        o->importing || !capture_interaction(*o))
        return false;
    if (o->previewing && !publish(*o, o->applied))
        return false;
    o->previewing = o->candidate_ready = false;
    wchar_t directory[kClientThemeIoMaximumPathUnits]{};
    u32 units = 0;
    if (!client_theme_catalog_directory({directory, kClientThemeIoMaximumPathUnits}).ok() ||
        !client_theme_catalog_path(directory, o->catalog.entries[index],
                                   {o->import_path, kClientThemeIoMaximumPathUnits}, &units))
        return false;
    o->import_units = units;
    o->import_identity = o->catalog.entries[index].identity;
    o->import_apply = !preview;
    o->import_preview = preview;
    o->import_install = false;
    return start_import(*o);
}
bool application_custom_theme_remove_installed(u32 index) noexcept {
    auto* o = owner;
    if (!o || o->releasing || index >= o->catalog.count)
        return false;
    const auto entry = o->catalog.entries[index];
    if (o->effective.custom_active && same_name(entry, o->effective.custom) && !application_custom_theme_use_builtin())
        return false;
    const bool removed = client_theme_catalog_remove(entry).ok();
    submit_catalog(*o);
    return removed;
}
void application_custom_theme_cancel_interaction() noexcept {
    if (owner && (owner->previewing || owner->selecting || owner->importing || owner->candidate_ready))
        cancel(*owner);
}
bool application_custom_theme_offer(const ClientThemeDocument& document) noexcept {
    auto* o = ensure();
    if (!o || o->loading || o->selecting || o->importing || application_theme_editor_active() ||
        !client_theme_document_valid(document) || !capture_interaction(*o))
        return false;
    if (o->previewing && !publish(*o, o->applied))
        return false;
    o->previewing = false;
    o->candidate = o->applied;
    o->candidate.custom = document;
    o->candidate.custom_present = true;
    o->candidate_ready = o->offered = true;
    o->read_failed = false;
    ready_status(*o, utf8_literal("Theme ready to preview."));
    changed(*o);
    return true;
}
void application_custom_theme_rescan() noexcept {
    if (owner && !owner->releasing)
        submit_catalog(*owner);
}
bool application_custom_theme_use_builtin() noexcept {
    auto* o = ensure();
    if (!o || o->loading || o->releasing)
        return false;
    if (o->previewing)
        cancel(*o);
    const ClientTheme prior = g_app.preferences.theme;
    g_app.preferences.theme = g_app.preferences.builtin_theme;
    if (application_custom_theme_preferences_changed(true, true))
        return true;
    g_app.preferences.theme = prior;
    return false;
}
bool application_custom_theme_preferences_changed(bool choose_builtin, bool persist) noexcept {
    auto* o = ensure();
    if (!o || o->loading || o->releasing)
        return false;
    // Built-in preferences never reach an active custom document: it keeps its own colours, gradient and effect.
    auto next = o->effective;
    if (!client_theme_storage_capture(&next, g_app.preferences, next.custom, next.custom_present,
                                      choose_builtin ? false : next.custom_active) ||
        !publish(*o, next))
        return false;
    if (choose_builtin)
        o->previewing = o->candidate_ready = false;
    adopt_effective(*o);
    return !persist || (!o->storage_dirty && !o->save_failed) || save(*o);
}
bool application_custom_theme_edit_background(const BackgroundTheme& background) noexcept {
    auto* o = ensure();
    if (!o || o->loading || o->releasing || !o->effective.custom_active)
        return false;
    auto next = o->effective;
    next.custom.theme.background = background;
    if (client_theme_document_equal(next.custom, o->effective.custom))
        return true;
    // The copy starts from the active document itself, so only the background differs from it.
    fork(next.custom);
    activate_document(next);
    if (!publish(*o, next))
        return false;
    o->fork_dirty = true;
    adopt_effective(*o);
    return true;
}
bool application_custom_theme_snapshot(ClientThemeStorageSnapshot* out) noexcept {
    if (!out || !owner)
        return false;
    *out = owner->effective;
    return true;
}
bool application_custom_theme_restore(const ClientThemeStorageSnapshot& snapshot) noexcept {
    auto* o = ensure();
    if (!o || !publish(*o, snapshot))
        return false;
    o->fork_dirty = o->fork_dirty && o->effective.custom_active && forked(o->effective.custom);
    adopt_effective(*o);
    return true;
}
namespace {
// Out of line: the completion holds a whole theme snapshot, and the poll runs on every loop wake.
__declspec(noinline) void complete_io(CustomThemeOwner* o) noexcept {
    ClientThemeIoCompletion completion{};
    bool ready{};
    if (!client_theme_io_poll(o->io, &completion, &ready) || !ready)
        return;
    if (completion.operation == ClientThemeIoOperation::load &&
        completion.generation == o->load_generation && o->loading) {
        if (completion.result.ok()) {
            if (!renderer_resources_ready()) {
                // Only startup readiness defers applying the theme. Resource failure
                // after readiness falls back to the complete active theme.
                o->candidate = completion.snapshot;
                o->candidate_ready = true;
                return;
            }
            if (!publish(*o, completion.snapshot)) {
                saved_theme_resource_failure(*o);
                return;
            }
            o->applied = o->effective;
            reset_value<Utf8View>(&o->status);
        } else if (completion.result.error != ClientThemeStorageError::missing)
            o->status =
                utf8_literal("Couldn't load your saved theme. Using the default theme.");
        o->loading = false;
        changed(*o);
    } else if (completion.operation == ClientThemeIoOperation::import_document &&
               completion.generation == o->import_generation && o->importing) {
        o->importing = false;
        if (!interaction_current(*o))
            return;
        if (completion.result.ok()) {
            o->candidate = completion.snapshot;
            o->candidate_ready = true;
            o->read_failed = false;
            ready_status(*o, completion.result.unsupported_fields
                                 ? utf8_literal("Theme loaded. Some settings aren't supported and were skipped.")
                                 : utf8_literal("Theme ready to preview."));
            // An imported file now lists among the installed themes.
            o->catalog_wanted = o->catalog_wanted || o->import_install;
            if (o->import_preview)
                preview_candidate(*o);
            else if (o->import_apply)
                apply_candidate(*o);
        } else {
            o->read_failed = true;
            const auto error = completion.result.error;
            if (error == ClientThemeStorageError::document)
                document_failure(*o, completion.result.document_offset);
            else
                o->status =
                    error == ClientThemeStorageError::changed ? utf8_literal("The file changed while loading. Choose it again.")
                    : error == ClientThemeStorageError::write || error == ClientThemeStorageError::flush ||
                            error == ClientThemeStorageError::commit || error == ClientThemeStorageError::directory
                        ? utf8_literal("Couldn't add the theme to Themes. Try again.")
                        : utf8_literal("Couldn't read the theme file. Try again or choose another file.");
        }
        changed(*o);
    } else if (completion.operation == ClientThemeIoOperation::catalog &&
               completion.generation == o->catalog_generation && o->catalog_scanning) {
        o->catalog_scanning = false;
        if (completion.result.ok())
            o->catalog = completion.catalog;
        changed(*o);
    } else if (completion.operation == ClientThemeIoOperation::save &&
               completion.generation == o->save_generation && o->saving) {
        o->saving = false;
        o->save_failed = !completion.result.ok();
        if (!o->save_failed && same(o->save_request, o->applied))
            o->storage_dirty = false;
        o->status = o->save_failed
                        ? utf8_literal("Theme applied, but couldn't be saved. Try again.")
                        : utf8_literal("Theme saved.");
        changed(*o);
    }
    if (o->catalog_wanted)
        submit_catalog(*o);
}
} // namespace
void application_custom_theme_poll() noexcept {
    auto* o = owner;
    if (!o || o->releasing)
        return;
    if (o->selecting && !interaction_current(*o))
        cancel(*o);
    if (o->picker.runtime) {
        const auto selected = native_file_picker_status(&o->picker);
        if (selected.phase == NativeFilePickerPhase::complete && !o->picker_consumed) {
            if (o->selecting && interaction_current(*o) && selected.ok() && selected.count == 1) {
                const auto path = native_file_picker_path(&o->picker, 0);
                if (path.count && path.count < kClientThemeIoMaximumPathUnits &&
                    native_file_picker_snapshot(&o->picker, 0, &o->import_identity)) {
                    copy_bytes(o->import_path, path.data, path.count * sizeof(wchar_t));
                    o->import_path[path.count] = 0;
                    o->import_units = path.count;
                    start_import(*o);
                } else {
                    o->status =
                        utf8_literal("The selected path is too long or unavailable. Choose another file.");
                    changed(*o);
                }
            } else if (o->selecting) {
                reset_value<Utf8View>(&o->status);
                changed(*o);
            }
            o->selecting = false;
            o->picker_consumed = true;
        }
        if (o->picker_consumed)
            native_file_picker_release(&o->picker, 0);
    }
    if (o->io && client_theme_io_completion_pending(o->io))
        complete_io(o);
}
bool application_custom_theme_synchronize() noexcept {
    auto* o = owner;
    if (!o)
        return true;
    if (o->releasing)
        return false;
    if (o->loading && o->candidate_ready) {
        if (!renderer_resources_ready())
            return true;
        if (!publish(*o, o->candidate)) {
            saved_theme_resource_failure(*o);
            return true;
        }
        o->applied = o->effective;
        o->loading = o->candidate_ready = false;
        changed(*o);
    }
    if ((o->previewing || o->candidate_ready || o->importing || o->selecting) && !interaction_current(*o))
        cancel(*o);
    application_custom_theme_poll();
    return !o->previewing || interaction_current(*o);
}
bool application_custom_theme_release(u32 timeout_ms) noexcept {
    auto* o = owner;
    if (!o)
        return true;
    if (o->previewing && !publish(*o, o->applied))
        return false;
    o->releasing = true;
    native_file_picker_cancel(&o->picker);
    if (!native_file_picker_release(&o->picker, timeout_ms))
        return false;
    if (!client_theme_io_stop(&o->io, timeout_ms))
        return false;
    owner = nullptr;
    VirtualFree(o, 0, MEM_RELEASE);
    return true;
}
} // namespace maicord::app_detail
