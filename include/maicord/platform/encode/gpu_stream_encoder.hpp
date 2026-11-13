#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/platform/capture/screen_capture.hpp"
#include "maicord/platform/decode/gpu_stream_decoder.hpp"

namespace maicord {

// The live (Go Live) video encoder: captured pictures in, access units for the stream sender out. Everything happens
// on the GPU: the BGRA picture is scaled and converted to NV12 there and encoded by the GPU's own encoder, never by
// software (Windows: hardware Media Foundation transforms on Direct3D 11 textures; Linux: VA-API from the dma-buf).
// Nothing assumes a screen: any producer of CaptureFrame pictures (a camera too) feeds it the same way.
//
// Codecs are tiered like the decoders': AV1, then H.265, then H.264, the order Discord prefers. A device without a
// hardware H.264 encoder cannot stream at all.

// One encoded picture.
//   H.264, H.265: Annex B with 4-byte start codes. A keyframe (IDR) starts with its parameter sets (H.265: VPS, SPS,
//                 PPS; H.264: SPS, PPS), so each keyframe is decodable alone.
//   AV1:          the temporal unit's OBUs, each with obu_size, without the temporal delimiter. A keyframe starts with
//                 the sequence header.
// No B-frames and no reordering: units come in capture order, one per submitted picture, each referencing only
// earlier ones.
struct GpuStreamAccessUnit {
    ConstSlice<u8> data;  // valid during the callback only
    u64 timestamp_us;     // the picture's CaptureFrame::timestamp_us
    u32 rtp_timestamp;    // 90 kHz from timestamp_us (wraps)
    u16 width, height;    // the encoded picture (the stream's resolution)
    GpuStreamCodec codec;
    bool keyframe;
};

enum class GpuStreamEncoderEnd : u8 {
    failed,      // the encoder stopped working (a driver error); nothing more is encoded
    device_lost, // the GPU was removed or reset
};

// Callbacks run on the encoder's own thread, one at a time, and must not call gpu_stream_encoder_destroy.
struct GpuStreamEncoderSink {
    void* context;
    void (*unit)(void* context, const GpuStreamAccessUnit& unit) noexcept;
    // The last callback: the encoder can no longer produce units (create a new one, or end the stream).
    void (*ended)(void* context, GpuStreamEncoderEnd reason) noexcept;
};

// What a stream asks of the encoder; every field can change while it runs.
struct GpuStreamEncoderSettings {
    // The encoded extent (stream_encode_extent): each picture is scaled to it. Even, at most 4096 x 2304 pixels.
    u32 width, height;
    u32 max_fps;     // pictures closer together than 1 / max_fps are dropped (by their timestamps)
    u32 bitrate;     // target bits per second (rate control holds the average near it, with low delay)
};

struct GpuStreamEncoderOptions {
    // Windows: the device CaptureFrame textures live on (the renderer's multithread-protected device). Linux: the
    // renderer's device; the VA display is the one the decoders use (the GPU it renders on).
    GpuDevice* device;
    GpuStreamCodec codec; // one of gpu_stream_encoder_codecs
    GpuStreamEncoderSettings settings;
    GpuStreamEncoderSink sink;
};

struct GpuStreamEncoderStats {
    u32 submitted;  // pictures handed to submit
    u32 dropped;    // refused: faster than max_fps, or the encoder was still busy with earlier ones
    u32 encoded;    // units delivered
    u32 keyframes;  // of them, keyframes
    u32 reconfigurations; // extent changes (each restarts the stream with a keyframe)
    u64 bytes;      // of the delivered units
    u32 failure;    // the system's status (HRESULT, VA status) that ended the encoder; 0 while it runs
};

// gpu_stream_codec_bit set of the codecs the device encodes in hardware, from 8-bit NV12, at least up to 1920x1080.
// Probed once per device and kept.
u32 gpu_stream_encoder_codecs(GpuDevice* device) noexcept;

// The codec to stream with: the best of AV1, H.265, H.264 in both sets (gpu_stream_codec_bit values: what the GPU
// encodes and what the stream accepts). False when none is common; a stream without H.264 hardware can never start.
constexpr bool gpu_stream_encoder_pick(u32 encodable, u32 accepted, GpuStreamCodec* codec) noexcept {
    constexpr GpuStreamCodec kOrder[]{GpuStreamCodec::av1, GpuStreamCodec::h265, GpuStreamCodec::h264};
    for (const GpuStreamCodec candidate : kOrder)
        if (encodable & accepted & gpu_stream_codec_bit(candidate)) {
            *codec = candidate;
            return true;
        }
    return false;
}

struct GpuStreamEncoder;
// Creates the hardware encoder and its thread. Null when the codec is not encodable or the encoder would not start.
GpuStreamEncoder* gpu_stream_encoder_create(const GpuStreamEncoderOptions& options) noexcept;

// The capture's thread, inside CaptureSink::frame: scales and converts the picture on the GPU into the encoder's own
// surface before returning (keeping nothing of the frame), and encodes it asynchronously. False when the picture was
// dropped (see GpuStreamEncoderStats::dropped) or the encoder has ended.
bool gpu_stream_encoder_submit(GpuStreamEncoder* encoder, const CaptureFrame& frame) noexcept;

// Any thread. Settings apply from the next picture; a new extent restarts the stream with a keyframe.
void gpu_stream_encoder_configure(GpuStreamEncoder* encoder, const GpuStreamEncoderSettings& settings) noexcept;
// Any thread: bandwidth estimation's target, bits per second.
void gpu_stream_encoder_set_bitrate(GpuStreamEncoder* encoder, u32 bitrate) noexcept;
// Any thread (PLI, FIR): the next unit is a keyframe. When the capture has sent nothing for 100 ms (or a frame
// interval, whichever is longer: a still screen), the newest picture is encoded again as that keyframe at once.
void gpu_stream_encoder_request_keyframe(GpuStreamEncoder* encoder) noexcept;
// Any thread. While held, pictures are still taken and converted (the self-preview stays current) but none is encoded:
// a stream nobody watches costs the video engine nothing. Releasing encodes the newest picture as a keyframe.
void gpu_stream_encoder_hold(GpuStreamEncoder* encoder, bool hold) noexcept;
// The renderer's thread, for the self-preview: the newest converted picture at the encoded extent (NV12, limited
// range; BT.709, or BT.601 below 720 lines on Linux, as the frame's bt709 says), its texture and both plane views each
// carrying one reference the caller releases. That picture is not
// written again until the next call, which replaces the hold, so a reader that enqueues its conversion before calling
// again always reads complete pictures. False before the first picture. *sequence counts the pictures made the
// newest so far (it changes with every new picture, so a reader presents a picture only when it differs from the
// sequence it showed last).
bool gpu_stream_encoder_preview(GpuStreamEncoder* encoder, GpuVideoFrame* frame, u32* sequence) noexcept;

GpuStreamEncoderStats gpu_stream_encoder_stats(const GpuStreamEncoder* encoder) noexcept;
// Joins the encoder thread (no callback runs after it returns) and releases everything. Null is fine.
void gpu_stream_encoder_destroy(GpuStreamEncoder* encoder) noexcept;

} // namespace maicord
