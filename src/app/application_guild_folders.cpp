#include "application_guild_folders.hpp"
#include "application_account_presentation.hpp"
#include "application_retained_projection.hpp"
#include "application_state.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/protocol/guild_folders.hpp"
#include "maicord/protocol/user_settings_proto.hpp"

#include <Windows.h>

namespace maicord::app_detail {
namespace {
struct ApplicationGuildFolders {
    // What the rail shows: the server layout, or an optimistic edit of it.
    GuildFolderSettings settings;
    // Last layout the server confirmed (READY or USER_SETTINGS_PROTO_UPDATE).
    GuildFolderSettings server;
    u8 partial[kUserSettingsPartialMaximumBytes];
    u64 expanded[kGuildFoldersMaximumFolders];
    u32 expanded_count;
    const AccountUiRuntime* ui;
    u64 account_serial;
    u64 request_id, retry_at_ms;
    u32 attempts;
    bool pending;
    bool dirty;    // settings holds an edit not yet sent
    bool edited;   // settings differs from server (sent or not)
    bool failed;
};
ApplicationGuildFolders* g_folders;

bool motion_allowed() noexcept {
    return !g_app.preferences.reduced_motion && !g_app.renderer.reduced_motion && g_app.window &&
           IsWindowVisible(g_app.window) && !IsIconic(g_app.window);
}
// Folder layout and expansion belong to one account UI lifetime.
ApplicationGuildFolders* owner() noexcept {
    if (!g_app.account_ui)
        return nullptr;
    if (!g_folders) {
        // Fresh pages are zero; clearing them would make the whole owner
        // (two layouts and the edit buffer) resident before any folder exists.
        g_folders = static_cast<ApplicationGuildFolders*>(
            VirtualAlloc(nullptr, sizeof(ApplicationGuildFolders), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!g_folders)
            return nullptr;
        g_folders->ui = g_app.account_ui;
        g_folders->account_serial = g_app.plugin_account_scope_serial;
    }
    if (g_folders->ui != g_app.account_ui || g_folders->account_serial != g_app.plugin_account_scope_serial) {
        // Decommit and recommit instead of clearing: the owner reads as zero
        // again without making its untouched pages resident.
        if (!VirtualFree(g_folders, sizeof(ApplicationGuildFolders), MEM_DECOMMIT)) {
            clear_bytes(g_folders, sizeof(*g_folders));
        } else if (VirtualAlloc(g_folders, sizeof(ApplicationGuildFolders), MEM_COMMIT, PAGE_READWRITE) !=
                   g_folders) {
            // The rail may still point at the old layout; unbind it before releasing.
            auto& view = g_app.account_ui->navigation_view;
            view.guild_folders = nullptr;
            view.expanded_folders = nullptr;
            view.expanded_folder_count = 0;
            VirtualFree(g_folders, 0, MEM_RELEASE);
            g_folders = nullptr;
            return nullptr;
        }
        g_folders->ui = g_app.account_ui;
        g_folders->account_serial = g_app.plugin_account_scope_serial;
    }
    return g_folders;
}
// Copies only the populated prefix of each array: an absent or small layout
// never touches the rest of either settings block.
void copy_settings(GuildFolderSettings& to, const GuildFolderSettings& from) noexcept {
    copy_bytes(to.folders, from.folders, from.folder_count * static_cast<u32>(sizeof(from.folders[0])));
    copy_bytes(to.guild_ids, from.guild_ids, from.guild_id_count * static_cast<u32>(sizeof(from.guild_ids[0])));
    copy_bytes(to.positions, from.positions, from.position_count * static_cast<u32>(sizeof(from.positions[0])));
    to.folder_count = from.folder_count;
    to.guild_id_count = from.guild_id_count;
    to.position_count = from.position_count;
    to.present = from.present;
}
void bind(ApplicationGuildFolders& folders) noexcept {
    auto& view = g_app.account_ui->navigation_view;
    view.guild_folders = folders.settings.present ? &folders.settings : nullptr;
    view.expanded_folders = folders.expanded;
    view.expanded_folder_count = folders.expanded_count;
}
void invalidate(ApplicationGuildFolders& folders) noexcept {
    bind(folders);
    ++g_app.account_ui->navigation_view.folder_revision;
    folders.pending = true;
    application_request_frame();
}
bool rebuild() noexcept {
    return synchronize_navigation_view() && refresh_retained_projection(false);
}
bool online() noexcept {
    return g_app.account_ui && g_app.account.instance && g_app.account.status.gateway_ready &&
           g_app.account.api.account_settings;
}
// The rail as an editable canonical layout (every shown guild listed).
bool editable_layout(ApplicationGuildFolders& folders) noexcept {
    return guild_folders_canonical(folders.settings.present ? &folders.settings : nullptr,
                                   g_app.account_ui->chat_state, &folders.settings) == GuildFolderEditError::none;
}
void restore_server(ApplicationGuildFolders& folders) noexcept {
    copy_settings(folders.settings, folders.server);
    folders.dirty = folders.edited = false;
    invalidate(folders);
}
} // namespace

void application_guild_folders_gateway(const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    if (event.kind != GatewayClientEventKind::dispatch ||
        event.dispatch_kind != GatewayClientDispatchKind::guild_folders || payload.count > kGuildFoldersMaximumGroupBytes)
        return;
    auto* folders = owner();
    if (!folders)
        return;
    if (!payload.count) {
        folders->server.present = false;
    } else if (guild_folders_decode(payload, &folders->server) != GuildFoldersError::none) {
        log_write(LogLevel::warning, "guild_folders", "decode_rejected");
        return;
    }
    // An edit still on its way keeps showing; its echo or failure settles it.
    if (folders->edited)
        return;
    copy_settings(folders->settings, folders->server);
    invalidate(*folders);
}

bool application_guild_folders_synchronize(u64 now_ms) noexcept {
    auto* folders = owner();
    if (!folders)
        return true;
    bind(*folders);
    bool ok = true;
    if (folders->pending) {
        folders->pending = false;
        ok = rebuild();
    }
    if (guild_rail_scene_tick(&g_app.guild_rail_scene, now_ms, motion_allowed())) {
        ok = guild_rail_scene_update(&g_app.guild_rail_scene, &g_app.account_ui->navigation_view,
                                     g_app.guild_rail_scene.viewport, g_app.guild_rail_scene.scroll_dips)
                 .ok() &&
             refresh_retained_projection(false) && ok;
    }
    return ok;
}

u64 application_guild_folders_next_wake(u64 now_ms) noexcept {
    return motion_allowed() ? guild_rail_scene_next_wake(g_app.guild_rail_scene, now_ms) : 0;
}

bool application_guild_folders_toggle(u64 folder_row_id) noexcept {
    auto* folders = owner();
    if (!folders || !guild_folder_row_id(folder_row_id))
        return false;
    bool expanded = false;
    for (u32 i = 0; i < folders->expanded_count; ++i)
        if (folders->expanded[i] == folder_row_id) {
            folders->expanded[i] = folders->expanded[--folders->expanded_count];
            expanded = true;
            break;
        }
    if (!expanded) {
        if (folders->expanded_count >= kGuildFoldersMaximumFolders)
            return false;
        folders->expanded[folders->expanded_count++] = folder_row_id;
    }
    guild_rail_scene_folder_toggled(&g_app.guild_rail_scene, folder_row_id, !expanded,
                                    clock_monotonic_milliseconds(), motion_allowed());
    invalidate(*folders);
    folders->pending = false;
    return rebuild();
}

Utf8View application_guild_rail_label(const GuildViewRow& row, Slice<char> output) noexcept {
    if (!g_app.account_ui || !output.data)
        return {};
    const auto& state = g_app.account_ui->chat_state;
    if (row.folder_role == GuildFolderRole::header) {
        if (!g_folders || !g_folders->settings.present || row.folder_index >= g_folders->settings.folder_count)
            return {};
        return {output.data, guild_folder_display_name(g_folders->settings, row.folder_index, state, output)};
    }
    const auto* guild = chat_state_store_find_guild(state, row.id);
    if (!guild)
        return {};
    const Utf8View name = chat_state_store_guild_name(state, *guild);
    return {output.data, utf8_copy_truncated(output.data, output.count, name)};
}

u64 application_guild_folders_folder_id(const GuildViewRow& row) noexcept {
    if (row.folder_role != GuildFolderRole::header || !g_folders || !g_folders->settings.present ||
        row.folder_index >= g_folders->settings.folder_count || !g_folders->settings.folders[row.folder_index].has_id)
        return 0;
    return g_folders->settings.folders[row.folder_index].id;
}

bool application_guild_folders_folder(u64 folder_id, ApplicationGuildFolder* output) noexcept {
    if (!output || !g_folders || !g_folders->settings.present || !g_app.account_ui)
        return false;
    const u32 index = guild_folders_find_folder(g_folders->settings, folder_id);
    if (index == ~0u)
        return false;
    const auto& record = g_folders->settings.folders[index];
    *output = {};
    output->id = folder_id;
    output->color = record.has_color ? record.color : kGuildFolderDefaultColor;
    output->has_color = record.has_color;
    output->name_bytes = record.name_bytes;
    copy_bytes(output->name, record.name, record.name_bytes);
    output->guild_count = record.guild_count;
    output->placeholder_bytes = static_cast<u8>(guild_folder_display_name(
        g_folders->settings, index, g_app.account_ui->chat_state, {output->placeholder, sizeof(output->placeholder)}));
    return true;
}

GuildFolderEditError application_guild_folders_move(GuildFolderItem source, GuildFolderItem target,
                                                    GuildFolderDrop drop) noexcept {
    auto* folders = owner();
    if (!folders || !online())
        return GuildFolderEditError::input;
    if (!editable_layout(*folders))
        return GuildFolderEditError::capacity;
    const u64 seed = clock_unix_milliseconds() ^ (static_cast<u64>(GetTickCount64()) << 20) ^ source.id;
    const u64 folder_id = drop == GuildFolderDrop::combine && !target.folder
                              ? guild_folders_new_id(folders->settings, seed)
                              : 0;
    const auto moved = guild_folders_move(&folders->settings, source, target, drop, folder_id);
    if (moved != GuildFolderEditError::none) {
        if (!folders->edited)
            copy_settings(folders->settings, folders->server);
        return moved;
    }
    folders->dirty = folders->edited = true;
    folders->failed = false;
    folders->attempts = 0;
    invalidate(*folders);
    return GuildFolderEditError::none;
}

GuildFolderEditError application_guild_folders_edit(u64 folder_id, Utf8View name, u32 color, bool has_color) noexcept {
    auto* folders = owner();
    if (!folders || !online())
        return GuildFolderEditError::input;
    if (!editable_layout(*folders))
        return GuildFolderEditError::capacity;
    const auto edited = guild_folders_edit(&folders->settings, folder_id, name, color, has_color);
    if (edited != GuildFolderEditError::none) {
        if (!folders->edited)
            copy_settings(folders->settings, folders->server);
        return edited;
    }
    folders->dirty = folders->edited = true;
    folders->failed = false;
    folders->attempts = 0;
    invalidate(*folders);
    return GuildFolderEditError::none;
}

bool application_guild_folders_pending(u64 now_ms) noexcept {
    return g_folders && g_folders->ui == g_app.account_ui && g_folders->dirty && !g_folders->request_id &&
           online() && now_ms >= g_folders->retry_at_ms;
}

bool application_guild_folders_submit(u64 request_id, u64 now_ms) noexcept {
    if (!request_id || !application_guild_folders_pending(now_ms))
        return false;
    auto& folders = *g_folders;
    const u32 bytes = guild_folders_encode_settings_patch(folders.settings, {folders.partial, sizeof(folders.partial)});
    if (!bytes) {
        log_write(LogLevel::warning, "guild_folders", "encode_failed");
        folders.failed = true;
        restore_server(folders);
        return false;
    }
    AccountSettingsRequest request{};
    request.operation = AccountSettingsOperation::settings_patch;
    request.payload = {folders.partial, bytes};
    const auto admitted = g_app.account.api.account_settings(g_app.account.instance, &request, request_id);
    SecureZeroMemory(folders.partial, bytes);
    if (!admitted.ok()) {
        folders.retry_at_ms = now_ms + 500;
        return false;
    }
    folders.dirty = false;
    folders.request_id = request_id;
    return true;
}

bool application_guild_folders_owns_response(u64 request_id) noexcept {
    return g_folders && request_id && g_folders->request_id == request_id;
}

bool application_guild_folders_response(const RestClientResponse& response, Utf8View) noexcept {
    if (!application_guild_folders_owns_response(response.request_id))
        return false;
    auto& folders = *g_folders;
    folders.request_id = 0;
    if (folders.ui != g_app.account_ui)
        return true;
    const bool ok = response.transport_ok() && response.status_code >= 200 && response.status_code < 300;
    if (ok) {
        folders.attempts = 0;
        // Later edits are still queued; otherwise the sent layout is current
        // until the server's USER_SETTINGS_PROTO_UPDATE echo replaces it.
        if (!folders.dirty) {
            copy_settings(folders.server, folders.settings);
            folders.edited = false;
        }
        return true;
    }
    if (response.transport_ok() && (response.rate_limited || response.status_code == 429) && ++folders.attempts < 4) {
        folders.dirty = true;
        folders.retry_at_ms = response.retry_at_ms ? response.retry_at_ms : clock_monotonic_milliseconds() + 2000;
        return true;
    }
    log_u64(LogLevel::warning, "guild_folders", "save_failed", response.status_code);
    folders.failed = true;
    restore_server(folders);
    return true;
}

u64 application_guild_folders_request_wake() noexcept {
    if (!g_folders || !g_folders->dirty || g_folders->request_id)
        return 0;
    return g_folders->retry_at_ms ? g_folders->retry_at_ms : 1;
}

void application_guild_folders_release() noexcept {
    if (!g_folders)
        return;
    if (g_app.account_ui && g_app.account_ui->navigation_view.expanded_folders == g_folders->expanded) {
        g_app.account_ui->navigation_view.guild_folders = nullptr;
        g_app.account_ui->navigation_view.expanded_folders = nullptr;
        g_app.account_ui->navigation_view.expanded_folder_count = 0;
    }
    VirtualFree(g_folders, 0, MEM_RELEASE);
    g_folders = nullptr;
}

} // namespace maicord::app_detail
