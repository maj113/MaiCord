#include "maicord/core/win32_process.hpp"
#include "maicord/platform/h264_gpu_decoder.hpp"
#include "maicord/platform/mp4_decoder.hpp"
#include "h264_gpu_configuration.hpp"
#include "maicord/media/h264_references.hpp"
#include "maicord/media/h264_output_queue.hpp"
#include "maicord/media/video_display.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/initguid.hpp"
#include "maicord/sys/dxva.hpp"
#include "h264_dxva_parameters.hpp"
#include "gpu_decoder_fence.hpp"

namespace maicord {
namespace {

struct State {
    H264GpuPicture picture;
    Mp4Demuxer demux;
    H264ReferenceState references;
    H264OutputQueue output;
    ID3D11VideoDevice* device;
    ID3D11VideoContext* context;
    ID3D11Multithread* guard;
    ID3D11VideoDecoder* decoder;
    ID3D11Texture2D* texture;
    ID3D11VideoDecoderOutputView* views[32];
    GpuDecoderFence wait;
    GpuVideoFrame frame;
    VideoDisplayExtent display;
    u32 feedback;
    u8 surface_count;
    bool full_stream;
};
static_assert(sizeof(State) <= 4'096);

HRESULT submit(State* s, const H264ReferencePlan& plan, u8 surface) noexcept {
    const auto& picture = s->picture;
    DXVA_PicParams_H264 p{};
    DXVA_Qmatrix_H264 q{};
    h264_dxva_picture_parameters(picture.sps, picture.pps, picture.header.first, picture.header.all_intra,
                                 s->references, plan.decode_order, surface, s->feedback, &p, &q);
    D3D11_VIDEO_DECODER_BUFFER_DESC buffers[4]{};
    HRESULT status = dxva_upload_buffer(s->context, s->decoder, D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, &p,
                                        sizeof(p), &buffers[0]);
    if (SUCCEEDED(status)) status = dxva_upload_buffer(s->context, s->decoder,
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

bool h264_gpu_decoder_initialize(H264GpuDecoder* probe, ConstSlice<u8> encoded, ID3D11Device* device,
                                 ID3D11DeviceContext* context, bool full_stream,
                                 Mp4SourceAvailability availability) noexcept {
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
    s->full_stream = full_stream; s->output.leased = 255;
    const u8* owned = encoded.data;
    if (!borrowed) {
        auto* copy = reinterpret_cast<u8*>(s) + input_offset;
        copy_bytes(copy, encoded.data, encoded.count);
        owned = copy;
    }
    probe->demux_error = mp4_demux_initialize(&s->demux, {owned, encoded.count}, availability);
    probe->parse_error = probe->demux_error == Mp4DemuxError::none
        ? h264_gpu_configuration_from_demux(s->demux, &s->picture, &probe->demux_error)
        : H264Error::malformed;
    if (probe->parse_error == H264Error::none) {
        const auto& picture = s->picture.sps;
        s->display = video_display_rotated(video_display_extent(picture.width, picture.height,
            s->demux.sar_width ? s->demux.sar_width : picture.sar_width,
            s->demux.sar_height ? s->demux.sar_height : picture.sar_height), s->demux.rotation);
        if (!s->display.width) probe->parse_error = H264Error::unsupported;
    }
    if (probe->parse_error == H264Error::none &&
        (s->picture.sps.width > kMp4DecodeMaximumDimension ||
         s->picture.sps.height > kMp4DecodeMaximumDimension ||
         s->picture.sps.coded_width > kMp4DecodeMaximumDimension + 32u ||
         s->picture.sps.coded_height > kMp4DecodeMaximumDimension + 32u ||
         s->picture.sps.transform_bypass ||
         (s->picture.sps.matrix != 1 && s->picture.sps.matrix != 2 &&
          s->picture.sps.matrix != 5 && s->picture.sps.matrix != 6)))
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
        if (SUCCEEDED(status) && !supported) status = DXGI_ERROR_UNSUPPORTED;
    }
    if (SUCCEEDED(status)) {
        UINT count = 0;
        status = s->device->GetVideoDecoderConfigCount(&decoder_desc, &count);
        bool found = false;
        for (UINT i = 0; SUCCEEDED(status) && i < count; ++i) {
            D3D11_VIDEO_DECODER_CONFIG candidate{};
            status = s->device->GetVideoDecoderConfig(&decoder_desc, i, &candidate);
            if (SUCCEEDED(status) && candidate.ConfigBitstreamRaw == 2 &&
                bytes_equal(&candidate.guidConfigBitstreamEncryption, &DXVA_NoEncrypt, sizeof(GUID))) {
                selected = candidate; found = true; break;
            }
        }
        if (SUCCEEDED(status) && !found) status = DXGI_ERROR_UNSUPPORTED;
    }
    if (SUCCEEDED(status)) status = s->device->CreateVideoDecoder(&decoder_desc, &selected, &s->decoder);
    if (SUCCEEDED(status)) {
        D3D11_TEXTURE2D_DESC texture{};
        texture.Width = a.coded_width; texture.Height = a.coded_height;
        // DPB plus the renderer lease and current decode. References and
        // waiting output share slots; displayed but referenced pictures stay.
        // An unrestricted SPS uses its level's DPB (1080p@4.x: 4 + 2 = 6
        // surfaces, ~19 MB) instead of the 16-frame maximum (18, ~56 MB).
        texture.ArraySize = maximum<u32>(selected.ConfigMinRenderTargetBuffCount,
            maximum<u32>(a.max_references, h264_sps_dpb_frames(a)) + 2u);
        texture.MipLevels = 1; texture.Format = DXGI_FORMAT_NV12;
        texture.SampleDesc.Count = 1; texture.BindFlags = D3D11_BIND_DECODER;
        if (texture.ArraySize > 32) status = DXGI_ERROR_UNSUPPORTED;
        else status = device->CreateTexture2D(&texture, nullptr, &s->texture);
        s->surface_count = static_cast<u8>(texture.ArraySize);
    }
    if (SUCCEEDED(status)) {
        D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC view{};
        view.DecodeProfile = decoder_desc.Guid;
        view.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
        const u32 views = full_stream ? s->surface_count : 1u;
        for (u32 i = 0; i < views && SUCCEEDED(status); ++i) {
            view.Texture2D.ArraySlice = i;
            status = s->device->CreateVideoDecoderOutputView(s->texture, &view, &s->views[i]);
        }
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
    s->frame = {s->texture, 0, a.crop_x, a.crop_y, a.width, a.height,
        a.matrix == 1, a.full_range, s->demux.rotation, {}};
    return true;
}

namespace {
u32 reference_mask(const State& s) noexcept {
    u32 mask = 0;
    for (u32 i = 0; i < s.references.count; ++i) mask |= 1u << s.references.pictures[i].surface;
    return mask;
}
Mp4DecodedFrame output_frame(State* s) noexcept {
    H264Output picture{};
    Mp4DecodedFrame frame{};
    if (!h264_output_pop(&s->output, &picture)) { frame.error = Mp4DecodeError::sample; return frame; }
    s->frame.subresource = picture.surface;
    frame.gpu = &s->frame; frame.width = s->frame.width; frame.height = s->frame.height;
    frame.display_width = s->display.width; frame.display_height = s->display.height;
    frame.duration_us = picture.duration_us;
    frame.source_duration_us = picture.duration_us;
    frame.presentation_time_us = picture.presentation_time_us;
    return frame;
}
}

Mp4DecodedFrame h264_gpu_decoder_read(H264GpuDecoder* probe, bool loop_at_end,u32 max_submissions) noexcept {
    Mp4DecodedFrame frame{};
    frame.error = Mp4DecodeError::invalid_input;
    if (!probe || !probe->state) return frame;
    auto* s = static_cast<State*>(probe->state);
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
        Mp4Demuxer next = s->demux;
        const Mp4AccessUnit unit = mp4_demux_next(&next);
        if (unit.error == Mp4DemuxError::end_of_stream) {
            if (s->output.count) { probe->submitted = true; return output_frame(s); }
            if (s->full_stream && loop_at_end && !restarted) {
                if(!h264_gpu_decoder_rewind(probe)){frame.error=Mp4DecodeError::stream;return frame;}
                restarted = true; continue;
            }
            frame.error = Mp4DecodeError::end_of_stream;
            return frame;
        }
        if (unit.ok() && !mp4_demux_available(s->demux, unit.bytes)) {
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
                s->output = {};
            else { probe->submitted = true; return output_frame(s); }
        }
        const u8 surface = h264_free_surface(s->output, header.first.idr ? 0 : reference_mask(*s),
            s->full_stream ? s->surface_count : 1);
        H264ReferencePlan plan{};
        probe->parse_error = h264_reference_plan(s->references, a, header.first, surface, &plan);
        if (probe->parse_error != H264Error::none) {
            probe->status = E_INVALIDARG; frame.error = Mp4DecodeError::stream; return frame;
        }
        s->picture.unit = unit; s->picture.header = header;
        s->guard->Enter();
        HRESULT status = s->context->DecoderBeginFrame(s->decoder, s->views[surface], 0, nullptr);
        if (SUCCEEDED(status)) {
            status = submit(s, plan, surface);
            const HRESULT ended = s->context->DecoderEndFrame(s->decoder);
            if (SUCCEEDED(status)) status = ended;
        }
        s->guard->Leave();
        probe->status = status;
        if (FAILED(status)) { frame.error = Mp4DecodeError::sample; return frame; }
        if (++s->feedback == 0) ++s->feedback;
        s->references = plan.after; s->demux = next;
        ++submissions;
        const auto order = plan.retained_order;
        if (!h264_output_push(&s->output, {unit.duration_us,
            order.top < order.bottom ? order.top : order.bottom, surface,unit.presentation_time_us_signed})) {
            probe->status = E_UNEXPECTED; frame.error = Mp4DecodeError::sample; return frame;
        }
    }
}

H264GpuDiagnostic h264_gpu_decoder_diagnostic(const H264GpuDecoder& decoder) noexcept {
    const auto* s = static_cast<const State*>(decoder.state);
    if (!s)
        return {};
    auto cursor = s->demux;
    const auto next = mp4_demux_next(&cursor);
    return {s->demux.sample_index, next.presentation_time_us_signed, next.decode_time_us, s->output.count,
            next.ok()};
}
H264GpuMovie h264_gpu_decoder_movie(const H264GpuDecoder& decoder) noexcept {
    const auto* s = static_cast<const State*>(decoder.state);
    if (!s || !s->demux.ready)
        return {};
    H264GpuMovie movie{s->demux.duration_us, s->demux.sample_count, s->demux.has_edit_list};
    Mp4Demuxer first = s->demux;
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
    Mp4Demuxer next=s->demux;Mp4SeekPoint selected{};
    const auto result=mp4_demux_seek_sync(&next,target,&selected);
    if(result!=Mp4DemuxError::none)return result;
    s->demux=next;s->references={};s->output={};decoder->submitted=false;
    decoder->status=S_OK;decoder->parse_error=H264Error::none;*point=selected;
    return Mp4DemuxError::none;
}
bool h264_gpu_decoder_rewind(H264GpuDecoder* decoder) noexcept {
    if(!decoder||!decoder->state)return false;
    auto* s=static_cast<State*>(decoder->state);if(!s->full_stream)return false;
    Mp4Demuxer next=s->demux;mp4_demux_reset(&next);if(!next.ready)return false;
    s->demux=next;s->references={};s->output={};decoder->submitted=false;return true;
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
        for (auto*& view : s->views) com_release(view);
        com_release(s->texture); com_release(s->decoder);
        gpu_decoder_fence_release(&s->wait);
        com_release(s->guard); com_release(s->context); com_release(s->device);
        VirtualFree(s, 0, MEM_RELEASE);
    }
    *probe = {};
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
