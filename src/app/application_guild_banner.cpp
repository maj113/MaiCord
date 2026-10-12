#include "application_guild_banner.hpp"
#include "application_shell_input.hpp"
#include "application_state.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/guild_banner.hpp"
#include "maicord/ui/message_rich_layout.hpp"

#include <Windows.h>

namespace maicord::app_detail {
namespace {
struct BannerState {
    UiGuildBannerAnimation animation;
    UiGuildBannerView view;
    UiGuildImageMedia still, moving;
    u64 signature;
    MediaAssetKey retired, playing;
    float pointer_x, pointer_y;
    u32 revision;
    bool shown, visible, animate, gif, pointer_inside, header_hovered;
};
BannerState g_banner;

bool reduced_motion() noexcept {
    const auto& motion = g_app.surface_motion;
    return g_app.preferences.reduced_motion || g_app.renderer.reduced_motion ||
           (motion.system_preference_cached && !motion.client_area_animation);
}
const UiNode* shell_node(UiSemantic semantic) noexcept {
    return ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, semantic));
}
const GuildRecord* selected_guild() noexcept {
    if (!g_app.account_ui || g_app.login.visible || g_app.shell_scene.view != UiShellView::guild_channel)
        return nullptr;
    const auto& state = g_app.account_ui->chat_state;
    const auto* guild = state.selected_guild_id ? chat_state_store_find_guild(state, state.selected_guild_id) : nullptr;
    return guild && !guild->unavailable && chat_state_store_guild_banner_hash(*guild).bytes ? guild : nullptr;
}
bool inside(UiRect r) noexcept {
    return g_banner.pointer_inside && ui_rect_has_area(r) && ui_rect_contains(r, g_banner.pointer_x, g_banner.pointer_y);
}
void retire(MediaAssetKey key) noexcept {
    if (media_asset_key_valid(key))
        g_banner.retired = key;
}
} // namespace

void application_guild_banner_project(u64 now_ms) noexcept {
    auto& b = g_banner;
    const GuildRecord* guild = selected_guild();
    const u64 guild_id = guild ? guild->id : 0;
    const Utf8View hash = guild ? chat_state_store_guild_banner_hash(*guild) : Utf8View{};
    const auto* sidebar = shell_node(UiSemantic::channel_sidebar);
    const auto* header = shell_node(UiSemantic::channel_header);
    auto& scene = g_app.shell_navigation_scene;
    const bool shown = guild_id && sidebar && header;
    const float scroll = scene.active ? scene.scroll_dips : 0.f;
    UiGuildBannerGeometry geometry{};
    if (shown)
        geometry = ui_guild_banner_geometry(sidebar->rectangle, header->rectangle, scroll, false);
    const bool header_hovered = shown && geometry.visible && inside(geometry.header);
    const bool eligible = shown && ui_guild_banner_animated_hash(hash) &&
                          (guild->upload_features & guild_record_animated_banner) && !reduced_motion();
    const bool animate =
        ui_guild_banner_animate(&b.animation, guild_id, eligible, shown && geometry.visible, inside(geometry.hover),
                                message_rich_display().gifs == MessageMediaAnimation::always, now_ms);
    if (guild_id != b.view.guild_id)
        b.gif = false;
    const u32 dpi = g_app.renderer.dpi ? g_app.renderer.dpi : 96;
    bool media = shown && ui_guild_banner_media(guild_id, hash, dpi, false, false, &b.still);
    if (!media)
        b.still = {};
    if (!(eligible && media && ui_guild_banner_media(guild_id, hash, dpi, true, b.gif, &b.moving)))
        b.moving = {};
    // A stopped animation (or one of a server no longer shown) gives its
    // playback back; the still stays resident.
    const MediaAssetKey playing = animate && media_asset_key_valid(b.moving.key) ? b.moving.key : MediaAssetKey{};
    if (media_asset_key_valid(b.playing) && !media_asset_key_equal(b.playing, playing))
        retire(b.playing);
    b.playing = playing;
    b.shown = shown && media;
    b.visible = b.shown && geometry.visible;
    b.animate = media_asset_key_valid(playing);
    b.header_hovered = header_hovered;
    b.view = b.shown ? UiGuildBannerView{guild_id, b.animate ? b.moving.key : b.still.key, b.still.key}
                     : UiGuildBannerView{};
    u64 signature = hash64_bytes(kHash64Seed, &b.view, sizeof(b.view));
    signature = hash64_mix(signature, (b.visible ? 1u : 0u) | (header_hovered ? 2u : 0u));
    if (signature != b.signature) {
        b.signature = signature;
        ++b.revision;
        // Starting an animation (or a new server) needs a media scan.
        g_app.message_media.scan_pending = true;
        application_request_frame();
    }
    g_app.renderer.guild_banner = b.shown ? &b.view : nullptr;
    const float inset = b.shown ? kUiGuildBannerListInset : 0.f;
    const bool rebuild = scene.banner_inset != inset;
    shell_navigation_scene_set_banner(&scene, inset, header_hovered, b.revision);
    if (rebuild && scene.active && g_app.account_ui)
        shell_navigation_scene_update(&scene, &g_app.account_ui->navigation_view, scene.viewport, scene.scroll_dips);
}

