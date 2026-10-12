#include "maicord/ui/theme_style_projection.hpp"
#include "maicord/ui/message_presentation.hpp"
#include "maicord/ui/message_reaction_presentation.hpp"
#include "maicord/ui/scrollbar.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/attachment_actions.hpp"
#include "maicord/ui/audio_presentation.hpp"
#include "maicord/ui/poll_presentation.hpp"
#include "maicord/ui/message_component_layout.hpp"
#include "maicord/ui/message_component_media.hpp"
#include "maicord/ui/message_system_text.hpp"
#include "maicord/model/invite_card_cache.hpp"
#include "maicord/model/message_poll_store.hpp"
#include "maicord/model/scheduled_event_store.hpp"
#include "maicord/core/hash.hpp"

namespace maicord {
namespace {

bool append_media(MessageMediaCommandStream* stream, u64 id, MessageMediaCommandKind kind, u8 format,
                  bool animated, UiRect rectangle, UiRect clip, UiWidgetReference owner,
                  u32 message_revision = 0, u8 embed_index = 0, u8 embed_resource = 0,
                  bool ordinary_video = false, bool snapshot = false,
                  MessageMediaObscure obscure = MessageMediaObscure::none) noexcept {
    if (!stream || !stream->storage.data || id == 0 || !ui_rect_has_area(rectangle) || !owner.manager) {
        saturating_increment_if(stream, stream->rejected);
        return false;
    }
    if (stream->count >= stream->storage.count) {
        saturating_increment(stream->rejected);
        return false;
    }
    stream->storage[stream->count++] = MessageMediaCommand{
        id,          rectangle,      clip,     owner,          message_revision, kind, format,
        embed_index, embed_resource, animated, ordinary_video, snapshot,         false, 0,
        static_cast<u8>(obscure)};
    return true;
}

Color tone_color(const Theme& theme, MessageSystemTone tone) noexcept {
    switch (tone) {
    case MessageSystemTone::positive:
        return theme.tokens.accent.success;
    case MessageSystemTone::negative:
        return theme.tokens.accent.danger;
    case MessageSystemTone::boost:
        return theme.syntax_keyword;
    case MessageSystemTone::accent:
        return theme.tokens.accent.brand;
    default:
        return theme.text_secondary;
    }
}

// Blocked explicit media keeps its box as a muted notice; the text pass adds
// the label. Obscured previews still draw (blurred by the media shader).
bool push_blocked(PaintStream* paint, UiWidgetReference owner, UiRect rectangle, const Theme& theme) noexcept {
    return paint_stream_push_rounded_border(paint, rectangle.left, rectangle.top, rectangle.right,
                                            rectangle.bottom, theme.surface_high, theme.tokens.border.subtle,
                                            theme.scale.radius_sm, 1.0f) &&
           paint_stream_record_content(paint, owner, rectangle, UiLayoutContentKind::decoration,
                                       ui_layout_content_bounds_allow_overlap);
}

bool push_owned(PaintStream* stream, UiWidgetReference owner, UiRect rectangle, const Color& fill,
                const Color& border, float radius, float border_width, UiLayoutContentKind kind,
                u16 flags = ui_layout_content_bounds_collision) noexcept {
    return paint_stream_push_rounded_border(stream, rectangle.left, rectangle.top, rectangle.right,
                                            rectangle.bottom, fill, border, radius, border_width) &&
           paint_stream_record_content(stream, owner, rectangle, kind, flags);
}
bool push_material(PaintStream* stream, UiWidgetReference owner, UiRect rectangle, const Theme& theme,
                   Color fill, Color border, float radius, float border_width, UiLayoutContentKind kind,
                   UiBackgroundTier tier,
                   UiBackgroundBorderTone tone = UiBackgroundBorderTone::palette) noexcept {
    return ui_background_paint_surface(stream, rectangle, theme, fill, border, radius, border_width, .9f,
                                       UiBackgroundSurfaceMode::gradient, tier, tone) &&
           paint_stream_record_content(stream, owner, rectangle, kind,
                                       ui_layout_content_bounds_allow_overlap);
}

// Invite card chrome and the server icon (fetched by URL like profile art).
bool append_invite_cards(const MessageScene& scene, const MessageSceneRowWidget& row, const MessageRecord& record,
                         const Theme& theme, UiRect row_clip, PaintStream* paint,
                         MessageMediaCommandStream* media) noexcept {
    InviteLink links[kMessageInviteCardMaximum];
    const u32 count = minimum<u32>(row.invite_item_count,
                                   message_scene_invite_codes(scene, record, {links, kMessageInviteCardMaximum}));
    for (u32 i = 0; i < count; ++i) {
        const UiLayoutNode* node = message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::invite, i);
        if (!node)
            return false;
        const auto geometry = message_invite_card_geometry(node->rectangle);
        if (!ui_rect_has_area(ui_rect_intersection(geometry.card, row_clip)))
            continue;
        const UiWidgetReference owner{&scene.widgets, message_scene_rich_item_widget(row, MessageSceneRichItemKind::invite, i)};
        if (invite_link_event(links[i])) {
            // Event cards: the cover image (or a calendar) and a View action.
            const ScheduledEvent* event = scene.scheduled_events
                                              ? scheduled_event_store_find(*scene.scheduled_events, links[i].event_id)
                                              : nullptr;
            if (!push_material(paint, owner, geometry.card, theme, theme.surface, theme.interactive, 8.0f, 1.0f,
                               UiLayoutContentKind::control, UiBackgroundTier::high) ||
                !push_owned(paint, owner, geometry.icon, theme.surface_high, theme.surface_high, 12.0f, 0.0f,
                            UiLayoutContentKind::icon, ui_layout_content_bounds_allow_overlap))
                return false;
            char url[192];
            const u32 url_bytes = event ? message_event_cover_url(*event, {url, sizeof(url)}) : 0;
            if (url_bytes) {
                const MediaAssetKey key = message_component_media_key(hash64_bytes(kHash64Seed, url, url_bytes),
                                                                      MediaAttachmentFormat::png);
                if (!append_media(media, key.id, MessageMediaCommandKind::component,
                                  static_cast<u8>(MediaAttachmentFormat::png), false, geometry.icon, row_clip, owner))
                    return false;
            } else {
                const UiRect glyph{geometry.icon.left + 13.0f, geometry.icon.top + 13.0f, geometry.icon.right - 13.0f,
                                   geometry.icon.bottom - 13.0f};
                if (!ui_icon_paint(paint, UiIcon::calendar, glyph,
                                   event && event->status == ScheduledEventStatus::active
                                       ? theme.tokens.accent.success
                                       : theme.text_secondary))
                    return false;
            }
            if (event) {
                const Color fill = event->interested ? theme.interactive : theme.tokens.accent.brand;
                if (!push_owned(paint, owner, geometry.button, fill, fill, 4.0f, 0.0f, UiLayoutContentKind::control))
                    return false;
            }
            continue;
        }
        const InviteCardEntry* entry =
            scene.invite_cards ? invite_card_cache_find(*scene.invite_cards, {links[i].code, links[i].code_bytes})
                               : nullptr;
        const bool known = entry && entry->invite.guild_id;
        if (!push_material(paint, owner, geometry.card, theme, theme.surface, theme.interactive, 8.0f, 1.0f,
                           UiLayoutContentKind::control, UiBackgroundTier::high) ||
            !push_owned(paint, owner, geometry.icon, theme.surface_high, theme.surface_high, 16.0f, 0.0f,
                        UiLayoutContentKind::icon, ui_layout_content_bounds_allow_overlap))
            return false;
        if (!known) {
            const UiRect glyph{geometry.icon.left + 13.0f, geometry.icon.top + 13.0f, geometry.icon.right - 13.0f,
                               geometry.icon.bottom - 13.0f};
            if (!ui_icon_paint(paint, entry && entry->state == InviteCardState::invalid ? UiIcon::circle_x : UiIcon::guild,
                               glyph, theme.text_secondary))
                return false;
            continue;
        }
        char url[192];
        const u32 url_bytes = message_invite_icon_url(entry->invite, {url, sizeof(url)});
        if (url_bytes) {
            const MediaAssetKey key = message_component_media_key(hash64_bytes(kHash64Seed, url, url_bytes),
                                                                  MediaAttachmentFormat::png);
            if (!append_media(media, key.id, MessageMediaCommandKind::component,
                              static_cast<u8>(MediaAttachmentFormat::png), false, geometry.icon, row_clip, owner))
                return false;
        }
        const Color fill = entry->joined ? theme.interactive : theme.tokens.accent.success;
        if (!push_owned(paint, owner, geometry.button, fill, fill, 4.0f, 0.0f, UiLayoutContentKind::control))
            return false;
    }
    return true;
}

bool append_row(const MessageScene& scene, const MessageSceneRowWidget& row, const Theme& theme,
                PaintStream* paint, MessageMediaCommandStream* media, u32* geometry_quads) noexcept {
    if (row.redacted || scene.content_gated)
        return true;
    const UiLayoutNode* avatar = message_scene_layout_node(scene, row.avatar);
    const UiLayoutNode* body = message_scene_layout_node(scene, row.body);
    const UiLayoutNode* reply = message_scene_layout_node(scene, row.reply);
    const UiLayoutNode* row_layout = message_scene_layout_node(scene, row.widget);
    if ((!row.forwarded_snapshot && !avatar) || !body || !reply || !row_layout) {
        return false;
    }
    const UiRect row_clip = ui_rect_intersection(scene.viewport, row_layout->rectangle);
    if (!ui_rect_has_area(row_clip)) {
        return true;
    }
    const u32 geometry_begin = paint->count;
    const UiRect message_rectangle = message_scene_message_rectangle(scene, row);
    const UiRect divider = message_scene_unread_divider_rectangle(scene, row);
    if (ui_rect_has_area(divider) &&
        !ui_unread_divider_paint(paint, divider, theme, scene.unread_divider_style, scene.pixel_scale))
        return false;
    const MessageSceneRowWidget* hovered = message_scene_resolve(scene, scene.widgets.hovered);
    if (!row.forwarded_snapshot && hovered == &row) {
        Color hover_fill = theme.surface;
        hover_fill.alpha = 0.55f;
        if (ui_background_active(theme))
            hover_fill = ui_background_hover_color(theme, theme.surface, .10f);
        if (!push_owned(paint, UiWidgetReference{&scene.widgets, row.widget}, message_rectangle, hover_fill,
                        hover_fill, 0.0f, 0.0f, UiLayoutContentKind::decoration,
                        ui_layout_content_bounds_allow_overlap)) {
            return false;
        }
    }
    const MessageRecord* record = message_store_resolve(*scene.timeline->store, row.message);
    if (!row.forwarded_snapshot && record && scene.highlight_message_id == record->id &&
        scene.highlight_level > 0.0f) {
        const Color tint = ui_theme_color_alpha(
            {theme.tokens.emphasis.outline.red, theme.tokens.emphasis.outline.green,
             theme.tokens.emphasis.outline.blue, 1},
            kMessageSceneHighlightAlpha * scene.highlight_level);
        if (!push_owned(paint, UiWidgetReference{&scene.widgets, row.widget}, message_rectangle, tint, tint, 0.0f,
                        0.0f, UiLayoutContentKind::decoration, ui_layout_content_bounds_allow_overlap))
            return false;
    }
    if (!row.forwarded_snapshot && record && (record->flags & kMessageSceneEphemeralFlag) != 0) {
        const Color tint = ui_theme_color_alpha(
            {theme.tokens.emphasis.outline.red, theme.tokens.emphasis.outline.green,
             theme.tokens.emphasis.outline.blue, 1},
            kMessageSceneEphemeralAlpha);
        if (!push_owned(paint, UiWidgetReference{&scene.widgets, row.widget}, message_rectangle, tint, tint, 0.0f,
                        0.0f, UiLayoutContentKind::decoration, ui_layout_content_bounds_allow_overlap))
            return false;
    }
    if (!row.forwarded_snapshot && record &&
        (record->presentation_flags & message_presentation_mentions_current_user) != 0) {
        const Color mention_fill = theme.tokens.mention.background;
        const UiRect edge{message_rectangle.left, message_rectangle.top, message_rectangle.left + 2.0f,
                          message_rectangle.bottom};
        const UiWidgetReference row_owner{&scene.widgets, row.widget};
        if (!push_owned(paint, row_owner, message_rectangle, mention_fill, mention_fill, 0.0f, 0.0f,
                        UiLayoutContentKind::decoration, ui_layout_content_bounds_allow_overlap) ||
            !push_owned(paint, row_owner, edge, theme.tokens.mention.bar, theme.tokens.mention.bar, 0.0f, 0.0f,
                        UiLayoutContentKind::decoration, ui_layout_content_bounds_allow_overlap)) {
            return false;
        }
    }
    const UiWidgetReference avatar_owner{&scene.widgets, row.avatar};
    if (message_scene_avatar_visible(scene, row) && !push_owned(paint, avatar_owner, avatar->rectangle, theme.avatar_placeholder,
                                    theme.avatar_placeholder, ui_rect_width(avatar->rectangle) * 0.5f, 0.0f,
                                    UiLayoutContentKind::icon)) {
        return false;
    }
    if (!row.forwarded_snapshot && record && message_type_is_system(record->type) && avatar) {
        // System rows: a tinted glyph centred in the avatar gutter, on the
        // first text line.
        const MessageRichRecord* system_rich =
            scene.timeline->rich_store ? message_rich_store_find(*scene.timeline->rich_store, record->id) : nullptr;
        MessageSystemTextInput input{};
        input.type = record->type;
        input.message_id = record->id;
        if (system_rich && system_rich->message_revision == record->revision) {
            input.call_present = system_rich->system.call_present;
            input.call_ended = system_rich->system.call_ended;
            input.call_participated = system_rich->system.call_participated;
            input.same_user = system_rich->system.target_user_id == record->author_id;
        }
        char storage[kMessageSystemTextMaximumBytes];
        MessageSystemText system{};
        if (message_system_text_build(input, {storage, sizeof(storage)}, &system)) {
            const float extent = 18.0f;
            const float center = (avatar->rectangle.left + avatar->rectangle.right) * 0.5f;
            const float top = body->content_rectangle.top + 2.0f;
            const UiRect glyph{center - extent * 0.5f, top, center + extent * 0.5f, top + extent};
            if (!ui_icon_paint(paint, system.icon, glyph, tone_color(theme, system.tone)))
                return false;
        }
    }

    const MessageRichStore* rich_store = scene.timeline->rich_store;
    if (!row.forwarded_snapshot && row.poll_content_revision) {
        UiPollAnswer answers[kUiPollMaximumAnswers]{};
        UiPollProjection poll{};
        const UiWidgetReference owner{&scene.widgets, row.poll};
        if (row.poll_unavailable) {
            const auto* node = message_scene_layout_node(scene, row.poll);
            if (!node || !push_material(paint, owner,
                                        {node->rectangle.left, node->rectangle.top,
                                         minimum(node->rectangle.right,
                                                 node->rectangle.left +
                                                     ui_theme_poll_style(scene.theme_styles).maximum_width),
                                         node->rectangle.top + row.poll_height -
                                             ui_theme_metrics(scene.theme_styles).message_rich_gap},
                                        theme, theme.surface, theme.interactive,
                                        ui_theme_metrics(scene.theme_styles).control_radius, 1,
                                        UiLayoutContentKind::control, UiBackgroundTier::high))
                return false;
        } else if (!message_scene_poll_projection(scene, row, theme, {answers, kUiPollMaximumAnswers},
                                                  &poll) ||
                   !ui_poll_controls_paint(poll, ui_theme_poll_style(scene.theme_styles), row_clip, theme,
                                           owner, paint))
            return false;
        for (u32 i = 0; i < poll.state.answers.count; ++i) {
            const auto& answer = poll.state.answers[i];
            if (answer.emoji_id &&
                !append_media(media, answer.emoji_id, MessageMediaCommandKind::custom_emoji, 0,
                              answer.emoji_animated, poll.geometry.answers[i].emoji, row_clip, owner))
                return false;
        }
    }
    const MessageRichRecord* rich =
        record && rich_store
            ? (row.forwarded_snapshot ? message_rich_store_snapshot(*rich_store, record->id)
                                      : message_rich_store_find(*rich_store, record->id))
            : nullptr;
    if (rich && rich->message_revision != record->revision) {
        rich = nullptr;
    }
    // A plugin-collapsed row has no rich widgets (see the scene's row_rich).
    if (rich && !row.forwarded_snapshot &&
        message_scene_plugin_visibility(scene, *record) != ui_plugin_render_normal)
        rich = nullptr;
    if (!row.forwarded_snapshot && record && row.invite_item_count &&
        !append_invite_cards(scene, row, *record, theme, row_clip, paint, media))
        return false;
    if (!rich) {
        *geometry_quads += paint->count - geometry_begin;
        return true;
    }

    const float text_left = body->content_rectangle.left;
    if (row.forwarded_snapshot) {
        const UiWidgetReference owner{&scene.widgets, row.widget};
        const UiRect edge{row_layout->rectangle.left, row_layout->rectangle.top,
                          row_layout->rectangle.left + 3, row_layout->rectangle.bottom};
        const UiRect glyph{reply->rectangle.left, reply->rectangle.top + 1, reply->rectangle.left + 18,
                           reply->rectangle.top + 19};
        if (!push_material(paint, owner, row_layout->rectangle, theme, theme.surface, theme.interactive, 8,
                           1, UiLayoutContentKind::decoration, UiBackgroundTier::low) ||
            !push_owned(paint, owner, edge, theme.interactive_hover, theme.interactive_hover, 1, 0,
                        UiLayoutContentKind::decoration, ui_layout_content_bounds_allow_overlap) ||
            !ui_icon_paint(paint, UiIcon::forwarded, glyph, theme.text_secondary))
            return false;
    }
    if (!row.forwarded_snapshot && message_rich_has_header(*rich)) {
        const float reference_extent = 16.0f;
        const float reference_top = reply->rectangle.top + 1.0f;
        const UiRect reference_avatar{text_left, reference_top, text_left + reference_extent,
                                      reference_top + reference_extent};
        const float connector_y = reference_top + reference_extent * 0.5f;
        const float connector_x = (avatar->rectangle.left + avatar->rectangle.right) * 0.5f;
        const UiRect connector_vertical{connector_x - 1.0f, connector_y, connector_x + 1.0f,
                                        avatar->rectangle.top + 3.0f};
        const UiRect connector_horizontal{connector_x, connector_y - 1.0f, reference_avatar.left - 3.0f,
                                          connector_y + 1.0f};
        const UiWidgetReference reply_owner{&scene.widgets, row.reply};
        if (!push_owned(paint, reply_owner, connector_vertical, theme.interactive_hover,
                        theme.interactive_hover, 1.0f, 0.0f, UiLayoutContentKind::decoration,
                        ui_layout_content_bounds_allow_overflow) ||
            !push_owned(paint, reply_owner, connector_horizontal, theme.interactive_hover,
                        theme.interactive_hover, 1.0f, 0.0f, UiLayoutContentKind::decoration,
                        ui_layout_content_bounds_allow_overflow) ||
            !push_owned(paint, reply_owner, reference_avatar, theme.avatar_placeholder,
                        theme.avatar_placeholder, reference_extent * 0.5f, 0.0f, UiLayoutContentKind::icon,
                        ui_layout_content_bounds_allow_overlap)) {
            return false;
        }
        // The official client's trailing reply icon (attachment/embed image, sticker,
        // command, voice message), 4 DIPs after the measured preview text.
        const auto& reference = rich->reference;
        const float reply_width = row.rich_item_layout.reply_width;
        if (reference.state == GatewayMessageReferenceState::available && reply_width > 0.0f) {
            const bool text_empty = !reference.content.bytes;
            const bool poll = (reference.content_flags & gateway_reference_poll_question) ||
                              (text_empty && scene.polls && scene.polls->store &&
                               message_poll_store_find(*scene.polls->store, reference.message_id));
            const MessageReplyIcon kind =
                message_rich_reply_icon(reference.content_flags, reference.message_type, text_empty, poll);
            const UiIcon icon = kind == MessageReplyIcon::sticker   ? UiIcon::sticker
                                : kind == MessageReplyIcon::command ? UiIcon::command
                                : kind == MessageReplyIcon::voice   ? UiIcon::mic
                                                                    : UiIcon::image;
            const float icon_left = text_left + kMessageReplyTextInset + reply_width + kMessageReplyIconGap;
            const float icon_top = reference_top + reference_extent * 0.5f - kMessageReplyIcon * 0.5f;
            const UiRect glyph{icon_left, icon_top, icon_left + kMessageReplyIcon, icon_top + kMessageReplyIcon};
            if (kind != MessageReplyIcon::none &&
                ui_rect_has_area(ui_rect_intersection(glyph, row_clip)) &&
                !ui_icon_paint(paint, icon, glyph, theme.text_primary))
                return false;
        }
    }

    for (u32 index = 0; index < rich->attachment_count; ++index) {
        const UiWidgetHandle item_widget =
            message_scene_rich_item_widget(row, MessageSceneRichItemKind::attachment, index);
        const UiLayoutNode* item_layout =
            message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::attachment, index);
        if (!item_layout) {
            return false;
        }
        const UiWidgetReference owner{&scene.widgets, item_widget};
        const float left = item_layout->rectangle.left;
        const float top = item_layout->rectangle.top;
        float preview_width = 0.0f;
        float preview_height = 0.0f;
        const MessageRichAttachment& attachment = rich->attachments[index];
        const MediaAttachmentFormat format = message_rich_attachment_format(*rich_store, attachment);
        const float audio_height = message_rich_attachment_audio_supported(*rich_store, attachment)
                                       ? ui_audio_card_height(ui_theme_audio_style(scene.theme_styles),
                                                              ui_rect_width(item_layout->content_rectangle),
                                                              message_rich_record_voice(*rich))
                                       : 0;
        if (audio_height > 0) {
            const auto card = UiRect{item_layout->content_rectangle.left, top,
                                     item_layout->content_rectangle.right, top + audio_height};
            if (!ui_rect_has_area(ui_rect_intersection(card, row_clip)))
                continue;
            u8 waveform[kUiAudioWaveformMaximum]{};
            UiAudioControlsProjection projection{};
            if (!ui_audio_attachment_project(*rich_store, attachment, rich->message_id,
                                             rich->message_revision, message_rich_record_voice(*rich), card,
                                             theme, {waveform, sizeof(waveform)}, &projection) ||
                !ui_audio_controls_paint(projection, ui_theme_audio_style(scene.theme_styles), row_clip,
                                         theme, owner, paint))
                return false;
        } else if (message_rich_attachment_preview_extent(*rich_store, attachment,
                                                          ui_rect_width(item_layout->content_rectangle),
                                                          &preview_width, &preview_height)) {
            const UiRect preview{left, top, left + preview_width, top + preview_height};
            const MessageMediaObscure obscure = message_scene_media_obscure(
                scene, rich->message_id, message_media_reveal_key(rich->message_id, MessageMediaSource::attachment, index),
                message_rich_attachment_spoiler(*rich_store, attachment), message_rich_attachment_explicit(attachment));
            if (message_media_obscure_notice(obscure)) {
                if (!push_blocked(paint, owner, preview, theme))
                    return false;
                continue;
            }
            if (!push_owned(paint, owner, preview, theme.surface_high, theme.surface_high, 6.0f, 0.0f,
                            UiLayoutContentKind::decoration, ui_layout_content_bounds_allow_overlap) ||
                !append_media(media, attachment.id, MessageMediaCommandKind::attachment,
                              static_cast<u8>(format), false, preview, row_clip, owner, 0, 0, 0,
                              format == MediaAttachmentFormat::mp4 && obscure == MessageMediaObscure::none, false,
                              obscure)) {
                return false;
            }
        } else {
            const UiRect card{left, top, minimum(item_layout->rectangle.right, left + 420.0f), top + 36.0f};
            const UiRect icon{left + 8.0f, top + 8.0f, left + 28.0f, top + 28.0f};
            if (!push_material(paint, owner, card, theme, theme.surface, theme.interactive, 6.0f, 1.0f,
                               UiLayoutContentKind::control, UiBackgroundTier::high) ||
                !push_owned(paint, owner, icon, theme.tokens.emphasis.fill, theme.tokens.emphasis.fill, 4.0f, 0.0f,
                            UiLayoutContentKind::icon) ||
                (!scene.result_cards &&
                 !ui_attachment_save_paint(paint, owner, ui_attachment_save_rectangle(card, true), row_clip,
                                           theme, true))) {
                return false;
            }
        }
    }

