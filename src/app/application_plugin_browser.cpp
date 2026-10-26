#include "application_plugin_browser.hpp"
#include "maicord/core/memory.hpp"
#include "application_link_trust.hpp"
#include "application_plugin_browser_projection.hpp"
#include "application_plugins_runtime.hpp"
#include "application_settings.hpp"
#include "application_state.hpp"
#include "application_text_form.hpp"
#include "maicord/app/update_log.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/time.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace {
using Kind = PluginBrowserCommandKind;
using Op = PluginJournalOperation;
using Action = PluginBrowserDetailAction;

enum class Stage : u8 { none, preparing, installing, finishing };
enum class DialogKind : u8 { none, install, update, review, uninstall, rollback, remove_provider, update_all, address, add_provider, trust_key, search };
struct Queued {
    Op op;
    u64 owner, provider, version_code, approved;
};
// The running catalog operation; survives the Settings owner.
struct BrowserOperation {
    Stage stage;
    Kind command;
    u64 serial;
    Op op;
    u64 owner, provider, version_code, from_code, approved;
    bool was_enabled, enable_after, delete_data, downgrade, committed, cancelled, cancel_probe;
    u32 queue_count;
    Queued queue[kPluginManagerStateMaximumEntries];
};
struct Pending {
    DialogKind kind;
    Op op;
    u64 owner, provider, version_code, approved, from_code;
    u32 count;
};
// Allocated with Settings > Plugins; the bank is borrowed by the settings scene.
struct BrowserUi {
    PluginBrowserBank bank;
    PluginBrowserUiState state;
    PluginCatalogSnapshot* snapshot;
    bool visible;
    PluginBrowserDialog dialog;
    Pending pending;
    u64 dialog_serial, form_revision;
    bool form_busy, form_complete, accepted, keep_probe;
    u32 form_status_bytes;
    char form_status[256];
    u64 loaded[8], settings_owners[8];
    u64 fingerprint;
    u32 count;
};
PluginBrowserConfig g_config;
bool g_initialized;
PluginBrowserWorkerView g_view;
u64 g_seen_revision, g_seen_revoked, g_seen_snapshot;
bool g_enforce_pending;
PluginStartupGate g_gate;
PluginRevokedPlugin g_revoked;
BrowserOperation g_op;
char g_status[256];
u32 g_status_bytes;
bool g_status_critical;
BrowserUi* g_ui;

