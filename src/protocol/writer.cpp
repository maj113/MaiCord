#include "maicord/protocol/writer.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/decimal.hpp"

#include "json_write.hpp"

namespace maicord {

bool protocol_writer_append_byte(ProtocolWriter* writer, u8 value) noexcept {
    if (!writer->output.data || writer->used >= writer->output.count) {
        return false;
    }
    writer->output[writer->used++] = value;
    return true;
}

bool protocol_writer_append_bytes(ProtocolWriter* writer, const u8* value, u32 count) noexcept {
    if (!writer->output.data || (!value && count != 0) || writer->used > writer->output.count ||
        count > writer->output.count - writer->used) {
        return false;
    }
    copy_bytes(writer->output.data + writer->used, value, count);
    writer->used += count;
    return true;
}

bool protocol_writer_append_c_string(ProtocolWriter* writer, const char* value) noexcept {
    if (!value) {
        return false;
    }
    u32 bytes = 0;
    while (value[bytes] != 0) {
        if (bytes == 0xFFFFFFFFu) {
            return false;
        }
        ++bytes;
    }
    return protocol_writer_append_bytes(writer, reinterpret_cast<const u8*>(value), bytes);
}

bool protocol_writer_append_u64(ProtocolWriter* writer, u64 value) noexcept {
    if (!writer->output.data || writer->used > writer->output.count ||
        decimal_digits(value) > writer->output.count - writer->used) {
        return false;
    }
    writer->used += decimal_write(writer->output.data + writer->used, value);
    return true;
}

bool protocol_writer_append_snowflake(ProtocolWriter* writer, u64 id) noexcept {
    return protocol_writer_append_byte(writer, '"') && protocol_writer_append_u64(writer, id) &&
           protocol_writer_append_byte(writer, '"');
}

bool protocol_writer_append_percent_encoded(ProtocolWriter* writer, Utf8View value) noexcept {
    for (u32 index = 0; index < value.bytes; ++index) {
        const u8 byte = static_cast<u8>(value.data[index]);
        if (url_unreserved(byte)) {
            if (!protocol_writer_append_byte(writer, byte)) {
                return false;
            }
        } else if (!protocol_writer_append_byte(writer, '%') ||
                   !protocol_writer_append_byte(writer, ascii_hex_digit_upper(byte >> 4)) ||
                   !protocol_writer_append_byte(writer, ascii_hex_digit_upper(byte))) {
            return false;
        }
    }
    return true;
}

bool protocol_writer_append_json_string(ProtocolWriter* writer, Utf8View value) noexcept {
    u32 encoded_bytes = 0;
    if (!protocol_detail::measure_json_string(value, &encoded_bytes) || !writer->output.data ||
        writer->used > writer->output.count || encoded_bytes > writer->output.count - writer->used) {
        return false;
    }
    protocol_detail::write_json_string(writer->output.data + writer->used, value);
    writer->used += encoded_bytes;
    return true;
}

} // namespace maicord
