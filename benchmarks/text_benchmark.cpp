#include "maicord/core/time.hpp"
#include "maicord/render/message_hit_test.hpp"
#include "maicord/ui/message_text_presentation.hpp"
#include "maicord/ui/text.hpp"
#include "benchmark_output.hpp"

#include <Windows.h>

namespace maicord {
namespace {

inline constexpr u32 kTextBenchmarkIterations = 20'000;
inline constexpr u32 kMessageHitRebuildIterations = 1'000;
inline constexpr char kArabicText[] =
    "MaiCord \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 "
    "\xD8\xA8\xD8\xA7\xD9\x84\xD8\xAC\xD9\x85\xD9\x8A\xD8\xB9";
inline constexpr char kEmojiText[] =
    "emoji \xF0\x9F\x98\x80 \xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x92\xBB reactions";

wchar_t g_utf16[512];
TextGlyph g_glyphs[512];
TextRun g_runs[64];
u16 g_clusters[512];
TextDecoration g_decorations[128];
TextHitRegion g_hit_regions[128];
volatile u64 g_text_checksum = 0;

void write_metric(const char* name, u64 value) noexcept {
    write_text(name);
    write_text("=");
    write_u64(value);
    write_text(" runs/s\r\n");
}

void write_value(const char* name, u64 value) noexcept {
    write_text(name);
    write_text("=");
    write_u64(value);
    write_text("\r\n");
}

u64 message_hit_cached_per_second(MessageHitTestCacheOwner* owner,
                                  const MessageTextPresentation& presentation,
                                  const TextSystem& system, const Clock& clock) noexcept {
    const i64 begin = clock_now();
    u64 checksum = 0;
    for (u32 iteration = 0; iteration < kTextBenchmarkIterations; ++iteration) {
        const MessagePositionHit hit = message_position_hit_test(
            owner, presentation, system, 96, 12 + static_cast<i32>(iteration & 7u), 18);
        if (!hit.valid) {
            return 0;
        }
        checksum += hit.byte_offset + hit.command_index + hit.message_id;
    }
    const u64 elapsed = clock_microseconds(clock, begin, clock_now());
    g_text_checksum ^= checksum;
    return elapsed == 0 ? 0 : static_cast<u64>(kTextBenchmarkIterations) * 1'000'000ull / elapsed;
}

u64 message_hit_rebuild_per_second(MessageHitTestCacheOwner* owner,
                                   const MessageTextPresentation& presentation,
                                   const TextSystem& system, const Clock& clock) noexcept {
    const i64 begin = clock_now();
    u64 checksum = 0;
    for (u32 iteration = 0; iteration < kMessageHitRebuildIterations; ++iteration) {
        message_hit_test_cache_invalidate(owner);
        const MessagePositionHit hit = message_position_hit_test(
            owner, presentation, system, 96, 12 + static_cast<i32>(iteration & 7u), 18);
        if (!hit.valid) {
            return 0;
        }
        checksum += hit.byte_offset + hit.command_index + hit.message_id;
    }
    const u64 elapsed = clock_microseconds(clock, begin, clock_now());
    g_text_checksum ^= checksum;
    return elapsed == 0
               ? 0
               : static_cast<u64>(kMessageHitRebuildIterations) * 1'000'000ull / elapsed;
}

u64 shape_per_second(TextSystem* system, ShapedText* shaped, Utf8View input,
                     const Clock& clock) noexcept {
    const i64 begin = clock_now();
    u64 checksum = 0;
    for (u32 iteration = 0; iteration < kTextBenchmarkIterations; ++iteration) {
        const TextShapeResult result = text_shape_utf8(
            system, input, 640.0f, 200.0f, Slice<wchar_t>{g_utf16, 512}, shaped);
        if (!result.ok()) {
            return 0;
        }
        checksum += shaped->glyph_count + shaped->run_count + shaped->line_count;
        if (shaped->glyph_count != 0) {
            checksum += shaped->glyphs[iteration % shaped->glyph_count].index;
        }
    }
    const u64 elapsed = clock_microseconds(clock, begin, clock_now());
    g_text_checksum = g_text_checksum ^ checksum;
    return elapsed == 0
               ? 0
               : (static_cast<u64>(kTextBenchmarkIterations) * 1'000'000ull) / elapsed;
}

u64 measure_per_second(TextSystem* system, Utf8View input, const Clock& clock) noexcept {
    const i64 begin = clock_now();
    u64 checksum = 0;
    for (u32 iteration = 0; iteration < kTextBenchmarkIterations; ++iteration) {
        const TextMeasureResult result = text_measure_utf8_styled(
            system, input, {}, 640.0f, 200.0f, Slice<wchar_t>{g_utf16, 512});
        if (!result.ok()) {
            return 0;
        }
        checksum += result.line_count + static_cast<u64>(result.width);
    }
    const u64 elapsed = clock_microseconds(clock, begin, clock_now());
    g_text_checksum = g_text_checksum ^ checksum;
    return elapsed == 0
               ? 0
               : (static_cast<u64>(kTextBenchmarkIterations) * 1'000'000ull) / elapsed;
}

// Alternating inputs defeat the single-entry paragraph cache, so every call
// shapes. Returns thread cycles per call.
u64 alternating_shape_cycles(TextSystem* system, ShapedText* shaped, Utf8View first,
                             Utf8View second) noexcept {
    constexpr u32 iterations = 4'096;
    ULONG64 begin = 0, end = 0;
    u64 checksum = 0;
    QueryThreadCycleTime(GetCurrentThread(), &begin);
    for (u32 iteration = 0; iteration < iterations; ++iteration) {
        const TextShapeResult result = text_shape_utf8(system, (iteration & 1) ? second : first, 640.0f,
                                                       200.0f, Slice<wchar_t>{g_utf16, 512}, shaped);
        if (!result.ok() || shaped->glyph_count == 0) {
            return 0;
        }
        checksum += shaped->glyph_count + shaped->glyphs[iteration % shaped->glyph_count].index;
    }
    QueryThreadCycleTime(GetCurrentThread(), &end);
    g_text_checksum ^= checksum;
    return (end - begin) / iterations;
}

u64 styled_shape_per_second(TextSystem* system, ShapedText* shaped,
                            const Clock& clock) noexcept {
    constexpr Utf8View input = utf8_literal(
        "Heading bold italic code mention link strike spoiler quote");
    constexpr TextStyleSpan styles[] = {
        {0, 7, text_style_heading_1 | text_style_bold},
        {8, 4, text_style_bold},
        {13, 6, text_style_italic},
        {20, 4, text_style_monospace},
        {25, 7, text_style_mention},
        {33, 4, text_style_link | text_style_underline},
        {38, 6, text_style_strikethrough},
        {45, 7, text_style_spoiler},
        {53, 5, text_style_quote},
    };
    const i64 begin = clock_now();
    u64 checksum = 0;
    for (u32 iteration = 0; iteration < kTextBenchmarkIterations;
         ++iteration) {
        const TextShapeResult result = text_shape_utf8_styled(
            system, input, ConstSlice<TextStyleSpan>{styles, 9}, 640.0f,
            200.0f, Slice<wchar_t>{g_utf16, 512}, shaped);
        if (!result.ok()) {
            return 0;
        }
        checksum += shaped->glyph_count + shaped->run_count +
                    shaped->decoration_count + shaped->line_count;
        checksum += shaped->runs[iteration % shaped->run_count].style_flags;
    }
    const u64 elapsed = clock_microseconds(clock, begin, clock_now());
    g_text_checksum ^= checksum;
    return elapsed == 0
               ? 0
               : (static_cast<u64>(kTextBenchmarkIterations) * 1'000'000ull) /
                     elapsed;
}

u64 interactive_shape_per_second(TextSystem* system, ShapedText* shaped,
                                 const Clock& clock) noexcept {
    constexpr Utf8View input = utf8_literal(
        "Mai performance native timestamp external spoiler interaction");
    constexpr TextStyleSpan styles[] = {
        {0, 3, text_style_mention},
        {4, 11, text_style_mention},
        {16, 6, text_style_mention},
        {23, 9, text_style_timestamp | text_style_link},
        {33, 8, text_style_link | text_style_underline},
        {42, 7, text_style_spoiler},
    };
    constexpr TextHitSpan hits[] = {
        {0, 3, 0, 0}, {4, 11, 1, 0}, {16, 6, 2, 0},
        {23, 9, 3, 0}, {33, 8, 4, 0}, {42, 7, 5, 0},
    };
    const i64 begin = clock_now();
    u64 checksum = 0;
    for (u32 iteration = 0; iteration < kTextBenchmarkIterations;
         ++iteration) {
        const TextShapeResult result = text_shape_utf8_interactive(
            system, input, ConstSlice<TextStyleSpan>{styles, 6},
            ConstSlice<TextHitSpan>{hits, 6}, 320.0f, 200.0f,
            Slice<wchar_t>{g_utf16, 512}, shaped);
        if (!result.ok() || shaped->hit_region_count < 6) {
            return 0;
        }
        checksum += shaped->glyph_count + shaped->run_count +
                    shaped->decoration_count + shaped->hit_region_count;
        checksum += shaped->hit_regions[
            iteration % shaped->hit_region_count]
                        .semantic_index;
    }
    const u64 elapsed = clock_microseconds(clock, begin, clock_now());
    g_text_checksum ^= checksum;
    return elapsed == 0
               ? 0
               : static_cast<u64>(kTextBenchmarkIterations) * 1'000'000ull /
                     elapsed;
}

int run() noexcept {
    TextSystem system{};
    if (!text_system_initialize(&system, L"Segoe UI", 14.0f, L"en-us").ok()) {
        return 1;
    }
    ShapedText shaped{};
    if (!shaped_text_initialize(&shaped, Slice<TextGlyph>{g_glyphs, 512},
                                Slice<TextRun>{g_runs, 64}, Slice<u16>{g_clusters, 512}) ||
        !shaped_text_bind_decorations(
            &shaped, Slice<TextDecoration>{g_decorations, 128}) ||
        !shaped_text_bind_hit_regions(
            &shaped, Slice<TextHitRegion>{g_hit_regions, 128})) {
        text_system_release(&system);
        return 2;
    }

    const Clock clock = clock_initialize();
    const u64 latin = shape_per_second(
        &system, &shaped,
        utf8_literal("MaiCord shapes office, @mentions, #channels, timestamps, and reactions."),
        clock);
    const u64 arabic =
        shape_per_second(&system, &shaped, utf8_literal(kArabicText), clock);
    const u64 emoji = shape_per_second(&system, &shaped, utf8_literal(kEmojiText), clock);
    const u64 styled = styled_shape_per_second(&system, &shaped, clock);
    const u64 interactive =
        interactive_shape_per_second(&system, &shaped, clock);
    const u64 measure = measure_per_second(
        &system,
        utf8_literal("MaiCord shapes office, @mentions, #channels, timestamps, and reactions."),
        clock);

    struct Pair {
        const char* name;
        Utf8View first, second;
    };
    const Pair pairs[] = {
        {"cycles_latin", utf8_literal("Caf\xc3\xa9, fianc\xc3\xa9" "e, na\xc3\xafve, r\xc3\xa9sum\xc3\xa9. AV To office."),
         utf8_literal("R\xc3\xa9sum\xc3\xa9, caf\xc3\xa9, na\xc3\xafve, fianc\xc3\xa9" "e. To AV office.")},
        {"cycles_arabic",
         utf8_literal("\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xd8\xa8\xd8\xa7\xd9\x84\xd8\xac\xd9\x85\xd9\x8a\xd8\xb9 "
                      "\xd9\x81\xd9\x8a \xd8\xa7\xd9\x84\xd9\x82\xd9\x86\xd8\xa7\xd8\xa9"),
         utf8_literal("\xd8\xa7\xd9\x84\xd9\x82\xd9\x86\xd8\xa7\xd8\xa9 \xd8\xaa\xd8\xb1\xd8\xad\xd8\xa8 "
                      "\xd8\xa8\xd8\xa7\xd9\x84\xd8\xac\xd9\x85\xd9\x8a\xd8\xb9")},
        {"cycles_devanagari",
         utf8_literal("\xe0\xa4\xb9\xe0\xa4\xbf\xe0\xa4\xa8\xe0\xa5\x8d\xe0\xa4\xa6\xe0\xa5\x80 "
                      "\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\xb7\xe0\xa5\x87\xe0\xa4\xa4\xe0\xa5\x8d\xe0\xa4\xb0 "
                      "\xe0\xa4\xb0\xe0\xa5\x8d\xe0\xa4\x95\xe0\xa4\xbf"),
         utf8_literal("\xe0\xa4\xb0\xe0\xa5\x8d\xe0\xa4\x95\xe0\xa4\xbf "
                      "\xe0\xa4\xb9\xe0\xa4\xbf\xe0\xa4\xa8\xe0\xa5\x8d\xe0\xa4\xa6\xe0\xa5\x80 "
                      "\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\xb7\xe0\xa5\x87\xe0\xa4\xa4\xe0\xa5\x8d\xe0\xa4\xb0")},
    };
    for (const auto& pair : pairs) {
        const u64 cycles = alternating_shape_cycles(&system, &shaped, pair.first, pair.second);
        if (cycles == 0) {
            return 4;
        }
        write_value(pair.name, cycles);
    }
    constexpr Utf8View message_hit_text = utf8_literal(
        "Retained native selection layout with bold and emoji \xF0\x9F\x98\x80");
    constexpr TextStyleSpan message_hit_styles[] = {
        {9, 11, text_style_bold},
        {21, 9, text_style_italic},
    };
    MessageTextCommand message_hit_command{};
    message_hit_command.text = message_hit_text;
    message_hit_command.styles = ConstSlice<TextStyleSpan>{message_hit_styles, 2};
    message_hit_command.rectangle = UiRect{0.0f, 0.0f, 640.0f, 64.0f};
    message_hit_command.clip = message_hit_command.rectangle;
    message_hit_command.message_id = 73;
    message_hit_command.measurement_identity = 0xF13A'9C84'EE21'084Bull;
    message_hit_command.message_revision = 4;
    message_hit_command.measurement_kind = MessageTextMeasurementKind::content;
    message_hit_command.horizontal_alignment = UiTextAlignment::start;
    message_hit_command.vertical_alignment = UiTextAlignment::start;
    message_hit_command.wrapping = UiTextWrapping::wrap;
    MessageTextPresentation message_hit_presentation{};
    message_hit_presentation.commands = Slice<MessageTextCommand>{&message_hit_command, 1};
    message_hit_presentation.command_count = 1;
    message_hit_presentation.active = true;
    MessageHitTestCacheOwner message_hit_owner{};
    const u64 message_hit_cached = message_hit_cached_per_second(
        &message_hit_owner, message_hit_presentation, system, clock);
    const u32 builds_after_cached = message_hit_owner.builds;
    const u32 hits_after_cached = message_hit_owner.hits;
    const u64 message_hit_rebuild = message_hit_rebuild_per_second(
        &message_hit_owner, message_hit_presentation, system, clock);
    const u32 builds_after_rebuild = message_hit_owner.builds;
    const u32 rejected_after_rebuild = message_hit_owner.rejected;
    message_hit_test_cache_release(&message_hit_owner);
    shaped_text_reset(&shaped);
    text_system_release(&system);
    if (latin == 0 || arabic == 0 || emoji == 0 || styled == 0 ||
        interactive == 0 || measure == 0 || message_hit_cached == 0 ||
        message_hit_rebuild == 0 || builds_after_cached != 1 ||
        hits_after_cached != kTextBenchmarkIterations - 1 ||
        builds_after_rebuild != 1 + kMessageHitRebuildIterations ||
        rejected_after_rebuild != 0 || g_text_checksum == 0) {
        return 3;
    }
    write_metric("text_shape_latin", latin);
    write_metric("text_shape_arabic", arabic);
    write_metric("text_shape_emoji", emoji);
    write_metric("text_shape_styled", styled);
    write_metric("text_shape_interactive", interactive);
    write_metric("text_measure_latin", measure);
    write_metric("message_hit_cached", message_hit_cached);
    write_metric("message_hit_rebuild", message_hit_rebuild);
    write_value("message_hit_cache_owner_bytes", sizeof(MessageHitTestCacheOwner));
    write_value("message_hit_cached_speedup_x1000",
                message_hit_rebuild == 0 ? 0 : message_hit_cached * 1'000ull / message_hit_rebuild);
    return 0;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_text_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
