#include "mp4_mft_decoder.hpp"
#include "mp4_mft_trace.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/media/mp4_demux.hpp"
#include "maicord/media/yuv_convert.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <d3d11.h>
#include <codecapi.h>

namespace maicord {
namespace {

void record(Mp4DirectTrace* trace, Mp4DirectOperation operation, u32 sample,
    i64 begin, HRESULT status) noexcept {
    const i64 end = clock_now();
    if (trace->count < trace->events.count) {
        trace->events.data[trace->count++] = {begin, end, operation, sample, status};
    } else { ++trace->dropped; }
}
template<bool Profile, class Call>
__forceinline HRESULT traced(Mp4DirectTrace* trace, Mp4DirectOperation operation,
    u32 sample, Call call) noexcept {
    if constexpr (!Profile) { return call(); }
    else {
        const i64 begin = clock_now();
        const HRESULT result = call();
        record(trace, operation, sample, begin, result);
        return result;
    }
}

struct DirectState {
    IMFTransform* transform;
    IMFSample* output_sample;
    IMFMediaBuffer* output_buffer;
    IMFDXGIDeviceManager* device_manager;
    IMFSample* gpu_sample;
    GpuVideoFrame gpu_frame;
    u32 output_buffer_bytes;
    Mp4DirectOutputLayout output_layout;
    Mp4Demuxer demuxer;
    GUID output_subtype;
    MFVideoTransferMatrix matrix;
    MFNominalRange range;
    HRESULT last_status;
    bool transform_provides_samples;
    bool draining;
    bool configuration_pending;
    u32 output_count;
};

Mp4DecodedFrame failure(Mp4DecodeError error) noexcept {
    Mp4DecodedFrame result{};
    result.error = error;
    return result;
}

void release_unknown(IUnknown* value) noexcept {
    if (value) {
        value->Release();
    }
}

bool guid_equal(const GUID& left, const GUID& right) noexcept {
    return left.Data1 == right.Data1 && left.Data2 == right.Data2 &&
           left.Data3 == right.Data3 &&
           bytes_equal(left.Data4, right.Data4, sizeof(left.Data4));
}

bool configure_output_surface(Mp4DirectDecoder* decoder, u32 width,
                   u32 height) noexcept {
    if (!decoder || width == 0 || height == 0 ||
        width > kMp4DecodeMaximumDimension ||
        height > kMp4DecodeMaximumDimension) {
        return false;
    }
    const u64 bytes = static_cast<u64>(width) * height * 4u;
    if (bytes > ~0u) {
        return false;
    }
    if (!decoder->gpu && (!decoder->rgba8 || decoder->rgba8_capacity < bytes)) {
        const u32 capacity = static_cast<u32>((bytes + 65'535u) & ~65'535ull);
        void* allocation = VirtualAlloc(nullptr, capacity,
                                        MEM_RESERVE | MEM_COMMIT,
                                        PAGE_READWRITE);
        if (!allocation) {
            return false;
        }
        if (decoder->rgba8) {
            VirtualFree(decoder->rgba8, 0, MEM_RELEASE);
        }
        decoder->rgba8 = allocation;
        decoder->rgba8_capacity = capacity;
    }
    decoder->width = width;
    decoder->height = height;
    decoder->row_pitch = decoder->gpu ? 0 : width * 4u;
    return true;
}

bool append_start_code_nal(u8* destination, u32 capacity, u32* cursor,
                           const u8* nal, u32 nal_bytes) noexcept {
    if (!destination || !cursor || !nal || nal_bytes == 0 ||
        *cursor > capacity || capacity - *cursor < 4u ||
        nal_bytes > capacity - *cursor - 4u) {
        return false;
    }
    destination[(*cursor)++] = 0;
    destination[(*cursor)++] = 0;
    destination[(*cursor)++] = 0;
    destination[(*cursor)++] = 1;
    copy_bytes(destination + *cursor, nal, nal_bytes);
    *cursor += nal_bytes;
    return true;
}

bool append_configuration(const Mp4Demuxer& demuxer, u8* destination,
                          u32 capacity, u32* cursor) noexcept {
    const ConstSlice<u8> avcc = mp4_demux_avcc(demuxer);
    if (!avcc.data || avcc.count < 7u) {
        return false;
    }
    u32 source = 6;
    const u32 sps_count = avcc.data[5] & 31u;
    if (sps_count == 0) {
        return false;
    }
    for (u32 index = 0; index < sps_count; ++index) {
        if (avcc.count - source < 2u) {
            return false;
        }
        const u32 bytes = (static_cast<u32>(avcc.data[source]) << 8u) |
                          avcc.data[source + 1u];
        source += 2;
        if (bytes == 0 || bytes > avcc.count - source ||
            !append_start_code_nal(destination, capacity, cursor,
                                   avcc.data + source, bytes)) {
            return false;
        }
        source += bytes;
    }
    if (source >= avcc.count) {
        return false;
    }
    const u32 pps_count = avcc.data[source++];
    if (pps_count == 0) {
        return false;
    }
    for (u32 index = 0; index < pps_count; ++index) {
        if (avcc.count - source < 2u) {
            return false;
        }
        const u32 bytes = (static_cast<u32>(avcc.data[source]) << 8u) |
                          avcc.data[source + 1u];
        source += 2;
        if (bytes == 0 || bytes > avcc.count - source ||
            !append_start_code_nal(destination, capacity, cursor,
                                   avcc.data + source, bytes)) {
            return false;
        }
        source += bytes;
    }
    return true;
}

bool append_access_unit(const Mp4Demuxer& demuxer,
                        ConstSlice<u8> sample, u8* destination,
                        u32 capacity, u32* cursor) noexcept {
    u32 source = 0;
    while (source < sample.count) {
        if (sample.count - source < demuxer.nal_length_bytes) {
            return false;
        }
        u32 bytes = 0;
        for (u32 index = 0; index < demuxer.nal_length_bytes; ++index) {
            bytes = (bytes << 8u) | sample.data[source + index];
        }
        source += demuxer.nal_length_bytes;
        if (bytes == 0 || bytes > sample.count - source ||
            !append_start_code_nal(destination, capacity, cursor,
                                   sample.data + source, bytes)) {
            return false;
        }
        source += bytes;
    }
    return true;
}

HRESULT set_input_type(DirectState* state) noexcept {
    IMFMediaType* type = nullptr;
    HRESULT status = MFCreateMediaType(&type);
    if (SUCCEEDED(status)) {
        status = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    }
    if (SUCCEEDED(status)) {
        status = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    }
    if (SUCCEEDED(status)) {
        status = MFSetAttributeSize(type, MF_MT_FRAME_SIZE,
                                    state->demuxer.width,
                                    state->demuxer.height);
    }
    if (SUCCEEDED(status)) {
        status = MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    }
    if (SUCCEEDED(status)) {
        status = type->SetUINT32(MF_MT_INTERLACE_MODE,
                                 MFVideoInterlace_Progressive);
    }
    if (SUCCEEDED(status)) {
        status = state->transform->SetInputType(0, type, 0);
    }
    release_unknown(type);
    return status;
}

HRESULT set_output_type(DirectState* state) noexcept {
    IMFMediaType* selected = nullptr;
    IMFMediaType* first = nullptr;
    for (DWORD index = 0;; ++index) {
        IMFMediaType* candidate = nullptr;
        const HRESULT available =
            state->transform->GetOutputAvailableType(0, index, &candidate);
        if (available == MF_E_NO_MORE_TYPES) {
            break;
        }
        if (FAILED(available)) {
            release_unknown(candidate);
            release_unknown(first);
            return available;
        }
        GUID subtype{};
        if (SUCCEEDED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype))) {
            if (!first && (guid_equal(subtype, MFVideoFormat_NV12) ||
                           guid_equal(subtype, MFVideoFormat_I420) ||
                           guid_equal(subtype, MFVideoFormat_IYUV))) {
                first = candidate;
                first->AddRef();
            }
            if (guid_equal(subtype, MFVideoFormat_NV12)) {
                selected = candidate;
                break;
            }
        }
        release_unknown(candidate);
    }
    if (!selected) {
        selected = first;
        first = nullptr;
    }
    release_unknown(first);
    if (!selected) {
        return MF_E_INVALIDMEDIATYPE;
    }
    HRESULT status = state->transform->SetOutputType(0, selected, 0);
    if (SUCCEEDED(status)) {
        status = selected->GetGUID(MF_MT_SUBTYPE, &state->output_subtype);
    }
    UINT32 matrix = MFVideoTransferMatrix_Unknown;
    UINT32 range = MFNominalRange_Unknown;
    selected->GetUINT32(MF_MT_YUV_MATRIX, &matrix);
    selected->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range);
    state->matrix = static_cast<MFVideoTransferMatrix>(matrix);
    state->range = static_cast<MFNominalRange>(range);
    Mp4DirectOutputLayout layout{};
    if (SUCCEEDED(status)) {
        status = MFGetAttributeSize(selected, MF_MT_FRAME_SIZE,
                                    &layout.storage_width,
                                    &layout.storage_height);
    }
    layout.width = state->demuxer.width;
    layout.height = state->demuxer.height;
    MFVideoArea aperture{};
    UINT32 aperture_bytes = 0;
    HRESULT aperture_status = selected->GetBlob(
        MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&aperture),
        sizeof(aperture), &aperture_bytes);
    if (aperture_status == MF_E_ATTRIBUTENOTFOUND) {
        aperture_status = selected->GetBlob(
            MF_MT_GEOMETRIC_APERTURE, reinterpret_cast<UINT8*>(&aperture),
            sizeof(aperture), &aperture_bytes);
    }
    if (SUCCEEDED(aperture_status)) {
        if (aperture_bytes != sizeof(aperture) ||
            aperture.OffsetX.value < 0 || aperture.OffsetY.value < 0 ||
            aperture.OffsetX.fract != 0 || aperture.OffsetY.fract != 0 ||
            aperture.Area.cx <= 0 || aperture.Area.cy <= 0) {
            status = MF_E_INVALIDMEDIATYPE;
        } else {
            layout.crop_x = static_cast<u32>(aperture.OffsetX.value);
            layout.crop_y = static_cast<u32>(aperture.OffsetY.value);
            layout.width = static_cast<u32>(aperture.Area.cx);
            layout.height = static_cast<u32>(aperture.Area.cy);
        }
    } else if (aperture_status != MF_E_ATTRIBUTENOTFOUND) {
        status = aperture_status;
    }
    if (FAILED(selected->GetUINT32(MF_MT_DEFAULT_STRIDE, &layout.pitch))) {
        layout.pitch = (layout.storage_width + 1u) & ~1u;
    }
    if (layout.storage_width == 0 || layout.storage_height == 0 ||
        layout.storage_width > kMp4DecodeMaximumDimension + 32u ||
        layout.storage_height > kMp4DecodeMaximumDimension + 32u ||
        layout.width == 0 || layout.height == 0 ||
        layout.width > kMp4DecodeMaximumDimension ||
        layout.height > kMp4DecodeMaximumDimension ||
        layout.crop_x > layout.storage_width ||
        layout.width > layout.storage_width - layout.crop_x ||
        layout.crop_y > layout.storage_height ||
        layout.height > layout.storage_height - layout.crop_y ||
        ((layout.crop_x | layout.crop_y) & 1u) != 0 ||
        layout.pitch < ((layout.storage_width + 1u) & ~1u) ||
        layout.pitch > (kMp4DecodeMaximumDimension + 32u) * 4u) {
        status = MF_E_INVALIDMEDIATYPE;
    }
    if (SUCCEEDED(status)) {
        state->output_layout = layout;
    }
    release_unknown(selected);
    return status;
}

