#include "maicord/core/win32_process.hpp"
#include "application_account_switch.hpp"
#include "application_account_logout.hpp"
#include "application_account_module.hpp"
#include "application_captcha_auth.hpp"
#include "application_login_ui.hpp"
#include "application_plugins.hpp"
#include "application_state.hpp"
#include "application_text_form.hpp"
#include "application_ui_runtime.hpp"
#include "maicord/app/account_credential_forget.hpp"
#include "maicord/app/account_inventory.hpp"
#include "maicord/app/account_logout_stop.hpp"
#include "maicord/app/account_switch.hpp"
#include "maicord/model/asset_hash.hpp"
#include "maicord/ui/login_view.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/win32_error.hpp"

#include <Windows.h>

namespace maicord::app_detail {
namespace {

inline constexpr u32 kConfirmTitleBytes = 200;
inline constexpr u32 kConfirmContextBytes = 240;

struct SwitchOwner {
    AccountInventory inventory;
    AccountSwitch machine;
    AccountLogoutStopOwner stop;
    AccountHost stop_host;
    u8 panel_slots[kUiAccountSwitcherAccounts];
    u32 panel_count;
    u64 observed_user;
    u32 observed_slot;
    u32 observed_revision;
    u32 inventory_revision;
    u32 return_revision;
    RendererLoginStatus return_status;
    u64 confirm_serial;
    u64 confirm_user;
    u64 confirm_revision;
    u32 confirm_slot;
    char confirm_title[kConfirmTitleBytes];
    char confirm_context[kConfirmContextBytes];
    u32 confirm_title_bytes, confirm_context_bytes;
    bool confirm_failed;
    bool initialized, host_transferred, nothing_to_stop, publishing_login, add_mode, return_known;
};
SwitchOwner g_switch{};

u32 selected() noexcept {
    return account_paths_selected_slot();
}

bool busy() noexcept {
    return account_switch_busy(g_switch.machine);
}

bool save_inventory() noexcept {
    ++g_switch.inventory_revision;
    AccountPaths root{};
    wchar_t path[kAccountProfileMaximumPathCharacters];
    const bool saved = account_paths_resolve_slot(0, &root).ok() && account_paths_ensure_directories(root).ok() &&
                       account_inventory_path(root, path, kAccountProfileMaximumPathCharacters) &&
                       account_inventory_save(path, g_switch.inventory).ok();
    if (!saved)
        log_write(LogLevel::warning, "account", "inventory_save_failed");
    return saved;
}

bool slot_profile_present(u32 slot) noexcept {
    AccountPaths paths{};
    if (!account_paths_resolve_slot(slot, &paths).ok())
        return false;
    const DWORD attributes = GetFileAttributesW(paths.profile);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Logout contract on a slot without a worker: marker, delete, confirm. The
// drafts of an unused slot go too, so a new account never inherits them.
bool forget_slot(u32 slot, bool drafts) noexcept {
    AccountPaths paths{};
    if (!account_paths_resolve_slot(slot, &paths).ok())
        return false;
    if (GetFileAttributesW(paths.account_directory) == INVALID_FILE_ATTRIBUTES)
        return win32_error_is_missing_file(thread_last_error());
    const auto begun = account_credential_forget_begin(paths);
    const auto forgotten = begun.ok() ? account_credential_forget_complete_after_join(paths, true) : begun;
    if (!forgotten.ok() || forgotten.pending) {
        log_u64(LogLevel::warning, "account", "saved_account_forget_error", static_cast<u64>(forgotten.error));
        return false;
    }
    if (drafts && !DeleteFileW(paths.drafts) && !win32_error_is_missing_file(thread_last_error()))
        log_write(LogLevel::warning, "account", "saved_account_drafts_remove_failed");
    return true;
}

AccountInventoryEntry* entry_for(u32 slot) noexcept {
    return const_cast<AccountInventoryEntry*>(account_inventory_find_slot(g_switch.inventory, slot));
}

// Most recently used saved account other than `excluded` that can restore.
bool most_recent_saved(u32 excluded, u32* slot) noexcept {
    u8 order[kAccountInventoryMaximumAccounts]{};
    const u32 count = account_inventory_order(g_switch.inventory, order, kAccountInventoryMaximumAccounts);
    for (u32 i = 0; i < count; ++i) {
        const auto& entry = g_switch.inventory.entries[order[i]];
        if (entry.slot != excluded && !(entry.flags & account_inventory_needs_login) &&
            slot_profile_present(entry.slot)) {
            *slot = entry.slot;
            return true;
        }
    }
    return false;
}

// Records the signed-in account in its slot. The same account saved in another
// slot is an older sign-in: that copy is forgotten.
bool record_account(u32 slot, u64 user) noexcept {
    if (!user)
        return false;
    auto& inventory = g_switch.inventory;
    for (u32 i = 0; i < inventory.count;) {
        const auto& other = inventory.entries[i];
        if (other.user_id == user && other.slot != slot) {
            const u32 stale = other.slot;
            if (!forget_slot(stale, true))
                return false;
            account_inventory_remove_slot(&inventory, stale);
            continue;
        }
        ++i;
    }
    const auto* existing = account_inventory_find_slot(inventory, slot);
    AccountInventoryEntry entry = existing && existing->user_id == user ? *existing : AccountInventoryEntry{};
    entry.user_id = user;
    entry.slot = static_cast<u8>(slot);
    entry.flags = static_cast<u8>(entry.flags & ~account_inventory_needs_login);
    return account_inventory_upsert(&inventory, entry) == AccountInventoryError::none;
}

void clear_login_hints() noexcept {
    auto& scene = g_app.login_scene;
    scene.add_account = scene.account_return = scene.switching_accounts = false;
    g_switch.add_mode = false;
    g_switch.return_known = false;
}

// Stops a login owner (QR, password) of the visible sign-in screen and attaches
// the selected slot through the ordinary restore path.
bool attach_selected() noexcept {
    AccountHost& host = g_app.account;
    if (host.instance)
        return false;
    if (host.login_instance) {
        account_host_login_stop(&host, kAccountStopTimeoutMs);
        if (host.login_instance)
            return false;
    }
    application_account_module_release();
    if (host.module)
        return false;
    ++g_app.login.application_auth_epoch;
    g_app.login.webauthn_post_epoch = 0;
    g_app.login.browser_launch_epoch = 0;
    application_captcha_auth_reset();
    secure_edit_clear(&g_app.login.login);
    secure_edit_clear(&g_app.login.password);
    secure_edit_clear(&g_app.login.mfa);
    g_app.login.remote_active = false;
    g_app.login.qr_ready = false;
    g_app.login.remote_status = LoginRemoteStatus::idle;
    g_app.login.remote_restart_at_ms = 0;
    g_app.login.remote_retry_attempt = 0;
    SecureZeroMemory(g_app.login.remote_user, sizeof(g_app.login.remote_user));
    g_app.login.remote_user_bytes = 0;
    g_app.login.reset_mode = false;
    g_app.login.mfa_capabilities = 0;
    clear_login_hints();
    g_app.login_scene.switching_accounts = true;
    application_account_attach_if_present();
    if (host.instance)
        return true;
    g_app.login_scene.switching_accounts = false;
    return false;
}

bool show_login() noexcept {
    g_switch.publishing_login = true;
    const bool shown = application_account_publish_logged_out(false);
    g_switch.publishing_login = false;
    return shown;
}

// ---- Switch effects ----

bool io_stop_begin(void*) noexcept {
    AccountHost& host = g_app.account;
    if (!host.module && !host.instance && !host.login_instance) {
        g_switch.nothing_to_stop = true;
        return true;
    }
    // Quiesce owners while the host still exists; drafts are persisted here.
    application_prepare_account_stop();
    g_switch.stop_host = host;
    g_app.account = {};
    g_switch.host_transferred = true;
    const auto started =
        account_logout_stop_start(&g_switch.stop, &g_switch.stop_host, kAccountStopTimeoutMs,
                                  clock_monotonic_milliseconds(), clock_unix_milliseconds(), account_host_stop);
    if (started.ok())
        return true;
    g_app.account = g_switch.stop_host;
    g_switch.stop_host = {};
    g_switch.host_transferred = false;
    log_u64(LogLevel::warning, "account", "switch_stop_start_error", static_cast<u64>(started.error));
    return false;
}

void restore_transferred_host(const AccountHostStopResult& result) noexcept {
    if (!g_switch.host_transferred)
        return;
    if (!result.ok())
        g_app.account = g_switch.stop_host;
    g_switch.stop_host = {};
    g_switch.host_transferred = false;
}

AccountSwitchStop io_stop_poll(void*) noexcept {
    if (g_switch.nothing_to_stop) {
        g_switch.nothing_to_stop = false;
        return AccountSwitchStop::stopped;
    }
    AccountHostStopResult result{};
    if (!account_logout_stop_take(&g_switch.stop, &result))
        return AccountSwitchStop::pending;
    restore_transferred_host(result);
    application_account_stop_observed(result, true);
    return result.ok() ? AccountSwitchStop::stopped : AccountSwitchStop::failed;
}

void io_reset(void*) noexcept {
    application_release_account_ui(g_app);
    popup_scene_close(&g_app.popup_scene);
    application_plugins_menu_dismissed();
    g_app.popup_owner_kind = PopupOwnerKind::none;
    g_app.message_action_bar.active = false;
    g_app.member_list_scene.active = false;
    g_app.message_scene.active = false;
    g_app.message_composer_scene.active = false;
    message_view_runtime_release(&g_app.message_view);
    if (!message_view_runtime_initialize(
             &g_app.message_view, ui_extent_units(kDefaultUiBootstrapMetrics.message_row_height), 1, true)
             .ok())
        log_write(LogLevel::error, "account", "switch_message_view_failed");
    clear_login_hints();
    g_app.login_scene.switching_accounts = true;
    g_app.login.visible = true;
    g_app.login.status = RendererLoginStatus::restoring_session;
    g_app.login.focus = LoginControl::none;
    g_app.login.remote_restart_at_ms = 0;
    if (!application_refresh_login_view())
        log_write(LogLevel::warning, "ui", "switch_view_refresh_failed");
    application_request_frame();
}

bool io_select(void*, u32 slot) noexcept {
    return account_paths_select_slot(slot);
}

bool io_attach(void*) noexcept {
    return attach_selected();
}

bool io_open_login(void*) noexcept {
    if (!show_login())
        return false;
    g_switch.add_mode = true;
    g_app.login_scene.switching_accounts = false;
    g_app.login_scene.account_return = false;
    g_app.login_scene.add_account = true;
    g_app.login.status = RendererLoginStatus::idle;
    g_app.login.focus = LoginControl::login;
    if (!application_refresh_login_view())
        log_write(LogLevel::warning, "ui", "add_account_view_refresh_failed");
    return true;
}

void io_commit(void*, u32 slot, AccountSwitchKind) noexcept {
    if (!account_inventory_find_slot(g_switch.inventory, slot) ||
        account_inventory_find_slot(g_switch.inventory, slot)->user_id != g_app.account.status.profile_id)
        record_account(slot, g_app.account.status.profile_id);
    account_inventory_touch(&g_switch.inventory, slot, clock_unix_milliseconds());
    save_inventory();
    g_switch.add_mode = false;
    g_app.login_scene.add_account = false;
}

AccountSwitchIo switch_io() noexcept {
    return {nullptr, io_stop_begin, io_stop_poll, io_reset, io_select, io_attach, io_open_login, io_commit};
}

void after_machine() noexcept {
    auto& machine = g_switch.machine;
    if (machine.phase != AccountSwitchPhase::idle || machine.outcome == AccountSwitchOutcome::none)
        return;
    const auto outcome = machine.outcome;
    machine.outcome = AccountSwitchOutcome::none;
    log_u64(LogLevel::info, "account", "switch_outcome", static_cast<u64>(outcome));
    if (outcome == AccountSwitchOutcome::failed) {
        clear_login_hints();
        if (g_app.account.instance)
            application_account_return_to_login(false);
        else if (!g_app.account.login_instance && !show_login())
            log_write(LogLevel::error, "account", "switch_login_unavailable");
    }
    application_request_frame();
}

bool begin(AccountSwitchKind kind, u32 target) noexcept {
    if (!account_switch_begin(&g_switch.machine, switch_io(), kind, selected(), target))
        return false;
    // A host-less stop completes at once; a worker stop wakes the loop by handle.
    application_account_switch_pump();
    return true;
}

bool begin_add(u32 slot) noexcept {
    if (!application_account_switch_available())
        return false;
    const auto* existing = account_inventory_find_slot(g_switch.inventory, slot);
    if (!existing && g_switch.inventory.count >= kAccountInventoryMaximumAccounts)
        return false;
    // Leftovers of an unused (or token-less) slot never reach the new sign-in.
    if (!forget_slot(slot, !existing))
        return false;
    return begin(AccountSwitchKind::add_account, slot);
}

bool begin_switch(u32 slot) noexcept {
    if (!application_account_switch_available())
        return false;
    AccountPaths target{};
    if (!account_paths_resolve_slot(slot, &target).ok())
        return false;
    const auto probe = account_credential_forget_probe(target);
    if (!probe.ok() || probe.pending || !slot_profile_present(slot)) {
        // An interrupted logout or a missing token: finish forgetting it and
        // ask for that account's sign-in instead.
        if (auto* entry = entry_for(slot)) {
            entry->flags |= account_inventory_needs_login;
            save_inventory();
        }
        return begin_add(slot);
    }
    return begin(AccountSwitchKind::switch_account, slot);
}

// ---- Log out of a saved account that is not active ----

TextFormOwnerKey confirm_key() noexcept {
    return {g_switch.confirm_serial, g_switch.confirm_slot, g_switch.confirm_user};
}

bool confirm_valid(TextFormOwnerKey key) noexcept {
    if (!key.scope || key.scope != g_switch.confirm_serial || key.collection == selected() || busy() ||
        application_account_logout_blocked())
        return false;
    const auto* entry = account_inventory_find_slot(g_switch.inventory, static_cast<u32>(key.collection));
    return entry && entry->user_id == key.item;
}

bool confirm_save(TextFormOwnerKey key, Utf8View) noexcept {
    if (!confirm_valid(key))
        return false;
    const u32 slot = static_cast<u32>(key.collection);
    if (!forget_slot(slot, false)) {
        g_switch.confirm_failed = true;
        ++g_switch.confirm_revision;
        return false;
    }
    account_inventory_remove_slot(&g_switch.inventory, slot);
    save_inventory();
    log_write(LogLevel::info, "account", "saved_account_logged_out");
    return true;
}

ApplicationTextFormProgress confirm_progress(TextFormOwnerKey key) noexcept {
    if (key.scope != g_switch.confirm_serial)
        return {{}, {}, 0, false, false, true, true, true};
    return {g_switch.confirm_failed ? utf8_literal("Couldn't log out. Try again.") : Utf8View{},
            utf8_literal("Log out"),
            g_switch.confirm_revision,
            false,
            true,
            false,
            true,
            true};
}

u32 append(char* output, u32 used, u32 capacity, Utf8View text) noexcept {
    return used + utf8_copy_truncated(output + used, capacity - used, text);
}

bool open_forget_confirmation(const AccountInventoryEntry& entry) noexcept {
    ++g_switch.confirm_serial;
    if (!g_switch.confirm_serial)
        ++g_switch.confirm_serial;
    g_switch.confirm_slot = entry.slot;
    g_switch.confirm_user = entry.user_id;
    g_switch.confirm_failed = false;
    g_switch.confirm_revision = 1;
    const Utf8View display = account_inventory_display_name(entry);
    const Utf8View username = account_inventory_username(entry);
    const Utf8View name = display.bytes ? display : username.bytes ? username : utf8_literal("this account");
    u32 title = append(g_switch.confirm_title, 0, kConfirmTitleBytes - 1, utf8_literal("Log out of "));
    title = append(g_switch.confirm_title, title, kConfirmTitleBytes - 1, name);
    title = append(g_switch.confirm_title, title, kConfirmTitleBytes, utf8_literal("?"));
    u32 context =
        append(g_switch.confirm_context, 0, kConfirmContextBytes, utf8_literal("You'll have to log in to "));
    context = append(g_switch.confirm_context, context, kConfirmContextBytes - 12, username.bytes ? username : name);
    context = append(g_switch.confirm_context, context, kConfirmContextBytes, utf8_literal(" again later."));
    g_switch.confirm_title_bytes = title;
    g_switch.confirm_context_bytes = context;
    ApplicationTextFormRequest request{};
    request.title = {g_switch.confirm_title, title};
    request.context = {g_switch.confirm_context, context};
    request.accept = utf8_literal("Log out");
    request.owner = confirm_key();
    request.valid = confirm_valid;
    request.save = confirm_save;
    request.confirmation_only = true;
    request.progress = confirm_progress;
    request.danger = true;
    return application_text_form_open(request);
}

// ---- Observation ----

// Keeps the active slot's public identity current once READY names the user.
void observe_running() noexcept {
    if (!g_app.account.instance || !g_app.account_ui || g_app.account.stop_failed)
        return;
    const u64 self = g_app.account_ui->chat_state.self_user_id;
    if (!self || self != g_app.account.status.profile_id)
        return;
    const auto& users = g_app.account_ui->user_store;
    const auto* user = user_store_find(users, self);
    const u32 slot = selected();
    if (!user || (g_switch.observed_user == self && g_switch.observed_slot == slot &&
                  g_switch.observed_revision == user->revision))
        return;
    g_switch.observed_user = self;
    g_switch.observed_slot = slot;
    g_switch.observed_revision = user->revision;
    bool changed = false;
    const auto* existing = account_inventory_find_slot(g_switch.inventory, slot);
    if (!existing || existing->user_id != self || (existing->flags & account_inventory_needs_login)) {
        if (!record_account(slot, self))
            return;
        if (!existing)
            account_inventory_touch(&g_switch.inventory, slot, clock_unix_milliseconds());
        changed = true;
    }
    auto* entry = entry_for(slot);
    if (!entry)
        return;
    changed = account_inventory_set_identity(entry, user_store_name(users, *user), user_store_username(users, *user),
                                             user_store_avatar_hash(*user)) ||
              changed;
    if (g_switch.inventory.active_slot != slot) {
        g_switch.inventory.active_slot = slot;
        changed = true;
    }
    if (changed)
        save_inventory();
}

bool login_escape_reserved() noexcept {
    const auto& login = g_app.login;
    return login.reset_mode || login_requires_back(login.status, login.remote_status) ||
           login_phone_mode(login.status) || login_recovery_mode(login.status) ||
           login.status == RendererLoginStatus::restoring_session ||
           login.status == RendererLoginStatus::cancelling ||
           (login.webauthn_attempted && login.status == RendererLoginStatus::submitting);
}

// A rejected token marks its entry; an idle sign-in screen offers Esc back
// to another saved account.
void observe_signed_out() noexcept {
    if (g_app.account.instance || !g_app.login.visible || application_account_logout_blocked() ||
        application_account_logout_host_owned())
        return;
    const u32 slot = selected();
    if (g_app.login.status == RendererLoginStatus::session_expired) {
        auto* entry = entry_for(slot);
        if (entry && !(entry->flags & account_inventory_needs_login) && !slot_profile_present(slot)) {
            entry->flags |= account_inventory_needs_login;
            save_inventory();
        }
    }
    if (g_switch.return_known && g_switch.return_revision == g_switch.inventory_revision &&
        g_switch.return_status == g_app.login.status)
        return;
    g_switch.return_known = true;
    g_switch.return_revision = g_switch.inventory_revision;
    g_switch.return_status = g_app.login.status;
    u32 other = 0;
    const bool offer = !g_switch.add_mode && !login_escape_reserved() && most_recent_saved(slot, &other);
    if (g_app.login_scene.account_return != offer) {
        g_app.login_scene.account_return = offer;
        application_refresh_login_view();
    }
}

void ensure_user_media(const AccountInventoryEntry& entry) noexcept {
    if (!g_app.account_ui)
        return;
    auto& users = g_app.account_ui->user_store;
    const Utf8View display = account_inventory_display_name(entry);
    const Utf8View username = account_inventory_username(entry);
    if (user_store_find(users, entry.user_id) || (!display.bytes && !username.bytes))
        return;
    // Another saved account's avatar loads through the same user media path.
    if (user_store_upsert(&users, entry.user_id, display.bytes ? display : username, username).ok() &&
        entry.avatar_hash_bytes)
        user_store_set_avatar(&users, entry.user_id, account_inventory_avatar_hash(entry));
}

} // namespace

void application_account_switch_initialize() noexcept {
    if (g_switch.initialized)
        return;
    g_switch.initialized = true;
    AccountPaths root{};
    if (!account_paths_resolve_slot(0, &root).ok())
        return;
    const auto opened = account_inventory_open(root, &g_switch.inventory, clock_unix_milliseconds());
    if (!opened.ok()) {
        log_u64(LogLevel::warning, "account", "inventory_error", static_cast<u64>(opened.error));
        g_switch.inventory = {};
    }
    if (opened.migrated)
        log_write(LogLevel::info, "account", "inventory_migrated");
    u32 slot = 0;
    if (account_inventory_find_slot(g_switch.inventory, g_switch.inventory.active_slot)) {
        slot = g_switch.inventory.active_slot;
    } else if (g_switch.inventory.count) {
        u8 order[kAccountInventoryMaximumAccounts]{};
        account_inventory_order(g_switch.inventory, order, kAccountInventoryMaximumAccounts);
        slot = g_switch.inventory.entries[order[0]].slot;
    }
    account_paths_select_slot(slot);
    log_u64(LogLevel::info, "account", "saved_accounts", g_switch.inventory.count);
}

bool application_account_switch_available() noexcept {
    return g_switch.initialized && !busy() && !application_account_logout_blocked() &&
           !application_account_logout_host_owned() && g_app.account.instance && !g_app.account.stop_failed &&
           g_app.account_ui && g_app.account_ui->chat_state.self_user_id && !g_app.login.visible;
}

bool application_account_switch_panel(UiAccountSwitcherModel* model, u64 owner_id,
                                      UiAccountSwitcherPanel panel) noexcept {
    observe_running();
    UiAccountSwitcherAccount accounts[kUiAccountSwitcherAccounts]{};
    u8 order[kAccountInventoryMaximumAccounts]{};
    const u32 count = account_inventory_order(g_switch.inventory, order, kAccountInventoryMaximumAccounts);
    const u32 active = selected();
    for (u32 i = 0; i < count; ++i) {
        const auto& entry = g_switch.inventory.entries[order[i]];
        const Utf8View hash = account_inventory_avatar_hash(entry);
        ensure_user_media(entry);
        accounts[i] = {entry.user_id,
                       account_inventory_display_name(entry),
                       account_inventory_username(entry),
                       user_avatar_variant(hash),
                       asset_hash_animated(hash),
                       entry.slot == active,
                       (entry.flags & account_inventory_needs_login) != 0};
        g_switch.panel_slots[i] = entry.slot;
    }
    g_switch.panel_count = count;
    return ui_account_switcher_build(model, owner_id, panel, {accounts, count},
                                     count < kAccountInventoryMaximumAccounts);
}

bool application_account_switch_action(UiAccountSwitcherPanel panel, u32 action) noexcept {
    if (action == static_cast<u32>(UiAccountSwitcherAction::add_account)) {
        u32 slot = 0;
        return account_inventory_free_slot(g_switch.inventory, &slot) && begin_add(slot);
    }
    const u32 index = ui_account_switcher_account_index(action);
    if (index >= g_switch.panel_count)
        return false;
    const u32 slot = g_switch.panel_slots[index];
    const auto* entry = account_inventory_find_slot(g_switch.inventory, slot);
    if (!entry)
        return false;
    if (panel == UiAccountSwitcherPanel::manage_accounts)
        return slot == selected() ? application_account_logout_open() : open_forget_confirmation(*entry);
    if (slot == selected())
        return true;
    if (entry->flags & account_inventory_needs_login)
        return begin_add(slot);
    return begin_switch(slot);
}

HANDLE application_account_switch_notification() noexcept {
    return account_logout_stop_owned(g_switch.stop) ? account_logout_stop_notification(g_switch.stop) : nullptr;
}

void application_account_switch_pump() noexcept {
    if (!g_switch.initialized)
        return;
    auto& machine = g_switch.machine;
    if (machine.phase == AccountSwitchPhase::stopping) {
        if (account_switch_pump(&machine, switch_io()))
            after_machine();
    } else if (machine.phase == AccountSwitchPhase::login && g_app.account.instance) {
        account_switch_login_finished(&machine, switch_io(), true);
        after_machine();
    }
    if (!busy()) {
        observe_running();
        observe_signed_out();
    }
    if (!g_app.login.visible &&
        (g_app.login_scene.switching_accounts || g_app.login_scene.add_account || g_app.login_scene.account_return))
        clear_login_hints();
}

bool application_account_switch_login_escape() noexcept {
    if (!g_switch.initialized || !g_app.login.visible || application_account_logout_blocked() ||
        application_account_logout_host_owned() || login_escape_reserved())
        return false;
    auto& machine = g_switch.machine;
    if (machine.phase == AccountSwitchPhase::login) {
        account_switch_login_finished(&machine, switch_io(), false);
        after_machine();
        return true;
    }
    if (busy() || !g_app.login_scene.account_return || g_app.account.instance)
        return false;
    const u32 current = selected();
    u32 slot = 0;
    if (!most_recent_saved(current, &slot) || !account_paths_select_slot(slot))
        return false;
    if (attach_selected()) {
        account_inventory_touch(&g_switch.inventory, slot, clock_unix_milliseconds());
        save_inventory();
        return true;
    }
    account_paths_select_slot(current);
    if (!g_app.account.login_instance && !show_login())
        log_write(LogLevel::error, "account", "switch_login_unavailable");
    return true;
}

bool application_account_switch_after_logout() noexcept {
    if (!g_switch.initialized || g_switch.publishing_login || busy())
        return false;
    const u32 slot = selected();
    if (slot_profile_present(slot))
        return false;
    if (account_inventory_remove_slot(&g_switch.inventory, slot))
        save_inventory();
    u32 next = 0;
    if (!most_recent_saved(slot, &next))
        return false;
    log_write(LogLevel::info, "account", "logout_continues_with_saved_account");
    account_paths_select_slot(next);
    io_reset(nullptr);
    if (attach_selected()) {
        account_inventory_touch(&g_switch.inventory, next, clock_unix_milliseconds());
        save_inventory();
        return true;
    }
    account_paths_select_slot(slot);
    clear_login_hints();
    return false;
}

bool application_account_switch_shutdown() noexcept {
    if (!account_logout_stop_owned(g_switch.stop))
        return true;
    AccountHostStopResult result{};
    if (!account_logout_stop_join_for_shutdown(&g_switch.stop, kAccountStopTimeoutMs, &result))
        return false;
    restore_transferred_host(result);
    application_account_stop_observed(result, true);
    g_switch.machine = {};
    return true;
}

} // namespace maicord::app_detail
