#pragma once

#include "maicord/core/utf.hpp"
#include "maicord/ui/text_style.hpp"

namespace maicord {

enum class CodeLanguage : u8 {
    none,
    c,
    cpp,
    csharp,
    rust,
    javascript,
    typescript,
    java,
    json,
    python,
    powershell,
    shell,
    hlsl,
    glsl,
    sql,
    markup,
    css,
    // ```ansi blocks: SGR escapes become styled runs (ansi_text.hpp),
    // not syntax tokens.
    ansi,
};

struct CodeHighlightResult {
    u32 token_count;
    u32 styled_bytes;
    bool truncated;
};

inline constexpr u32 kCodeHighlightTokenMaximum = 48;

using CodeHighlightRangeSink = void (*)(void* context, u32 begin, u32 end, u32 flags) noexcept;

CodeLanguage code_language_from_tag(Utf8View tag) noexcept;
CodeHighlightResult code_highlight(Utf8View code, CodeLanguage language,
                                   Slice<u32> byte_styles,
                                   u32 token_capacity) noexcept;
CodeHighlightResult code_highlight_ranges(Utf8View code, CodeLanguage language,
                                          CodeHighlightRangeSink sink, void* context,
                                          u32 token_capacity) noexcept;

} // namespace maicord