u64 now_unix() noexcept { return clock_unix_milliseconds() / 1000; }
void set_status(const Utf8View& text, bool critical = false) noexcept {
    g_status_bytes = utf8_copy_truncated(g_status, text);
    g_status_critical = critical;
}
void set_form_status(Utf8View text, bool busy) noexcept {
    if (!g_ui) return;
    g_ui->form_status_bytes = minimum(text.bytes, static_cast<u32>(sizeof(g_ui->form_status)));
    copy_bytes(g_ui->form_status, text.data, g_ui->form_status_bytes);
    g_ui->form_busy = busy;
    ++g_ui->form_revision;
    application_request_frame();
}
BrowserUi* ensure_ui() noexcept {
    if (g_ui) return g_ui;
    g_ui = static_cast<BrowserUi*>(VirtualAlloc(nullptr, sizeof(BrowserUi), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (g_ui) g_ui->state.tab = g_config.key_count ? PluginBrowserTab::browse : PluginBrowserTab::installed;
    return g_ui;
}
void ensure_worker() noexcept {
    if (!g_initialized || plugin_browser_worker_running()) return;
    PluginBrowserWorkerOptions options{};
    // After the updater's first check (1-3 minutes).
    options.first_check_delay = 180;
    options.first_check_jitter = 180;
    plugin_browser_worker_start(g_config, options);
}
bool command(const PluginBrowserCommand& cmd) noexcept {
    ensure_worker();
    u64 serial = 0;
    if (!plugin_browser_worker_command(cmd, &serial)) return false;
    g_op.serial = serial;
    g_op.command = cmd.kind;
    return true;
}
bool busy() noexcept {
    return g_op.stage != Stage::none || g_op.serial || plugin_browser_worker_busy() || application_plugins_busy();
}
const PluginCatalogSnapshot* snapshot() noexcept { return g_ui ? g_ui->snapshot : nullptr; }
const PluginCatalogEntryView* entry_at(u32 index) noexcept {
    const auto* s = snapshot();
    return s && index < s->entry_count ? &s->entries[index] : nullptr;
}
u32 decode(const PluginCatalogSnapshot& s, const PluginCatalogEntryView& e, PluginIndexText text, char* out, u32 capacity) noexcept {
    const auto decoded = plugin_index_text_decode(plugin_catalog_document(s, e.provider), text, {out, capacity});
    return decoded.ok() ? decoded.bytes : 0;
}
i32 loaded_slot(u64 owner) noexcept {
    const auto* plugins = g_app.plugins;
    for (u32 i = 0; plugins && i < 8; ++i)
        if (application_plugins_present(i) && plugins->modules[i].owner_id == owner) return static_cast<i32>(i);
    return -1;
}
bool owner_enabled(u64 owner) noexcept {
    const i32 slot = loaded_slot(owner);
    return slot >= 0 && g_app.plugins->modules[slot].enabled;
}
Utf8View manager_error_text(const PluginManagerResult& r) noexcept {
    using ME = PluginManagerError;
    switch (r.error) {
    case ME::busy: return utf8_literal("Another plugin change is still finishing. Restart MaiCord and try again.");
    case ME::cache: return utf8_literal("The catalog is out of date. Refresh it, then try again.");
    case ME::not_listed: return utf8_literal("This version is no longer listed.");
    case ME::incompatible: return utf8_literal("This plugin doesn't work with this version of MaiCord.");
    case ME::revoked: return utf8_literal("This version was revoked by its provider.");
    case ME::reserved: return utf8_literal("An official plugin uses this ID, so it can't be installed from this provider.");
    case ME::conflict: return utf8_literal("The provider changed a version it already published. Nothing was installed.");
    case ME::review: return utf8_literal("This version asks for new permissions. Review it first.");
    case ME::capacity: return utf8_literal("Too many plugins are installed. Uninstall one first.");
    case ME::installed: return utf8_literal("This plugin is already installed.");
    case ME::not_installed: return utf8_literal("This plugin isn't installed.");
    case ME::current: return utf8_literal("This version is already installed.");
    case ME::provider: return utf8_literal("This plugin was installed from another source.");
    case ME::fetch:
        return r.fetch_error == UpdateFetchError::rate_limited ? utf8_literal("The plugin host is busy. Try again later.")
                                                               : utf8_literal("Couldn't download the plugin. Check your connection and try again.");
    case ME::integrity: return utf8_literal("The download didn't match the catalog. Nothing was installed.");
    case ME::image: return utf8_literal("The download isn't a 64-bit Windows plugin. Nothing was installed.");
    case ME::rollback: return utf8_literal("The previous version is no longer available.");
    case ME::installer: return utf8_literal("Couldn't finish the plugin change.");
    default: return utf8_literal("Couldn't change your plugins. Check your disk and try again.");
    }
}
Utf8View address_error_text(PluginProviderAddressError error) noexcept {
    using AE = PluginProviderAddressError;
    switch (error) {
    case AE::scheme: return utf8_literal("Use an https:// address.");
    case AE::userinfo: return utf8_literal("Remove the user name from the address.");
    case AE::port: return utf8_literal("Remove the port from the address.");
    case AE::host: return utf8_literal("Use a website name, not an IP address or localhost.");
    case AE::path: return utf8_literal("The address has characters MaiCord doesn't accept.");
    case AE::query: return utf8_literal("Remove the part after ? from the address.");
    case AE::fragment: return utf8_literal("Only a #sha256= key link may follow #.");
    case AE::pin: return utf8_literal("The #sha256= key link needs 64 hex digits.");
    default: return utf8_literal("Enter the provider's full address, like https://example.github.io/plugins/");
    }
}
Utf8View probe_error_text(const PluginProviderProbe& probe) noexcept {
    using PE = PluginProviderProbeError;
    switch (probe.error) {
    case PE::fetch:
        return probe.fetch_error == UpdateFetchError::not_found ? utf8_literal("No plugin catalog was found at this address.")
                                                                : utf8_literal("Couldn't reach this provider. Check the address and your connection.");
    case PE::unreadable: return utf8_literal("This address doesn't serve a plugin catalog.");
    case PE::unsigned_provider: return utf8_literal("This provider doesn't sign its catalog. Unsigned providers need Developer Mode.");
    case PE::document: return utf8_literal("This provider's catalog isn't valid.");
    case PE::pin: return utf8_literal("The signing key doesn't match the link you were given. Don't add this provider.");
    default: return utf8_literal("Couldn't check this provider. Try again.");
    }
}
void refresh() noexcept {
    application_settings_refresh_pages();
    application_request_frame();
}

// ---- operations -------------------------------------------------------------------------------
bool start_operation(Op op, u64 owner, u64 provider, u64 version_code, u64 approved, bool enable_after, u64 from_code) noexcept {
    if (busy()) { set_status(utf8_literal("Wait for the current plugin change to finish."), true); return false; }
    PluginBrowserCommand cmd{};
    cmd.kind = op == Op::uninstall ? Kind::uninstall : Kind::operation;
    cmd.operation = op;
    cmd.owner = owner;
    cmd.provider = provider;
    cmd.version_code = version_code;
    cmd.approved = approved & kNativePluginPermissionsAll;
    cmd.enable_after = enable_after;
    if (!command(cmd)) { set_status(utf8_literal("Wait for the current plugin change to finish."), true); return false; }
    g_op.stage = Stage::preparing;
    g_op.op = op;
    g_op.owner = owner;
    g_op.provider = provider;
    g_op.version_code = version_code;
    g_op.from_code = from_code;
    g_op.approved = approved;
    g_op.enable_after = enable_after;
    g_op.downgrade = op == Op::update && from_code && version_code < from_code;
    g_op.was_enabled = owner_enabled(owner);
    g_op.committed = g_op.cancelled = g_op.delete_data = false;
    set_status(op == Op::uninstall ? utf8_literal("Uninstalling\xE2\x80\xA6") : op == Op::rollback ? utf8_literal("Rolling back\xE2\x80\xA6")
                                                                                                   : utf8_literal("Downloading\xE2\x80\xA6"));
    refresh();
    return true;
}
void start_next() noexcept {
    if (!g_op.queue_count || busy()) return;
    const Queued next = g_op.queue[0];
    for (u32 i = 1; i < g_op.queue_count; ++i) g_op.queue[i - 1] = g_op.queue[i];
    --g_op.queue_count;
    start_operation(next.op, next.owner, next.provider, next.version_code, next.approved, false, 0);
}
bool send_finish() noexcept {
    PluginBrowserCommand cmd{};
    cmd.kind = Kind::finish;
    cmd.delete_data = g_op.delete_data;
    if (!command(cmd)) return false;
    g_op.stage = Stage::finishing;
    return true;
}
wchar_t g_plugins_directory[kNativePluginCatalogPathCapacity];
void begin_installer() noexcept {
    const auto& prepared = g_view.prepared;
    const DataPathResult directory = data_path(g_plugins_directory, kNativePluginCatalogPathCapacity, L"plugins");
    if (g_op.op == Op::uninstall && g_app.plugins) native_plugin_preferences_remove_owner(&g_app.plugins->preferences, g_op.owner);
    const bool started = directory.ok() &&
        application_plugins_install_prepared({prepared.manifest, prepared.manifest_count}, prepared.options,
            g_op.op == Op::uninstall ? g_op.owner : 0, {g_plugins_directory, directory.length});
    if (started) {
        g_op.stage = Stage::installing;
        set_status(g_op.op == Op::uninstall ? utf8_literal("Uninstalling\xE2\x80\xA6") : utf8_literal("Installing\xE2\x80\xA6"));
        return;
    }
    // Settings > Plugins closed meanwhile: an official update applies at the next start.
    if (g_op.op == Op::update && g_op.provider == kPluginProviderOfficial) {
        g_op.stage = Stage::none;
        g_op.queue_count = 0;
        set_status(utf8_literal("The update downloaded. It applies the next time MaiCord starts."));
        return;
    }
    g_op.cancelled = true;
    if (!send_finish()) g_op.stage = Stage::none;
    set_status(utf8_literal("Installation cancelled. Keep Settings open while a plugin installs."), true);
}
void finished() noexcept {
    const auto& r = g_view.manager;
    const auto& settled = g_view.settled;
    g_op.stage = Stage::none;
    application_plugins_reload_trust();
    if (!r.ok()) { set_status(manager_error_text(r), true); g_op.queue_count = 0; return; }
    const bool removed = settled.action == PluginRecoveryAction::finish_uninstall;
    const bool applied = settled.action == PluginRecoveryAction::finish;
    if (!removed && !applied) {
        if (!g_op.cancelled) set_status(utf8_literal("The plugin change didn't finish. Nothing changed."), true);
        return;
    }
    if (removed && g_revoked.owner == g_op.owner) reset_value(&g_revoked);
    set_status(removed ? utf8_literal("Plugin uninstalled.") : g_op.op == Op::install ? utf8_literal("Plugin installed.")
               : g_op.op == Op::rollback ? utf8_literal("Plugin rolled back.") : utf8_literal("Plugin updated."));
    if (g_op.downgrade) {
        PluginBrowserCommand hold{};
        hold.kind = Kind::plugin_flags;
        hold.owner = g_op.owner;
        hold.set = kPluginStateHeld;
        hold.held_code = g_op.from_code;
        command(hold);
    }
    if (!removed && settled.trusted && (g_op.was_enabled || g_op.enable_after)) {
        application_plugins_discover();
        if (!application_plugins_enable_owner(g_op.owner))
            set_status(utf8_literal("Plugin updated. It turns on the next time MaiCord starts."));
    } else if (application_settings_active()) {
        application_plugins_discover();
    }
}
void complete_command() noexcept {
    const Kind kind = g_op.command;
    g_op.serial = 0;
    switch (kind) {
    case Kind::operation:
    case Kind::uninstall:
        if (g_view.manager.ok()) begin_installer();
        else { g_op.stage = Stage::none; g_op.queue_count = 0; set_status(manager_error_text(g_view.manager), true); }
        break;
    case Kind::finish: finished(); break;
    case Kind::probe:
        if (!g_ui || g_ui->pending.kind != DialogKind::address) break;
        if (g_view.address_error != PluginProviderAddressError::none) set_form_status(address_error_text(g_view.address_error), false);
        else if (g_view.probe.error != PluginProviderProbeError::none) set_form_status(probe_error_text(g_view.probe), false);
        else {
            g_ui->form_complete = true;
            ++g_ui->form_revision;
        }
        break;
    case Kind::probe_key:
        if (!g_ui || g_ui->pending.kind != DialogKind::trust_key) break;
        if (g_view.probe.error != PluginProviderProbeError::none) set_status(probe_error_text(g_view.probe), true);
        else g_ui->form_complete = true;
        break;
    case Kind::add_provider:
    case Kind::trust_key:
        if (g_ui) { g_ui->form_complete = true; g_ui->form_busy = false; ++g_ui->form_revision; }
        set_status(g_view.add.error == PluginProviderAddError::none
                       ? (kind == Kind::add_provider ? utf8_literal("Provider added.") : utf8_literal("The new key is trusted."))
                       : utf8_literal("Couldn't save the provider. Try again."),
            g_view.add.error != PluginProviderAddError::none);
        if (g_view.add.error == PluginProviderAddError::none) plugin_browser_worker_check(true, g_view.address.id);
        break;
    case Kind::remove_provider:
        set_status(g_view.record.ok() ? utf8_literal("Provider removed.") : utf8_literal("Couldn't remove the provider. Try again."), !g_view.record.ok());
        break;
    default: break;
    }
}

// ---- dialogs ----------------------------------------------------------------------------------
bool dialog_valid(TextFormOwnerKey key) noexcept {
    return g_ui && key.collection == g_ui->dialog_serial && key.item == static_cast<u64>(g_ui->pending.kind) &&
        key.scope == g_app.plugin_account_scope_serial;
}
ApplicationTextFormProgress dialog_progress(TextFormOwnerKey key) noexcept {
    ApplicationTextFormProgress p{};
    if (!dialog_valid(key)) { p.complete = true; return p; }
    p.status = {g_ui->form_status, g_ui->form_status_bytes};
    p.revision = g_ui->form_revision;
    p.editable = p.can_accept = !g_ui->form_busy;
    p.complete = g_ui->form_complete;
    return p;
}
void dialog_closed(TextFormOwnerKey key) noexcept {
    if (!dialog_valid(key)) return;
    const DialogKind kind = g_ui->pending.kind;
    // A probe document stays in the worker until it is used or dropped.
    if ((kind == DialogKind::address || kind == DialogKind::add_provider || kind == DialogKind::trust_key) && !g_ui->keep_probe)
        g_op.cancel_probe = true;
    if (kind == DialogKind::update_all && !g_ui->accepted) g_op.queue_count = 0;
    reset_value(&g_ui->pending);
}
bool dialog_accept(TextFormOwnerKey key, bool checked) noexcept;
bool open_dialog(DialogKind kind, bool progress) noexcept {
    if (!g_ui || g_app.text_form) return false;
    g_ui->pending.kind = kind;
    g_ui->form_status_bytes = 0;
    g_ui->form_busy = g_ui->form_complete = g_ui->accepted = false;
    const auto& d = g_ui->dialog;
    ApplicationTextFormRequest request{};
    request.title = d.title;
    request.context = d.context;
    request.accept = d.accept;
    request.cancel = d.cancel;
    request.option = d.option;
    request.option_checked = d.option_checked;
    request.danger = d.danger;
    request.confirmation_only = true;
    request.owner = {g_app.plugin_account_scope_serial, ++g_ui->dialog_serial, static_cast<u64>(kind)};
    request.valid = dialog_valid;
    request.save_option = dialog_accept;
    request.progress = progress ? dialog_progress : nullptr;
    request.closed = dialog_closed;
    return application_text_form_open(request);
}
bool address_save(TextFormOwnerKey key, Utf8View text) noexcept {
    if (!dialog_valid(key) || g_ui->form_busy) return false;
    PluginBrowserCommand cmd{};
    cmd.kind = Kind::probe;
    cmd.allow_unsigned = g_app.preferences.developer_mode;
    cmd.text_bytes = minimum(text.bytes, kPluginProviderAddressMaximumBytes);
    copy_bytes(cmd.text, text.data, cmd.text_bytes);
    if (!command(cmd)) { set_form_status(utf8_literal("Wait for the current plugin change to finish."), false); return false; }
    set_form_status(utf8_literal("Checking the provider\xE2\x80\xA6"), true);
    return false;
}
bool open_address_form() noexcept {
    if (!g_ui || g_app.text_form || busy()) return false;
    reset_value(&g_ui->pending);
    g_ui->pending.kind = DialogKind::address;
    g_ui->form_status_bytes = 0;
    g_ui->form_busy = g_ui->form_complete = false;
    ApplicationTextFormRequest request{};
    request.title = utf8_literal("Add a plugin provider");
    request.context = utf8_literal("Enter the address of a plugin catalog, like https://example.github.io/plugins/. "
                                   "A link with #sha256= checks the provider's key for you.");
    request.label = utf8_literal("Provider address");
    request.accept = utf8_literal("Check provider");
    request.owner = {g_app.plugin_account_scope_serial, ++g_ui->dialog_serial, static_cast<u64>(DialogKind::address)};
    request.valid = dialog_valid;
    request.save = address_save;
    request.single_line = true;
    request.maximum_bytes = kPluginProviderAddressMaximumBytes;
    request.maximum_codepoints = kPluginProviderAddressMaximumBytes;
    request.progress = dialog_progress;
    request.closed = dialog_closed;
    return application_text_form_open(request);
}
bool search_save(TextFormOwnerKey key, Utf8View text) noexcept {
    if (!dialog_valid(key)) return false;
    auto& state = g_ui->state;
    state.query_bytes = 0;
    for (u32 i = 0; i < text.bytes;) {
        const auto decoded = utf8_decode_one(text.data + i, text.bytes - i);
        if (!decoded.valid || !decoded.width || decoded.width > kPluginBrowserQueryBytes - state.query_bytes) break;
        for (u32 j = 0; j < decoded.width; ++j) state.query[state.query_bytes++] = text.data[i++];
    }
    while (state.query_bytes && state.query[state.query_bytes - 1] == ' ') --state.query_bytes;
    state.first = 0;
    state.detail = false;
    refresh();
    return true;
}
bool open_search_form() noexcept {
    if (!g_ui || g_app.text_form) return false;
    reset_value(&g_ui->pending);
    g_ui->pending.kind = DialogKind::search;
    ApplicationTextFormRequest request{};
    request.title = utf8_literal("Search plugins");
    request.label = utf8_literal("Name, author or description");
    request.accept = utf8_literal("Search");
    request.initial = {g_ui->state.query, g_ui->state.query_bytes};
    request.owner = {g_app.plugin_account_scope_serial, ++g_ui->dialog_serial, static_cast<u64>(DialogKind::search)};
    request.valid = dialog_valid;
    request.save = search_save;
    request.single_line = true;
    request.maximum_bytes = kPluginBrowserQueryBytes;
    request.maximum_codepoints = kPluginBrowserQueryBytes;
    request.closed = dialog_closed;
    return application_text_form_open(request);
}
// Opens the add-provider (or key change) confirmation from the probe in g_view.
bool open_provider_confirmation(DialogKind kind) noexcept {
    if (!g_ui) return false;
    const auto& probe = g_view.probe;
    const auto& address = g_view.address;
    char name[kPluginProviderNameCapacity]{};
    u32 name_bytes = 0;
    const u8* old = nullptr;
    const PluginCatalogSnapshot* s = snapshot();
    for (u32 i = 0; s && i < s->provider_count; ++i)
        if (s->providers[i].id == address.id) {
            old = s->providers[i].fingerprint;
            name_bytes = s->providers[i].name_bytes;
            copy_bytes(name, s->providers[i].name, name_bytes);
        }
    PluginBrowserProviderFacts facts{};
    facts.host = {address.host, address.host_bytes};
    facts.path = {address.path, address.path_bytes};
    facts.name = {name, name_bytes};
    facts.plugin_count = probe.plugin_count;
    facts.signed_provider = probe.signed_provider;
    facts.pin_matched = probe.pin_matched;
    facts.fingerprint = probe.fingerprint;
    facts.old_fingerprint = kind == DialogKind::trust_key ? old : nullptr;
    plugin_browser_provider_dialog(facts, &g_ui->dialog);
    return open_dialog(kind, true);
}
bool dialog_accept(TextFormOwnerKey key, bool checked) noexcept {
    if (!dialog_valid(key)) return false;
    const Pending p = g_ui->pending;
    if (!plugin_browser_dialog_accepts(g_ui->dialog, checked)) {
        set_form_status(utf8_literal("Check \xE2\x80\x9CI trust this provider\xE2\x80\x9D to continue."), false);
        return false;
    }
    g_ui->accepted = true;
    switch (p.kind) {
    case DialogKind::install: return start_operation(p.op, p.owner, p.provider, p.version_code, p.approved, checked, p.from_code), true;
    case DialogKind::update:
    case DialogKind::rollback: return start_operation(p.op, p.owner, p.provider, p.version_code, p.approved, false, p.from_code), true;
    case DialogKind::review:
        if (checked) {
            PluginBrowserCommand hold{};
            hold.kind = Kind::plugin_flags;
            hold.owner = p.owner;
            hold.set = kPluginStateHeld;
            hold.held_code = p.version_code;
            if (!command(hold)) set_status(utf8_literal("Wait for the current plugin change to finish."), true);
            return true;
        }
        return start_operation(Op::update, p.owner, p.provider, p.version_code, p.approved, false, p.from_code), true;
    case DialogKind::uninstall:
        if (start_operation(Op::uninstall, p.owner, p.provider, 0, 0, false, 0)) g_op.delete_data = checked;
        return true;
    case DialogKind::update_all: start_next(); return true;
    case DialogKind::remove_provider: {
        PluginBrowserCommand cmd{};
        cmd.kind = Kind::remove_provider;
        cmd.provider = p.provider;
        if (busy() || !command(cmd)) { set_status(utf8_literal("Wait for the current plugin change to finish."), true); return true; }
        g_op.queue_count = 0;
        const auto* s = snapshot();
        for (u32 i = 0; checked && s && i < s->installed_count && g_op.queue_count < kPluginManagerStateMaximumEntries; ++i)
            if (s->installed[i].has_state && s->installed[i].provider == p.provider)
                g_op.queue[g_op.queue_count++] = {Op::uninstall, s->installed[i].owner, p.provider, 0, 0};
        return true;
    }
    case DialogKind::add_provider:
    case DialogKind::trust_key: {
        PluginBrowserCommand cmd{};
        cmd.kind = p.kind == DialogKind::add_provider ? Kind::add_provider : Kind::trust_key;
        if (!command(cmd)) { set_form_status(utf8_literal("Wait for the current plugin change to finish."), false); return false; }
        set_form_status(p.kind == DialogKind::add_provider ? utf8_literal("Adding the provider\xE2\x80\xA6") : utf8_literal("Saving the new key\xE2\x80\xA6"), true);
        return false;
    }
    default: return true;
    }
}

// The install or update confirmation for one listed version.
bool open_install(u32 index, u32 version, DialogKind kind) noexcept {
    const auto* s = snapshot();
    const auto* e = entry_at(index);
    if (!s || !e || version >= e->version_count || !g_ui) return false;
    const auto& v = s->versions[e->first_version + version];
    const auto& provider = s->providers[e->provider];
    const auto* local = e->installed_index < s->installed_count ? &s->installed[e->installed_index] : nullptr;
    char name[kNativePluginNameCapacity], publisher[kNativePluginPublisherCapacity];
    PluginBrowserInstallFacts facts{};
    facts.name = {name, decode(*s, *e, e->name, name, sizeof(name))};
    facts.publisher = {publisher, decode(*s, *e, e->publisher, publisher, sizeof(publisher))};
    facts.provider_name = {provider.name, provider.name_bytes};
    facts.host = {provider.host, provider.host_bytes};
    facts.kind = provider.kind;
    facts.version_code = v.code;
    facts.permissions = v.permissions;
    facts.disclosures = e->disclosures;
    facts.downgrade_from = local && local->installed_code > v.code ? local->installed_code : 0;
    facts.update_from = local && local->installed_code < v.code ? local->installed_code : 0;
    plugin_browser_install_dialog(facts, &g_ui->dialog);
    reset_value(&g_ui->pending);
    g_ui->pending.op = local ? Op::update : Op::install;
    g_ui->pending.owner = e->owner;
    g_ui->pending.provider = provider.id;
    g_ui->pending.version_code = v.code;
    g_ui->pending.approved = local ? (local->trusted ? local->approved_permissions | v.permissions : v.permissions) : v.permissions;
    g_ui->pending.from_code = local ? local->installed_code : 0;
    return open_dialog(kind, false);
}
bool update_entry(u32 index) noexcept {
    const auto* s = snapshot();
    const auto* e = entry_at(index);
    if (!s || !e || !e->bound) return false;
    const auto* local = e->installed_index < s->installed_count ? &s->installed[e->installed_index] : nullptr;
    if (!local) return false;
    const auto& v = s->versions[e->first_version + e->update.version];
    const auto& provider = s->providers[e->provider];
    if (e->update.outcome == PluginUpdateOutcome::review) {
        char name[kNativePluginNameCapacity];
        plugin_browser_review_dialog({name, decode(*s, *e, e->name, name, sizeof(name))}, v.code, e->update.permissions, &g_ui->dialog);
        reset_value(&g_ui->pending);
        g_ui->pending.owner = e->owner;
        g_ui->pending.provider = provider.id;
        g_ui->pending.version_code = v.code;
        g_ui->pending.approved = v.permissions;
        g_ui->pending.from_code = local->installed_code;
        return open_dialog(DialogKind::review, false);
    }
    if (e->update.outcome != PluginUpdateOutcome::available) return false;
    // Official, within the approved permissions: one click. Unofficial: always confirmed.
    if (provider.kind == PluginProviderKind::official)
        return start_operation(Op::update, e->owner, provider.id, v.code, local->approved_permissions, false, local->installed_code);
    return open_install(index, e->update.version, DialogKind::update);
}
bool uninstall_owner(u64 owner) noexcept {
    if (!g_ui) return false;
    const auto* s = snapshot();
    const auto* local = plugin_browser_installed(s, owner);
    char name[kNativePluginNameCapacity];
    u32 bytes = 0;
    if (local) { bytes = local->name_bytes; copy_bytes(name, local->name, bytes); }
    else if (g_revoked.owner == owner) { bytes = g_revoked.name_bytes; copy_bytes(name, g_revoked.name, bytes); }
    plugin_browser_uninstall_dialog(bytes ? Utf8View{name, bytes} : utf8_literal("this plugin"), &g_ui->dialog);
    reset_value(&g_ui->pending);
    g_ui->pending.owner = owner;
    g_ui->pending.provider = local ? local->provider : 0;
    return open_dialog(DialogKind::uninstall, false);
}
bool detail_action(Action action) noexcept {
    const auto* s = snapshot();
    const u32 index = g_ui->bank.map.detail_entry;
    const auto* e = entry_at(index);
    if (!s || !e) return false;
    const auto* local = e->installed_index < s->installed_count ? &s->installed[e->installed_index] : nullptr;
    switch (action) {
    case Action::install: return e->newest != kPluginIndexNoVersion && open_install(index, e->newest, DialogKind::install);
    case Action::enable:
        if (!application_plugins_catalog_current()) application_plugins_discover();
        return application_plugins_enable_owner(e->owner);
    case Action::disable: {
        const i32 slot = loaded_slot(e->owner);
        return slot >= 0 && application_plugins_unload(static_cast<u32>(slot));
    }
    case Action::uninstall: return uninstall_owner(e->owner);
    case Action::update:
    case Action::review: return update_entry(index);
    case Action::rollback: {
        if (!local || !local->previous_code) return false;
        char name[kNativePluginNameCapacity];
        plugin_browser_rollback_dialog({name, decode(*s, *e, e->name, name, sizeof(name))}, local->previous_code, local->installed_code,
            &g_ui->dialog);
        reset_value(&g_ui->pending);
        g_ui->pending.op = Op::rollback;
        g_ui->pending.owner = e->owner;
        g_ui->pending.provider = s->providers[e->provider].id;
        g_ui->pending.approved = local->approved_permissions;
        g_ui->pending.from_code = local->installed_code;
        return open_dialog(DialogKind::rollback, false);
    }
    case Action::settings: {
        const auto* registry = application_settings_pages();
        for (u32 i = 0; registry && i < registry->count; ++i)
            if (registry->storage[i].key.owner_id == e->owner) return application_settings_open_plugin_page(registry->storage[i].key);
        return false;
    }
    default: return false;
    }
}
bool update_all() noexcept {
    const auto* s = snapshot();
    if (!s || busy()) return false;
    g_op.queue_count = 0;
    u32 unofficial = 0;
    for (u32 i = 0; i < s->entry_count && g_op.queue_count < kPluginManagerStateMaximumEntries; ++i) {
        const auto& e = s->entries[i];
        if (!e.bound || e.update.outcome != PluginUpdateOutcome::available || e.installed_index >= s->installed_count) continue;
        const auto& local = s->installed[e.installed_index];
        g_op.queue[g_op.queue_count++] = {Op::update, e.owner, s->providers[e.provider].id, s->versions[e.first_version + e.update.version].code,
            local.approved_permissions};
        unofficial += s->providers[e.provider].kind != PluginProviderKind::official ? 1u : 0u;
    }
    if (!g_op.queue_count) return false;
    if (!unofficial) { start_next(); return true; }
    // One confirmation lists every unofficial plugin.
    u32 entries[kPluginManagerStateMaximumEntries];
    u32 count = 0;
    for (u32 q = 0; q < g_op.queue_count; ++q) {
        const u32 index = plugin_browser_find(s, g_op.queue[q].provider, g_op.queue[q].owner);
        if (index != ~0u) entries[count++] = index;
    }
    plugin_browser_update_all_dialog(*s, {entries, count}, &g_ui->dialog);
    reset_value(&g_ui->pending);
    return open_dialog(DialogKind::update_all, false);
}
bool provider_action(u32 slot, u32 choice) noexcept {
    const auto* s = snapshot();
    const auto& map = g_ui->bank.map;
    if (!s || slot >= map.provider_count || choice >= 3) return false;
    const auto& provider = s->providers[map.providers[slot]];
    PluginBrowserCommand cmd{};
    switch (map.provider_actions[slot][choice]) {
    case PluginBrowserProviderAction::refresh: return plugin_browser_worker_check(true, provider.id);
    case PluginBrowserProviderAction::checks:
        cmd.kind = Kind::provider_flags;
        cmd.provider = provider.id;
        (provider.check_daily ? cmd.clear : cmd.set) = kPluginProviderCheckDaily;
        return command(cmd);
    case PluginBrowserProviderAction::remove: {
        u32 installed = 0;
        for (u32 i = 0; i < s->installed_count; ++i) installed += s->installed[i].has_state && s->installed[i].provider == provider.id ? 1u : 0u;
        plugin_browser_remove_provider_dialog(provider.name_bytes ? Utf8View{provider.name, provider.name_bytes}
                                                                  : Utf8View{provider.host, provider.host_bytes}, installed, &g_ui->dialog);
        reset_value(&g_ui->pending);
        g_ui->pending.provider = provider.id;
        return open_dialog(DialogKind::remove_provider, false);
    }
    case PluginBrowserProviderAction::trust_key:
        cmd.kind = Kind::probe_key;
        cmd.provider = provider.id;
        if (busy() || !command(cmd)) return false;
        reset_value(&g_ui->pending);
        g_ui->pending.kind = DialogKind::trust_key;
        g_ui->form_complete = false;
        set_status(utf8_literal("Checking the provider's new key\xE2\x80\xA6"));
        return true;
    default: return false;
    }
}
void enforce_revocations() noexcept {
    for (u32 i = 0; i < g_view.revoked_count && i < kPluginManagerStateMaximumEntries; ++i) {
        const PluginRevokedPlugin& revoked = g_view.revoked[i];
        if (plugin_startup_gate_reason(g_gate, revoked.owner) == 0 && g_gate.count < kPluginManagerStateMaximumEntries) {
            g_gate.owners[g_gate.count] = revoked.owner;
            g_gate.reasons[g_gate.count++] = kPluginGateRevoked;
        } else {
            for (u32 k = 0; k < g_gate.count; ++k) if (g_gate.owners[k] == revoked.owner) g_gate.reasons[k] |= kPluginGateRevoked;
        }
        if (auto* plugins = g_app.plugins) {
            const i32 slot = loaded_slot(revoked.owner);
            if (slot >= 0) {
                if (application_plugins_busy()) return; // retried on the next poll
                application_plugins_unload(static_cast<u32>(slot));
            }
            // plugins.bin already lost this owner; keep the session copy in step.
            native_plugin_preferences_remove_owner(&plugins->preferences, revoked.owner);
        }
        g_revoked = revoked;
        UpdateLog("plugins", "revocation_enforced").hex("owner", revoked.owner).field("loaded", loaded_slot(revoked.owner) >= 0).write(LogLevel::warning);
    }
    g_enforce_pending = false;
    refresh();
}
} // namespace

// ---- public -----------------------------------------------------------------------------------
void application_plugin_browser_initialize() noexcept {
    g_config = application_plugin_browser_build_config();
    g_initialized = true;
    // Users who never touched the catalog get no thread until Settings > Plugins opens.
    wchar_t manager[kPluginManagerPathCapacity];
    if (data_path(manager, kPluginManagerPathCapacity, L"plugin-manager").ok() && GetFileAttributesW(manager) != INVALID_FILE_ATTRIBUTES)
        ensure_worker();
}
void application_plugin_browser_release() noexcept {
    plugin_browser_worker_stop();
    if (g_ui) {
        plugin_catalog_snapshot_release(g_ui->snapshot);
        g_ui->snapshot = nullptr;
    }
}
HANDLE application_plugin_browser_notification() noexcept { return plugin_browser_worker_notification(); }
void application_plugin_browser_poll() noexcept {
    if (g_enforce_pending) enforce_revocations();
    if (g_op.cancel_probe && !g_op.serial && !plugin_browser_worker_busy()) {
        PluginBrowserCommand cancel{};
        cancel.kind = Kind::cancel_probe;
        u64 serial = 0;
        if (plugin_browser_worker_command(cancel, &serial)) g_op.cancel_probe = false;
    }
    if (g_ui && g_ui->form_complete && g_app.text_form) {
        // The probe finished under an open form: replace it with the confirmation.
        const DialogKind kind = g_ui->pending.kind;
        if (kind == DialogKind::address || kind == DialogKind::add_provider || kind == DialogKind::trust_key) {
            // The address form gives way to the confirmation; its probe stays.
            g_ui->keep_probe = kind == DialogKind::address;
            application_text_form_close(false);
            g_ui->keep_probe = false;
            g_ui->form_complete = false;
            if (kind == DialogKind::address) open_provider_confirmation(DialogKind::add_provider);
        }
    } else if (g_ui && g_ui->form_complete && g_ui->pending.kind == DialogKind::trust_key && !g_app.text_form) {
        g_ui->form_complete = false;
        set_status({});
        open_provider_confirmation(DialogKind::trust_key);
    }
    if (!plugin_browser_worker_view(&g_view, g_seen_revision)) {
        if (g_op.stage == Stage::none && !g_op.serial) start_next();
        return;
    }
    g_seen_revision = g_view.revision;
    if (g_view.revoked_serial != g_seen_revoked) {
        g_seen_revoked = g_view.revoked_serial;
        enforce_revocations();
    }
    if (g_view.snapshot_serial != g_seen_snapshot) {
        g_seen_snapshot = g_view.snapshot_serial;
        PluginCatalogSnapshot* s = plugin_browser_worker_take_snapshot();
        if (s && g_ui && g_ui->visible) {
            plugin_catalog_snapshot_release(g_ui->snapshot);
            g_ui->snapshot = s;
        } else {
            plugin_catalog_snapshot_release(s);
        }
    }
    if (g_op.serial && g_view.command_serial == g_op.serial) complete_command();
    if (g_op.stage == Stage::none && !g_op.serial) start_next();
    refresh();
}
void application_plugin_browser_startup() noexcept {
    reset_value(&g_gate);
    auto* paths = static_cast<PluginManagerPaths*>(VirtualAlloc(nullptr, sizeof(PluginManagerPaths), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* work = static_cast<PluginManagerWork*>(VirtualAlloc(nullptr, sizeof(PluginManagerWork), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    wchar_t base[kPluginManagerBaseMaximumCharacters + 1];
    const DataPathResult built = data_path(base, kPluginManagerBaseMaximumCharacters + 1, L"");
    if (paths && work && built.ok() && plugin_manager_paths_build({base, built.length}, paths)) {
        const auto result = plugin_manager_startup(*paths, plugin_index_current_host(), work, &g_gate);
        if (result.rolled_forward) set_status(utf8_literal("A plugin was updated."));
    }
    if (work) VirtualFree(work, 0, MEM_RELEASE);
    if (paths) VirtualFree(paths, 0, MEM_RELEASE);
}
u8 application_plugin_browser_gate(u64 owner) noexcept { return plugin_startup_gate_reason(g_gate, owner); }
void application_plugin_browser_installer_complete(const NativePluginBundleInstallResult& result) noexcept {
    if (g_op.stage != Stage::installing) return;
    g_op.committed = result.committed;
    if (!send_finish()) {
        g_op.stage = Stage::none;
        set_status(utf8_literal("Couldn't finish the plugin change. Restart MaiCord to complete it."), true);
    } else {
        set_status(utf8_literal("Finishing\xE2\x80\xA6"));
    }
}
u32 application_plugin_browser_project(UiSettingsControl** controls, UiSettingsControlPresentation** presentation,
    NativePluginError plugin_error, bool* changed) noexcept {
    BrowserUi* ui = ensure_ui();
    if (!ui || !controls || !presentation || !changed) return 0;
    if (!ui->visible) {
        ui->visible = true;
        ensure_worker();
        plugin_browser_worker_want_snapshot(true);
        if (ui->state.tab == PluginBrowserTab::browse) plugin_browser_worker_browse();
    }
    UiSettingsControl local[kApplicationPluginSettingsMaximumControls]{};
    u32 local_count = 0;
    if (ui->state.tab == PluginBrowserTab::installed)
        local_count = application_plugins_settings_controls({local, kApplicationPluginSettingsMaximumControls},
            {ui->bank.local_text, kApplicationPluginCatalogWindow}, plugin_error);
    u32 loaded = 0, pages = 0;
    for (u32 i = 0; g_app.plugins && i < 8; ++i)
        if (application_plugins_present(i) && g_app.plugins->modules[i].enabled) ui->loaded[loaded++] = g_app.plugins->modules[i].owner_id;
    if (const auto* registry = application_settings_pages())
        for (u32 i = 0; i < registry->count && pages < 8; ++i)
            for (u32 j = 0; j < loaded; ++j)
                if (registry->storage[i].key.owner_id == ui->loaded[j]) {
                    bool known = false;
                    for (u32 k = 0; k < pages; ++k) known = known || ui->settings_owners[k] == ui->loaded[j];
                    if (!known) ui->settings_owners[pages++] = ui->loaded[j];
                }
    PluginBrowserFacts facts{};
    facts.snapshot = ui->snapshot;
    facts.configured = g_config.key_count != 0;
    facts.busy = busy();
    facts.checking = g_view.phase == PluginBrowserPhase::checking;
    facts.now_unix = now_unix();
    facts.checked_unix = g_view.checked_unix;
    facts.status = {g_status, g_status_bytes};
    facts.status_critical = g_status_critical;
    facts.local = {local, local_count};
    facts.inventory = application_plugins_catalog_current();
    facts.loaded = {ui->loaded, loaded};
    facts.settings_owners = {ui->settings_owners, pages};
    facts.gate = &g_gate;
    facts.revoked = g_revoked.owner ? &g_revoked : nullptr;
    facts.offers = g_view.offers;
    const u32 count = plugin_browser_project(ui->state, facts, &ui->bank);
    // Text lives in the bank's arena, so its bytes plus every view's address
    // and length identify the projection.
    u64 fingerprint = hash64_bytes(kHash64Seed, ui->bank.text, ui->bank.used);
    const auto view = [&fingerprint](Utf8View text) noexcept {
        const usize address = reinterpret_cast<usize>(text.data);
        fingerprint = hash64_bytes(fingerprint, &address, sizeof(address));
        fingerprint = hash64_bytes(fingerprint, &text.bytes, sizeof(text.bytes));
    };
    for (u32 i = 0; i < count; ++i) {
        const auto& c = ui->bank.controls[i];
        const u64 scalars[]{c.id, static_cast<u64>(c.kind), c.enabled, c.checked, c.selected_choice, c.choices.count};
        fingerprint = hash64_bytes(fingerprint, scalars, sizeof(scalars));
        fingerprint = hash64_bytes(fingerprint, &ui->bank.presentation[i], sizeof(ui->bank.presentation[i]));
        view(c.label); view(c.description); view(c.action_label);
        for (u32 k = 0; k < c.choices.count; ++k) view(c.choices[k]);
    }
    *changed = fingerprint != ui->fingerprint || count != ui->count;
    ui->fingerprint = fingerprint;
    ui->count = count;
    *controls = ui->bank.controls;
    *presentation = ui->bank.presentation;
    return count;
}
bool application_plugin_browser_activate(u64 id, const UiSettingsActivation& request) noexcept {
    if (!g_ui) return false;
    auto& state = g_ui->state;
    const auto& map = g_ui->bank.map;
    const u32 choice = request.selected_choice;
    bool result = true;
    if (id == kPluginBrowserTabsId) {
        if (choice > 3) return false;
        state.tab = static_cast<PluginBrowserTab>(choice);
        state.detail = false;
        if (g_op.stage == Stage::none) set_status({});
        state.first = state.updates_first = 0;
        if (state.tab == PluginBrowserTab::browse) plugin_browser_worker_browse();
        refresh();
        if (state.tab == PluginBrowserTab::installed && !application_plugins_catalog_current()) application_plugins_discover();
        return true;
    }
    if (id == kPluginBrowserStatusId) result = plugin_browser_worker_check(true);
    else if (id == kPluginBrowserRevokedId) result = g_revoked.owner && uninstall_owner(g_revoked.owner);
    else if (id == kPluginBrowserSearchId) {
        if (choice == 0) return open_search_form();
        state.query_bytes = 0;
        state.first = 0;
    } else if (id == kPluginBrowserSourceId && choice < 3) { state.source = static_cast<PluginBrowserSource>(choice); state.first = 0; }
    else if (id == kPluginBrowserCategoryId && choice <= kPluginIndexCategoryCount) { state.category = choice; state.first = 0; }
    else if (id >= kPluginBrowserRowFirst && id < kPluginBrowserRowFirst + kPluginBrowserRows) {
        const u32 row = static_cast<u32>(id - kPluginBrowserRowFirst);
        const auto* e = row < map.row_count ? entry_at(map.rows[row]) : nullptr;
        if (!e) return false;
        state.detail = true;
        state.detail_owner = e->owner;
        state.detail_provider = g_ui->snapshot->providers[e->provider].id;
    } else if (id == kPluginBrowserPreviousId || id == kPluginBrowserNextId) {
        const bool previous = id == kPluginBrowserPreviousId;
        if (previous && state.first < kPluginBrowserRows) return false;
        state.first = previous ? state.first - kPluginBrowserRows : state.first + kPluginBrowserRows;
    } else if (id == kPluginBrowserBackId) state.detail = false;
    else if (id == kPluginBrowserActionsId) result = choice < map.action_count && detail_action(map.actions[choice]);
    else if (id == kPluginBrowserHomepageId) {
        const auto* s = snapshot();
        const auto* e = entry_at(map.detail_entry);
        char url[kPluginIndexUrlMaximumBytes];
        result = s && e && application_link_open({url, decode(*s, *e, e->homepage, url, sizeof(url))}, false);
    } else if (id >= kPluginBrowserVersionFirst && id < kPluginBrowserVersionFirst + 4) {
        const u32 slot = static_cast<u32>(id - kPluginBrowserVersionFirst);
        result = slot < map.version_count && open_install(map.detail_entry, map.versions[slot], DialogKind::install);
    } else if (id == kPluginBrowserAutoUpdateId) {
        const auto* e = entry_at(map.detail_entry);
        PluginBrowserCommand cmd{};
        cmd.kind = Kind::plugin_flags;
        cmd.owner = e ? e->owner : 0;
        (request.checked ? cmd.clear : cmd.set) = kPluginStateAutoUpdateOff;
        result = e && command(cmd);
    } else if (id == kPluginBrowserUpdatesHeaderId) {
        const bool all = map.update_all && choice == 0;
        result = all ? update_all() : plugin_browser_worker_check(true);
    } else if (id == kPluginBrowserUpdatesToggleId || id == kPluginBrowserOfficialId) {
        PluginBrowserCommand cmd{};
        cmd.kind = Kind::official_flags;
        const u32 flag = id == kPluginBrowserOfficialId ? kPluginOfficialChecksOff : kPluginOfficialAutoUpdateOff;
        (request.checked ? cmd.clear : cmd.set) = flag;
        result = command(cmd);
    } else if (id >= kPluginBrowserUpdateFirst && id < kPluginBrowserUpdateFirst + kPluginBrowserRows) {
        const u32 slot = static_cast<u32>(id - kPluginBrowserUpdateFirst);
        result = slot < map.update_count && update_entry(map.updates[slot]);
    } else if (id == kPluginBrowserUpdatesPreviousId || id == kPluginBrowserUpdatesNextId) {
        const bool previous = id == kPluginBrowserUpdatesPreviousId;
        if (previous && state.updates_first < kPluginBrowserRows) return false;
        state.updates_first = previous ? state.updates_first - kPluginBrowserRows : state.updates_first + kPluginBrowserRows;
    } else if (id >= kPluginBrowserProviderFirst && id < kPluginBrowserProviderFirst + kPluginProviderMaximum) {
        result = provider_action(static_cast<u32>(id - kPluginBrowserProviderFirst), choice);
    } else if (id == kPluginBrowserAddProviderId) {
        return open_address_form();
    } else {
        return false;
    }
    refresh();
    return result;
}
void application_plugin_browser_page_visible(bool visible) noexcept {
    // Showing the page is noticed by its first projection.
    if (visible || !g_ui || !g_ui->visible) return;
    g_ui->visible = false;
    plugin_browser_worker_want_snapshot(false);
    plugin_catalog_snapshot_release(g_ui->snapshot);
    g_ui->snapshot = nullptr;
}
void application_plugin_browser_settings_released() noexcept {
    if (!g_ui) return;
    if (g_app.text_form) application_text_form_cancel_owner(dialog_valid);
    plugin_browser_worker_want_snapshot(false);
    plugin_catalog_snapshot_release(g_ui->snapshot);
    VirtualFree(g_ui, 0, MEM_RELEASE);
    g_ui = nullptr;
}

} // namespace maicord::app_detail
