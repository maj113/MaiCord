#include "maicord/ui/message_reaction_presentation.hpp"

#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/emoji_catalog.hpp"
#include "maicord/ui/regional_flag.hpp"

namespace maicord {
namespace {

bool append_decimal(u32 value, Slice<char> output, u32* cursor) noexcept {
    if (decimal_digits(value) > output.count - *cursor) {
        return false;
    }
    *cursor += decimal_write(output.data + *cursor, value);
    return true;
}

bool label_bytes(const MessageRichStore& store, const MessageRichReaction& reaction, u32* bytes) noexcept {
    u32 required = decimal_digits(reaction.count);
    if (reaction.emoji_id == 0 && !regional_flag_sequence(message_rich_store_string(store, reaction.name))) {
        const Utf8View name = message_rich_store_string(store, reaction.name);
        if (!name.data && name.bytes != 0) {
            return false;
        }
        if (name.bytes > ~0u - required - (name.bytes != 0 ? 1u : 0u)) {
            return false;
        }
        required += name.bytes + (name.bytes != 0 ? 1u : 0u);
    }
    *bytes = required;
    return true;
}

bool append_label(const MessageRichStore& store, const MessageRichReaction& reaction, Slice<char> output,
                  u32* cursor, Utf8View* label) noexcept {
    const u32 begin = *cursor;
    if (reaction.emoji_id == 0 && !regional_flag_sequence(message_rich_store_string(store, reaction.name))) {
        const Utf8View name = message_rich_store_string(store, reaction.name);
        if ((!name.data && name.bytes != 0) || name.bytes > output.count - *cursor) {
            return false;
        }
        copy_bytes(output.data + *cursor, name.data, name.bytes);
        *cursor += name.bytes;
        if (name.bytes != 0) {
            if (*cursor == output.count) {
                return false;
            }
            output.data[(*cursor)++] = ' ';
        }
    }
    if (!append_decimal(reaction.count, output, cursor)) {
        return false;
    }
    *label = Utf8View{output.data + begin, *cursor - begin};
    return label->bytes != 0;
}

MessageReactionPresentationResult error(MessageReactionPresentationError value) noexcept {
    return MessageReactionPresentationResult{0, 0, value};
}

} // namespace

bool message_reaction_presentation_resolve(const MessageScene& scene, const MessageSceneRowWidget& row,
                                           const MessageRichRecord& rich, u32 index,
                                           MessageReactionVisualItem* item) noexcept {
    if (!item || row.message_id == 0 || row.message_id != rich.message_id ||
        row.message_revision != rich.message_revision || row.reaction_button_count != rich.reaction_count ||
        index >= rich.reaction_count) {
        return false;
    }
    const UiWidget* region = ui_widget_manager_resolve(scene.widgets, row.reactions);
    const UiWidgetHandle handle = row.reaction_buttons[index];
    const UiWidget* widget = ui_widget_manager_resolve(scene.widgets, handle);
    const UiLayoutNode* layout = message_scene_layout_node(scene, handle);
    if (!region || !widget || !layout || widget->parent.slot != row.reactions.slot ||
        widget->parent.generation != row.reactions.generation || widget->kind != UiWidgetKind::button ||
        !ui_rect_has_area(layout->rectangle) || !ui_rect_has_area(layout->content_rectangle)) {
        return false;
    }
    const MessageRichReaction& reaction = rich.reactions[index];
    const auto* store = scene.timeline ? scene.timeline->rich_store : nullptr;
    const u16 flag = reaction.emoji_id == 0 && store
        ? regional_flag_sequence(message_rich_store_string(*store, reaction.name)) : 0;
    UiRect text = layout->content_rectangle;
    // The 24-DIP reaction pill lifts the emoji cell and the 14-DIP count by one
    // DIP: geometric centering looks low because both glyph sets carry more
    // descent than visible ink.
    constexpr float optical_lift = 1.0f;
    text.top -= optical_lift;
    text.bottom -= optical_lift;
    UiRect emoji{};
    if (reaction.emoji_id != 0 || flag) {
        emoji.left = layout->content_rectangle.left;
        emoji.right = emoji.left + kDefaultMessageReactionLayoutMetrics.emoji_extent;
        emoji.top =
            layout->rectangle.top +
            (ui_rect_height(layout->rectangle) - kDefaultMessageReactionLayoutMetrics.emoji_extent) * 0.5f -
            optical_lift;
        emoji.bottom = emoji.top + kDefaultMessageReactionLayoutMetrics.emoji_extent;
        if (!ui_rect_has_area(emoji) || emoji.left < layout->rectangle.left ||
            emoji.right > layout->content_rectangle.right) {
            return false;
        }
        text.left = emoji.right + kDefaultMessageReactionLayoutMetrics.emoji_gap;
    } else {
        text.left += kDefaultMessageReactionLayoutMetrics.unicode_text_inset;
    }
    if (!ui_rect_has_area(text) || text.left < layout->content_rectangle.left ||
        text.right > layout->content_rectangle.right) {
        return false;
    }
    *item = MessageReactionVisualItem{handle,
                                      layout->rectangle,
                                      text,
                                      emoji,
                                      reaction.emoji_id,
                                      (reaction.flags & 1u) != 0,
                                      (reaction.flags & 4u) != 0};
    item->regional_flag = flag;
    return true;
}

MessageReactionPresentationResult message_reaction_presentation_build(
    const MessageScene& scene, const MessageSceneRowWidget& row, const MessageRichStore& store,
    const MessageRichRecord& rich, Slice<char> utf8, Slice<MessageReactionTextItem> items) noexcept {
    if ((!utf8.data && utf8.count != 0) || (!items.data && items.count != 0) ||
        rich.reaction_count > kGatewayMessageReactionMaximum) {
        return error(MessageReactionPresentationError::invalid_input);
    }
    if (row.message_id == 0 || row.message_id != rich.message_id ||
        row.message_revision != rich.message_revision) {
        return error(MessageReactionPresentationError::model);
    }
    if (row.reaction_button_count != rich.reaction_count || items.count < rich.reaction_count) {
        return error(MessageReactionPresentationError::capacity);
    }
    u32 required_bytes = 0;
    for (u32 index = 0; index < rich.reaction_count; ++index) {
        MessageReactionVisualItem visual{};
        if (!message_reaction_presentation_resolve(scene, row, rich, index, &visual)) {
            return error(MessageReactionPresentationError::layout);
        }
        u32 item_bytes = 0;
        if (!label_bytes(store, rich.reactions[index], &item_bytes)) {
            return error(MessageReactionPresentationError::model);
        }
        if (required_bytes > utf8.count || item_bytes > utf8.count - required_bytes) {
            return error(MessageReactionPresentationError::capacity);
        }
        required_bytes += item_bytes;
    }

    u32 utf8_count = 0;
    for (u32 index = 0; index < rich.reaction_count; ++index) {
        MessageReactionVisualItem visual{};
        if (!message_reaction_presentation_resolve(scene, row, rich, index, &visual)) {
            return error(MessageReactionPresentationError::layout);
        }
        Utf8View label{};
        if (!append_label(store, rich.reactions[index], utf8, &utf8_count, &label)) {
            return error(MessageReactionPresentationError::capacity);
        }
        items[index] =
            MessageReactionTextItem{label, visual.widget, visual.text_rectangle, visual.selected};
    }
    return MessageReactionPresentationResult{rich.reaction_count, required_bytes,
                                             MessageReactionPresentationError::none};
}

namespace {
struct LabelWriter {
    Slice<char> output;
    u32 bytes;
    bool ok;
    __declspec(noinline) void append(const char* source, u32 count) noexcept {
        if (!ok || !output.data || count > output.count - bytes) {
            ok = false;
            return;
        }
        copy_bytes(output.data + bytes, source, count);
        bytes += count;
    }
    void add(const Utf8View& text) noexcept {
        append(text.data, text.bytes);
    }
    template <usize Count> __forceinline void add(const char (&text)[Count]) noexcept {
        append(text, Count - 1);
    }
};
} // namespace

MessageReactionTooltipLabel message_reaction_tooltip_label(u64 emoji_id, Utf8View emoji,
                                                           ConstSlice<Utf8View> names, u32 count,
                                                           Slice<char> output,
                                                           Slice<TextStyleSpan> bold) noexcept {
    LabelWriter out{output, 0, emoji.data && emoji.bytes};
    char shortcode[64];
    const u32 shortcode_bytes = emoji_id ? 0 : emoji_shortcode_key(emoji, {shortcode, sizeof(shortcode)});
    if (emoji_id || shortcode_bytes) {
        out.add(":");
        out.add(emoji_id ? emoji : Utf8View{shortcode, shortcode_bytes});
        out.add(":");
    } else {
        out.add(emoji);
    }
    if (!out.ok)
        return {};
    const u32 heading = out.bytes;
    const u32 shown = names.count, others = count > shown ? count - shown : 0;
    if (!shown || shown > bold.count)
        return {heading, 0};
    out.add(" reacted by ");
    for (u32 i = 0; i < shown; ++i) {
        if (i)
            out.add(i + 1 == shown && !others ? (shown == 2 ? utf8_literal(" and ") : utf8_literal(", and "))
                                              : utf8_literal(", "));
        bold[i] = {out.bytes, names[i].bytes, text_style_bold};
        out.add(names[i]);
    }
    if (others) {
        out.add(shown == 1 ? utf8_literal(" and ") : utf8_literal(", and "));
        char digits[10];
        out.add({digits, decimal_write(digits, others)});
        out.add(others == 1 ? utf8_literal(" other") : utf8_literal(" others"));
    }
    return out.ok ? MessageReactionTooltipLabel{out.bytes, shown} : MessageReactionTooltipLabel{heading, 0};
}

} // namespace maicord
