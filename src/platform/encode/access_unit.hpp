#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/platform/decode/gpu_stream_decoder.hpp"

namespace maicord {

// The encoder's units in the form gpu_stream_encoder.hpp promises, whatever the hardware encoder wrote: Annex B with
// 4-byte start codes and no access unit delimiters (H.264, H.265), or OBUs each with obu_size and no temporal delimiter
// or padding (AV1), every keyframe led by the parameter sets or sequence header it needs.

// A growable buffer of committed pages, kept across units (no allocation per picture once it is large enough).
struct EncodedUnitBuffer {
    u8* data;
    u32 count, capacity;
};
bool encoded_unit_reserve(EncodedUnitBuffer* buffer, u32 bytes) noexcept;
void encoded_unit_release(EncodedUnitBuffer* buffer) noexcept;

// H.264 and H.265: `input` is Annex B with start codes of any length. A keyframe whose NAL units hold no SPS gets the
// NAL units of `parameter_sets` (Annex B, the encoder's codec configuration) first. False when nothing remains or
// memory runs out.
bool encoded_annexb_unit(GpuStreamCodec codec, ConstSlice<u8> input, ConstSlice<u8> parameter_sets, bool keyframe,
                         EncodedUnitBuffer* output) noexcept;

// AV1: `input` is a temporal unit of OBUs (the last may lack obu_size). A keyframe without a sequence header gets
// `sequence_header` (one whole OBU, as written) first. *found is the sequence header OBU of the output when it has
// one (pointing into output->data), for the caller to keep. False on a malformed unit, when nothing remains, or when
// memory runs out.
bool encoded_av1_unit(ConstSlice<u8> input, ConstSlice<u8> sequence_header, bool keyframe, EncodedUnitBuffer* output,
                      ConstSlice<u8>* found) noexcept;

} // namespace maicord
