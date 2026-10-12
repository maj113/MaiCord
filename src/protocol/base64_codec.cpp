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

bool decode_digit(char character, Base64CodecAlphabet alphabet, u8* value) noexcept {
    if (character >= 'A' && character <= 'Z') {
        *value = static_cast<u8>(character - 'A');
        return true;
    }
    if (character >= 'a' && character <= 'z') {
        *value = static_cast<u8>(character - 'a' + 26);
        return true;
    }
    if (character >= '0' && character <= '9') {
        *value = static_cast<u8>(character - '0' + 52);
        return true;
    }
    const char* symbols = alphabet_text(alphabet);
    if (character == symbols[62] || character == symbols[63]) {
        *value = static_cast<u8>(character == symbols[62] ? 62 : 63);
        return true;
    }
    return false;
}

} // namespace

Base64Result base64_codec_decode(Utf8View input, Slice<u8> output, Base64CodecAlphabet alphabet,
                                 Base64CodecPadding padding_policy) noexcept {
    if ((!input.data && input.bytes != 0) || (!output.data && output.count != 0) ||
        (alphabet != Base64CodecAlphabet::standard && alphabet != Base64CodecAlphabet::url) ||
        padding_policy == Base64CodecPadding::omitted) {
        return failure(Base64Error::invalid_input);
    }
    if (padding_policy == Base64CodecPadding::required && (input.bytes & 3u) != 0) {
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
    if (padding_policy == Base64CodecPadding::required) {
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
    u8 final_digit = 0;
    for (u32 index = 0; index < digits; ++index) {
        u8 value = 0;
        if (!decode_digit(input.data[index], alphabet, &value)) {
            return failure(input.data[index] == '=' ? Base64Error::invalid_padding
                                                    : Base64Error::invalid_character,
                           index);
        }
        final_digit = value;
    }
    if ((remainder == 2 && (final_digit & 0x0Fu) != 0) || (remainder == 3 && (final_digit & 0x03u) != 0)) {
        return failure(Base64Error::non_canonical, digits == 0 ? 0 : digits - 1);
    }
    const u64 required64 = static_cast<u64>(digits / 4u) * 3u + (remainder == 2   ? 1u
                                                                 : remainder == 3 ? 2u
                                                                                  : 0u);
    if (required64 > 0xFFFFFFFFull) {
        return failure(Base64Error::invalid_length);
    }
    const u32 required = static_cast<u32>(required64);
    if (required > output.count || (required != 0 && !output.data)) {
        return failure(Base64Error::output_too_small, 0, required);
    }

    u32 source = 0;
    u32 destination = 0;
    while (source + 4 <= digits) {
        u8 a = 0, b = 0, c = 0, d = 0;
        decode_digit(input.data[source], alphabet, &a);
        decode_digit(input.data[source + 1], alphabet, &b);
        decode_digit(input.data[source + 2], alphabet, &c);
        decode_digit(input.data[source + 3], alphabet, &d);
        output[destination++] = static_cast<u8>((a << 2) | (b >> 4));
        output[destination++] = static_cast<u8>((b << 4) | (c >> 2));
        output[destination++] = static_cast<u8>((c << 6) | d);
        source += 4;
    }
    if (remainder != 0) {
        u8 a = 0, b = 0, c = 0;
        decode_digit(input.data[source], alphabet, &a);
        decode_digit(input.data[source + 1], alphabet, &b);
        output[destination++] = static_cast<u8>((a << 2) | (b >> 4));
        if (remainder == 3) {
            decode_digit(input.data[source + 2], alphabet, &c);
            output[destination++] = static_cast<u8>((b << 4) | (c >> 2));
        }
    }
    return Base64Result{destination, 0, Base64Error::none};
}

Base64Result base64_codec_encode(ConstSlice<u8> input, Slice<char> output, Base64CodecAlphabet alphabet,
                                 Base64CodecPadding padding_policy) noexcept {
    if ((!input.data && input.count != 0) || (!output.data && output.count != 0) ||
        (alphabet != Base64CodecAlphabet::standard && alphabet != Base64CodecAlphabet::url) ||
        padding_policy == Base64CodecPadding::optional) {
        return failure(Base64Error::invalid_input);
    }
    const u32 remainder = input.count % 3u;
    const u64 required64 =
        padding_policy == Base64CodecPadding::required
            ? (static_cast<u64>(input.count) + 2u) / 3u * 4u
            : static_cast<u64>(input.count / 3u) * 4u + (remainder == 0 ? 0u : remainder + 1u);
    if (required64 > 0xFFFFFFFFull) {
        return failure(Base64Error::invalid_length);
    }
    const u32 required = static_cast<u32>(required64);
    if (required > output.count || (required != 0 && !output.data)) {
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
            if (padding_policy == Base64CodecPadding::required) {
                output[destination++] = '=';
            }
        } else {
            output[destination++] = digits[(a << 4) & 63u];
            if (padding_policy == Base64CodecPadding::required) {
                output[destination++] = '=';
                output[destination++] = '=';
            }
        }
    }
    return Base64Result{destination, 0, Base64Error::none};
}

} // namespace maicord
