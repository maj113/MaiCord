#include "maicord/core/win32_process.hpp"
#include "maicord/platform/h264_gpu_decoder.hpp"
#include "maicord/platform/mp4_decoder.hpp"
#include "h264_gpu_configuration.hpp"
#include "maicord/media/h264_references.hpp"
#include "maicord/media/h264_output_queue.hpp"
#include "maicord/media/video_display.hpp"
#include "maicord/media/mp4_fragment_source.hpp"
#include "maicord/media/mp4_segment_store.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/initguid.hpp"
#include "maicord/sys/dxva.hpp"
#include "h264_dxva_parameters.hpp"
#include "gpu_decoder_fence.hpp"
#include "gpu_decoder_surfaces.hpp"
// DXVA submission, once per frame: the slice copy is one memcpy per slice. Compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {

struct State {
    H264GpuPicture picture;
    Mp4Demuxer demux;
    Mp4FragmentSource* fragmented;
    // Only fragment initialization installs these hooks. Keeping the optional
    // parser out of the ordinary read path lets unused adapters be discarded.
    Mp4DemuxError (*fragment_peek)(Mp4FragmentSource*, Mp4AccessUnit*) noexcept;
    bool (*fragment_commit)(Mp4FragmentSource*) noexcept;
    Mp4DemuxError (*fragment_seek)(State*, i64, Mp4SeekPoint*, bool) noexcept;
    i64 first_presentation;
    H264ReferenceState references;
    H264OutputQueue output;
    ID3D11VideoDevice* device;
    ID3D11VideoContext* context;
    ID3D11Multithread* guard;
    ID3D11VideoDecoder* decoder;
    GpuDecoderSurfaces surfaces;
    GpuDecoderFence wait;
    GpuVideoFrame frame;
    VideoDisplayExtent display;
    u32 feedback;
    i64 fragment_seek_target;
    u32 fragment_seek_candidate, fragment_seek_attempts;
    bool fragment_seek_pending, fragment_seek_start;
    bool full_stream;
};
static_assert(sizeof(State) <= 4'096);

Mp4DemuxError seek_fragment(State* s, i64 target, Mp4SeekPoint* point, bool start) noexcept {
    auto& stream = *s->fragmented;
    Mp4FragmentRange range{};
    u32 selected = 0;
    if (stream.segments) {
        if (s->fragment_seek_pending && s->fragment_seek_target == target && s->fragment_seek_start == start)
            selected = s->fragment_seek_candidate;
        else {
            if (!start) {
                i64 empty{};
                if (target < 0 || !mp4_fragment_source_time(stream, stream.track.media_start, &empty)) return Mp4DemuxError::unsupported;
                const i64 relative = target - empty;
                const u64 absolute = relative < 0 ? static_cast<u64>(-relative) : static_cast<u64>(relative);
                if (absolute > 86400000000ull) return Mp4DemuxError::unsupported;
                const u64 ticks = absolute / 1000000 * stream.track.timescale +
                    (absolute % 1000000) * stream.track.timescale / 1000000;
                const u64 origin = static_cast<u64>(stream.track.media_start);
                if ((relative < 0 && ticks > origin) || ticks > 0x7fffffffffffffffull - origin) return Mp4DemuxError::unsupported;
                u64 time = relative < 0 ? origin - ticks : origin + ticks;
                time = minimum(time, stream.index.first_time + stream.index.duration - 1);
                const auto found = mp4_fragment_source_find(&stream, time, &range);
                if (found != Mp4DemuxError::none) return found;
                selected = range.index;
            }
            s->fragment_seek_target = target; s->fragment_seek_start = start; s->fragment_seek_pending = true;
            s->fragment_seek_candidate = selected;
            s->fragment_seek_attempts = 0;
        }
    } else {
        if (!mp4_fragment_index_range(stream.index, 0, &range)) return Mp4DemuxError::malformed;
        if (!start) {
            i64 time{};
            if (!mp4_fragment_source_time(stream, static_cast<i64>(range.time), &time)) return Mp4DemuxError::unsupported;
            if (time > target) return Mp4DemuxError::unsupported;
            for (u32 i = 1; i < stream.index.count; ++i) {
                if (!mp4_fragment_index_follow(stream.index, range, &range)) return Mp4DemuxError::malformed;
                if (!mp4_fragment_source_time(stream, static_cast<i64>(range.time), &time)) return Mp4DemuxError::unsupported;
                if (time > target) break;
                selected = i;
            }
        }
    }
    // Index SAP flags are hints. Validate the real access unit against the
    // active codec configuration before replacing any cursor or GPU references.
    for (u32 attempts = 0; attempts < 64; ++attempts) {
        if (stream.segments && s->fragment_seek_attempts == 64) break;
        Mp4FragmentPosition candidate{}; Mp4AccessUnit unit{};
        auto error = mp4_fragment_source_prepare(&stream, selected, 0, &candidate);
        if (error != Mp4DemuxError::none) return error;
        error = mp4_fragment_position_unit(candidate, &unit);
        if (error != Mp4DemuxError::none) { mp4_fragment_source_cancel_prepare(&stream); return error; }
        if (start || unit.presentation_time_us_signed <= target) {
            H264Picture picture{};
            const auto parsed = h264_parse_picture(unit.bytes, s->picture.length_bytes, s->picture.sps,
                s->picture.pps, &picture, stream.track.configuration);
            if (parsed != H264Error::none) {
                mp4_fragment_source_cancel_prepare(&stream);
                return parsed == H264Error::unsupported ? Mp4DemuxError::unsupported : Mp4DemuxError::malformed;
            }
            if (picture.first.idr) {
                if (!mp4_fragment_source_adopt(&stream, candidate)) return Mp4DemuxError::data_pending;
                s->fragment_seek_pending = false;
                *point = {unit.sample_index, unit.decode_time_us, unit.presentation_time_us_signed};
                return Mp4DemuxError::none;
            }
        }
        if (start || !selected) break;
        --selected; s->fragment_seek_candidate = selected; ++s->fragment_seek_attempts;
    }
    mp4_fragment_source_cancel_prepare(&stream);
    s->fragment_seek_pending = false;
    return Mp4DemuxError::unsupported;
}

HRESULT submit(State* s, const H264ReferencePlan& plan, u8 surface) noexcept {
    const auto& picture = s->picture;
    DXVA_PicParams_H264 p{};
    DXVA_Qmatrix_H264 q{};
    h264_dxva_picture_parameters(picture.sps, picture.pps, picture.header.first, picture.header.all_intra,
                                 s->references, plan.decode_order, surface, s->feedback, &p, &q);
    D3D11_VIDEO_DECODER_BUFFER_DESC buffers[4]{};
    HRESULT status = gpu_decoder_upload_buffer(s->context, s->decoder, D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS,
                                               &p, sizeof(p), &buffers[0]);
    if (SUCCEEDED(status)) status = gpu_decoder_upload_buffer(s->context, s->decoder,
        D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, &q, sizeof(q), &buffers[1]);
    if (FAILED(status)) return status;

    // Directly fill the driver's mapped compressed/slice buffers: no staging
    // Annex-B packet allocation. The same immutable sample was validated above.
    UINT bit_capacity = 0, slice_capacity = 0;
    void* bit_data = nullptr;
    void* slice_data = nullptr;
    status = s->context->GetDecoderBuffer(s->decoder, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM,
        &bit_capacity, &bit_data);
    if (FAILED(status)) return status;
    status = s->context->GetDecoderBuffer(s->decoder, D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL,
        &slice_capacity, &slice_data);
    const bool slice_acquired = SUCCEEDED(status);
    const u64 aligned_bytes = (static_cast<u64>(picture.header.bitstream_bytes) + 127u) & ~127ull;
    const u64 slice_bytes = static_cast<u64>(picture.header.slice_count) * sizeof(DXVA_Slice_H264_Short);
    if (SUCCEEDED(status) && (!bit_data || !slice_data || aligned_bytes > bit_capacity || slice_bytes > slice_capacity))
        status = E_OUTOFMEMORY;
    u32 offset = 0, count = 0;
    if (SUCCEEDED(status)) {
        auto* bytes = static_cast<u8*>(bit_data);
        auto* slices = static_cast<DXVA_Slice_H264_Short*>(slice_data);
        H264NalCursor cursor{picture.unit.bytes, 0, picture.length_bytes};
        for (;;) {
            const auto nal = h264_next_nal(&cursor);
            if (nal.error == H264Error::end) break;
            if (nal.error != H264Error::none) { status = E_INVALIDARG; break; }
            if (nal.type != 1 && nal.type != 5) continue;
            if (count >= picture.header.slice_count || offset > picture.header.bitstream_bytes ||
                picture.header.bitstream_bytes - offset < 3u || nal.bytes.count > picture.header.bitstream_bytes - offset - 3u) {
                status = E_INVALIDARG; break;
            }
            slices[count++] = {offset, nal.bytes.count + 3u, 0};
            bytes[offset++] = 0; bytes[offset++] = 0; bytes[offset++] = 1;
            copy_bytes(bytes + offset, nal.bytes.data, nal.bytes.count);
            offset += nal.bytes.count;
        }
        if (count != picture.header.slice_count || offset != picture.header.bitstream_bytes) status = E_INVALIDARG;
        if (SUCCEEDED(status)) {
            const u32 padding = static_cast<u32>(aligned_bytes) - offset;
            clear_bytes(bytes + offset, padding);
            slices[count - 1u].SliceBytesInBuffer += padding;
        }
    }
    const HRESULT bit_released = s->context->ReleaseDecoderBuffer(s->decoder, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM);
    const HRESULT slice_released = slice_acquired ? s->context->ReleaseDecoderBuffer(s->decoder,
        D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL) : S_OK;
    if (FAILED(status)) return status;
    if (FAILED(bit_released)) return bit_released;
    if (FAILED(slice_released)) return slice_released;
    buffers[2].BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
    buffers[2].DataSize = static_cast<u32>(aligned_bytes);
    buffers[3].BufferType = D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL;
    buffers[3].DataSize = static_cast<u32>(slice_bytes);
    return s->context->SubmitDecoderBuffers(s->decoder, 4, buffers);
}

} // namespace

