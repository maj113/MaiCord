#include "maicord/core/win32_process.hpp"
#include "maicord/platform/decode/h264_gpu_decoder.hpp"
#include "maicord/platform/decode/mp4_decoder.hpp"
#include "h264_gpu_configuration.hpp"
#include "maicord/media/video/h264_references.hpp"
#include "maicord/media/video/h264_output_queue.hpp"
#include "maicord/media/video/video_display.hpp"
#include "maicord/media/containers/mp4_fragment_source.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
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
    H264FragmentSeek seek;
};
static_assert(sizeof(State) <= 4'096);

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
                                 ID3D11DeviceContext* context,
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
    auto* s = static_cast<State*>(commit_pages(static_cast<SIZE_T>(input_offset) + (borrowed ? 0 : encoded.count)));
    if (!s) { probe->status = E_OUTOFMEMORY; return false; }
    probe->state = s;
    s->fragmented = fragmented;
    s->output.leased = 255;
    const u8* owned = encoded.data;
    if (!borrowed) {
        auto* copy = reinterpret_cast<u8*>(s) + input_offset;
        copy_bytes(copy, encoded.data, encoded.count);
        owned = copy;
    }
    if (fragmented) {
        probe->parse_error = h264_gpu_configuration_from_fragment(fragmented, encoded, availability, &s->demux,
            &s->picture, &probe->demux_error, &s->first_presentation);
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
            a.coded_height, count);
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
                                 ID3D11DeviceContext* context, Mp4SourceAvailability availability) noexcept {
    return initialize_decoder(probe, encoded, device, context, availability, nullptr);
}
bool h264_gpu_decoder_initialize_fragmented(H264GpuDecoder* decoder, Mp4FragmentSource* source,
                                            ID3D11Device* device, ID3D11DeviceContext* context) noexcept {
    Mp4SourceAvailability availability{};
    if (!decoder || decoder->state || !device || !context || !source ||
        !h264_fragment_availability(source, &decoder->demux_error, &availability)) return false;
    return initialize_decoder(decoder, mp4_fragment_source_bytes(*source), device, context, availability, source);
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

Mp4DecodedFrame h264_gpu_decoder_read(H264GpuDecoder* probe, u32 max_submissions) noexcept {
    Mp4DecodedFrame frame{};
    frame.error = Mp4DecodeError::invalid_input;
    if (!probe || !probe->state) return frame;
    auto* s = static_cast<State*>(probe->state);
    s->seek.pending = false;
    if(max_submissions&&s->output.leased!=255)return frame;
    // Caller has enqueued its copy before requesting another frame. All video
    // decode and presentation commands use the same protected immediate context.
    s->output.leased = 255;
    u32 submissions{};
    for (;;) {
        const auto& a = s->picture.sps;
        if (s->output.count && h264_output_ready(s->output, reference_mask(*s),
            h264_sps_output_reorder(a), maximum<u8>(h264_sps_dpb_frames(a), 1))) {
            return output_frame(s);
        }
        Mp4Demuxer next; copy_value(&next, s->demux);
        Mp4AccessUnit unit{};
        if (s->fragmented) unit.error = mp4_fragment_source_unit(s->fragmented, &unit);
        else unit = mp4_demux_next(&next);
        if (unit.error == Mp4DemuxError::data_pending) { frame.error = Mp4DecodeError::data_pending; return frame; }
        if (unit.error == Mp4DemuxError::end_of_stream) {
            if (s->output.count) return output_frame(s);
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
            else return output_frame(s);
        }
        const u8 surface = h264_free_surface(s->output, header.first.idr ? 0 : reference_mask(*s),
            s->surfaces.count);
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
            if (!mp4_fragment_source_commit(s->fragmented)) {
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
    return s ? h264_gpu_movie(s->demux, s->fragmented, s->first_presentation) : H264GpuMovie{};
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
    if(!s->demux.ready||s->output.leased!=255||!byte_range_valid(output)||
        byte_ranges_overlap(output,{reinterpret_cast<const u8*>(decoder),sizeof(*decoder)})||
        byte_ranges_overlap(output,{reinterpret_cast<const u8*>(s),sizeof(*s)})||
        byte_ranges_overlap(output,s->demux.encoded))return Mp4DemuxError::invalid_input;
    Mp4Demuxer next; copy_value(&next, s->demux);Mp4SeekPoint selected{};
    const auto result=s->fragmented ? h264_fragment_seek(*s->fragmented,&s->seek,s->picture,target,&selected,false)
                                    : mp4_demux_seek_sync(&next,target,&selected);
    if(result!=Mp4DemuxError::none)return result;
    if(s->fragmented)next.sample_index=selected.sample_index;
    copy_value(&s->demux, next);reset_value(&s->references);reset_value(&s->output);
    decoder->status=S_OK;decoder->parse_error=H264Error::none;decoder->demux_error=Mp4DemuxError::none;*point=selected;
    return Mp4DemuxError::none;
}
bool h264_gpu_decoder_rewind(H264GpuDecoder* decoder) noexcept {
    if(!decoder||!decoder->state)return false;
    auto* s=static_cast<State*>(decoder->state);
    if (s->fragmented) {
        Mp4SeekPoint point{};
        decoder->demux_error=h264_fragment_seek(*s->fragmented,&s->seek,s->picture,0,&point,true);
        if (decoder->demux_error != Mp4DemuxError::none) return false;
        s->demux.sample_index=0;reset_value(&s->references);reset_value(&s->output);return true;
    }
    Mp4Demuxer next; copy_value(&next, s->demux);mp4_demux_reset(&next);if(!next.ready)return false;
    copy_value(&s->demux, next);reset_value(&s->references);reset_value(&s->output);return true;
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
        pages_release(s);
    }
    reset_value(probe);
}

bool h264_gpu_decoder_busy(const H264GpuDecoder& decoder) noexcept {
    return decoder.status == E_PENDING || decoder.status == static_cast<HRESULT>(0x8876021C) ||
        decoder.status == DXGI_ERROR_WAS_STILL_DRAWING;
}

HRESULT gpu_decoder_fence_wait(GpuDecoderFence* f, ID3D11VideoContext* context, ID3D11Multithread* guard,
                               u32 timeout_ms) noexcept {
    // The video context is the device's immediate context; its fence interfaces are the Windows 10 1703 runtime's,
    // and device creation refused a driver without fences, so only the fence itself can fail.
    HRESULT status = S_OK;
    if (!f->context) context->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void**>(&f->context));
    if (!f->fence) {
        ID3D11Device* device = nullptr;
        ID3D11Device5* device5 = nullptr;
        context->GetDevice(&device);
        device->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&device5));
        status = device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence),
                                      reinterpret_cast<void**>(&f->fence));
        device5->Release(); device->Release();
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
