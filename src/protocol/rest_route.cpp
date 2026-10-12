#include "maicord/protocol/rest_route.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

constexpr u8 hexadecimal(u8 value) noexcept {
    return value < 10 ? static_cast<u8>('0' + value) : static_cast<u8>('A' + value - 10);
}

} // namespace

bool route_writer_initialize(RouteWriter* writer, Slice<u8> output) noexcept {
    if (!writer || !output.data || output.count == 0) {
        return false;
    }
    *writer = RouteWriter{output, 0, true};
    return true;
}

bool route_append_byte(RouteWriter* writer, u8 value) noexcept {
    if (!writer || !writer->valid || writer->bytes >= writer->output.count) {
        if (writer) {
            writer->valid = false;
        }
        return false;
    }
    writer->output[writer->bytes++] = value;
    return true;
}

bool route_append(RouteWriter* writer, Utf8View value) noexcept {
    if (!writer || !writer->valid || (!value.data && value.bytes != 0) ||
        writer->bytes > writer->output.count || value.bytes > writer->output.count - writer->bytes) {
        if (writer) {
            writer->valid = false;
        }
        return false;
    }
    copy_bytes(writer->output.data + writer->bytes, value.data, value.bytes);
    writer->bytes += value.bytes;
    return true;
}

bool route_append_u64(RouteWriter* writer, u64 value) noexcept {
    u8 digits[20];
    const u32 count = decimal_write(digits, value);
    for (u32 index = 0; index < count; ++index) {
        if (!route_append_byte(writer, digits[index])) {
            return false;
        }
    }
    return true;
}

bool route_append_path_component(RouteWriter* writer, Utf8View value) noexcept {
    if (!writer || !writer->valid || !value.data || value.bytes == 0) {
        if (writer) {
            writer->valid = false;
        }
        return false;
    }
    for (u32 index = 0; index < value.bytes; ++index) {
        const u8 byte = static_cast<u8>(value.data[index]);
        if (url_unreserved(byte)) {
            if (!route_append_byte(writer, byte)) {
                return false;
            }
        } else if (!route_append_byte(writer, '%') || !route_append_byte(writer, hexadecimal(byte >> 4)) ||
                   !route_append_byte(writer, hexadecimal(byte & 0x0Fu))) {
            return false;
        }
    }
    return true;
}

} // namespace maicord
