#pragma once
#include "maicord/core/utf.hpp"

namespace maicord {
using NativeThaiSegmentSink=void(*)(void*,u8) noexcept;
// Internal byte-alphabet segmentation contract. Emits positive encoded segment
// lengths, stopping at the first zero. The caller owns Unicode itemization and
// boundary projection; no input-sized scratch is retained.
bool native_thai_forward_segments(ConstSlice<u32> text,void* context,NativeThaiSegmentSink sink) noexcept;
}
