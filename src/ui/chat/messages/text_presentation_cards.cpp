#include "text_presentation_internal.hpp"
#include "maicord/ui/theme/style_projection.hpp"
#include "maicord/ui/plugins/slots.hpp"
#include "maicord/ui/voice/audio_presentation.hpp"
#include "maicord/ui/chat/polls/presentation.hpp"
#include "maicord/ui/text/timestamp_format.hpp"
#include "maicord/ui/chat/messages/component_layout.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/model/guild/invite_card_cache.hpp"
#include "maicord/model/guild/scheduled_event_store.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/model/user/store.hpp"
#include "maicord/model/chat/chat_state.hpp"
#include "maicord/ui/theme/background_presentation.hpp"

namespace maicord {
using namespace text_presentation_detail;

namespace text_presentation_detail {
void set_failure_context(MessageTextPresentation* presentation, MessageTextPresentationStage stage,
                         MessageTextPresentationOperation operation,
                         u16 item_index,
                         UiTextFlowError flow_error) noexcept {
    presentation->failure_stage = stage;
    presentation->failure_operation = operation;
    presentation->failure_item_index = item_index;
    presentation->failure_flow_error = flow_error;
}

namespace {
constexpr CommandLook kLookMiddleWrapped{.vertical = UiTextAlignment::center};
constexpr CommandLook kLookNotice{.kind = MessageTextMeasurementKind::notice,
                                  .vertical = UiTextAlignment::center,
                                  .wrapping = TextWrapping::no_wrap,
                                  .overflow = TextOverflow::ellipsis};
constexpr CommandLook kLookNoticeCentered{.kind = MessageTextMeasurementKind::notice,
                                          .horizontal = UiTextAlignment::center,
                                          .vertical = UiTextAlignment::center,
                                          .wrapping = TextWrapping::no_wrap,
                                          .overflow = TextOverflow::ellipsis};
constexpr CommandLook kLookNoticeLink{.kind = MessageTextMeasurementKind::notice,
                                      .themed = true,
                                      .vertical = UiTextAlignment::center,
                                      .wrapping = TextWrapping::no_wrap,
                                      .slot = 1,
                                      .overflow = TextOverflow::ellipsis};
constexpr CommandLook kLookAttachmentName{.kind = MessageTextMeasurementKind::attachment,
                                          .minimum_height = 20.0f,
                                          .wrapping = TextWrapping::no_wrap,
                                          .overflow = TextOverflow::ellipsis};
constexpr CommandLook kLookBlock{};
UiRect line_rectangle(float x, float y, float width) noexcept {
    return UiRect{x, y, x + maximum(width, 1.0f), y + 4'096.0f};
}

} // namespace
// Generates the system line (with a trailing muted time) into the retained
// buffers; names open profiles and references become actions.
bool retain_system_text(MessageTextPresentation* presentation, const ChatStateStore& state, const UserStore* users,
                        const MessageRecord& record, const MessageRichStore* rich_store,
                        const MessageRichRecord* rich, Utf8View content, u64 now_unix_seconds, Utf8View* text,
                        ConstSlice<TextStyleSpan>* styles, ConstSlice<MessageMarkdownSemantic>* semantics) noexcept {
    MessageSystemTextInput input{};
    input.type = record.type;
    input.message_id = record.id;
    input.timestamp_ms = record.timestamp_ms;
    input.content = content;
    const UserRecord* author = users && record.author_id ? user_store_find(*users, record.author_id) : nullptr;
    input.author = member_nickname(*presentation, state, record.channel_id, record.author_id);
    if (!input.author.bytes)
        input.author = author ? user_store_name(*users, *author) : Utf8View{};
    const ChannelRecord* channel = chat_state_store_find_channel(state, record.channel_id);
    if (channel && channel->guild_id) {
        if (const GuildRecord* guild = chat_state_store_find_guild(state, channel->guild_id))
            input.guild = chat_state_store_guild_name(state, *guild);
        // Forum and media channel posts are threads whose parent is a forum.
        const ChannelRecord* parent = channel->parent_id ? chat_state_store_find_channel(state, channel->parent_id)
                                                         : nullptr;
        input.forum_post = parent && (parent->type == 15 || parent->type == 16);
    }
    u64 target_id = 0;
    if (rich && rich_store) {
        target_id = rich->system.target_user_id;
        input.target = message_rich_store_string(*rich_store, rich->system.target_name);
        const Utf8View target_nickname =
            member_nickname(*presentation, state, record.channel_id, target_id);
        if (target_nickname.bytes) {
            input.target = target_nickname;
        } else if (target_id && users) {
            if (const UserRecord* target = user_store_find(*users, target_id)) {
                const Utf8View name = user_store_name(*users, *target);
                if (name.bytes)
                    input.target = name;
            }
        }
        input.same_user = target_id && target_id == record.author_id;
        input.call_present = rich->system.call_present;
        input.call_ended = rich->system.call_ended;
        input.call_participated = rich->system.call_participated;
        input.call_ended_ms = rich->system.call_ended_ms;
        if (record.type == message_type_poll_result) {
            input.poll_question = poll_result_field(*rich_store, *rich, utf8_literal("poll_question_text"));
        }
    }
    const u32 available = presentation->utf8.count - presentation->utf8_count;
    MessageSystemText system{};
    char* const begin = presentation->utf8.data + presentation->utf8_count;
    if (!message_system_text_build(input, {begin, available}, &system))
        return fail(presentation, MessageTextPresentationError::capacity);
    u32 bytes = system.text.bytes;
    char timestamp[kMessageMarkdownTimestampMaximumBytes];
    const TimestampFormatResult time =
        record.timestamp_ms ? format_timestamp_calendar(record.timestamp_ms / 1'000u, now_unix_seconds,
                                                        Slice<char>{timestamp, sizeof(timestamp)})
                            : TimestampFormatResult{};
    const u32 time_bytes = time.ok ? time.bytes : 0u;
    if (bytes + (time_bytes ? 2u + time_bytes : 0u) > available ||
        system.span_count + 1u > presentation->styles.count - presentation->style_count ||
        system.span_count > presentation->semantics.count - presentation->semantic_count)
        return fail(presentation, MessageTextPresentationError::capacity);
    const u32 style_begin = presentation->style_count;
    const u32 semantic_begin = presentation->semantic_count;
    for (u32 i = 0; i < system.span_count; ++i) {
        const MessageSystemSpan& span = system.spans[i];
        u64 target = 0;
        MessageMarkdownSemanticKind kind = MessageMarkdownSemanticKind::none;
        u32 flags = text_style_bold;
        switch (span.kind) {
        case MessageSystemSpanKind::author:
            target = record.author_id;
            kind = MessageMarkdownSemanticKind::user_mention;
            break;
        case MessageSystemSpanKind::target:
            target = target_id;
            kind = MessageMarkdownSemanticKind::user_mention;
            break;
        case MessageSystemSpanKind::message:
            target = rich ? rich->reference.message_id : 0;
            kind = MessageMarkdownSemanticKind::message_link;
            flags = text_style_bold | text_style_link;
            break;
        case MessageSystemSpanKind::thread:
            target = rich ? rich->reference.channel_id : 0;
            kind = MessageMarkdownSemanticKind::channel_mention;
            flags = text_style_bold | text_style_link;
            break;
        case MessageSystemSpanKind::pins:
            target = kSystemPinsTarget;
            kind = MessageMarkdownSemanticKind::link;
            flags = text_style_bold | text_style_link;
            break;
        case MessageSystemSpanKind::emphasis:
            break;
        }
        presentation->styles[presentation->style_count++] = TextStyleSpan{span.offset, span.bytes, flags};
        if (target && kind != MessageMarkdownSemanticKind::none)
            presentation->semantics[presentation->semantic_count++] =
                MessageMarkdownSemantic{target, span.offset, span.bytes, 0, 0, kind, 0};
    }
    if (time_bytes) {
        begin[bytes++] = ' ';
        begin[bytes++] = ' ';
        copy_bytes(begin + bytes, timestamp, time_bytes);
        presentation->styles[presentation->style_count++] =
            TextStyleSpan{bytes, time_bytes, text_style_subtext | text_style_timestamp};
        bytes += time_bytes;
    }
    presentation->utf8_count += bytes;
    *text = {begin, bytes};
    *styles = {presentation->styles.data + style_begin, presentation->style_count - style_begin};
    *semantics = {presentation->semantics.data + semantic_begin, presentation->semantic_count - semantic_begin};
    return true;
}

// Label of an obscured or blocked preview: inside the scrim pill, or across a
// blocked notice box.
bool append_obscure_label(MessageTextPresentation* presentation, MessageMediaObscure obscure, const UiRect& media,
                          const UiWidgetReference& owner, const UiRect& clip, const Theme& theme, const MessageRecord& record,
                          u16 measurement) noexcept {
    if (obscure == MessageMediaObscure::none)
        return true;
    const Utf8View label = message_media_obscure_label(obscure);
    const bool notice = message_media_obscure_notice(obscure);
    const UiRect box = notice
                           ? UiRect{media.left + 12.0f, media.top, media.right - 12.0f, media.bottom}
                           : message_media_obscure_pill(media, obscure);
    const UiRect visible = ui_rect_intersection(clip, box);
    if (!ui_rect_has_area(box) || !ui_rect_has_area(visible) || box.right - box.left < 8.0f)
        return true;
    return append_marked(presentation, label, notice ? TextStyleSpan{} : TextStyleSpan{0, label.bytes, text_style_bold},
                         nullptr, owner, box, visible,
                         notice ? ui_background_muted_text_color(theme) : theme.tokens.text.on_accent, &record,
                         measurement, kLookNoticeCentered);
}

namespace {
// Show image descriptions: an attachment with a description shows "ALT" 4 DIPs inside its bottom-left corner, the
// official client's 14 px semibold label on a 32-DIP scrim pill (renderer_layout_media.cpp paints the pill around the
// label's glyphs); clicking it opens the description's popout (tooltip.cpp). Small previews and hidden media show
// none. The label's target is the attachment's index, with kMessageAltSnapshot set when it is a forwarded snapshot's
// (message_rich_store_snapshot), not the forward's own.
bool append_alt_badge(MessageTextPresentation* presentation, Utf8View description, bool snapshot, u32 attachment,
                      const UiRect& media, const UiWidgetReference& owner, const UiRect& clip, const Theme& theme,
                      const MessageRecord& record, u16 measurement) noexcept {
    if (!message_rich_display().image_descriptions || !description.bytes || ui_rect_width(media) < 64.0f ||
        ui_rect_height(media) < 48.0f)
        return true;
    const UiRect box{media.left + 12.5f, media.bottom - 36.0f, media.left + 64.0f, media.bottom - 4.0f};
    const UiRect visible = ui_rect_intersection(clip, box);
    if (!ui_rect_has_area(visible))
        return true;
    constexpr Utf8View label = utf8_literal("ALT");
    const MessageMarkdownSemantic semantic{attachment | (snapshot ? kMessageAltSnapshot : 0ull), 0, label.bytes, 0, 0,
                                           MessageMarkdownSemanticKind::alt_text, 0};
    return append_marked(presentation, label, TextStyleSpan{0, label.bytes, text_style_bold, text_size_sm}, &semantic,
                         owner, box, visible, theme.tokens.text.on_accent, &record, measurement, kLookMiddle);
}
// Message annotations below the content: a plugin card (an embed), the
// failed-interaction notice and the ephemeral footer (an eye, "Only you can
// see this \u2022 Dismiss message"). They extend the notice item, after any
// truncation notice. Plugin decoration accessories (labels, buttons, links)
// follow as one wrapping row.
constexpr float kAnnotationLineHeight = 20.0f;
bool append_plugin_accessories(MessageTextPresentation* presentation, const UiPluginRenderDecoration& decoration,
                               const UiRect& region, const UiRect& row_clip, const UiWidgetReference& owner,
                               const Theme& theme, const MessageRecord& record, u16 measurement_index,
                               float* top) noexcept {
    // Every command measures against the full line width, so its identity (and
    // measured width) does not depend on where the previous node ended.
    const float line_width = maximum(1.0f, ui_rect_width(region));
    const UiRect clip = ui_rect_intersection(row_clip, {region.left, row_clip.top, region.right, row_clip.bottom});
    float x = region.left;
    bool placed = false;
    for (u32 index = 0; index < decoration.accessory_count && index < kUiPluginRenderAccessories; ++index) {
        const UiPluginRenderNode& node = decoration.accessories[index];
        const Utf8View text = node.text.bytes ? node.text : node.tooltip;
        if (!text.bytes)
            continue;
        const bool button = node.kind == kUiPluginRenderButton;
        const bool link = node.kind == kUiPluginRenderLink;
        const MessageMarkdownSemantic semantic{index + 1u, 0, text.bytes,
                                               scoped_source_offset(kUiPluginRenderGeneratedSource, 0), 0,
                                               MessageMarkdownSemanticKind::link, 0};
        const Color color = button ? ui_icon_role_color(theme, ui_plugin_slot_role(node.tone ? node.tone : 1))
                            : link ? theme.tokens.text.link
                                   : ui_background_muted_text_color(theme);
        const u32 before = presentation->command_count;
        if (!ui_rect_has_area(clip) ||
            !append_marked(presentation, text,
                           {0, text.bytes, button ? text_style_bold : link ? text_style_link : text_style_subtext},
                           button || link ? &semantic : nullptr, owner,
                           {x, *top, x + line_width, *top + kAnnotationLineHeight}, clip, color, &record,
                           measurement_index,
                           {.kind = MessageTextMeasurementKind::notice,
                            .themed = link,
                            .vertical = UiTextAlignment::center,
                            .wrapping = TextWrapping::no_wrap,
                            .slot = static_cast<u8>(8u + index),
                            .overflow = TextOverflow::ellipsis,
                            .source_id = kUiPluginRenderGeneratedSource}))
            return !ui_rect_has_area(clip);
        auto& command = presentation->commands[before];
        // The measured width is known from the previous pass: wrap a node that
        // would cross the line end onto the next line.
        const float width = maximum(command.measured_width, 0.0f);
        if (placed && x > region.left && x + width > region.right) {
            x = region.left;
            *top += kAnnotationLineHeight;
            command.rectangle = {x, *top, x + line_width, *top + kAnnotationLineHeight};
        }
        x += width + 12.0f;
        placed = true;
    }
    if (placed)
        *top += kAnnotationLineHeight;
    return true;
}

// A plugin card is an embed (rich_layout.hpp's embed geometry; presentation.cpp paints the same chrome): the title
// is its title (a link when the card has one), the text its description and the caption its footer, which the
// buttons follow as link-style actions, " \u2022 " apart. Every part measures against the embed's widest text column,
// so no measurement depends on the card's own width; the card then narrows to its widest line as an embed does.
// *top moves past the card and the embed item gap; *height and *width receive the card's own extent.
bool append_plugin_card(MessageTextPresentation* presentation, const UiPluginRenderCard& card, const UiRect& region,
                        const UiRect& row_clip, const UiWidgetReference& owner, const Theme& theme,
                        const MessageRecord& record, u16 measurement_index, float* top, float* height,
                        float* width) noexcept {
    constexpr float insets = kMessageEmbedContentInset + kMessageEmbedEndInset;
    const float widest = minimum(ui_rect_width(region), kMessageEmbedMaximumWidth);
    const float column = maximum(1.0f, widest - insets);
    const float left = region.left + kMessageEmbedContentInset;
    const UiRect clip =
        ui_rect_intersection(row_clip, {region.left, row_clip.top, region.left + widest, row_clip.bottom});
    const Color muted = ui_background_muted_text_color(theme);
    const float begin = *top;
    float y = begin + kMessageEmbedTopInset, natural = 0.0f;
    bool measured = true;
    // One part at (x, y), as wide as the text column; null when the row has no room for text.
    const auto append = [&](Utf8View text, u32 style, u8 size, u64 target, float x, const Color& color, u8 slot,
                            bool wrap, MessageTextCommand** placed) noexcept {
        *placed = nullptr;
        if (!text.bytes || !ui_rect_has_area(clip))
            return true;
        const MessageMarkdownSemantic semantic{target, 0, text.bytes,
                                               scoped_source_offset(kUiPluginRenderGeneratedSource, 0), 0,
                                               MessageMarkdownSemanticKind::link, 0};
        const u32 before = presentation->command_count;
        if (!append_marked(presentation, text, {0, text.bytes, style, size}, target ? &semantic : nullptr, owner,
                           line_rectangle(x, y, column), clip, color, &record, measurement_index,
                           {.kind = MessageTextMeasurementKind::notice,
                            .themed = (style & text_style_link) != 0,
                            .wrapping = wrap ? TextWrapping::wrap : TextWrapping::no_wrap,
                            .slot = slot,
                            .overflow = wrap ? TextOverflow::clip : TextOverflow::ellipsis,
                            .source_id = kUiPluginRenderGeneratedSource}))
            return false;
        *placed = &presentation->commands[before];
        measured = measured && (*placed)->measured_height > 0.0f;
        return true;
    };
    // A block below the embed's margin: its measured height (one line until measured) and its width.
    const auto block = [&](const MessageTextCommand* placed) noexcept {
        y += maximum(placed ? placed->measured_height : 0.0f, kMessageEmbedLineMinimum);
        natural = maximum(natural, (placed ? placed->measured_width : 0.0f) + insets);
    };
    MessageTextCommand* command = nullptr;
    if (card.title.bytes) {
        const bool link = card.link.bytes != 0;
        y += kMessageEmbedBlockGap;
        if (!append(card.title, link ? text_style_bold | text_style_link : text_style_bold, text_size_md,
                    link ? kUiPluginRenderCardLinkTarget : 0, left, link ? theme.tokens.text.link : theme.text_primary,
                    16, true, &command))
            return false;
        block(command);
    }
    if (card.text.bytes) {
        y += kMessageEmbedBlockGap;
        if (!append(card.text, text_style_none, text_size_sm, 0, left, theme.text_primary, 17, true, &command))
            return false;
        block(command);
    }
    // The footer row: the caption, then each button after a separator, wrapping before a part that would cross the
    // column's end (each part's width is the previous pass's measurement).
    const u32 buttons = minimum(card.button_count, kUiPluginRenderCardButtons);
    if (card.caption.bytes || buttons) {
        y += kMessageEmbedBlockGap;
        float x = left, line = 0.0f;
        const auto part = [&](Utf8View text, u32 style, u64 target, const Color& color, u8 slot) noexcept {
            if (!append(text, style, text_size_xs, target, x, color, slot, false, &command))
                return false;
            const float part_width = command ? command->measured_width : 0.0f;
            if (command && x > left && x + part_width > left + column) {
                y += maximum(line, kMessageEmbedLineMinimum);
                x = left;
                line = 0.0f;
                command->rectangle = line_rectangle(x, y, column);
            }
            x += part_width;
            line = maximum(line, command ? command->measured_height : 0.0f);
            natural = maximum(natural, x - region.left + kMessageEmbedEndInset);
            return true;
        };
        if (!part(card.caption, text_style_subtext, 0, muted, 18))
            return false;
        for (u32 i = 0; i < buttons; ++i) {
            const UiPluginRenderCardButton& button = card.buttons[i];
            const u8 slot = static_cast<u8>(19 + 2 * i);
            const Color color =
                button.tone ? ui_icon_role_color(theme, ui_plugin_slot_role(button.tone)) : theme.tokens.text.link;
            if ((x > left && !part(utf8_literal(" \xE2\x80\xA2 "), text_style_subtext, 0, muted, slot)) ||
                !part(button.label, text_style_subtext | text_style_link, kUiPluginRenderCardTarget + i, color,
                      static_cast<u8>(slot + 1)))
                return false;
        }
        y += maximum(line, kMessageEmbedLineMinimum);
    }
    y += kMessageEmbedBottomInset;
    *height = y - begin;
    // Unmeasured text keeps the widest card until the pass that measures it, as an embed does.
    *width = measured ? minimum(widest, maximum(kMessageEmbedMinimumWidth, natural)) : widest;
    *top = y + kMessageEmbedItemGap;
    return true;
}
} // namespace
// The row or section showing this message's failed component error, or
// kMessageComponentNone when the failure has no control of its own.
u32 failed_component_host(const MessageScene& scene, const MessageRecord& record) noexcept {
    if (!scene.interaction.failed || scene.interaction.message_id != record.id || !scene.timeline ||
        !scene.timeline->rich_store)
        return kMessageComponentNone;
    const MessageRichRecord* rich = message_rich_store_find(*scene.timeline->rich_store, record.id);
    return rich && rich->message_revision == record.revision
               ? message_component_error_host(*rich, scene.interaction.component_index)
               : kMessageComponentNone;
}

bool append_annotations(MessageTextPresentation* presentation, const MessageScene& scene,
                        const MessageSceneRowWidget& row, const MessageRecord& record, const Theme& theme,
                        const UiRect& row_clip, u16 measurement_index, bool forwarded, float offset,
                        const UiPluginRenderDecoration* decoration) noexcept {
    // The notice item is measured again from what this pass shows. Nothing shown leaves only the truncation notice
    // (offset): an annotation that went away (a card hidden, an ephemeral message no longer) takes its space along,
    // instead of the row keeping the height its previous pass measured.
    auto& measured = presentation->measurements[measurement_index];
    measured.rich_item_layout.plugin_card_top = 0.0f;
    measured.rich_item_layout.plugin_card_height = 0.0f;
    measured.rich_item_layout.plugin_card_width = 0.0f;
    if (forwarded)
        return true;
    // A failure on a control shows under its action row instead.
    const bool failed = scene.interaction.failed && scene.interaction.message_id == record.id &&
                        failed_component_host(scene, record) == kMessageComponentNone;
    const bool ephemeral = (record.flags & kMessageFlagEphemeral) != 0;
    const bool accessories = decoration && decoration->accessory_count;
    const bool card = decoration && decoration->has_card;
    if (!failed && !ephemeral && !accessories && !card) {
        measured.rich_item_layout.heights[kMessageSceneNoticeItemOffset] = offset;
        measured.rich_layout.notice = offset;
        return true;
    }
    const UiLayoutNode* notice = message_scene_layout_node(scene, row.notice);
    if (!notice)
        return fail(presentation, MessageTextPresentationError::layout);
    const UiRect region = notice->content_rectangle;
    float top = region.top + offset;
    const UiWidgetReference owner{&scene.widgets, row.notice};
    if (card) {
        auto& items = measured.rich_item_layout;
        items.plugin_card_top = offset;
        if (!append_plugin_card(presentation, decoration->card, region, row_clip, owner, theme, record,
                                measurement_index, &top, &items.plugin_card_height, &items.plugin_card_width))
            return false;
    }
    if (failed) {
        const UiRect line{region.left, top, maximum(region.left + 1.0f, region.right), top + kAnnotationLineHeight};
        if (ui_rect_has_area(ui_rect_intersection(line, row_clip)) &&
            !append_command(presentation, utf8_literal("This interaction failed"), kNoStyles, kNoSemantics, owner, line, row_clip,
                            theme.tokens.text.danger, &record, kInvalidMessageTextMeasurement,
                            kLookMiddle))
            return false;
        top += kAnnotationLineHeight;
    }
    if (ephemeral) {
        // Two measured commands: the muted notice after its eye icon, then the dismiss link placed after the
        // notice's shaped width ("Only you can see this \u2022 Dismiss message"). The icon cell is an EM SPACE, a
        // 4-DIP gap and a joiner, as the status icons and link chips have.
        const auto notice_text =
            utf8_literal("\xE2\x80\x83\xE2\x80\x85\xE2\x80\x8DOnly you can see this \xE2\x80\xA2 ");
        const auto dismiss_text = utf8_literal("Dismiss message");
        const UiRect line{region.left, top, maximum(region.left + 1.0f, region.right), top + kAnnotationLineHeight};
        if (ui_rect_has_area(ui_rect_intersection(line, row_clip))) {
            // The footer is set in the small muted type.
            const MessageMarkdownSemantic eye{static_cast<u64>(MessageMarkdownChipIcon::ephemeral), 0, 3, 0, 0,
                                              MessageMarkdownSemanticKind::chip_icon, 0};
            if (!append_marked(presentation, notice_text, {0, notice_text.bytes, text_style_subtext}, &eye, owner, line,
                               row_clip, ui_background_muted_text_color(theme), &record, measurement_index,
                               kLookNotice))
                return false;
            const float notice_width = presentation->commands[presentation->command_count - 1].measured_width;
            if (notice_width > 0.0f && line.left + notice_width + 8.0f < line.right) {
                // The dismiss action is a message link to the ephemeral message itself.
                const MessageMarkdownSemantic semantic{record.id, 0, dismiss_text.bytes, 0, 0,
                                                       MessageMarkdownSemanticKind::message_link, 0};
                const UiRect dismiss{line.left + notice_width, line.top, line.right, line.bottom};
                if (!append_marked(presentation, dismiss_text,
                                   {0, dismiss_text.bytes, text_style_link | text_style_subtext}, &semantic, owner,
                                   dismiss, row_clip, theme.tokens.text.link, &record, measurement_index,
                                   kLookNoticeLink))
                    return false;
            }
        }
        top += kAnnotationLineHeight;
    }
    if (accessories && !append_plugin_accessories(presentation, *decoration, region, row_clip, owner, theme, record,
                                                  measurement_index, &top))
        return false;
    measured.rich_item_layout.heights[kMessageSceneNoticeItemOffset] = top - region.top;
    measured.rich_layout.notice = measured.rich_item_layout.heights[kMessageSceneNoticeItemOffset];
    return true;
}

bool append_edited_tail(MessageTextPresentation* presentation, const MessageScene& scene,
                        const MessageSceneRowWidget& row, const UiRect& row_clip, const Theme& theme,
                        const MessageRecord& record, u16 measurement_index) noexcept {
    const UiLayoutNode* group = message_scene_layout_node(scene, row.edited);
    if (!group)
        return fail(presentation, MessageTextPresentationError::layout);
    // The group takes the line's height from this pass, as the notice group does.
    MessageTextMeasurement& measured = presentation->measurements[measurement_index];
    measured.rich_layout.edited = measured.rich_item_layout.edited_height = kAnnotationLineHeight;
    const UiRect region = group->content_rectangle;
    const UiRect line{region.left, region.top, maximum(region.left + 1.0f, region.right),
                      region.top + kAnnotationLineHeight};
    Utf8View text{};
    ConstSlice<TextStyleSpan> styles{};
    ConstSlice<MessageMarkdownSemantic> semantics{};
    // A line out of view, or without room, goes without its label.
    if (!ui_rect_has_area(ui_rect_intersection(line, row_clip)) ||
        !append_edited_label(presentation, record.edited_timestamp_ms / 1'000u, &text, &styles, &semantics))
        return true;
    constexpr CommandLook look{.kind = MessageTextMeasurementKind::notice,
                               .themed = true,
                               .vertical = UiTextAlignment::center,
                               .wrapping = TextWrapping::no_wrap,
                               .overflow = TextOverflow::ellipsis};
    return append_command(presentation, text, styles, semantics, UiWidgetReference{&scene.widgets, row.edited}, line,
                          row_clip, ui_background_muted_text_color(theme), &record, measurement_index, look);
}

namespace {
bool append_card_line(MessageTextPresentation* presentation, const Utf8View& text, u32 style, const UiWidgetReference& owner,
                      const UiRect& rectangle, const UiRect& clip, const Color& color, const MessageRecord& record, u8 slot,
                      u16 measurement, UiTextAlignment alignment = UiTextAlignment::start) noexcept {
    const UiRect visible = ui_rect_intersection(clip, rectangle);
    if (!text.bytes || !ui_rect_has_area(rectangle) || !ui_rect_has_area(visible))
        return true;
    // Measured (notice kind: no height contribution) so centred labels align.
    return append_marked(presentation, text, style ? TextStyleSpan{0, text.bytes, style} : TextStyleSpan{}, nullptr,
                         owner, rectangle, visible, color, &record, measurement,
                          {.kind = MessageTextMeasurementKind::notice,
                           .horizontal = alignment,
                           .vertical = UiTextAlignment::center,
                           .wrapping = TextWrapping::no_wrap,
                           .slot = slot,
                           .overflow = TextOverflow::ellipsis});
}

// Invite cards: header, server name, counts and the Join action, from the
// session invite cache (loading and invalid states included).
// Event link cards share the invite card frame: when, name, place and the
// interested count from the scheduled events store, and a View action.
bool append_event_card(MessageTextPresentation* presentation, const MessageScene& scene, const InviteLink& link,
                       const MessageInviteCardGeometry& geometry, const UiWidgetReference& owner,
                       const MessageRecord& record, const ChatStateStore& state, const Theme& theme,
                       const UiRect& row_clip, u8 ordinal, u16 measurement_index, u64 now_unix_seconds) noexcept {
    const ScheduledEvent* event =
        scene.scheduled_events ? scheduled_event_store_find(*scene.scheduled_events, link.event_id) : nullptr;
    const bool member = chat_state_store_find_guild(state, link.event_guild_id) != nullptr;
    char header_storage[96];
    Utf8View header = utf8_literal("Event");
    if (event && event->status == ScheduledEventStatus::active) {
        header = utf8_literal("Live now");
    } else if (event) {
        const auto formatted = format_timestamp_local(event->start_ms / 1'000u, 'f', now_unix_seconds,
                                                      {header_storage, sizeof(header_storage)});
        if (formatted.ok && !retain_text(presentation, {header_storage, formatted.bytes}, &header))
            return false;
    }
    const Utf8View name = event ? (event->name_bytes ? Utf8View{event->name, event->name_bytes}
                                                     : utf8_literal("Untitled event"))
                          : member ? utf8_literal("Loading event…")
                                   : utf8_literal("Join the server to see this event.");
    char detail_storage[192];
    u32 detail_bytes = 0;
    const auto add = [&](Utf8View text) noexcept {
        const u32 take = minimum<u32>(text.bytes, sizeof(detail_storage) - detail_bytes);
        copy_bytes(detail_storage + detail_bytes, text.data, take);
        detail_bytes += take;
    };
    if (event) {
        if (event->entity == ScheduledEventEntity::external)
            add({event->location, event->location_bytes});
        else if (const auto* channel = chat_state_store_find_channel(state, event->channel_id))
            add(chat_state_store_channel_name(state, *channel));
        if (event->user_count_known) {
            if (detail_bytes)
                add(utf8_literal(" \xC2\xB7 "));
            char count[16];
            add({count, decimal_write_grouped(count, event->user_count)});
            add(utf8_literal(" interested"));
        }
    }
    Utf8View detail{};
    if (detail_bytes && !retain_text(presentation, {detail_storage, detail_bytes}, &detail))
        return false;
    return append_card_line(presentation, header, text_style_bold, owner, geometry.header, row_clip,
                            event && event->status == ScheduledEventStatus::active
                                ? theme.tokens.text.positive
                                : ui_background_muted_text_color(theme),
                            record, ordinal, measurement_index) &&
           append_card_line(presentation, name, event ? text_style_bold : 0, owner, geometry.name, row_clip,
                            theme.text_primary, record, ordinal, measurement_index) &&
           append_card_line(presentation, detail, 0, owner, geometry.detail, row_clip,
                            ui_background_muted_text_color(theme), record, ordinal, measurement_index) &&
           (!event || append_card_line(presentation, event->interested ? utf8_literal("Interested")
                                                                       : utf8_literal("View"),
                                       text_style_bold, owner, geometry.button, row_clip,
                                       event->interested ? theme.text_primary : theme.tokens.text.on_accent, record,
                                       ordinal, measurement_index, UiTextAlignment::center));
}

} // namespace
bool append_invite_cards(MessageTextPresentation* presentation, const MessageScene& scene,
                         const MessageSceneRowWidget& row, const MessageRecord& record, const ChatStateStore& state,
                         const Theme& theme, const UiRect& row_clip, u16 measurement_index,
                         u64 now_unix_seconds) noexcept {
    MessageTextMeasurement* measurement = &presentation->measurements[measurement_index];
    InviteLink links[kMessageInviteCardMaximum];
    const u32 count = minimum<u32>(row.invite_item_count,
                                   message_scene_invite_codes(scene, record, {links, kMessageInviteCardMaximum}));
    float total = 0;
    for (u32 i = 0; i < count; ++i) {
        const u32 slot = kMessageRichInviteItemOffset + i;
        measurement->rich_item_layout.heights[slot] = kMessageInviteCardHeight;
        total += kMessageInviteCardHeight + (i ? kMessageInviteCardGap : 0.0f);
        const UiWidgetHandle item_widget = message_scene_rich_item_widget(row, MessageSceneRichItemKind::invite, i);
        const UiLayoutNode* node = message_scene_layout_node(scene, item_widget);
        if (!node)
            return fail(presentation, MessageTextPresentationError::layout);
        const auto geometry = message_invite_card_geometry(node->rectangle);
        if (!ui_rect_has_area(ui_rect_intersection(geometry.card, row_clip)))
            continue;
        const UiWidgetReference owner{&scene.widgets, item_widget};
        if (invite_link_event(links[i])) {
            if (!append_event_card(presentation, scene, links[i], geometry, owner, record, state, theme, row_clip,
                                   static_cast<u8>(i), measurement_index, now_unix_seconds))
                return false;
            continue;
        }
        const Utf8View code{links[i].code, links[i].code_bytes};
        const InviteCardEntry* entry = scene.invite_cards ? invite_card_cache_find(*scene.invite_cards, code) : nullptr;
        const bool known = entry && entry->invite.guild_id;
        const bool invalid = entry && entry->state == InviteCardState::invalid;
        Utf8View name = known ? Utf8View{entry->invite.name, entry->invite.name_bytes}
                        : invalid ? utf8_literal("Invalid invite")
                                  : utf8_literal("Loading invite\xE2\x80\xA6");
        if (known && !name.bytes)
            name = utf8_literal("Unnamed server");
        char detail_storage[96];
        u32 detail_bytes = 0;
        if (known && entry->invite.counts_known) {
            detail_bytes += decimal_write_grouped(detail_storage, entry->invite.presence_count);
            const auto online = utf8_literal(" online \xC2\xB7 ");
            copy_bytes(detail_storage + detail_bytes, online.data, online.bytes);
            detail_bytes += online.bytes;
            detail_bytes += decimal_write_grouped(detail_storage + detail_bytes, entry->invite.member_count);
            const auto members = utf8_literal(" members");
            copy_bytes(detail_storage + detail_bytes, members.data, members.bytes);
            detail_bytes += members.bytes;
        }
        Utf8View detail{};
        if (detail_bytes && !retain_text(presentation, {detail_storage, detail_bytes}, &detail))
            return false;
        if (invalid)
            detail = utf8_literal("This invite may have expired.");
        const bool joined = known && (entry->joined || chat_state_store_find_guild(state, entry->invite.guild_id));
        const u8 ordinal = static_cast<u8>(i);
        if (!append_card_line(presentation, invalid ? utf8_literal("You received an invite, but\xE2\x80\xA6")
                                                    : utf8_literal("You've been invited to join a server"),
                              text_style_bold, owner, geometry.header, row_clip,
                              ui_background_muted_text_color(theme), record, ordinal, measurement_index) ||
            !append_card_line(presentation, name, known ? text_style_bold : 0, owner, geometry.name, row_clip,
                              invalid ? theme.tokens.text.danger : theme.text_primary, record, ordinal,
                              measurement_index) ||
            !append_card_line(presentation, detail, 0, owner, geometry.detail, row_clip,
                              ui_background_muted_text_color(theme), record, ordinal, measurement_index) ||
            (known && !append_card_line(presentation, joined ? utf8_literal("Joined") : utf8_literal("Join"),
                                        text_style_bold, owner, geometry.button, row_clip,
                                        joined ? theme.text_primary : theme.tokens.text.on_accent, record, ordinal,
                                        measurement_index, UiTextAlignment::center)))
            return false;
        // A server without an icon shows its initials in the tile.
        if (known && !entry->invite.icon_bytes) {
            char initials_storage[16];
            const Utf8View initials = ui_admin_initials(name, {initials_storage, sizeof(initials_storage)});
            Utf8View retained{};
            if (initials.bytes &&
                (!retain_text(presentation, initials, &retained) ||
                 !append_card_line(presentation, retained, text_style_bold, owner, geometry.icon, row_clip,
                                   theme.text_primary, record, ordinal, measurement_index, UiTextAlignment::center)))
                return false;
        }
    }
    measurement->rich_layout.invites = total;
    return true;
}

// Collapsed plugin rows: "<label> · Show" after any compact name prefix. The
// Show link carries a plugin semantic the host routes to a session reveal.
bool append_plugin_collapsed(MessageTextPresentation* presentation, const UiPluginRenderDecoration& decoration,
                             Utf8View* text, ConstSlice<TextStyleSpan>* styles,
                             ConstSlice<MessageMarkdownSemantic>* semantics) noexcept {
    const Utf8View label = ui_plugin_render_collapsed_label(decoration);
    const auto separator = utf8_literal(" \xC2\xB7 ");
    const auto show = utf8_literal("Show");
    const u32 lead = text->bytes ? 1u : 0u;
    const u32 bytes = lead + label.bytes + separator.bytes + show.bytes;
    if (presentation->utf8_count > presentation->utf8.count ||
        bytes > presentation->utf8.count - presentation->utf8_count ||
        2u > presentation->styles.count - presentation->style_count ||
        presentation->semantic_count >= presentation->semantics.count)
        return fail(presentation, MessageTextPresentationError::capacity);
    // A compact name prefix was retained just before: extend it in place.
    const u32 text_begin = text->bytes ? static_cast<u32>(text->data - presentation->utf8.data) : presentation->utf8_count;
    const u32 style_begin = styles->count ? static_cast<u32>(styles->data - presentation->styles.data) : presentation->style_count;
    const u32 semantic_begin =
        semantics->count ? static_cast<u32>(semantics->data - presentation->semantics.data) : presentation->semantic_count;
    if (text_begin + text->bytes != presentation->utf8_count || style_begin + styles->count != presentation->style_count ||
        semantic_begin + semantics->count != presentation->semantic_count)
        return fail(presentation, MessageTextPresentationError::model);
    char* out = presentation->utf8.data + presentation->utf8_count;
    const u32 base = presentation->utf8_count - text_begin;
    u32 at = 0;
    if (lead)
        out[at++] = ' ';
    copy_bytes(out + at, label.data, label.bytes);
    copy_bytes(out + at + label.bytes, separator.data, separator.bytes);
    copy_bytes(out + at + label.bytes + separator.bytes, show.data, show.bytes);
    const u32 label_offset = base + at;
    const u32 show_offset = label_offset + label.bytes + separator.bytes;
    presentation->utf8_count += bytes;
    presentation->styles[presentation->style_count++] = {label_offset, label.bytes + separator.bytes, text_style_italic};
    presentation->styles[presentation->style_count++] = {show_offset, show.bytes, text_style_link};
    presentation->semantics[presentation->semantic_count++] = {
        kUiPluginRenderShowTarget, show_offset, show.bytes, scoped_source_offset(kUiPluginRenderGeneratedSource, 0),
        0, MessageMarkdownSemanticKind::link, 0};
    *text = {presentation->utf8.data + text_begin, presentation->utf8_count - text_begin};
    *styles = {presentation->styles.data + style_begin, presentation->style_count - style_begin};
    *semantics = {presentation->semantics.data + semantic_begin, presentation->semantic_count - semantic_begin};
    return true;
}

// Channel-beginning header text: bold 2rem title, optional DM username, and
// the description with its bold name runs (scene-owned text and spans).
bool append_intro(MessageTextPresentation* presentation, const MessageScene& scene, const Theme& theme) noexcept {
    MessageSceneIntroGeometry geometry{};
    if (scene.content_gated || !message_scene_intro_geometry(scene, &geometry))
        return true;
    const UiRect clip = geometry.area;
    if (!ui_rect_has_area(clip))
        return true;
    const UiWidgetReference owner{&scene.widgets, scene.root};
    const MessageSceneIntro& intro = scene.intro;
    const struct {
        Utf8View text;
        ConstSlice<TextStyleSpan> styles;
        UiRect rectangle;
    } parts[]{
        {message_scene_intro_header(intro), {&scene.intro_header_style, 1}, geometry.header},
        {message_scene_intro_subtitle(intro), {&scene.intro_subtitle_style, 1}, geometry.subtitle},
        {message_scene_intro_description(intro), {intro.description_spans, intro.description_span_count},
         geometry.description},
    };
    for (const auto& part : parts) {
        if (!part.text.bytes || !ui_rect_has_area(part.rectangle) ||
            !ui_rect_has_area(ui_rect_intersection(clip, part.rectangle)))
            continue;
        if (!append_command(presentation, part.text, part.styles, kNoSemantics, owner, part.rectangle, clip,
                            theme.text_primary, nullptr, kInvalidMessageTextMeasurement, kLookBlock))
            return false;
    }
    return true;
}

bool append_rich_media_text(BuildContext& context, const MessageScene& scene,
                            const MessageSceneRowWidget& row, const Theme& theme,
                            const UiRect& row_clip, u16 measurement_index, float text_width,
                            MessageMediaPolicyResult media_policy) noexcept {
    auto* presentation = context.presentation;
    const auto* record = context.message;
    const auto* rich_store = context.rich_store;
    const auto* rich = context.rich;
    if (rich->attachment_count != 0) {
        for (u32 index = 0; index < rich->attachment_count; ++index) {
            set_failure_context(presentation, MessageTextPresentationStage::attachment,
                                MessageTextPresentationOperation::resolve_layout, static_cast<u16>(index));
            const UiWidgetHandle item_widget =
                message_scene_rich_item_widget(row, MessageSceneRichItemKind::attachment, index);
            const UiLayoutNode* item_layout = message_scene_layout_node(scene, item_widget);
            if (!item_layout) {
                return fail(presentation, MessageTextPresentationError::layout);
            }
            const UiWidgetReference owner{&scene.widgets, item_widget};
            const float item_left = item_layout->content_rectangle.left;
            const auto& attachment = rich->attachments[index];
            const UiAudioControlsStyle audio_style = ui_theme_audio_style(scene.theme_styles);
            const float audio_height =
                message_rich_attachment_audio_supported(*rich_store, attachment)
                    ? ui_audio_card_height(audio_style, ui_rect_width(item_layout->content_rectangle),
                                           message_rich_record_voice(*rich))
                    : 0.f;
            const float item_width =
                minimum(audio_height > 0 ? audio_style.maximum_width : 420.0f,
                        ui_rect_width(item_layout->content_rectangle));
            UiTextFlowLayout flow{};
            const UiRect attachment_clip = ui_rect_intersection(
                row_clip, {item_left, row_clip.top, item_left + item_width, row_clip.bottom});
            if (!initialize_flow(presentation, *item_layout, item_left, item_width, attachment_clip,
                                 &flow)) {
                return false;
            }
            float preview_width = 0.0f;
            float preview_height = 0.0f;
            const bool preview = message_rich_attachment_preview_extent(
                *rich_store, rich->attachments[index], text_width, &preview_width, &preview_height);
            float required_extent = 42.0f;
            if (audio_height > 0) {
                required_extent = audio_height + 6;
                UiAudioControlsProjection projection{};
                const UiRect card{item_left, item_layout->rectangle.top, item_left + item_width,
                                  item_layout->rectangle.top + audio_height};
                struct AudioTextContext {
                    MessageTextPresentation* presentation;
                    const MessageRecord* record;
                    u16 measurement;
                    u8 slot;
                } audio_context{presentation, record, measurement_index, static_cast<u8>(index)};
                UiTextEmitter emitter{};
                const auto append_audio = +[](void* source, const UiTextCommand& command, u32) noexcept {
                    auto& audio = *static_cast<AudioTextContext*>(source);
                    auto* p = audio.presentation;
                    Utf8View retained{};
                    if (!retain_text(p, command.text, &retained))
                        return false;
                    return append_marked(
                        p, retained, {0, command.text.bytes, command.style_flags}, nullptr, command.owner,
                        command.rectangle, command.clip, command.color, audio.record,
                        audio.measurement,
                        {.kind = MessageTextMeasurementKind::attachment,
                         .minimum_height = ui_rect_height(command.rectangle),
                         .horizontal = command.horizontal_alignment,
                         .vertical = command.vertical_alignment,
                         .wrapping = command.wrapping,
                         .slot = audio.slot,
                         .overflow = command.overflow});
                };
                UiTextOutput output{nullptr, &emitter};
                if (!ui_audio_attachment_project(*rich_store, attachment, rich->message_id,
                                                 rich->message_revision, message_rich_record_voice(*rich),
                                                 card, theme, {}, &projection) ||
                    !ui_text_emitter_initialize(&emitter, append_audio, &audio_context,
                                                kUiAudioControlCount) ||
                    !ui_audio_controls_text_output(projection, attachment_clip, owner, &output) ||
                    !ui_text_flow_advance(&flow, required_extent))
                    return fail(presentation, MessageTextPresentationError::layout);
            } else if (preview) {
                required_extent = preview_height + 6.0f;
                const MessageMediaObscure obscure = message_scene_media_obscure(
                    scene, media_policy, rich->message_id,
                    message_media_reveal_key(rich->message_id, MessageMediaSource::attachment, index,
                                             row.forwarded_snapshot),
                    message_rich_attachment_spoiler(*rich_store, attachment),
                    message_rich_attachment_explicit(attachment));
                const UiRect media{item_left, item_layout->rectangle.top, item_left + preview_width,
                                   item_layout->rectangle.top + preview_height};
                if (!append_obscure_label(presentation, obscure, media, owner, attachment_clip, theme, *record,
                                          measurement_index) ||
                    (obscure == MessageMediaObscure::none &&
                     !append_alt_badge(presentation, message_rich_store_string(*rich_store, attachment.description),
                                       rich->forwarded_snapshot_child, index, media, owner, attachment_clip, theme, *record,
                                       measurement_index)) ||
                    !ui_text_flow_advance(&flow, required_extent)) {
                    return fail(presentation, MessageTextPresentationError::layout);
                }
            } else {
                Utf8View label = message_rich_store_string(*rich_store, rich->attachments[index].filename);
                if (label.bytes == 0) {
                    label = utf8_literal("Attachment");
                }
                UiRect label_rectangle{};
                if (!ui_text_flow_advance(&flow, 7.0f) ||
                    !begin_flow_item_clamped(&flow, 36.0f, 44.0f, &label_rectangle) ||
                    !append_command(presentation, label, kNoStyles, kNoSemantics, owner, label_rectangle, attachment_clip,
                                    theme.text_primary, record, measurement_index,
                                    kLookAttachmentName) ||
                    !ui_text_flow_commit_item(&flow, 20.0f, 20.0f, 8.0f)) {
                    return fail(presentation, MessageTextPresentationError::layout);
                }
            }
            const u32 slot = kMessageSceneAttachmentItemOffset + index;
            if (!ui_text_flow_ensure_extent(&flow, required_extent) ||
                !finish_flow(
                    presentation, flow,
                    &presentation->measurements[measurement_index].rich_item_layout.heights[slot])) {
                return fail(presentation, MessageTextPresentationError::layout);
            }
            saturating_accumulate(presentation->rich.attachments);
        }
        presentation->measurements[measurement_index].rich_layout.attachments =
            rich_item_total(presentation->measurements[measurement_index].rich_item_layout,
                            kMessageSceneAttachmentItemOffset, rich->attachment_count);
    }

    if (rich->embed_count != 0 && message_rich_display().embeds) {
        for (u32 index = 0; index < rich->embed_count; ++index) {
            set_failure_context(presentation, MessageTextPresentationStage::embed,
                                MessageTextPresentationOperation::resolve_layout, static_cast<u16>(index));
            const UiWidgetHandle item_widget =
                message_scene_rich_item_widget(row, MessageSceneRichItemKind::embed, index);
            const UiLayoutNode* item_layout = message_scene_layout_node(scene, item_widget);
            if (!item_layout) {
                return fail(presentation, MessageTextPresentationError::layout);
            }
            const UiWidgetReference owner{&scene.widgets, item_widget};
            MessageRichItemLayout& merged_items = presentation->measurements[measurement_index].rich_item_layout;
            if (message_rich_embed_merge_host(*rich_store, *rich, index) != index) {
                // A merged embed's image is drawn in the first embed's gallery.
                merged_items.heights[kMessageSceneEmbedItemOffset + index] = 0.0f;
                merged_items.embed_widths[index] = merged_items.embed_media_tops[index] = 0.0f;
                merged_items.embed_media_widths[index] = merged_items.embed_media_heights[index] = 0.0f;
                merged_items.embed_author_tops[index] = merged_items.embed_footer_tops[index] = 0.0f;
                continue;
            }
            UiTextFlowLayout flow{};
            const float embed_left = item_layout->content_rectangle.left;
            const float maximum_width =
                minimum(ui_rect_width(item_layout->content_rectangle), kMessageEmbedMaximumWidth);
            const float retained_width = row.rich_item_layout.embed_widths[index];
            const float embed_width =
                minimum(maximum_width, retained_width > 0 ? retained_width : maximum_width);
            const MessageRichEmbed& embed = rich->embeds[index];
            const MessageRichEmbedPlan plan = message_rich_embed_plan(*rich_store, embed);
            const bool media_only = plan.media_only;
            float thumb_width = 0, thumb_height = 0;
            const bool thumbnail = message_rich_embed_thumbnail_extent(
                *rich_store, embed, maximum(1.f, embed_width - kMessageEmbedContentInset - kMessageEmbedEndInset),
                &thumb_width, &thumb_height);
            const float thumbnail_inset = thumbnail ? thumb_width + kMessageEmbedThumbnailGap : 0;
            const float body_width = maximum(1.f, embed_width - thumbnail_inset);
            const UiRect embed_clip = ui_rect_intersection(
                row_clip, UiRect{embed_left, row_clip.top, embed_left + embed_width, row_clip.bottom});
            if (!ui_rect_has_area(embed_clip) ||
                !initialize_flow(presentation, *item_layout, embed_left, body_width, embed_clip, &flow)) {
                return false;
            }
            const u32 embed_command_begin = presentation->command_count;
            float natural_width = 0;
            MessageRichItemLayout& measured_items =
                presentation->measurements[measurement_index].rich_item_layout;
            measured_items.embed_author_tops[index] = 0.0f;
            measured_items.embed_footer_tops[index] = 0.0f;
            const auto measure_from = [&](u32 first_command, float inset) noexcept {
                natural_width = widest_framed(presentation, first_command, inset, natural_width);
            };
            const auto block_gap = [&]() noexcept { return ui_text_flow_advance(&flow, kMessageEmbedBlockGap); };
            const auto source = [&](u32 slot) noexcept {
                return static_cast<u16>(kRichTextSource | (context.forwarded ? kForwardedTextSource : 0u) |
                                        (kRichTextEmbedBegin + index * kRichTextEmbedSlots + slot));
            };
            const Color muted = ui_background_muted_text_color(theme);
            constexpr float text_insets = kMessageEmbedContentInset + kMessageEmbedEndInset;
            if (!media_only) {
                if (!ui_text_flow_advance(&flow, message_rich_embed_top_inset(plan.components)))
                    return fail(presentation, MessageTextPresentationError::layout);
                // Provider: small subtext (12 DIP).
                const Utf8View provider = message_rich_embed_provider_label(*rich_store, embed);
                const Utf8View provider_url = message_rich_store_string(*rich_store, embed.provider_url);
                if (provider.bytes) {
                    const u32 first = presentation->command_count;
                    if (!block_gap() ||
                        !append_embed_plain_line(&context, provider, &flow, kMessageEmbedContentInset,
                                                 kMessageEmbedEndInset, muted, owner, measurement_index,
                                                 text_style_subtext, provider_url, source(7), 0.0f, source(6),
                                                 text_size_xs))
                        return false;
                    measure_from(first, text_insets);
                }
                // Author: optional 24-DIP round icon, semibold name, author.url link.
                const Utf8View author_name = message_rich_store_string(*rich_store, embed.author_name);
                if (author_name.bytes) {
                    if (!block_gap())
                        return fail(presentation, MessageTextPresentationError::layout);
                    const bool icon = message_rich_embed_author_icon(*rich_store, embed).bytes != 0;
                    const float inset =
                        kMessageEmbedContentInset + (icon ? kMessageEmbedAuthorIcon + kMessageEmbedIconGap : 0.0f);
                    measured_items.embed_author_tops[index] = ui_text_flow_required_height(flow);
                    const u32 first = presentation->command_count;
                    if (!append_embed_plain_line(&context, author_name, &flow, inset, kMessageEmbedEndInset,
                                                 theme.text_primary, owner, measurement_index, text_style_bold,
                                                 message_rich_store_string(*rich_store, embed.author_url),
                                                 source(5), icon ? kMessageEmbedAuthorIcon : 0.0f, source(0),
                                                 text_size_sm))
                        return false;
                    measure_from(first, inset + kMessageEmbedEndInset);
                }
                // Title: semibold, link-coloured when embed.url is set; title rules.
                const Utf8View title = message_rich_store_string(*rich_store, embed.title);
                if (title.bytes) {
                    const Utf8View url = message_rich_store_string(*rich_store, embed.url);
                    const u32 first = presentation->command_count;
                    if (!block_gap() ||
                        !append_embed_block(&context, title, &flow, kMessageEmbedContentInset,
                                            kMessageEmbedEndInset, theme.text_primary, owner,
                                            measurement_index, plan.markdown, 1,
                                            text_style_bold | (url.bytes ? text_style_link : 0u), text_size_md, url,
                                            source(4)))
                        return false;
                    measure_from(first, text_insets);
                }
                const Utf8View description = message_rich_store_string(*rich_store, embed.description);
                if (description.bytes && plan.description) {
                    const u32 first = presentation->command_count;
                    if (!block_gap() ||
                        !append_embed_block(&context, description, &flow, kMessageEmbedContentInset,
                                            kMessageEmbedEndInset, theme.text_primary, owner,
                                            measurement_index, plan.markdown, 0, 0,
                                            plan.components ? text_size_inherit : text_size_sm))
                        return false;
                    measure_from(first, text_insets);
                }
                if (embed.type == GatewayMessageEmbedType::poll_result) {
                    const u32 first = presentation->command_count;
                    if (!block_gap() ||
                        !append_poll_result(&context, *rich_store, *rich, &flow, theme.text_primary, muted, owner,
                                            measurement_index))
                        return false;
                    measure_from(first, text_insets);
                }
                if (plan.fields &&
                    !append_embed_fields(&context, *rich_store, *rich, embed, &flow, theme.text_primary,
                                         theme.text_primary, owner, measurement_index, &natural_width,
                                         plan.markdown)) {
                    return false;
                }
                natural_width += thumbnail_inset;
                if (thumbnail &&
                    !ui_text_flow_ensure_extent(
                        &flow, message_rich_embed_top_inset(plan.components) + kMessageEmbedBlockGap + thumb_height))
                    return false;
            }
            flow.content_rectangle.right = embed_left + embed_width;
            measured_items.embed_media_tops[index] = 0.0f;
            measured_items.embed_media_widths[index] = 0.0f;
            measured_items.embed_media_heights[index] = 0.0f;
            MessageRichEmbedMediaSelection media{};
            MessageRichEmbedGallery gallery{};
            float media_width = 0.0f;
            float media_height = 0.0f;
            // A gallery sits in the text column, beside any corner thumbnail.
            if (message_rich_embed_gallery(*rich_store, *rich, index, &gallery)) {
                media_width = message_rich_embed_gallery_width(
                    embed, maximum(0.0f, body_width - (media_only ? 0.0f : text_insets)));
                media_height = kMessageEmbedGalleryHeight;
            }
            if ((media_width > 0.0f && media_height > 0.0f) ||
                (!gallery.count && message_rich_embed_media_select(*rich_store, embed, &media) &&
                 message_rich_embed_media_extent(media, maximum(0.0f, embed_width - (media_only ? 0.0f : text_insets)),
                                                 &media_width, &media_height))) {
                if (!media_only && !ui_text_flow_advance(&flow, kMessageEmbedMediaGap))
                    return fail(presentation, MessageTextPresentationError::layout);
                measured_items.embed_media_tops[index] = ui_text_flow_required_height(flow);
                measured_items.embed_media_widths[index] = media_width;
                measured_items.embed_media_heights[index] = media_height;
                const float media_left = embed_left + message_rich_embed_media_left(media_only);
                const UiRect media_box{media_left, flow.cursor_y, media_left + media_width,
                                       flow.cursor_y + media_height};
                const u32 labels = gallery.count ? minimum<u32>(gallery.count, kMessageEmbedGalleryDrawn) : 1;
                for (u32 tile = 0; tile < labels; ++tile) {
                    const u32 member = gallery.count ? gallery.members[tile] : index;
                    const auto obscure = message_scene_media_obscure(
                        scene, media_policy, record->id,
                        message_media_reveal_key(record->id, MessageMediaSource::embed, member,
                                                 row.forwarded_snapshot),
                        false, message_rich_embed_explicit(rich->embeds[member]));
                    const UiRect cell = gallery.count
                                            ? message_rich_embed_gallery_tile(media_box, gallery.count, tile)
                                            : media_box;
                    if (!append_obscure_label(presentation, obscure, cell, owner, embed_clip, theme, *record,
                                              measurement_index))
                        return false;
                }
                if (!ui_text_flow_advance(&flow, media_height)) {
                    return fail(presentation, MessageTextPresentationError::layout);
                }
            }
            if (media_only) {
                if (!ui_text_flow_ensure_extent(&flow, media_height)) {
                    return fail(presentation, MessageTextPresentationError::layout);
                }
            } else {
                // Footer: optional 20-DIP round icon, "text \u2022 Yesterday at 10:08 PM".
                const Utf8View footer = message_rich_store_string(*rich_store, embed.footer_text);
                char stamp[kMessageMarkdownTimestampMaximumBytes];
                const TimestampFormatResult formatted =
                    embed.timestamp_ms ? format_timestamp_calendar(embed.timestamp_ms / 1'000u,
                                                                   context.now_unix_seconds, {stamp, sizeof(stamp)})
                                       : TimestampFormatResult{};
                const u32 stamp_bytes = formatted.ok ? formatted.bytes : 0u;
                if (stamp_bytes && formatted.next_update_unix_seconds &&
                    (presentation->next_timestamp_update_unix_seconds == 0 ||
                     formatted.next_update_unix_seconds < presentation->next_timestamp_update_unix_seconds))
                    presentation->next_timestamp_update_unix_seconds = formatted.next_update_unix_seconds;
                Utf8View line = footer;
                if (stamp_bytes) {
                    const Utf8View separator = utf8_literal(" \xE2\x80\xA2 ");
                    const u32 bytes = footer.bytes + (footer.bytes ? separator.bytes : 0u) + stamp_bytes;
                    if (bytes > presentation->utf8.count - presentation->utf8_count)
                        return fail(presentation, MessageTextPresentationError::capacity);
                    char* out = presentation->utf8.data + presentation->utf8_count;
                    u32 at = 0;
                    copy_bytes(out, footer.data, footer.bytes);
                    at += footer.bytes;
                    if (footer.bytes) {
                        copy_bytes(out + at, separator.data, separator.bytes);
                        at += separator.bytes;
                    }
                    copy_bytes(out + at, stamp, stamp_bytes);
                    presentation->utf8_count += bytes;
                    line = Utf8View{out, bytes};
                }
                if (line.bytes) {
                    if (!block_gap())
                        return fail(presentation, MessageTextPresentationError::layout);
                    const bool icon = message_rich_embed_footer_icon(*rich_store, embed).bytes != 0;
                    const float inset =
                        kMessageEmbedContentInset + (icon ? kMessageEmbedFooterIcon + kMessageEmbedIconGap : 0.0f);
                    measured_items.embed_footer_tops[index] = ui_text_flow_required_height(flow);
                    const u32 first = presentation->command_count;
                    if (!append_embed_plain_line(&context, line, &flow, inset, kMessageEmbedEndInset, muted, owner,
                                                 measurement_index, text_style_subtext, kNoLink, 0,
                                                 icon ? kMessageEmbedFooterIcon : 0.0f,
                                                 footer.bytes ? source(3) : 0, text_size_xs))
                        return false;
                    measure_from(first, inset + kMessageEmbedEndInset);
                }
                if (!ui_text_flow_advance(&flow, kMessageEmbedBottomInset + kMessageEmbedItemGap)) {
                    return fail(presentation, MessageTextPresentationError::layout);
                }
            }
            const u32 slot = kMessageSceneEmbedItemOffset + index;
            // The content width comes from the initial maximum-width pass.
            // Measuring again after wrapping at that width feeds the shorter
            // wrapped lines back and keeps shrinking the card. A new
            // viewport/content identity resets retained_width.
            bool measured = true;
            for (u32 c = embed_command_begin; c < presentation->command_count; ++c)
                measured = measured && presentation->commands[c].measured_height > 0;
            natural_width = maximum(natural_width, media_width + (media_only ? 0 : text_insets) +
                                                       (gallery.count ? thumbnail_inset : 0.0f));
            if (measured && retained_width <= 0)
                measured_items.embed_widths[index] =
                    minimum(maximum_width, maximum(kMessageEmbedMinimumWidth, natural_width));
            if (!finish_flow(
                    presentation, flow,
                    &presentation->measurements[measurement_index].rich_item_layout.heights[slot])) {
                return false;
            }
            saturating_accumulate(presentation->rich.embeds);
        }
        presentation->measurements[measurement_index].rich_layout.embeds =
            rich_item_total(presentation->measurements[measurement_index].rich_item_layout,
                            kMessageSceneEmbedItemOffset, rich->embed_count);
    }

    return true;
}

// A quoted system notice builds its line through a buffer of its own, kept out
// of append_row's frame (as is the poll below).
__declspec(noinline) RetainedReplySummary retain_quoted_system(MessageTextPresentation* presentation, u64 message_id,
                                                              const Utf8View& author, const Utf8View& poll_question,
                                                              decltype(MessageSystemTextInput{}.type) type) noexcept {
    MessageSystemTextInput system_input{};
    system_input.message_id = message_id;
    system_input.author = author;
    system_input.poll_question = poll_question;
    system_input.type = type;
    char system_storage[kMessageSystemTextMaximumBytes];
    MessageSystemText system_text{};
    if (message_system_text_build(system_input, {system_storage, sizeof(system_storage)}, &system_text))
        return retain_reply_plain(presentation, system_text);
    return {};
}

// A row's poll: the unavailable notice, or the controls' text through the
// poll projection (which is large). False once the presentation has failed.
__declspec(noinline) bool append_poll_text(MessageTextPresentation* presentation, const MessageScene& scene,
                                           const MessageSceneRowWidget& row, const Theme& theme,
                                           const UiRect& row_clip, const MessageRecord* record) noexcept {
    if (row.poll_unavailable) {
        const auto* node = message_scene_layout_node(scene, row.poll);
        const UiPollStyle poll_style = ui_theme_poll_style(scene.theme_styles);
        if (!node ||
            !append_command(
                presentation, poll_style.unavailable, kNoStyles, kNoSemantics,
                {&scene.widgets, row.poll},
                {node->rectangle.left + poll_style.padding,
                 node->rectangle.top + poll_style.padding,
                 minimum(node->rectangle.right, node->rectangle.left + poll_style.maximum_width) -
                     poll_style.padding,
                 node->rectangle.top + row.poll_height - kDefaultMessageRichBlockGapDips -
                     poll_style.padding},
                row_clip, theme.text_secondary, record,
                kInvalidMessageTextMeasurement, kLookMiddleWrapped))
            return fail(presentation, MessageTextPresentationError::layout);
        return true;
    }
    UiPollAnswer answers[kUiPollMaximumAnswers];
    UiPollProjection poll{};
    struct PollTextContext {
        MessageTextPresentation* presentation;
        const MessageRecord* record;
    } poll_context{presentation, record};
    const auto append_poll = +[](void* source, const UiTextCommand& command, u32 ordinal) noexcept {
        const auto& poll_text = *static_cast<PollTextContext*>(source);
        auto* p = poll_text.presentation;
        Utf8View retained{};
        if (!retain_text(p, command.text, &retained))
            return false;
        return append_marked(p, retained,
                             command.style_flags != text_style_none ? TextStyleSpan{0, command.text.bytes, command.style_flags}
                                                                    : TextStyleSpan{},
                             nullptr, command.owner, command.rectangle, command.clip, command.color,
                             poll_text.record,
                             kInvalidMessageTextMeasurement,
                             {.horizontal = command.horizontal_alignment,
                              .vertical = command.vertical_alignment,
                              .wrapping = command.wrapping,
                              .slot = static_cast<u8>(ordinal),
                              .overflow = command.overflow});
    };
    UiTextEmitter emitter{};
    UiTextOutput output{nullptr, &emitter};
    const UiPollStyle poll_style = ui_theme_poll_style(scene.theme_styles);
    if (!message_scene_poll_projection(scene, row, theme, {answers, kUiPollMaximumAnswers}, &poll) ||
        !ui_text_emitter_initialize(&emitter, append_poll, &poll_context) ||
        !ui_poll_controls_text_output(poll, poll_style, row_clip, {&scene.widgets, row.poll}, &output))
        return fail(presentation, MessageTextPresentationError::layout);
    return true;
}

// A row's body as its quote and code segments, one command each. The segment
// array is large, so it has a frame of its own.
__declspec(noinline) bool append_content_segments(
    MessageTextPresentation* presentation, const MessageScene& scene, const MessageSceneRowWidget& row,
    const Utf8View& display_content, const ConstSlice<TextStyleSpan>& styles, const ConstSlice<MessageMarkdownSemantic>& semantics,
    bool split, float content_top, float text_left, float text_width, const UiRect& row_clip,
    const Color& content_color, const MessageRecord* record, u16 measurement_index, u16 source_id,
    const Color& role_color, u64 edited_unix_seconds) noexcept {
    QuoteSegment segments[kQuoteSegmentMaximum];
    // Compact rows keep the name inline with the first line of the body.
    u32 segment_count = split ? split_quote_segments(display_content, styles, semantics, segments) : 0u;
    if (segment_count == 0) {
        segments[0] = QuoteSegment{display_content, styles, semantics, false, false};
        segment_count = 1;
    }
    if (edited_unix_seconds != 0) {
        // The label follows the last plain text; after a quote or a code block, or with no text at all, it
        // starts a line of its own. Without room the label is left out.
        u32 last = segment_count;
        while (last != 0 && segments[last - 1].text.bytes == 0)
            --last;
        if (last != 0 && !segments[last - 1].quote && !segments[last - 1].code) {
            QuoteSegment& tail = segments[last - 1];
            append_edited_label(presentation, edited_unix_seconds, &tail.text, &tail.styles, &tail.semantics);
        } else if (segment_count < kQuoteSegmentMaximum) {
            QuoteSegment& tail = segments[segment_count];
            tail = QuoteSegment{};
            if (append_edited_label(presentation, edited_unix_seconds, &tail.text, &tail.styles, &tail.semantics))
                ++segment_count;
        }
    }
    bool first_segment = true;
    for (u32 index = 0; index < segment_count; ++index) {
        const QuoteSegment& segment = segments[index];
        if (segment.text.bytes == 0)
            continue;
        float left = text_left + (segment.quote ? quote_text_inset() : 0.0f);
        float width = segment.quote ? quote_text_width(text_width) : text_width;
        if (segment.code) {
            // A message code block is at most 90% of the markup width.
            if (first_segment)
                content_top += kMessageCodeBlockInset;
            left += kMessageCodeBlockInset;
            width = maximum(1.0f, width * 0.9f - 2.0f * kMessageCodeBlockInset);
        }
        first_segment = false;
        const UiRect content_rectangle = line_rectangle(left, content_top, width);
        if (!append_command(presentation, segment.text, segment.styles, segment.semantics,
                            UiWidgetReference{&scene.widgets, row.content}, content_rectangle, row_clip,
                            content_color, record, measurement_index,
                            {.kind = MessageTextMeasurementKind::content,
                             .gap_after = quote_gap_after(segments, segment_count, index, 0.0f),
                             .themed = segment.styles.count != 0,
                             .source_id = source_id},
                            role_color)) {
            return false;
        }
        const MessageTextCommand& command = presentation->commands[presentation->command_count - 1];
        content_top += command.measured_height + command.gap_after;
    }
    return true;
}
} // namespace text_presentation_detail
} // namespace maicord
