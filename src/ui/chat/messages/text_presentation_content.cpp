#include "text_presentation_internal.hpp"
#include "maicord/ui/theme/style_projection.hpp"
#include "maicord/ui/theme/styles.hpp"
#include "maicord/ui/text/timestamp_format.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/ui/theme/background_presentation.hpp"

namespace maicord {
using namespace text_presentation_detail;

namespace text_presentation_detail {
u32 scoped_source_offset(u16 source_id, u32 offset) noexcept {
    return (static_cast<u32>(source_id) << 16u) | offset;
}

MessageRichString rich_source_string(const MessageRichRecord& rich, u16 source_id) noexcept {
    const u16 source = source_id & ~(kRichTextSource | kForwardedTextSource);
    if (source == kRichTextBody && rich.forwarded_snapshot_child)
        return rich.snapshot.content;
    if (source >= kRichTextEmbedBegin && source < kRichTextFieldBegin) {
        const u32 index = (source - kRichTextEmbedBegin) / kRichTextEmbedSlots;
        if (index >= rich.embed_count)
            return {};
        const auto& embed = rich.embeds[index];
        switch ((source - kRichTextEmbedBegin) % kRichTextEmbedSlots) {
        case 0:
            return embed.author_name;
        case 1:
            return embed.title;
        case 2:
            return embed.description;
        case 3:
            return embed.footer_text;
        case 4:
            return embed.url;
        case 5:
            return embed.author_url;
        case 6:
            return embed.provider_name;
        default:
            return embed.provider_url;
        }
    }
    if (source >= kRichTextFieldBegin && source < kRichTextComponentBegin) {
        const u32 index = (source - kRichTextFieldBegin) / 2u;
        if (index >= rich.embed_field_count)
            return {};
        return (source - kRichTextFieldBegin) % 2u ? rich.embed_fields[index].value
                                                   : rich.embed_fields[index].name;
    }
    if (source >= kRichTextComponentBegin) {
        const u32 index = (source - kRichTextComponentBegin) / 2u;
        if (index >= rich.component_count)
            return {};
        return (source - kRichTextComponentBegin) % 2u ? rich.components[index].content
                                                       : rich.components[index].label;
    }
    return {};
}

namespace {
u16 identify_rich_source(const BuildContext& context, const Utf8View& source) noexcept {
    if (!context.rich || !context.rich_store || source.bytes == 0)
        return 0;
    const u16 scope = kRichTextSource | (context.forwarded ? kForwardedTextSource : 0u);
    const u16 starts[] = {kRichTextEmbedBegin, kRichTextFieldBegin, kRichTextComponentBegin};
    const u16 counts[] = {static_cast<u16>(context.rich->embed_count * kRichTextEmbedSlots),
                          static_cast<u16>(context.rich->embed_field_count * 2u),
                          static_cast<u16>(context.rich->component_count * 2u)};
    for (u32 group = 0; group < 3; ++group) {
        for (u16 slot = 0; slot < counts[group]; ++slot) {
            const u16 id = starts[group] + slot;
            const Utf8View candidate =
                message_rich_store_string(*context.rich_store, rich_source_string(*context.rich, id));
            if (candidate.data == source.data && candidate.bytes == source.bytes)
                return scope | id;
        }
    }
    return 0;
}

bool spoiler_revealed(void* opaque, u32 source_offset) noexcept {
    auto* context = static_cast<BuildContext*>(opaque);
    if (!context || !context->presentation || !context->message) {
        return false;
    }
    context->trace.spoilers = true;
    const MessageTextPresentation& presentation = *context->presentation;
    if (presentation.reveal_spoilers)
        return true;
    for (u32 index = 0; index < presentation.spoiler_reveal_count; ++index) {
        const MessageTextSpoilerReveal& reveal = presentation.spoiler_reveals[index];
        if (reveal.message_id == context->message->id &&
            reveal.message_revision == context->message->revision &&
            reveal.source_offset == scoped_source_offset(context->source_id, source_offset)) {
            return true;
        }
    }
    return false;
}

// The compact timestamp: right-aligned in its column, never ellipsized (a
// wide time overruns into the row padding like the client's fixed-width box).
constexpr CommandLook kLookStampEnd{.themed = true,
                                    .horizontal = UiTextAlignment::end,
                                    .vertical = UiTextAlignment::center,
                                    .wrapping = TextWrapping::no_wrap};
} // namespace
// Compact body: "Name content" as one flow. The name prefix precedes the
// parsed text in the retained buffers so styles and semantics shift by it.
// A plugin name prefix/suffix wraps the name as muted subtext (in the role
// colour for MC_TONE_ROLE); the name stays the bold role-coloured span.
bool compact_content(MessageTextPresentation* presentation, BuildContext* context, Utf8View name,
                     Utf8View source, Utf8View* text, ConstSlice<TextStyleSpan>* styles,
                     ConstSlice<MessageMarkdownSemantic>* semantics,
                     const UiPluginRenderDecoration* decoration) noexcept {
    const Utf8View name_prefix = decoration ? decoration->name_prefix : Utf8View{};
    const Utf8View name_suffix = decoration ? decoration->name_suffix : Utf8View{};
    const u32 lead = name_prefix.bytes ? name_prefix.bytes + 1u : 0u;
    const u32 tail = name_suffix.bytes ? name_suffix.bytes + 1u : 0u;
    const u32 prefix = lead + name.bytes + tail + 1u;
    const u32 leading_spans = 1u + (lead ? 1u : 0u) + (tail ? 1u : 0u);
    if (presentation->utf8_count > presentation->utf8.count ||
        prefix > presentation->utf8.count - presentation->utf8_count ||
        leading_spans > presentation->styles.count - presentation->style_count) {
        return fail(presentation, MessageTextPresentationError::capacity);
    }
    const u32 text_begin = presentation->utf8_count;
    const u32 style_begin = presentation->style_count;
    const u32 decoration_flags =
        text_style_subtext | (decoration && decoration->name_tone == kUiPluginToneRole ? text_style_inline_role_color : 0u);
    char* out = presentation->utf8.data + text_begin;
    if (lead) {
        copy_bytes(out, name_prefix.data, name_prefix.bytes);
        out[name_prefix.bytes] = ' ';
        presentation->styles[presentation->style_count++] = TextStyleSpan{0, name_prefix.bytes, decoration_flags};
    }
    copy_bytes(out + lead, name.data, name.bytes);
    presentation->styles[presentation->style_count++] =
        TextStyleSpan{lead, name.bytes, text_style_bold | text_style_inline_role_color};
    if (tail) {
        out[lead + name.bytes] = ' ';
        copy_bytes(out + lead + name.bytes + 1u, name_suffix.data, name_suffix.bytes);
        presentation->styles[presentation->style_count++] =
            TextStyleSpan{lead + name.bytes + 1u, name_suffix.bytes, decoration_flags};
    }
    out[prefix - 1u] = ' ';
    presentation->utf8_count += prefix;
    const u32 semantic_begin = presentation->semantic_count;
    Utf8View parsed = source;
    ConstSlice<TextStyleSpan> parsed_styles{};
    ConstSlice<MessageMarkdownSemantic> parsed_semantics{};
    if (source.bytes && !parse_content(presentation, context, source, &parsed, &parsed_styles, &parsed_semantics))
        return false;
    if (parsed.bytes && parsed.data != presentation->utf8.data + text_begin + prefix) {
        // Markdown rejected the source: append it verbatim after the name.
        if (parsed.bytes > presentation->utf8.count - presentation->utf8_count)
            return fail(presentation, MessageTextPresentationError::capacity);
        copy_bytes(presentation->utf8.data + presentation->utf8_count, parsed.data, parsed.bytes);
        presentation->utf8_count += parsed.bytes;
    }
    for (u32 index = style_begin + leading_spans; index < presentation->style_count; ++index)
        presentation->styles[index].byte_offset += prefix;
    for (u32 index = semantic_begin; index < presentation->semantic_count; ++index)
        presentation->semantics[index].byte_offset += prefix;
    if (!parsed.bytes)
        --presentation->utf8_count; // no trailing separator after a bare name
    *text = Utf8View{presentation->utf8.data + text_begin, presentation->utf8_count - text_begin};
    *styles = ConstSlice<TextStyleSpan>{presentation->styles.data + style_begin,
                                        presentation->style_count - style_begin};
    *semantics = ConstSlice<MessageMarkdownSemantic>{presentation->semantics.data + semantic_begin,
                                                     presentation->semantic_count - semantic_begin};
    return true;
}

namespace {
// A compact timestamp shows on a group start (or a reply, which always starts
// one) and, on the other rows of a group, while the row is hovered or focused.
bool compact_timestamp_shown(const MessageScene& scene, const MessageSceneRowWidget& row) noexcept {
    return !row.grouped || message_scene_resolve(scene, scene.widgets.hovered) == &row ||
           message_scene_resolve(scene, scene.widgets.focused) == &row;
}

} // namespace
bool append_compact_timestamp(MessageTextPresentation* presentation, const MessageScene& scene,
                              const MessageSceneRowWidget& row, const UiRect& clip, const Theme& theme,
                              const MessageRecord& record, u64 now_unix_seconds) noexcept {
    const UiLayoutNode* gutter = message_scene_layout_node(scene, row.avatar);
    const UiLayoutNode* row_layout = message_scene_layout_node(scene, row.widget);
    if (!record.timestamp_ms || !gutter || !row_layout || !ui_rect_has_area(gutter->rectangle) ||
        !compact_timestamp_shown(scene, row))
        return true;
    char storage[kMessageMarkdownTimestampMaximumBytes];
    const auto formatted = format_timestamp_local(record.timestamp_ms / 1'000u, 't', now_unix_seconds,
                                                          Slice<char>{storage, sizeof(storage)});
    if (!formatted.ok || !formatted.bytes || formatted.bytes > presentation->utf8.count - presentation->utf8_count ||
        presentation->style_count >= presentation->styles.count)
        return true;
    const u32 text_begin = presentation->utf8_count;
    copy_bytes(presentation->utf8.data + text_begin, storage, formatted.bytes);
    presentation->utf8_count += formatted.bytes;
    TextStyleSpan* style = presentation->styles.data + presentation->style_count++;
    *style = TextStyleSpan{0, formatted.bytes, text_style_none, text_size_xs};
    // The column's right edge is the anchor; a longer time grows leftwards
    // into the row's left padding.
    UiRect column = gutter->rectangle;
    column.left = maximum(row_layout->rectangle.left, column.left - ui_theme_metrics(scene.theme_styles).message_left);
    return append_command(presentation, Utf8View{presentation->utf8.data + text_begin, formatted.bytes}, {style, 1},
                          kNoSemantics, UiWidgetReference{&scene.widgets, row.avatar}, column, clip,
                          ui_background_muted_text_color(theme), &record,
                          kInvalidMessageTextMeasurement, kLookStampEnd);
}

bool parse_content(MessageTextPresentation* presentation, BuildContext* context, const Utf8View& source,
                   Utf8View* text, ConstSlice<TextStyleSpan>* styles,
                   ConstSlice<MessageMarkdownSemantic>* semantics, u32 base_style, u8 base_size) noexcept {
    if (!presentation || !context || !text || !styles || !semantics) {
        return fail(presentation, MessageTextPresentationError::model);
    }
    if (presentation->utf8_count > presentation->utf8.count ||
        presentation->style_count > presentation->styles.count ||
        presentation->semantic_count > presentation->semantics.count ||
        source.bytes > presentation->measurement_history_offset) {
        return fail(presentation, MessageTextPresentationError::capacity);
    }
    const u32 text_begin = presentation->utf8_count;
    const u32 style_begin = presentation->style_count;
    const u32 semantic_begin = presentation->semantic_count;
    MessageMarkdownOptions options{context,
                                   resolve_entity,
                                   format_timestamp,
                                   spoiler_revealed,
                                   presentation->hovered_source_offset & 0xFFFFu,
                                   presentation->hovered_kind,
                                   presentation->has_hover &&
                                       presentation->hovered_message_id == context->message->id &&
                                       (presentation->hovered_source_offset >> 16u) == context->source_id};
    options.base_style = base_style;
    options.no_links = context->markdown_rules == 1;
    options.no_headings = context->markdown_rules != 0;
    options.no_lists = context->markdown_rules == 1;
    options.text_sizes = true;
    options.base_size = base_size;
    options.resolve_channel = resolve_channel;
    const MessageMarkdownResult parsed = cached_markdown_parse(
        presentation, context, MarkdownCacheMode::content, source,
        Slice<char>{presentation->utf8.data + text_begin, presentation->utf8.count - text_begin},
        Slice<u32>{presentation->byte_style_scratch.data, presentation->measurement_history_offset},
        Slice<TextStyleSpan>{presentation->styles.data + style_begin,
                             presentation->styles.count - style_begin},
        options,
        Slice<MessageMarkdownSemantic>{
            presentation->semantics.data + semantic_begin,
            minimum(presentation->semantics.count - semantic_begin, kMessageMarkdownSemanticMaximum)});
    if (!parsed.ok()) {
        saturating_accumulate(presentation->markdown_rejected);
        return true;
    }
    presentation->utf8_count += parsed.text.bytes;
    presentation->style_count += parsed.span_count;
    presentation->semantic_count += parsed.semantic_count;
    for (u32 index = 0; index < parsed.semantic_count; ++index) {
        auto& semantic = presentation->semantics[semantic_begin + index];
        semantic.source_offset = scoped_source_offset(context->source_id, semantic.source_offset);
    }
    *text = parsed.text;
    *styles = ConstSlice<TextStyleSpan>{presentation->styles.data + style_begin, parsed.span_count};
    *semantics = ConstSlice<MessageMarkdownSemantic>{presentation->semantics.data + semantic_begin,
                                                     parsed.semantic_count};
    saturating_accumulate(presentation->markdown_transformations, parsed.transformations);
    saturating_accumulate(presentation->markdown_spans_emitted, parsed.span_count);
    saturating_accumulate(presentation->markdown_syntax_tokens, parsed.syntax_tokens);
    saturating_accumulate(presentation->markdown_syntax_styled_bytes, parsed.syntax_styled_bytes);
    saturating_accumulate(presentation->markdown_list_items, parsed.list_items);
    saturating_accumulate(presentation->markdown_resolved_entities, parsed.resolved_entities);
    saturating_accumulate(presentation->markdown_timestamps, parsed.timestamps);
    if (parsed.syntax_truncated) {
        saturating_accumulate(presentation->markdown_syntax_truncated);
    }
    if (parsed.next_timestamp_update_unix_seconds != 0 &&
        (presentation->next_timestamp_update_unix_seconds == 0 ||
         parsed.next_timestamp_update_unix_seconds < presentation->next_timestamp_update_unix_seconds)) {
        presentation->next_timestamp_update_unix_seconds = parsed.next_timestamp_update_unix_seconds;
    }
    return true;
}

namespace {
constexpr float kQuoteEndPadding = 8.0f;
constexpr float kQuoteMargin = 4.0f;
} // namespace
// Text width of a quote inside a markup box `width` wide; the text starts
// quote_text_inset() after the box.
float quote_text_width(float width) noexcept {
    const float block = minimum(width - kQuoteSpine, width * 0.9f);
    return maximum(1.0f, block - kQuoteStartPadding - kQuoteEndPadding);
}
// Splits presentation-owned parsed text into quote, code-block and plain runs,
// rebasing its spans and semantics in place; code runs drop their trailing
// newlines and their spans are marked framed. Returns 0 (nothing changed) when
// the text has no quote or code block, too many runs, or a span/semantic that
// crosses a boundary.
u32 split_quote_segments(const Utf8View& text, const ConstSlice<TextStyleSpan>& styles,
                         const ConstSlice<MessageMarkdownSemantic>& semantics, QuoteSegment* output) noexcept {
    // Ordinary styled text has no block boundaries to split.
    u32 block = 0;
    while (block < styles.count && !(styles[block].flags & (text_style_quote | text_style_code_block)))
        ++block;
    if (block == styles.count)
        return 0;
    struct Range {
        u32 begin, end;
        bool quote, code;
    } ranges[kQuoteSegmentMaximum];
    u32 count = 0;
    bool blocks = false;
    u32 style = 0, code_style = 0;
    const auto flags_at = [&](u32 at) noexcept {
        while (style < styles.count && styles[style].byte_offset + styles[style].byte_count <= at)
            ++style;
        return style < styles.count && styles[style].byte_offset <= at ? styles[style].flags : 0u;
    };
    const auto add = [&](u32 begin, u32 end, bool quote, bool code) noexcept {
        // Consecutive plain or quoted lines (one newline apart) stay one run.
        if (count && !code && !ranges[count - 1].code && ranges[count - 1].quote == quote &&
            ranges[count - 1].end + 1 == begin) {
            ranges[count - 1].end = end;
            return true;
        }
        if (count == kQuoteSegmentMaximum)
            return false;
        ranges[count++] = {begin, end, quote, code};
        return true;
    };
    for (u32 at = 0; at < text.bytes;) {
        const u32 flags = flags_at(at);
        const bool quote = (flags & text_style_quote) != 0;
        if (flags & text_style_code_block) {
            u32 end = at;
            // Block membership stays constant until the current style span ends.
            while (end < text.bytes && (flags_at(end) & text_style_code_block))
                end = minimum(text.bytes, styles[style].byte_offset + styles[style].byte_count);
            u32 last = end;
            while (last > at && text.data[last - 1] == '\n')
                --last;
            if (last > at && !add(at, last, quote, true))
                return 0;
            blocks = true;
            at = end < text.bytes && text.data[end] == '\n' ? end + 1 : end;
            continue;
        }
        code_style = maximum(code_style, style);
        while (code_style < styles.count &&
               (!(styles[code_style].flags & text_style_code_block) ||
                styles[code_style].byte_offset + styles[code_style].byte_count <= at))
            ++code_style;
        const u32 limit = code_style < styles.count ? minimum(text.bytes, styles[code_style].byte_offset)
                                                    : text.bytes;
        u32 end = at;
        while (end < limit && text.data[end] != '\n')
            ++end;
        blocks = blocks || quote;
        if (!add(at, end, quote, false))
            return 0;
        at = end < text.bytes && text.data[end] == '\n' ? end + 1 : end;
    }
    if (!blocks || count == 0)
        return 0;
    // A span or semantic may reach into the newlines after its run, no further.
    const auto next_begin = [&](u32 k) noexcept { return k + 1 < count ? ranges[k + 1].begin : text.bytes; };
    const auto segment_of = [&](u32 offset, u32 bytes, u32 from) noexcept {
        while (from < count && offset >= next_begin(from))
            ++from;
        return from < count && offset + bytes <= next_begin(from) ? from : count;
    };
    for (u32 i = 0, k = 0; i < styles.count; ++i)
        if ((k = segment_of(styles[i].byte_offset, styles[i].byte_count, k)) == count)
            return 0;
    for (u32 i = 0, k = 0; i < semantics.count; ++i)
        if ((k = segment_of(semantics[i].byte_offset, semantics[i].byte_count, k)) == count)
            return 0;
    auto* spans = const_cast<TextStyleSpan*>(styles.data);
    auto* marks = const_cast<MessageMarkdownSemantic*>(semantics.data);
    u32 read_style = 0, write_style = 0, read_mark = 0, write_mark = 0;
    for (u32 k = 0; k < count; ++k) {
        const Range range = ranges[k];
        const u32 limit = next_begin(k);
        const u32 first_style = write_style, first_mark = write_mark;
        for (; read_style < styles.count && spans[read_style].byte_offset < limit; ++read_style) {
            TextStyleSpan span = spans[read_style];
            const u32 begin = maximum(span.byte_offset, range.begin);
            const u32 end = minimum(span.byte_offset + span.byte_count, range.end);
            if (end <= begin)
                continue;
            span.byte_offset = begin - range.begin;
            span.byte_count = end - begin;
            if (range.code)
                span.reserved[2] |= kTextSpanFramedBlock;
            spans[write_style++] = span;
        }
        for (; read_mark < semantics.count && marks[read_mark].byte_offset < limit; ++read_mark) {
            MessageMarkdownSemantic mark = marks[read_mark];
            const u32 begin = maximum(mark.byte_offset, range.begin);
            const u32 end = minimum(mark.byte_offset + mark.byte_count, range.end);
            if (end <= begin)
                continue;
            mark.byte_offset = begin - range.begin;
            mark.byte_count = end - begin;
            marks[write_mark++] = mark;
        }
        output[k] = QuoteSegment{{text.data + range.begin, range.end - range.begin},
                                 {spans + first_style, write_style - first_style},
                                 {marks + first_mark, write_mark - first_mark},
                                 range.quote,
                                 range.code};
    }
    return count;
}

// The space between non-empty segment `index` and the next one's text: the
// code box insets on either side, plus the code margin before a code block or
// the quote margin next to a quote; `last` (after the box) for the final one.
float quote_gap_after(const QuoteSegment* segments, u32 count, u32 index, float last) noexcept {
    const float after = segments[index].code ? kMessageCodeBlockInset : 0.0f;
    for (u32 next = index + 1; next < count; ++next) {
        if (!segments[next].text.bytes)
            continue;
        const float margin = segments[next].code                             ? kMessageCodeBlockMargin
                             : segments[index].quote || segments[next].quote ? kQuoteMargin
                                                                             : 0.0f;
        return after + margin + (segments[next].code ? kMessageCodeBlockInset : 0.0f);
    }
    return after + last;
}

bool append_rich_markdown_line(BuildContext* context, const Utf8View& source, UiTextFlowLayout* flow,
                               float left_inset, float right_inset, const Color& color,
                               const UiWidgetReference& owner, u16 measurement_index,
                               MessageTextMeasurementKind measurement_kind, float gap_after,
                               u32 base_style, u8 base_size) noexcept {
    MessageTextPresentation* const presentation = context ? context->presentation : nullptr;
    if (!presentation || !context->message || !flow || (!source.data && source.bytes != 0)) {
        return fail(presentation, MessageTextPresentationError::model);
    }
    if (source.bytes == 0) {
        return true;
    }
    Utf8View display = source;
    ConstSlice<TextStyleSpan> styles{};
    ConstSlice<MessageMarkdownSemantic> semantics{};
    context->source_id = identify_rich_source(*context, source);
    if (!parse_content(presentation, context, source, &display, &styles, &semantics, base_style, base_size)) {
        return false;
    }
    QuoteSegment segments[kQuoteSegmentMaximum];
    u32 segment_count = split_quote_segments(display, styles, semantics, segments);
    if (segment_count == 0) {
        segments[0] = QuoteSegment{display, styles, semantics, false, false};
        segment_count = 1;
    }
    bool first = true;
    for (u32 index = 0; index < segment_count; ++index) {
        const QuoteSegment& segment = segments[index];
        if (segment.text.bytes == 0)
            continue;
        float left = left_inset, right = right_inset;
        const float width = maximum(1.0f, ui_rect_width(flow->content_rectangle) - left_inset - right_inset);
        if (segment.quote) {
            left += quote_text_inset();
            right += maximum(0.0f, width - quote_text_inset() - quote_text_width(width));
        }
        if (segment.code) {
            // Embeds give a code block the full width, message markup 90%.
            const float markup = segment.quote ? quote_text_width(width) : width;
            left += kMessageCodeBlockInset;
            right += kMessageCodeBlockInset +
                     (measurement_kind == MessageTextMeasurementKind::embed ? 0.0f : markup * 0.1f);
            if (first && !ui_text_flow_advance(flow, kMessageCodeBlockInset)) {
                presentation->failure_flow_error = flow->last_error;
                return fail(presentation, MessageTextPresentationError::layout);
            }
        }
        first = false;
        UiRect rectangle{};
        if (!begin_item(presentation, flow, left, right, &rectangle)) {
            return false;
        }
        if (!append_command(presentation, segment.text, segment.styles, segment.semantics, owner, rectangle,
                            flow->clip_rectangle, color, context->message, measurement_index,
                            {.kind = measurement_kind,
                             .minimum_height = 16.0f,
                             .gap_after = quote_gap_after(segments, segment_count, index, gap_after),
                             .themed = segment.styles.count != 0,
                             .source_id = context->source_id})) {
            return false;
        }
        if (!commit_last_command(presentation, flow)) {
            return false;
        }
    }
    return true;
}

namespace {
// A command's measured width, plus the box of a framed code block.
float framed_width(const MessageTextCommand& command) noexcept {
    return command.measured_width + (message_text_command_code_block(command) ? 2.0f * kMessageCodeBlockInset : 0.0f);
}

} // namespace
// The widest framed command from `first` on, each plus `inset`, or `widest`.
float widest_framed(const MessageTextPresentation* presentation, u32 first, float inset, float widest) noexcept {
    for (u32 c = first; c < presentation->command_count; ++c)
        widest = maximum(widest, framed_width(presentation->commands[c]) + inset);
    return widest;
}

// One embed text block that is not markdown (author, footer, provider, and
// every part of a non-rich embed): the whole line takes `flags`; a non-empty
// `link` makes it one link semantic addressing that rich source slot.
bool append_embed_plain_line(BuildContext* context, const Utf8View& text, UiTextFlowLayout* flow,
                             float left_inset, float right_inset, const Color& color,
                             const UiWidgetReference& owner, u16 measurement_index, u32 flags,
                             const Utf8View& link, u16 link_source, float minimum_height,
                             u16 source_id, u8 size) noexcept {
    MessageTextPresentation* const presentation = context ? context->presentation : nullptr;
    if (!presentation || !context->message || !flow || (!text.data && text.bytes != 0))
        return fail(presentation, MessageTextPresentationError::model);
    if (text.bytes == 0)
        return true;
    const u64 message_id = context->message->id;
    const bool linked = link.bytes != 0 && link_source != 0 && link.bytes <= 0xFFFFu;
    const u32 scoped = linked ? scoped_source_offset(link_source, 0) : 0;
    if (linked && presentation->has_hover && presentation->hovered_message_id == message_id &&
        presentation->hovered_kind == MessageMarkdownSemanticKind::link && presentation->hovered_source_offset == scoped)
        flags |= text_style_underline | text_style_interactive_hover;
    ConstSlice<TextStyleSpan> styles{};
    ConstSlice<MessageMarkdownSemantic> semantics{};
    if (flags || size) {
        if (presentation->style_count >= presentation->styles.count)
            return fail(presentation, MessageTextPresentationError::capacity);
        presentation->styles[presentation->style_count] = TextStyleSpan{0, text.bytes, flags, size};
        styles = {presentation->styles.data + presentation->style_count++, 1};
    }
    if (linked && presentation->semantic_count < presentation->semantics.count) {
        presentation->semantics[presentation->semantic_count] = MessageMarkdownSemantic{
            0, 0, text.bytes, scoped, static_cast<u16>(link.bytes), MessageMarkdownSemanticKind::link, 0};
        semantics = {presentation->semantics.data + presentation->semantic_count++, 1};
    }
    UiRect rectangle{};
    if (!begin_item(presentation, flow, left_inset, right_inset, &rectangle)) {
        return false;
    }
    if (!append_command(presentation, text, styles, semantics, owner, rectangle, flow->clip_rectangle, color,
                        context->message, measurement_index,
                        {.kind = MessageTextMeasurementKind::embed,
                         .minimum_height = minimum_height,
                         .themed = styles.count != 0,
                         .source_id = source_id}))
        return false;
    // A line shorter than its row (the author or footer icon) is centred on it.
    MessageTextCommand& command = presentation->commands[presentation->command_count - 1];
    if (command.measured_height > 0 && command.measured_height < minimum_height)
        command.rectangle.top += (minimum_height - command.measured_height) * 0.5f;
    return commit_last_command(presentation, flow);
}

// A rich embed markdown block (title, description, field); non-rich embeds
// show the raw string. A title with embed.url becomes one link to it.
bool append_embed_block(BuildContext* context, const Utf8View& source, UiTextFlowLayout* flow, float left_inset,
                        float right_inset, const Color& color, const UiWidgetReference& owner,
                        u16 measurement_index, bool markdown, u8 rules, u32 base_style, u8 base_size,
                        const Utf8View& link, u16 link_source) noexcept {
    if (source.bytes == 0)
        return true;
    if (!markdown) {
        return append_embed_plain_line(context, source, flow, left_inset, right_inset, color, owner,
                                       measurement_index, base_style, link, link_source, kMessageEmbedLineMinimum,
                                       identify_rich_source(*context, source), base_size);
    }
    MessageTextPresentation* const presentation = context->presentation;
    const u64 message_id = context->message->id;
    const bool linked = link.bytes != 0 && link_source != 0 && link.bytes <= 0xFFFFu;
    const u32 scoped = linked ? scoped_source_offset(link_source, 0) : 0;
    if (linked && presentation->has_hover && presentation->hovered_message_id == message_id &&
        presentation->hovered_kind == MessageMarkdownSemanticKind::link && presentation->hovered_source_offset == scoped)
        base_style |= text_style_underline | text_style_interactive_hover;
    context->markdown_rules = rules;
    const u32 command_before = presentation->command_count;
    const bool appended = append_rich_markdown_line(context, source, flow, left_inset, right_inset, color, owner,
                                                    measurement_index, MessageTextMeasurementKind::embed, 0.0f,
                                                    base_style, base_size);
    context->markdown_rules = 0;
    if (!appended)
        return false;
    if (linked && presentation->command_count == command_before + 1 &&
        presentation->semantic_count < presentation->semantics.count) {
        MessageTextCommand& command = presentation->commands[presentation->command_count - 1];
        // The parsed semantics are the newest entries; extend that run by one.
        const bool contiguous = command.semantics.count == 0 ||
                                command.semantics.data + command.semantics.count ==
                                    presentation->semantics.data + presentation->semantic_count;
        if (contiguous) {
            MessageMarkdownSemantic* first = command.semantics.count
                                                 ? const_cast<MessageMarkdownSemantic*>(command.semantics.data)
                                                 : presentation->semantics.data + presentation->semantic_count;
            presentation->semantics[presentation->semantic_count++] = MessageMarkdownSemantic{
                0, 0, command.text.bytes, scoped, static_cast<u16>(link.bytes), MessageMarkdownSemanticKind::link, 0};
            command.semantics = {first, command.semantics.count + 1};
        }
    }
    return true;
}

bool append_embed_fields(BuildContext* context, const MessageRichStore& store, const MessageRichRecord& rich,
                         const MessageRichEmbed& embed, UiTextFlowLayout* flow, const Color& name_color,
                         const Color& value_color, const UiWidgetReference& owner, u16 measurement_index,
                         float* natural_width, bool markdown) noexcept {
    MessageTextPresentation* const presentation = context ? context->presentation : nullptr;
    if (!presentation || !flow) {
        return false;
    }
    const u32 field_end =
        minimum(static_cast<u32>(rich.embed_field_count),
                static_cast<u32>(embed.field_begin) + static_cast<u32>(embed.field_count));
    // Embed fields: margin-top 8 and an 8-DIP grid gap; field names keep a
    // 2-DIP bottom margin.
    constexpr float column_gap = 8.0f;
    constexpr float minimum_column_width = 80.0f;
    const float outer_width = ui_rect_width(flow->content_rectangle);
    const float inset = minimum(kMessageEmbedContentInset, maximum(0.0f, (outer_width - 1.0f) * 0.5f));
    const float end_inset = minimum(kMessageEmbedEndInset, maximum(0.0f, outer_width - 1.0f - inset));
    const float inner_left = flow->content_rectangle.left + inset;
    const float inner_width = maximum(1.0f, outer_width - inset - end_inset);
    for (u32 field = embed.field_begin; field < field_end;) {
        if (!ui_text_flow_advance(flow, kMessageEmbedBlockGap)) {
            presentation->failure_flow_error = flow->last_error;
            return fail(presentation, MessageTextPresentationError::layout);
        }
        u32 columns = 1;
        if (rich.embed_fields[field].inline_field) {
            u32 inline_count = 0;
            while (field + inline_count < field_end && inline_count < 3 &&
                   rich.embed_fields[field + inline_count].inline_field) {
                ++inline_count;
            }
            const u32 fitting_columns = maximum<u32>(
                1u, static_cast<u32>((inner_width + column_gap) / (minimum_column_width + column_gap)));
            columns = minimum(inline_count, fitting_columns);
        }
        const float column_width =
            maximum(1.0f, (inner_width - column_gap * static_cast<float>(columns - 1)) /
                              static_cast<float>(columns));
        float row_height = 0.0f;
        float row_width = kMessageEmbedContentInset + kMessageEmbedEndInset + column_gap * (columns - 1);
        for (u32 column = 0; column < columns; ++column) {
            const u32 first_command = presentation->command_count;
            const float left = inner_left + static_cast<float>(column) * (column_width + column_gap);
            // The retained item initially owns only its estimated height. A
            // field can start below that estimate while the text pass is
            // discovering the real height, so do not bound shaping by it.
            const float content_bottom =
                minimum(kMaximumUiCoordinate, flow->cursor_y + kUiTextFlowItemLayoutHeight);
            const UiRect content{left, flow->cursor_y, left + column_width, content_bottom};
            const UiRect clip = ui_rect_intersection(
                flow->clip_rectangle,
                UiRect{left, flow->clip_rectangle.top, left + column_width, flow->clip_rectangle.bottom});
            UiTextFlowLayout column_flow{};
            if (!ui_rect_has_area(content) || !ui_rect_has_area(clip) ||
                !ui_text_flow_initialize(&column_flow, content, clip)) {
                return fail(presentation, MessageTextPresentationError::layout);
            }
            const MessageRichEmbedField& value = rich.embed_fields[field + column];
            if (!append_embed_block(context, message_rich_store_string(store, value.name), &column_flow, 0.0f, 0.0f,
                                    name_color, owner, measurement_index, markdown, 1, text_style_bold,
                                    text_size_sm) ||
                !ui_text_flow_advance(&column_flow, 2.0f) ||
                !append_embed_block(context, message_rich_store_string(store, value.value), &column_flow, 0.0f, 0.0f,
                                    value_color, owner, measurement_index, markdown, 2, 0, text_size_sm)) {
                return false;
            }
            row_height = maximum(row_height, ui_text_flow_required_height(column_flow));
            const float measured_width = widest_framed(presentation, first_command, 0.0f, 0.0f);
            row_width += columns > 1 ? maximum(minimum_column_width, measured_width) : measured_width;
        }
        *natural_width = maximum(*natural_width, row_width);
        if (!ui_text_flow_advance(flow, row_height)) {
            presentation->failure_flow_error = flow->last_error;
            return fail(presentation, MessageTextPresentationError::layout);
        }
        field += columns;
    }
    return true;
}

Utf8View poll_result_field(const MessageRichStore& store, const MessageRichRecord& rich, const Utf8View& name) noexcept {
    for (u32 e = 0; e < rich.embed_count; ++e) {
        const auto& embed = rich.embeds[e];
        if (embed.type != GatewayMessageEmbedType::poll_result)
            continue;
        const u32 end = static_cast<u32>(embed.field_begin) + embed.field_count;
        for (u32 f = embed.field_begin; f < end && f < rich.embed_field_count; ++f) {
            const Utf8View key = message_rich_store_string(store, rich.embed_fields[f].name);
            if (key.bytes == name.bytes && bytes_equal(key.data, name.data, name.bytes))
                return message_rich_store_string(store, rich.embed_fields[f].value);
        }
    }
    return {};
}

namespace {
u32 parse_u32(const Utf8View& text) noexcept {
    u32 value = 0;
    for (u32 i = 0; i < text.bytes && i < 9; ++i) {
        if (text.data[i] < '0' || text.data[i] > '9')
            break;
        value = value * 10u + static_cast<u32>(text.data[i] - '0');
    }
    return value;
}

// Generated card text, written into the presentation's free UTF-8 bank.
TextWriter retained_writer(const MessageTextPresentation& presentation) noexcept {
    return {{presentation.utf8.data + presentation.utf8_count, presentation.utf8.count - presentation.utf8_count}};
}
// Keeps what the writer wrote; empty when it did not fit.
Utf8View retain(MessageTextPresentation* presentation, const TextWriter& writer) noexcept {
    if (!writer.ok)
        return {};
    presentation->utf8_count += writer.used;
    return {writer.output.data, writer.used};
}

} // namespace
// The poll result card: the winning answer with its share, a tie or no winner.
// The system line above links the poll itself.
bool append_poll_result(BuildContext* context, const MessageRichStore& store, const MessageRichRecord& rich,
                        UiTextFlowLayout* flow, const Color& text, const Color& muted,
                        const UiWidgetReference& owner, u16 measurement_index) noexcept {
    MessageTextPresentation* const presentation = context->presentation;
    const u32 total = parse_u32(poll_result_field(store, rich, utf8_literal("total_votes")));
    const u32 votes = parse_u32(poll_result_field(store, rich, utf8_literal("victor_answer_votes")));
    const bool victor = poll_result_field(store, rich, utf8_literal("victor_answer_id")).bytes != 0;
    const u32 percent =
        total ? static_cast<u32>(minimum<u64>((u64{votes} * 200u + total) / (2u * u64{total}), 100u)) : 0u;
    char number[16];
    const Utf8View share{number, decimal_write(number, percent)};
    TextWriter head = retained_writer(*presentation);
    if (!total)
        head.add("There was no winner");
    else if (victor) {
        const Utf8View emoji = poll_result_field(store, rich, utf8_literal("victor_answer_emoji_name"));
        const Utf8View answer = poll_result_field(store, rich, utf8_literal("victor_answer_text"));
        if (emoji.bytes && !poll_result_field(store, rich, utf8_literal("victor_answer_emoji_id")).bytes) {
            head.add(emoji);
            if (answer.bytes)
                head.add(" ");
        }
        head.add(answer.bytes || emoji.bytes ? answer : utf8_literal("Answer"));
    } else
        head.add("The results were tied");
    const Utf8View head_text = retain(presentation, head);
    TextWriter detail = retained_writer(*presentation);
    if (total) {
        if (victor)
            detail.add("Winning answer \xE2\x80\xA2 ");
        detail.add(share);
        detail.add("%");
    }
    const Utf8View detail_text = retain(presentation, detail);
    if (!head.ok || !detail.ok)
        return fail(presentation, MessageTextPresentationError::capacity);
    return append_embed_plain_line(context, head_text, flow, kMessageEmbedContentInset, kMessageEmbedEndInset, text,
                                   owner, measurement_index, text_style_bold, kNoLink, 0, 16.0f, 0, text_size_sm) &&
           append_embed_plain_line(context, detail_text, flow, kMessageEmbedContentInset, kMessageEmbedEndInset, muted,
                                   owner, measurement_index, text_style_none, kNoLink, 0, 16.0f, 0, text_size_xs);
}
} // namespace text_presentation_detail
} // namespace maicord