    for (u32 index = 0; message_rich_display().embeds && index < rich->embed_count; ++index) {
        const UiWidgetHandle item_widget =
            message_scene_rich_item_widget(row, MessageSceneRichItemKind::embed, index);
        const UiLayoutNode* item_layout =
            message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::embed, index);
        if (!item_layout) {
            return false;
        }
        const UiWidgetReference owner{&scene.widgets, item_widget};
        // A merged embed draws nothing; its image is a tile of the first's gallery.
        if (message_rich_embed_merge_host(*rich_store, *rich, index) != index)
            continue;
        const float left = item_layout->rectangle.left;
        const float top = item_layout->rectangle.top;
        const float bottom = maximum(top, item_layout->rectangle.bottom - kMessageEmbedItemGap);
        const float measured_width = row.rich_item_layout.embed_widths[index];
        const UiRect card{left, top,
                          minimum(item_layout->rectangle.right,
                                  left + (measured_width > 0 ? measured_width : kMessageEmbedMaximumWidth)),
                          bottom};
        const UiRect accent{left, top, left + kMessageEmbedAccentWidth, bottom};
        const MessageRichEmbed& embed = rich->embeds[index];
        constexpr float rgb_scale = 1.0f / 255.0f;
        // Embed colors are RGB payload data, not a request for the theme accent.
        // Explicit black (color = 0) is distinct from an absent color.
        const Color accent_color = embed.has_color
                                       ? Color{static_cast<float>((embed.color >> 16u) & 0xFFu) * rgb_scale,
                                               static_cast<float>((embed.color >> 8u) & 0xFFu) * rgb_scale,
                                               static_cast<float>(embed.color & 0xFFu) * rgb_scale, 1.0f}
                                       : theme.interactive;
        const bool media_only = message_rich_embed_media_only(rich->embeds[index]);
        if (!media_only &&
            (!push_material(paint, owner, card, theme, theme.surface, theme.interactive, 4.0f, 1.0f,
                            UiLayoutContentKind::decoration, UiBackgroundTier::low) ||
             !push_owned(paint, owner, accent, accent_color, accent_color, 0.0f, 0.0f,
                         UiLayoutContentKind::decoration, ui_layout_content_bounds_allow_overlap))) {
            return false;
        }
        MessageRichEmbedMediaSelection selected{};
        float thumb_width = 0, thumb_height = 0;
        if (record &&
            message_rich_embed_thumbnail_extent(
                *rich_store, embed,
                maximum(1.f, ui_rect_width(card) - kMessageEmbedContentInset - kMessageEmbedEndInset),
                &thumb_width, &thumb_height) &&
            message_rich_embed_resource_select(*rich_store, embed, MediaEmbedResource::thumbnail,
                                               &selected)) {
            const UiRect thumb = message_rich_embed_thumbnail_rect(card, thumb_width, thumb_height);
            if (!append_media(media, record->id, MessageMediaCommandKind::embed,
                              static_cast<u8>(selected.format), selected.animated, thumb, row_clip, owner,
                              record->revision, static_cast<u8>(index), static_cast<u8>(selected.resource),
                              false, row.forwarded_snapshot))
                return false;
        }
        // Author and footer icons: round, fetched by proxied URL like other
        // URL-keyed message images.
        UiRect icons[2]{};
        const Utf8View icon_urls[2]{message_rich_embed_author_icon(*rich_store, embed),
                                    message_rich_embed_footer_icon(*rich_store, embed)};
        message_rich_embed_author_icon_rect(*rich_store, embed, row.rich_item_layout, index,
                                            item_layout->content_rectangle, &icons[0]);
        message_rich_embed_footer_icon_rect(*rich_store, embed, row.rich_item_layout, index,
                                            item_layout->content_rectangle, &icons[1]);
        for (u32 icon = 0; record && icon < 2; ++icon) {
            if (!ui_rect_has_area(icons[icon]) || !icon_urls[icon].bytes)
                continue;
            const MediaAttachmentFormat format = message_rich_embed_icon_format(icon_urls[icon]);
            const MediaAssetKey key = message_component_media_key(
                hash64_bytes(kHash64Seed, icon_urls[icon].data, icon_urls[icon].bytes), format);
            if (!append_media(media, key.id, MessageMediaCommandKind::component, static_cast<u8>(format), false,
                              icons[icon], row_clip, owner))
                return false;
            media->storage[media->count - 1].tile_radius =
                static_cast<u8>(ui_rect_width(icons[icon]) * 0.5f);
        }
        const float media_width = row.rich_item_layout.embed_media_widths[index];
        const float media_height = row.rich_item_layout.embed_media_heights[index];
        MessageRichEmbedGallery gallery{};
        if (record && media_width > 0.0f && media_height > 0.0f &&
            message_rich_embed_gallery(*rich_store, *rich, index, &gallery)) {
            const UiRect block = message_rich_embed_media_rect(row.rich_item_layout, index,
                                                               item_layout->content_rectangle, media_only);
            const MessageMediaObscure obscure = message_scene_media_obscure(
                scene, record->id, message_media_reveal_key(record->id, MessageMediaSource::embed, index), false,
                message_rich_embed_explicit(embed));
            if (message_media_obscure_notice(obscure)) {
                if (!push_blocked(paint, owner, block, theme))
                    return false;
                continue;
            }
            for (u32 tile = 0; tile < gallery.count && tile < kMessageEmbedGalleryDrawn; ++tile) {
                const u32 member = gallery.members[tile];
                const UiRect cell = message_rich_embed_gallery_tile(block, gallery.count, tile);
                if (!push_owned(paint, owner, cell, theme.surface_high, theme.surface_high, 0.0f, 0.0f,
                                UiLayoutContentKind::decoration, ui_layout_content_bounds_allow_overlap))
                    return false;
                MessageRichEmbedMediaSelection image{};
                if (!message_rich_embed_gallery_select(*rich_store, rich->embeds[member], &image))
                    continue;
                if (!append_media(media, record->id, MessageMediaCommandKind::embed, static_cast<u8>(image.format),
                                  image.animated, cell, row_clip, owner, record->revision, static_cast<u8>(member),
                                  static_cast<u8>(image.resource), false, row.forwarded_snapshot, obscure))
                    return false;
                media->storage[media->count - 1].cover = 1;
            }
        } else if (record && media_width > 0.0f && media_height > 0.0f &&
            message_rich_embed_media_select(*rich_store, rich->embeds[index], &selected)) {
            const float media_top =
                item_layout->content_rectangle.top + row.rich_item_layout.embed_media_tops[index];
            const float media_left = item_layout->content_rectangle.left + message_rich_embed_media_left(media_only);
            const UiRect preview{media_left, media_top, media_left + media_width, media_top + media_height};
            const MessageMediaObscure obscure = message_scene_media_obscure(
                scene, record->id, message_media_reveal_key(record->id, MessageMediaSource::embed, index), false,
                message_rich_embed_explicit(embed));
            if (message_media_obscure_notice(obscure)) {
                if (!push_blocked(paint, owner, preview, theme))
                    return false;
                continue;
            }
            if (!push_owned(paint, owner, preview, theme.surface_high, theme.surface_high, 4.0f, 0.0f,
                            UiLayoutContentKind::decoration, ui_layout_content_bounds_allow_overlap) ||
                !append_media(media, record->id, MessageMediaCommandKind::embed,
                              static_cast<u8>(selected.format), selected.animated, preview, row_clip, owner,
                              record->revision, static_cast<u8>(index), static_cast<u8>(selected.resource),
                              selected.format == MediaAttachmentFormat::mp4 &&
                                  embed.type != GatewayMessageEmbedType::gifv && obscure == MessageMediaObscure::none,
                              row.forwarded_snapshot, obscure)) {
                return false;
            }
        }
    }

    for (u32 index = 0; index < rich->sticker_count; ++index) {
        const UiWidgetHandle item_widget =
            message_scene_rich_item_widget(row, MessageSceneRichItemKind::sticker, index);
        const UiLayoutNode* item_layout =
            message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::sticker, index);
        if (!item_layout) {
            return false;
        }
        const UiWidgetReference owner{&scene.widgets, item_widget};
        const UiRect rectangle{
            item_layout->rectangle.left, item_layout->rectangle.top,
            item_layout->rectangle.left + kDefaultMessageStickerLayoutMetrics.preview_extent,
            item_layout->rectangle.top + kDefaultMessageStickerLayoutMetrics.preview_extent};
        if (!push_owned(paint, owner, rectangle, theme.surface_high, theme.surface_high, 8.0f, 0.0f,
                        UiLayoutContentKind::decoration, ui_layout_content_bounds_allow_overlap) ||
            !append_media(media, rich->stickers[index].id, MessageMediaCommandKind::sticker,
                          rich->stickers[index].format_type, false, rectangle, row_clip, owner)) {
            return false;
        }
    }

    const UiWidgetHandle hovered_widget = scene.widgets.hovered;
    for (u32 index = 0; index < rich->component_count; ++index) {
        const MessageRichComponent& component = rich->components[index];
        const MessageComponentKind kind = message_component_kind(component);
        if (kind == MessageComponentKind::hidden || kind == MessageComponentKind::row ||
            kind == MessageComponentKind::section || kind == MessageComponentKind::gallery ||
            kind == MessageComponentKind::text)
            continue;
        const UiWidgetHandle item_widget =
            message_scene_rich_item_widget(row, MessageSceneRichItemKind::component, index);
        const UiLayoutNode* item_layout =
            message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::component, index);
        if (!item_layout) {
            return false;
        }
        const UiWidgetReference owner{&scene.widgets, item_widget};
        const UiRect control = item_layout->rectangle;
        if (!ui_rect_has_area(ui_rect_intersection(control, row_clip)))
            continue;
        const bool item_hovered = hovered_widget.slot == item_widget.slot &&
                                  hovered_widget.generation == item_widget.generation && item_widget.generation;
        const bool pending = record && scene.interaction.pending && scene.interaction.message_id == record->id &&
                             scene.interaction.component_index == index && !row.forwarded_snapshot;
        switch (kind) {
        case MessageComponentKind::container: {
            if (!push_material(paint, owner, control, theme, theme.surface, theme.interactive, 10.0f, 1.0f,
                               UiLayoutContentKind::control, UiBackgroundTier::low))
                return false;
            if (component.has_accent_color) {
                constexpr float scale = 1.0f / 255.0f;
                const u32 rgb = component.accent_color;
                const Color color{static_cast<float>((rgb >> 16) & 255) * scale,
                                  static_cast<float>((rgb >> 8) & 255) * scale,
                                  static_cast<float>(rgb & 255) * scale, 1};
                const UiRect accent{control.left, control.top, minimum(control.left + 4, control.right),
                                    control.bottom};
                if (!push_owned(paint, owner, accent, color, color, 2, 0, UiLayoutContentKind::decoration,
                                ui_layout_content_bounds_allow_overlap))
                    return false;
            }
            break;
        }
        case MessageComponentKind::button: {
            const auto colors =
                message_component_button_colors(theme, component.style, component.disabled || pending);
            const Color fill = item_hovered && message_component_interactive(component) && !pending ? colors.hover
                                                                                               : colors.fill;
            if (!push_owned(paint, owner, control, fill, colors.border, 4.0f, 0.0f, UiLayoutContentKind::control))
                return false;
            if (component.emoji_id && record) {
                const float top = control.top + (ui_rect_height(control) - kMessageComponentEmojiExtent) * 0.5f;
                const UiRect emoji{control.left + kMessageComponentButtonPadding, top,
                                   control.left + kMessageComponentButtonPadding + kMessageComponentEmojiExtent,
                                   top + kMessageComponentEmojiExtent};
                if (!append_media(media, component.emoji_id, MessageMediaCommandKind::custom_emoji, 0,
                                  (component.flags & gateway_message_component_emoji_animated) != 0, emoji, row_clip,
                                  owner))
                    return false;
            }
            if (pending) {
                const float extent = 16.0f;
                const float cx = (control.left + control.right) * 0.5f, cy = (control.top + control.bottom) * 0.5f;
                if (!ui_icon_paint(paint, UiIcon::sending,
                                   {cx - extent * 0.5f, cy - extent * 0.5f, cx + extent * 0.5f, cy + extent * 0.5f},
                                   colors.text))
                    return false;
            } else if (message_component_is_link(component)) {
                const float extent = 14.0f;
                const UiRect glyph{control.right - kMessageComponentButtonPadding - extent,
                                   control.top + (ui_rect_height(control) - extent) * 0.5f,
                                   control.right - kMessageComponentButtonPadding,
                                   control.top + (ui_rect_height(control) + extent) * 0.5f};
                if (!ui_icon_paint(paint, UiIcon::external_link, glyph, colors.text))
                    return false;
            }
            break;
        }
        case MessageComponentKind::select: {
            Color fill = theme.tokens.background.input;
            Color border = item_hovered && message_component_interactive(component) ? theme.tokens.border.strong
                                                                               : theme.tokens.border.subtle;
            if (component.disabled || pending) {
                fill.alpha *= 0.5f;
                border.alpha *= 0.5f;
            }
            if (!push_owned(paint, owner, control, fill, border, 4.0f, 1.0f, UiLayoutContentKind::control))
                return false;
            const float extent = 18.0f;
            const UiRect chevron{control.right - extent - 12.0f, control.top + (ui_rect_height(control) - extent) * 0.5f,
                                 control.right - 12.0f, control.top + (ui_rect_height(control) + extent) * 0.5f};
            if (!ui_icon_paint(paint, pending ? UiIcon::sending : UiIcon::chevron_down, chevron,
                               ui_background_muted_text_color(theme)))
                return false;
            break;
        }
        case MessageComponentKind::file: {
            if (!push_material(paint, owner, control, theme, theme.surface, theme.interactive, 6.0f, 1.0f,
                               UiLayoutContentKind::control, UiBackgroundTier::high))
                return false;
            const UiRect glyph{control.left + 12.0f, control.top + (ui_rect_height(control) - 28.0f) * 0.5f,
                               control.left + 40.0f, control.top + (ui_rect_height(control) + 28.0f) * 0.5f};
            if (!ui_icon_paint(paint, UiIcon::file, glyph, theme.text_secondary))
                return false;
            break;
        }
        case MessageComponentKind::separator: {
            if (component.flags & gateway_message_component_divider) {
                const float y = (control.top + control.bottom) * 0.5f;
                const UiRect line{control.left, y - 0.5f, control.right, y + 0.5f};
                if (!push_owned(paint, owner, line, theme.tokens.border.subtle, theme.tokens.border.subtle, 0, 0,
                                UiLayoutContentKind::decoration, ui_layout_content_bounds_allow_overlap))
                    return false;
            }
            break;
        }
        case MessageComponentKind::thumbnail:
        case MessageComponentKind::gallery_item: {
            if (!push_owned(paint, owner, control, theme.surface_high, theme.surface_high, 8.0f, 0.0f,
                            UiLayoutContentKind::decoration, ui_layout_content_bounds_allow_overlap))
                return false;
            MessageComponentMedia resolved{};
            if (record && message_component_media_resolve(*rich_store, component, &resolved)) {
                const MessageMediaObscure obscure = message_scene_media_obscure(
                    scene, 0, message_media_reveal_key(record->id, MessageMediaSource::component, index),
                    message_rich_component_spoiler(component), false);
                if (!append_media(media, resolved.key.id, MessageMediaCommandKind::component, resolved.format, false,
                                  control, row_clip, owner, 0, 0, 0, false, row.forwarded_snapshot, obscure))
                    return false;
            } else if (!ui_icon_paint(paint, UiIcon::image,
                                      {(control.left + control.right) * 0.5f - 12.0f,
                                       (control.top + control.bottom) * 0.5f - 12.0f,
                                       (control.left + control.right) * 0.5f + 12.0f,
                                       (control.top + control.bottom) * 0.5f + 12.0f},
                                      theme.text_secondary)) {
                return false;
            }
            break;
        }
        default:
            break;
        }
    }

    if (row.rich_layout.thread > 0.0f && rich->thread.id) {
        const auto* node = message_scene_layout_node(scene, row.thread);
        if (!node)
            return false;
        const UiRect card = message_thread_card_rectangle(node->rectangle);
        const float icon_top = card.top + (kMessageThreadCardHeightDips - 20.0f) * 0.5f;
        const UiRect glyph{card.left + kMessageThreadCardPaddingDips, icon_top,
                           card.left + kMessageThreadCardPaddingDips + 20.0f, icon_top + 20.0f};
        if (!push_material(paint, UiWidgetReference{&scene.widgets, row.thread}, card, theme, theme.surface,
                           theme.interactive, ui_theme_metrics(scene.theme_styles).control_radius, 1,
                           UiLayoutContentKind::control, UiBackgroundTier::high) ||
            !ui_icon_paint(paint, UiIcon::thread, glyph, theme.text_secondary))
            return false;
    }
    if (rich->reaction_count != 0 && message_rich_display().reactions) {
        for (u32 index = 0; index < rich->reaction_count; ++index) {
            MessageReactionVisualItem reaction{};
            if (!message_reaction_presentation_resolve(scene, row, *rich, index, &reaction)) {
                return false;
            }
            const UiWidgetReference owner{&scene.widgets, reaction.widget};
            if (!push_material(paint, owner, reaction.control_rectangle, theme,
                               reaction.selected ? theme.surface_high : theme.surface,
                               reaction.selected ? theme.tokens.emphasis.outline : theme.interactive, 8.0f, 1.0f,
                               UiLayoutContentKind::control, UiBackgroundTier::high,
                               reaction.selected ? UiBackgroundBorderTone::semantic
                                                 : UiBackgroundBorderTone::palette)) {
                return false;
            }
            if ((reaction.emoji_id != 0 || reaction.regional_flag) &&
                !append_media(media, reaction.regional_flag ? reaction.regional_flag : reaction.emoji_id,
                              reaction.regional_flag ? MessageMediaCommandKind::regional_flag
                                                     : MessageMediaCommandKind::custom_emoji,
                              0, reaction.animated, reaction.emoji_rectangle, row_clip, owner)) {
                return false;
            }
        }
    }

    *geometry_quads += paint->count - geometry_begin;
    return true;
}

