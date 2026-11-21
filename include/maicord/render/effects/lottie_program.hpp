#pragma once
#include "maicord/render/effects/colr_program.hpp"
#include "maicord/media/core/asset_key.hpp"
namespace maicord {
inline constexpr u32 kLottieMaximumBytes = 1024 * 1024;
// Raster extent of a compiled Lottie frame: a message sticker at 160 px; picker
// previews (96 DIP cells) at 128.
constexpr u32 lottie_raster_pixels(const MediaAssetKey& key) noexcept {
    return key.kind == MediaAssetKind::sticker ? 160u : 128u;
}
// The histogram bin of a height across an outline's bounds. A sliver a matte
// leaves can have no height (scale 0) or bounds that are not finite (NaN
// here); either shares bin 0, which only overstates the outline's winding.
constexpr i32 lottie_height_bin(float y, float top, float scale, i32 bins) noexcept {
    const float at = (y - top) * scale;
    return !(at > 0) ? 0 : at >= bins - 1 ? bins - 1 : static_cast<i32>(at);
}
struct LottieTiming { float first=0, rate=0; u32 frames=1; u64 duration_us=0; };
// A Lottie document read once for all its frames: the structural index of its JSON followed by a table of where each
// container and string ends, each member's next member and each array's length, in one block of pages, and its header.
struct LottieDocument {
    u32* index;
    u32 count;
    LottieTiming timing;
};
// Validates a JSON document of at most kLottieMaximumBytes, reads its header and indexes it. False leaves nothing to
// release and the default timing.
bool lottie_document_read(ConstSlice<u8> json, LottieDocument* document) noexcept;
void lottie_document_release(LottieDocument* document) noexcept;
// Compile frame `frame_index` (below document.timing.frames) of the document read from exactly `json` into the
// hardware paint program. Canvas coordinates are normalized so the larger extent is one. Temporary native geometry is
// released before this call returns.
bool lottie_program_compile_frame(const LottieDocument& document, ConstSlice<u8> json, u32 frame_index, float pixels,
                                  ColrPaintProgram* program) noexcept;
} // namespace maicord
