#include "maicord/app/video_stream_decoders.hpp"
#include "maicord/platform/h264_gpu_stream_decoder.hpp"
#include <d3d11.h>

namespace maicord {
namespace {

void retain_frame(const GpuVideoFrame& frame) noexcept {
    if (frame.texture) frame.texture->AddRef();
    if (frame.luma) frame.luma->AddRef();
    if (frame.chroma) frame.chroma->AddRef();
}

void release_frame(GpuVideoFrame& frame) noexcept {
    com_release(frame.chroma);
    com_release(frame.luma);
    com_release(frame.texture);
}

VideoStreamDecoders::Stream* find(VideoStreamDecoders* decoders, u32 ssrc) noexcept {
    for (auto& stream : decoders->streams)
        if (stream.decoder && stream.ssrc == ssrc) return &stream;
    return nullptr;
}

void request(VideoStreamDecoders* decoders, u32 ssrc) noexcept {
    AcquireSRWLockShared(decoders->lock.native());
    const VideoKeyframeRequester requester = decoders->keyframes;
    if (requester.request_keyframe) requester.request_keyframe(requester.context, ssrc);
    ReleaseSRWLockShared(decoders->lock.native());
}

// Decode thread: publish the newest picture of this SSRC.
void on_frame(void* context, u32 ssrc, const H264StreamFrame& frame) noexcept {
    auto* decoders = static_cast<VideoStreamDecoders*>(context);
    if (!frame.gpu || !frame.gpu->texture) return;
    AcquireSRWLockExclusive(decoders->lock.native());
    if (auto* stream = find(decoders, ssrc)) {
        if (stream->latest.texture != frame.gpu->texture || stream->latest.luma != frame.gpu->luma ||
            stream->latest.chroma != frame.gpu->chroma) {
            retain_frame(*frame.gpu);
            release_frame(stream->latest);
        }
        stream->latest = *frame.gpu;
        stream->rtp_timestamp = frame.rtp_timestamp;
        stream->display_width = frame.display_width;
        stream->display_height = frame.display_height;
        stream->keyframe = frame.keyframe;
        ++stream->sequence;
    }
    ReleaseSRWLockExclusive(decoders->lock.native());
}

void on_keyframe(void* context, u32 ssrc) noexcept {
    request(static_cast<VideoStreamDecoders*>(context), ssrc);
}

// Removes a stream under the lock; the decoder is destroyed afterwards because
// joining its thread while holding the lock could deadlock with on_frame.
H264StreamDecoder* detach(VideoStreamDecoders::Stream* stream) noexcept {
    H264StreamDecoder* decoder = stream->decoder;
    release_frame(stream->latest);
    *stream = {};
    return decoder;
}

bool push_frame(void* context, u32 ssrc, ConstSlice<u8> frame, u32 rtp_timestamp, bool keyframe,
                bool intact) noexcept {
    auto* decoders = static_cast<VideoStreamDecoders*>(context);
    if (!decoders || !ssrc || !frame.data || !frame.count) return false;
    AcquireSRWLockShared(decoders->lock.native());
    auto* stream = find(decoders, ssrc);
    // push copies into the decoder queue; the worker is the only creator and
    // destroyer, so the decoder outlives this shared section.
    const bool pushed = stream && h264_stream_decoder_push(stream->decoder, frame, rtp_timestamp, intact);
    ID3D11Device* device = decoders->device;
    if (!stream && device) device->AddRef();
    ReleaseSRWLockShared(decoders->lock.native());
    if (stream) return pushed;
    if (!device || !keyframe) {
        // Decoding can only begin at a keyframe: ask for one now and then at
        // most every kH264StreamKeyframeIntervalMs while this SSRC waits.
        if (device) device->Release();
        const u64 now = GetTickCount64();
        u32 slot = 0;
        for (u32 i = 0; i < kVideoStreamDecoderStreams; ++i) {
            if (decoders->waiting_ssrc[i] == ssrc) { slot = i; break; }
            if (decoders->waiting_ms[i] < decoders->waiting_ms[slot]) slot = i;
        }
        if (decoders->waiting_ssrc[slot] == ssrc && now - decoders->waiting_ms[slot] < kH264StreamKeyframeIntervalMs)
            return false;
        decoders->waiting_ssrc[slot] = ssrc;
        decoders->waiting_ms[slot] = now ? now : 1;
        request(decoders, ssrc);
        return false;
    }
    for (u32 i = 0; i < kVideoStreamDecoderStreams; ++i)
        if (decoders->waiting_ssrc[i] == ssrc) { decoders->waiting_ssrc[i] = 0; decoders->waiting_ms[i] = 0; }
    H264StreamDecoder* decoder =
        h264_stream_decoder_create({device, ssrc, {decoders, on_frame, on_keyframe}, false});
    device->Release(); // The decoder keeps its own reference.
    if (!decoder) return false;
    AcquireSRWLockExclusive(decoders->lock.native());
    VideoStreamDecoders::Stream* slot = nullptr;
    for (auto& candidate : decoders->streams)
        if (!candidate.decoder) {
            slot = &candidate;
            break;
        }
    if (slot) {
        *slot = {};
        slot->decoder = decoder;
        slot->ssrc = ssrc;
    }
    ReleaseSRWLockExclusive(decoders->lock.native());
    if (!slot) {
        h264_stream_decoder_destroy(decoder);
        return false;
    }
    return h264_stream_decoder_push(decoder, frame, rtp_timestamp);
}

void end_stream(void* context, u32 ssrc) noexcept {
    auto* decoders = static_cast<VideoStreamDecoders*>(context);
    if (!decoders) return;
    AcquireSRWLockExclusive(decoders->lock.native());
    auto* stream = find(decoders, ssrc);
    H264StreamDecoder* decoder = stream ? detach(stream) : nullptr;
    ReleaseSRWLockExclusive(decoders->lock.native());
    h264_stream_decoder_destroy(decoder);
}

} // namespace

void video_stream_decoders_initialize(VideoStreamDecoders* decoders) noexcept {
    if (!decoders) return;
    *decoders = {};
    InitializeSRWLock(decoders->lock.native());
}

void video_stream_decoders_set_device(VideoStreamDecoders* decoders, ID3D11Device* device) noexcept {
    if (!decoders) return;
    if (device) device->AddRef();
    AcquireSRWLockExclusive(decoders->lock.native());
    ID3D11Device* previous = decoders->device;
    decoders->device = device;
    ReleaseSRWLockExclusive(decoders->lock.native());
    if (previous) previous->Release();
}

void video_stream_decoders_set_requester(VideoStreamDecoders* decoders, VideoKeyframeRequester requester) noexcept {
    if (!decoders) return;
    AcquireSRWLockExclusive(decoders->lock.native());
    decoders->keyframes = requester;
    ReleaseSRWLockExclusive(decoders->lock.native());
}

VideoFrameSink video_stream_decoders_sink(VideoStreamDecoders* decoders) noexcept {
    return {decoders, push_frame, end_stream};
}

bool video_stream_decoders_frame(VideoStreamDecoders* decoders, u32 ssrc, VideoStreamFrame* output) noexcept {
    if (!decoders || !ssrc || !output) return false;
    bool found = false;
    AcquireSRWLockShared(decoders->lock.native());
    if (const auto* stream = find(decoders, ssrc); stream && stream->latest.texture) {
        retain_frame(stream->latest);
        *output = {stream->latest, ssrc, stream->rtp_timestamp, stream->sequence, stream->display_width,
                   stream->display_height, stream->keyframe, {}};
        found = true;
    }
    ReleaseSRWLockShared(decoders->lock.native());
    return found;
}

void video_stream_decoders_release(VideoStreamDecoders* decoders) noexcept {
    if (!decoders) return;
    H264StreamDecoder* detached[kVideoStreamDecoderStreams]{};
    AcquireSRWLockExclusive(decoders->lock.native());
    for (u32 i = 0; i < kVideoStreamDecoderStreams; ++i)
        if (decoders->streams[i].decoder) detached[i] = detach(&decoders->streams[i]);
    decoders->keyframes = {};
    ID3D11Device* device = decoders->device;
    decoders->device = nullptr;
    ReleaseSRWLockExclusive(decoders->lock.native());
    for (auto* decoder : detached) h264_stream_decoder_destroy(decoder);
    if (device) device->Release();
}

} // namespace maicord