// The channel-beginning circle and glyph; its text is a text-presentation
// command and a DM avatar image is drawn over the circle by the renderer.
bool append_intro(const MessageScene& scene, const Theme& theme, PaintStream* paint) noexcept {
    MessageSceneIntroGeometry geometry{};
    if (!message_scene_intro_geometry(scene, &geometry) || !ui_rect_has_area(geometry.icon) || scene.content_gated)
        return true;
    const UiRect clip = ui_rect_intersection(scene.viewport, geometry.area);
    if (!ui_rect_has_area(clip) || !ui_rect_has_area(ui_rect_intersection(clip, geometry.icon)))
        return true;
    const UiWidgetReference owner{&scene.widgets, scene.root};
    const float extent = ui_rect_width(geometry.icon);
    const MessageSceneIntroKind kind = scene.intro.kind;
    const Color fill = kind == MessageSceneIntroKind::direct_message ||
                               kind == MessageSceneIntroKind::group_direct_message
                           ? theme.avatar_placeholder
                           : theme.surface_high;
    if (!push_owned(paint, owner, geometry.icon, fill, fill, extent * 0.5f, 0.0f, UiLayoutContentKind::icon,
                    ui_layout_content_bounds_allow_overlap))
        return false;
    // 42 px channel glyph in the strong icon colour; 32 px thread glyph in the subtle one.
    const bool thread = kind == MessageSceneIntroKind::thread;
    if (kind != MessageSceneIntroKind::channel && !thread)
        return true;
    const float glyph = thread ? 32.0f : 42.0f;
    const float cx = (geometry.icon.left + geometry.icon.right) * 0.5f;
    const float cy = (geometry.icon.top + geometry.icon.bottom) * 0.5f;
    const UiRect bounds{cx - glyph * 0.5f, cy - glyph * 0.5f, cx + glyph * 0.5f, cy + glyph * 0.5f};
    return ui_icon_paint(paint,
                         scene.intro.locked ? UiIcon::lock : thread ? UiIcon::thread : UiIcon::channel, bounds,
                         thread ? theme.text_secondary : theme.text_primary);
}

} // namespace