HRESULT initialize_output_sample(DirectState* state) noexcept {
    MFT_OUTPUT_STREAM_INFO stream{};
    HRESULT status = state->transform->GetOutputStreamInfo(0, &stream);
    if (FAILED(status)) {
        return status;
    }
    state->transform_provides_samples =
        (stream.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
    if (state->device_manager && !state->transform_provides_samples) {
        return MF_E_UNSUPPORTED_D3D_TYPE;
    }
    if (state->transform_provides_samples) {
        return S_OK;
    }
    const Mp4DirectOutputLayout& layout = state->output_layout;
    const u64 minimum_bytes = static_cast<u64>(layout.pitch) *
        (layout.storage_height + (layout.storage_height + 1u) / 2u);
    const u64 requested = maximum<u64>(stream.cbSize, minimum_bytes);
    if (requested == 0 || requested > ~0u) {
        return E_OUTOFMEMORY;
    }
    state->output_buffer_bytes = static_cast<u32>(requested);
    status = MFCreateSample(&state->output_sample);
    if (SUCCEEDED(status)) {
        status = MFCreateMemoryBuffer(state->output_buffer_bytes,
                                      &state->output_buffer);
    }
    if (SUCCEEDED(status)) {
        status = state->output_sample->AddBuffer(state->output_buffer);
    }
    return status;
}

HRESULT reset_transform(DirectState* state) noexcept {
    HRESULT status = state->transform->ProcessMessage(
        MFT_MESSAGE_COMMAND_FLUSH, 0);
    if (SUCCEEDED(status)) {
        status = state->transform->ProcessMessage(
            MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    }
    if (SUCCEEDED(status)) {
        status = state->transform->ProcessMessage(
            MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    }
    if (SUCCEEDED(status)) {
        mp4_demux_reset(&state->demuxer);
        if (!state->demuxer.ready) {
            status = E_FAIL;
        }
    }
    state->draining = false;
    state->configuration_pending = true;
    state->output_count = 0;
    return status;
}

template<bool Profile>
HRESULT submit_sample(DirectState* state, const Mp4AccessUnit& unit,
    Mp4DirectTrace* trace) noexcept {
    const i64 prepare_begin = Profile ? clock_now() : 0;
    // Length-prefixed AVC normally uses four-byte lengths, making the
    // conversion size-neutral.  Account for valid one/two-byte variants
    // without pre-scanning every access unit or relying on arbitrary slack.
    const u32 length_bytes = state->demuxer.nal_length_bytes;
    if (length_bytes == 0 || length_bytes > 4u) {
        return E_INVALIDARG;
    }
    const u64 maximum_nals = unit.bytes.count / (length_bytes + 1u);
    const u64 start_code_growth =
        length_bytes < 4u ? maximum_nals * (4u - length_bytes) : 0u;
    // avcC uses two-byte SPS/PPS lengths.  There can be at most 31 SPS and
    // 255 PPS entries, so 572 bytes covers all prefix growth exactly.
    const u64 configuration_capacity = state->configuration_pending
        ? static_cast<u64>(state->demuxer.avcc_bytes) + 572u
        : 0u;
    const u64 capacity64 = static_cast<u64>(unit.bytes.count) +
                           start_code_growth + configuration_capacity;
    if (capacity64 > ~0u) {
        return E_OUTOFMEMORY;
    }
    IMFMediaBuffer* buffer = nullptr;
    IMFSample* sample = nullptr;
    HRESULT status = MFCreateMemoryBuffer(static_cast<DWORD>(capacity64),
                                          &buffer);
    if (SUCCEEDED(status)) {
        status = MFCreateSample(&sample);
    }
    if (SUCCEEDED(status)) {
        status = sample->AddBuffer(buffer);
    }
    BYTE* destination = nullptr;
    DWORD maximum = 0;
    DWORD current = 0;
    if (SUCCEEDED(status)) {
        status = buffer->Lock(&destination, &maximum, &current);
    }
    u32 written = 0;
    if (SUCCEEDED(status) && state->configuration_pending &&
        !append_configuration(state->demuxer, destination, maximum,
                              &written)) {
        status = E_FAIL;
    }
    if (SUCCEEDED(status) &&
        !append_access_unit(state->demuxer, unit.bytes, destination, maximum,
                            &written)) {
        status = E_FAIL;
    }
    if (destination) {
        buffer->Unlock();
    }
    if (SUCCEEDED(status)) {
        status = buffer->SetCurrentLength(written);
    }
    if (SUCCEEDED(status)) {
        status = sample->SetSampleTime(
            static_cast<LONGLONG>(unit.presentation_time_us * 10u));
    }
    if (SUCCEEDED(status)) {
        status = sample->SetSampleDuration(
            static_cast<LONGLONG>(unit.duration_us * 10u));
    }
    if (SUCCEEDED(status) && unit.sync) {
        sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
    }
    if constexpr (Profile) record(trace, Mp4DirectOperation::input_prepare,
        unit.sample_index, prepare_begin, status);
    if (SUCCEEDED(status)) {
        status = traced<Profile>(trace, Mp4DirectOperation::process_input, unit.sample_index,
            [&]() noexcept { return state->transform->ProcessInput(0, sample, 0); });
    }
    if (SUCCEEDED(status)) {
        state->configuration_pending = false;
    }
    release_unknown(sample);
    release_unknown(buffer);
    return status;
}

bool convert_yuv(DirectState* state, Mp4DirectDecoder* decoder,
                 IMFMediaBuffer* buffer) noexcept {
    BYTE* source = nullptr;
    DWORD maximum = 0;
    DWORD current = 0;
    if (FAILED(buffer->Lock(&source, &maximum, &current)) || !source) {
        return false;
    }
    const u32 width = decoder->width;
    const u32 height = decoder->height;
    const Mp4DirectOutputLayout& layout = state->output_layout;
    const u32 pitch = layout.pitch;
    const bool nv12 = guid_equal(state->output_subtype, MFVideoFormat_NV12);
    const u32 stored_height = layout.storage_height;
    const u64 y_bytes = static_cast<u64>(pitch) * stored_height;
    const u64 chroma_rows = (stored_height + 1u) / 2u;
    const u64 needed = nv12 ? y_bytes + static_cast<u64>(pitch) * chroma_rows
                            : y_bytes + static_cast<u64>((pitch + 1u) / 2u) *
                                            chroma_rows * 2u;
    if (pitch < width || current < needed) {
        buffer->Unlock();
        return false;
    }
    const u8* y_plane = source + static_cast<u64>(layout.crop_y) * pitch +
                        layout.crop_x;
    const u32 chroma_pitch = nv12 ? pitch : (pitch + 1u) / 2u;
    const u64 chroma_crop = static_cast<u64>(layout.crop_y / 2u) *
                            chroma_pitch +
                            (nv12 ? layout.crop_x : layout.crop_x / 2u);
    const u8* u_plane = source + y_bytes + chroma_crop;
    const u8* v_plane = nv12 ? nullptr : source + y_bytes +
        static_cast<u64>(chroma_pitch) * chroma_rows + chroma_crop;
    const bool bt709 = state->matrix == MFVideoTransferMatrix_BT709;
    const bool full = state->range == MFNominalRange_0_255;
    auto* output = static_cast<u8*>(decoder->rgba8);
    if (nv12 && IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE)) {
        nv12_to_rgba8_avx2(y_plane, u_plane, pitch, output,
                           decoder->row_pitch, width, height, bt709, full);
        buffer->Unlock();
        return true;
    }
    yuv420_to_rgba8_scalar(y_plane, u_plane, v_plane, pitch, chroma_pitch,
                           output, decoder->row_pitch, width, height, bt709,
                           full);
    buffer->Unlock();
    return true;
}

template<bool Profile>
Mp4DecodedFrame process_output(DirectState* state,
                               Mp4DirectDecoder* decoder,
                               HRESULT* status, Mp4DirectTrace* trace) noexcept {
    if (state->output_buffer) {
        state->output_buffer->SetCurrentLength(0);
    }
    MFT_OUTPUT_DATA_BUFFER output{};
    output.dwStreamID = 0;
    output.pSample = state->transform_provides_samples
                         ? nullptr
                         : state->output_sample;
    DWORD output_status = 0;
    *status = traced<Profile>(trace, Mp4DirectOperation::process_output, state->demuxer.sample_index,
        [&]() noexcept { return state->transform->ProcessOutput(0, 1, &output, &output_status); });
    if (output.pEvents) {
        output.pEvents->Release();
    }
    if (FAILED(*status)) {
        if (state->transform_provides_samples) {
            release_unknown(output.pSample);
        }
        return failure(Mp4DecodeError::sample);
    }
    IMFSample* sample = output.pSample;
    if (state->device_manager) {
        const i64 extract_begin = Profile ? clock_now() : 0;
        IMFMediaBuffer* buffer = nullptr;
        IMFDXGIBuffer* dxgi_buffer = nullptr;
        ID3D11Texture2D* texture = nullptr;
        UINT subresource = 0;
        HRESULT extracted = sample ? sample->GetBufferByIndex(0, &buffer) : E_FAIL;
        if (SUCCEEDED(extracted)) {
            extracted = buffer->QueryInterface(__uuidof(IMFDXGIBuffer),
                reinterpret_cast<void**>(&dxgi_buffer));
        }
        if (SUCCEEDED(extracted)) {
            extracted = dxgi_buffer->GetResource(__uuidof(ID3D11Texture2D),
                reinterpret_cast<void**>(&texture));
        }
        if (SUCCEEDED(extracted)) {
            extracted = dxgi_buffer->GetSubresourceIndex(&subresource);
        }
        D3D11_TEXTURE2D_DESC description{};
        if (texture) {
            texture->GetDesc(&description);
        }
        const Mp4DirectOutputLayout& layout = state->output_layout;
        if (SUCCEEDED(extracted) &&
            (description.Format != DXGI_FORMAT_NV12 ||
             subresource >= description.ArraySize * description.MipLevels ||
             layout.crop_x + layout.width > description.Width ||
             layout.crop_y + layout.height > description.Height)) {
            extracted = MF_E_INVALIDMEDIATYPE;
        }
        release_unknown(dxgi_buffer);
        release_unknown(buffer);
        if constexpr (Profile) record(trace, Mp4DirectOperation::extract_frame,
            state->demuxer.sample_index, extract_begin, extracted);
        if (FAILED(extracted)) {
            release_unknown(texture);
            release_unknown(sample);
            *status = extracted;
            return failure(Mp4DecodeError::buffer);
        }
        // Keep the sample, not just its texture: the decoder pool tracks the
        // sample lease when deciding whether an array slice can be reused.
        state->gpu_sample = sample;
        state->gpu_frame = GpuVideoFrame{texture, subresource,
            layout.crop_x, layout.crop_y, layout.width, layout.height,
            state->matrix == MFVideoTransferMatrix_BT709,
            state->range == MFNominalRange_0_255, {}};
        LONGLONG duration = 0;
        sample->GetSampleDuration(&duration);
        Mp4DecodedFrame frame{};
        frame.duration_us = duration > 0 ? maximum<u64>(1, static_cast<u64>(duration) / 10u) : 33'334u;
        frame.width = layout.width;
        frame.height = layout.height;
        frame.gpu = &state->gpu_frame;
        return frame;
    }
    // The ordinary path owns this single linear buffer. Reacquiring it via
    // ConvertToContiguousBuffer adds a COM call and AddRef/Release per frame.
    IMFMediaBuffer* contiguous = state->output_buffer;
    const HRESULT buffer_status = state->transform_provides_samples
        ? (sample ? sample->ConvertToContiguousBuffer(&contiguous) : E_FAIL)
        : (contiguous ? S_OK : E_FAIL);
    LONGLONG duration = 0;
    const HRESULT duration_status = sample
                                        ? sample->GetSampleDuration(&duration)
                                        : E_FAIL;
    const bool converted = SUCCEEDED(buffer_status) && contiguous &&
                           convert_yuv(state, decoder, contiguous);
    if (state->transform_provides_samples) {
        release_unknown(contiguous);
        release_unknown(sample);
    }
    if (!converted) {
        return failure(Mp4DecodeError::buffer);
    }
    return Mp4DecodedFrame{
        ConstSlice<u8>{static_cast<const u8*>(decoder->rgba8),
                       decoder->row_pitch * decoder->height},
        SUCCEEDED(duration_status) && duration > 0
            ? maximum<u64>(1, static_cast<u64>(duration) / 10u)
            : 33'334u,
        decoder->width, decoder->height, decoder->row_pitch,
        Mp4DecodeError::none};
}

} // namespace

bool mp4_direct_decoder_initialize(Mp4DirectDecoder* decoder,
                                   ConstSlice<u8> encoded,
                                   ID3D11Device* video_device) noexcept {
    if (!decoder || !encoded.data || encoded.count == 0) {
        return false;
    }
    mp4_direct_decoder_release(decoder);
    // The compressed input has exactly the decoder state's lifetime. Keep it
    // in the same reservation instead of allocating a second owner/region.
    constexpr u32 input_offset = (sizeof(DirectState) + 15u) & ~15u;
    auto* state = static_cast<DirectState*>(VirtualAlloc(
        nullptr, static_cast<SIZE_T>(input_offset) + encoded.count, MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE));
    if (!state) {
        return false;
    }
    auto* owned_input = reinterpret_cast<u8*>(state) + input_offset;
    copy_bytes(owned_input, encoded.data, encoded.count);
    HRESULT status = mp4_demux_initialize(
                         &state->demuxer,
                         ConstSlice<u8>{owned_input, encoded.count}) ==
                             Mp4DemuxError::none
                         ? S_OK
                         : E_FAIL;
    if (SUCCEEDED(status)) {
        status = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  __uuidof(IMFTransform),
                                  reinterpret_cast<void**>(&state->transform));
    }
    if (SUCCEEDED(status)) {
        if (video_device) {
            UINT token = 0;
            status = MFCreateDXGIDeviceManager(&token, &state->device_manager);
            if (SUCCEEDED(status)) {
                status = state->device_manager->ResetDevice(video_device, token);
            }
            if (SUCCEEDED(status)) {
                status = state->transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                    reinterpret_cast<ULONG_PTR>(state->device_manager));
            }
            decoder->gpu = &state->gpu_frame;
            // The H.264 MFT reads this policy during media-type/stream setup.
            // Setting it after initialize (even before the first input) leaves
            // its startup buffering in place. CPU fallback keeps its defaults.
            IMFAttributes* attributes = nullptr;
            if (SUCCEEDED(status)) {
                status = state->transform->GetAttributes(&attributes);
            }
            if (SUCCEEDED(status)) {
                status = attributes->SetUINT32(CODECAPI_AVLowLatencyMode, 1);
            }
            release_unknown(attributes);
        }
    }
    if (SUCCEEDED(status)) {
        status = set_input_type(state);
    }
    if (SUCCEEDED(status)) {
        status = set_output_type(state);
    }
    if (SUCCEEDED(status)) {
        status = initialize_output_sample(state);
    }
    if (SUCCEEDED(status) &&
        !configure_output_surface(decoder, state->output_layout.width,
                       state->output_layout.height)) {
        status = E_OUTOFMEMORY;
    }
    if (SUCCEEDED(status)) {
        status = reset_transform(state);
    }
    if (FAILED(status)) {
        decoder->state = state;
        mp4_direct_decoder_release(decoder);
        return false;
    }
    decoder->state = state;
    decoder->ready = true;
    return true;
}

