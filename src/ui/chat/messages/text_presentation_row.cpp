#include "text_presentation_internal.hpp"
#include "maicord/ui/chat/messages/unread_divider.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/text/timestamp_format.hpp"
#include "maicord/ui/chat/messages/reaction_presentation.hpp"
#include "maicord/ui/chat/messages/component_layout.hpp"
#include "maicord/model/chat/message_poll_store.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/model/guild/member_store.hpp"
#include "maicord/model/user/store.hpp"
#include "maicord/ui/shell/navigation_view.hpp"
#include "maicord/ui/text/measure_cache.hpp"

namespace maicord {
using namespace text_presentation_detail;

namespace text_presentation_detail {
namespace {
constexpr CommandLook kLookStartThemed{.themed = true,
                                       .wrapping = TextWrapping::no_wrap,
                                       .overflow = TextOverflow::ellipsis};
constexpr CommandLook kLookStamp{.themed = true, .wrapping = TextWrapping::no_wrap, .slot = 1};
constexpr CommandLook kLookMiddleSecond{.vertical = UiTextAlignment::center,
                                        .wrapping = TextWrapping::no_wrap,
                                        .slot = 1,
                                        .overflow = TextOverflow::ellipsis};
constexpr CommandLook kLookMiddleClipped{.vertical = UiTextAlignment::center, .wrapping = TextWrapping::no_wrap};
constexpr CommandLook kLookEnd{.horizontal = UiTextAlignment::end,
                               .vertical = UiTextAlignment::center,
                               .wrapping = TextWrapping::no_wrap,
                               .overflow = TextOverflow::ellipsis};
bool append_rich_line(MessageTextPresentation* presentation, Utf8View text, UiTextFlowLayout* flow,
                      const Color& color, const UiWidgetReference& owner, const MessageRecord& record,
                      u16 measurement_index, MessageTextMeasurementKind measurement_kind) noexcept {
    if (!text.data && text.bytes != 0) {
        return fail(presentation, MessageTextPresentationError::model);
    }
    if (text.bytes == 0) {
        return true;
    }
    UiRect rectangle{};
    if (!begin_item(presentation, flow, 0.0f, 0.0f, &rectangle)) {
        return false;
    }
    if (!append_command(presentation, text, kNoStyles, kNoSemantics, owner, rectangle, flow->clip_rectangle,
                        color, &record, measurement_index,
                        {.kind = measurement_kind, .minimum_height = 16.0f, .gap_after = 3.0f})) {
        return false;
    }
    return commit_last_command(presentation, flow);
}

Color member_role_color(const MessageTextPresentation& presentation, const ChatStateStore& state,
                        u64 channel_id, u64 user_id, const Color& fallback) noexcept {
    const ChannelRecord* channel = chat_state_store_find_channel(state, channel_id);
    const GuildMemberRecord* member =
        presentation.guild_members && channel && channel->guild_id != 0 && user_id != 0
            ? guild_member_store_find(*presentation.guild_members, channel->guild_id, user_id)
            : nullptr;
    return member && member->role_color != 0 ? color_from_rgb24(member->role_color) : fallback;
}

struct RetainedAuthorHeader {
    Utf8View text;
    ConstSlice<TextStyleSpan> styles;
};

RetainedAuthorHeader retain_author_header(MessageTextPresentation* presentation, Utf8View name,
                                          u64 timestamp_ms, u64 now_unix_seconds) noexcept {
    if (!presentation || name.bytes == 0 || !name.data) {
        return RetainedAuthorHeader{};
    }
    char timestamp_storage[kMessageMarkdownTimestampMaximumBytes];
    const TimestampFormatResult timestamp =
        timestamp_ms != 0
            ? format_timestamp_local(timestamp_ms / 1'000u, 't', now_unix_seconds,
                                             Slice<char>{timestamp_storage, sizeof(timestamp_storage)})
            : TimestampFormatResult{};
    const u32 separator = timestamp.ok && timestamp.bytes != 0 ? 1u : 0u;
    const u32 timestamp_bytes = separator != 0 ? timestamp.bytes : 0u;
    if (name.bytes + separator + timestamp_bytes > presentation->utf8.count - presentation->utf8_count ||
        1u + (timestamp_bytes != 0 ? 1u : 0u) > presentation->styles.count - presentation->style_count) {
        return RetainedAuthorHeader{};
    }
    const u32 text_begin = presentation->utf8_count;
    const u32 style_begin = presentation->style_count;
    copy_bytes(presentation->utf8.data + presentation->utf8_count, name.data, name.bytes);
    presentation->utf8_count += name.bytes;
    presentation->styles[presentation->style_count++] = TextStyleSpan{0, name.bytes, text_style_bold};
    if (separator != 0) {
        presentation->utf8.data[presentation->utf8_count++] = ' ';
        for (u32 index = 0; index < timestamp_bytes; ++index) {
            presentation->utf8.data[presentation->utf8_count++] = timestamp_storage[index];
        }
        presentation->styles[presentation->style_count++] =
            TextStyleSpan{name.bytes + separator, timestamp_bytes, text_style_subtext};
    }
    return RetainedAuthorHeader{
        Utf8View{presentation->utf8.data + text_begin, presentation->utf8_count - text_begin},
        ConstSlice<TextStyleSpan>{presentation->styles.data + style_begin,
                                  presentation->style_count - style_begin}};
}

RetainedAuthorHeader retain_styled_text(MessageTextPresentation* presentation, const Utf8View& text, u32 flags) noexcept {
    if (!presentation || text.bytes == 0 || !text.data ||
        text.bytes > presentation->utf8.count - presentation->utf8_count ||
        presentation->style_count >= presentation->styles.count)
        return RetainedAuthorHeader{};
    const u32 text_begin = presentation->utf8_count;
    copy_bytes(presentation->utf8.data + text_begin, text.data, text.bytes);
    presentation->utf8_count += text.bytes;
    presentation->styles[presentation->style_count] = TextStyleSpan{0, text.bytes, flags};
    return RetainedAuthorHeader{Utf8View{presentation->utf8.data + text_begin, text.bytes},
                                ConstSlice<TextStyleSpan>{presentation->styles.data + presentation->style_count++, 1}};
}
float result_card_text_width(const MessageScene& scene, const MessageRecord& record, const Utf8View& text, u32 flags,
                             u16 item, float constraint) noexcept {
    UiTextMeasureCache* cache = scene.polls ? scene.polls->measure : nullptr;
    const TextStyleSpan span{0, text.bytes, flags};
    UiTextMeasureValue value{};
    u64 signature = hash64_counted_bytes(kHash64Seed, text.data, text.bytes);
    signature = hash64_mix(signature, flags);
    if (cache && constraint > 0.0f &&
        ui_text_measure_cache_measure(cache,
                                      {{record.id, signature, record.revision, item, scene.polls->backend_revision},
                                       text,
                                       ConstSlice<TextStyleSpan>{&span, 1},
                                       {constraint, kMaximumUiCoordinate},
                                       true},
                                      &value)
            .ok() &&
        ui_coordinate_valid(value.size.width) && value.size.width > 0.0f)
        return value.size.width;
    // Without a measurer, a generous per-byte estimate keeps the timestamp whole.
    return static_cast<float>(text.bytes) * ((flags & text_style_subtext) != 0 ? 7.0f : 9.0f);
}
// Search result cards have no day dividers, so the header carries the calendar
// timestamp ("Today at", "Yesterday at", or the short date) beside the name.
// Name and timestamp are separate commands laid out from measured widths: the
// timestamp always fits and follows the name, and a long name truncates first.
bool append_result_card_header(MessageTextPresentation* presentation, const MessageScene& scene,
                               const MessageSceneRowWidget& row, const UiRect& author, const UiRect& clip, const Color& color,
                               const MessageRecord& record, Utf8View name, u64 now_unix_seconds) noexcept {
    char storage[kMessageMarkdownTimestampMaximumBytes];
    const TimestampFormatResult stamp =
        record.timestamp_ms != 0
            ? format_timestamp_calendar(record.timestamp_ms / 1'000u, now_unix_seconds,
                                        Slice<char>{storage, sizeof(storage)})
            : TimestampFormatResult{};
    if (stamp.ok && stamp.next_update_unix_seconds != 0 &&
        (presentation->next_timestamp_update_unix_seconds == 0 ||
         stamp.next_update_unix_seconds < presentation->next_timestamp_update_unix_seconds))
        presentation->next_timestamp_update_unix_seconds = stamp.next_update_unix_seconds;
    const RetainedAuthorHeader retained_name = retain_styled_text(presentation, name, text_style_bold);
    const RetainedAuthorHeader retained_stamp =
        stamp.ok && stamp.bytes ? retain_styled_text(presentation, Utf8View{storage, stamp.bytes}, text_style_subtext)
                                : RetainedAuthorHeader{};
    if (retained_name.text.bytes == 0 || (stamp.ok && stamp.bytes && retained_stamp.text.bytes == 0))
        return fail(presentation, MessageTextPresentationError::capacity);
    const float width = ui_rect_width(author);
    const float stamp_width =
        retained_stamp.text.bytes
            ? result_card_text_width(scene, record, retained_stamp.text, text_style_subtext, 0x7473u, width)
            : 0.0f;
    const float name_width = result_card_text_width(scene, record, retained_name.text, text_style_bold, 0x6e6du, width);
    auto layout = message_result_card_header_layout(author.left, author.right, name_width, stamp_width);
    layout.name.top = layout.timestamp.top = author.top;
    layout.name.bottom = layout.timestamp.bottom = author.bottom;
    const UiWidgetReference owner{&scene.widgets, row.author};
    if (!append_command(presentation, retained_name.text, retained_name.styles, kNoSemantics,
                        owner, layout.name, clip, color, &record,
                        kInvalidMessageTextMeasurement, kLookStartThemed))
        return false;
    return !ui_rect_has_area(layout.timestamp) ||
           append_command(presentation, retained_stamp.text, retained_stamp.styles,
                          kNoSemantics, owner, layout.timestamp, clip, color, &record,
                          kInvalidMessageTextMeasurement, kLookStamp);
}

} // namespace
bool append_row(MessageTextPresentation* presentation, const MessageScene& scene,
                const MessageSceneRowWidget& row, const ChatStateStore& state,
                const UserStore* users, const NavigationView& navigation, const Theme& theme,
                u64 now_unix_seconds, bool forwarded) noexcept {
    if (row.redacted || scene.content_gated)
        return true;
    const MessageStore* const store = scene.timeline ? scene.timeline->store : nullptr;
    const MessageRecord* record = store ? message_store_resolve(*store, row.message) : nullptr;
    if (!record || record->id != row.message_id) {
        return fail(presentation, MessageTextPresentationError::model);
    }
    presentation->failure_message_id = record->id;
    presentation->failure_source_index = row.source_index;
    set_failure_context(presentation, MessageTextPresentationStage::row,
                        MessageTextPresentationOperation::resolve_layout);
    if (presentation->measurement_count >= presentation->measurements.count) {
        return fail(presentation, MessageTextPresentationError::capacity);
    }
    const UiLayoutNode* row_layout = message_scene_layout_node(scene, row.widget);
    const UiLayoutNode* reply = message_scene_layout_node(scene, row.reply);
    const UiLayoutNode* author = message_scene_layout_node(scene, row.author);
    const UiLayoutNode* content_layout = message_scene_layout_node(scene, row.content);
    // Rich groups exist only for rows that have such items (the item nodes
    // carry their own checks); the notice group is always present.
    const UiLayoutNode* notice = message_scene_layout_node(scene, row.notice);
    if (!row_layout || !reply || (!forwarded && !author) || !content_layout || !notice) {
        return fail(presentation, MessageTextPresentationError::layout);
    }

    // Rows clip to their own rectangle, never the viewport: overscan rows are
    // measured before they scroll in, and a scrolled projection stays exact.
    // The renderer limits paint to the viewport.
    const UiRect row_clip = row_layout->rectangle;
    const float text_left = content_layout->content_rectangle.left;
    const float text_width = ui_rect_width(content_layout->content_rectangle);
    // Transient zero-area rows are valid retained states. They must not poison
    // every other message by failing the atomic build.
    if (!ui_rect_has_area(row_clip) || text_width <= 0.0f) {
        return true;
    }
    const auto divider = message_scene_unread_divider_rectangle(scene, row);
    if (ui_rect_has_area(divider)) {
        const auto geometry = ui_unread_divider_geometry(divider, scene.unread_divider_style);
        if (!append_command(presentation, kUiUnreadDividerLabel, kNoStyles, kNoSemantics,
                            UiWidgetReference{&scene.widgets, row.widget}, geometry.label, row_clip,
                            ui_unread_divider_text_color(theme), nullptr, kInvalidMessageTextMeasurement,
                            kLookEnd))
            return false;
    }

    // Plugin decoration verdict cached for this revision (no plugin code runs); borrowed from the one-entry
    // memo, which only this message's own lookups touch while the row is built.
    const auto* decoration = forwarded ? nullptr : message_scene_row_decoration(scene, *record);
    const bool collapsed = decoration && decoration->visibility == ui_plugin_render_collapsed;
    const bool replaced = decoration && !collapsed && decoration->replace_content &&
                          record->content_availability == MessageContentAvailability::available;
    Utf8View content{};
    Color content_color = theme.text_primary;
    if (collapsed) {
        content_color = ui_background_muted_text_color(theme);
    } else if (replaced) {
        // Display-only markdown; the stored message text is untouched.
        content = decoration->content;
    } else if (record->content_availability == MessageContentAvailability::available) {
        content = message_store_content(*store, row.message);
    } else if (record->content_availability == MessageContentAvailability::restricted) {
        content = utf8_literal("Message content is unavailable");
        content_color = ui_background_muted_text_color(theme);
    } else {
        content = utf8_literal("Loading…");
        content_color = ui_background_muted_text_color(theme);
    }
    if (record->delivery == MessageDeliveryState::pending || record->delivery == MessageDeliveryState::retained) {
        content_color = ui_background_muted_text_color(theme);
    } else if (record->delivery == MessageDeliveryState::failed) {
        content_color = theme.notification;
    }
    const u16 measurement_index = static_cast<u16>(presentation->measurement_count++);
    MessageTextMeasurement& row_measurement = presentation->measurements[measurement_index];
    row_measurement.message_id = record->id;
    row_measurement.message_revision = record->revision;
    row_measurement.source_index = row.source_index;
    row_measurement.reply_height = 0.0f;
    row_measurement.content_height = 0.0f;
    row_measurement.forwarded = forwarded;
    row_measurement.rich_layout = row.rich_layout;
    copy_value(&row_measurement.rich_item_layout, row.rich_item_layout);
    row_measurement.content_present = content.bytes != 0;
    for (u32 index = 0; index < kGatewayMessageReactionMaximum; ++index) {
        row_measurement.reaction_widths[index] = row.reaction_widths[index];
    }

    const MessageRichStore* rich_store = scene.timeline->rich_store;
    const MessageRichRecord* rich = rich_store
                                        ? (forwarded ? message_rich_store_snapshot(*rich_store, record->id)
                                                     : message_rich_store_find(*rich_store, record->id))
                                        : nullptr;
    if (rich && rich->message_revision != record->revision) {
        rich = nullptr;
    }
    if (collapsed)
        rich = nullptr; // the scene built no rich widgets for this row
    const MessageMediaPolicyResult media_policy =
        rich ? message_scene_media_policy(
                   scene, message_media_owner(*record, message_rich_store_find(*rich_store, record->id)))
             : MessageMediaPolicyResult{MessageExplicitFilter::show, true};
    if (forwarded) {
        if (!rich)
            return fail(presentation, MessageTextPresentationError::model);
        content = message_rich_store_string(*rich_store, rich->snapshot.content);
        content_color = theme.text_primary;
        row_measurement.content_present = content.bytes != 0;
        UiRect label_rectangle = reply->content_rectangle;
        label_rectangle.left = minimum(label_rectangle.right - 1.0f, label_rectangle.left + 22.0f);
        if (!append_command(presentation, utf8_literal("Forwarded"), kNoStyles, kNoSemantics,
                            UiWidgetReference{&scene.widgets, row.reply}, label_rectangle, row_clip,
                            theme.text_secondary, record,
                            kInvalidMessageTextMeasurement, kLookMiddleClipped))
            return false;
    }
    if (rich && !replaced && record->content_availability == MessageContentAvailability::available &&
        message_rich_embed_suppresses_content(*rich_store, *rich, content)) {
        reset_value<Utf8View>(&content);
        row_measurement.content_present = false;
    }
    if (!forwarded && rich && message_rich_has_header(*rich)) {
        set_failure_context(presentation, MessageTextPresentationStage::reply,
                            MessageTextPresentationOperation::initialize_flow);
        const u32 reply_begin = presentation->command_count;
        UiTextFlowLayout flow{};
        const float reply_indent = message_scene_reply_indent(scene);
        if (!initialize_flow(presentation, *reply, text_left + reply_indent, maximum(1.0f, text_width - reply_indent),
                             row_clip, &flow)) {
            return false;
        }
        Utf8View label{};
        const bool interaction_header = rich->reference.state == GatewayMessageReferenceState::none;
        if (interaction_header) {
            label = member_nickname(*presentation, state, record->channel_id, rich->interaction.user_id);
            if (label.bytes == 0)
                label = message_rich_store_string(*rich_store, rich->interaction.user_name);
            if (label.bytes == 0)
                label = utf8_literal("Unknown user");
        } else if (rich->reference.state == GatewayMessageReferenceState::available) {
            label = member_nickname(*presentation, state, record->channel_id, rich->reference.author_id);
            if (label.bytes == 0)
                label = message_rich_store_string(*rich_store, rich->reference.author_name);
            if (label.bytes == 0 && users) {
                const auto* user = user_store_find(*users, rich->reference.author_id);
                if (user)
                    label = user_store_name(*users, *user);
            }
            if (label.bytes == 0) {
                label = utf8_literal("Reply");
            }
        } else if (rich->reference.state == GatewayMessageReferenceState::deleted) {
            label = utf8_literal("Original message was deleted");
        } else {
            label = utf8_literal("Message could not be loaded");
        }
        const bool reference_available = rich->reference.state == GatewayMessageReferenceState::available;
        const Utf8View reference_content = message_rich_store_string(*rich_store, rich->reference.content);
        // A resident poll keeps its question in the poll store, not the quote.
        Utf8View poll_question{};
        if (reference_available && !interaction_header && reference_content.bytes == 0 && scene.polls &&
            scene.polls->store) {
            if (const auto* poll = message_poll_store_find(*scene.polls->store, rich->reference.message_id))
                poll_question = message_poll_store_string(*scene.polls->store, poll->question);
        }
        BuildContext reply_context{presentation, &state, users, &navigation, record, now_unix_seconds};
        // A quoted join, app-premium or poll-result notice reads as its system line, without an author name.
        const MessageReplyLeading leading = interaction_header ? MessageReplyLeading::avatar
                                                               : message_rich_reply_leading(rich->reference);
        RetainedReplySummary summary{};
        if (leading != MessageReplyLeading::avatar)
            summary = retain_quoted_system(presentation, rich->reference.message_id, label, reference_content,
                                           rich->reference.message_type);
        // A reply that points into a clip quotes the clip's title and the moment instead of the text.
        const bool media_mention = reference_available && !interaction_header &&
                                   (rich->reference.quote_flags & gateway_quote_media_mention) != 0;
        if (summary.text.bytes == 0 && media_mention)
            summary = retain_reply_media_mention(presentation, label,
                                                 (rich->reference.content_flags & gateway_reference_mentions_author) != 0,
                                                 reference_content);
        if (summary.text.bytes == 0)
            summary = interaction_header
                          ? retain_interaction_header(
                                presentation, label,
                                message_rich_store_string(*rich_store, rich->interaction.command_name),
                                message_interaction_chip(record->type))
                      : reference_available
                          ? retain_reply_summary(presentation, &reply_context, label, reference_content, true,
                                                 rich->reference.content_flags, rich->reference.message_type,
                                                 poll_question, rich->reference.quote_flags,
                                                 scene.display.reply_timestamps
                                                     ? ((rich->reference.message_id >> 22) + kSnowflakeEpochMilliseconds) / 1000u
                                                     : 0u)
                          : retain_reply_placeholder(presentation, label);
        const MessageReplyIcon reply_icon =
            reference_available && !interaction_header && leading == MessageReplyLeading::avatar && !media_mention
                ? message_rich_reply_icon(rich->reference.content_flags, rich->reference.message_type,
                                          reference_content.bytes == 0,
                                          poll_question.bytes != 0 ||
                                              (rich->reference.content_flags & gateway_reference_poll_question))
                : MessageReplyIcon::none;
        const UiWidgetReference owner{&scene.widgets, row.reply};
        // Role colours are server data and pass through every theme unchanged.
        // The quoted author's name is muted; the
        // interaction header mutes its name when it lays the line out.
        const Color reply_author_color = member_role_color(*presentation, state, record->channel_id,
                                                           message_rich_header_user(*rich), theme.text_primary);
        const Color reply_name_color =
            interaction_header ? reply_author_color
                               : ui_theme_color_alpha(reply_author_color, kMessageReplyAuthorOpacity);
        if (summary.text.bytes == 0 ||
            !append_reply_line(presentation, summary.text, summary.styles, &flow, theme.text_primary,
                               reply_name_color, owner, *record, measurement_index,
                               TextOverflow::ellipsis,
                               reply_icon != MessageReplyIcon::none ? kMessageReplyIconGap + kMessageReplyIcon
                                                                    : 0.0f,
                               message_reply_leading_small(leading) ? kMessageReplyLeadingIconInset
                                                                    : kMessageReplyTextInset,
                               summary.semantics)) {
            return false;
        }
        if (presentation->command_count == reply_begin) {
            return fail(presentation, MessageTextPresentationError::model);
        }
        {
            const MessageTextCommand& line = presentation->commands[presentation->command_count - 1];
            presentation->measurements[measurement_index].rich_item_layout.reply_width =
                reply_icon != MessageReplyIcon::none
                    ? minimum(line.measured_width, ui_rect_width(line.rectangle))
                    : 0.0f;
        }
        if (!finish_flow(presentation, flow, &presentation->measurements[measurement_index].reply_height)) {
            return false;
        }
        saturating_accumulate(presentation->rich.replies);
    }

    // Compact rows: timestamp in the hanging-indent gutter, name inline.
    Utf8View compact_name{};
    Color compact_color{};
    if (!forwarded && (!row.grouped || scene.display.compact) && users && record->author_id != 0) {
        set_failure_context(presentation, MessageTextPresentationStage::author,
                            MessageTextPresentationOperation::append_command);
        const UserRecord* user = user_store_find(*users, record->author_id);
        Utf8View name = member_nickname(*presentation, state, record->channel_id, record->author_id);
        if (name.bytes == 0 && user)
            name = user_store_name(*users, *user);
        if (name.bytes == 0 && record->delivery != MessageDeliveryState::authoritative) {
            name = utf8_literal("You");
        }
        Color author_color = record->delivery == MessageDeliveryState::failed ? theme.notification
                             : record->delivery == MessageDeliveryState::pending
                                 ? ui_background_muted_text_color(theme)
                                 : theme.text_primary;
        author_color = ui_foreground_color(theme, author_color);
        // Role colours are server data and pass through every theme unchanged.
        if (record->delivery == MessageDeliveryState::authoritative) {
            author_color = member_role_color(*presentation, state, record->channel_id, record->author_id,
                                             author_color);
        }
        if (scene.display.compact) {
            compact_name = name;
            compact_color = author_color;
            if (!append_compact_timestamp(presentation, scene, row, row_clip, theme, *record, now_unix_seconds))
                return false;
        }
        const bool result_card_header = scene.result_cards && !scene.display.compact && name.bytes != 0;
        if (result_card_header && !append_result_card_header(presentation, scene, row, author->content_rectangle,
                                                             row_clip, author_color, *record, name,
                                                             now_unix_seconds))
            return false;
        const RetainedAuthorHeader header = scene.display.compact || result_card_header
                                                ? RetainedAuthorHeader{}
                                                : retain_author_header(presentation, name, record->timestamp_ms,
                                                                       now_unix_seconds);
        if (name.bytes != 0 && !scene.display.compact && !result_card_header &&
            (header.text.bytes == 0 ||
             !append_command(presentation, header.text, header.styles,
                             kNoSemantics,
                             UiWidgetReference{&scene.widgets, row.author}, author->content_rectangle,
                             row_clip, author_color, record,
                             kInvalidMessageTextMeasurement,
                             kLookStartThemed))) {
            return false;
        }
    }

    Utf8View display_content = content;
    ConstSlice<TextStyleSpan> styles{};
    ConstSlice<MessageMarkdownSemantic> semantics{};
    set_failure_context(presentation, MessageTextPresentationStage::content,
                        MessageTextPresentationOperation::append_command);
    BuildContext context{presentation, &state, users, &navigation, record, now_unix_seconds};
    context.rich_store = rich_store;
    context.rich = rich;
    context.forwarded = forwarded;
    context.source_id = forwarded  ? kRichTextSource | kForwardedTextSource | kRichTextBody
                        : replaced  ? kUiPluginRenderContentSource
                        : collapsed ? kUiPluginRenderGeneratedSource
                                    : 0;
    const bool parse = (forwarded || record->content_availability == MessageContentAvailability::available) &&
                       content.bytes != 0;
    const bool system_row = !forwarded && !replaced && !collapsed && message_type_is_system(record->type) &&
                            record->content_availability == MessageContentAvailability::available;
    const bool thinking =
        !forwarded && !collapsed && (record->flags & kMessageFlagLoading) != 0 && content.bytes == 0;
    // A local command message shows its status text as
    // is ("Sending command...", the refusal), after a status icon.
    const bool command_status = !forwarded && !replaced && !collapsed && !thinking && content.bytes != 0 &&
                                (record->type == 20 || record->type == 23) &&
                                record->delivery != MessageDeliveryState::authoritative;
    if (system_row) {
        if (!retain_system_text(presentation, state, users, *record, rich_store, rich, content, now_unix_seconds,
                                &display_content, &styles, &semantics))
            return false;
        content_color = ui_background_muted_text_color(theme);
        row_measurement.content_present = true;
    } else if (thinking || command_status) {
        // Deferred interaction response: the
        // app "is thinking..." until 15 minutes after the message id, then
        // "The application did not respond" in the critical colour.
        bool failed = record->delivery == MessageDeliveryState::failed;
        Utf8View name{}, suffix = content;
        if (thinking) {
            constexpr u64 kInteractionResponseWindowSeconds = 15 * 60;
            const u64 expires = snowflake_timestamp_ms(record->id) / 1000 + kInteractionResponseWindowSeconds;
            failed = now_unix_seconds >= expires;
            if (!failed && (presentation->next_timestamp_update_unix_seconds == 0 ||
                            expires < presentation->next_timestamp_update_unix_seconds))
                presentation->next_timestamp_update_unix_seconds = expires;
            const UserRecord* app_user = users ? user_store_find(*users, record->author_id) : nullptr;
            name = failed ? Utf8View{} : app_user ? user_store_name(*users, *app_user) : utf8_literal("The app");
            suffix = failed ? utf8_literal("The application did not respond") : utf8_literal(" is thinking...");
        }
        // An icon cell of EM SPACEs (one for the failure icon, two for the
        // three dots), a 4 DIP gap and a joiner, as link chips do.
        const Utf8View cell = failed ? utf8_literal("\xE2\x80\x83\xE2\x80\x85\xE2\x80\x8D")
                                     : utf8_literal("\xE2\x80\x83\xE2\x80\x83\xE2\x80\x85\xE2\x80\x8D");
        const u32 bytes = cell.bytes + name.bytes + suffix.bytes;
        if (bytes > presentation->utf8.count - presentation->utf8_count ||
            presentation->semantic_count >= presentation->semantics.count)
            return fail(presentation, MessageTextPresentationError::capacity);
        char* retained = presentation->utf8.data + presentation->utf8_count;
        copy_bytes(retained, cell.data, cell.bytes);
        copy_bytes(retained + cell.bytes, name.data, name.bytes);
        copy_bytes(retained + cell.bytes + name.bytes, suffix.data, suffix.bytes);
        presentation->utf8_count += bytes;
        display_content = {retained, bytes};
        auto& icon = presentation->semantics[presentation->semantic_count++];
        icon = MessageMarkdownSemantic{
            static_cast<u64>(failed ? MessageMarkdownChipIcon::failed : MessageMarkdownChipIcon::pending_dots), 0,
            failed ? 3u : 6u, 0, 0, MessageMarkdownSemanticKind::chip_icon, 0};
        semantics = {&icon, 1};
        if (failed)
            content_color = theme.notification;
        else
            presentation->status_dots = true;
        row_measurement.content_present = true;
    } else if (compact_name.bytes) {
        if (!compact_content(presentation, &context, compact_name, parse ? content : Utf8View{}, &display_content,
                             &styles, &semantics, decoration))
            return false;
    } else if (parse && !parse_content(presentation, &context, content, &display_content, &styles, &semantics)) {
        return false;
    }
    if (collapsed) {
        if (!append_plugin_collapsed(presentation, *decoration, &display_content, &styles, &semantics))
            return false;
        row_measurement.content_present = true;
    }
    // The edit label follows a message's text; one without text keeps it on a line below its accessories.
    const bool edited = !forwarded && !collapsed && !thinking && !command_status && !system_row &&
                        record->edited_timestamp_ms >= 1'000u &&
                        record->content_availability == MessageContentAvailability::available &&
                        record->delivery == MessageDeliveryState::authoritative;
    const bool edited_tail = edited && row.edited.slot != kInvalidUiWidgetSlot;
    const u64 edited_inline = edited && !edited_tail ? record->edited_timestamp_ms / 1'000u : 0u;
    if (edited_inline)
        row_measurement.content_present = true;
    if (!append_content_segments(presentation, scene, row, display_content, styles, semantics,
                                 parse && !system_row && !thinking && !command_status,
                                 content_layout->content_rectangle.top, text_left, text_width, row_clip, content_color,
                                 record, measurement_index, context.source_id,
                                 compact_name.bytes ? compact_color : kNoRoleColor, edited_inline))
        return false;

    if (!forwarded && (row.poll_unavailable || row.poll_content_revision) &&
        !append_poll_text(presentation, scene, row, theme, row_clip, record))
        return false;
    if (!forwarded && row.invite_item_count &&
        !append_invite_cards(presentation, scene, row, *record, state, theme, row_clip, measurement_index,
                             now_unix_seconds))
        return false;
    row_measurement.rich_layout.edited = row_measurement.rich_item_layout.edited_height = 0.0f;
    if (edited_tail && !append_edited_tail(presentation, scene, row, row_clip, theme, *record, measurement_index))
        return false;
    if (!rich) {
        return append_annotations(presentation, scene, row, *record, theme, row_clip, measurement_index, forwarded,
                                  0.0f, collapsed ? nullptr : decoration);
    }
    if (!append_rich_media_text(context, scene, row, theme, row_clip, measurement_index, text_width,
                                media_policy))
        return false;

    if (rich->sticker_count != 0) {
        // Stickers draw no caption: the name is their tooltip's, popout's and
        // accessible name.
        auto& measured = presentation->measurements[measurement_index];
        for (u32 index = 0; index < rich->sticker_count; ++index) {
            measured.rich_item_layout.heights[kMessageSceneStickerItemOffset + index] =
                kDefaultMessageStickerLayoutMetrics.extent;
            saturating_accumulate(presentation->rich.stickers);
        }
        measured.rich_layout.stickers = kDefaultMessageStickerLayoutMetrics.group_height(rich->sticker_count);
    }

    if (rich->component_count != 0) {
        for (u32 index = 0; index < rich->component_count; ++index) {
            set_failure_context(presentation, MessageTextPresentationStage::component,
                                MessageTextPresentationOperation::resolve_layout, static_cast<u16>(index));
            const MessageRichComponent& component = rich->components[index];
            const MessageComponentKind kind = message_component_kind(component);
            const u32 slot = kMessageSceneComponentItemOffset + index;
            auto& measured_items = row_measurement.rich_item_layout;
            const bool text_leaf = kind == MessageComponentKind::text || kind == MessageComponentKind::button ||
                                   kind == MessageComponentKind::select || kind == MessageComponentKind::file;
            // Inside an unrevealed spoiler container, text is clipped away
            // (still measured, so revealing moves nothing) and media add no
            // labels: the container carries the one spoiler label.
            const u32 spoiler = message_component_spoiler_container(*rich, index);
            const MessageMediaObscure concealed =
                spoiler == kMessageComponentNone
                    ? MessageMediaObscure::none
                    : message_scene_media_obscure(scene, media_policy, 0,
                                                  message_media_reveal_key(record->id, MessageMediaSource::component,
                                                                           spoiler, row.forwarded_snapshot),
                                                  true, false);
            const bool hidden = concealed != MessageMediaObscure::none && spoiler != index;
            const UiRect component_clip =
                hidden ? UiRect{row_clip.left, row_clip.top, row_clip.left + 0.01f, row_clip.top + 0.01f} : row_clip;
            // Parents are folded from their measured children below; media
            // leaves and separators have fixed extents.
            if (!text_leaf) {
                const UiLayoutNode* node =
                    message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::component, index);
                if (kind == MessageComponentKind::container && concealed != MessageMediaObscure::none && node &&
                    !append_obscure_label(
                        presentation, concealed, node->rectangle,
                        {&scene.widgets,
                         message_scene_rich_item_widget(row, MessageSceneRichItemKind::component, index)},
                        row_clip, theme, *record, measurement_index))
                    return false;
                if (!hidden && (kind == MessageComponentKind::thumbnail || kind == MessageComponentKind::gallery_item)) {
                    const MessageMediaObscure obscure = message_scene_media_obscure(
                        scene, media_policy, 0,
                        message_media_reveal_key(record->id, MessageMediaSource::component, index,
                                                 row.forwarded_snapshot),
                        message_rich_component_spoiler(component), false);
                    if (node && obscure != MessageMediaObscure::none &&
                        !append_obscure_label(
                            presentation, obscure, node->rectangle,
                            {&scene.widgets, message_scene_rich_item_widget(row, MessageSceneRichItemKind::component,
                                                                            index)},
                            row_clip, theme, *record, measurement_index))
                        return false;
                }
                measured_items.heights[slot] = message_component_leaf_height(component);
                if (kind == MessageComponentKind::thumbnail)
                    measured_items.component_widths[index] = kMessageComponentThumbnailExtent;
                if (kind != MessageComponentKind::thumbnail && kind != MessageComponentKind::separator)
                    saturating_accumulate(presentation->rich.skipped_components);
                if (kind == MessageComponentKind::gallery) {
                    const UiLayoutNode* gallery =
                        message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::component, index);
                    const UiLayoutNode* region = message_scene_layout_node(scene, row.components);
                    const float available = region ? ui_rect_width(region->content_rectangle)
                                            : gallery ? ui_rect_width(gallery->rectangle)
                                                      : text_width;
                    // Containers cap their width; galleries inside them fit the padded lane.
                    float ancestors = 0;
                    bool contained = false;
                    for (u16 at = component.parent; at != 0xFFFFu; at = rich->components[at].parent)
                        if (rich->components[at].type == gateway_component_container) {
                            ancestors += 2 * kMessageComponentPadding;
                            contained = true;
                        }
                    const float lane = contained ? minimum(available, kMessageComponentMaximumWidth) : available;
                    measured_items.component_widths[index] =
                        message_component_leaf_width(component, maximum(1.0f, lane - ancestors));
                }
                continue;
            }
            const UiWidgetHandle item_widget =
                message_scene_rich_item_widget(row, MessageSceneRichItemKind::component, index);
            const UiLayoutNode* item_layout =
                message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::component, index);
            if (!item_layout) {
                return fail(presentation, MessageTextPresentationError::layout);
            }
            const UiWidgetReference owner{&scene.widgets, item_widget};
            const UiRect box = item_layout->rectangle;
            const UiRect item_clip = ui_rect_intersection(component_clip, box);
            const u32 command_begin = presentation->command_count;
            if (kind == MessageComponentKind::text) {
                // Text inside a container uses the reduced .875rem size.
                bool contained = false;
                for (u16 at = component.parent; at != 0xFFFFu && !contained; at = rich->components[at].parent)
                    contained = rich->components[at].type == gateway_component_container;
                UiTextFlowLayout flow{};
                if (!initialize_flow(presentation, *item_layout, item_layout->content_rectangle.left,
                                     ui_rect_width(item_layout->content_rectangle), component_clip, &flow) ||
                    !append_rich_markdown_line(
                        &context, message_rich_store_string(*rich_store, component.content), &flow, 0.0f, 0.0f,
                        theme.text_primary, owner, measurement_index, MessageTextMeasurementKind::component, 3.0f, 0,
                        contained ? text_size_sm : text_size_inherit)) {
                    return false;
                }
                set_failure_operation(presentation, MessageTextPresentationOperation::ensure_extent);
                if (!ui_text_flow_ensure_extent(&flow, message_component_leaf_height(component))) {
                    presentation->failure_flow_error = flow.last_error;
                    return fail(presentation, MessageTextPresentationError::layout);
                }
                if (!finish_flow(presentation, flow, &measured_items.heights[slot]))
                    return false;
                float measured_width = 0;
                for (u32 c = command_begin; c < presentation->command_count; ++c)
                    measured_width = maximum(measured_width, presentation->commands[c].measured_width);
                if (measured_width > 0)
                    measured_items.component_widths[index] =
                        static_cast<float>(static_cast<u32>(measured_width + 0.999f));
                saturating_accumulate(presentation->rich.components);
                continue;
            }
            // Single-line controls: button labels, select placeholders and file names.
            Utf8View label{};
            Color color = theme.text_primary;
            float left_inset = kMessageComponentButtonPadding, right_inset = kMessageComponentButtonPadding;
            UiTextAlignment alignment = UiTextAlignment::start;
            float fixed_height = message_component_leaf_height(component);
            ConstSlice<TextStyleSpan> styles{};
            if (kind == MessageComponentKind::button) {
                label = message_rich_store_string(*rich_store, component.label);
                color = message_component_button_colors(theme, component.style, component.disabled).text;
                // Unicode emoji render as text ahead of the 14-DIP label;
                // custom emoji are media painted in the leading inset.
                const Utf8View unicode =
                    component.emoji_id ? Utf8View{} : message_rich_store_string(*rich_store, component.emoji_name);
                if (message_component_is_link(component))
                    right_inset += kMessageComponentLinkIconGap + kMessageComponentLinkIconExtent;
                if (unicode.bytes) {
                    const u32 bytes = unicode.bytes + (label.bytes ? 1u : 0u) + label.bytes;
                    if (bytes > presentation->utf8.count - presentation->utf8_count)
                        return fail(presentation, MessageTextPresentationError::capacity);
                    char* retained = presentation->utf8.data + presentation->utf8_count;
                    copy_bytes(retained, unicode.data, unicode.bytes);
                    if (label.bytes) {
                        retained[unicode.bytes] = ' ';
                        copy_bytes(retained + unicode.bytes + 1u, label.data, label.bytes);
                    }
                    label = {retained, bytes};
                    presentation->utf8_count += bytes;
                }
                if (component.emoji_id)
                    left_inset += kMessageComponentEmojiExtent + (label.bytes ? kMessageComponentEmojiGap : 0.0f);
                // The emoji keeps the body size; the label is .875rem.
                if (label.bytes) {
                    if (presentation->styles.count - presentation->style_count < 2)
                        return fail(presentation, MessageTextPresentationError::capacity);
                    TextStyleSpan* spans = presentation->styles.data + presentation->style_count;
                    u32 count = 0;
                    if (unicode.bytes)
                        spans[count++] = {0, unicode.bytes, 0, text_size_md, {}};
                    if (label.bytes > unicode.bytes)
                        spans[count++] = {unicode.bytes, label.bytes - unicode.bytes, 0, text_size_sm, {}};
                    presentation->style_count += count;
                    styles = {spans, count};
                }
                alignment = UiTextAlignment::center;
                // A pending interaction replaces the label with loading dots.
                if (scene.interaction.pending && scene.interaction.message_id == record->id &&
                    scene.interaction.component_index == index)
                    reset_value(&label);
            } else if (kind == MessageComponentKind::select) {
                // Default options read as the current choice, else the placeholder.
                color = ui_background_muted_text_color(theme);
                for (u32 option = index + 1u; option < rich->component_count && !label.bytes; ++option)
                    if (rich->components[option].parent == index &&
                        rich->components[option].type == gateway_component_select_option &&
                        (rich->components[option].flags & gateway_message_component_default)) {
                        label = message_rich_store_string(*rich_store, rich->components[option].label);
                        color = theme.text_primary;
                    }
                if (!label.bytes)
                    label = message_rich_store_string(*rich_store, component.content);
                if (!label.bytes)
                    label = utf8_literal("Make a selection");
                left_inset = kMessageComponentSelectPadding;
                right_inset = kMessageComponentSelectIconLane;
            } else {
                label = message_rich_store_string(*rich_store, component.label);
                if (!label.bytes)
                    label = utf8_literal("File");
                left_inset += 36.0f; // file glyph
                color = theme.tokens.text.link;
            }
            const float available = ui_rect_width(box);
            const float clamped_left = minimum(left_inset, maximum(0.0f, available - 1.0f));
            const float clamped_right = minimum(right_inset, maximum(0.0f, available - 1.0f - clamped_left));
            const UiRect label_rectangle{box.left + clamped_left, box.top, box.right - clamped_right,
                                         box.top + fixed_height};
            if (label.bytes && ui_rect_has_area(item_clip) &&
                !append_command(presentation, label, styles, kNoSemantics, owner, label_rectangle, item_clip, color,
                                record, measurement_index,
                                {.kind = MessageTextMeasurementKind::component,
                                 .themed = styles.count != 0,
                                 .horizontal = alignment,
                                 .vertical = UiTextAlignment::center,
                                 .wrapping = TextWrapping::no_wrap,
                                 .slot = static_cast<u8>(index),
                                 .overflow = TextOverflow::ellipsis}))
                return false;
            measured_items.heights[slot] = fixed_height;
            if (kind == MessageComponentKind::button) {
                float measured_width = 0;
                for (u32 c = command_begin; c < presentation->command_count; ++c)
                    measured_width = maximum(measured_width, presentation->commands[c].measured_width);
                if (measured_width > 0)
                    // Round outward so a fractional glyph advance cannot clip
                    // when the hugging row is rebuilt from this measurement.
                    measured_items.component_widths[index] = maximum(
                        kMessageComponentButtonMinimumWidth,
                        static_cast<float>(static_cast<u32>(measured_width + 0.999f)) + left_inset + right_inset);
                else if (!label.bytes && !component.label.bytes)
                    measured_items.component_widths[index] =
                        maximum(kMessageComponentButtonMinimumWidth,
                                kMessageComponentEmojiExtent + 2.0f * kMessageComponentButtonPadding);
            }
            saturating_accumulate(presentation->rich.components);
        }
        // Rows wrap within the components region (or the text column before it lays out).
        const UiLayoutNode* components_region = message_scene_layout_node(scene, row.components);
        const float components_available =
            components_region ? ui_rect_width(components_region->content_rectangle) : text_width;
        const u32 error_host = forwarded ? kMessageComponentNone : failed_component_host(scene, *record);
        presentation->measurements[measurement_index].rich_layout.components = message_component_measure_tree(
            *rich, &row_measurement.rich_item_layout, maximum(1.0f, components_available), error_host);
        // The failed control's error line: after the icon, in the space the
        // measure reserved under its row or section.
        const UiLayoutNode* host =
            error_host != kMessageComponentNone
                ? message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::component, error_host)
                : nullptr;
        if (host && ui_rect_height(host->rectangle) >= kMessageComponentErrorHeight) {
            const float bottom = host->rectangle.bottom - 4.0f;
            const UiRect line{host->rectangle.left + kMessageComponentErrorIconExtent + kMessageComponentErrorIconGap,
                              bottom - (kMessageComponentErrorHeight - 8.0f),
                              maximum(host->rectangle.left + kMessageComponentErrorIconExtent +
                                          kMessageComponentErrorIconGap + 1.0f,
                                      host->rectangle.right),
                              bottom};
            if (ui_rect_has_area(ui_rect_intersection(line, row_clip)) &&
                !append_command(presentation, utf8_literal("This interaction failed"), kNoStyles, kNoSemantics,
                                {&scene.widgets, message_scene_rich_item_widget(row, MessageSceneRichItemKind::component,
                                                                                error_host)},
                                line, row_clip, theme.tokens.text.danger, record, kInvalidMessageTextMeasurement,
                                kLookMiddle))
                return false;
        }
    }

    MessageReactionTextItem reaction_items[kGatewayMessageReactionMaximum]; // the build writes the first item_count
    set_failure_context(presentation, MessageTextPresentationStage::reaction,
                        MessageTextPresentationOperation::build_reactions);
    const MessageReactionPresentationResult reaction_result =
        !message_rich_display().reactions ? MessageReactionPresentationResult{} : message_reaction_presentation_build(
        scene, row, *rich_store, *rich,
        Slice<char>{presentation->utf8.data + presentation->utf8_count,
                    presentation->utf8.count - presentation->utf8_count},
        Slice<MessageReactionTextItem>{reaction_items, kGatewayMessageReactionMaximum});
    if (!reaction_result.ok()) {
        const MessageTextPresentationError reaction_error =
            reaction_result.error == MessageReactionPresentationError::capacity
                ? MessageTextPresentationError::capacity
            : reaction_result.error == MessageReactionPresentationError::layout
                ? MessageTextPresentationError::layout
                : MessageTextPresentationError::model;
        return fail(presentation, reaction_error);
    }
    presentation->utf8_count += reaction_result.utf8_bytes;
    for (u32 index = 0; index < reaction_result.item_count; ++index) {
        const MessageReactionTextItem& item = reaction_items[index];
        if (!append_command(
                presentation, item.text, kNoStyles, kNoSemantics,
                UiWidgetReference{&scene.widgets, item.widget}, item.content_rectangle, row_clip,
                item.selected ? theme.text_primary : ui_background_muted_text_color(theme), record,
                measurement_index,
                {.kind = MessageTextMeasurementKind::reaction,
                 .vertical = UiTextAlignment::center,
                 .wrapping = TextWrapping::no_wrap,
                 .slot = static_cast<u8>(index)})) {
            return false;
        }
        saturating_accumulate(presentation->rich.reactions);
    }

    if (row.rich_layout.thread > 0.0f && rich->thread.id) {
        const auto* node = message_scene_layout_node(scene, row.thread);
        if (!node)
            return fail(presentation, MessageTextPresentationError::layout);
        const UiRect card = message_thread_card_rectangle(node->rectangle);
        const float left = card.left + kMessageThreadCardPaddingDips + 30.0f;
        const float right = maximum(left + 1.0f, card.right - kMessageThreadCardPaddingDips);
        const UiWidgetReference owner{&scene.widgets, row.thread};
        Utf8View name = message_rich_store_string(*scene.timeline->rich_store, rich->thread.name);
        if (!name.bytes)
            name = utf8_literal("Thread");
        char detail[96];
        const u32 detail_bytes = message_thread_card_detail(rich->thread, {detail, sizeof(detail)});
        const Utf8View meta = detail_bytes ? Utf8View{detail, detail_bytes} : utf8_literal("Open thread");
        if (meta.bytes > presentation->utf8.count - presentation->utf8_count ||
            presentation->style_count >= presentation->styles.count)
            return fail(presentation, MessageTextPresentationError::capacity);
        const u32 offset = presentation->utf8_count;
        copy_bytes(presentation->utf8.data + offset, meta.data, meta.bytes);
        presentation->utf8_count += meta.bytes;
        auto& bold = presentation->styles[presentation->style_count++];
        bold = {0, name.bytes, text_style_bold};
        const bool single = !detail_bytes && !rich->thread.name.bytes;
        if (!append_command(presentation, name, {&bold, 1}, kNoSemantics, owner,
                            {left, card.top + (single ? 16.0f : 8.0f), right, card.top + (single ? 34.0f : 26.0f)},
                            row_clip, theme.text_primary, record,
                            kInvalidMessageTextMeasurement, kLookMiddle) ||
            (!single &&
             !append_command(presentation, {presentation->utf8.data + offset, meta.bytes}, kNoStyles, kNoSemantics, owner,
                             {left, card.top + 27.0f, right, card.top + 43.0f}, row_clip, theme.text_secondary,
                             record, kInvalidMessageTextMeasurement,
                             kLookMiddleSecond)))
            return false;
    }
    if (rich->truncations != 0) {
        set_failure_context(presentation, MessageTextPresentationStage::notice,
                            MessageTextPresentationOperation::resolve_layout, 0);
        const UiWidgetHandle item_widget =
            message_scene_rich_item_widget(row, MessageSceneRichItemKind::notice, 0);
        const UiLayoutNode* item_layout =
            message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::notice, 0);
        UiTextFlowLayout flow{};
        if (!item_layout ||
            !initialize_flow(presentation, *item_layout, item_layout->content_rectangle.left,
                             ui_rect_width(item_layout->content_rectangle), row_clip, &flow) ||
            !ui_text_flow_advance(&flow, 4.0f) ||
            !append_rich_line(presentation, utf8_literal("Some content can't be shown"), &flow,
                              ui_background_muted_text_color(theme),
                              UiWidgetReference{&scene.widgets, item_widget}, *record,
                              measurement_index, MessageTextMeasurementKind::notice) ||
            !finish_flow(presentation, flow,
                         &presentation->measurements[measurement_index]
                              .rich_item_layout.heights[kMessageSceneNoticeItemOffset])) {
            return false;
        }
        presentation->measurements[measurement_index].rich_layout.notice =
            presentation->measurements[measurement_index]
                .rich_item_layout.heights[kMessageSceneNoticeItemOffset];
    }
    return append_annotations(presentation, scene, row, *record, theme, row_clip, measurement_index, forwarded,
                              rich->truncations != 0
                                  ? presentation->measurements[measurement_index]
                                        .rich_item_layout.heights[kMessageSceneNoticeItemOffset]
                                  : 0.0f,
                              collapsed ? nullptr : decoration);
}
} // namespace text_presentation_detail
} // namespace maicord
