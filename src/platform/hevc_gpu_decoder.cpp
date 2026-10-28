#include "maicord/platform/hevc_gpu_decoder.hpp"
#include "maicord/platform/mp4_decoder.hpp"
#include "maicord/media/hevc_references.hpp"
#include "maicord/media/hevc_mp4_demux.hpp"
#include "maicord/media/h264_output_queue.hpp"
#include "maicord/media/video_display.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_range.hpp"
#include "hevc_dxva_parameters.hpp"
#include "gpu_decoder_fence.hpp"
#include "gpu_decoder_surfaces.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/initguid.hpp"
#include "maicord/sys/dxva.hpp"

namespace maicord {
namespace {
struct State {
    HevcConfiguration config;
    HevcPicture picture;
    Mp4Demuxer demux;
    HevcReferenceState references;
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
    bool full_stream;
};
static_assert(sizeof(State) <= 16384);
H264Error mapped(HevcError error) noexcept {
    return static_cast<H264Error>(static_cast<u8>(error));
}
HRESULT submit(State& s, const Mp4AccessUnit& unit, const HevcPicture& picture,
               const HevcReferencePlan& plan, u8 surface) noexcept {
    const auto& a = s.config.sps;
    DXVA_PicParams_HEVC p;
    DXVA_Qmatrix_HEVC q{};
    if (!hevc_dxva_picture_parameters(a, s.config.pps, picture, picture.slice_type == 2, s.references, plan, surface,
                                      s.feedback, &p, &q))
        return E_INVALIDARG;
    D3D11_VIDEO_DECODER_BUFFER_DESC buffers[4]{};
    HRESULT result = gpu_decoder_upload_buffer(s.context, s.decoder, D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS,
                                               &p, sizeof(p), &buffers[0]);
    u32 buffer_count = 1;
    // DXVA HEVC forbids sending a matrix when scaling lists are disabled.
    if (SUCCEEDED(result) && a.scaling_enabled) {
        result = gpu_decoder_upload_buffer(s.context, s.decoder, D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX,
                                           &q, sizeof(q), &buffers[1]);
        ++buffer_count;
    }
    if (FAILED(result))
        return result;
    UINT bit_capacity{};
    void* bit_data{};
    DXVA_Slice_HEVC_Short slices[kHevcMaximumSlices]{};
    result = s.context->GetDecoderBuffer(s.decoder, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, &bit_capacity, &bit_data);
    if (FAILED(result))
        return result;
    const u64 aligned = hevc_gpu_bitstream_extent(picture.bitstream_bytes);
    const u64 slice_bytes = u64{picture.slice_count} * sizeof(DXVA_Slice_HEVC_Short);
    if (!bit_data || aligned > bit_capacity || !picture.slice_count ||
        picture.slice_count > kHevcMaximumSlices)
        result = E_OUTOFMEMORY;
    if (SUCCEEDED(result)) {
        auto* bytes = static_cast<u8*>(bit_data);
        u32 offset{};
        for (u32 i = 0; i < picture.slice_count; ++i) {
            const auto& span = picture.slices[i];
            if (span.offset > unit.bytes.count || span.bytes > unit.bytes.count - span.offset ||
                offset > picture.bitstream_bytes || picture.bitstream_bytes - offset < 3 ||
                span.bytes > picture.bitstream_bytes - offset - 3) {
                result = E_INVALIDARG;
                break;
            }
            slices[i] = {offset, span.bytes + 3, 0};
            bytes[offset++] = 0;
            bytes[offset++] = 0;
            bytes[offset++] = 1;
            copy_bytes(bytes + offset, unit.bytes.data + span.offset, span.bytes);
            offset += span.bytes;
        }
        if (SUCCEEDED(result) && offset != picture.bitstream_bytes)
            result = E_INVALIDARG;
        if (SUCCEEDED(result)) {
            const u32 padding = static_cast<u32>(aligned) - offset;
            clear_bytes(bytes + offset, padding);
            slices[picture.slice_count - 1].SliceBytesInBuffer += padding;
            buffers[buffer_count].BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
            buffers[buffer_count++].DataSize = static_cast<UINT>(aligned);
        }
    }
    // Release the compressed buffer before mapping the separate slice buffer.
    // Drivers may reuse upload memory between decoder buffer types.
    const HRESULT released = s.context->ReleaseDecoderBuffer(s.decoder, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM);
    if (SUCCEEDED(result))
        result = released;
    if (SUCCEEDED(result)) {
        result = gpu_decoder_upload_buffer(s.context, s.decoder, D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL,
                                           slices, static_cast<u32>(slice_bytes), &buffers[buffer_count]);
        ++buffer_count;
    }
    if (SUCCEEDED(result))
        result = s.context->SubmitDecoderBuffers(s.decoder, buffer_count, buffers);
    return result;
}
bool reset(H264GpuDecoder& handle) noexcept {
    auto& s = *static_cast<State*>(handle.state);
    auto cursor = s.demux;
    mp4_demux_reset(&cursor);
    if (!cursor.ready)
        return false;
    copy_value(&s.demux, cursor);
    reset_value(&s.references);
    reset_value(&s.output);
    handle.submitted = false;
    return true;
}
Mp4DecodedFrame output_frame(State& s) noexcept {
    return gpu_decoder_output_frame(&s.output, &s.frame, s.surfaces, s.display);
}
} // namespace

bool hevc_gpu_decoder_initialize(H264GpuDecoder* handle, ConstSlice<u8> encoded, ID3D11Device* device,
                                 ID3D11DeviceContext* context, bool full_stream,
                                 Mp4SourceAvailability availability) noexcept {
    if (!handle || !device || !context || !encoded.data || !encoded.count)
        return false;
    hevc_gpu_decoder_release(handle);
    // A progressive source is borrowed in place; its owner outlives release.
    const bool borrowed = availability.available != nullptr;
    if (encoded.count > (borrowed ? kMediaMaximumProgressiveVideoBytes : kMp4DemuxMaximumEncodedBytes)) {
        handle->status = E_INVALIDARG;
        handle->demux_error = Mp4DemuxError::unsupported;
        handle->parse_error = H264Error::unsupported;
        return false;
    }
    constexpr u32 offset = (sizeof(State) + 15u) & ~15u;
    auto* s = static_cast<State*>(VirtualAlloc(nullptr, u64{offset} + (borrowed ? 0 : encoded.count),
                                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!s) {
        handle->status = E_OUTOFMEMORY;
        return false;
    }
    handle->state = s;
    s->full_stream = full_stream;
    s->output.leased = 255;
    const u8* owned = encoded.data;
    if (!borrowed) {
        auto* copy = reinterpret_cast<u8*>(s) + offset;
        copy_bytes(copy, encoded.data, encoded.count);
        owned = copy;
    }
    handle->demux_error = hevc_demux_initialize(&s->demux, {owned, encoded.count}, availability);
    HRESULT result = handle->demux_error == Mp4DemuxError::none ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(result)) {
        Mp4Demuxer cursor; copy_value(&cursor, s->demux);
        const auto unit = mp4_demux_next_ready(&cursor);
        u8 pps{};
        auto error = unit.ok() ? hevc_picture_parameter_id(unit.bytes, s->demux.nal_length_bytes, &pps)
                               : HevcError::malformed;
        if (error == HevcError::none)
            error = hevc_select_parameters(hevc_demux_hvcc(s->demux), pps, &s->config);
        if (error == HevcError::none)
            error = hevc_parse_picture(unit.bytes, s->config, &s->picture);
        if (error == HevcError::none && !s->picture.irap)
            error = HevcError::unsupported;
        handle->parse_error = mapped(error);
        if (error != HevcError::none)
            result = E_INVALIDARG;
    }
    const auto& a = s->config.sps;
    const auto surface_extent = hevc_gpu_surface_extent(a.coded_width, a.coded_height);
    if (SUCCEEDED(result) &&
        (a.profile != 1 || !surface_extent.width || !a.max_buffering || a.max_buffering > 15 ||
         a.max_reorder > 15 || a.log2_min_cb_minus3 > 3 || !a.width || !a.height ||
         a.crop_x > a.coded_width || a.width > a.coded_width - a.crop_x || a.crop_y > a.coded_height ||
         a.height > a.coded_height - a.crop_y)) {
        handle->parse_error = H264Error::unsupported;
        result = E_INVALIDARG;
    }
    s->display = video_display_rotated(video_display_extent(a.width, a.height, a.sar_width, a.sar_height),
                                       s->demux.rotation);
    if (SUCCEEDED(result) && !s->display.width) {
        handle->parse_error = H264Error::unsupported;
        result = E_INVALIDARG;
    }
    if (SUCCEEDED(result))
        result = device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(&s->device));
    if (SUCCEEDED(result))
        result =
            context->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void**>(&s->context));
    if (SUCCEEDED(result))
        result = context->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&s->guard));
    D3D11_VIDEO_DECODER_DESC desc{D3D11_DECODER_PROFILE_HEVC_VLD_MAIN, a.coded_width, a.coded_height,
                                  DXGI_FORMAT_NV12};
    D3D11_VIDEO_DECODER_CONFIG selected{};
    if (SUCCEEDED(result)) {
        BOOL supported{};
        result = s->device->CheckVideoDecoderFormat(&desc.Guid, DXGI_FORMAT_NV12, &supported);
        if (SUCCEEDED(result) && (!supported || !gpu_decoder_surfaces_supported(device)))
            result = DXGI_ERROR_UNSUPPORTED;
    }
    if (SUCCEEDED(result))
        result = gpu_decoder_select_config(s->device, desc, 1, &selected);
    if (SUCCEEDED(result))
        result = s->device->CreateVideoDecoder(&desc, &selected, &s->decoder);
    if (SUCCEEDED(result)) {
        const u32 count =
            maximum<u32>(selected.ConfigMinRenderTargetBuffCount, maximum<u32>(a.max_buffering, 1) + 2);
        result = gpu_decoder_surfaces_create(&s->surfaces, device, s->device, desc.Guid,
                                             surface_extent.width, surface_extent.height, count,
                                             full_stream ? count : 1u);
    }
    handle->status = result;
    if (FAILED(result)) {
        const auto parse = handle->parse_error;
        const auto demux = handle->demux_error;
        hevc_gpu_decoder_release(handle);
        handle->status = result;
        handle->parse_error = parse;
        handle->demux_error = demux;
        return false;
    }
    s->frame = {s->surfaces.texture, 0, a.crop_x, a.crop_y, a.width, a.height,
                video_matrix_bt709(a.matrix, a.height), a.full_range, s->demux.rotation, {}};
    return true;
}

