#pragma once
#include "maicord/ui/badge.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/message_rich_layout.hpp"
#include "maicord/ui/message_text_presentation.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/model/message_rich_store.hpp"
#include "maicord/model/user_store.hpp"

namespace maicord {

// The "<user> used <command>" header above an application's reply, one 18-DIP
// line to the right of the 16-DIP user avatar:
//   name (muted) · server tag chip (muted) · "used" · command
// The command of a slash command is a chip (grid icon, name without the
// slash); any other command is the name as plain link-coloured text.
inline constexpr float kMessageInteractionNameGap = 4.0f;    // after the name
inline constexpr float kMessageInteractionTagGap = 2.0f;     // before the server tag chip
inline constexpr float kMessageInteractionWordGap = 4.0f;    // a space beside "used"
inline constexpr float kMessageInteractionChipPadding = 2.0f;
inline constexpr float kMessageInteractionChipIcon = 10.0f;
inline constexpr float kMessageInteractionChipIconGap = 3.0f;
inline constexpr float kMessageInteractionChipRadius = 3.0f;
inline constexpr float kMessageInteractionAvatar = 16.0f;

struct MessageInteractionLayout {
    UiTextCommand name, used, command;
    // The interaction user's server tag chip; no label without one.
    UiBadgeWidget tag;
    const UserAppearanceRecord* appearance;
    // The slash command chip and its icon; empty for a plain command name.
    UiRect chip, chip_icon;
    bool chip_hovered;
    u64 user_id, guild_id, message_id, channel_id;
};

enum class MessageInteractionTarget : u8 { none, avatar, name, tag, command };
// MessageScene::header_hover_target values 1..4 are these targets.

struct MessageInteractionHit {
    MessageInteractionTarget target;
    u64 user_id, guild_id, message_id, channel_id;
    // The target's rectangle: popouts anchor to it.
    UiRect anchor;
};

// The row whose header line owns the command, when that line is an
// interaction header ("used" spans on a reply-region command).
inline const MessageSceneRowWidget* message_interaction_command_row(const MessageScene& scene,
                                                                    const MessageTextCommand& c) noexcept {
    // Only the one reply-line command of an interaction row passes these;
    // resolving the row (a scan of the scene) comes last.
    if (c.measurement_kind != MessageTextMeasurementKind::reply || c.styles.count != 2 ||
        !(c.styles[0].flags & text_style_inline_role_color) ||
        !(c.styles[1].flags & (text_style_mention | text_style_link)) || c.owner.manager != &scene.widgets)
        return nullptr;
    const auto* row = message_scene_resolve(scene, c.owner.handle);
    return row && c.owner.handle.slot == row->reply.slot && c.owner.handle.generation == row->reply.generation &&
                   c.message_id == row->message_id && c.message_revision == row->message_revision
               ? row
               : nullptr;
}

inline bool message_interaction_layout(const MessageScene& scene, const MessageTextCommand& command,
                                       const UserStore& users, UiTextMeasureCache* cache, const Theme& theme,
                                       MessageInteractionLayout* out) noexcept {
    if (!out || !scene.timeline || !scene.timeline->store || !scene.timeline->rich_store)
        return false;
    const auto* row = message_interaction_command_row(scene, command);
    if (!row)
        return false;
    const auto* record = message_store_resolve(*scene.timeline->store, row->message);
    const auto* rich = record ? message_rich_store_find(*scene.timeline->rich_store, record->id) : nullptr;
    if (!rich || rich->message_revision != record->revision || rich->reference.state != GatewayMessageReferenceState::none ||
        !message_rich_has_interaction(*rich))
        return false;
    const TextStyleSpan& name_span = command.styles[0];
    const TextStyleSpan& command_span = command.styles[1];
    if (name_span.byte_offset || name_span.byte_count > command.text.bytes ||
        command_span.byte_offset > command.text.bytes ||
        command_span.byte_count > command.text.bytes - command_span.byte_offset ||
        name_span.byte_count >= command_span.byte_offset)
        return false;
    const Utf8View name{command.text.data, name_span.byte_count};
    const Utf8View used{command.text.data + name_span.byte_count + 1u,
                        command_span.byte_offset - name_span.byte_count - 2u};
    const Utf8View label{command.text.data + command_span.byte_offset, command_span.byte_count};
    if (!name.bytes || !used.bytes || !label.bytes)
        return false;
    const bool chip = (command_span.flags & text_style_mention) != 0;
    const u64 user_id = rich->interaction.user_id;
    const auto measure = [&](Utf8View text, u32 flags, u16 salt) noexcept {
        const TextStyleSpan span{0, text.bytes, flags};
        UiTextMeasureValue value{};
        const bool ok =
            cache && ui_text_measure_cache_measure(
                         cache,
                         {{user_id, hash64_bytes(kHash64Seed, text.data, text.bytes), 0, salt, 0},
                          text,
                          flags ? ConstSlice<TextStyleSpan>{&span, 1} : ConstSlice<TextStyleSpan>{},
                          {maximum(1.f, ui_rect_width(command.rectangle)), 64},
                          true},
                         &value)
                         .ok();
        return ok ? value.size.width + 2.0f : text.bytes * 8.0f;
    };
    reset_value(out);
    out->user_id = user_id;
    out->message_id = record->id;
    out->channel_id = record->channel_id;
    out->appearance = user_appearance_find(users.appearance, user_id);
    const auto& line = command.rectangle;
    const float right = line.right;
    float x = line.left;
    const auto push = [&](UiTextCommand* target, Utf8View text, float left, float width, const Color& color,
                          u32 flags) noexcept {
        const float end = minimum(right, left + width);
        *target = {text,
                   command.owner,
                   {left, line.top, maximum(left, end), line.bottom},
                   command.clip,
                   color,
                   flags,
                   UiTextAlignment::start,
                   UiTextAlignment::center,
                   TextWrapping::no_wrap,
                   TextOverflow::ellipsis};
    };
    const u8 hover = scene.header_hover_message_id == record->id ? scene.header_hover_target : u8{0};
    out->chip_hovered = hover == static_cast<u8>(MessageInteractionTarget::command);
    const Color name_color = command.inline_role_color.alpha > 0.0f ? command.inline_role_color : command.color;
    push(&out->name, name, x, measure(name, text_style_bold, 0x4948),
         ui_theme_color_alpha(name_color, kMessageReplyAuthorOpacity),
         text_style_bold | (hover == static_cast<u8>(MessageInteractionTarget::name) ? text_style_underline : 0u));
    x = out->name.rectangle.right + kMessageInteractionNameGap;
    const auto* appearance = out->appearance;
    if (appearance && appearance->primary_enabled && appearance->primary_tag.bytes &&
        appearance->primary_guild_id && x < right) {
        auto& tag = out->tag;
        tag.owner = command.owner.handle;
        tag.label = appearance->primary_tag.view();
        tag.style = text_style_subtext | text_style_bold;
        tag.leading = true;
        tag.compact = true;
        tag.opacity = kMessageReplyAuthorOpacity;
        tag.hovered = hover == static_cast<u8>(MessageInteractionTarget::tag);
        x += kMessageInteractionTagGap;
        const auto geometry = ui_badge_compact_layout(cache, tag.label, {x, line.top, right, line.bottom}, true, tag.style);
        tag.rectangle = geometry.outer;
        out->guild_id = appearance->primary_guild_id;
        x = tag.rectangle.right;
    }
    x += kMessageInteractionWordGap;
    push(&out->used, used, x, measure(used, 0, 0x4955), command.color, 0);
    x = out->used.rectangle.right + kMessageInteractionWordGap;
    if (chip) {
        const float mark = kMessageInteractionChipIcon + kMessageInteractionChipIconGap;
        const float width = 2 * kMessageInteractionChipPadding + mark + measure(label, text_style_bold, 0x4943);
        const float cy = (line.top + line.bottom) * .5f;
        out->chip = {x, cy - (line.bottom - line.top) * .5f, minimum(right, x + width), cy + (line.bottom - line.top) * .5f};
        out->chip_icon = {x + kMessageInteractionChipPadding, cy - kMessageInteractionChipIcon * .5f,
                          x + kMessageInteractionChipPadding + kMessageInteractionChipIcon,
                          cy + kMessageInteractionChipIcon * .5f};
        push(&out->command, label, x + kMessageInteractionChipPadding + mark,
             out->chip.right - kMessageInteractionChipPadding - (x + kMessageInteractionChipPadding + mark),
             out->chip_hovered ? theme.tokens.emphasis.text : ui_foreground_color(theme, theme.tokens.mention.text),
             text_style_bold);
        out->command.rectangle.right = maximum(out->command.rectangle.left, out->chip.right - kMessageInteractionChipPadding);
    } else {
        push(&out->command, label, x, right - x, ui_foreground_color(theme, theme.tokens.text.link), text_style_bold);
    }
    return true;
}

// The chip's fill and icon (the server tag chip paints through ui_badge_paint).
inline bool message_interaction_paint(const MessageInteractionLayout& layout, const MessageTextCommand& command,
                                      const Theme& theme, PaintStream* paint) noexcept {
    if (!ui_rect_has_area(layout.chip))
        return true;
    if (!paint_stream_push_clip(paint, command.clip))
        return false;
    const bool ok = paint_stream_push_rounded(paint, layout.chip,
                                              layout.chip_hovered ? theme.tokens.emphasis.fill
                                                                  : theme.tokens.mention.inline_background,
                                              kMessageInteractionChipRadius) &&
                    ui_icon_paint(paint, UiIcon::picker, layout.chip_icon,
                                  layout.chip_hovered ? theme.tokens.emphasis.text
                                                      : ui_foreground_color(theme, theme.tokens.mention.text));
    return paint_stream_pop_clip(paint) && ok;
}

// What lies under a point of an interaction header: the user's avatar and
// name (profile), the server tag chip (server tag popout) and the command.
inline MessageInteractionHit message_interaction_hit_test(const MessageScene& scene,
                                                          const MessageTextPresentation& text, const UserStore& users,
                                                          UiTextMeasureCache* cache, const Theme& theme, float x,
                                                          float y) noexcept {
    if (!scene.active || !scene.timeline || !scene.timeline->store || !ui_rect_contains(scene.viewport, x, y))
        return {};
    for (u32 i = 0; i < text.command_count; ++i) {
        const auto& command = text.commands[i];
        if (!ui_rect_contains(ui_rect_intersection(command.clip, scene.viewport), x, y) ||
            !message_interaction_command_row(scene, command))
            continue;
        MessageInteractionLayout layout{};
        if (!message_interaction_layout(scene, command, users, cache, theme, &layout))
            continue;
        const auto at = [&](MessageInteractionTarget target, UiRect rectangle) noexcept -> MessageInteractionHit {
            return ui_rect_contains(ui_rect_intersection(rectangle, command.clip), x, y)
                       ? MessageInteractionHit{target, layout.user_id, layout.guild_id, layout.message_id,
                                               layout.channel_id, rectangle}
                       : MessageInteractionHit{};
        };
        const float cy = (command.rectangle.top + command.rectangle.bottom) * .5f;
        // The compact display shows a reply badge, not an avatar.
        const UiRect avatar = scene.display.compact
                                  ? UiRect{}
                                  : UiRect{command.rectangle.left - kMessageReplyTextInset,
                                           cy - kMessageInteractionAvatar * .5f,
                                           command.rectangle.left - kMessageReplyTextInset + kMessageInteractionAvatar,
                                           cy + kMessageInteractionAvatar * .5f};
        const UiRect command_rectangle = ui_rect_has_area(layout.chip) ? layout.chip : layout.command.rectangle;
        const MessageInteractionHit hits[4]{
            at(MessageInteractionTarget::avatar, avatar),
            at(MessageInteractionTarget::name, layout.name.rectangle),
            at(MessageInteractionTarget::tag, layout.tag.label.bytes ? layout.tag.rectangle : UiRect{}),
            at(MessageInteractionTarget::command, command_rectangle)};
        for (const auto& hit : hits)
            if (hit.target != MessageInteractionTarget::none)
                return hit;
    }
    return {};
}

// The command chip (a plain command name's rectangle) of one message's
// interaction header in the visible lines: the popout anchors to it. False
// when that header is not laid out.
inline bool message_interaction_command_anchor(const MessageScene& scene, const MessageTextPresentation& text,
                                               const UserStore& users, UiTextMeasureCache* cache, const Theme& theme,
                                               u64 message_id, UiRect* anchor) noexcept {
    for (u32 i = 0; anchor && i < text.command_count; ++i) {
        const auto& command = text.commands[i];
        if (command.message_id != message_id || !message_interaction_command_row(scene, command))
            continue;
        MessageInteractionLayout layout{};
        if (!message_interaction_layout(scene, command, users, cache, theme, &layout))
            continue;
        *anchor = ui_rect_has_area(layout.chip) ? layout.chip : layout.command.rectangle;
        return ui_rect_intersects(*anchor, scene.viewport);
    }
    return false;
}

// A quoted reply's author: the 16-DIP avatar and the name at the start of the
// preview line open that user's profile (the rest of the line jumps to the
// quoted message). Quoted join and app notices show no author.
inline MessageInteractionHit message_reply_author_hit_test(const MessageScene& scene,
                                                           const MessageTextPresentation& text,
                                                           UiTextMeasureCache* cache, float x, float y) noexcept {
    if (!scene.active || !scene.timeline || !scene.timeline->store || !scene.timeline->rich_store ||
        !ui_rect_contains(scene.viewport, x, y))
        return {};
    for (u32 i = 0; i < text.command_count; ++i) {
        const auto& command = text.commands[i];
        if (command.measurement_kind != MessageTextMeasurementKind::reply || !command.styles.count ||
            command.styles[0].byte_offset || !(command.styles[0].flags & text_style_inline_role_color) ||
            command.styles[0].byte_count > command.text.bytes || command.owner.manager != &scene.widgets ||
            !ui_rect_contains(ui_rect_intersection(command.clip, scene.viewport), x, y))
            continue;
        const auto* row = message_scene_resolve(scene, command.owner.handle);
        if (!row || command.owner.handle.slot != row->reply.slot ||
            command.owner.handle.generation != row->reply.generation || command.message_id != row->message_id ||
            command.message_revision != row->message_revision)
            continue;
        const auto* record = message_store_resolve(*scene.timeline->store, row->message);
        const auto* rich = record ? message_rich_store_find(*scene.timeline->rich_store, record->id) : nullptr;
        if (!rich || rich->message_revision != record->revision || !rich->reference.author_id ||
            rich->reference.state != GatewayMessageReferenceState::available ||
            message_rich_reply_leading(rich->reference) != MessageReplyLeading::avatar)
            continue;
        const Utf8View name{command.text.data, command.styles[0].byte_count};
        const TextStyleSpan span{0, name.bytes, text_style_bold};
        UiTextMeasureValue value{};
        const bool measured =
            cache && ui_text_measure_cache_measure(
                         cache,
                         {{rich->reference.author_id, hash64_bytes(kHash64Seed, name.data, name.bytes), 0, 0x4952, 0},
                          name,
                          {&span, 1},
                          {maximum(1.f, ui_rect_width(command.rectangle)), 64},
                          true},
                         &value)
                         .ok();
        const float cy = (command.rectangle.top + command.rectangle.bottom) * .5f;
        const UiRect name_rectangle{command.rectangle.left, command.rectangle.top,
                                    minimum(command.rectangle.right,
                                            command.rectangle.left + (measured ? value.size.width + 2.0f : name.bytes * 8.0f)),
                                    command.rectangle.bottom};
        const UiRect avatar = scene.display.compact
                                  ? UiRect{}
                                  : UiRect{command.rectangle.left - kMessageReplyTextInset,
                                           cy - kMessageInteractionAvatar * .5f,
                                           command.rectangle.left - kMessageReplyTextInset + kMessageInteractionAvatar,
                                           cy + kMessageInteractionAvatar * .5f};
        const UiRect hit_rectangle = ui_rect_contains(avatar, x, y) ? avatar : name_rectangle;
        if (ui_rect_contains(ui_rect_intersection(hit_rectangle, command.clip), x, y))
            return {ui_rect_contains(avatar, x, y) ? MessageInteractionTarget::avatar : MessageInteractionTarget::name,
                    rich->reference.author_id, 0, record->id, record->channel_id, hit_rectangle};
    }
    return {};
}

} // namespace maicord
