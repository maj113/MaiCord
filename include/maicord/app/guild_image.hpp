#pragma once
#include "maicord/protocol/guild_assets.hpp"

namespace maicord {

// Re-encoded output of guild_image_prepare; empty when the source is uploaded
// as picked. Released (and erased) by guild_image_release.
struct GuildImageBuffer {
    u8* data;
    u32 capacity;
    u32 bytes;
};

// Recognizes PNG, APNG, GIF, JPEG, WebP and Lottie JSON by content.
GuildImageKind guild_image_sniff(ConstSlice<u8> source) noexcept;

// Validates a picked file for one image use. Files that already satisfy the
// use upload unchanged; a static PNG that is too large, oversized or of the
// wrong shape is centre-cropped to the use's aspect, box-filtered down to its
// maximum size and re-encoded as PNG into `buffer`. `prepared` then views
// either the source or the buffer. Nothing is allocated on failure.
GuildImageInfo guild_image_prepare(GuildImageProfile profile, ConstSlice<u8> source, GuildImageBuffer* buffer,
                                   GuildPreparedImage* prepared) noexcept;
void guild_image_release(GuildImageBuffer* buffer) noexcept;

// Largest re-encoded width/height for each use (banner 16:9, profile banner 17:6).
void guild_image_maximum(GuildImageProfile profile, u32* width, u32* height) noexcept;

} // namespace maicord
