#pragma once

#include "maicord/media/h264_slice.hpp"

namespace maicord {

// No mutation until the picture is accepted by the decoder. Failed GPU submits
// or repeated slices must not advance POC history. Frame-coded pictures only.
struct H264PocState {
    i32 reference_msb, reference_lsb, frame_offset;
    u16 frame_num;
    bool ready;
};
struct H264PictureOrder {
    i32 top, bottom, msb, frame_offset;
};
H264Error h264_picture_order(const H264PocState& state, const H264Sps& sps,
    const H264Slice& slice, H264PictureOrder* output) noexcept;
// Applies MMCO5 reset semantics only after successfully decoding the picture.
// Returns the normalized order for retained reference/output bookkeeping.
H264Error h264_picture_order_commit(H264PocState* state, const H264Slice& slice,
    H264PictureOrder* picture) noexcept;

static_assert(sizeof(H264PocState) == 16);

} // namespace maicord
