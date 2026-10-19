#include "maicord/app/video_stream_decoders.hpp"
#include "maicord/app/voice_log.hpp"
#include "maicord/platform/gpu_stream_decoder.hpp"
#include "maicord/protocol/voice_video_signaling.hpp"
#include "maicord/sys/d3d11.hpp"

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
void on_frame(void* context, u32 ssrc, const GpuStreamFrame& frame) noexcept {
    auto* decoders = static_cast<VideoStreamDecoders*>(context);
    if (!frame.gpu || !frame.gpu->texture) return;
    AcquireSRWLockExclusive(decoders->lock.native());
    if (auto* stream = find(decoders, ssrc)) {
        retain_frame(*frame.gpu);
        release_frame(stream->latest);
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
GpuStreamDecoder* detach(VideoStreamDecoders::Stream* stream) noexcept {
    GpuStreamDecoder* decoder = stream->decoder;
    // Counts only, for checking real streams.
    const auto s = gpu_stream_decoder_stats(decoder);
    const VoiceLogField fields[]{{"codec", static_cast<u64>(stream->codec)},
                                 {"received", s.received},
                                 {"decoded", s.decoded},
                                 {"presented", s.presented},
                                 {"damaged", s.damaged},
                                 {"gaps", s.gaps},
                                 {"requests", s.keyframe_requests},
                                 {"reinits", s.reinitializations},
                                 {"gpu_failures", s.gpu_failures},
                                 {"unsupported", s.unsupported},
                                 {"width", s.coded_width},
                                 {"height", s.coded_height},
                                 {"surfaces", s.surfaces},
                                 {"upload_kib", s.uploaded_bytes >> 10}};
    voice_log_fields(s.gpu_failures || s.unsupported ? VoiceLogLevel::warning : VoiceLogLevel::info,
                     "video_decoder_summary", {fields, sizeof(fields) / sizeof(fields[0])});
    release_frame(stream->latest);
    *stream = {};
    return decoder;
}

bool decodable_codec(const VideoStreamDecoders& decoders, VoiceVideoCodec codec, GpuStreamCodec* output) noexcept {
    const u32 bits = static_cast<u32>(InterlockedCompareExchange(const_cast<volatile LONG*>(&decoders.decodable), 0, 0));
    if (!(bits & voice_video_codec_bit(codec))) return false;
    *output = codec == VoiceVideoCodec::av1    ? GpuStreamCodec::av1
              : codec == VoiceVideoCodec::h265 ? GpuStreamCodec::h265
                                               : GpuStreamCodec::h264;
    return true;
}

bool push_frame(void* context, u32 ssrc, VoiceVideoCodec codec, ConstSlice<u8> frame, u32 rtp_timestamp,
                bool keyframe, bool intact) noexcept {
    auto* decoders = static_cast<VideoStreamDecoders*>(context);
    if (!decoders || !ssrc || !frame.data || !frame.count) return false;
    AcquireSRWLockShared(decoders->lock.native());
    auto* stream = find(decoders, ssrc);
    const bool current = stream && stream->codec == codec;
    // push copies into the decoder queue; the worker is the only creator and
    // destroyer, so the decoder outlives this shared section.
    const bool pushed = current && gpu_stream_decoder_push(stream->decoder, frame, rtp_timestamp, intact);
    ID3D11Device* device = decoders->device;
    if (!current && device) device->AddRef();
    ReleaseSRWLockShared(decoders->lock.native());
    if (current) return pushed;
    GpuStreamCodec decoder_codec{};
    const bool supported = device && decodable_codec(*decoders, codec, &decoder_codec);
    if (!supported || !keyframe) {
        // Decoding can only begin (or change codec) at a keyframe: ask for one
        // now and then at most every kGpuStreamKeyframeIntervalMs while waiting.
        if (device) device->Release();
        if (!supported) return false;
        const u64 now = GetTickCount64();
        u32 slot = 0;
        for (u32 i = 0; i < kVideoStreamDecoderStreams; ++i) {
            if (decoders->waiting_ssrc[i] == ssrc) { slot = i; break; }
            if (decoders->waiting_ms[i] < decoders->waiting_ms[slot]) slot = i;
        }
        if (decoders->waiting_ssrc[slot] == ssrc && now - decoders->waiting_ms[slot] < kGpuStreamKeyframeIntervalMs)
            return false;
        decoders->waiting_ssrc[slot] = ssrc;
        decoders->waiting_ms[slot] = now ? now : 1;
        request(decoders, ssrc);
        return false;
    }
    for (u32 i = 0; i < kVideoStreamDecoderStreams; ++i)
        if (decoders->waiting_ssrc[i] == ssrc) { decoders->waiting_ssrc[i] = 0; decoders->waiting_ms[i] = 0; }
    GpuStreamDecoder* decoder =
        gpu_stream_decoder_create({device, ssrc, {decoders, on_frame, on_keyframe}, decoder_codec, false});
    device->Release(); // The decoder keeps its own reference.
    if (!decoder) return false;
    GpuStreamDecoder* replaced = nullptr;
    AcquireSRWLockExclusive(decoders->lock.native());
    VideoStreamDecoders::Stream* slot = find(decoders, ssrc);
    if (slot) replaced = detach(slot); // The stream switched codec at this keyframe.
    for (auto& candidate : decoders->streams)
        if (!slot && !candidate.decoder) slot = &candidate;
    if (slot) {
        *slot = {};
        slot->decoder = decoder;
        slot->ssrc = ssrc;
        slot->codec = codec;
    }
    ReleaseSRWLockExclusive(decoders->lock.native());
    gpu_stream_decoder_destroy(replaced);
    if (!slot) {
        gpu_stream_decoder_destroy(decoder);
        return false;
    }
    return gpu_stream_decoder_push(decoder, frame, rtp_timestamp);
}

void end_stream(void* context, u32 ssrc) noexcept {
    auto* decoders = static_cast<VideoStreamDecoders*>(context);
    if (!decoders) return;
    AcquireSRWLockExclusive(decoders->lock.native());
    auto* stream = find(decoders, ssrc);
    GpuStreamDecoder* decoder = stream ? detach(stream) : nullptr;
    ReleaseSRWLockExclusive(decoders->lock.native());
    gpu_stream_decoder_destroy(decoder);
}

u32 decodable(void* context) noexcept {
    auto* decoders = static_cast<VideoStreamDecoders*>(context);
    return static_cast<u32>(InterlockedCompareExchange(&decoders->decodable, 0, 0));
}

} // namespace

void video_stream_decoders_initialize(VideoStreamDecoders* decoders) noexcept {
    if (!decoders) return;
    *decoders = {};
    InitializeSRWLock(decoders->lock.native());
}

void video_stream_decoders_set_device(VideoStreamDecoders* decoders, ID3D11Device* device) noexcept {
    if (!decoders) return;
    const u32 codecs = device ? gpu_stream_decoder_codecs(device) : 0;
    u32 bits = 0;
    if (codecs & gpu_stream_codec_bit(GpuStreamCodec::h264)) bits |= voice_video_codec_bit(VoiceVideoCodec::h264);
    if (codecs & gpu_stream_codec_bit(GpuStreamCodec::h265)) bits |= voice_video_codec_bit(VoiceVideoCodec::h265);
    if (codecs & gpu_stream_codec_bit(GpuStreamCodec::av1)) bits |= voice_video_codec_bit(VoiceVideoCodec::av1);
    if (device) device->AddRef();
    AcquireSRWLockExclusive(decoders->lock.native());
    ID3D11Device* previous = decoders->device;
    decoders->device = device;
    InterlockedExchange(&decoders->decodable, static_cast<LONG>(bits));
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
    return {decoders, push_frame, end_stream, decodable};
}

bool video_stream_decoders_frame(VideoStreamDecoders* decoders, u32 ssrc, VideoStreamFrame* output) noexcept {
    if (!decoders || !ssrc || !output) return false;
    bool found = false;
    AcquireSRWLockShared(decoders->lock.native());
    if (const auto* stream = find(decoders, ssrc); stream && stream->latest.texture) {
        // Under the lock that orders on_frame: the decoder sees this hold
        // before it can pick a slice for its next picture.
        gpu_stream_decoder_hold(stream->decoder, stream->latest.subresource);
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
    GpuStreamDecoder* detached[kVideoStreamDecoderStreams]{};
    AcquireSRWLockExclusive(decoders->lock.native());
    for (u32 i = 0; i < kVideoStreamDecoderStreams; ++i)
        if (decoders->streams[i].decoder) detached[i] = detach(&decoders->streams[i]);
    decoders->keyframes = {};
    ID3D11Device* device = decoders->device;
    decoders->device = nullptr;
    InterlockedExchange(&decoders->decodable, 0);
    ReleaseSRWLockExclusive(decoders->lock.native());
    for (auto* decoder : detached) gpu_stream_decoder_destroy(decoder);
    if (device) device->Release();
}

} // namespace maicord
