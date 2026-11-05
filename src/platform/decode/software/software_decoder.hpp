#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/platform/decode/gpu_stream_decoder.hpp"

namespace maicord {

// The software tier of live video (Linux): libopenh264 for H.264 and libdav1d for AV1, dlopen()ed, decoding access
// units to 8-bit 4:2:0 pictures in CPU memory. Used from the decode thread of one stream; the libraries are never
// linked, so a missing one is only "not available". HEVC has no software decoder.
// A live stream (the default) is decoded for lowest latency: libopenh264 gives each unit's picture at once, in
// decoding order, so an H.264 stream with B slices is refused (unsupported); real-time senders do not use them.
// A movie (`reordering`, below) is decoded the other way: libopenh264 keeps pictures until its reorder buffer allows
// them out, in presentation order, and the decoder is drained at the end. MAICORD_DAV1D_LIBRARY and
// MAICORD_OPENH264_LIBRARY replace the sonames (an unusual install, or a test of the missing-library path); without
// them libdav1d.so.7 then .6 and libopenh264.so.7, .6 then .8 are tried.

// A decoded picture: planes of the visible extent (any crop is applied), valid until the next push, pop or destroy
// of the decoder that returned it.
struct SoftwarePicture {
    const u8* plane[3]; // Y, Cb, Cr
    u32 stride[3];
    u32 width, height;
    u8 matrix; // matrix_coefficients as coded (2 when unspecified)
    bool full_range;
    u16 sar_width, sar_height;
    u64 timestamp; // the timestamp of the unit that completed it (software_decoder_push)
};

enum class SoftwareStatus : u8 {
    ok,          // the unit was consumed; software_decoder_pop returns the pictures it completed
    lost,        // the unit refers to what the decoder never saw or is damaged: wait for a keyframe
    unsupported, // the stream is outside what is decoded here (bit depth, chroma format, size)
    failed,      // the library failed (out of memory, invalid state)
};

// True when the library for codec loads and exports every function used (the first call loads it).
bool software_codec_available(GpuStreamCodec codec) noexcept;

struct SoftwareDecoder;
// Null when the library is missing or refuses to open. `reordering` (H.264): the caller decodes a movie, with B
// slices allowed, and drains the decoder at the end; see above.
SoftwareDecoder* software_decoder_create(GpuStreamCodec codec, bool reordering = false) noexcept;
void software_decoder_destroy(SoftwareDecoder* decoder) noexcept;
// One access unit: H.264 Annex B with in-band parameter sets, or an AV1 temporal unit of OBUs. `timestamp` comes back
// with the picture libopenh264 completes of it (which a reordering decoder may show after later units).
SoftwareStatus software_decoder_push(SoftwareDecoder* decoder, ConstSlice<u8> unit, u64 timestamp = 0) noexcept;
// Next completed picture in output order; false when none is ready.
bool software_decoder_pop(SoftwareDecoder* decoder, SoftwarePicture* picture) noexcept;
// A reordering decoder only: the next picture it still holds back; false when it holds none. Call it until false at
// the end of a movie. libopenh264 shows the last unit's picture first, ahead of one its reorder buffer still holds,
// so the caller orders what a drain returns by its own presentation times.
bool software_decoder_drain(SoftwareDecoder* decoder, SoftwarePicture* picture) noexcept;
// Forgets references and queued pictures; decoding resumes at the next keyframe.
void software_decoder_reset(SoftwareDecoder* decoder) noexcept;

} // namespace maicord
