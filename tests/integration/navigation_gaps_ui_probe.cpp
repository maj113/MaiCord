#include "navigation_gaps_ui_probe.hpp"
#include "probe_present.hpp"
#include "../../src/render/d3d11_renderer_interaction_state.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/guild_rail_scene.hpp"

namespace maicord {
namespace {
bool fail(u32 stage) noexcept {
    log_u64(LogLevel::error, "probe", "navigation_gaps_stage", stage);
    return false;
}
bool present(D3D11Renderer& renderer, bool (*sync)() noexcept) noexcept {
    renderer.retained_presentation.valid = false;
    return sync() && probe_present(&renderer, kDefaultTheme);
}
} // namespace

bool navigation_gaps_ui_probe(D3D11Renderer& renderer, NavigationView* view, const ChatStateStore& state,
                              bool (*sync)() noexcept, bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (!view || !sync || !renderer.message_scene)
        return fail(1);
    // Unread bar and jump bar over the live message scene.
    static UiUnreadBannerScene banner;
    banner = {};
    char label[kUiUnreadBannerLabelBytes];
    const u32 label_bytes = ui_unread_banner_format_label(12, false, utf8_literal("3:04 PM"), {label, sizeof(label)});
    const UiUnreadBannerView unread{state.selected_channel_id ? state.selected_channel_id : 1, 1, 12, false, true, true};
    const auto& messages = *renderer.message_scene;
    if (!ui_unread_banner_update(&banner, unread, {label, label_bytes}, messages.viewport,
                                 {&messages.widgets, messages.root}, 1000, false))
        return fail(2);
    renderer.unread_banner_scene = &banner;
    const bool banner_drawn = present(renderer, sync) && d3d11_renderer_unread_banner_visible(renderer);
    log_u64(LogLevel::info, "probe", "navigation_unread_banner_visible", banner_drawn);
    if (!banner_drawn || (capture && !capture(L"navigation-unread-banner.bmp")))
        return fail(3);
    renderer.unread_banner_scene = nullptr;

    // One folder holding the first two guilds, collapsed then expanded.
    if (state.guild_count < 2 || state.guild_count + 1 > view->guild_capacity)
        return present(renderer, sync) || fail(4);
    static GuildFolderSettings folders;
    folders = {};
    folders.present = true;
    folders.folder_count = 1;
    folders.guild_id_count = 2;
    folders.guild_ids[0] = state.guilds[state.guild_order[0]].id;
    folders.guild_ids[1] = state.guilds[state.guild_order[1]].id;
    auto& folder = folders.folders[0];
    folder = {7, 0x3BA55C, 0, 2, 5, true, true, {}};
    copy_bytes(folder.name, "Games", 5);
    view->guild_folders = &folders;
    ++view->folder_revision;
    bool ok = navigation_view_rebuild(view, state).ok() && present(renderer, sync) &&
              view->guild_rows[0].folder_role == GuildFolderRole::header &&
              (!capture || capture(L"navigation-guild-folder-collapsed.bmp"));
    const u64 expanded[] = {view->guild_rows[0].id};
    view->expanded_folders = expanded;
    view->expanded_folder_count = 1;
    ++view->folder_revision;
    ok = ok && navigation_view_rebuild(view, state).ok() && present(renderer, sync) &&
         (!capture || capture(L"navigation-guild-folder-expanded.bmp"));
    view->guild_folders = nullptr;
    view->expanded_folders = nullptr;
    view->expanded_folder_count = 0;
    ++view->folder_revision;
    ok = navigation_view_rebuild(view, state).ok() && present(renderer, sync) && ok;
    return ok || fail(5);
}

} // namespace maicord
