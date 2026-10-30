#pragma once
#include "maicord/core/slice.hpp"
#include "maicord/ui/tooltip.hpp"

namespace maicord::app_detail {
// An emoji in a message's body text (content, reply preview, embeds, text
// components) or a sticker's preview: what its tooltip and popout show.
enum class MessageExpressionKind : u8 {
    none,
    unicode_emoji,
    custom_emoji,
    regional_flag,
    sticker,
};
struct MessageExpression {
    UiWidgetReference owner; // The text owner or the sticker's item.
    UiRect anchor;           // The emoji's cell or the sticker's preview.
    u64 message_id;
    u64 id; // Custom emoji, flag sequence or sticker; zero for a Unicode emoji.
    u32 message_revision;
    u32 source; // Unique within the message: its text command and offset, or its sticker slot.
    // A custom emoji's <:name:id> tag in the message's source text.
    u32 source_offset;
    u16 source_bytes;
    MessageExpressionKind kind;
    bool animated, snapshot;
    u8 sticker_format;
    u8 sticker_index;
    // The Unicode emoji or flag as displayed; borrowed from the current
    // message text until its next build.
    Utf8View glyph;
};
// The expression under a point in the active channel's current messages.
bool application_message_expression_at(UiPoint point, MessageExpression* output) noexcept;
// The name the official client shows: ":joy:" (a Unicode emoji's primary
// shortcode), ":custom:", a known custom emoji's bare name when its server
// does not require colons, or a sticker's name. Zero bytes when unnamed.
u32 application_message_expression_name(const MessageExpression& expression, Slice<char> output) noexcept;
bool application_message_expression_same(const MessageExpression& a, const MessageExpression& b) noexcept;

// Hovering an expression shows the official tooltip above it after 300 ms:
// an emoji 48 DIPs large beside its bold name, or the sticker icon leading the
// sticker's bold name; both with the grey "Click to learn more" (the click
// opens its popout). Not while that expression's popout is open. Sets the
// target's style; false when the point is on no expression.
bool application_emoji_tooltip_target(UiPoint point, u64 scope, UiTooltipTarget* target,
                                      UiTooltipStyle* style) noexcept;
} // namespace maicord::app_detail
