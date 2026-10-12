#pragma once
#include "maicord/core/slice.hpp"
namespace maicord {
bool h264_state_probe(ConstSlice<u8> encoded, void (*metric)(const char*, u64)) noexcept;
}
