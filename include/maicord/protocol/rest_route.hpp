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

// route_write(&route, parts...) appends each part in order and stops at the first that fails. A part is a string
// literal, a char, an unsigned integer (decimal digits), a Utf8View (as is) or path_component(view) (percent-encoded,
// nonempty).
struct PathComponent { Utf8View value; };
constexpr PathComponent path_component(Utf8View value) noexcept { return {value}; }

template <usize Count> __forceinline bool route_put(RouteWriter* writer, const char (&literal)[Count]) noexcept {
    return route_append_bytes(writer, literal, Count - 1);
}
__forceinline bool route_put(RouteWriter* writer, char value) noexcept {
    return route_append_byte(writer, static_cast<u8>(value));
}
bool route_put(RouteWriter*, bool) = delete;
template <typename Number>
    requires(__is_unsigned(Number) && !__is_same(Number, bool))
__forceinline bool route_put(RouteWriter* writer, Number value) noexcept {
    return route_append_u64(writer, value);
}
__forceinline bool route_put(RouteWriter* writer, const Utf8View& value) noexcept {
    return route_append(writer, value);
}
__forceinline bool route_put(RouteWriter* writer, const PathComponent& value) noexcept {
    return route_append_path_component(writer, value.value);
}
template <typename... Parts> __forceinline bool route_write(RouteWriter* writer, const Parts&... parts) noexcept {
    return (route_put(writer, parts) && ...);
}

} // namespace maicord
