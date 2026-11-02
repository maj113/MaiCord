#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

// Reads the inflated bytes of a table that a build tool packed (tools/pack_settings_catalog.py and
// tools/pack_tables.py write them, the owner's loader reads them): it is the build's own data, so nothing here
// checks lengths. Little endian; a text is its bytes then a NUL, an empty one meaning no text.
struct PackedReader {
    const u8* at;
    u32 byte() noexcept {
        return *at++;
    }
    u32 word() noexcept {
        const u32 value = at[0] | static_cast<u32>(at[1]) << 8;
        at += 2;
        return value;
    }
    Utf8View text() noexcept {
        const char* start = reinterpret_cast<const char*>(at);
        u32 bytes = 0;
        while (at[bytes])
            ++bytes;
        at += bytes + 1;
        return bytes ? Utf8View{start, bytes} : Utf8View{};
    }
};

} // namespace maicord
