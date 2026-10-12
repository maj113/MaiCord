#pragma once
#include "maicord/app/guild_image.hpp"
#include "maicord/model/image_crop.hpp"

namespace maicord {

// Avatar and profile banner uploads, cropped the way the official client's
// "Edit Image" dialog does it locally. Stills become a PNG of the crop resized to
// the capped output size (canvas.toDataURL("image/png")); an animated GIF is
// cropped and rotated at source resolution and stays a GIF (as the official
// client's GIF cropper does). Sources: PNG (APNG: its default image), JPEG
// (baseline and progressive, EXIF orientation applied), WebP and GIF.
// Gaps: animated WebP and APNG crop to a still PNG of their first frame
// (the official client re-encodes animated WebP); lossless WebP and WebP alpha
// planes with VP8L compression decode only up to 512 px per side.
inline constexpr u32 kProfileImageSourceMaximumBytes = 10u * 1024u * 1024u; // chooser limit (10 MB)
// Re-encoded GIF crops past this fall back to a PNG of the first frame.
inline constexpr u32 kProfileImageGifOutputMaximumBytes = 16u * 1024u * 1024u;
// Largest still output per side (the official client caps at 1024 and 2400x848).
inline constexpr u32 kProfileImageOutputDimensionMaximum = 4096;

enum class ProfileImageError : u8 {
    none,
    input,     // empty source, bad crop rectangle, rotation or output size
    format,    // not a PNG, JPEG, WebP or GIF
    too_large, // source over kProfileImageSourceMaximumBytes
    decode,
    memory,
};

struct ProfileImageInfo {
    u32 width, height;          // display orientation (EXIF applied)
    u32 frames;                 // >1 for animated GIF/APNG/WebP
    u32 bytes;                  // encoded output bytes (crop only)
    u32 output_width, output_height;
    GuildImageKind kind;        // source kind
    GuildImageKind output_kind; // png for stills; gif for an animated GIF crop
    ProfileImageError error;
    bool animated;
};

// Recognises and validates a picked file without decoding its pixels.
ProfileImageInfo profile_image_probe(ConstSlice<u8> source) noexcept;
// Crops and encodes into *output (allocated with VirtualAlloc; release with
// guild_image_release). `crop` is image_crop_output() for this source's
// display size; output_width/height are ignored for animated GIFs. Nothing
// is allocated on failure.
ProfileImageInfo profile_image_crop(ConstSlice<u8> source, const ImageCropOutput& crop,
                                    GuildImageBuffer* output) noexcept;

} // namespace maicord