Mp4DecodedFrame hevc_gpu_decoder_read(H264GpuDecoder* handle, bool loop, u32 budget) noexcept {
    Mp4DecodedFrame frame{};
    frame.error = Mp4DecodeError::invalid_input;
    if (!handle || !handle->state)
        return frame;
    auto& s = *static_cast<State*>(handle->state);
    if (budget && s.output.leased != 255)
        return frame;
    if (!s.full_stream && handle->submitted) {
        frame.error = Mp4DecodeError::end_of_stream;
        return frame;
    }
    s.output.leased = 255;
    u32 submissions{};
    bool restarted{};
    for (;;) {
        if (s.output.count &&
            (!s.full_stream || h264_output_ready(s.output, hevc_reference_surfaces(s.references),
                                                 s.config.sps.max_reorder, s.config.sps.max_buffering))) {
            handle->submitted = true;
            return output_frame(s);
        }
        auto next = s.demux;
        const auto unit = mp4_demux_next(&next);
        if (unit.error == Mp4DemuxError::end_of_stream) {
            if (s.output.count) {
                handle->submitted = true;
                return output_frame(s);
            }
            if (loop && !restarted && s.full_stream && reset(*handle)) {
                restarted = true;
                continue;
            }
            frame.error = Mp4DecodeError::end_of_stream;
            return frame;
        }
        if (unit.ok() && !mp4_demux_available(s.demux, unit.bytes)) {
            frame.error = Mp4DecodeError::data_pending;
            return frame;
        }
        if (budget && submissions >= budget) {
            frame.error = Mp4DecodeError::needs_service;
            return frame;
        }
        HevcPicture picture{};
        const auto error =
            unit.ok() ? hevc_parse_picture(unit.bytes, s.config, &picture) : HevcError::malformed;
        handle->parse_error = mapped(error);
        if (error != HevcError::none) {
            handle->status = E_INVALIDARG;
            frame.error = Mp4DecodeError::stream;
            return frame;
        }
        if (picture.irap && s.output.count) {
            if (picture.no_output_prior)
                reset_value(&s.output);
            else {
                handle->submitted = true;
                return output_frame(s);
            }
        }
        const u8 surface =
            h264_free_surface(s.output, picture.idr ? 0 : hevc_reference_surfaces(s.references),
                              s.full_stream ? s.surfaces.count : 1);
        HevcReferencePlan plan{};
        handle->parse_error =
            mapped(hevc_reference_plan(s.references, s.config.sps, picture, surface, &plan));
        if (handle->parse_error != H264Error::none) {
            handle->status = E_INVALIDARG;
            frame.error = Mp4DecodeError::stream;
            return frame;
        }
        s.guard->Enter();
        HRESULT result = s.context->DecoderBeginFrame(s.decoder, s.surfaces.views[surface], 0, nullptr);
        if (SUCCEEDED(result)) {
            result = submit(s, unit, picture, plan, surface);
            const HRESULT end = s.context->DecoderEndFrame(s.decoder);
            if (SUCCEEDED(result))
                result = end;
        }
        s.guard->Leave();
        handle->status = result;
        if (FAILED(result)) {
            frame.error = Mp4DecodeError::sample;
            return frame;
        }
        s.picture = picture;
        copy_value(&s.demux, next);
        s.references = plan.after;
        if (++s.feedback == 0)
            ++s.feedback;
        ++submissions;
        if (picture.output && !h264_output_push(&s.output, {unit.duration_us, plan.poc, surface,
                                                            unit.presentation_time_us_signed})) {
            handle->status = E_UNEXPECTED;
            frame.error = Mp4DecodeError::sample;
            return frame;
        }
    }
}

