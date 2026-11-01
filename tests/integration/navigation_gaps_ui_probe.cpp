#include "navigation_gaps_ui_probe.hpp"
#include "probe_present.hpp"
#include "../../src/render/d3d11/renderer_interaction_state.hpp"
#include "../support/model_store_fixtures.hpp"
#include "../support/ui_scene_fixtures.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/guild/folders.hpp"
#include "maicord/ui/voice/channel_rows.hpp"
#include "maicord/model/voice/channel_info_store.hpp"
#include "maicord/ui/guild/hidden_channel_menu.hpp"

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

    // A voice row with a session timer, a status and "Invite to Voice".
    static VoiceChannelInfoStore info;
    static UiVoiceChannelExtras extras;
    voice_channel_info_store_reset(&info);
    extras = {};
    u64 voice = 0;
    for (u32 i = 0; i < view->channel_count && !voice; ++i)
        if (view->channel_rows[i].type == 2)
            voice = view->channel_rows[i].id;
    constexpr u64 now_s = 1'790'000'000;
    if (!voice || !voice_channel_info_store_set_start(&info, state.selected_guild_id, voice, now_s - 3'723) ||
        !voice_channel_info_store_set_status(&info, state.selected_guild_id, voice,
                                             utf8_literal("Pairing on the glyph atlas <:wave:123>")))
        return fail(6);
    extras.info = &info;
    extras.invite_channel_id = voice;
    extras.clock_s = now_s;
    extras.revision = 1;
    const UiVoiceChannelExtras* previous_extras = view->voice_extras;
    view->voice_extras = &extras;
    const bool extras_drawn = navigation_view_refresh_voice(view) && present(renderer, sync);
    const auto* row = navigation_view_find_channel(*view, voice);
    const bool drawn = extras_drawn && row && row->voice_subtitle && row->voice_invite;
    log_u64(LogLevel::info, "probe", "navigation_voice_extras_drawn", drawn);
    const bool captured = !capture || capture(L"navigation-voice-channels.bmp");
    view->voice_extras = previous_extras;
    navigation_view_refresh_voice(view);
    if (!drawn || !captured)
        return fail(6);

    // "Show hidden channels": a channel the account cannot view lists locked under its category, then goes again.
    u64 hidden_id = 0;
    for (u32 i = 0; i < state.channel_count && !hidden_id; ++i) {
        const ChannelRecord& candidate = state.channels[state.channel_order[i]];
        if (candidate.guild_id == state.selected_guild_id && chat_state_store_channel_hidden(state, candidate))
            hidden_id = candidate.id;
    }
    if (hidden_id) {
        view->show_hidden = true;
        bool locked_ok = navigation_view_rebuild(view, state).ok() && present(renderer, sync);
        const auto* locked_row = navigation_view_find_channel(*view, hidden_id);
        locked_ok = locked_ok && locked_row && locked_row->locked && !locked_row->selectable &&
                    locked_row->actions == 0 && (!capture || capture(L"navigation-hidden-channels.bmp"));
        // Its explanation popup, built from the cached permissions alone.
        if (locked_ok) {
            static char popup_text[kHiddenChannelMenuTextBytes];
            TextBank bank{popup_text, sizeof(popup_text), 0};
            PopupItem items[kHiddenChannelMenuRows]{};
            PopupItem storage[16]{};
            PopupRowWidget popup_rows[16]{};
            UiWidget widgets[34]{};
            UiLayoutNode nodes[34]{};
            PopupScene menu{};
            const auto* original_popup = renderer.popup_scene;
            const auto* channel = chat_state_store_find_channel(state, hidden_id);
            const u32 count = channel && renderer.user_store
                                  ? hidden_channel_menu_build(state, *renderer.user_store, *channel, 1'790'000'000,
                                                              &bank, {items, kHiddenChannelMenuRows})
                                  : 0;
            const UiRect viewport{0, 0, static_cast<float>(renderer.width) * 96.f / renderer.dpi,
                                  static_cast<float>(renderer.height) * 96.f / renderer.dpi};
            const UiRect anchor{viewport.left + 160, viewport.top + 200, viewport.left + 161, viewport.top + 201};
            locked_ok = count != 0 &&
                        popup_scene_initialize(&menu, {storage, 16}, {popup_rows, 16}, {widgets, 34}, {nodes, 34}) &&
                        popup_scene_open(&menu, PopupKind::context_menu, hidden_id, {items, count}, anchor, viewport,
                                         264.0f)
                            .ok() &&
                        renderer_bind_popup_scene(&renderer, &menu) &&
                        renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
                        probe_present(&renderer, kDefaultTheme) &&
                        (!capture || capture(L"navigation-hidden-channel-info.bmp"));
            renderer_bind_popup_scene(&renderer, original_popup);
            popup_scene_release(&menu);
        }
        view->show_hidden = false;
        locked_ok = navigation_view_rebuild(view, state).ok() && present(renderer, sync) &&
                    !navigation_view_find_channel(*view, hidden_id) && locked_ok;
        log_u64(LogLevel::info, "probe", "navigation_hidden_channels_drawn", locked_ok);
        if (!locked_ok)
            return fail(7);
    }

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
