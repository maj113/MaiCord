#include "base64_codec.hpp"

namespace maicord {
namespace {

inline constexpr char kStandardAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
inline constexpr char kUrlAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

Base64Result failure(Base64Error error, u32 offset = 0, u32 bytes = 0) noexcept {
    return Base64Result{bytes, offset, error};
}

const char* alphabet_text(Base64CodecAlphabet alphabet) noexcept {
    return alphabet == Base64CodecAlphabet::standard ? kStandardAlphabet : kUrlAlphabet;
}

// For each ASCII byte: its digit value in the low 6 bits, kStandardDigit when the standard alphabet has it and
// kUrlDigit when the URL alphabet has it. A byte that is no digit has neither bit. No byte from 0x80 up is a digit.
constexpr u8 kStandardDigit = 0x40, kUrlDigit = 0x80;
struct DigitTable {
    u8 entry[128];
};
constexpr DigitTable make_digit_table() noexcept {
    DigitTable table{};
    for (u32 value = 0; value < 64; ++value) {
        table.entry[static_cast<u8>(kStandardAlphabet[value])] |= static_cast<u8>(value | kStandardDigit);
        table.entry[static_cast<u8>(kUrlAlphabet[value])] |= static_cast<u8>(value | kUrlDigit);
    }
    return table;
}
constexpr DigitTable kDigits = make_digit_table();

} // namespace

Base64Result base64_codec_decode(Utf8View input, Slice<u8> output, Base64CodecAlphabet alphabet,
                                 bool padded) noexcept {
    if ((!input.data && input.bytes != 0) || (!output.data && output.count != 0)) {
        return failure(Base64Error::invalid_input);
    }
    if (padded && (input.bytes & 3u) != 0) {
        return failure(Base64Error::invalid_length, input.bytes);
    }
    u32 padding = 0;
    while (padding < input.bytes && input.data[input.bytes - padding - 1] == '=') {
        ++padding;
    }
    if (padding > 2) {
        return failure(Base64Error::invalid_padding, input.bytes - padding);
    }
    const u32 digits = input.bytes - padding;
    const u32 remainder = digits & 3u;
    if (padded) {
        if ((padding == 2 && remainder != 2) || (padding == 1 && remainder != 3) ||
            (padding == 0 && remainder != 0)) {
            return failure(Base64Error::invalid_padding, digits);
        }
    } else {
        if (remainder == 1) {
            return failure(Base64Error::invalid_length, digits);
        }
        if (padding != 0 && ((input.bytes & 3u) != 0 || (remainder == 2 && padding != 2) ||
                             (remainder == 3 && padding != 1) || remainder == 0)) {
            return failure(Base64Error::invalid_padding, digits);
        }
    }
    const u8 accepted = alphabet == Base64CodecAlphabet::standard ? kStandardDigit : kUrlDigit;
    u8 final_digit = 0;
    for (u32 index = 0; index < digits; ++index) {
        const u8 byte = static_cast<u8>(input.data[index]);
        const u8 entry = kDigits.entry[byte & 127u];
        if (!(entry & (byte < 128 ? accepted : 0))) {
            return failure(input.data[index] == '=' ? Base64Error::invalid_padding
                                                    : Base64Error::invalid_character,
                           index);
        }
        final_digit = entry & 63u;
    }
    if ((remainder == 2 && (final_digit & 0x0Fu) != 0) || (remainder == 3 && (final_digit & 0x03u) != 0)) {
        return failure(Base64Error::non_canonical, digits == 0 ? 0 : digits - 1);
    }
    // digits <= 2^32 - 1 and remainder is 0, 2 or 3 here, so the size always fits u32.
    const u32 required = (digits / 4u) * 3u + (remainder != 0 ? remainder - 1u : 0u);
    if (required > output.count) {
        return failure(Base64Error::output_too_small, 0, required);
    }

    // Every byte below digits is an ASCII digit of the alphabet (checked above).
    const auto digit = [&](u32 at) noexcept {
        return static_cast<u32>(kDigits.entry[static_cast<u8>(input.data[at]) & 127u] & 63u);
    };
    u32 source = 0;
    u32 destination = 0;
    for (; source + 4 <= digits; source += 4) {
        const u32 group = digit(source) << 18 | digit(source + 1) << 12 | digit(source + 2) << 6 | digit(source + 3);
        output[destination++] = static_cast<u8>(group >> 16);
        output[destination++] = static_cast<u8>(group >> 8);
        output[destination++] = static_cast<u8>(group);
    }
    if (remainder != 0) {
        const u32 group = digit(source) << 18 | digit(source + 1) << 12 | (remainder == 3 ? digit(source + 2) << 6 : 0);
        output[destination++] = static_cast<u8>(group >> 16);
        if (remainder == 3)
            output[destination++] = static_cast<u8>(group >> 8);
    }
    return Base64Result{destination, 0, Base64Error::none};
}

Base64Result base64_codec_encode(ConstSlice<u8> input, Slice<char> output, Base64CodecAlphabet alphabet,
                                 bool padded) noexcept {
    if ((!input.data && input.count != 0) || (!output.data && output.count != 0)) {
        return failure(Base64Error::invalid_input);
    }
    const u32 remainder = input.count % 3u;
    const u64 required64 =
        padded ? (static_cast<u64>(input.count) + 2u) / 3u * 4u
               : static_cast<u64>(input.count / 3u) * 4u + (remainder == 0 ? 0u : remainder + 1u);
    if (required64 > 0xFFFFFFFFull) {
        return failure(Base64Error::invalid_length);
    }
    const u32 required = static_cast<u32>(required64);
    if (required > output.count) {
        return failure(Base64Error::output_too_small, 0, required);
    }

    const char* digits = alphabet_text(alphabet);
    u32 source = 0;
    u32 destination = 0;
    while (source + 3 <= input.count) {
        const u8 a = input[source], b = input[source + 1], c = input[source + 2];
        output[destination++] = digits[a >> 2];
        output[destination++] = digits[((a << 4) | (b >> 4)) & 63u];
        output[destination++] = digits[((b << 2) | (c >> 6)) & 63u];
        output[destination++] = digits[c & 63u];
        source += 3;
    }
    if (remainder != 0) {
        const u8 a = input[source];
        output[destination++] = digits[a >> 2];
        if (remainder == 2) {
            const u8 b = input[source + 1];
            output[destination++] = digits[((a << 4) | (b >> 4)) & 63u];
            output[destination++] = digits[(b << 2) & 63u];
            if (padded) {
                output[destination++] = '=';
            }
        } else {
            output[destination++] = digits[(a << 4) & 63u];
            if (padded) {
                output[destination++] = '=';
                output[destination++] = '=';
            }
        }
    }
    return Base64Result{destination, 0, Base64Error::none};
}

} // namespace maicord
