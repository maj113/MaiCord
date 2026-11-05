#pragma once

#include "va.hpp"
#include "va_parameters.hpp"

namespace maicord::vaapi {

// A picture of a movie through VA-API: one access unit of the container (the NAL units behind length prefixes, not Annex
// B) as one VA picture, its parameter buffers from the in-house parsers and one slice parameter and data buffer per slice
// NAL unit. They share what the live stream drivers (va_stream.cpp) do for their Annex B access units; only the walk over
// the NAL units differs. `references` is the DPB before the picture, as the parameter builders ask. False when a header
// does not parse, the references do not map or a buffer is refused (the picture is still ended).
bool submit_h264_sample(Session* session, const H264Sps& sps, const H264Pps& pps, const H264Picture& picture,
                        ConstSlice<u8> sample, u8 length_bytes, const H264ReferenceState& references,
                        const H264PictureOrder& order, u8 surface) noexcept;
bool submit_hevc_sample(Session* session, const HevcConfiguration& configuration, const HevcPicture& picture,
                        ConstSlice<u8> sample, const HevcReferenceState& references, const HevcReferencePlan& plan,
                        u8 surface) noexcept;

} // namespace maicord::vaapi
