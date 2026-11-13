#pragma once

#include "../gpu_stream_decoder_backend.hpp"

namespace maicord {

// The Linux backends of the live stream decoder (gpu_stream_decoder_backend.hpp), two tiers of decoder:
//
//   hardware  VA-API (src/platform/decode/vaapi): the in-house parsers of src/media/video build the VA parameter
//             and slice buffers, the decoder writes into VASurfaces that are exported as dma-bufs and imported as
//             EGLImages, so a picture is two zero-copy planes
//   software  libopenh264 and libdav1d (src/platform/decode/software): pictures in CPU memory, copied into planes
//             the renderer uploads
//
// Every backend runs on the decode thread only and never calls GL: the planes it publishes are made without GL
// (maicord/gpu/video_planes.hpp) and realized by the renderer's thread.

// Frees a slot's plane handles (the memory of a CPU slot is the planes').
void stream_slot_release(StreamSurface* slot) noexcept;
// Frees every slot's plane handles and memory.
void stream_surfaces_release(GpuStream* stream) noexcept;
// Replaces the planes of `slot` with the export of a VA surface (vaapi::export_picture): the old planes go once the
// new ones are in, and false leaves the slot as it was.
namespace vaapi {
struct Session;
}
bool stream_slot_export(GpuDevice* device, vaapi::Session* session, u32 surface, StreamSurface* slot) noexcept;
// CPU-backed slots (the software tier): `count` pictures of coded_width x coded_height (both even).
bool stream_surfaces_create_cpu(GpuStream* stream, u32 count, u32 coded_width, u32 coded_height) noexcept;
// Copies a decoded software picture into the CPU planes of slot and publishes them; the bytes written. The slot is at
// least as large as the picture (both planes, chroma interleaved).
struct SoftwarePicture;
u64 stream_copy_picture(const StreamSurface& slot, const SoftwarePicture& picture) noexcept;
// The movie decoders' (h264_file_decoder.cpp, ../hevc_gpu_decoder.cpp) picture slots: frees the planes of the first
// *count slots, then forgets them all.
void stream_slots_release(StreamSurface (&slots)[kStreamSurfaceLimit], u32* count) noexcept;
// Makes the first `wanted` slots CPU-backed pictures of width x height (both even), *count of them; false (every slot
// freed) when one could not be made.
bool stream_slots_create_cpu(GpuDevice* device, StreamSurface (&slots)[kStreamSurfaceLimit], u32* count, u32 wanted,
                             u32 width, u32 height) noexcept;
// The movie picture the output queue shows next, as *frame's planes: a hardware picture (session) is exported into
// its slot now (drivers differ in when a surface's dma-buf is complete, and a fresh export is always right).
struct H264OutputQueue;
struct Mp4DecodedFrame;
struct VideoDisplayExtent;
Mp4DecodedFrame stream_movie_frame(H264OutputQueue* output, GpuDevice* device, vaapi::Session* session,
                                   StreamSurface* slots, GpuVideoFrame* frame,
                                   const VideoDisplayExtent& display) noexcept;

// ---- Backends ----------------------------------------------------------------------------------------------------

extern const StreamBackend kSoftwareStreamBackend;
// The hardware backend of a codec (VA-API), null when this build or machine has none.
const StreamBackend* hardware_stream_backend(GpuStreamCodec codec) noexcept;
// The codecs the VA-API decoders can decode on the GPU `device` renders on (a gpu_stream_codec_bit set); the first
// call probes, and the answer is kept.
u32 hardware_probe_codecs(GpuDevice* device) noexcept;
// True when a keyframe is in the unit (H.264 IDR, AV1 sequence header): where a stream can start.
bool stream_unit_is_keyframe(GpuStreamCodec codec, ConstSlice<u8> bytes) noexcept;
// Gives a stream whose hardware backend has declared it unsupported (a profile, size or format the driver refuses)
// to the software tier, which then waits for a keyframe.
void stream_fall_back_to_software(GpuStream* stream) noexcept;

} // namespace maicord
