#pragma once

#include <stddef.h>

#include "maicord/media/h264_parameters.hpp"
#include "maicord/media/mp4_demux.hpp"
#include "maicord/sys/d3d11.hpp"

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace maicord {

struct Mp4DecodedFrame;

// Native GPU backend. No MFT or software decode fallback. Output
// lease lasts until the next read, which must follow enqueueing presentation.
struct H264GpuDecoder {
    void* state;
    i32 status;
    H264Error parse_error;
    bool submitted;
    // Demux rejection reason; sits in the struct's padding so the size stays 16 bytes.
    Mp4DemuxError demux_error;
};
static_assert(sizeof(H264GpuDecoder) == 16);
static_assert(offsetof(H264GpuDecoder, demux_error) == 14);
struct H264GpuMovie {
    u64 duration_us;
    u32 samples;
    bool edited;
    i64 first_presentation_time_us;
    bool first_presentation_valid;
};
// Ready source only. First PTS comes from one packet on a reset cursor COPY;
// first_presentation_valid distinguishes failure/unavailable from an actual PTS of 0.
H264GpuMovie h264_gpu_decoder_movie(const H264GpuDecoder&) noexcept;
struct H264GpuDiagnostic {
    u32 next_sample_index;
    i64 next_sample_pts_us;
    u64 next_sample_dts_us;
    u32 queued_pictures;
    bool next_sample_valid;
};
// Numeric metadata only, worker-thread use; never exposes source pointers.
H264GpuDiagnostic h264_gpu_decoder_diagnostic(const H264GpuDecoder&) noexcept;
// Worker-only borrow of the decoder's source bytes. No second copy;
// invalid after release/reinitialization, but survives rewind/seek.
ConstSlice<u8> h264_gpu_decoder_source(const H264GpuDecoder&) noexcept;
// Call only after the renderer enqueued its copy on the protected shared
// context. Seeking playback acknowledges before the seek; plain reads keep the
// rule that the lease ends at the next read.
// Idempotent when a ready decoder has no outstanding lease.
bool h264_gpu_decoder_acknowledge_frame(H264GpuDecoder*) noexcept;
// Finds the preceding IDR on the signed movie timeline. Rejects an outstanding
// output lease; on failure the cursor, DPB, queue and point are unchanged.
// Success resets reference/output state without reallocating source/surfaces.
Mp4DemuxError h264_gpu_decoder_seek(H264GpuDecoder*, i64 movie_time_us, Mp4SeekPoint*) noexcept;
// Resets to the first IDR for first play/replay or a leading empty edit. This
// does not claim that an IDR exists before the requested movie time.
bool h264_gpu_decoder_seek_start(H264GpuDecoder*) noexcept;
// Worker only; uses the same demux/DPB reset as EOF looping. Any
// previous output lease has already expired because a new read is starting.
bool h264_gpu_decoder_rewind(H264GpuDecoder*) noexcept;
bool h264_gpu_decoder_initialize(H264GpuDecoder* probe, ConstSlice<u8> encoded, ID3D11Device* device,
                                 ID3D11DeviceContext* context, bool full_stream = true,
                                 Mp4SourceAvailability availability = {}) noexcept;
// Busy hardware returns sample error plus E_PENDING/D3DERR_WASSTILLDRAWING;
// caller waits for queued GPU work, then retries without advancing stream state.
// A zero submission budget means no limit. A nonzero budget accepts at most
// that many decoded access units, then returns needs_service, which is not GPU
// backpressure. Budgeted callers must acknowledge the prior frame before reading again.
Mp4DecodedFrame h264_gpu_decoder_read(H264GpuDecoder* probe, bool loop_at_end = false,
                                      u32 max_submissions = 0) noexcept;
bool h264_gpu_decoder_busy(const H264GpuDecoder& decoder) noexcept;
// Worker-only wait for GPU backpressure. Creates its fence/event on first use, never waits
// with the shared context lock held and never spins on a busy driver.
bool h264_gpu_decoder_wait(H264GpuDecoder* decoder, u32 timeout_ms) noexcept;
void h264_gpu_decoder_release(H264GpuDecoder* probe) noexcept;

} // namespace maicord
