#pragma once

#include "maicord/media/video/av1.hpp"
#include "maicord/media/video/h264_slice_detail.hpp"
#include "maicord/media/video/hevc_references.hpp"
#include "maicord/media/video/hevc_slice_detail.hpp"
#include "maicord/media/video/h264_references.hpp"
#include "maicord/platform/decode/vaapi/abi.hpp"
#include "../hevc_gpu_references.hpp"

namespace maicord::vaapi {

// The VA-API buffers of a picture, from what the in-house parsers and reference trackers (src/media/video) decide.
// Pure functions of those values: they call no libva function, so tests build the same structs from fixture streams
// and compare them with the DXVA builders' output for the same pictures (tests/platform/decode/vaapi_parameters.cpp).
//
// `surfaces` maps the decoder's surface indices (what the reference trackers hold) to the VASurfaceIDs of the session;
// it needs an entry for every index a picture may name. Reference lists name pictures through the same mapping and
// mark the entries a slice does not use as invalid, as the specification of each buffer asks.

// ---- H.264 ------------------------------------------------------------------------------------------------------

// Picture parameters. `references` is the DPB before the picture's own marking; `order` its picture order counts.
void h264_picture_parameters(const H264Sps& sps, const H264Pps& pps, const H264Slice& first,
                             const H264ReferenceState& references, const H264PictureOrder& order, u8 surface,
                             const SurfaceId* surfaces, PictureParameterH264* output) noexcept;
// Scaling lists in raster order (the PPS keeps them in bitstream scan order, which DXVA takes as it is).
void h264_iq_matrix(const H264Pps& pps, IqMatrixH264* output) noexcept;
// One slice's parameters: its header detail, the reference lists built from `references` (8.2.4: initial order by
// frame number or picture order count, then the header's changes) and the extent of its data in the slice data buffer
// (the whole NAL unit at offset 0). False when a list change names a picture the DPB does not hold.
bool h264_slice_parameters(const H264Sps& sps, const H264Pps& pps, const H264Slice& first,
                           const H264SliceDetail& detail, const H264ReferenceState& references,
                           const H264PictureOrder& order, const SurfaceId* surfaces, u32 data_bytes,
                           SliceParameterH264* output) noexcept;

// ---- HEVC -------------------------------------------------------------------------------------------------------

// Picture parameters from the references mapped as the DXVA builder maps them (hevc_gpu_reference_map). `intra`: no
// slice of the picture is inter coded.
void hevc_picture_parameters(const HevcSps& sps, const HevcPps& pps, const HevcPictureHeader& picture, bool intra,
                             const HevcReferencePlan& plan, const HevcGpuReferences& references, u8 surface,
                             const SurfaceId* surfaces, PictureParameterHevc* output) noexcept;
// The scaling lists in syntax order; only valid when the SPS enables scaling lists.
void hevc_iq_matrix(const HevcPps& pps, IqMatrixHevc* output) noexcept;
// One slice segment's parameters. `last` marks the picture's last slice segment.
void hevc_slice_parameters(const HevcSliceDetail& detail, const HevcGpuReferences& references, bool last,
                           u32 data_bytes, SliceParameterHevc* output) noexcept;

// ---- AV1 --------------------------------------------------------------------------------------------------------

// `slot_surface` names the surface of each reference slot (255: none).
void av1_picture_parameters(const Av1SequenceHeader& sequence, const Av1FrameHeader& header,
                            const u8 (&slot_surface)[kAv1RefFrames], const SurfaceId* surfaces, u8 surface,
                            PictureParameterAv1* output) noexcept;
// One tile: `offset` and `size` within the slice data buffer.
void av1_tile_parameters(const Av1Tile& tile, u32 offset, u32 group_start, u32 group_end,
                         SliceParameterAv1* output) noexcept;

} // namespace maicord::vaapi