static bool initialize_decoder(H264GpuDecoder* probe, ConstSlice<u8> encoded, ID3D11Device* device,
                                 ID3D11DeviceContext* context, bool full_stream,
                                 Mp4SourceAvailability availability, Mp4FragmentSource* fragmented) noexcept {
    if (!probe || !device || !context || !encoded.data || !encoded.count)
        return false;
    h264_gpu_decoder_release(probe);
    // A progressive source is borrowed in place; its owner outlives release.
    const bool borrowed = availability.available != nullptr;
    if (encoded.count > (borrowed ? kMediaMaximumProgressiveVideoBytes : kMp4DemuxMaximumEncodedBytes)) {
        probe->status = E_INVALIDARG;
        probe->parse_error = H264Error::unsupported;
        probe->demux_error = Mp4DemuxError::unsupported;
        return false;
    }
    constexpr u32 input_offset = (sizeof(State) + 15u) & ~15u;
    auto* s = static_cast<State*>(VirtualAlloc(
        nullptr, static_cast<SIZE_T>(input_offset) + (borrowed ? 0 : encoded.count), MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE));
    if (!s) { probe->status = E_OUTOFMEMORY; return false; }
    probe->state = s;
    s->fragmented = fragmented;
    s->full_stream = full_stream; s->output.leased = 255;
    const u8* owned = encoded.data;
    if (!borrowed) {
        auto* copy = reinterpret_cast<u8*>(s) + input_offset;
        copy_bytes(copy, encoded.data, encoded.count);
        owned = copy;
    }
    if (fragmented) {
        s->fragment_peek = mp4_fragment_source_unit;
        s->fragment_commit = mp4_fragment_source_commit;
        s->fragment_seek = seek_fragment;
        const auto& track = fragmented->track;
        auto& demux = s->demux;
        demux.encoded = encoded; demux.availability = availability;
        demux.width = track.width; demux.height = track.height;
        demux.sar_width = track.sar_width; demux.sar_height = track.sar_height;
        demux.rotation = track.rotation; demux.nal_length_bytes = track.nal_length_bytes;
        demux.timescale = track.timescale; demux.has_edit_list = track.edited;
        demux.avcc_offset = static_cast<u32>(track.configuration.data - encoded.data);
        demux.avcc_bytes = track.configuration.count;
        Mp4AccessUnit first{};
        probe->demux_error = mp4_fragment_source_unit(fragmented, &first);
        if (probe->demux_error == Mp4DemuxError::none && !mp4_fragment_source_duration(*fragmented, &demux.duration_us))
            probe->demux_error = Mp4DemuxError::unsupported;
        probe->parse_error = probe->demux_error == Mp4DemuxError::none
            ? h264_gpu_configuration_from_unit(first, track.configuration, track.nal_length_bytes,
                                                track.width, track.height, &s->picture) : H264Error::malformed;
        demux.ready = probe->parse_error == H264Error::none;
        s->first_presentation = first.presentation_time_us_signed;
    } else {
        probe->demux_error = mp4_demux_initialize(&s->demux, {owned, encoded.count}, availability);
        probe->parse_error = probe->demux_error == Mp4DemuxError::none
            ? h264_gpu_configuration_from_demux(s->demux, &s->picture, &probe->demux_error)
            : H264Error::malformed;
    }
    if (probe->parse_error == H264Error::none) {
        const auto& picture = s->picture.sps;
        s->display = video_display_rotated(video_display_extent(picture.width, picture.height,
            s->demux.sar_width ? s->demux.sar_width : picture.sar_width,
            s->demux.sar_height ? s->demux.sar_height : picture.sar_height), s->demux.rotation);
        if (!s->display.width) probe->parse_error = H264Error::unsupported;
    }
    if (probe->parse_error == H264Error::none &&
        (!gpu_video_frame_visible_valid(s->picture.sps.width, s->picture.sps.height) ||
         s->picture.sps.coded_width > kMp4DecodeMaximumDimension + 32u ||
         s->picture.sps.coded_height > kMp4DecodeMaximumDimension + 32u ||
         s->picture.sps.transform_bypass))
        probe->parse_error = H264Error::unsupported;
    HRESULT status = probe->parse_error == H264Error::none ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(status)) status = device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(&s->device));
    if (SUCCEEDED(status)) status = context->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void**>(&s->context));
    if (SUCCEEDED(status)) status = context->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&s->guard));
    const auto& a = s->picture.sps;
    D3D11_VIDEO_DECODER_DESC decoder_desc{D3D11_DECODER_PROFILE_H264_VLD_NOFGT,
        a.coded_width, a.coded_height, DXGI_FORMAT_NV12};
    D3D11_VIDEO_DECODER_CONFIG selected{};
    if (SUCCEEDED(status)) {
        BOOL supported = FALSE;
        status = s->device->CheckVideoDecoderFormat(&decoder_desc.Guid, DXGI_FORMAT_NV12, &supported);
        if (SUCCEEDED(status) && (!supported || !gpu_decoder_surfaces_supported(device)))
            status = DXGI_ERROR_UNSUPPORTED;
    }
    if (SUCCEEDED(status))
        status = gpu_decoder_select_config(s->device, decoder_desc, 2, &selected);
    if (SUCCEEDED(status)) status = s->device->CreateVideoDecoder(&decoder_desc, &selected, &s->decoder);
    if (SUCCEEDED(status)) {
        // DPB plus the renderer lease and current decode. References and
        // waiting output share slots; displayed but referenced pictures stay.
        // An unrestricted SPS uses its level's DPB (1080p@4.x: 4 + 2 = 6
        // surfaces, ~19 MB) instead of the 16-frame maximum (18, ~56 MB).
        const u32 count = maximum<u32>(selected.ConfigMinRenderTargetBuffCount,
            maximum<u32>(a.max_references, h264_sps_dpb_frames(a)) + 2u);
        status = gpu_decoder_surfaces_create(&s->surfaces, device, s->device, decoder_desc.Guid, a.coded_width,
            a.coded_height, count, full_stream ? count : 1u);
    }
    probe->status = status;
    if (FAILED(status)) {
        const H264Error parse_error = probe->parse_error;
        const Mp4DemuxError demux_error = probe->demux_error;
        h264_gpu_decoder_release(probe);
        probe->status = status;
        probe->parse_error = parse_error;
        probe->demux_error = demux_error;
        return false;
    }
    s->frame = {s->surfaces.texture, 0, a.crop_x, a.crop_y, a.width, a.height,
        video_matrix_bt709(a.matrix, a.height), a.full_range, s->demux.rotation, {}};
    return true;
}
bool h264_gpu_decoder_initialize(H264GpuDecoder* probe, ConstSlice<u8> encoded, ID3D11Device* device,
                                 ID3D11DeviceContext* context, bool full_stream,
                                 Mp4SourceAvailability availability) noexcept {
    return initialize_decoder(probe, encoded, device, context, full_stream, availability, nullptr);
}
bool h264_gpu_decoder_initialize_fragmented(H264GpuDecoder* decoder, Mp4FragmentSource* source,
                                            ID3D11Device* device, ID3D11DeviceContext* context) noexcept {
    if (!decoder || decoder->state || !device || !context || !source || !mp4_fragment_source_valid(*source) || source->track.audio || source->ended || source->segment.index ||
        (source->loaded && source->fragment.index)) return false;
    Mp4AccessUnit first{};
    const auto error = mp4_fragment_source_unit(source, &first);
    if (error != Mp4DemuxError::none) { decoder->demux_error = error; return false; }
    const Mp4SourceAvailability header{[](void* context, u64 offset, u64 bytes) noexcept {
        const auto span = mp4_fragment_source_bytes(*static_cast<Mp4FragmentSource*>(context));
        return offset <= span.count && bytes <= span.count - offset;
    }, source};
    return initialize_decoder(decoder, mp4_fragment_source_bytes(*source), device, context, true,
        source->segments ? header : progressive_source_reader(source->source, source->reader), source);
}

