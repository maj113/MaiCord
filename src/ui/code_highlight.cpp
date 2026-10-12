#include "maicord/ui/code_highlight.hpp"
#include "maicord/core/ascii.hpp"

namespace maicord {
namespace {

bool ascii_alpha(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

bool identifier_begin(char value) noexcept {
    return ascii_alpha(value) || value == '_' || value == '$';
}

bool identifier_continue(char value) noexcept {
    return identifier_begin(value) || ascii_is_digit(value);
}

bool tag_equal(Utf8View tag, const char* expected, u32 expected_bytes) noexcept {
    while (tag.bytes != 0 && tag.data[tag.bytes - 1] == '\r') {
        --tag.bytes;
    }
    if (tag.bytes != expected_bytes) {
        return false;
    }
    for (u32 index = 0; index < expected_bytes; ++index) {
        if (ascii_lower(tag.data[index]) != expected[index]) {
            return false;
        }
    }
    return true;
}

bool c_family(CodeLanguage language) noexcept {
    return language == CodeLanguage::c || language == CodeLanguage::cpp ||
           language == CodeLanguage::csharp || language == CodeLanguage::rust ||
           language == CodeLanguage::javascript || language == CodeLanguage::typescript ||
           language == CodeLanguage::java || language == CodeLanguage::hlsl ||
           language == CodeLanguage::glsl || language == CodeLanguage::css;
}

bool preprocessor_language(CodeLanguage language) noexcept {
    return language == CodeLanguage::c || language == CodeLanguage::cpp || language == CodeLanguage::hlsl ||
           language == CodeLanguage::glsl;
}

bool hash_comment_language(CodeLanguage language) noexcept {
    return language == CodeLanguage::python || language == CodeLanguage::powershell ||
           language == CodeLanguage::shell;
}

bool dash_comment_language(CodeLanguage language) noexcept {
    return language == CodeLanguage::sql;
}

struct LanguageLexicon {
    const char* keywords;
    const char* types;
    const char* builtins;
    bool ignore_case;
};

LanguageLexicon language_lexicon(CodeLanguage language) noexcept {
    const char* keywords = "";
    const char* types = "";
    const char* builtins = "";
    switch (language) {
    case CodeLanguage::c:
        keywords = "auto break case const continue default do else enum extern for goto if inline register "
                   "restrict return sizeof static struct switch typedef union volatile while";
        types = "char double float int long short signed unsigned void _Bool size_t";
        builtins = "NULL offsetof alignof _Alignof _Atomic";
        break;
    case CodeLanguage::cpp:
        keywords = "alignas asm break case catch class concept const consteval constexpr constinit "
                   "continue co_await co_return co_yield decltype default delete do else enum explicit "
                   "export extern for friend goto if inline mutable namespace new noexcept operator "
                   "private protected public requires return sizeof static struct switch template this "
                   "thread_local throw try typedef typeid typename union using virtual volatile while";
        types = "auto bool char char8_t char16_t char32_t double float int long short signed unsigned void "
                "wchar_t size_t string u8 u16 u32 u64 i8 i16 i32 i64";
        builtins = "nullptr static_cast reinterpret_cast const_cast dynamic_cast alignof __declspec";
        break;
    case CodeLanguage::csharp:
        keywords =
            "abstract as base break case catch checked class const continue default delegate do else enum "
            "event explicit extern finally fixed for foreach goto if implicit in interface internal is "
            "lock namespace new operator out override params private protected public readonly ref return "
            "sealed sizeof stackalloc static struct switch this throw try typeof unchecked unsafe using "
            "virtual volatile while async await record required init";
        types = "bool byte char decimal double float int long object sbyte short string uint ulong ushort "
                "void var dynamic";
        builtins = "null true false get set value nameof";
        break;
    case CodeLanguage::rust:
        keywords = "as async await break const continue crate dyn else enum extern fn for if impl in let "
                   "loop match mod move mut pub ref return self Self static struct super trait type unsafe "
                   "use where while yield";
        types = "bool char f32 f64 i8 i16 i32 i64 i128 isize str u8 u16 u32 u64 u128 usize Option Result "
                "String Vec";
        builtins = "Some None Ok Err Box println panic todo unreachable";
        break;
    case CodeLanguage::javascript:
    case CodeLanguage::typescript:
        keywords = "async await break case catch class const continue debugger default delete do else "
                   "export extends finally for from function get if import in instanceof let new of return "
                   "set static switch throw try typeof var void while with yield";
        types = language == CodeLanguage::typescript
                    ? "any boolean bigint never number object string symbol unknown void interface type "
                      "enum namespace readonly keyof infer asserts is"
                    : "Array BigInt Boolean Date Error Map Number Object Promise RegExp Set String Symbol "
                      "WeakMap WeakSet";
        builtins = "console document globalThis JSON Math module process require window parseInt "
                   "parseFloat setTimeout setInterval";
        break;
    case CodeLanguage::java:
        keywords = "abstract assert break case catch class const continue default do else enum extends "
                   "final finally for goto if implements import instanceof interface native new package "
                   "private protected public return static strictfp super switch synchronized this throw "
                   "throws transient try volatile while record sealed permits var yield";
        types = "boolean byte char double float int long short void String Object Integer Long Double List "
                "Map Set Optional";
        builtins = "null true false System Math";
        break;
    case CodeLanguage::json:
        builtins = "true false null";
        break;
    case CodeLanguage::python:
        keywords = "and as assert async await break class continue def del elif else except finally for "
                   "from global if import in is lambda nonlocal not or pass raise return try while with "
                   "yield match case";
        types = "bool bytes bytearray complex dict float frozenset int list memoryview object range set "
                "slice str tuple type";
        builtins = "None True False abs all any bin callable chr dir enumerate filter format getattr "
                   "hasattr hash help hex id input isinstance issubclass iter len map max min next oct "
                   "open ord pow print repr reversed round sorted sum vars zip";
        break;
    case CodeLanguage::powershell:
        keywords = "begin break catch class continue data define do dynamicparam else elseif end enum exit "
                   "filter finally for foreach from function hidden if in param process return static "
                   "switch throw trap try until using var while workflow";
        types = "string int int32 int64 bool boolean byte char datetime decimal double float hashtable "
                "object psobject regex scriptblock switch void xml";
        builtins = "Write-Host Write-Output Get-Item Get-ChildItem Set-Item Remove-Item Where-Object "
                   "ForEach-Object Select-Object";
        break;
    case CodeLanguage::shell:
        keywords = "case do done elif else esac fi for function if in select then time until while";
        builtins =
            "alias bg bind break builtin cd command continue echo eval exec exit export false fg getopts "
            "hash help jobs kill let local logout mapfile printf pwd read readonly return set shift source "
            "suspend test times trap true type typeset ulimit umask unalias unset wait";
        break;
    case CodeLanguage::hlsl:
    case CodeLanguage::glsl:
        keywords = "break case const continue default discard do else for if in inout out return static "
                   "struct switch uniform volatile while cbuffer register";
        types = "bool int uint half float double bool2 bool3 bool4 int2 int3 int4 uint2 uint3 uint4 half2 "
                "half3 half4 float2 float3 float4 float2x2 float3x3 float4x4 vector matrix sampler "
                "sampler2D Texture2D Texture3D TextureCube SamplerState void vec2 vec3 vec4 ivec2 ivec3 "
                "ivec4 uvec2 uvec3 uvec4 mat2 mat3 mat4";
        builtins = "abs clamp cos cross ddx ddy distance dot exp floor frac lerp length log max min mul "
                   "normalize pow reflect refract saturate sin smoothstep sqrt step tex2D transpose";
        break;
    case CodeLanguage::sql:
        keywords =
            "ADD ALL ALTER AND ANY AS ASC BACKUP BETWEEN BY CASE CHECK COLUMN CONSTRAINT CREATE DATABASE "
            "DEFAULT DELETE DESC DISTINCT DROP ELSE END EXISTS FOREIGN FROM FULL GROUP HAVING IN INDEX "
            "INNER INSERT INTO IS JOIN KEY LEFT LIKE LIMIT NOT NULL OR ORDER OUTER PRIMARY PROCEDURE RIGHT "
            "ROW ROWNUM SELECT SET TABLE TOP TRUNCATE UNION UNIQUE UPDATE VALUES VIEW WHEN WHERE WITH";
        types = "BIGINT BINARY BIT BLOB BOOLEAN CHAR DATE DATETIME DECIMAL DOUBLE FLOAT INT INTEGER JSON "
                "NUMERIC REAL SMALLINT TEXT TIME TIMESTAMP VARCHAR";
        builtins = "AVG COUNT MAX MIN SUM COALESCE CAST CONVERT LOWER UPPER LENGTH NOW";
        break;
    case CodeLanguage::markup:
        builtins = "html head body div span a img script style link meta title section article header "
                   "footer nav main button input form label canvas svg path";
        break;
    case CodeLanguage::css:
        keywords = "important inherit initial revert unset auto none normal block inline flex grid "
                   "absolute relative fixed sticky";
        types = "color background border margin padding width height display position font transform "
                "transition animation opacity overflow justify-content align-items";
        builtins = "var calc rgb rgba hsl hsla url linear-gradient radial-gradient repeat minmax";
        break;
    default:
        break;
    }
    return LanguageLexicon{keywords, types, builtins,
                           language == CodeLanguage::powershell || language == CodeLanguage::sql ||
                               language == CodeLanguage::markup};
}

bool token_in_words(Utf8View token, const char* words, bool ignore_case) noexcept {
    u32 offset = 0;
    while (words[offset]) {
        while (words[offset] == ' ') {
            ++offset;
        }
        const u32 begin = offset;
        while (words[offset] && words[offset] != ' ') {
            ++offset;
        }
        const u32 bytes = offset - begin;
        if (bytes == token.bytes) {
            bool equal = true;
            for (u32 index = 0; index < bytes; ++index) {
                const char left = ignore_case ? ascii_lower(token.data[index]) : token.data[index];
                const char right = ignore_case ? ascii_lower(words[begin + index]) : words[begin + index];
                equal = equal && left == right;
            }
            if (equal) {
                return true;
            }
        }
    }
    return false;
}

u32 keyword_style(CodeLanguage language, Utf8View token) noexcept {
    const LanguageLexicon lexicon = language_lexicon(language);
    if (token_in_words(token, "true false null nullptr undefined NaN None True False self this super",
                       lexicon.ignore_case)) {
        return text_style_syntax_builtin;
    }
    if (token_in_words(token, lexicon.keywords, lexicon.ignore_case)) {
        return text_style_syntax_keyword;
    }
    if (token_in_words(token, lexicon.types, lexicon.ignore_case)) {
        return text_style_syntax_type;
    }
    if (token_in_words(token, lexicon.builtins, lexicon.ignore_case)) {
        return text_style_syntax_builtin;
    }
    return 0;
}

struct Highlighter {
    Utf8View code;
    CodeHighlightRangeSink sink;
    void* context;
    u32 capacity;
    u32 tokens;
    u32 styled_bytes;
    bool truncated;
};

bool mark(Highlighter* highlighter, u32 begin, u32 end, u32 style) noexcept {
    if (style == 0 || begin >= end || end > highlighter->code.bytes) {
        return true;
    }
    if (highlighter->tokens >= highlighter->capacity) {
        highlighter->truncated = true;
        return false;
    }
    highlighter->sink(highlighter->context, begin, end, style);
    ++highlighter->tokens;
    highlighter->styled_bytes += end - begin;
    return true;
}

u32 quoted_end(Utf8View code, u32 begin, char quote) noexcept {
    u32 offset = begin + 1;
    while (offset < code.bytes) {
        const char value = code.data[offset++];
        if (value == '\\' && offset < code.bytes) {
            ++offset;
        } else if (value == quote) {
            break;
        } else if (value == '\n' && quote != '`') {
            break;
        }
    }
    return offset;
}

u32 line_end(Utf8View code, u32 begin) noexcept {
    u32 offset = begin;
    while (offset < code.bytes && code.data[offset] != '\n') {
        ++offset;
    }
    return offset;
}

u32 block_end(Utf8View code, u32 begin, char first, char second) noexcept {
    u32 offset = begin;
    while (offset + 1 < code.bytes && !(code.data[offset] == first && code.data[offset + 1] == second)) {
        ++offset;
    }
    return offset + 1 < code.bytes ? offset + 2 : code.bytes;
}

u32 number_end(Utf8View code, u32 begin) noexcept {
    u32 offset = begin;
    if (offset < code.bytes && code.data[offset] == '.') {
        ++offset;
    }
    if (offset + 1 < code.bytes && code.data[offset] == '0' &&
        (code.data[offset + 1] == 'x' || code.data[offset + 1] == 'X' || code.data[offset + 1] == 'b' ||
         code.data[offset + 1] == 'B')) {
        offset += 2;
        while (offset < code.bytes &&
               (ascii_is_digit(code.data[offset]) ||
                (ascii_lower(code.data[offset]) >= 'a' && ascii_lower(code.data[offset]) <= 'f') ||
                code.data[offset] == '_')) {
            ++offset;
        }
    } else {
        while (offset < code.bytes && (ascii_is_digit(code.data[offset]) || code.data[offset] == '_')) {
            ++offset;
        }
        if (offset < code.bytes && code.data[offset] == '.') {
            ++offset;
            while (offset < code.bytes && (ascii_is_digit(code.data[offset]) || code.data[offset] == '_')) {
                ++offset;
            }
        }
        if (offset < code.bytes && (code.data[offset] == 'e' || code.data[offset] == 'E')) {
            ++offset;
            if (offset < code.bytes && (code.data[offset] == '+' || code.data[offset] == '-')) {
                ++offset;
            }
            while (offset < code.bytes && (ascii_is_digit(code.data[offset]) || code.data[offset] == '_')) {
                ++offset;
            }
        }
    }
    while (offset < code.bytes && ascii_alpha(code.data[offset])) {
        ++offset;
    }
    return offset;
}

bool operator_character(char value) noexcept {
    return value == '+' || value == '-' || value == '*' || value == '/' || value == '%' || value == '=' ||
           value == '!' || value == '<' || value == '>' || value == '&' || value == '|' || value == '^' ||
           value == '~' || value == '?' || value == ':';
}

CodeHighlightResult highlight_markup(Highlighter* highlighter) noexcept {
    u32 offset = 0;
    while (offset < highlighter->code.bytes && !highlighter->truncated) {
        if (offset + 4 <= highlighter->code.bytes && highlighter->code.data[offset] == '<' &&
            highlighter->code.data[offset + 1] == '!' && highlighter->code.data[offset + 2] == '-' &&
            highlighter->code.data[offset + 3] == '-') {
            u32 end = offset + 4;
            while (end + 2 < highlighter->code.bytes &&
                   !(highlighter->code.data[end] == '-' && highlighter->code.data[end + 1] == '-' &&
                     highlighter->code.data[end + 2] == '>')) {
                ++end;
            }
            end = end + 2 < highlighter->code.bytes ? end + 3 : highlighter->code.bytes;
            if (!mark(highlighter, offset, end, text_style_syntax_comment)) {
                break;
            }
            offset = end;
            continue;
        }
        const char value = highlighter->code.data[offset];
        if (value == '"' || value == '\'') {
            const u32 end = quoted_end(highlighter->code, offset, value);
            if (!mark(highlighter, offset, end, text_style_syntax_string)) {
                break;
            }
            offset = end;
            continue;
        }
        if (value == '<' || value == '>' || value == '/' || value == '=') {
            if (!mark(highlighter, offset, offset + 1, text_style_syntax_operator)) {
                break;
            }
            ++offset;
            continue;
        }
        if (identifier_begin(value)) {
            const u32 begin = offset++;
            while (offset < highlighter->code.bytes &&
                   (identifier_continue(highlighter->code.data[offset]) ||
                    highlighter->code.data[offset] == '-')) {
                ++offset;
            }
            const u32 style = keyword_style(CodeLanguage::markup,
                                            Utf8View{highlighter->code.data + begin, offset - begin});
            if (style != 0 && !mark(highlighter, begin, offset, style)) {
                break;
            }
            continue;
        }
        ++offset;
    }
    return CodeHighlightResult{highlighter->tokens, highlighter->styled_bytes, highlighter->truncated};
}

} // namespace

CodeLanguage code_language_from_tag(Utf8View tag) noexcept {
    if ((!tag.data && tag.bytes != 0) || tag.bytes > 32) {
        return CodeLanguage::none;
    }
    if (tag_equal(tag, "c", 1))
        return CodeLanguage::c;
    if (tag_equal(tag, "cpp", 3) || tag_equal(tag, "c++", 3) || tag_equal(tag, "cc", 2) ||
        tag_equal(tag, "cxx", 3))
        return CodeLanguage::cpp;
    if (tag_equal(tag, "cs", 2) || tag_equal(tag, "csharp", 6))
        return CodeLanguage::csharp;
    if (tag_equal(tag, "rs", 2) || tag_equal(tag, "rust", 4))
        return CodeLanguage::rust;
    if (tag_equal(tag, "js", 2) || tag_equal(tag, "javascript", 10) || tag_equal(tag, "jsx", 3))
        return CodeLanguage::javascript;
    if (tag_equal(tag, "ts", 2) || tag_equal(tag, "typescript", 10) || tag_equal(tag, "tsx", 3))
        return CodeLanguage::typescript;
    if (tag_equal(tag, "java", 4))
        return CodeLanguage::java;
    if (tag_equal(tag, "json", 4) || tag_equal(tag, "jsonc", 5))
        return CodeLanguage::json;
    if (tag_equal(tag, "py", 2) || tag_equal(tag, "python", 6))
        return CodeLanguage::python;
    if (tag_equal(tag, "ps1", 3) || tag_equal(tag, "powershell", 10))
        return CodeLanguage::powershell;
    if (tag_equal(tag, "sh", 2) || tag_equal(tag, "bash", 4) || tag_equal(tag, "zsh", 3) ||
        tag_equal(tag, "shell", 5))
        return CodeLanguage::shell;
    if (tag_equal(tag, "hlsl", 4))
        return CodeLanguage::hlsl;
    if (tag_equal(tag, "glsl", 4) || tag_equal(tag, "vert", 4) || tag_equal(tag, "frag", 4))
        return CodeLanguage::glsl;
    if (tag_equal(tag, "sql", 3))
        return CodeLanguage::sql;
    if (tag_equal(tag, "html", 4) || tag_equal(tag, "xml", 3))
        return CodeLanguage::markup;
    if (tag_equal(tag, "css", 3))
        return CodeLanguage::css;
    if (tag_equal(tag, "ansi", 4))
        return CodeLanguage::ansi;
    return CodeLanguage::none;
}

CodeHighlightResult code_highlight(Utf8View code, CodeLanguage language, Slice<u32> byte_styles,
                                   u32 token_capacity) noexcept {
    if (!byte_styles.data || code.bytes > byte_styles.count) return CodeHighlightResult{};
    return code_highlight_ranges(code, language,
        +[](void* context, u32 begin, u32 end, u32 flags) noexcept {
            auto* styles = static_cast<u32*>(context);
            for (u32 offset = begin; offset < end; ++offset) styles[offset] |= flags;
        }, byte_styles.data, token_capacity);
}

CodeHighlightResult code_highlight_ranges(Utf8View code, CodeLanguage language,
                                          CodeHighlightRangeSink sink, void* context,
                                          u32 token_capacity) noexcept {
    if ((!code.data && code.bytes != 0) || !sink ||
        language == CodeLanguage::none || language == CodeLanguage::ansi || token_capacity == 0) {
        return CodeHighlightResult{};
    }
    Highlighter highlighter{code, sink, context, token_capacity, 0, 0, false};
    if (language == CodeLanguage::markup) {
        return highlight_markup(&highlighter);
    }

    u32 offset = 0;
    bool line_start = true;
    while (offset < code.bytes && !highlighter.truncated) {
        const char value = code.data[offset];
        if (value == '\n') {
            line_start = true;
            ++offset;
            continue;
        }
        if (value == ' ' || value == '\t' || value == '\r') {
            ++offset;
            continue;
        }
        if (preprocessor_language(language) && line_start && value == '#') {
            const u32 end = line_end(code, offset);
            if (!mark(&highlighter, offset, end, text_style_syntax_preprocessor)) {
                break;
            }
            offset = end;
            continue;
        }
        line_start = false;
        if (c_family(language) && offset + 1 < code.bytes && value == '/' && code.data[offset + 1] == '/') {
            const u32 end = line_end(code, offset);
            if (!mark(&highlighter, offset, end, text_style_syntax_comment)) {
                break;
            }
            offset = end;
            continue;
        }
        if ((c_family(language) || language == CodeLanguage::sql) && offset + 1 < code.bytes &&
            value == '/' && code.data[offset + 1] == '*') {
            const u32 end = block_end(code, offset + 2, '*', '/');
            if (!mark(&highlighter, offset, end, text_style_syntax_comment)) {
                break;
            }
            offset = end;
            continue;
        }
        if (hash_comment_language(language) && value == '#') {
            const u32 end = line_end(code, offset);
            if (!mark(&highlighter, offset, end, text_style_syntax_comment)) {
                break;
            }
            offset = end;
            continue;
        }
        if (dash_comment_language(language) && offset + 1 < code.bytes && value == '-' &&
            code.data[offset + 1] == '-') {
            const u32 end = line_end(code, offset);
            if (!mark(&highlighter, offset, end, text_style_syntax_comment)) {
                break;
            }
            offset = end;
            continue;
        }
        if (value == '"' || value == '\'' ||
            (value == '`' &&
             (language == CodeLanguage::javascript || language == CodeLanguage::typescript ||
              language == CodeLanguage::shell || language == CodeLanguage::powershell))) {
            const u32 end = quoted_end(code, offset, value);
            if (!mark(&highlighter, offset, end, text_style_syntax_string)) {
                break;
            }
            offset = end;
            continue;
        }
        if (ascii_is_digit(value) ||
            (value == '.' && offset + 1 < code.bytes && ascii_is_digit(code.data[offset + 1]))) {
            const u32 end = number_end(code, offset);
            if (!mark(&highlighter, offset, end, text_style_syntax_number)) {
                break;
            }
            offset = end;
            continue;
        }
        if (identifier_begin(value)) {
            const u32 begin = offset++;
            while (offset < code.bytes &&
                   (identifier_continue(code.data[offset]) ||
                    (code.data[offset] == '-' &&
                     (language == CodeLanguage::powershell || language == CodeLanguage::css)))) {
                ++offset;
            }
            const u32 style = keyword_style(language, Utf8View{code.data + begin, offset - begin});
            if (style != 0 && !mark(&highlighter, begin, offset, style)) {
                break;
            }
            continue;
        }
        if (operator_character(value)) {
            u32 end = offset + 1;
            if (end < code.bytes && operator_character(code.data[end])) {
                ++end;
            }
            if (!mark(&highlighter, offset, end, text_style_syntax_operator)) {
                break;
            }
            offset = end;
            continue;
        }
        ++offset;
    }
    return CodeHighlightResult{highlighter.tokens, highlighter.styled_bytes, highlighter.truncated};
}

} // namespace maicord
