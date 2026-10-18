#include "maicord/protocol/rest_route.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {

bool route_writer_initialize(RouteWriter* writer, Slice<u8> output) noexcept {
    if (!output.data || output.count == 0) {
        return false;
    }
    *writer = RouteWriter{output, 0, true};
    return true;
}

bool route_append_byte(RouteWriter* writer, u8 value) noexcept {
    if (!writer->valid || writer->bytes >= writer->output.count) {
        writer->valid = false;
        return false;
    }
    writer->output[writer->bytes++] = value;
    return true;
}

bool route_append(RouteWriter* writer, Utf8View value) noexcept {
    if (!writer->valid || (!value.data && value.bytes != 0) || writer->bytes > writer->output.count ||
        value.bytes > writer->output.count - writer->bytes) {
        writer->valid = false;
        return false;
    }
    copy_bytes(writer->output.data + writer->bytes, value.data, value.bytes);
    writer->bytes += value.bytes;
    return true;
}

bool route_append_u64(RouteWriter* writer, u64 value) noexcept {
    char digits[20];
    return route_append(writer, Utf8View{digits, decimal_write(digits, value)});
}

bool route_append_path_component(RouteWriter* writer, Utf8View value) noexcept {
    if (!writer->valid || !value.data || value.bytes == 0) {
        writer->valid = false;
        return false;
    }
    for (u32 index = 0; index < value.bytes; ++index) {
        const u8 byte = static_cast<u8>(value.data[index]);
        if (url_unreserved(byte)) {
            if (!route_append_byte(writer, byte)) {
                return false;
            }
        } else if (!route_append_byte(writer, '%') ||
                   !route_append_byte(writer, static_cast<u8>(ascii_hex_digit_upper(byte >> 4))) ||
                   !route_append_byte(writer, static_cast<u8>(ascii_hex_digit_upper(byte)))) {
            return false;
        }
    }
    return true;
}

} // namespace maicord