namespace {
u32 reference_mask(const State& s) noexcept {
    u32 mask = 0;
    for (u32 i = 0; i < s.references.count; ++i) mask |= 1u << s.references.pictures[i].surface;
    return mask;
}
Mp4DecodedFrame output_frame(State* s) noexcept {
    return gpu_decoder_output_frame(&s->output, &s->frame, s->surfaces, s->display);
}
}

Mp4DecodedFrame h264_gpu_decoder_read(H264GpuDecoder* probe, bool loop_at_end,u32 max_submissions) noexcept {
    Mp4DecodedFrame frame{};
    frame.error = Mp4DecodeError::invalid_input;
    if (!probe || !probe->state) return frame;
    auto* s = static_cast<State*>(probe->state);
    s->fragment_seek_pending = false;
    if(max_submissions&&s->output.leased!=255)return frame;
    if (!s->full_stream && probe->submitted) { frame.error = Mp4DecodeError::end_of_stream; return frame; }
    // Caller has enqueued its copy before requesting another frame. All video
    // decode and presentation commands use the same protected immediate context.
    s->output.leased = 255;
    bool restarted = false;u32 submissions{};
    for (;;) {
        const auto& a = s->picture.sps;
        if (s->output.count && (!s->full_stream || h264_output_ready(s->output, reference_mask(*s),
            h264_sps_output_reorder(a), maximum<u8>(h264_sps_dpb_frames(a), 1)))) {
            probe->submitted = true; return output_frame(s);
        }
        Mp4Demuxer next; copy_value(&next, s->demux);
        Mp4AccessUnit unit{};
        if (s->fragmented) unit.error = s->fragment_peek(s->fragmented, &unit);
        else unit = mp4_demux_next(&next);
        if (unit.error == Mp4DemuxError::data_pending) { frame.error = Mp4DecodeError::data_pending; return frame; }
        if (unit.error == Mp4DemuxError::end_of_stream) {
            if (s->output.count) { probe->submitted = true; return output_frame(s); }
            if (s->full_stream && loop_at_end && !restarted) {
                if(!h264_gpu_decoder_rewind(probe)) {
                    frame.error=probe->demux_error==Mp4DemuxError::data_pending ? Mp4DecodeError::data_pending : Mp4DecodeError::stream;
                    return frame;
                }
                restarted = true; continue;
            }
            frame.error = Mp4DecodeError::end_of_stream;
            return frame;
        }
        // Fragment cursors already lease and validate their payload. Separate
        // files do not belong to the initialization span used for avcC.
        if (unit.ok() && !s->fragmented && !mp4_demux_available(s->demux, unit.bytes)) {
            frame.error = Mp4DecodeError::data_pending;
            return frame;
        }
        if (max_submissions && submissions >= max_submissions) {
            frame.error = Mp4DecodeError::needs_service;
            return frame;
        }
        H264Picture header{};
        probe->parse_error = unit.ok()
                                 ? h264_parse_picture(unit.bytes, s->picture.length_bytes, a,
                                                      s->picture.pps, &header, mp4_demux_avcc(s->demux))
                                 : H264Error::malformed;
        if (probe->parse_error != H264Error::none) {
            probe->status = E_INVALIDARG;
            frame.error = Mp4DecodeError::stream;
            return frame;
        }
        bool reset = header.first.idr;
        for (u32 i = 0; i < header.first.marking_count; ++i)
            reset |= header.first.marking[i].operation == 5;
        if (reset && s->output.count) {
            if (header.first.idr && header.first.discard_prior)
                reset_value(&s->output);
            else { probe->submitted = true; return output_frame(s); }
        }
        const u8 surface = h264_free_surface(s->output, header.first.idr ? 0 : reference_mask(*s),
            s->full_stream ? s->surfaces.count : 1);
        H264ReferencePlan plan{};
        probe->parse_error = h264_reference_plan(s->references, a, header.first, surface, &plan);
        if (probe->parse_error != H264Error::none) {
            probe->status = E_INVALIDARG; frame.error = Mp4DecodeError::stream; return frame;
        }
        s->picture.unit = unit; s->picture.header = header;
        s->guard->Enter();
        HRESULT status = s->context->DecoderBeginFrame(s->decoder, s->surfaces.views[surface], 0, nullptr);
        if (SUCCEEDED(status)) {
            status = submit(s, plan, surface);
            const HRESULT ended = s->context->DecoderEndFrame(s->decoder);
            if (SUCCEEDED(status)) status = ended;
        }
        s->guard->Leave();
        probe->status = status;
        if (FAILED(status)) { frame.error = Mp4DecodeError::sample; return frame; }
        if (++s->feedback == 0) ++s->feedback;
        copy_value(&s->references, plan.after);
        if (s->fragmented) {
            if (!s->fragment_commit(s->fragmented)) {
                probe->status = E_UNEXPECTED; frame.error = Mp4DecodeError::stream; return frame;
            }
            ++s->demux.sample_index;
        } else copy_value(&s->demux, next);
        ++submissions;
        const auto order = plan.retained_order;
        if (!h264_output_push(&s->output, {unit.duration_us,
            order.top < order.bottom ? order.top : order.bottom, surface,unit.presentation_time_us_signed})) {
            probe->status = E_UNEXPECTED; frame.error = Mp4DecodeError::sample; return frame;
        }
    }
}

