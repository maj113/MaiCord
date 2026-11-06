#include "maicord/ui/widgets/kit_markdown.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/chat/messages/markdown.hpp"

namespace maicord {
namespace {
// Heading flags the parser leaves on a `###` run, and the flags it puts on a list item's line.
constexpr u32 kEyebrowFlags = text_style_heading_3 | text_style_bold;
constexpr u32 kListFlags = text_style_list | text_style_list_bullet | kTextStyleListDepthMask;
// A list item's text sits kMarkerLeft in plus kLevelStep per level; its marker ends kMarkerRight before the text.
// Items are kItemGap apart.
constexpr float kItemGap = 2, kMarkerLeft = 22, kMarkerRight = 6, kLevelStep = 18;
// Body line height of a size code.
float line_height(u8 body_size) noexcept {
    return body_size == text_size_sm ? 20.f : 22.f;
}
float body_em(u8 size) noexcept {
    return size == text_size_sm ? 14.f : 16.f;
}
float item_indent(u8 level) noexcept {
    return kMarkerLeft + kLevelStep * level;
}
void ascii_upper(char* text, u32 bytes) noexcept {
    for (u32 i = 0; i < bytes; ++i)
        if (text[i] >= 'a' && text[i] <= 'z')
            text[i] = static_cast<char>(text[i] - 32);
}
// Wrapped height of a part without a measure: per paragraph, from the style of its first byte.
float estimate_height(u8 body_size, const Utf8View& text, ConstSlice<TextStyleSpan> spans, float width) noexcept {
    const float em = body_em(body_size), line = line_height(body_size);
    float height = 0;
    u32 start = 0, span = 0;
    for (u32 i = 0; i <= text.bytes; ++i) {
        if (i != text.bytes && text.data[i] != '\n')
            continue;
        while (span + 1 < spans.count && spans[span].byte_offset + spans[span].byte_count <= start)
            ++span;
        const float size = text_style_font_size(em, (spans.count ? spans[span].flags : 0u) & ~text_style_subtext);
        const u32 lines = i == start ? 1u
                                     : maximum(1u, ui_settings_text_lines({text.data + start, i - start}, size * 1.04f, width));
        height += static_cast<float>(lines) * maximum(line, size * 1.4f);
        start = i + 1;
    }
    return height;
}

// Cuts the parsed text into parts. The parser keeps a bullet item's marker as spaces and a number item's "N. ";
// the part's own text starts after it and its list flags are dropped, so the text layer draws no bullet of its own.
struct Carving {
    UiMarkdown* block;
    const TextStyleSpan* source;
    u32 source_count, cursor;
};
bool add_part(Carving& c, u32 offset, u32 bytes, UiMarkdownMarker marker, u32 level, u32 marker_offset,
              u32 marker_bytes) noexcept {
    UiMarkdown& block = *c.block;
    const u32 capacity = block.spans.count / 2;
    if (block.part_count >= block.parts.count)
        return false;
    UiMarkdownPart part{offset, bytes, static_cast<u16>(block.span_count), 0, marker_offset, static_cast<u8>(marker_bytes),
                        static_cast<u8>(level), marker, 0, 0};
    const u32 end = offset + bytes;
    while (c.cursor < c.source_count && c.source[c.cursor].byte_offset + c.source[c.cursor].byte_count <= offset)
        ++c.cursor;
    for (u32 i = c.cursor; i < c.source_count && c.source[i].byte_offset < end; ++i) {
        const TextStyleSpan& from = c.source[i];
        const u32 low = maximum(from.byte_offset, offset), high = minimum(from.byte_offset + from.byte_count, end);
        if (high <= low)
            continue;
        if (block.span_count >= capacity)
            return false;
        TextStyleSpan& to = block.spans[block.span_count++];
        to = from;
        to.byte_offset = low - offset;
        to.byte_count = high - low;
        if (marker != UiMarkdownMarker::none)
            to.flags &= ~kListFlags;
    }
    part.span_count = static_cast<u16>(block.span_count - part.span_first);
    block.parts[block.part_count++] = part;
    return true;
}
bool spaces(const char* text, u32 count) noexcept {
    for (u32 i = 0; i < count; ++i)
        if (text[i] != ' ')
            return false;
    return true;
}
bool carve(UiMarkdown* block, const TextStyleSpan* source, u32 source_count) noexcept {
    const char* text = block->text.data;
    const u32 bytes = block->text_bytes;
    Carving c{block, source, source_count, 0};
    // `run` is the first byte of the plain lines collected since the last item.
    u32 run = 0, span = 0;
    bool ok = true;
    for (u32 line = 0; ok && line <= bytes;) {
        u32 end = line;
        while (end < bytes && text[end] != '\n')
            ++end;
        UiMarkdownMarker marker = UiMarkdownMarker::none;
        u32 content = 0, marker_offset = 0, marker_bytes = 0, level = 0;
        if (end > line) {
            while (span + 1 < source_count && source[span].byte_offset + source[span].byte_count <= line)
                ++span;
            const u32 flags = span < source_count ? source[span].flags : 0u;
            if (flags & text_style_list) {
                level = (flags & kTextStyleListDepthMask) >> kTextStyleListDepthShift;
                const u32 indent = level * 2;
                if (flags & text_style_list_bullet) {
                    if (end - line >= indent + 2 && spaces(text + line, indent + 2)) {
                        marker = level ? UiMarkdownMarker::dash : UiMarkdownMarker::bullet;
                        content = line + indent + 2;
                    }
                } else if (end - line >= indent + 3 && spaces(text + line, indent)) {
                    u32 i = line + indent;
                    while (i < end && text[i] >= '0' && text[i] <= '9')
                        ++i;
                    if (i > line + indent && i + 1 < end && text[i] == '.' && text[i + 1] == ' ') {
                        marker = UiMarkdownMarker::number;
                        marker_offset = line + indent;
                        marker_bytes = i + 1 - marker_offset;
                        content = i + 2;
                    }
                }
            }
        }
        if (marker != UiMarkdownMarker::none) {
            // The run before the item, the item, and one part left for the rest of the text.
            if (block->part_count + 3 > block->parts.count)
                break;
            ok = (run >= line || add_part(c, run, line - 1 - run, UiMarkdownMarker::none, 0, 0, 0)) &&
                 add_part(c, content, end - content, marker, level, marker_offset, marker_bytes);
            run = end + 1;
        }
        line = end + 1;
    }
    if (ok && run < bytes)
        ok = add_part(c, run, bytes - run, UiMarkdownMarker::none, 0, 0, 0);
    if (!ok) {
        block->part_count = block->span_count = c.cursor = 0;
        ok = add_part(c, 0, bytes, UiMarkdownMarker::none, 0, 0, 0);
    }
    return ok;
}
} // namespace

bool ui_markdown_parse(UiMarkdown* block, const Utf8View& source, u8 body_size, Slice<u32> scratch) noexcept {
    if (!block)
        return false;
    // The parser writes the upper half of the span storage; the parts' own spans fill the lower half.
    const u32 half = block->spans.count / 2;
    if (source.bytes > kUiMarkdownSourceMaximum || source.bytes > block->text.count || source.bytes > scratch.count ||
        half < 1 || block->parts.count < 1) {
        block->parsed = false;
        return false;
    }
    u64 signature = hash64_bytes(kHash64Seed, source.data, source.bytes);
    signature = hash64_mix(signature, body_size);
    if (block->parsed && block->source_signature == signature)
        return true;
    block->parsed = false;
    MessageMarkdownOptions parser{};
    parser.text_sizes = true;
    parser.base_size = body_size;
    Slice<TextStyleSpan> parsed{block->spans.data + half, half};
    const auto result = message_markdown_parse(source, block->text, scratch, parsed, parser, {});
    if (!result.ok())
        return false;
    block->text_bytes = result.text.bytes;
    for (u32 i = 0; i < result.span_count; ++i) {
        auto& span = parsed[i];
        u32 flags = span.flags;
        u8 size = span.size;
        // A `###` run: a 12 pt bold uppercase eyebrow without the heading ratio.
        if ((flags & kEyebrowFlags) == kEyebrowFlags && !(flags & (text_style_heading_1 | text_style_heading_2))) {
            ascii_upper(block->text.data + span.byte_offset, span.byte_count);
            flags &= ~text_style_heading_3;
            size = text_size_xs;
        }
        span.flags = text_style_fold_size(flags, size);
        span.size = 0;
    }
    block->body_size = body_size;
    block->source_signature = signature;
    block->measured_width = -1;
    block->height = 0;
    block->span_count = 0;
    block->part_count = 0;
    // The carving cannot fail on a block that holds at least one part and as many spans as it parsed; a list
    // longer than the part storage keeps its remaining lines in one trailing run.
    block->parsed = carve(block, parsed.data, result.span_count);
    return block->parsed;
}

float ui_markdown_height(UiMarkdown* block, float width, const UiKitMeasure* measure) noexcept {
    if (!block || !block->parsed)
        return 0;
    const float bucket = static_cast<float>(static_cast<u32>(width < 1 ? 1.f : width));
    if (block->measured_width == bucket)
        return block->height;
    const float line = line_height(block->body_size);
    float top = 0;
    for (u32 i = 0; i < block->part_count; ++i) {
        auto& part = block->parts[i];
        const bool item = part.marker != UiMarkdownMarker::none;
        float height = line;
        if (part.bytes) {
            const Utf8View text{block->text.data + part.offset, part.bytes};
            const ConstSlice<TextStyleSpan> spans{block->spans.data + part.span_first, part.span_count};
            const float available = maximum(1.f, bucket - (item ? item_indent(part.level) : 0.f));
            height = measure && measure->styled_height ? measure->styled_height(measure->context, text, spans, available) : -1.f;
            if (height < 0)
                height = estimate_height(block->body_size, text, spans, available);
        }
        part.top = top;
        part.height = height;
        top += height + (item ? kItemGap : 0.f);
    }
    const bool last_item = block->part_count && block->parts[block->part_count - 1].marker != UiMarkdownMarker::none;
    block->measured_width = bucket;
    block->height = top - (last_item ? kItemGap : 0.f);
    return block->height;
}

u32 ui_markdown_part_count(const UiMarkdown& block) noexcept {
    return block.parsed ? block.part_count : 0;
}

UiTextCommand ui_markdown_command(UiMarkdown* block, u32 index, bool marker, const UiKitMeasure* measure,
                                  UiWidgetReference owner, const UiRect& rectangle, const UiRect& clip,
                                  const Color& color, const Color& muted, float fade) noexcept {
    UiTextCommand command{};
    if (!block || !block->parsed || index >= block->part_count)
        return command;
    ui_markdown_height(block, ui_rect_width(rectangle), measure);
    const auto& part = block->parts[index];
    const bool item = part.marker != UiMarkdownMarker::none;
    if (marker ? !item : !part.bytes)
        return command;
    const float indent = item ? item_indent(part.level) : 0.f, top = rectangle.top + part.top;
    const UiRect box = marker ? UiRect{rectangle.left + indent - kMarkerLeft, top, rectangle.left + indent - kMarkerRight,
                                       top + line_height(block->body_size)}
                              : UiRect{rectangle.left + indent, top, rectangle.right, top + part.height};
    if (!ui_rect_intersects(box, clip))
        return command;
    command.owner = owner;
    command.rectangle = box;
    command.clip = clip;
    command.color = item && part.level ? muted : color;
    if (fade > 0) {
        // The part's last line sits in the fade band: the lower it is, the fainter (never fully gone).
        const float middle = minimum(top + part.height, rectangle.bottom) - line_height(block->body_size) * .5f,
                    start = rectangle.bottom - fade;
        if (middle > start)
            command.color.alpha *= maximum(.2f, 1.f - (middle - start) / fade * .8f);
    }
    command.wrapping = marker ? TextWrapping::no_wrap : TextWrapping::wrap;
    command.overflow = TextOverflow::clip;
    command.horizontal_alignment = marker ? UiTextAlignment::end : UiTextAlignment::start;
    if (marker) {
        command.style_flags = text_style_fold_size(0, block->body_size);
        command.text = part.marker == UiMarkdownMarker::number ? Utf8View{block->text.data + part.marker_offset, part.marker_bytes}
                       : part.marker == UiMarkdownMarker::dash ? utf8_literal("\xE2\x80\x93")
                                                               : utf8_literal("\xE2\x80\xA2");
        return command;
    }
    command.text = {block->text.data + part.offset, part.bytes};
    command.styles = {block->spans.data + part.span_first, part.span_count};
    command.effect.themed_styles = true;
    return command;
}

UiMarkdown* ui_markdown_bank_block(UiMarkdownBank** slot, u64 owner) noexcept {
    if (!slot || !owner)
        return nullptr;
    if (!*slot) {
        constexpr usize kBlock = kUiMarkdownSourceMaximum + kUiMarkdownSpanStorage * sizeof(TextStyleSpan) +
                                 kUiMarkdownPartMaximum * sizeof(UiMarkdownPart);
        constexpr usize kScratch = kUiMarkdownSourceMaximum * sizeof(u32);
        Arena arena{};
        // The bank is the arena's first allocation: freeing the arena frees everything. Pages commit on first touch,
        // so a block costs memory only once it holds a parse.
        if (!arena_initialize(&arena, sizeof(UiMarkdownBank) + kBlock * kUiMarkdownBankBlocks + kScratch + 4096, 0, 4096,
                              ArenaTag::ui))
            return nullptr;
        auto* bank = arena_allocate_zero<UiMarkdownBank>(&arena);
        bool ok = bank != nullptr;
        for (u32 i = 0; ok && i < kUiMarkdownBankBlocks; ++i) {
            auto* text = static_cast<char*>(arena_allocate(&arena, kUiMarkdownSourceMaximum, 1));
            auto* spans = arena_allocate<TextStyleSpan>(&arena, kUiMarkdownSpanStorage);
            auto* parts = arena_allocate<UiMarkdownPart>(&arena, kUiMarkdownPartMaximum);
            ok = text && spans && parts;
            if (ok) {
                bank->blocks[i].text = {text, kUiMarkdownSourceMaximum};
                bank->blocks[i].spans = {spans, kUiMarkdownSpanStorage};
                bank->blocks[i].parts = {parts, kUiMarkdownPartMaximum};
            }
        }
        auto* scratch = ok ? arena_allocate<u32>(&arena, kUiMarkdownSourceMaximum) : nullptr;
        if (!scratch) {
            arena_release(&arena);
            return nullptr;
        }
        bank->scratch = {scratch, kUiMarkdownSourceMaximum};
        bank->arena = arena;
        *slot = bank;
    }
    auto* bank = *slot;
    u32 pick = 0;
    for (u32 i = 0; i < kUiMarkdownBankBlocks; ++i) {
        if (bank->owners[i] == owner) {
            pick = i;
            break;
        }
        if (bank->stamps[i] < bank->stamps[pick])
            pick = i;
    }
    if (bank->owners[pick] != owner) {
        bank->owners[pick] = owner;
        bank->blocks[pick].parsed = false;
    }
    bank->stamps[pick] = ++bank->clock;
    return &bank->blocks[pick];
}

Slice<u32> ui_markdown_bank_scratch(UiMarkdownBank* bank) noexcept {
    return bank ? bank->scratch : Slice<u32>{};
}

void ui_markdown_bank_release(UiMarkdownBank** slot) noexcept {
    if (!slot || !*slot)
        return;
    Arena arena = (*slot)->arena;
    *slot = nullptr;
    arena_release(&arena);
}

} // namespace maicord
