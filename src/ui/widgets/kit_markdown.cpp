#include "maicord/ui/widgets/kit_markdown.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/chat/messages/markdown.hpp"

namespace maicord {
namespace {
// Heading flags the parser leaves on a `###` run.
constexpr u32 kEyebrowFlags = text_style_heading_3 | text_style_bold;
float body_em(u8 size) noexcept {
    return size == text_size_sm ? 14.f : 16.f;
}
void ascii_upper(char* text, u32 bytes) noexcept {
    for (u32 i = 0; i < bytes; ++i)
        if (text[i] >= 'a' && text[i] <= 'z')
            text[i] = static_cast<char>(text[i] - 32);
}
} // namespace

float ui_markdown_line_height(u8 body_size) noexcept {
    return body_size == text_size_sm ? 20.f : 22.f;
}

bool ui_markdown_parse(UiMarkdown* block, const Utf8View& source, const UiMarkdownOptions& options,
                       Slice<u32> scratch) noexcept {
    if (!block)
        return false;
    if (source.bytes > kUiMarkdownSourceMaximum || source.bytes > block->text.count || source.bytes > scratch.count ||
        block->spans.count < 1) {
        block->parsed = false;
        return false;
    }
    const u8 body = options.body_size ? options.body_size : u8{text_size_sm};
    u64 signature = hash64_bytes(kHash64Seed, source.data, source.bytes);
    signature = hash64_mix(signature, static_cast<u64>(body) | (options.eyebrow_h3 ? 0x100u : 0u));
    if (block->parsed && block->source_signature == signature)
        return true;
    block->parsed = false;
    MessageMarkdownOptions parser{};
    parser.text_sizes = true;
    parser.base_size = body;
    const auto result = message_markdown_parse(source, block->text, scratch, block->spans, parser, {});
    if (!result.ok())
        return false;
    block->text_bytes = result.text.bytes;
    block->span_count = result.span_count;
    for (u32 i = 0; i < block->span_count; ++i) {
        auto& span = block->spans[i];
        u32 flags = span.flags;
        u8 size = span.size;
        // A `###` run: a 12 pt bold uppercase eyebrow without the heading ratio.
        if (options.eyebrow_h3 && (flags & kEyebrowFlags) == kEyebrowFlags &&
            !(flags & (text_style_heading_1 | text_style_heading_2))) {
            ascii_upper(block->text.data + span.byte_offset, span.byte_count);
            flags &= ~text_style_heading_3;
            size = text_size_xs;
        }
        span.flags = text_style_fold_size(flags, size);
        span.size = 0;
    }
    block->body_size = body;
    block->source_signature = signature;
    block->measured_width = -1;
    block->height = 0;
    block->parsed = true;
    return true;
}

float ui_markdown_height(UiMarkdown* block, float width, const UiKitMeasure* measure) noexcept {
    if (!block || !block->parsed)
        return 0;
    const float bucket = static_cast<float>(static_cast<u32>(width < 1 ? 1.f : width));
    if (block->measured_width == bucket)
        return block->height;
    const Utf8View text{block->text.data, block->text_bytes};
    float height = -1;
    if (measure && measure->styled_height)
        height = measure->styled_height(measure->context, text, ConstSlice<TextStyleSpan>{block->spans.data, block->span_count},
                                        bucket);
    if (height < 0) {
        height = 0;
        // Per paragraph: the style of its first byte sets the size; list items indent their text.
        const float em = body_em(block->body_size), line = ui_markdown_line_height(block->body_size);
        u32 start = 0;
        u32 span = 0;
        for (u32 i = 0; i <= text.bytes; ++i) {
            if (i != text.bytes && text.data[i] != '\n')
                continue;
            while (span + 1 < block->span_count && block->spans[span].byte_offset + block->spans[span].byte_count <= start)
                ++span;
            const u32 flags = block->span_count ? block->spans[span].flags : 0;
            const float size = text_style_font_size(em / 1.0f, flags & ~text_style_subtext);
            const float indent = (flags & text_style_list) ? 20.f * (1 + ((flags & kTextStyleListDepthMask) >> kTextStyleListDepthShift)) : 0;
            const u32 lines = i == start ? 1u
                                         : maximum(1u, ui_settings_text_lines({text.data + start, i - start}, size * 1.04f,
                                                                              maximum(1.f, bucket - indent)));
            height += static_cast<float>(lines) * maximum(line, size * 1.4f);
            start = i + 1;
        }
    }
    block->measured_width = bucket;
    block->height = height;
    return height;
}

UiTextCommand ui_markdown_command(const UiMarkdown& block, UiWidgetReference owner, const UiRect& rectangle,
                                  const UiRect& clip, const Color& color) noexcept {
    return {Utf8View{block.text.data, block.text_bytes},
            owner,
            rectangle,
            clip,
            color,
            0,
            UiTextAlignment::start,
            UiTextAlignment::start,
            TextWrapping::wrap,
            TextOverflow::clip,
            UiTextFace::normal,
            {},
            ConstSlice<TextStyleSpan>{block.spans.data, block.span_count}};
}

UiMarkdown* ui_markdown_bank_block(UiMarkdownBank** slot, u64 owner) noexcept {
    if (!slot || !owner)
        return nullptr;
    if (!*slot) {
        constexpr usize kBlock = kUiMarkdownSourceMaximum + kUiMarkdownSpanMaximum * sizeof(TextStyleSpan);
        constexpr usize kScratch = kUiMarkdownSourceMaximum * sizeof(u32);
        Arena arena{};
        // The bank is the arena's first allocation: freeing the arena frees everything. Pages commit on first touch,
        // so a block costs memory only once it holds a parse.
        if (!arena_initialize(&arena, sizeof(UiMarkdownBank) + kBlock * kUiMarkdownBankBlocks + kScratch + 4096, 0, 4096))
            return nullptr;
        auto* bank = arena_allocate_zero<UiMarkdownBank>(&arena);
        bool ok = bank != nullptr;
        for (u32 i = 0; ok && i < kUiMarkdownBankBlocks; ++i) {
            auto* text = static_cast<char*>(arena_allocate(&arena, kUiMarkdownSourceMaximum, 1));
            auto* spans = arena_allocate<TextStyleSpan>(&arena, kUiMarkdownSpanMaximum);
            ok = text && spans;
            if (ok) {
                bank->blocks[i].text = {text, kUiMarkdownSourceMaximum};
                bank->blocks[i].spans = {spans, kUiMarkdownSpanMaximum};
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