template<bool Profile>
static Mp4DecodedFrame read_decoder(Mp4DirectDecoder* decoder,
    bool loop_at_end, Mp4DirectTrace* trace) noexcept {
    if (!decoder || !decoder->ready || !decoder->state ||
        (!decoder->rgba8 && !decoder->gpu)) {
        return failure(Mp4DecodeError::invalid_input);
    }
    auto* state = static_cast<DirectState*>(decoder->state);
    const bool prefill = state->device_manager && state->gpu_sample && !state->draining;
    release_unknown(state->gpu_frame.texture);
    release_unknown(state->gpu_sample);
    state->gpu_frame = GpuVideoFrame{};
    state->gpu_sample = nullptr;
    // Keep the first frame immediate, then give the hardware bounded feed-ahead.
    // Eight outstanding access units recovered steady throughput in the tested
    // corpus without adding another queue or changing decoder surface leases.
    // This is a speculative-input cap, not a reference/reorder-buffer limit:
    // NEED_MORE_INPUT below can still request further dependencies.
    // A speculative cursor commits only after ProcessInput accepts the sample.
    if (prefill) {
        while (state->demuxer.sample_index - state->output_count < 8u &&
               state->demuxer.sample_index < state->demuxer.sample_count) {
            DWORD flags = 0;
            if (FAILED(state->transform->GetInputStatus(0, &flags)) ||
                !(flags & MFT_INPUT_STATUS_ACCEPT_DATA)) break;
            Mp4Demuxer next = state->demuxer;
            const Mp4AccessUnit unit = mp4_demux_next(&next);
            if (!unit.ok()) break; // The ordinary path will report the error.
            const HRESULT status = submit_sample<Profile>(state, unit, trace);
            if (status == MF_E_NOTACCEPTING) break;
            if (FAILED(status)) {
                state->last_status = status;
                return failure(Mp4DecodeError::sample);
            }
            state->demuxer = next;
        }
    }
    for (u32 attempt = 0; attempt < state->demuxer.sample_count + 32u;
         ++attempt) {
        HRESULT output_status = S_OK;
        const Mp4DecodedFrame output = process_output<Profile>(state, decoder,
                                                      &output_status, trace);
        if (output.ok()) {
            ++state->output_count;
            return output;
        }
        if (output_status == MF_E_TRANSFORM_STREAM_CHANGE) {
            release_unknown(state->output_buffer);
            release_unknown(state->output_sample);
            state->output_buffer = nullptr;
            state->output_sample = nullptr;
            state->output_buffer_bytes = 0;
            if (FAILED(traced<Profile>(trace, Mp4DirectOperation::output_type, state->demuxer.sample_index,
                    [&]() noexcept { return set_output_type(state); })) ||
                FAILED(traced<Profile>(trace, Mp4DirectOperation::output_sample, state->demuxer.sample_index,
                    [&]() noexcept { return initialize_output_sample(state); })) ||
                !configure_output_surface(decoder, state->output_layout.width,
                                state->output_layout.height)) {
                state->last_status = MF_E_TRANSFORM_STREAM_CHANGE;
                return failure(Mp4DecodeError::media_type);
            }
            continue;
        }
        if (output_status != MF_E_TRANSFORM_NEED_MORE_INPUT) {
            state->last_status = output_status;
            return output;
        }
        if (state->draining) {
            if (!loop_at_end) {
                return failure(Mp4DecodeError::end_of_stream);
            }
            if (FAILED(traced<Profile>(trace, Mp4DirectOperation::reset, state->demuxer.sample_index,
                    [&]() noexcept { return reset_transform(state); }))) {
                return failure(Mp4DecodeError::sample);
            }
            continue;
        }
        const Mp4AccessUnit unit = mp4_demux_next(&state->demuxer);
        if (unit.error == Mp4DemuxError::end_of_stream) {
            HRESULT status = traced<Profile>(trace, Mp4DirectOperation::end_of_stream, state->demuxer.sample_index,
                [&]() noexcept { return state->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0); });
            if (SUCCEEDED(status)) {
                status = traced<Profile>(trace, Mp4DirectOperation::drain, state->demuxer.sample_index,
                    [&]() noexcept { return state->transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0); });
            }
            if (FAILED(status)) {
                return failure(Mp4DecodeError::sample);
            }
            state->draining = true;
            continue;
        }
        if (!unit.ok()) {
            return failure(Mp4DecodeError::stream);
        }
        const HRESULT submitted = submit_sample<Profile>(state, unit, trace);
        if (submitted == MF_E_NOTACCEPTING) {
            // ProcessOutput explicitly asked for more input.  Silently
            // continuing here would consume and drop this access unit.
            state->last_status = submitted;
            return failure(Mp4DecodeError::sample);
        }
        if (FAILED(submitted)) {
            state->last_status = submitted;
            return failure(Mp4DecodeError::sample);
        }
    }
    return failure(Mp4DecodeError::sample);
}

