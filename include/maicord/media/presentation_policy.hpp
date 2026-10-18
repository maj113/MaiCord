#pragma once

#include "maicord/media/asset.hpp"
#include "maicord/media/jpeg.hpp"

namespace maicord {

struct MediaPresentationRequest {
    u16 width;
    u16 height;
    MediaResolutionTier resolution;
    u8 reserved;
};

// Quantizes display demand so small resize changes reuse decoded/GPU content
// while a real scale increase gets a distinct, upgradeable cache identity.
MediaResolutionTier media_resolution_tier_for_display(
    float width_dip, float height_dip, u32 dpi, u16 decoder_extent_cap = 512) noexcept;
// JPEG renditions past the JPEG decoder's limit arrive as PNG (see
// media_preview_proxy_png), so JPEG shares PNG's cap.
constexpr u16 media_preview_extent_cap(MediaAttachmentFormat format) noexcept {
    return format == MediaAttachmentFormat::png || format == MediaAttachmentFormat::jpeg ? 2048 : 512;
}
// A JPEG rendition larger than the JPEG decoder takes is asked of the media
// proxy as PNG.
constexpr bool media_preview_proxy_png(MediaAttachmentFormat format, u32 width, u32 height) noexcept {
    return format == MediaAttachmentFormat::jpeg &&
           (width > kJpegDimensionMaximum || height > kJpegDimensionMaximum);
}

// Produces an aspect-preserving media-proxy request. The source is
// never upscaled and native decoder limits remain authoritative.
MediaPresentationRequest media_presentation_request(
    u32 source_width, u32 source_height, float width_dip, float height_dip,
    u32 dpi, u16 decoder_extent_cap) noexcept;

} // namespace maicord
