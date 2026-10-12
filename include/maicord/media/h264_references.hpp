#pragma once

#include "maicord/media/h264_picture_order.hpp"

namespace maicord {

struct H264Reference {
    i32 top, bottom;
    u16 frame_num;
    u8 surface, long_term_index; // 0..15 long-term; 255 short-term.
    bool long_term() const noexcept { return long_term_index != 255; }
};
struct H264ReferenceState {
    H264Reference pictures[16];
    H264PocState poc;
    u16 last_reference_frame;
    u8 count, max_long_term_plus1;
    bool ready;
};
struct H264ReferencePlan {
    H264ReferenceState after;
    H264PictureOrder decode_order, retained_order;
    bool reset, discard_prior;
};

// CPU reference semantics only, separate from GPU/output leases. The caller
// must not reuse an unreferenced surface until display consumers also release
// it. Prepare against current state, submit using CURRENT references, then
// assign plan.after only if the GPU accepted the picture. No allocation.
H264Error h264_reference_plan(const H264ReferenceState& state, const H264Sps& sps,
    const H264Slice& slice, u8 surface, H264ReferencePlan* output) noexcept;

static_assert(sizeof(H264Reference) == 12);
static_assert(sizeof(H264ReferenceState) == 216);

} // namespace maicord
