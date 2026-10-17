#pragma once
#include "maicord/render/colr_program.hpp"
namespace maicord {
inline constexpr u32 kLottieMaximumBytes = 1024 * 1024;
struct LottieTiming { float first=0, rate=0; u32 frames=1; u64 duration_us=0; };
using LottieProgramAcquire = ColrPaintProgram* (*)(void* context) noexcept;
// Compile a bounded animation frame into the hardware paint program. Canvas coordinates are normalized so the larger extent is one.
// JSON and temporary native geometry are released before this call returns.
// The callback supplies a caller-owned program after metadata and relative-frame validation.
// The compiler calls the callback once and retains no reference after this call.
bool lottie_program_compile_frame(ConstSlice<u8> json, u32 frame_index, float pixels,
                                  LottieTiming* timing, LottieProgramAcquire acquire, void* context,
                                  float* source_width = nullptr) noexcept;
} // namespace maicord
