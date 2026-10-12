#pragma once

#include "maicord/media/deflate.hpp"

namespace maicord::deflate_reference {
ZlibInflateResult zlib_inflate(ConstSlice<DeflateInputSpan> input, Slice<u8> output,
                               u32 expected_output_bytes) noexcept;
void stream_reset() noexcept;
ZlibInflateResult stream_flush(ConstSlice<DeflateInputSpan> input, Slice<u8> output) noexcept;
} // namespace maicord::deflate_reference