Mp4DecodedFrame mp4_direct_decoder_read(Mp4DirectDecoder* decoder, bool loop_at_end) noexcept {
    return read_decoder<false>(decoder, loop_at_end, nullptr);
}

Mp4DecodedFrame mp4_direct_decoder_read_profiled(Mp4DirectDecoder* decoder,
    bool loop_at_end, Mp4DirectTrace* trace) noexcept {
    if (!trace || !trace->events.data || !trace->events.count) return failure(Mp4DecodeError::invalid_input);
    return read_decoder<true>(decoder, loop_at_end, trace);
}

i32 mp4_direct_decoder_last_status(const Mp4DirectDecoder* decoder) noexcept {
    return decoder && decoder->state
               ? static_cast<i32>(static_cast<const DirectState*>(
                                      decoder->state)->last_status)
               : 0;
}

IMFTransform* mp4_direct_decoder_transform(const Mp4DirectDecoder* decoder) noexcept {
    return decoder && decoder->state
        ? static_cast<const DirectState*>(decoder->state)->transform : nullptr;
}

Mp4DirectOutputLayout mp4_direct_decoder_output_layout(
    const Mp4DirectDecoder* decoder) noexcept {
    return decoder && decoder->state
        ? static_cast<const DirectState*>(decoder->state)->output_layout
        : Mp4DirectOutputLayout{};
}