H264GpuMovie h264_gpu_decoder_movie(const H264GpuDecoder& decoder) noexcept {
    const auto* s = static_cast<const State*>(decoder.state);
    if (!s || !s->demux.ready)
        return {};
    H264GpuMovie movie{s->demux.duration_us, s->demux.sample_count, s->demux.has_edit_list};
    if (s->fragmented) {
        if (const auto* store = s->fragmented->segments; store && store->live) {
            if (!mp4_fragment_source_duration(*s->fragmented, &movie.duration_us)) return {};
            movie.live = true;
            movie.duration_complete = store->ended && store->received_count == store->count;
        }
        movie.first_presentation_time_us = s->first_presentation; movie.first_presentation_valid = true;
        return movie;
    }
    Mp4Demuxer first; copy_value(&first, s->demux);
    mp4_demux_reset(&first);
    const auto unit = mp4_demux_next(&first);
    if(unit.ok()){movie.first_presentation_time_us=unit.presentation_time_us_signed;movie.first_presentation_valid=true;}
    return movie;
}
ConstSlice<u8> h264_gpu_decoder_source(const H264GpuDecoder& decoder) noexcept {
    const auto* s=static_cast<const State*>(decoder.state);
    return s&&s->demux.ready?s->demux.encoded:ConstSlice<u8>{};
}
bool h264_gpu_decoder_acknowledge_frame(H264GpuDecoder* decoder) noexcept {
    if(!decoder||!decoder->state)return false;
    auto* s=static_cast<State*>(decoder->state);if(!s->demux.ready)return false;
    s->output.leased=255;return true;
}
Mp4DemuxError h264_gpu_decoder_seek(H264GpuDecoder* decoder,i64 target,Mp4SeekPoint* point) noexcept {
    if(!decoder||!decoder->state||!point)return Mp4DemuxError::invalid_input;
    auto* s=static_cast<State*>(decoder->state);
    const ConstSlice<u8> output{reinterpret_cast<const u8*>(point),sizeof(*point)};
    if(!s->full_stream||!s->demux.ready||s->output.leased!=255||!byte_range_valid(output)||
        byte_ranges_overlap(output,{reinterpret_cast<const u8*>(decoder),sizeof(*decoder)})||
        byte_ranges_overlap(output,{reinterpret_cast<const u8*>(s),sizeof(*s)})||
        byte_ranges_overlap(output,s->demux.encoded))return Mp4DemuxError::invalid_input;
    Mp4Demuxer next; copy_value(&next, s->demux);Mp4SeekPoint selected{};
    const auto result=s->fragmented ? s->fragment_seek(s,target,&selected,false)
                                    : mp4_demux_seek_sync(&next,target,&selected);
    if(result!=Mp4DemuxError::none)return result;
    if(s->fragmented)next.sample_index=selected.sample_index;
    copy_value(&s->demux, next);reset_value(&s->references);reset_value(&s->output);decoder->submitted=false;
    decoder->status=S_OK;decoder->parse_error=H264Error::none;decoder->demux_error=Mp4DemuxError::none;*point=selected;
    return Mp4DemuxError::none;
}
bool h264_gpu_decoder_rewind(H264GpuDecoder* decoder) noexcept {
    if(!decoder||!decoder->state)return false;
    auto* s=static_cast<State*>(decoder->state);if(!s->full_stream)return false;
    if (s->fragmented) {
        Mp4SeekPoint point{};
        decoder->demux_error=s->fragment_seek(s,0,&point,true);
        if (decoder->demux_error != Mp4DemuxError::none) return false;
        s->demux.sample_index=0;reset_value(&s->references);reset_value(&s->output);decoder->submitted=false;return true;
    }
    Mp4Demuxer next; copy_value(&next, s->demux);mp4_demux_reset(&next);if(!next.ready)return false;
    copy_value(&s->demux, next);reset_value(&s->references);reset_value(&s->output);decoder->submitted=false;return true;
}
bool h264_gpu_decoder_seek_start(H264GpuDecoder* decoder) noexcept {
    if(!decoder||!decoder->state)return false;
    const auto* s=static_cast<const State*>(decoder->state);
    if(!s->demux.ready||s->output.leased!=255)return false;
    if(!h264_gpu_decoder_rewind(decoder))return false;
    decoder->status=S_OK;decoder->parse_error=H264Error::none;return true;
}