void application_guild_banner_pointer(UINT message, LPARAM lparam) noexcept {
    auto& b = g_banner;
    if (message == WM_MOUSELEAVE) {
        b.pointer_inside = false;
    } else if (message == WM_MOUSEMOVE) {
        b.pointer_inside = true;
        b.pointer_x = client_dip_x(lparam);
        b.pointer_y = client_dip_y(lparam);
    } else {
        return;
    }
    // Only a banner on screen reacts; re-project when its hover state moved.
    if (!b.shown)
        return;
    const auto* sidebar = shell_node(UiSemantic::channel_sidebar);
    const auto* header = shell_node(UiSemantic::channel_header);
    if (!sidebar || !header)
        return;
    const auto& scene = g_app.shell_navigation_scene;
    const auto geometry =
        ui_guild_banner_geometry(sidebar->rectangle, header->rectangle, scene.active ? scene.scroll_dips : 0.f, false);
    const bool header_hovered = geometry.visible && inside(geometry.header);
    const bool strip = geometry.visible && inside(geometry.hover);
    if (header_hovered != b.header_hovered || strip != b.animation.hovered)
        application_guild_banner_project(clock_monotonic_milliseconds());
}

u64 application_guild_banner_next_wake() noexcept {
    return g_banner.shown ? ui_guild_banner_next_wake(g_banner.animation, clock_monotonic_milliseconds()) : 0;
}

u32 application_guild_banner_media(GuildBannerMedia* output, u32 capacity) noexcept {
    const auto& b = g_banner;
    if (!output || !b.shown || !b.visible || renderer_settings_hides_shell(g_app.renderer))
        return 0;
    u32 count = 0;
    if (b.animate && count < capacity)
        output[count++] = {b.moving.url_view(), b.moving.key, true};
    if (media_asset_key_valid(b.still.key) && count < capacity)
        output[count++] = {b.still.url_view(), b.still.key, false};
    return count;
}

bool application_guild_banner_key_current(MediaAssetKey key) noexcept {
    const auto& b = g_banner;
    return b.shown && !renderer_settings_hides_shell(g_app.renderer) &&
           (media_asset_key_equal(key, b.still.key) || (b.animate && media_asset_key_equal(key, b.moving.key)));
}

void application_guild_banner_media_failed(MediaAssetKey key) noexcept {
    auto& b = g_banner;
    if (b.gif || !b.animate || !media_asset_key_equal(key, b.moving.key))
        return;
    b.gif = true;
    application_guild_banner_project(clock_monotonic_milliseconds());
}

bool application_guild_banner_take_retired(MediaAssetKey* key) noexcept {
    if (!key || !media_asset_key_valid(g_banner.retired))
        return false;
    *key = g_banner.retired;
    g_banner.retired = {};
    return true;
}
} // namespace maicord::app_detail