bool hevc_gpu_decoder_acknowledge_frame(H264GpuDecoder* handle) noexcept {
    if (!handle || !handle->state)
        return false;
    static_cast<State*>(handle->state)->output.leased = 255;
    return true;
}
ConstSlice<u8> hevc_gpu_decoder_source(const H264GpuDecoder& handle) noexcept {
    const auto* s = static_cast<const State*>(handle.state);
    return s ? s->demux.encoded : ConstSlice<u8>{};
}
H264GpuMovie hevc_gpu_decoder_movie(const H264GpuDecoder& handle) noexcept {
    const auto* s = static_cast<const State*>(handle.state);
    if (!s || !s->demux.ready)
        return {};
    Mp4Demuxer cursor; copy_value(&cursor, s->demux);
    mp4_demux_reset(&cursor);
    const auto first = mp4_demux_next(&cursor);
    return {s->demux.duration_us, s->demux.sample_count, s->demux.has_edit_list,
            first.presentation_time_us_signed, first.ok()};
}
Mp4DemuxError hevc_gpu_decoder_seek(H264GpuDecoder* handle, i64 target, Mp4SeekPoint* point) noexcept {
    if (!handle || !handle->state || !point)
        return Mp4DemuxError::invalid_input;
    auto& s = *static_cast<State*>(handle->state);
    const ConstSlice<u8> out{reinterpret_cast<const u8*>(point), sizeof(*point)};
    if (!s.full_stream || s.output.leased != 255 || !byte_range_valid(out) ||
        byte_ranges_overlap(out, {reinterpret_cast<const u8*>(handle), sizeof(*handle)}) ||
        byte_ranges_overlap(out, {reinterpret_cast<const u8*>(&s), sizeof(s)}) ||
        byte_ranges_overlap(out, s.demux.encoded))
        return Mp4DemuxError::invalid_input;
    auto next = s.demux;
    Mp4SeekPoint selected{};
    const auto result = hevc_demux_seek_sync(&next, target, &selected);
    if (result != Mp4DemuxError::none)
        return result;
    auto probe = next;
    const auto unit = mp4_demux_next_ready(&probe);
    if (unit.error == Mp4DemuxError::data_pending)
        return unit.error;
    HevcPicture picture{};
    if (!unit.ok() || hevc_parse_picture(unit.bytes, s.config, &picture) != HevcError::none ||
        !picture.irap)
        return Mp4DemuxError::unsupported;
    copy_value(&s.demux, next);
    reset_value(&s.references);
    reset_value(&s.output);
    handle->submitted = false;
    handle->status = S_OK;
    handle->parse_error = H264Error::none;
    *point = selected;
    return result;
}
bool hevc_gpu_decoder_seek_start(H264GpuDecoder* handle) noexcept {
    if (!handle || !handle->state || static_cast<State*>(handle->state)->output.leased != 255)
        return false;
    if (!reset(*handle))
        return false;
    handle->status = S_OK;
    handle->parse_error = H264Error::none;
    return true;
}
void hevc_gpu_decoder_release(H264GpuDecoder* handle) noexcept {
    if (!handle)
        return;
    if (auto* s = static_cast<State*>(handle->state)) {
        gpu_decoder_surfaces_release(&s->surfaces);
        com_release(s->decoder);
        gpu_decoder_fence_release(&s->wait);
        com_release(s->guard);
        com_release(s->context);
        com_release(s->device);
        VirtualFree(s, 0, MEM_RELEASE);
    }
    reset_value(handle);
}
bool hevc_gpu_decoder_wait(H264GpuDecoder* handle, u32 timeout) noexcept {
    if (!handle || !handle->state || !timeout || timeout > 16)
        return false;
    auto& s = *static_cast<State*>(handle->state);
    const HRESULT result = gpu_decoder_fence_wait(&s.wait, s.context, s.guard, timeout);
    if (FAILED(result))
        handle->status = result;
    return SUCCEEDED(result);
}
} // namespace maicord