bool message_media_command_stream_initialize(MessageMediaCommandStream* stream,
                                             Slice<MessageMediaCommand> storage) noexcept {
    if (!stream || !storage.data || storage.count == 0) {
        return false;
    }
    *stream = MessageMediaCommandStream{storage, 0, 0};
    return true;
}

bool message_presentation_build(const MessageScene& scene, const Theme& theme, PaintStream* paint,
                                MessageMediaCommandStream* media,
                                MessagePresentationResult* result) noexcept {
    if (result) {
        *result = MessagePresentationResult{};
    }
    if (!paint || !media || !result || !scene.active || !scene.timeline ||
        !ui_rect_has_area(scene.viewport) || !media->storage.data) {
        return false;
    }
    const u32 paint_count = paint->count;
    const u32 paint_clip_count = paint->clip_count;
    const u32 paint_batch_count = paint->batch_count;
    const u32 paint_clip_depth = paint->clip_depth;
    const u32 paint_rejected = paint->rejected;
    UiPaintContentRecorder* const recorder = paint->content_recorder;
    const u32 content_count = recorder ? recorder->count : 0;
    const u32 content_dropped = recorder ? recorder->dropped : 0;
    const auto rollback = [&]() noexcept {
        paint->count = paint_count;
        paint->clip_count = paint_clip_count;
        paint->batch_count = paint_batch_count;
        paint->clip_depth = paint_clip_depth;
        saturating_increment_if(paint->rejected == paint_rejected, paint->rejected);
        if (recorder) {
            recorder->count = content_count;
            saturating_increment_if(recorder->dropped == content_dropped, recorder->dropped);
        }
        media->count = 0;
        *result = MessagePresentationResult{};
    };
    media->count = 0;
    media->rejected = 0;
    if (!paint_stream_push_clip(paint, scene.viewport.left, scene.viewport.top, scene.viewport.right,
                                scene.viewport.bottom)) {
        rollback();
        return false;
    }
    if (!append_intro(scene, theme, paint)) {
        rollback();
        return false;
    }
    for (u32 index = 0; index < scene.count; ++index) {
        const u32 outer_media_begin = media->count;
        if (!append_row(scene, scene.rows[index], theme, paint, media, &result->geometry_quads)) {
            rollback();
            return false;
        }
        const auto* child = message_scene_snapshot_row(scene, scene.rows[index]);
        const u32 child_media_begin = media->count;
        if (child && !append_row(scene, *child, theme, paint, media, &result->geometry_quads)) {
            rollback();
            return false;
        }
        for (u32 i = outer_media_begin; i < media->count; ++i) {
            media->storage[i].search_result = scene.result_cards;
            if (scene.result_cards)
                media->storage[i].message_revision = scene.rows[index].message_revision;
            if (i >= child_media_begin)
                media->storage[i].snapshot = true;
        }
    }
    const UiScrollbarGeometry scrollbar =
        ui_scrollbar_geometry(scene.viewport, ui_extent_dips(scene.timeline->scroll_units),
                              ui_extent_dips(scene.timeline->list->total_units));
    if (!scene.result_cards && !ui_scrollbar_paint(paint, scrollbar, theme)) {
        rollback();
        return false;
    }
    if (!paint_stream_pop_clip(paint) || media->rejected != 0) {
        rollback();
        return false;
    }
    result->media_commands = media->count;
    return true;
}

} // namespace maicord
