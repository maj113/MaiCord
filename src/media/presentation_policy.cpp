#include "maicord/media/presentation_policy.hpp"

namespace maicord {
namespace {

u32 ceil_positive(float value) noexcept {
    if (!(value > 0.0f) || value > 65'535.0f) {
        return value > 0.0f ? 65'535u : 0u;
    }
    const u32 truncated = static_cast<u32>(value);
    return truncated + (static_cast<float>(truncated) < value ? 1u : 0u);
}

u16 media_resolution_tier_extent(MediaResolutionTier tier) noexcept {
    switch (tier) {
    case MediaResolutionTier::px64:
        return 64;
    case MediaResolutionTier::px128:
        return 128;
    case MediaResolutionTier::px256:
        return 256;
    case MediaResolutionTier::px512:
        return 512;
    case MediaResolutionTier::px1024: return 1024;
    case MediaResolutionTier::px2048: return 2048;
    default:
        return 0;
    }
}

} // namespace

MediaResolutionTier media_resolution_tier_for_display(
    float width_dip, float height_dip, u32 dpi, u16 decoder_extent_cap) noexcept {
    const float scale = static_cast<float>(maximum(dpi, 1u)) / 96.0f;
    const u32 demand = minimum<u32>(decoder_extent_cap, maximum(ceil_positive(width_dip * scale),
                               ceil_positive(height_dip * scale)));
    if (demand == 0) {
        return MediaResolutionTier::original;
    }
    if (demand <= 64) {
        return MediaResolutionTier::px64;
    }
    if (demand <= 128) {
        return MediaResolutionTier::px128;
    }
    if (demand <= 256) {
        return MediaResolutionTier::px256;
    }
    if (demand <= 512) return MediaResolutionTier::px512;
    if (demand <= 1024) return MediaResolutionTier::px1024;
    return MediaResolutionTier::px2048;
}

MediaPresentationRequest media_presentation_request(
    u32 source_width, u32 source_height, float width_dip, float height_dip,
    u32 dpi, u16 decoder_extent_cap) noexcept {
    MediaPresentationRequest request{};
    request.resolution =
        media_resolution_tier_for_display(width_dip, height_dip, dpi, decoder_extent_cap);
    if (source_width == 0 || source_height == 0 || decoder_extent_cap == 0) {
        return request;
    }
    u32 target = media_resolution_tier_extent(request.resolution);
    if (target == 0) {
        target = decoder_extent_cap;
    }
    target = minimum<u32>(target, decoder_extent_cap);
    target = minimum<u32>(target, maximum(source_width, source_height));
    target = maximum(target, 1u);
    if (source_width >= source_height) {
        const u64 scaled =
            (static_cast<u64>(source_height) * target + source_width / 2u) /
            source_width;
        request.width = static_cast<u16>(target);
        request.height = static_cast<u16>(maximum<u64>(scaled, 1));
    } else {
        const u64 scaled =
            (static_cast<u64>(source_width) * target + source_height / 2u) /
            source_height;
        request.width = static_cast<u16>(maximum<u64>(scaled, 1));
        request.height = static_cast<u16>(target);
    }
    return request;
}

} // namespace maicord
