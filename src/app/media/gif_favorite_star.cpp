#include "gif_favorites.hpp"
#include "../chat/channel_authority.hpp"
#include "../shell/state.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/chat/composer/attachment_actions.hpp"
#include "maicord/ui/chat/messages/media_policy.hpp"
#include "maicord/ui/chat/messages/rich_layout.hpp"
#include "maicord/ui/chat/messages/scene.hpp"

namespace maicord::app_detail {
namespace {
// A GIF on screen and the FavoriteGIF it saves, as the official client fills
// it: the URL people send (its key), the media source the picker shows, the
// media size and format.
struct GifStar {
    UiWidgetHandle owner;
    Utf8View url, src;
    u32 width, height;
    GifFavoriteFormat format;
    char proxy[2048];
};

// An attachment's media-proxy URL (its proxy_url): the CDN URL on
// media.discordapp.net. Other URLs are their own source.
Utf8View attachment_proxy(Utf8View url, GifStar* out) noexcept {
    constexpr Utf8View cdn = utf8_literal("https://cdn.discordapp.com/");
    constexpr Utf8View proxy = utf8_literal("https://media.discordapp.net/");
    if (url.bytes < cdn.bytes || !utf8_equal({url.data, cdn.bytes}, cdn) ||
        url.bytes - cdn.bytes + proxy.bytes > sizeof(out->proxy))
        return url;
    copy_bytes(out->proxy, proxy.data, proxy.bytes);
    copy_bytes(out->proxy + proxy.bytes, url.data + cdn.bytes, url.bytes - cdn.bytes);
    return {out->proxy, url.bytes - cdn.bytes + proxy.bytes};
}

bool available() noexcept {
    return g_app.account_ui && !g_app.login.visible && !g_app.popup_scene.active && !g_app.reaction_viewer.active &&
           !g_app.image_viewer.scene.active && g_app.message_scene.active;
}
// The GIF whose media widget is `owner`, or without an owner, the GIF whose
// visible star holds (x, y). Geometry matches message_presentation's.
bool find(UiWidgetHandle owner, float x, float y, GifStar* out) noexcept {
    if (!available())
        return false;
    const auto& scene = g_app.message_scene;
    const auto& store = g_app.account_ui->message_rich_store;
    const auto hit = [&](UiWidgetHandle widget, const UiLayoutNode& node, UiRect star) noexcept {
        if (owner.generation)
            return ui_widget_handle_equal(widget, owner);
        const UiRect visible = ui_rect_intersection(star, ui_rect_intersection(node.clip_rectangle, scene.viewport));
        return ui_rect_contains(visible, x, y);
    };
    for (u32 row_index = 0; row_index < scene.count; ++row_index) {
        for (u32 content_scope = 0; content_scope < 2; ++content_scope) {
            const auto* row = content_scope ? message_scene_snapshot_row(scene, scene.rows[row_index])
                                            : &scene.rows[row_index];
            if (!row)
                continue;
            const auto* message = message_store_resolve(g_app.message_view.store, row->message);
            const auto* outer_rich = message ? message_rich_store_find(store, message->id) : nullptr;
            const auto* rich =
                message && content_scope ? message_rich_store_snapshot(store, message->id) : outer_rich;
            if (!rich || message->channel_id != g_app.account_ui->active_channel_id ||
                message->channel_id != g_app.account_ui->chat_state.selected_channel_id ||
                !application_channel_readable(g_app, message->channel_id) ||
                rich->message_revision != message->revision)
                continue;
            for (u32 i = 0; i < rich->attachment_count; ++i) {
                const auto& attachment = rich->attachments[i];
                const auto widget =
                    message_scene_rich_item_widget(*row, MessageSceneRichItemKind::attachment, i);
                const auto* node = message_scene_rich_item_layout_node(
                    scene, *row, MessageSceneRichItemKind::attachment, i);
                float width = 0, height = 0;
                if (!node || (owner.generation && !ui_widget_handle_equal(widget, owner)) ||
                    !message_rich_attachment_gif_favorite(store, attachment) ||
                    message_scene_media_obscure(
                        scene, message_media_owner(*message, outer_rich), rich->message_id,
                        message_media_reveal_key(rich->message_id, MessageMediaSource::attachment, i,
                                                 content_scope != 0),
                        message_rich_attachment_spoiler(store, attachment),
                        message_rich_attachment_explicit(attachment)) != MessageMediaObscure::none ||
                    !message_rich_attachment_preview_extent(
                        store, attachment, ui_rect_width(node->content_rectangle), &width, &height))
                    continue;
                const UiRect preview{node->rectangle.left, node->rectangle.top, node->rectangle.left + width,
                                     node->rectangle.top + height};
                const UiRect star = ui_gif_favorite_rectangle(preview, application_theme());
                const Utf8View url = message_rich_store_string(store, attachment.url);
                if (!ui_rect_has_area(star) || !url.bytes || !hit(widget, *node, star))
                    continue;
                out->owner = widget;
                out->url = url;
                out->src = attachment_proxy(url, out);
                out->width = attachment.width;
                out->height = attachment.height;
                out->format = GifFavoriteFormat::image;
                return true;
            }
            for (u32 i = 0; i < rich->embed_count; ++i) {
                const auto& embed = rich->embeds[i];
                const auto widget = message_scene_rich_item_widget(*row, MessageSceneRichItemKind::embed, i);
                const auto* node = message_scene_rich_item_layout_node(scene, *row, MessageSceneRichItemKind::embed, i);
                MessageRichEmbedGalleryPlace place{};
                MessageRichEmbedMediaSelection selected{};
                if (!node || (owner.generation && !ui_widget_handle_equal(widget, owner)) ||
                    message_rich_embed_gallery_place(store, *rich, i, &place) ||
                    !message_rich_embed_media_select(store, embed, &selected) || !selected.media ||
                    !message_rich_embed_gif_favorite(store, embed, selected) ||
                    message_scene_media_obscure(
                        scene, message_media_owner(*message, outer_rich), message->id,
                        message_media_reveal_key(message->id, MessageMediaSource::embed, i,
                                                 content_scope != 0),
                        false, message_rich_embed_explicit(embed)) != MessageMediaObscure::none)
                    continue;
                const UiRect preview = message_rich_embed_media_rect(row->rich_item_layout, i, node->content_rectangle,
                                                                     message_rich_embed_media_only(embed));
                const UiRect star = ui_gif_favorite_rectangle(preview, application_theme());
                // The image, else the video; the key is the embed URL only for a gifv,
                // else the media's own URL.
                const auto present = [&](const MessageRichEmbedMedia& media) noexcept {
                    return message_rich_store_string(store, media.url).bytes ||
                           message_rich_store_string(store, media.proxy_url).bytes;
                };
                const bool video = present(embed.video);
                const MessageRichEmbedMedia& media = present(embed.image) ? embed.image : embed.video;
                const Utf8View media_url = message_rich_store_string(store, media.url);
                const Utf8View media_proxy = message_rich_store_string(store, media.proxy_url);
                const Utf8View src = media_proxy.bytes ? media_proxy : media_url;
                Utf8View url = embed.type == GatewayMessageEmbedType::gifv
                                   ? message_rich_store_string(store, embed.url)
                                   : media_url;
                if (!url.bytes)
                    url = src;
                if (!ui_rect_has_area(star) || !url.bytes || !src.bytes || !hit(widget, *node, star))
                    continue;
                out->owner = widget;
                out->url = url;
                out->src = src;
                out->width = media.width;
                out->height = media.height;
                out->format = video ? GifFavoriteFormat::video : GifFavoriteFormat::image;
                return true;
            }
        }
    }
    return false;
}
} // namespace

void application_gif_favorite_star_synchronize() noexcept {
    auto& renderer = g_app.renderer;
    const UiWidgetHandle hovered = g_app.message_scene.widgets.hovered;
    GifStar gif{};
    const bool found = hovered.generation && find(hovered, 0, 0, &gif);
    // Refresh the source interest even when a scene reuses the same widget
    // handle after a channel or account transition.
    application_gif_favorites_set_hovered(found);
    const ApplicationGifFavoriteState state =
        found ? application_gif_favorites_state(gif.url) : ApplicationGifFavoriteState{};
    renderer.gif_favorite_owner = found ? gif.owner : UiWidgetHandle{};
    renderer.gif_favorite = state.confirmed;
    renderer.gif_favorite_pending = state.pending;
}

bool application_gif_favorite_star_target(float x, float y) noexcept {
    GifStar gif{};
    return find({}, x, y, &gif);
}

bool application_gif_favorite_star_press(float x, float y) noexcept {
    GifStar gif{};
    if (!find({}, x, y, &gif))
        return false;
    const auto state = application_gif_favorites_state(gif.url);
    if (state.available && !state.pending)
        application_gif_favorites_toggle(gif.url, gif.src, gif.width, gif.height, gif.format);
    application_gif_favorite_star_synchronize();
    application_request_frame();
    return true;
}
} // namespace maicord::app_detail
