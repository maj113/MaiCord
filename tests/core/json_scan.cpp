#include "../support/test_main_shared.hpp"

#include "maicord/core/json_scan.hpp"

namespace maicord {
namespace {

bool json_is_structural(char value) noexcept {
    return value == '{' || value == '}' || value == '[' || value == ']' || value == ':' || value == ',';
}

// The byte-serial reference: the offsets it stored before the end or the first error (*stored), and the count or
// that error with its offset.
JsonScanResult reference_scan(const char* text, u32 bytes, u32* output, u32 capacity, u32* stored) noexcept {
    u32& count = *stored;
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
}

// The scanner and the reference agree: the count or the error with its offset, and every offset stored. The scanner
// writes nothing past `capacity` (at most 320): the 80 words after it keep their guard value.
bool matches_reference(const char* text, u32 bytes, u32 capacity) noexcept {
    u32 expected[320], actual[320 + 80], stored = 0;
    for (u32& word : actual) word = 0xA5A5A5A5u;
    const JsonScanResult want = reference_scan(text, bytes, expected, capacity, &stored);
    const JsonScanResult got = json_scan_structurals_avx1(Utf8View{text, bytes}, Slice<u32>{actual, capacity});
    bool same = want.count == got.count && want.error == got.error;
    for (u32 index = 0; index < stored && same; ++index)
        same = expected[index] == actual[index];
    for (u32 index = capacity; index < capacity + 80 && same; ++index)
        same = actual[index] == 0xA5A5A5A5u;
    return same;
}

} // namespace

void test_json_scan() noexcept {
    // Runs of up to 24 backslashes before a quote at each offset of the first 64 bytes, across block edges.
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
            if (!matches_reference(text, text_bytes, 200)) {
                boundary_matrix_matches = false;
                break;
            }
        }
    }
    expect(boundary_matrix_matches, "json_scan_escape_boundary_matrix");

    u32 offsets[1]{};
    const JsonScanResult empty = json_scan_structurals_avx1(Utf8View{nullptr, 0}, Slice<u32>{nullptr, 0});
    expect(empty.ok() && empty.count == 0, "json_scan_empty");

    const JsonScanResult bad_input =
        json_scan_structurals_avx1(Utf8View{nullptr, 1}, Slice<u32>{offsets, 1});
    expect(bad_input.error == JsonScanError::invalid_input, "json_scan_invalid_input");
    const JsonScanResult bad_output =
        json_scan_structurals_avx1(utf8_literal("{}"), Slice<u32>{nullptr, 1});
    expect(bad_output.error == JsonScanError::invalid_input, "json_scan_invalid_output");

    // Random quote/backslash/punctuation/control mixes across chunk boundaries, some into a small output, against
    // the reference: overflow, control characters and unterminated strings at their offsets included.
    constexpr char alphabet[] = {'"', '\\', '\\', '{', '}', '[', ']', ':', ',', 'a', ' ', '\x01', '\xC3', '"'};
    char text[320];
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
        differential = matches_reference(text, bytes, capacity);
    }
    expect(differential, "json_scan_differential");

    // Every output capacity of a text whose every byte is structural (the most entries per 64-byte block) and of one
    // with quotes and commas around string edges.
    char dense[300], quotes[300];
    for (u32 index = 0; index < 300; ++index) {
        dense[index] = "[{,:"[index & 3];
        quotes[index] = index % 5 == 0 ? '"' : index % 5 == 3 ? ',' : 'x';
    }
    bool capacities = true;
    for (u32 capacity = 0; capacity <= 320 && capacities; ++capacity)
        capacities = matches_reference(dense, 300, capacity) && matches_reference(quotes, 300, capacity);
    expect(capacities, "json_scan_fills_every_output_capacity_exactly");
}

} // namespace maicord
