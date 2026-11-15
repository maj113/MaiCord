#include "banner.hpp"
#include "maicord/core/memory.hpp"
#include "../shell/input.hpp"
#include "../shell/state.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/guild/banner.hpp"
#include "maicord/ui/chat/messages/rich_layout.hpp"
#include "maicord/render/d3d11/renderer_settings_visibility.hpp"
#include "../account/account_ui_runtime.hpp"

#include "maicord/media/core/asset.hpp"

namespace maicord::app_detail {
namespace {
// What `still` and `moving` come from. The client makes them again only when one of these values changes.
struct MediaSource {
    u64 guild_id; // zero: no banner is shown
    u32 dpi;
    u8 hash_bytes;
    bool eligible, gif;
    char hash[kGuildIconHashMaximumBytes];
};
struct BannerState {
    UiGuildBannerAnimation animation;
    UiGuildBannerView view;
    UiGuildImageMedia still, moving;
    MediaSource source;
    MediaAssetKey retired, playing;
    float pointer_x, pointer_y;
    u32 revision;
    bool media, shown, visible, animate, gif, pointer_inside, header_hovered;
};
BannerState g_banner;

const UiLayoutNode* shell_node(UiSemantic semantic) noexcept {
    return ui_scene_node(g_app.shell_scene, semantic);
}
const GuildRecord* selected_guild() noexcept {
    if (!g_app.account_ui || g_app.login.visible || g_app.shell_scene.view != UiShellView::guild_channel)
        return nullptr;
    const auto& state = g_app.account_ui->chat_state;
    const auto* guild = state.selected_guild_id ? chat_state_store_find_guild(state, state.selected_guild_id) : nullptr;
    return guild && !guild->unavailable && chat_state_store_guild_banner_hash(*guild).bytes ? guild : nullptr;
}
bool inside(const UiRect& r) noexcept {
    return g_banner.pointer_inside && ui_rect_has_area(r) && ui_rect_contains(r, g_banner.pointer_x, g_banner.pointer_y);
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
                          (guild->upload_features & guild_record_animated_banner) && !application_reduced_motion();
    const bool animate =
        ui_guild_banner_animate(&b.animation, guild_id, eligible, shown && geometry.visible, inside(geometry.hover),
                                message_rich_display().gifs == MessageMediaAnimation::always, now_ms);
    if (guild_id != b.view.guild_id)
        b.gif = false;
    // Cleared whole, the padding too: the comparison reads every byte.
    MediaSource source;
    clear_bytes(&source, sizeof(source));
    if (shown) {
        source.guild_id = guild_id;
        source.dpi = g_app.renderer.dpi ? g_app.renderer.dpi : 96;
        source.hash_bytes = static_cast<u8>(hash.bytes);
        source.eligible = eligible;
        source.gif = b.gif;
        copy_bytes(source.hash, hash.data, minimum<u32>(hash.bytes, sizeof(source.hash)));
    }
    if (!bytes_equal(&source, &b.source, sizeof(source))) {
        copy_bytes(&b.source, &source, sizeof(source));
        b.media = shown && ui_guild_banner_media(guild_id, hash, source.dpi, false, false, &b.still);
        if (!b.media)
            reset_value(&b.still);
        if (!(eligible && b.media && ui_guild_banner_media(guild_id, hash, source.dpi, true, b.gif, &b.moving)))
            reset_value(&b.moving);
    }
    // A stopped animation (or one of a server no longer shown) gives its
    // playback back; the still stays resident.
    const MediaAssetKey playing = animate && media_asset_key_valid(b.moving.key) ? b.moving.key : MediaAssetKey{};
    if (media_asset_key_valid(b.playing) && !media_asset_key_equal(b.playing, playing))
        b.retired = b.playing;
    b.playing = playing;
    b.shown = shown && b.media;
    b.animate = media_asset_key_valid(playing);
    const MediaAssetKey& shown_key = b.animate ? b.moving.key : b.still.key;
    const UiGuildBannerView view = b.shown ? UiGuildBannerView{guild_id, shown_key, b.still.key} : UiGuildBannerView{};
    const bool visible = b.shown && geometry.visible;
    // The first projection also counts as a change.
    if (!b.revision || view.guild_id != b.view.guild_id || !media_asset_key_equal(view.key, b.view.key) ||
        !media_asset_key_equal(view.fallback, b.view.fallback) || visible != b.visible ||
        header_hovered != b.header_hovered) {
        ++b.revision;
        // Starting an animation (or a new server) needs a media scan.
        g_app.message_media.scan_pending = true;
        application_request_frame();
    }
    b.view = view;
    b.visible = visible;
    b.header_hovered = header_hovered;
    g_app.renderer.guild_banner = b.shown ? &b.view : nullptr;
    const float inset = b.shown ? kUiGuildBannerListInset : 0.f;
    const bool rebuild = scene.banner_inset != inset;
    shell_navigation_scene_set_banner(&scene, inset, header_hovered, b.revision);
    if (rebuild && scene.active && g_app.account_ui)
        shell_navigation_scene_update(&scene, &g_app.account_ui->navigation_view, scene.viewport, scene.scroll_dips);
}

void application_guild_banner_pointer(const WindowEvent& event) noexcept {
    auto& b = g_banner;
    if (event.type == EventType::pointer_leave) {
        b.pointer_inside = false;
    } else if (event.type == EventType::pointer_move) {
        b.pointer_inside = true;
        b.pointer_x = client_dip_x(event);
        b.pointer_y = client_dip_y(event);
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
    if (!b.shown || !b.visible || renderer_settings_hides_shell(g_app.renderer))
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
    if (!media_asset_key_valid(g_banner.retired))
        return false;
    *key = g_banner.retired;
    reset_value(&g_banner.retired);
    return true;
}
} // namespace maicord::app_detail
