#pragma once

#include "maicord/core/utf.hpp"
#include "maicord/sdk/maicord_plugin.h"

// The one text rule of everything a plugin hands the host (descriptor strings, toasts, banners, decorations):
// valid UTF-8 with no control characters and no DEL, except the line breaks the caller allows.
namespace maicord {

inline constexpr u32 kPluginTextLines = 1u << '\n';
inline constexpr u32 kPluginTextBody = 1u << '\n' | 1u << '\t' | 1u << '\r';

// A string of the plugin ABI and the host's view of the same bytes, in both directions; neither copies.
constexpr Utf8View plugin_view(const mc_str& text) noexcept {
    return {text.data, text.bytes};
}
constexpr mc_str plugin_str(const Utf8View& text) noexcept {
    return {text.data, text.bytes, 0};
}

// `allowed` has bit n set for each control character n (below 0x20) the text may hold. Empty text is clean.
__declspec(noinline) inline bool plugin_text_clean(const char* data, u32 bytes, u32 allowed = 0) noexcept {
    if (!bytes)
        return true;
    if (!data || !utf8_validate({data, bytes}).valid)
        return false;
    for (u32 i = 0; i < bytes; ++i) {
        const u8 c = static_cast<u8>(data[i]);
        if (c == 0x7f || (c < 0x20 && !(allowed >> c & 1)))
            return false;
    }
    return true;
}

} // namespace maicord
