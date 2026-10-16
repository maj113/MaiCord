#include "application_gif_favorite_star.hpp"
#include "application_gif_favorites.hpp"
#include "application_channel_authority.hpp"
#include "application_state.hpp"
#include "maicord/ui/attachment_actions.hpp"
#include "maicord/ui/message_media_policy.hpp"
#include "maicord/ui/message_rich_layout.hpp"
#include "maicord/ui/message_scene.hpp"

namespace maicord::app_detail {
namespace {
// A GIF on screen and the FavoriteGIF it saves: the URL people send (its
// key), the media source the picker shows, and the media size.
struct GifStar {
    UiWidgetHandle owner;
    UiRect star;
    Utf8View url, src;
    u32 width, height;
};
UiWidgetHandle g_loaded_owner{};

bool same(UiWidgetHandle a, UiWidgetHandle b) noexcept {
    return a.generation && a.slot == b.slot && a.generation == b.generation;
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
            return same(widget, owner);
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
            const auto* rich = message ? (content_scope ? message_rich_store_snapshot(store, message->id)
                                                        : message_rich_store_find(store, message->id))
                                       : nullptr;
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
                if (!node || (owner.generation && !same(widget, owner)) ||
                    message_rich_attachment_format(store, attachment) != MediaAttachmentFormat::gif ||
                    message_scene_media_obscure(
                        scene, rich->message_id,
                        message_media_reveal_key(rich->message_id, MessageMediaSource::attachment, i),
                        message_rich_attachment_spoiler(store, attachment),
                        message_rich_attachment_explicit(attachment)) != MessageMediaObscure::none ||
                    !message_rich_attachment_preview_extent(store, attachment, ui_rect_width(node->content_rectangle),
                                                            &width, &height))
                    continue;
                const UiRect preview{node->rectangle.left, node->rectangle.top, node->rectangle.left + width,
                                     node->rectangle.top + height};
                const UiRect star = ui_gif_favorite_rectangle(preview, true);
                const Utf8View url = message_rich_store_string(store, attachment.url);
                if (!ui_rect_has_area(star) || !url.bytes || !hit(widget, *node, star))
                    continue;
                *out = {widget, star, url, url, attachment.width, attachment.height};
                return true;
            }
            for (u32 i = 0; i < rich->embed_count; ++i) {
                const auto& embed = rich->embeds[i];
                const auto widget = message_scene_rich_item_widget(*row, MessageSceneRichItemKind::embed, i);
                const auto* node = message_scene_rich_item_layout_node(scene, *row, MessageSceneRichItemKind::embed, i);
                MessageRichEmbedGalleryPlace place{};
                MessageRichEmbedMediaSelection selected{};
                if (!node || (owner.generation && !same(widget, owner)) ||
                    message_rich_embed_gallery_place(store, *rich, i, &place) ||
                    !message_rich_embed_media_select(store, embed, &selected) || !selected.media ||
                    (embed.type != GatewayMessageEmbedType::gifv && selected.format != MediaAttachmentFormat::gif) ||
                    message_scene_media_obscure(scene, message->id,
                                                message_media_reveal_key(message->id, MessageMediaSource::embed, i),
                                                false, message_rich_embed_explicit(embed)) !=
                        MessageMediaObscure::none)
                    continue;
                const UiRect preview = message_rich_embed_media_rect(row->rich_item_layout, i, node->content_rectangle,
                                                                     message_rich_embed_media_only(embed));
                const UiRect star = ui_gif_favorite_rectangle(preview, false);
                Utf8View url = message_rich_store_string(store, embed.url);
                if (!url.bytes)
                    url = selected.source;
                if (!ui_rect_has_area(star) || !url.bytes || !selected.source.bytes || !hit(widget, *node, star))
                    continue;
                *out = {widget, star, url, selected.source, selected.media->width, selected.media->height};
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
    ApplicationGifFavoriteState state{};
    // Refresh the source interest even when a scene reuses the same widget
    // handle after a channel or account transition.
    application_gif_favorites_set_hovered(found);
    if (found) {
        state = application_gif_favorites_state(gif.url);
    }
    g_loaded_owner = found ? gif.owner : UiWidgetHandle{};
    renderer.gif_favorite_owner = g_loaded_owner;
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
        application_gif_favorites_toggle(gif.url, gif.src, gif.width, gif.height,
                                         application_gif_favorites_format(gif.src));
    application_gif_favorite_star_synchronize();
    application_request_frame();
    return true;
}
} // namespace maicord::app_detail
