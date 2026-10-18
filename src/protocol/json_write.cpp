#include "json_write.hpp"
#include "maicord/core/ascii.hpp"

namespace maicord::protocol_detail {
namespace {

bool measure_encoded_string(Utf8View source, bool require_nonempty, u32 maximum_bytes,
                            EncodedString* output) noexcept {
    u32 bytes = 0;
    if ((require_nonempty && source.bytes == 0) || source.bytes > maximum_bytes ||
        !utf8_validate(source).valid || !measure_json_string(source, &bytes)) {
        return false;
    }
    *output = EncodedString{source, bytes};
    return true;
}

} // namespace

bool measure_json_string(Utf8View source, u32* encoded_bytes) noexcept {
    if (!source.data && source.bytes != 0) {
        return false;
    }
    u64 bytes = 2;
    for (u32 index = 0; index < source.bytes; ++index) {
        const u8 value = static_cast<u8>(source.data[index]);
        bytes += value == '"' || value == '\\' || value == '\b' || value == '\f' ||
                         value == '\n' || value == '\r' || value == '\t'
                     ? 2
                 : value < 0x20 ? 6
                                : 1;
    }
    if (bytes > 0xFFFFFFFFull) {
        return false;
    }
    *encoded_bytes = static_cast<u32>(bytes);
    return true;
}

void write_json_string(u8* output, Utf8View source) noexcept {
    u32 offset = 0;
    output[offset++] = '"';
    for (u32 index = 0; index < source.bytes; ++index) {
        const u8 value = static_cast<u8>(source.data[index]);
        if (value == '"' || value == '\\') {
            output[offset++] = '\\';
            output[offset++] = value;
        } else if (value == '\b' || value == '\f' || value == '\n' || value == '\r' || value == '\t') {
            output[offset++] = '\\';
            output[offset++] = value == '\b'   ? 'b'
                               : value == '\f' ? 'f'
                               : value == '\n' ? 'n'
                               : value == '\r' ? 'r'
                                                : 't';
        } else if (value < 0x20) {
            output[offset++] = '\\';
            output[offset++] = 'u';
            output[offset++] = '0';
            output[offset++] = '0';
            output[offset++] = static_cast<u8>(ascii_hex_digit_upper(value >> 4));
            output[offset++] = static_cast<u8>(ascii_hex_digit_upper(value));
        } else {
            output[offset++] = value;
        }
    }
    output[offset] = '"';
}

bool encoded_string(Utf8View source, bool require_nonempty, EncodedString* output) noexcept {
    return measure_encoded_string(source, require_nonempty, 0xFFFFFFFFu, output);
}

bool encoded_string(Utf8View source, u32 maximum_bytes, EncodedString* output) noexcept {
    return measure_encoded_string(source, true, maximum_bytes, output);
}

void write_encoded(u8* output, u32* offset, const EncodedString& string) noexcept {
    write_json_string(output + *offset, string.source);
    *offset += string.bytes;
}

} // namespace maicord::protocol_detail
