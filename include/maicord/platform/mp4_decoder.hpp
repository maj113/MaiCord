#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/media/gpu_video_frame.hpp"
#include "maicord/platform/h264_gpu_decoder.hpp"
#include "maicord/sys/d3d11.hpp"

struct ID3D11Device;

namespace maicord {

inline constexpr u32 kMp4DecodeMaximumDimension = 2'048;
enum class Mp4FrameVisibility : u8 { invalid, before, visible, after };
struct Mp4FrameInterval { Mp4FrameVisibility visibility;u64 visible_duration_us; };
// Pure checked classification shared by mp4_frame_interval and the test fixtures. Keep
// the source PTS unchanged; a frame beginning before 0 may still cover movie time 0.
inline Mp4FrameInterval mp4_frame_interval_from(i64 pts,u64 duration,i64 target,u64 endpoint,bool edited) noexcept {
    constexpr i64 limit=0x7fffffffffffffffll;
    if(target<0||!duration||duration>static_cast<u64>(limit)||(edited&&endpoint>static_cast<u64>(limit))||
        (pts>=0&&duration>static_cast<u64>(limit-pts)))return {Mp4FrameVisibility::invalid,0};
    const i64 end=pts+static_cast<i64>(duration);
    if(end<=target)return {Mp4FrameVisibility::before,0};
    const u64 start=pts<target?static_cast<u64>(target):static_cast<u64>(pts);
    if(edited&&start>=endpoint)return {Mp4FrameVisibility::after,0};
    const u64 visible_end=edited?minimum(static_cast<u64>(end),endpoint):static_cast<u64>(end);
    return {Mp4FrameVisibility::visible,visible_end-start};
}
inline Mp4FrameInterval mp4_frame_interval(i64 pts,u64 duration,u64 endpoint,bool edited) noexcept {
    return mp4_frame_interval_from(pts,duration,0,endpoint,edited);
}

enum class Mp4DecodeError : u8 {
    none,
    invalid_input,
    runtime,
    stream,
    reader,
    media_type,
    dimensions,
    memory,
    sample,
    buffer,
    end_of_stream,
    needs_service,
    // Progressive source: the next sample has not arrived; decoder state is kept.
    data_pending,
};

struct Mp4DecodedFrame {
    ConstSlice<u8> rgba8;
    u64 duration_us;
    u32 width;
    u32 height;
    u32 row_pitch;
    Mp4DecodeError error;
    const GpuVideoFrame* gpu;
    // Natural layout extent; width/height above remain cropped pixel counts.
    u16 display_width;
    u16 display_height;
    // Signed movie PTS of the selected access unit, preserved across
    // decode/picture-order reordering. Negative values are decode preroll.
    i64 presentation_time_us;
    // Source interval before movie-start/end clipping. duration_us is the
    // visible interval used by animation callers; the PTS is not clipped.
    u64 source_duration_us;

    constexpr bool ok() const noexcept {
        return error == Mp4DecodeError::none;
    }
};

// One native GPU decoder per media worker. No CPU pixel buffer, MFT, source
// reader or per-frame allocation. The borrowed texture lasts until the next read;
// presentation must enqueue its GPU conversion before the worker is released.
struct Mp4Decoder {
    H264GpuDecoder native;
    const GpuVideoFrame* gpu;
    bool ready;
    bool hevc;
};

// A non-null availability borrows a sparse progressive source in place (its
// owner must outlive release); its first sample must already be resident.
bool mp4_decoder_initialize(Mp4Decoder* decoder, ConstSlice<u8> encoded,
                            u16 requested_width, u16 requested_height,
                            ID3D11Device* video_device,
                            Mp4SourceAvailability availability = {}) noexcept;
// Worker-only source borrow/metadata. The native decoder keeps the source
// until release; neither accessor allocates, reparses or creates a device.
ConstSlice<u8> mp4_decoder_source(const Mp4Decoder&) noexcept;
H264GpuMovie mp4_decoder_movie(const Mp4Decoder&) noexcept;
bool mp4_decoder_busy(const Mp4Decoder&) noexcept;
bool mp4_decoder_wait(Mp4Decoder*, u32 timeout_ms) noexcept;
// The renderer's copy acknowledgement must reach the codec worker before seek.
// The gpu pointer from the last read is invalid after acknowledgement.
// Idempotent for a ready decoder whose last frame is already acknowledged.
bool mp4_decoder_acknowledge_frame(Mp4Decoder*) noexcept;
// Selects the IDR at or before the target; no decode or allocation here. The caller
// then reads with loop_at_end=false and discards only complete preroll intervals
// ending at or before the target, keeping a frame whose interval covers the target.
Mp4DemuxError mp4_decoder_seek(Mp4Decoder*,i64 movie_time_us,Mp4SeekPoint*) noexcept;
// Resets for start/replay; requires the last frame to be acknowledged. Also
// permits decoding the first future frame while a leading empty edit elapses.
bool mp4_decoder_seek_start(Mp4Decoder*) noexcept;
// One native read, at most 4 submitted access units; no preroll skip, EOF loop, wait
// or implicit release on failure. needs_service means the caller should yield and
// call again; sample error plus h264_gpu_decoder_busy is real device backpressure.
// Signed/source intervals are not clipped; the caller does its own clipping.
Mp4DecodedFrame mp4_decoder_read_selected(Mp4Decoder*) noexcept;
// Call only on a decode worker. Busy hardware is handled with event waits;
// a terminal failure releases the decoder instead of restarting or falling back.
Mp4DecodedFrame mp4_decoder_read(Mp4Decoder* decoder, bool loop_at_end = true) noexcept;
void mp4_decoder_release(Mp4Decoder* decoder) noexcept;

static_assert(sizeof(Mp4Decoder) <= 32);

} // namespace maicord
