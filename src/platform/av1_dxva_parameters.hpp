#pragma once

#include "maicord/media/av1.hpp"
#include "maicord/sys/dxva.hpp"

namespace maicord {

// DXVA picture parameters for header decoded into surface; slot_surface maps
// reference slots to surfaces (255: empty).
void av1_dxva_picture_parameters(const Av1SequenceHeader& sequence, const Av1FrameHeader& header,
                                 const u8 (&slot_surface)[kAv1RefFrames], const Av1References& references,
                                 u8 surface, DXVA_PicParams_AV1* output) noexcept;
// Spec 7.11.3.6 setup_shear(): whether a warp model is usable.
bool av1_warp_valid(const i32 (&parameters)[6]) noexcept;

} // namespace maicord
