#pragma once

#include "maicord/media/asset_key.hpp"

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
// JPEG and still WebP renditions past the GPU paths' 512 px decode on the CPU
// (kReducedJpegPreviewExtent, kReducedWebpExtent), so they share PNG's cap.
// Animated WebP frames stay on the GPU path: the proxy scales them to 512.
constexpr u16 media_preview_extent_cap(MediaAttachmentFormat format, bool animated = false) noexcept {
    return format == MediaAttachmentFormat::png || format == MediaAttachmentFormat::jpeg ||
                   (format == MediaAttachmentFormat::webp && !animated)
               ? 2048
               : 512;
}

// Produces an aspect-preserving media-proxy request. The source is
// never upscaled and native decoder limits remain authoritative.
MediaPresentationRequest media_presentation_request(
    u32 source_width, u32 source_height, float width_dip, float height_dip,
    u32 dpi, u16 decoder_extent_cap) noexcept;

} // namespace maicord
