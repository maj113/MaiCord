#pragma once
#include "maicord/render/colr_program.hpp"
#include "maicord/media/asset_key.hpp"
namespace maicord {
inline constexpr u32 kLottieMaximumBytes = 1024 * 1024;
// Raster extent of a compiled Lottie frame: a message sticker at the official
// client's 160 px sticker size; picker previews (96 DIP cells) at 128.
constexpr u32 lottie_raster_pixels(const MediaAssetKey& key) noexcept {
    return key.kind == MediaAssetKind::sticker ? 160u : 128u;
}
struct LottieTiming { float first=0, rate=0; u32 frames=1; u64 duration_us=0; };
using LottieProgramAcquire = ColrPaintProgram* (*)(void* context) noexcept;
// Compile a bounded animation frame into the hardware paint program. Canvas coordinates are normalized so the larger extent is one.
// JSON and temporary native geometry are released before this call returns.
// The callback supplies a caller-owned program after metadata and relative-frame validation.
// The compiler calls the callback once and retains no reference after this call.
bool lottie_program_compile_frame(ConstSlice<u8> json, u32 frame_index, float pixels,
                                  LottieTiming* timing, LottieProgramAcquire acquire, void* context) noexcept;
} // namespace maicord
