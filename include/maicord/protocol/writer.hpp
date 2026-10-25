#pragma once

#include "maicord/core/utf.hpp"
#include "maicord/core/ascii.hpp"

namespace maicord {

struct ProtocolWriter {
    Slice<u8> output;
    u32 used;
};

bool protocol_writer_append_byte(ProtocolWriter* writer, u8 value) noexcept;
bool protocol_writer_append_bytes(ProtocolWriter* writer, const u8* value, u32 count) noexcept;
__forceinline bool protocol_writer_append_bytes(ProtocolWriter* writer,
                                                ConstSlice<u8> value) noexcept {
    return protocol_writer_append_bytes(writer, value.data, value.count);
}
bool protocol_writer_append_c_string(ProtocolWriter* writer, const char* value) noexcept;
bool protocol_writer_append_u64(ProtocolWriter* writer, u64 value) noexcept;
// A snowflake in its JSON form: decimal digits in a string.
bool protocol_writer_append_snowflake(ProtocolWriter* writer, u64 id) noexcept;
bool protocol_writer_append_json_string(ProtocolWriter* writer, Utf8View value) noexcept;

// RFC 3986 unreserved characters, which percent-encoding leaves as they are.
constexpr bool url_unreserved(u8 value) noexcept {
    return ascii_is_alnum(value) || value == '-' || value == '_' || value == '.' || value == '~';
}
// Appends value with every byte but the url_unreserved ones as %XX.
bool protocol_writer_append_percent_encoded(ProtocolWriter* writer, Utf8View value) noexcept;

// Copies a body staged by writer into output once it was written completely
// and fits. Result is a {bytes, error} pair whose error enum has none and
// capacity, such as SoundboardWriteResult.
template <typename Result>
Result protocol_writer_finish(const ProtocolWriter& writer, bool ok, Slice<u8> output,
                              const u8* staged) noexcept {
    using Error = decltype(Result{}.error);
    if (!ok || writer.used > output.count) {
        return {0, Error::capacity};
    }
    copy_bytes(output.data, staged, writer.used);
    return {writer.used, Error::none};
}

template <usize Count>
__forceinline bool protocol_writer_append_literal(ProtocolWriter* writer,
                                                  const char (&literal)[Count]) noexcept {
    static_assert(Count > 0 && Count - 1 <= 0xFFFFFFFFull);
    return protocol_writer_append_bytes(writer, reinterpret_cast<const u8*>(literal),
                                        static_cast<u32>(Count - 1));
}

} // namespace maicord
