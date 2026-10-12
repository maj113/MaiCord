#pragma once

#include "maicord/ui/message_scene.hpp"

namespace maicord {

struct MessageReactionTextItem {
    Utf8View text;
    UiWidgetHandle widget;
    UiRect content_rectangle;
    bool selected;
};

struct MessageReactionVisualItem {
    UiWidgetHandle widget;
    UiRect control_rectangle;
    UiRect text_rectangle;
    UiRect emoji_rectangle;
    u64 emoji_id;
    bool selected;
    bool animated;
    u16 regional_flag;
};

enum class MessageReactionPresentationError : u8 {
    none,
    invalid_input,
    model,
    layout,
    capacity,
};

struct MessageReactionPresentationResult {
    u32 item_count;
    u32 utf8_bytes;
    MessageReactionPresentationError error;

    constexpr bool ok() const noexcept {
        return error == MessageReactionPresentationError::none;
    }
};

MessageReactionPresentationResult message_reaction_presentation_build(
    const MessageScene& scene, const MessageSceneRowWidget& row, const MessageRichStore& store,
    const MessageRichRecord& rich, Slice<char> utf8, Slice<MessageReactionTextItem> items) noexcept;
bool message_reaction_presentation_resolve(const MessageScene& scene, const MessageSceneRowWidget& row,
                                           const MessageRichRecord& rich, u32 index,
                                           MessageReactionVisualItem* item) noexcept;

static_assert(sizeof(MessageReactionTextItem) == 40);
static_assert(sizeof(MessageReactionVisualItem) == 72);

} // namespace maicord