void h264_gpu_decoder_release(H264GpuDecoder* probe) noexcept {
    if (!probe) return;
    if (auto* s = static_cast<State*>(probe->state)) {
        gpu_decoder_surfaces_release(&s->surfaces);
        com_release(s->decoder);
        gpu_decoder_fence_release(&s->wait);
        com_release(s->guard); com_release(s->context); com_release(s->device);
        VirtualFree(s, 0, MEM_RELEASE);
    }
    reset_value(probe);
}

bool h264_gpu_decoder_busy(const H264GpuDecoder& decoder) noexcept {
    return decoder.status == E_PENDING || decoder.status == static_cast<HRESULT>(0x8876021C) ||
        decoder.status == DXGI_ERROR_WAS_STILL_DRAWING;
}

HRESULT gpu_decoder_fence_wait(GpuDecoderFence* f, ID3D11VideoContext* context, ID3D11Multithread* guard,
                               u32 timeout_ms) noexcept {
    HRESULT status = S_OK;
    if (!f->context) status = context->QueryInterface(__uuidof(ID3D11DeviceContext4),
        reinterpret_cast<void**>(&f->context));
    if (SUCCEEDED(status) && !f->fence) {
        ID3D11Device* device = nullptr;
        ID3D11Device5* device5 = nullptr;
        context->GetDevice(&device);
        status = device ? device->QueryInterface(__uuidof(ID3D11Device5),
            reinterpret_cast<void**>(&device5)) : E_NOINTERFACE;
        if (SUCCEEDED(status)) status = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE,
            __uuidof(ID3D11Fence), reinterpret_cast<void**>(&f->fence));
        com_release(device5); com_release(device);
    }
    if (SUCCEEDED(status) && !f->wake) {
        f->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!f->wake) status = HRESULT_FROM_WIN32(thread_last_error());
    }
    if (SUCCEEDED(status)) {
        guard->Enter();
        status = f->context->Signal(f->fence, ++f->signal);
        if (SUCCEEDED(status)) status = f->fence->SetEventOnCompletion(f->signal, f->wake);
        f->context->Flush();
        guard->Leave();
        if (SUCCEEDED(status)) {
            const DWORD waited = WaitForSingleObject(f->wake, timeout_ms);
            if (waited != WAIT_OBJECT_0) status = HRESULT_FROM_WIN32(
                waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : thread_last_error());
        }
    }
    return status;
}

void gpu_decoder_fence_release(GpuDecoderFence* f) noexcept {
    com_release(f->fence); com_release(f->context);
    if (f->wake) CloseHandle(f->wake);
}

bool h264_gpu_decoder_wait(H264GpuDecoder* decoder, u32 timeout_ms) noexcept {
    if (!decoder || !decoder->state || !timeout_ms || timeout_ms > 16) return false;
    auto* s = static_cast<State*>(decoder->state);
    const HRESULT status = gpu_decoder_fence_wait(&s->wait, s->context, s->guard, timeout_ms);
    if (FAILED(status)) decoder->status = status;
    return SUCCEEDED(status);
}

} // namespace maicord
#pragma clang attribute pop