bool mp4_direct_decoder_copy_i420(const Mp4DirectDecoder* decoder,
                                  Slice<u8> destination) noexcept {
    if (!decoder || !decoder->ready || !decoder->state || !destination.data) {
        return false;
    }
    const auto* state = static_cast<const DirectState*>(decoder->state);
    const Mp4DirectOutputLayout& layout = state->output_layout;
    const u32 chroma_width = (layout.width + 1u) / 2u;
    const u32 chroma_height = (layout.height + 1u) / 2u;
    const u64 pixels = static_cast<u64>(layout.width) * layout.height;
    const u64 chroma_pixels = static_cast<u64>(chroma_width) * chroma_height;
    if (!state->output_buffer || pixels + chroma_pixels * 2u > destination.count) {
        return false;
    }
    BYTE* source = nullptr;
    DWORD bytes = 0;
    if (FAILED(state->output_buffer->Lock(&source, nullptr, &bytes))) {
        return false;
    }
    const bool nv12 = guid_equal(state->output_subtype, MFVideoFormat_NV12);
    const u32 chroma_pitch = nv12 ? layout.pitch : (layout.pitch + 1u) / 2u;
    const u64 y_bytes = static_cast<u64>(layout.pitch) * layout.storage_height;
    const u64 plane_bytes = static_cast<u64>(chroma_pitch) *
                            ((layout.storage_height + 1u) / 2u);
    const bool valid = source && bytes >= y_bytes + plane_bytes * (nv12 ? 1u : 2u);
    if (valid) {
        for (u32 y = 0; y < layout.height; ++y) {
            copy_bytes(destination.data + static_cast<u64>(y) * layout.width,
                source + static_cast<u64>(layout.crop_y + y) * layout.pitch +
                    layout.crop_x, layout.width);
        }
        for (u32 y = 0; y < chroma_height; ++y) {
            const u8* u = source + y_bytes +
                static_cast<u64>(layout.crop_y / 2u + y) * chroma_pitch +
                (nv12 ? layout.crop_x : layout.crop_x / 2u);
            const u8* v = nv12 ? u + 1u : u + plane_bytes;
            u8* out_u = destination.data + pixels + static_cast<u64>(y) * chroma_width;
            u8* out_v = out_u + chroma_pixels;
            for (u32 x = 0; x < chroma_width; ++x) {
                out_u[x] = u[nv12 ? x * 2u : x];
                out_v[x] = v[nv12 ? x * 2u : x];
            }
        }
    }
    state->output_buffer->Unlock();
    return valid;
}

