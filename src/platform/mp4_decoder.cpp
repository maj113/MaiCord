#include "maicord/platform/mp4_decoder.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/platform/hevc_gpu_decoder.hpp"
#include "maicord/media/hevc_mp4_demux.hpp"
#include "maicord/sys/d3d11.hpp"

namespace maicord {
bool mp4_decoder_initialize_fragmented(Mp4Decoder* decoder, Mp4FragmentSource* source,
                                       ID3D11Device* device) noexcept {
    if (!decoder || decoder->ready || decoder->native.state || !source || !device) return false;
    ID3D11DeviceContext* context{};
    device->GetImmediateContext(&context);
    const bool ready = context && h264_gpu_decoder_initialize_fragmented(&decoder->native, source, device, context);
    com_release(context);
    if (ready) { decoder->ready = decoder->fragmented = true; decoder->hevc = false; decoder->gpu = nullptr; }
    return ready;
}
bool mp4_decoder_initialize(Mp4Decoder* decoder, ConstSlice<u8> encoded, u16 requested_width,
                            u16 requested_height, ID3D11Device* video_device,
                            Mp4SourceAvailability availability) noexcept {
    if (!decoder)
        return false;
    mp4_decoder_release(decoder);
    // Bound an unreadable oversize source before walking any box metadata.
    if (encoded.count >
        (availability.available ? kMediaMaximumProgressiveVideoBytes : kMp4DemuxMaximumEncodedBytes)) {
        decoder->native.status = E_INVALIDARG;
        decoder->native.parse_error = H264Error::unsupported;
        decoder->native.demux_error = Mp4DemuxError::unsupported;
        return false;
    }
    decoder->hevc = hevc_container_present(encoded);
    const u32 maximum_dimension = decoder->hevc ? 4096u : kMp4DecodeMaximumDimension;
    if (!video_device || !encoded.data || !encoded.count ||
        ((requested_width == 0) != (requested_height == 0)) || requested_width > maximum_dimension ||
        requested_height > maximum_dimension)
        return false;
    ID3D11DeviceContext* context = nullptr;
    video_device->GetImmediateContext(&context);
    decoder->ready =
        context &&
        (decoder->hevc
             ? hevc_gpu_decoder_initialize(&decoder->native, encoded, video_device, context, true, availability)
             : h264_gpu_decoder_initialize(&decoder->native, encoded, video_device, context, true,
                                           availability));
    com_release(context);
    return decoder->ready;
}

ConstSlice<u8> mp4_decoder_source(const Mp4Decoder& decoder) noexcept {
    return decoder.ready ? (decoder.hevc ? hevc_gpu_decoder_source(decoder.native)
                                         : h264_gpu_decoder_source(decoder.native))
                         : ConstSlice<u8>{};
}
H264GpuMovie mp4_decoder_movie(const Mp4Decoder& decoder) noexcept {
    return decoder.ready ? (decoder.hevc ? hevc_gpu_decoder_movie(decoder.native)
                                         : h264_gpu_decoder_movie(decoder.native))
                         : H264GpuMovie{};
}
bool mp4_decoder_busy(const Mp4Decoder& decoder) noexcept {
    return decoder.hevc ? hevc_gpu_decoder_busy(decoder.native) : h264_gpu_decoder_busy(decoder.native);
}
bool mp4_decoder_wait(Mp4Decoder* decoder, u32 timeout_ms) noexcept {
    return decoder && (decoder->hevc ? hevc_gpu_decoder_wait(&decoder->native, timeout_ms)
                                     : h264_gpu_decoder_wait(&decoder->native, timeout_ms));
}
bool mp4_decoder_acknowledge_frame(Mp4Decoder* decoder) noexcept {
    if (!decoder || !decoder->ready ||
        !(decoder->hevc ? hevc_gpu_decoder_acknowledge_frame(&decoder->native)
                        : h264_gpu_decoder_acknowledge_frame(&decoder->native)))
        return false;
    decoder->gpu = nullptr;
    return true;
}
Mp4DemuxError mp4_decoder_seek(Mp4Decoder* decoder, i64 target, Mp4SeekPoint* point) noexcept {
    if (!decoder || !decoder->ready || !point)
        return Mp4DemuxError::invalid_input;
    const ConstSlice<u8> output{reinterpret_cast<const u8*>(point), sizeof(*point)};
    if (!byte_range_valid(output) ||
        byte_ranges_overlap(output, {reinterpret_cast<const u8*>(decoder), sizeof(*decoder)}))
        return Mp4DemuxError::invalid_input;
    const auto result = decoder->hevc ? hevc_gpu_decoder_seek(&decoder->native, target, point)
                                      : h264_gpu_decoder_seek(&decoder->native, target, point);
    if (result == Mp4DemuxError::none)
        decoder->gpu = nullptr;
    return result;
}
bool mp4_decoder_seek_start(Mp4Decoder* decoder) noexcept {
    if (!decoder || !decoder->ready ||
        !(decoder->hevc ? hevc_gpu_decoder_seek_start(&decoder->native)
                        : h264_gpu_decoder_seek_start(&decoder->native)))
        return false;
    decoder->gpu = nullptr;
    return true;
}
Mp4DecodedFrame mp4_decoder_read_selected(Mp4Decoder* decoder) noexcept {
    Mp4DecodedFrame frame{};
    frame.error = Mp4DecodeError::invalid_input;
    if (!decoder || !decoder->ready || decoder->gpu)
        return frame;
    frame = decoder->hevc ? hevc_gpu_decoder_read(&decoder->native, false, 4)
                          : h264_gpu_decoder_read(&decoder->native, false, 4);
    if (frame.ok())
        decoder->gpu = frame.gpu;
    return frame;
}

Mp4DecodedFrame mp4_decoder_read(Mp4Decoder* decoder, bool loop_at_end) noexcept {
    Mp4DecodedFrame frame{};
    frame.error = Mp4DecodeError::invalid_input;
    if (!decoder || !decoder->ready)
        return frame;
    // Generic reads retain their existing implicit lease handoff. Selected
    // playback uses read_selected and its explicit renderer acknowledgement.
    if (decoder->fragmented && decoder->gpu && !mp4_decoder_acknowledge_frame(decoder)) return frame;
    const auto movie = mp4_decoder_movie(*decoder);
    u32 remaining = decoder->fragmented ? 64u : movie.samples;
    bool rewound = false;
    for (;;) {
        if (!remaining) {
            frame.error = decoder->fragmented ? Mp4DecodeError::needs_service : Mp4DecodeError::end_of_stream;
        } else {
            for (u32 attempt = 0; attempt < 4; ++attempt) {
                frame = decoder->hevc ? hevc_gpu_decoder_read(&decoder->native, false)
                                      : h264_gpu_decoder_read(&decoder->native, false, decoder->fragmented ? 4u : 0u);
                if (frame.ok())
                    break;
                if (!mp4_decoder_busy(*decoder) || attempt == 3 || !mp4_decoder_wait(decoder, 16))
                    break;
            }
        }
        if (frame.ok()) {
            --remaining;
            const u64 source = frame.source_duration_us ? frame.source_duration_us : frame.duration_us;
            const auto interval =
                mp4_frame_interval(frame.presentation_time_us, source, movie.duration_us, movie.edited);
            if (interval.visibility == Mp4FrameVisibility::before) {
                if (decoder->fragmented) h264_gpu_decoder_acknowledge_frame(&decoder->native);
                continue;
            }
            if (interval.visibility == Mp4FrameVisibility::visible) {
                frame.source_duration_us = source;
                frame.duration_us = interval.visible_duration_us;
                decoder->gpu = frame.gpu;
                return frame;
            }
            frame.error = interval.visibility == Mp4FrameVisibility::after ? Mp4DecodeError::end_of_stream
                                                                           : Mp4DecodeError::stream;
        }
        if (frame.error == Mp4DecodeError::end_of_stream && loop_at_end && !rewound && (movie.samples || decoder->fragmented) &&
            (decoder->hevc ? hevc_gpu_decoder_seek_start(&decoder->native)
                           : h264_gpu_decoder_rewind(&decoder->native))) {
            rewound = true;
            remaining = decoder->fragmented ? 64u : movie.samples;
            continue;
        }
        if (decoder->fragmented && frame.error == Mp4DecodeError::end_of_stream && loop_at_end && !rewound &&
            decoder->native.demux_error != Mp4DemuxError::none)
            frame.error = decoder->native.demux_error == Mp4DemuxError::data_pending
                ? Mp4DecodeError::data_pending : Mp4DecodeError::stream;
        break;
    }
    if (frame.error != Mp4DecodeError::end_of_stream && frame.error != Mp4DecodeError::data_pending &&
        frame.error != Mp4DecodeError::needs_service) {
        const i32 status = decoder->native.status;
        const H264Error parse_error = decoder->native.parse_error;
        const Mp4DemuxError demux_error = decoder->native.demux_error;
        mp4_decoder_release(decoder);
        decoder->native.status = status;
        decoder->native.parse_error = parse_error;
        decoder->native.demux_error = demux_error;
    }
    return frame;
}

void mp4_decoder_release(Mp4Decoder* decoder) noexcept {
    if (!decoder)
        return;
    if (decoder->hevc)
        hevc_gpu_decoder_release(&decoder->native);
    else
        h264_gpu_decoder_release(&decoder->native);
    reset_value(decoder);
}
} // namespace maicord
