#include "maicord/core/time.hpp"
#include "maicord/ui/code_highlight.hpp"
#include "maicord/ui/message_markdown.hpp"
#include "maicord/ui/message_system_text.hpp"
#include "maicord/ui/timestamp_format.hpp"
#include "benchmark_output.hpp"

#include <Windows.h>

namespace maicord {
namespace {

inline constexpr u32 kIterations = 1'000'000;
// Windows locale/time-zone APIs and per-message system lines.
inline constexpr u32 kApiIterations = 200'000;
inline constexpr Utf8View kPlain = utf8_literal(
    "A retained message with Unicode: \xCE\x9A\xCE\xB1\xCE\xBB\xCE\xB7\xCE\xBC\xCE\xAD\xCF\x81\xCE\xB1 / "
    "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C and no formatting controls.");
inline constexpr Utf8View kRich = utf8_literal(
    "# GPU markdown\n> **bold *nested***, __underline__, ~~strike~~, "
    "[link](https://discord.com), <@123>, <#456>, <@&789>, "
    "`inline code`, ||spoiler|| and ```cpp\nint value = 42;\n```");
inline constexpr Utf8View kSemantic = utf8_literal(
    "[link](https://discord.com), <@123>, <#456>, <@&789>, "
    "<t:1725021296:R>, and ||spoiler||");
inline constexpr Utf8View kAlias = utf8_literal(
    "Hello :smile: and :wave::skin-tone-2: from :not_an_emoji: at 12:30:45, :thumbsup: ok");
inline constexpr Utf8View kAnsi = utf8_literal(
    "```ansi\n\x1B[31mred\x1B[0m plain \x1B[1;32mgreen\x1B[0m and \x1B[38;5;208morange\x1B[0m\n```");

char g_output[512];
u32 g_byte_styles[512];
TextStyleSpan g_spans[kTextStyleSpanMaximum];
MessageMarkdownSemantic g_semantics[kMessageMarkdownSemanticMaximum];
volatile u64 g_checksum = 0;

u64 parse_per_second(Utf8View input, const Clock& clock,
                     u64* bytes_per_second, u32 iterations = kIterations) noexcept {
    const i64 begin = clock_now();
    u64 checksum = 0;
    for (u32 iteration = 0; iteration < iterations; ++iteration) {
        const MessageMarkdownResult result = message_markdown_parse(
            input, Slice<char>{g_output, 512},
            Slice<u32>{g_byte_styles, 512},
            Slice<TextStyleSpan>{g_spans, kTextStyleSpanMaximum});
        if (!result.ok()) {
            return 0;
        }
        checksum += result.text.bytes + result.span_count +
                    result.transformations + result.syntax_tokens +
                    result.list_items +
                    static_cast<u8>(g_output[iteration % result.text.bytes]);
    }
    const u64 elapsed = clock_microseconds(clock, begin, clock_now());
    g_checksum ^= checksum;
    *bytes_per_second = per_second(static_cast<u64>(input.bytes) * iterations, elapsed);
    return per_second(iterations, elapsed);
}

Utf8View resolve_entity(void*, MessageMarkdownEntityKind kind, u64) noexcept {
    switch (kind) {
    case MessageMarkdownEntityKind::user:
        return utf8_literal("Mai");
    case MessageMarkdownEntityKind::channel:
        return utf8_literal("performance");
    case MessageMarkdownEntityKind::role:
        return utf8_literal("Native");
    default:
        return Utf8View{};
    }
}

TimestampFormatResult format_timestamp(void*, u64, char,
                                              Slice<char> output) noexcept {
    constexpr Utf8View value = utf8_literal("2 years ago");
    if (!output.data || output.count < value.bytes) {
        return TimestampFormatResult{};
    }
    for (u32 index = 0; index < value.bytes; ++index) {
        output[index] = value.data[index];
    }
    return TimestampFormatResult{1, value.bytes, true};
}

u64 semantic_per_second(const Clock& clock, u64* bytes_per_second) noexcept {
    const MessageMarkdownOptions options{
        nullptr, resolve_entity, format_timestamp, nullptr, 0,
        MessageMarkdownSemanticKind::none, false};
    const i64 begin = clock_now();
    u64 checksum = 0;
    for (u32 iteration = 0; iteration < kIterations; ++iteration) {
        const MessageMarkdownResult result = message_markdown_parse(
            kSemantic, Slice<char>{g_output, 512},
            Slice<u32>{g_byte_styles, 512},
            Slice<TextStyleSpan>{g_spans, kTextStyleSpanMaximum}, options,
            Slice<MessageMarkdownSemantic>{
                g_semantics, kMessageMarkdownSemanticMaximum});
        if (!result.ok() || result.semantic_count != 6) {
            return 0;
        }
        checksum += result.text.bytes + result.span_count +
                    result.resolved_entities + result.timestamps +
                    g_semantics[iteration % result.semantic_count].source_offset;
    }
    const u64 elapsed = clock_microseconds(clock, begin, clock_now());
    g_checksum ^= checksum;
    *bytes_per_second = per_second(static_cast<u64>(kSemantic.bytes) * kIterations, elapsed);
    return per_second(kIterations, elapsed);
}

u64 highlight_per_second(const Clock& clock, u64* bytes_per_second) noexcept {
    constexpr Utf8View code = utf8_literal(
        "#include <cstdint>\nstruct Item { uint32_t id; };\n"
        "if (ready && item.id == 42) return \"gpu\"; // retained\n");
    const i64 begin = clock_now();
    u64 checksum = 0;
    for (u32 iteration = 0; iteration < kIterations; ++iteration) {
        const CodeHighlightResult result = code_highlight(
            code, CodeLanguage::cpp, Slice<u32>{g_byte_styles, 512},
            kCodeHighlightTokenMaximum);
        if (result.token_count == 0 || result.truncated) {
            return 0;
        }
        checksum += result.token_count + result.styled_bytes +
                    g_byte_styles[iteration % code.bytes];
    }
    const u64 elapsed = clock_microseconds(clock, begin, clock_now());
    g_checksum ^= checksum;
    *bytes_per_second = per_second(static_cast<u64>(code.bytes) * kIterations, elapsed);
    return per_second(kIterations, elapsed);
}

// A message list formats the same few timestamps on every projection build.
u64 timestamp_per_second(const Clock& clock, bool calendar) noexcept {
    constexpr u64 kNow = 1'725'021'296ull;
    char text[kMessageMarkdownTimestampMaximumBytes];
    const i64 begin = clock_now();
    u64 checksum = 0;
    for (u32 iteration = 0; iteration < kApiIterations; ++iteration) {
        const u64 stamp = kNow - 90'000ull + (iteration & 15u) * 7'000ull;
        const TimestampFormatResult result =
            calendar ? format_timestamp_calendar(stamp, kNow, Slice<char>{text, sizeof(text)})
                     : format_timestamp_local(stamp, 't', kNow, Slice<char>{text, sizeof(text)});
        if (!result.ok) {
            return 0;
        }
        checksum += result.bytes + static_cast<u8>(text[0]);
    }
    const u64 elapsed = clock_microseconds(clock, begin, clock_now());
    g_checksum ^= checksum;
    return per_second(kApiIterations, elapsed);
}

u64 system_text_per_second(const Clock& clock) noexcept {
    char text[kMessageSystemTextMaximumBytes];
    MessageSystemTextInput input{};
    input.author = utf8_literal("Mai");
    const i64 begin = clock_now();
    u64 checksum = 0;
    for (u32 iteration = 0; iteration < kApiIterations; ++iteration) {
        input.type = iteration & 1u ? message_type_channel_pinned_message : message_type_user_join;
        input.message_id = iteration * 0x400000ull;
        MessageSystemText result;
        if (!message_system_text_build(input, Slice<char>{text, sizeof(text)}, &result)) {
            return 0;
        }
        checksum += result.text.bytes + result.span_count + static_cast<u8>(result.icon);
    }
    const u64 elapsed = clock_microseconds(clock, begin, clock_now());
    g_checksum ^= checksum;
    return per_second(kApiIterations, elapsed);
}

int run() noexcept {
    const Clock clock = clock_initialize();
    u64 plain_bytes = 0;
    u64 rich_bytes = 0;
    u64 highlight_bytes = 0;
    u64 semantic_bytes = 0;
    u64 alias_bytes = 0;
    u64 ansi_bytes = 0;
    const u64 plain = parse_per_second(kPlain, clock, &plain_bytes);
    const u64 rich = parse_per_second(kRich, clock, &rich_bytes);
    const u64 highlight = highlight_per_second(clock, &highlight_bytes);
    const u64 semantic = semantic_per_second(clock, &semantic_bytes);
    const u64 alias = parse_per_second(kAlias, clock, &alias_bytes, kApiIterations);
    const u64 ansi = parse_per_second(kAnsi, clock, &ansi_bytes, kApiIterations);
    const u64 stamp = timestamp_per_second(clock, false);
    const u64 calendar = timestamp_per_second(clock, true);
    const u64 system = system_text_per_second(clock);
    if (plain == 0 || rich == 0 || highlight == 0 || plain_bytes == 0 ||
        rich_bytes == 0 || highlight_bytes == 0 || semantic == 0 ||
        semantic_bytes == 0 || alias == 0 || ansi == 0 || stamp == 0 || calendar == 0 || system == 0 ||
        g_checksum == 0) {
        return 1;
    }
    write_metric("markdown_plain", plain, "messages/s");
    write_metric("markdown_plain_bytes", plain_bytes, "bytes/s");
    write_metric("markdown_rich", rich, "messages/s");
    write_metric("markdown_rich_bytes", rich_bytes, "bytes/s");
    write_metric("markdown_alias", alias, "messages/s");
    write_metric("markdown_ansi", ansi, "messages/s");
    write_metric("timestamp_time", stamp, "ops/s");
    write_metric("timestamp_calendar", calendar, "ops/s");
    write_metric("system_text", system, "ops/s");
    write_metric("code_highlight_cpp", highlight, "blocks/s");
    write_metric("code_highlight_cpp_bytes", highlight_bytes, "bytes/s");
    write_metric("markdown_semantic", semantic, "messages/s");
    write_metric("markdown_semantic_bytes", semantic_bytes, "bytes/s");
    return 0;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI
maicord_markdown_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
