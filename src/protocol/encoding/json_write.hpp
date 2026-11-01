#pragma once

#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord::protocol_detail {

struct EncodedString {
    Utf8View source;
    u32 bytes;
};

bool measure_json_string(const Utf8View& source, u32* encoded_bytes) noexcept;
void write_json_string(u8* output, const Utf8View& source) noexcept;
bool encoded_string(const Utf8View& source, bool require_nonempty, EncodedString* output) noexcept;
bool encoded_string(const Utf8View& source, u32 maximum_bytes, EncodedString* output) noexcept;
void write_encoded(u8* output, u32* offset, const EncodedString& string) noexcept;
// True when the reason percent-encodes within the audit-reason bounds (the encoding is discarded).
bool audit_reason_encodes(const Utf8View& reason) noexcept;

template <usize Count>
__forceinline void write_literal(u8* output, u32* offset,
                                 const char (&literal)[Count]) noexcept {
    static_assert(Count > 0 && Count - 1 <= 0xFFFFFFFFull);
    constexpr u32 bytes = static_cast<u32>(Count - 1);
    copy_bytes(output + *offset, literal, bytes);
    *offset += bytes;
}

} // namespace maicord::protocol_detail
