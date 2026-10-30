#pragma once

#include "maicord/core/base.hpp"

namespace maicord {
enum class UiTextFace : u8 {
    normal,
    display,
    serif,
    rounded,
    mono,
    hand,
    techno,
    bangers,
    biorhyme,
    cherry_bomb,
    chicle,
    compagnon,
    museo,
    medieval,
    pixelify,
    ribes,
    sinistre,
    zilla,
    playpen,
    new_rocker,
    kalam,
    count
};

enum class TextWrapping : u8 {
    wrap,
    no_wrap,
};

enum class TextOverflow : u8 {
    clip,
    ellipsis,
};

// Em size of quick-reaction glyphs, the same in the 44-DIP context targets and
// the 28-DIP hover targets. Independent of the target extent so layout, rather
// than font size, owns centering.
inline constexpr float kPickerEmojiEmSize = 24.0f;
// Expression picker grid emoji: Discord's medium and large emoji sizes.
inline constexpr float kPickerGridEmojiEmSize = 40.0f;
inline constexpr float kPickerLargeEmojiEmSize = 48.0f;

enum TextStyleFlag : u32 {
    text_style_none = 0,
    text_style_bold = 1u << 0,
    text_style_italic = 1u << 1,
    text_style_underline = 1u << 2,
    text_style_strikethrough = 1u << 3,
    text_style_monospace = 1u << 4,
    text_style_heading_1 = 1u << 5,
    text_style_heading_2 = 1u << 6,
    text_style_heading_3 = 1u << 7,
    text_style_subtext = 1u << 8,
    text_style_code_block = 1u << 9,
    text_style_spoiler = 1u << 10,
    text_style_link = 1u << 11,
    text_style_mention = 1u << 12,
    text_style_quote = 1u << 13,
    text_style_list = 1u << 14,
    text_style_list_bullet = 1u << 15,
    text_style_list_depth_1 = 1u << 16,
    text_style_list_depth_2 = 1u << 17,
    text_style_syntax_keyword = 1u << 18,
    text_style_syntax_type = 1u << 19,
    text_style_syntax_string = 1u << 20,
    text_style_syntax_number = 1u << 21,
    text_style_syntax_comment = 1u << 22,
    text_style_syntax_operator = 1u << 23,
    text_style_syntax_builtin = 1u << 24,
    text_style_syntax_preprocessor = 1u << 25,
    text_style_timestamp = 1u << 26,
    text_style_interactive_hover = 1u << 27,
    // Shapes an object-replacement cell but emits no glyph quad; the retained
    // media pass fills the semantic cell from the CDN atlas.
    text_style_custom_emoji = 1u << 28,
    // Uses MessageTextCommand::inline_role_color for an inline author while
    // preserving the command's muted base color for the surrounding preview.
    text_style_inline_role_color = 1u << 29,
    // Messages containing only emoji are enlarged when there are at most
    // thirty emoji. This flag selects the corresponding 48-DIP text/object
    // cell without changing ordinary message typography.
    text_style_jumbo_emoji = 1u << 30,
    // Reaction pickers use a small fixed em inside independently sized targets,
    // not the 48-DIP emoji-only message typography.
    text_style_picker_emoji = 1u << 31,
};
// Picker emoji plus one heading bit: the expression picker grid sizes.
inline constexpr u32 text_style_picker_grid_emoji = text_style_picker_emoji | text_style_heading_1;
inline constexpr u32 text_style_picker_large_emoji = text_style_picker_emoji | text_style_heading_2;
// A slash command option key ("name:") in the composer: a neutral pill (no
// list or mention meaning; list depth needs text_style_list).
inline constexpr u32 text_style_command_option = text_style_mention | text_style_list_depth_2;
constexpr bool text_style_is_command_option(u32 flags) noexcept {
    return (flags & text_style_command_option) == text_style_command_option && !(flags & text_style_list);
}
constexpr float text_style_picker_emoji_size(u32 flags) noexcept {
    return flags & text_style_heading_1   ? kPickerGridEmojiEmSize
           : flags & text_style_heading_2 ? kPickerLargeEmojiEmSize
                                          : kPickerEmojiEmSize;
}

// Shapers see one flags word: the text layer folds TextStyleSpan::size into
// the heading bits (text_style_fold_size). One heading bit keeps its own
// ratio (1.5, 1.25, 1.1); two together are a size code with the rem ratio
// noted below. "#" and "##" sizes are the single heading bits.
inline constexpr u32 kTextStyleHeadingMask = text_style_heading_1 | text_style_heading_2 | text_style_heading_3;
inline constexpr u32 kTextStyleSizeCodeMd = text_style_heading_1 | text_style_heading_2; // 1rem
inline constexpr u32 kTextStyleSizeCodeSm = text_style_heading_1 | text_style_heading_3; // .875rem
inline constexpr u32 kTextStyleSizeCodeXs = text_style_heading_2 | text_style_heading_3; // .75rem
inline constexpr u32 kTextStyleSizeCodeXxl = kTextStyleHeadingMask;                       // 2rem
// The rem ratio of a size code, or zero for flags without one.
constexpr float text_style_size_code_ratio(u32 flags) noexcept {
    switch (flags & kTextStyleHeadingMask) {
    case kTextStyleSizeCodeMd:
        return 1.0f;
    case kTextStyleSizeCodeSm:
        return .875f;
    case kTextStyleSizeCodeXs:
        return .75f;
    case kTextStyleSizeCodeXxl:
        return 2.0f;
    default:
        return 0.0f;
    }
}
inline constexpr u32 kTextStyleListDepthMask = text_style_list_depth_1 | text_style_list_depth_2;
inline constexpr u32 kTextStyleListDepthShift = 16;
inline constexpr u32 kTextStyleSyntaxMask = text_style_syntax_keyword | text_style_syntax_type |
                                            text_style_syntax_string | text_style_syntax_number |
                                            text_style_syntax_comment | text_style_syntax_operator |
                                            text_style_syntax_builtin | text_style_syntax_preprocessor;

// The rem text scale (1rem is the body size), for runs whose size is not
// the one their style flags select: markup headings and subtext, and the embed
// text variants. Zero keeps the flag-selected size.
enum TextSize : u8 {
    text_size_inherit,
    text_size_xs, // .75rem: embed footer and provider
    text_size_sm, // .875rem: embed author/description/fields, "-#" subtext
    text_size_md, // 1rem: embed title, "###"
    text_size_lg, // 1.25rem: "##"
    text_size_xl, // 1.5rem: "#"
    text_size_xxl, // 2rem: channel-beginning header
    text_size_count
};

struct TextStyleSpan {
    u32 byte_offset;
    u32 byte_count;
    // A span may carry only a size (flags zero) to scale otherwise plain text.
    u32 flags;
    u8 size; // TextSize
    u8 reserved[3];
};

// reserved[2] bit 2: the code block is framed by its owner (a block box drawn
// around the command), so the text layer draws no code-block background.
// Bits 0/1 belong to the ```ansi colours (ansi_text.hpp).
inline constexpr u8 kTextSpanFramedBlock = 1u << 2;
constexpr bool text_span_framed_block(const TextStyleSpan& span) noexcept {
    return (span.reserved[2] & kTextSpanFramedBlock) != 0;
}

enum class TextDecorationKind : u8 {
    background,
    underline,
    strikethrough,
    quote_bar,
    list_bullet,
};

struct TextDecoration {
    float left;
    float top;
    float right;
    float bottom;
    u32 style_flags;
    TextDecorationKind kind;
    u8 reserved0;
    u8 reserved1;
    u8 reserved2;
};

inline constexpr u32 kTextStyleSpanMaximum = 1'024;

// The shaping flags for an explicit size (zero keeps flags). An invalid size
// has no code; text layers reject such spans before folding.
constexpr u32 text_style_fold_size(u32 flags, u8 size) noexcept {
    const u32 rest = flags & ~kTextStyleHeadingMask;
    switch (size) {
    case text_size_xs:
        return rest | kTextStyleSizeCodeXs;
    case text_size_sm:
        return rest | kTextStyleSizeCodeSm;
    case text_size_md:
        return rest | kTextStyleSizeCodeMd;
    case text_size_lg:
        return rest | text_style_heading_2;
    case text_size_xl:
        return rest | text_style_heading_1;
    case text_size_xxl:
        return rest | kTextStyleSizeCodeXxl;
    default:
        return flags;
    }
}

inline float text_style_font_size(float em,u32 flags) noexcept {
    if(flags&text_style_picker_emoji)return text_style_picker_emoji_size(flags);
    if(flags&text_style_jumbo_emoji)return em*3.0f;
    if(const float ratio=text_style_size_code_ratio(flags))return em*ratio;
    if(flags&text_style_heading_1)return em*1.50f;
    if(flags&text_style_heading_2)return em*1.25f;
    if(flags&text_style_heading_3)return em*1.10f;
    if(flags&text_style_subtext)return em*0.8125f;
    if(flags&text_style_monospace)return em*0.9375f;
    return em;
}

static_assert(sizeof(TextStyleSpan) == 16);
static_assert(sizeof(TextDecoration) == 24);

} // namespace maicord
