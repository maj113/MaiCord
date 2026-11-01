#pragma once

#include "maicord/core/utf.hpp"

namespace maicord {

struct RouteWriter {
    Slice<u8> output;
    u32 bytes;
    bool valid;
};

bool route_writer_initialize(RouteWriter* writer,
                             Slice<u8> output) noexcept;
bool route_append_bytes(RouteWriter* writer, const char* value, u32 bytes) noexcept;
// Inlined so that a literal argument travels as a pointer and a length.
__forceinline bool route_append(RouteWriter* writer, const Utf8View& value) noexcept {
    return route_append_bytes(writer, value.data, value.bytes);
}
bool route_append_byte(RouteWriter* writer, u8 value) noexcept;
bool route_append_u64(RouteWriter* writer, u64 value) noexcept;
bool route_append_path_component(RouteWriter* writer,
                                 Utf8View value) noexcept;

} // namespace maicord
