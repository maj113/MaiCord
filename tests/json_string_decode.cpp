// Differential coverage against the previous scalar implementation.
#include "../src/core/json_reader.cpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace json_string_probe {
bool reference_number(const JsonValue& value, u64* output) noexcept {
    if (!output || value.kind != JsonValueKind::number || value.source.bytes == 0) return false;
    u64 result = 0;
    for (u32 offset = 0; offset < value.source.bytes; ++offset) {
        const char current = value.source.data[offset];
        if (current < '0' || current > '9') return false;
        const u64 digit = static_cast<u64>(current - '0');
        if (result > (~0ull - digit) / 10) return false;
        result = result * 10 + digit;
    }
    *output = result;
    return true;
}
bool compare_number(Utf8View source) noexcept {
    // Nonempty null views are outside JsonValue's parsed-source contract.
    if (!source.data && source.bytes) return true;
    for (u32 kind = 0; kind <= static_cast<u32>(JsonValueKind::null_value); ++kind) {
        const JsonValue value{source, 0, static_cast<JsonValueKind>(kind)};
        u64 before = 0xa537ed4f10382acbull, after = before;
        if (reference_number(value, &before) != json_value_to_u64(value, &after) || before != after ||
            reference_number(value, nullptr) != json_value_to_u64(value, nullptr)) return false;
    }
    return true;
}
bool reference_integer(const JsonString& value, u64* output) noexcept {
    if (!output || (!value.encoded.data && value.encoded.bytes != 0)) return false;
    u32 source_offset = 0, digits = 0;
    u64 result = 0;
    while (source_offset < value.encoded.bytes) {
        u32 codepoint = 0;
        if (!decode_string_scalar(value, &source_offset, &codepoint) || codepoint < '0' || codepoint > '9') return false;
        const u64 digit = codepoint - '0';
        if (result > (~0ull - digit) / 10) return false;
        result = result * 10 + digit;
        ++digits;
    }
    if (!digits) return false;
    *output = result;
    return true;
}
bool compare_integer(Utf8View source) noexcept {
    if (!compare_number(source)) return false;
    for (u32 flag = 0; flag < 2; ++flag) {
        u64 before = 0xa537ed4f10382acbull, after = before;
        const JsonString value{source, 0, flag != 0};
        if (reference_integer(value, &before) != json_string_to_u64(value, &after) || before != after ||
            reference_integer(value, nullptr) != json_string_to_u64(value, nullptr)) return false;
    }
    return true;
}
bool reference_ascii(const JsonString& value, const Utf8View& ascii) noexcept {
    if ((!value.encoded.data && value.encoded.bytes != 0) || (!ascii.data && ascii.bytes != 0)) return false;
    u32 source_offset = 0, ascii_offset = 0;
    while (source_offset < value.encoded.bytes) {
        u32 codepoint = 0;
        if (!decode_string_scalar(value, &source_offset, &codepoint) || ascii_offset >= ascii.bytes) return false;
        const u8 expected = static_cast<u8>(ascii.data[ascii_offset++]);
        if (expected >= 0x80 || codepoint != expected) return false;
    }
    return ascii_offset == ascii.bytes;
}
bool compare_ascii(Utf8View source, Utf8View text) noexcept {
    for (u32 flag = 0; flag < 2; ++flag) {
        const JsonString value{source, 0, flag != 0};
        if (reference_ascii(value, text) != json_string_equals_ascii(value, text)) return false;
    }
    return true;
}
JsonStringDecodeResult reference_decode(JsonString value, Slice<char> output) noexcept {
    if ((!value.encoded.data && value.encoded.bytes != 0) || (!output.data && output.count != 0)) {
        return JsonStringDecodeResult{0, JsonStringDecodeError::invalid_input};
    }

    u32 source_offset = 0;
    u32 required = 0;
    while (source_offset < value.encoded.bytes) {
        u32 codepoint = 0;
        if (!decode_string_scalar(value, &source_offset, &codepoint)) {
            return JsonStringDecodeResult{0, JsonStringDecodeError::invalid_string};
        }
        char encoded[4]{};
        const u8 bytes = utf8_encode(codepoint, encoded);
        if (bytes == 0 || required > ~0u - bytes) {
            return JsonStringDecodeResult{0, JsonStringDecodeError::invalid_string};
        }
        required += bytes;
    }
    if (required > output.count) {
        return JsonStringDecodeResult{required, JsonStringDecodeError::output_too_small};
    }

    source_offset = 0;
    u32 destination_offset = 0;
    while (source_offset < value.encoded.bytes) {
        u32 codepoint = 0;
        char encoded[4]{};
        if (!decode_string_scalar(value, &source_offset, &codepoint)) {
            return JsonStringDecodeResult{0, JsonStringDecodeError::invalid_string};
        }
        const u8 bytes = utf8_encode(codepoint, encoded);
        for (u8 index = 0; index < bytes; ++index) {
            output[destination_offset++] = encoded[index];
        }
    }
    return JsonStringDecodeResult{required, JsonStringDecodeError::none};
}

