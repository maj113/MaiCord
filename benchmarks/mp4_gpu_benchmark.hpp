#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
int mp4_gpu_benchmark(ConstSlice<u8> encoded,
    void (*metric)(const char*, u64)) noexcept;
}
