#include "maicord/ui/code_highlight.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/ui/text_style.hpp"

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

// Length-prefixed words avoid rescanning fixed keyword lists during highlighting.
template<usize N> struct PackedWords {
    char data[N + 1]{};
    bool valid = true;
    consteval PackedWords(const char (&text)[N]) noexcept {
        usize source = 0, target = 0;
        while (source + 1 < N) {
            while (text[source] == ' ')
                ++source;
            const usize begin = source;
            while (text[source] && text[source] != ' ')
                ++source;
            const usize bytes = source - begin;
            if (!bytes)
                break;
            if (bytes > 255) {
                valid = false;
                return;
            }
            data[target++] = static_cast<char>(bytes);
            for (usize i = begin; i < source; ++i)
                data[target++] = text[i];
        }
    }
};

#define WORDS(name, text) \
    constexpr PackedWords words_##name{text}; \
    static_assert(words_##name.valid);
// Tokens in words_common take precedence and need no language-specific entry.
WORDS(c_keywords, "auto break case const continue default do else enum extern for goto if inline register "
                   "restrict return sizeof static struct switch typedef union volatile while")
WORDS(c_types, "char double float int long short signed unsigned void _Bool size_t")
WORDS(c_builtins, "NULL offsetof alignof _Alignof _Atomic")
WORDS(cpp_keywords,
      "alignas asm break case catch class concept const consteval constexpr constinit continue "
      "co_await co_return co_yield decltype default delete do else enum explicit export extern "
      "for friend goto if inline mutable namespace new noexcept operator private protected public "
      "requires return sizeof static struct switch template thread_local throw try typedef typeid "
      "typename union using virtual volatile while")
WORDS(cpp_types, "auto bool char char8_t char16_t char32_t double float int long short signed unsigned void "
                "wchar_t size_t string u8 u16 u32 u64 i8 i16 i32 i64")
WORDS(cpp_builtins,
      "static_cast reinterpret_cast const_cast dynamic_cast alignof __declspec")
WORDS(csharp_keywords,
      "abstract as base break case catch checked class const continue default delegate do else "
      "enum event explicit extern finally fixed for foreach goto if implicit in interface "
      "internal is lock namespace new operator out override params private protected public "
      "readonly ref return sealed sizeof stackalloc static struct switch throw try typeof "
      "unchecked unsafe using virtual volatile while async await record required init")
WORDS(csharp_types, "bool byte char decimal double float int long object sbyte short string uint ulong ushort "
                "void var dynamic")
WORDS(csharp_builtins,
      "get set value nameof")
WORDS(rust_keywords,
      "as async await break const continue crate dyn else enum extern fn for if impl in let loop "
      "match mod move mut pub ref return Self static struct trait type unsafe use where while "
      "yield")
WORDS(rust_types, "bool char f32 f64 i8 i16 i32 i64 i128 isize str u8 u16 u32 u64 u128 usize Option Result "
                "String Vec")
WORDS(rust_builtins,
      "Some Ok Err Box println panic todo unreachable")
WORDS(typescript_keywords, "async await break case catch class const continue debugger default delete do else "
                   "export extends finally for from function get if import in instanceof let new of return "
                   "set static switch throw try typeof var void while with yield")
WORDS(typescript_types, "any boolean bigint never number object string symbol unknown void interface type "
                      "enum namespace readonly keyof infer asserts is")
WORDS(javascript_types, "Array BigInt Boolean Date Error Map Number Object Promise RegExp Set String Symbol "
                      "WeakMap WeakSet")
WORDS(typescript_builtins, "console document globalThis JSON Math module process require window parseInt "
                   "parseFloat setTimeout setInterval")
WORDS(java_keywords,
      "abstract assert break case catch class const continue default do else enum extends final "
      "finally for goto if implements import instanceof interface native new package private "
      "protected public return static strictfp switch synchronized throw throws transient try "
      "volatile while record sealed permits var yield")
WORDS(java_types, "boolean byte char double float int long short void String Object Integer Long Double List "
                "Map Set Optional")
WORDS(java_builtins,
      "System Math")

WORDS(python_keywords, "and as assert async await break class continue def del elif else except finally for "
                   "from global if import in is lambda nonlocal not or pass raise return try while with "
                   "yield match case")
WORDS(python_types, "bool bytes bytearray complex dict float frozenset int list memoryview object range set "
                "slice str tuple type")
WORDS(python_builtins,
      "abs all any bin callable chr dir enumerate filter format getattr hasattr hash help hex id "
      "input isinstance issubclass iter len map max min next oct open ord pow print repr reversed "
      "round sorted sum vars zip")
WORDS(powershell_keywords, "begin break catch class continue data define do dynamicparam else elseif end enum exit "
                   "filter finally for foreach from function hidden if in param process return static "
                   "switch throw trap try until using var while workflow")
WORDS(powershell_types, "string int int32 int64 bool boolean byte char datetime decimal double float hashtable "
                "object psobject regex scriptblock switch void xml")
WORDS(powershell_builtins, "Write-Host Write-Output Get-Item Get-ChildItem Set-Item Remove-Item Where-Object "
                   "ForEach-Object Select-Object")
WORDS(shell_keywords, "case do done elif else esac fi for function if in select then time until while")
WORDS(shell_builtins,
      "alias bg bind break builtin cd command continue echo eval exec exit export fg getopts hash "
      "help jobs kill let local logout mapfile printf pwd read readonly return set shift source "
      "suspend test times trap type typeset ulimit umask unalias unset wait")
WORDS(glsl_keywords, "break case const continue default discard do else for if in inout out return static "
                   "struct switch uniform volatile while cbuffer register")
WORDS(glsl_types, "bool int uint half float double bool2 bool3 bool4 int2 int3 int4 uint2 uint3 uint4 half2 "
                "half3 half4 float2 float3 float4 float2x2 float3x3 float4x4 vector matrix sampler "
                "sampler2D Texture2D Texture3D TextureCube SamplerState void vec2 vec3 vec4 ivec2 ivec3 "
                "ivec4 uvec2 uvec3 uvec4 mat2 mat3 mat4")
WORDS(glsl_builtins, "abs clamp cos cross ddx ddy distance dot exp floor frac lerp length log max min mul "
                   "normalize pow reflect refract saturate sin smoothstep sqrt step tex2D transpose")
WORDS(sql_keywords, "ADD ALL ALTER AND ANY AS ASC BACKUP BETWEEN BY CASE CHECK COLUMN CONSTRAINT CREATE DATABASE "
            "DEFAULT DELETE DESC DISTINCT DROP ELSE END EXISTS FOREIGN FROM FULL GROUP HAVING IN INDEX "
            "INNER INSERT INTO IS JOIN KEY LEFT LIKE LIMIT NOT NULL OR ORDER OUTER PRIMARY PROCEDURE RIGHT "
            "ROW ROWNUM SELECT SET TABLE TOP TRUNCATE UNION UNIQUE UPDATE VALUES VIEW WHEN WHERE WITH")
WORDS(sql_types, "BIGINT BINARY BIT BLOB BOOLEAN CHAR DATE DATETIME DECIMAL DOUBLE FLOAT INT INTEGER JSON "
                "NUMERIC REAL SMALLINT TEXT TIME TIMESTAMP VARCHAR")
WORDS(sql_builtins, "AVG COUNT MAX MIN SUM COALESCE CAST CONVERT LOWER UPPER LENGTH NOW")
WORDS(markup_builtins, "html head body div span a img script style link meta title section article header "
                   "footer nav main button input form label canvas svg path")
WORDS(css_keywords, "important inherit initial revert unset auto none normal block inline flex grid "
                   "absolute relative fixed sticky")
WORDS(css_types, "color background border margin padding width height display position font transform "
                "transition animation opacity overflow justify-content align-items")
WORDS(css_builtins, "var calc rgb rgba hsl hsla url linear-gradient radial-gradient repeat minmax")
WORDS(common, "true false null nullptr undefined NaN None True False self this super")

#undef WORDS

LanguageLexicon language_lexicon(CodeLanguage language) noexcept {
    const char* keywords = "";
    const char* types = "";
    const char* builtins = "";
    const auto select = [&](const char* keyword_words, const char* type_words, const char* builtin_words) noexcept {
        keywords = keyword_words;
        types = type_words;
        builtins = builtin_words;
    };
    switch (language) {
    case CodeLanguage::c:
        select(words_c_keywords.data, words_c_types.data, words_c_builtins.data);
        break;
    case CodeLanguage::cpp:
        select(words_cpp_keywords.data, words_cpp_types.data, words_cpp_builtins.data);
        break;
    case CodeLanguage::csharp:
        select(words_csharp_keywords.data, words_csharp_types.data, words_csharp_builtins.data);
        break;
    case CodeLanguage::rust:
        select(words_rust_keywords.data, words_rust_types.data, words_rust_builtins.data);
        break;
    case CodeLanguage::javascript:
    case CodeLanguage::typescript:
        select(words_typescript_keywords.data, language == CodeLanguage::typescript
                    ? words_typescript_types.data
                    : words_javascript_types.data, words_typescript_builtins.data);
        break;
    case CodeLanguage::java:
        select(words_java_keywords.data, words_java_types.data, words_java_builtins.data);
        break;
    case CodeLanguage::python:
        select(words_python_keywords.data, words_python_types.data, words_python_builtins.data);
        break;
    case CodeLanguage::powershell:
        select(words_powershell_keywords.data, words_powershell_types.data, words_powershell_builtins.data);
        break;
    case CodeLanguage::shell:
        keywords = words_shell_keywords.data;
        builtins =
            words_shell_builtins.data;
        break;
    case CodeLanguage::hlsl:
    case CodeLanguage::glsl:
        select(words_glsl_keywords.data, words_glsl_types.data, words_glsl_builtins.data);
        break;
    case CodeLanguage::sql:
        select(words_sql_keywords.data, words_sql_types.data, words_sql_builtins.data);
        break;
    case CodeLanguage::markup:
        builtins = words_markup_builtins.data;
        break;
    case CodeLanguage::css:
        select(words_css_keywords.data, words_css_types.data, words_css_builtins.data);
        break;
    default:
        break;
    }
    return LanguageLexicon{keywords, types, builtins,
                           language == CodeLanguage::powershell || language == CodeLanguage::sql ||
                               language == CodeLanguage::markup};
}

bool token_in_words(Utf8View token, const char* words, bool ignore_case) noexcept {
    const auto fold = [ignore_case](char c) noexcept { return ignore_case ? ascii_lower(c) : c; };
    u32 offset = 0;
    while (words[offset]) {
        const u32 bytes = static_cast<u8>(words[offset++]);
        const u32 begin = offset;
        offset += bytes;
        if (bytes == token.bytes) {
            u32 index = 0;
            while (index < bytes && fold(token.data[index]) == fold(words[begin + index]))
                ++index;
            if (index == bytes)
                return true;
        }
    }
    return false;
}

u32 keyword_style(CodeLanguage language, Utf8View token) noexcept {
    const LanguageLexicon lexicon = language_lexicon(language);
    if (token_in_words(token, words_common.data,
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
    if (style == 0) {
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
    const Utf8View code = highlighter->code;
    u32 offset = 0;
    while (offset < code.bytes && !highlighter->truncated) {
        const char value = code.data[offset];
        u32 end = offset + 1, style = text_style_syntax_operator;
        if (offset + 4 <= code.bytes && value == '<' && code.data[offset + 1] == '!' &&
            code.data[offset + 2] == '-' && code.data[offset + 3] == '-') {
            end = offset + 4;
            while (end + 2 < code.bytes &&
                   !(code.data[end] == '-' && code.data[end + 1] == '-' && code.data[end + 2] == '>')) {
                ++end;
            }
            end = end + 2 < code.bytes ? end + 3 : code.bytes;
            style = text_style_syntax_comment;
        } else if (value == '"' || value == '\'') {
            end = quoted_end(code, offset, value);
            style = text_style_syntax_string;
        } else if (value == '<' || value == '>' || value == '/' || value == '=') {
            // One-byte operator: end and style already hold.
        } else if (identifier_begin(value)) {
            while (end < code.bytes && (identifier_continue(code.data[end]) || code.data[end] == '-')) {
                ++end;
            }
            style = keyword_style(CodeLanguage::markup, Utf8View{code.data + offset, end - offset});
        } else {
            ++offset;
            continue;
        }
        if (!mark(highlighter, offset, end, style)) {
            break;
        }
        offset = end;
    }
    return CodeHighlightResult{highlighter->tokens, highlighter->styled_bytes, highlighter->truncated};
}

} // namespace

CodeLanguage code_language_from_tag(Utf8View tag) noexcept {
    if ((!tag.data && tag.bytes != 0) || tag.bytes > 32) {
        return CodeLanguage::none;
    }
    while (tag.bytes != 0 && tag.data[tag.bytes - 1] == '\r') {
        --tag.bytes;
    }
    struct Alias { const char* name; u8 bytes; CodeLanguage language; };
    static constexpr Alias aliases[]{
        {"c", 1, CodeLanguage::c},
        {"cpp", 3, CodeLanguage::cpp},
        {"c++", 3, CodeLanguage::cpp},
        {"cc", 2, CodeLanguage::cpp},
        {"cxx", 3, CodeLanguage::cpp},
        {"cs", 2, CodeLanguage::csharp},
        {"csharp", 6, CodeLanguage::csharp},
        {"rs", 2, CodeLanguage::rust},
        {"rust", 4, CodeLanguage::rust},
        {"js", 2, CodeLanguage::javascript},
        {"javascript", 10, CodeLanguage::javascript},
        {"jsx", 3, CodeLanguage::javascript},
        {"ts", 2, CodeLanguage::typescript},
        {"typescript", 10, CodeLanguage::typescript},
        {"tsx", 3, CodeLanguage::typescript},
        {"java", 4, CodeLanguage::java},
        {"json", 4, CodeLanguage::json},
        {"jsonc", 5, CodeLanguage::json},
        {"py", 2, CodeLanguage::python},
        {"python", 6, CodeLanguage::python},
        {"ps1", 3, CodeLanguage::powershell},
        {"powershell", 10, CodeLanguage::powershell},
        {"sh", 2, CodeLanguage::shell},
        {"bash", 4, CodeLanguage::shell},
        {"zsh", 3, CodeLanguage::shell},
        {"shell", 5, CodeLanguage::shell},
        {"hlsl", 4, CodeLanguage::hlsl},
        {"glsl", 4, CodeLanguage::glsl},
        {"vert", 4, CodeLanguage::glsl},
        {"frag", 4, CodeLanguage::glsl},
        {"sql", 3, CodeLanguage::sql},
        {"html", 4, CodeLanguage::markup},
        {"xml", 3, CodeLanguage::markup},
        {"css", 3, CodeLanguage::css},
        {"ansi", 4, CodeLanguage::ansi},
    };
    for (const auto& alias : aliases)
        if (tag.bytes == alias.bytes && tag_equal(tag, alias.name, alias.bytes))
            return alias.language;
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
        const bool at_line_start = line_start;
        line_start = false;
        const bool has_next = offset + 1 < code.bytes;
        u32 end = offset + 1, style = text_style_syntax_comment;
        if (preprocessor_language(language) && at_line_start && value == '#') {
            end = line_end(code, offset);
            style = text_style_syntax_preprocessor;
        } else if (c_family(language) && has_next && value == '/' && code.data[offset + 1] == '/') {
            end = line_end(code, offset);
        } else if ((c_family(language) || language == CodeLanguage::sql) && has_next && value == '/' &&
                   code.data[offset + 1] == '*') {
            end = block_end(code, offset + 2, '*', '/');
        } else if (hash_comment_language(language) && value == '#') {
            end = line_end(code, offset);
        } else if (dash_comment_language(language) && has_next && value == '-' && code.data[offset + 1] == '-') {
            end = line_end(code, offset);
        } else if (value == '"' || value == '\'' ||
                   (value == '`' &&
                    (language == CodeLanguage::javascript || language == CodeLanguage::typescript ||
                     language == CodeLanguage::shell || language == CodeLanguage::powershell))) {
            end = quoted_end(code, offset, value);
            style = text_style_syntax_string;
        } else if (ascii_is_digit(value) || (value == '.' && has_next && ascii_is_digit(code.data[offset + 1]))) {
            end = number_end(code, offset);
            style = text_style_syntax_number;
        } else if (identifier_begin(value)) {
            while (end < code.bytes &&
                   (identifier_continue(code.data[end]) ||
                    (code.data[end] == '-' &&
                     (language == CodeLanguage::powershell || language == CodeLanguage::css)))) {
                ++end;
            }
            style = keyword_style(language, Utf8View{code.data + offset, end - offset});
        } else if (operator_character(value)) {
            if (end < code.bytes && operator_character(code.data[end])) {
                ++end;
            }
            style = text_style_syntax_operator;
        } else {
            ++offset;
            continue;
        }
        if (!mark(&highlighter, offset, end, style)) {
            break;
        }
        offset = end;
    }
    return CodeHighlightResult{highlighter.tokens, highlighter.styled_bytes, highlighter.truncated};
}

} // namespace maicord