bool compare(Utf8View source, u32 capacity, bool flag) noexcept {
    char before[258], after[258];
    for (u32 i = 0; i < sizeof(before); ++i) before[i] = after[i] = '\x5a';
    const JsonString value{source, 0, flag};
    const auto expected = reference_decode(value, {capacity ? before + 1 : nullptr, capacity});
    const auto actual = json_string_decode_utf8(value, {capacity ? after + 1 : nullptr, capacity});
    return actual.error == expected.error && actual.bytes == expected.bytes && bytes_equal(before, after, sizeof(before));
}
u32 random(u32& state) noexcept { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }
}
void test_json_string_decode(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace json_string_probe;
    const Utf8View integer_cases[]{ {}, {nullptr, 1}, utf8_literal("0"), utf8_literal("00"),
        utf8_literal("18446744073709551614"), utf8_literal("18446744073709551615"),
        utf8_literal("18446744073709551616"), utf8_literal("184467440737095516150"),
        utf8_literal("00000018446744073709551615"), utf8_literal("1844674407370955161"),
        utf8_literal("1844674407370955162"), utf8_literal("-1"), utf8_literal("+1"),
        utf8_literal(" 1"), utf8_literal("1 "), utf8_literal("1.0"), utf8_literal("1e1"),
        utf8_literal("\\u0031\\u0032"), utf8_literal("1844674407370955161\\u0035"),
        utf8_literal("1844674407370955161\\u0036"), utf8_literal("\\u0000"),
        utf8_literal("\\uff11"), utf8_literal("\\ud83d\\ude00"), utf8_literal("\\ud800"),
        utf8_literal("123\\u003"), utf8_literal("123\\x"), utf8_literal("123\\") };
    for (auto value : integer_cases) check(compare_integer(value), "JSON integer preserves boundaries, escapes and failed-output contents");
    char boundary[] = "18446744073709551610";
    bool boundary_valid = true;
    for (u32 byte = 0; byte < 256; ++byte) {
        boundary[sizeof(boundary) - 2] = static_cast<char>(byte);
        boundary_valid = boundary_valid && compare_integer({boundary, sizeof(boundary) - 1});
    }
    check(boundary_valid, "JSON numeric and quoted integers preserve every final byte at the overflow boundary");
    char integer_input[256]; u32 integer_state = 0x27fdeb91; bool integer_valid = true;
    for (u32 run = 0; integer_valid && run < 10000; ++run) {
        const u32 bytes = random(integer_state) % 129;
        for (u32 i = 0; i < bytes; ++i)
            integer_input[i] = run & 1 ? static_cast<char>('0' + random(integer_state) % 10) : static_cast<char>(random(integer_state));
        integer_valid = compare_integer({integer_input, bytes});
        const u64 number = (static_cast<u64>(random(integer_state)) << 32) | random(integer_state);
        u64 remaining = number; u32 start = sizeof(integer_input) - 1;
        do { integer_input[--start] = static_cast<char>('0' + remaining % 10); remaining /= 10; } while (remaining);
        u64 parsed = 0;
        integer_valid = integer_valid && json_string_to_u64({{integer_input + start, static_cast<u32>(sizeof(integer_input) - 1 - start)}, 0, false}, &parsed) && parsed == number;
        integer_valid = integer_valid && json_value_to_u64({{integer_input + start, static_cast<u32>(sizeof(integer_input) - 1 - start)}, 0, JsonValueKind::number}, &parsed) && parsed == number;
        integer_input[sizeof(integer_input) - 1] = static_cast<char>('0' + run % 10);
        integer_valid = integer_valid && compare_integer({integer_input + start, static_cast<u32>(sizeof(integer_input) - start)});
    }
    check(integer_valid, "JSON integer agrees with scalar reference on randomized inputs and u64 round trips");
    auto* integer_guard = static_cast<char*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE, PAGE_NOACCESS));
    integer_valid = integer_guard && VirtualAlloc(integer_guard, 4096, MEM_COMMIT, PAGE_READWRITE);
    if (integer_valid) {
        for (u32 i = 0; i < 4096; ++i) integer_guard[i] = '0';
        for (u32 length = 0; integer_valid && length <= 128; ++length)
            integer_valid = compare_integer({integer_guard + 4096 - length, length});
        for (auto value : integer_cases) if (value.data) {
            copy_bytes(integer_guard + 4096 - value.bytes, value.data, value.bytes);
            integer_valid = integer_valid && compare_integer({integer_guard + 4096 - value.bytes, value.bytes});
        }
    }
    if (integer_guard) VirtualFree(integer_guard, 0, MEM_RELEASE);
    check(integer_valid, "JSON integer respects exact page-end bounds, including truncated escapes");
    bool ascii_valid = true;
    // Equal, ASCII-mismatched and high-bit targets reach every comparison branch for each source byte.
    constexpr u32 ascii_flips[]{0, 1, 0x80};
    for (u32 source = 0; ascii_valid && source < 256; ++source) for (const u32 flip : ascii_flips) {
        const char a = static_cast<char>(source), b = static_cast<char>(source ^ flip);
        if (!compare_ascii({&a, 1}, {&b, 1})) { ascii_valid = false; break; }
    }
    check(ascii_valid, "JSON ASCII equality preserves every single-byte source comparison");
    const Utf8View ascii_cases[]{ {}, {nullptr, 1}, utf8_literal("id"), utf8_literal("identifier"),
        utf8_literal("\\u0069d"), utf8_literal("\\n"), utf8_literal("\n"), utf8_literal("\\\\"),
        utf8_literal("\\\""), utf8_literal("\""), utf8_literal("\\u0000"), {"\0", 1},
        utf8_literal("\\u00e9"), utf8_literal("\xc3\xa9"), utf8_literal("\\ud83d\\ude00"),
        utf8_literal("\\ud800"), utf8_literal("\\u123x"), utf8_literal("\\x"), utf8_literal("tail\\") };
    for (auto a : ascii_cases) for (auto b : ascii_cases)
        check(compare_ascii(a, b), "JSON ASCII equality preserves escapes, Unicode, malformed input and null views");
    char ascii_source[128], ascii_target[128]; u32 ascii_state = 0x683114u;
    ascii_valid = true;
    for (u32 run = 0; ascii_valid && run < 20000; ++run) {
        const u32 source_bytes = random(ascii_state) % 129;
        const u32 target_bytes = run & 1 ? source_bytes : random(ascii_state) % 129;
        for (u32 i = 0; i < 128; ++i) {
            ascii_source[i] = static_cast<char>(random(ascii_state) & (run & 2 ? 127 : 255));
            ascii_target[i] = run & 1 ? ascii_source[i] : static_cast<char>(random(ascii_state));
        }
        ascii_valid = compare_ascii({ascii_source, source_bytes}, {ascii_target, target_bytes});
    }
    check(ascii_valid, "JSON ASCII equality agrees with scalar reference on randomized strings");
    auto* guarded_ascii = static_cast<char*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE, PAGE_NOACCESS));
    ascii_valid = guarded_ascii && VirtualAlloc(guarded_ascii, 4096, MEM_COMMIT, PAGE_READWRITE);
    if (ascii_valid) {
        for (u32 i = 0; i < 4096; ++i) guarded_ascii[i] = 'x';
        for (u32 length = 0; ascii_valid && length <= 128; ++length) {
            const Utf8View view{guarded_ascii + 4096 - length, length};
            ascii_valid = compare_ascii(view, view) && compare_ascii(view, utf8_literal("x")) &&
                          compare_ascii(utf8_literal("x"), view);
        }
    }
    if (guarded_ascii) VirtualFree(guarded_ascii, 0, MEM_RELEASE);
    check(ascii_valid, "JSON ASCII comparison never reads past source or target view bounds");
    char input[256]; u32 state = 0x437a531fu; bool valid = true;
    // Arbitrary bytes include malformed UTF-8 and escapes, embedded NUL/control
    // bytes, and caller hints that disagree with the source. Preserve semantics.
    for (u32 run = 0; valid && run < 10'000; ++run) {
        const u32 length = random(state) % 257;
        for (u32 i = 0; i < length; ++i) input[i] = static_cast<char>(random(state) & ((run & 1) ? 0x7f : 0xff));
        valid = compare({input, length}, random(state) % 257, (run & 1) != 0);
    }
    check(valid, "json decoder preserves scalar results and untouched output for arbitrary bytes");
    const Utf8View cases[]{utf8_literal(""), utf8_literal("plain text"),
        utf8_literal("\\u0000\\t\\n\\r\\b\\f\\/\\\\\\\""), utf8_literal("\\u007f\\u0080\\u07ff\\u0800\\ud7ff\\ue000\\uffff"),
        utf8_literal("\\ud800\\udc00\\udbff\\udfff"), utf8_literal("\\uD800"),
        utf8_literal("\\udc00"), utf8_literal("\\u123x"), utf8_literal("tail\\"), utf8_literal("\\x")};
    for (const auto source : cases) for (u32 capacity = 0; capacity <= 80; ++capacity)
        check(compare(source, capacity, false) && compare(source, capacity, true),
              "json decoder preserves escape, surrogate, capacity and hint semantics");
    // Sample the Unicode range to exercise every UTF-8 width independently of escaping.
    valid = true;
    for (u32 scalar = 0; valid && scalar <= 0x10ffff; scalar += 97) {
        const u8 bytes = utf8_encode(scalar, input);
        if (!bytes) continue;
        valid = compare({input, bytes}, bytes, false) && compare({input, bytes}, bytes - 1, true);
    }
    check(valid, "json decoder preserves valid Unicode and tight output capacities");
    for (const auto source : cases) {
        char before[256], after[256];
        copy_bytes(before, source.data, source.bytes); copy_bytes(after, source.data, source.bytes);
        const auto expected = reference_decode({{before, source.bytes}, 0, false}, {before, sizeof(before)});
        const auto actual = json_string_decode_utf8({{after, source.bytes}, 0, false}, {after, sizeof(after)});
        check(actual.error == expected.error && actual.bytes == expected.bytes &&
              bytes_equal(before, after, source.bytes), "json decoder preserves in-place unescaping");
    }
    auto* source_pages = static_cast<char*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE, PAGE_NOACCESS));
    auto* output_pages = static_cast<char*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE, PAGE_NOACCESS));
    valid = source_pages && output_pages && VirtualAlloc(source_pages, 4096, MEM_COMMIT, PAGE_READWRITE) &&
            VirtualAlloc(output_pages, 4096, MEM_COMMIT, PAGE_READWRITE);
    for (u32 length = 1; valid && length <= 256; ++length) {
        char* source = source_pages + 4096 - length;
        char* destination = output_pages + 4096 - length;
        for (u32 i = 0; i < length; ++i) source[i] = 'a' + i % 26;
        const auto decoded = json_string_decode_utf8({{source, length}, 0, false}, {destination, length});
        valid = decoded.ok() && decoded.bytes == length && bytes_equal(source, destination, length);
    }
    check(valid, "json plain-string copies stay within exact page-boundary input and output");
    if (source_pages) VirtualFree(source_pages, 0, MEM_RELEASE);
    if (output_pages) VirtualFree(output_pages, 0, MEM_RELEASE);
}
}