u32 mp4_direct_decoder_color_matrix(const Mp4DirectDecoder* decoder) noexcept {
    return decoder && decoder->state
               ? static_cast<u32>(static_cast<const DirectState*>(
                                      decoder->state)->matrix)
               : 0;
}

u32 mp4_direct_decoder_nominal_range(const Mp4DirectDecoder* decoder) noexcept {
    return decoder && decoder->state
               ? static_cast<u32>(static_cast<const DirectState*>(
                                      decoder->state)->range)
               : 0;
}

void mp4_direct_decoder_release(Mp4DirectDecoder* decoder) noexcept {
    if (!decoder) {
        return;
    }
    auto* state = static_cast<DirectState*>(decoder->state);
    if (state) {
        if (state->transform) {
            state->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING,
                                             0);
        }
        release_unknown(state->output_buffer);
        release_unknown(state->output_sample);
        release_unknown(state->gpu_frame.texture);
        release_unknown(state->gpu_sample);
        release_unknown(state->transform);
        release_unknown(state->device_manager);
        VirtualFree(state, 0, MEM_RELEASE);
    }
    if (decoder->rgba8) {
        VirtualFree(decoder->rgba8, 0, MEM_RELEASE);
    }
    *decoder = Mp4DirectDecoder{};
}

} // namespace maicord
