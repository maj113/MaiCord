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
bool route_append(RouteWriter* writer,
                  Utf8View value) noexcept;
bool route_append_byte(RouteWriter* writer, u8 value) noexcept;
bool route_append_u64(RouteWriter* writer, u64 value) noexcept;
bool route_append_path_component(RouteWriter* writer,
                                 Utf8View value) noexcept;

} // namespace maicord
