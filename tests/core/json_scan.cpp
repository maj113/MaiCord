#include "../support/test_main_shared.hpp"

#include "maicord/core/json_scan.hpp"

namespace maicord {
namespace {

bool json_is_structural(char value) noexcept {
    return value == '{' || value == '}' || value == '[' || value == ']' || value == ':' || value == ',';
}

u32 scalar_json_offsets(Utf8View input, u32* output, u32 capacity) noexcept {
    bool inside_string = false;
    u32 count = 0;
    u32 backslashes = 0;
    for (u32 offset = 0; offset < input.bytes; ++offset) {
        const char value = input.data[offset];
        if (value == '"' && (backslashes & 1u) == 0) {
            if (count < capacity) {
                output[count] = offset;
            }
            ++count;
            inside_string = !inside_string;
        } else if (!inside_string && json_is_structural(value)) {
            if (count < capacity) {
                output[count] = offset;
            }
            ++count;
        }
        backslashes = value == '\\' ? backslashes + 1 : 0;
    }
    return count;
}

bool json_matches_scalar(Utf8View input) noexcept {
    u32 expected[128]{};
    u32 actual[128]{};
    const u32 expected_count = scalar_json_offsets(input, expected, 128);
    const JsonScanResult result = json_scan_structurals_avx1(input, Slice<u32>{actual, 128});
    bool matches = result.ok() && result.count == expected_count;
    for (u32 index = 0; index < expected_count && index < 128 && matches; ++index) {
        matches = actual[index] == expected[index];
    }
    return matches;
}

void expect_json_matches_scalar(Utf8View input, const char* name) noexcept {
    expect(json_matches_scalar(input), name);
}

} // namespace

void test_json_scan() noexcept {
    expect_json_matches_scalar(utf8_literal(R"({"a":"x,}","b":"\\\"","c":[1,2]})"),
                               "json_scan_strings_and_escapes");
    expect_json_matches_scalar(utf8_literal(R"([{"nested":{"text":"[,{}:]"}},null,false])"),
                               "json_scan_ignores_string_punctuation");

    char even_cross_block[17]{};
    even_cross_block[0] = '"';
    for (u32 index = 1; index < 14; ++index) {
        even_cross_block[index] = 'a';
    }
    even_cross_block[14] = '\\';
    even_cross_block[15] = '\\';
    even_cross_block[16] = '"';
    expect_json_matches_scalar(Utf8View{even_cross_block, 17}, "json_scan_even_escape_run_crosses_block");

    char odd_cross_block[18]{};
    odd_cross_block[0] = '"';
    for (u32 index = 1; index < 13; ++index) {
        odd_cross_block[index] = 'a';
    }
    odd_cross_block[13] = '\\';
    odd_cross_block[14] = '\\';
    odd_cross_block[15] = '\\';
    odd_cross_block[16] = '"';
    odd_cross_block[17] = '"';
    expect_json_matches_scalar(Utf8View{odd_cross_block, 18}, "json_scan_odd_escape_run_crosses_block");

    bool boundary_matrix_matches = true;
    for (u32 quote_offset = 1; quote_offset <= 63 && boundary_matrix_matches; ++quote_offset) {
        const u32 maximum_slashes = quote_offset - 1 < 24 ? quote_offset - 1 : 24;
        for (u32 slash_count = 0; slash_count <= maximum_slashes; ++slash_count) {
            char text[66]{};
            text[0] = '"';
            const u32 slash_begin = quote_offset - slash_count;
            for (u32 index = 1; index < slash_begin; ++index) {
                text[index] = 'a';
            }
            for (u32 index = slash_begin; index < quote_offset; ++index) {
                text[index] = '\\';
            }
            text[quote_offset] = '"';
            u32 text_bytes = quote_offset + 1;
            if ((slash_count & 1u) != 0) {
                text[text_bytes++] = '"';
            }
            if (!json_matches_scalar(Utf8View{text, text_bytes})) {
                boundary_matrix_matches = false;
                break;
            }
        }
    }
    expect(boundary_matrix_matches, "json_scan_escape_boundary_matrix");

    u32 offsets[8]{};
    const JsonScanResult empty = json_scan_structurals_avx1(Utf8View{nullptr, 0}, Slice<u32>{nullptr, 0});
    expect(empty.ok() && empty.count == 0, "json_scan_empty");

    const JsonScanResult bad_input =
        json_scan_structurals_avx1(Utf8View{nullptr, 1}, Slice<u32>{offsets, 8});
    expect(bad_input.error == JsonScanError::invalid_input, "json_scan_invalid_input");
    const JsonScanResult bad_output =
        json_scan_structurals_avx1(utf8_literal("{}"), Slice<u32>{nullptr, 1});
    expect(bad_output.error == JsonScanError::invalid_input, "json_scan_invalid_output");

    const JsonScanResult overflow = json_scan_structurals_avx1(utf8_literal("{}"), Slice<u32>{offsets, 1});
    expect(overflow.error == JsonScanError::output_overflow && overflow.error_offset == 1 && offsets[0] == 0,
           "json_scan_output_bound");

    const char control_text[] = {'[', '"', 'a', 0x1F, '"', ']'};
    const JsonScanResult control = json_scan_structurals_avx1(
        Utf8View{control_text, static_cast<u32>(sizeof(control_text))}, Slice<u32>{offsets, 8});
    expect(control.error == JsonScanError::control_in_string && control.error_offset == 3,
           "json_scan_control_in_string");

    const JsonScanResult unterminated =
        json_scan_structurals_avx1(utf8_literal("\"abc"), Slice<u32>{offsets, 8});
    expect(unterminated.error == JsonScanError::unterminated_string && unterminated.error_offset == 4,
           "json_scan_unterminated_string");

    // Random quote/backslash/punctuation/control mixes across chunk boundaries
    // against the byte-serial reference: counts, errors and error offsets match,
    // and so do the offsets stored before an error.
    u32 stored = 0;
    const auto reference = [&stored](const char* text, u32 bytes, u32* output, u32 capacity) noexcept {
        u32& count = stored;
        count = 0;
        u32 backslashes = 0;
        bool inside = false;
        for (u32 offset = 0; offset < bytes; ++offset) {
            const u8 value = static_cast<u8>(text[offset]);
            const bool quote = value == '"' && (backslashes & 1u) == 0;
            if (quote || (!inside && json_is_structural(static_cast<char>(value)))) {
                if (count == capacity) return JsonScanResult{offset, JsonScanError::output_overflow};
                output[count++] = offset;
                inside = quote ? !inside : inside;
            } else if (inside && value < 0x20) {
                return JsonScanResult{offset, JsonScanError::control_in_string};
            }
            backslashes = value == '\\' ? backslashes + 1 : 0;
        }
        return inside ? JsonScanResult{bytes, JsonScanError::unterminated_string} : JsonScanResult{count, JsonScanError::none};
    };
    constexpr char alphabet[] = {'"', '\\', '\\', '{', '}', '[', ']', ':', ',', 'a', ' ', '\x01', '\xC3', '"'};
    char text[320];
    u32 expected[200], actual[200];
    u64 seed = 0x243F6A8885A308D3ull;
    bool differential = true;
    for (u32 round = 0; round < 6000 && differential; ++round) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        const u32 bytes = static_cast<u32>(seed >> 33) % sizeof(text);
        const u32 controls = round % 4 == 0 ? 14u : 11u;
        for (u32 index = 0; index < bytes; ++index) {
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            const u32 pick = static_cast<u32>(seed >> 40) % 24;
            text[index] = pick < controls ? alphabet[pick] : 'a';
        }
        const u32 capacity = round % 3 == 0 ? static_cast<u32>(seed >> 20) % 48 : 200u;
        const JsonScanResult want = reference(text, bytes, expected, capacity);
        const JsonScanResult got = json_scan_structurals_avx1(Utf8View{text, bytes}, Slice<u32>{actual, capacity});
        differential = want.count == got.count && want.error == got.error;
        for (u32 index = 0; index < stored && differential; ++index) differential = expected[index] == actual[index];
    }
    expect(differential, "json_scan_differential");
}

} // namespace maicord
