#include "mp4_mf_gpu_reference.hpp"
#include "maicord/core/memory.hpp"
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>
#include <d3d11.h>

namespace maicord {
namespace {
template<class T> void release(T*& value) noexcept {
    if (value) { value->Release(); value = nullptr; }
}
constexpr DWORD video_stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
bool read_format(Mp4MfGpuReference* r) noexcept {
    IMFMediaType* type = nullptr;
    HRESULT status = r->reader->GetCurrentMediaType(video_stream, &type);
    UINT32 width = 0, height = 0;
    GUID subtype{};
    if (SUCCEEDED(status)) status = type->GetGUID(MF_MT_SUBTYPE, &subtype);
    if (SUCCEEDED(status) && !bytes_equal(&subtype, &MFVideoFormat_NV12, sizeof(GUID))) status = MF_E_INVALIDMEDIATYPE;
    if (SUCCEEDED(status)) status = MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width, &height);
    MFVideoArea area{};
    UINT32 size = 0;
    bool aperture = type && SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
        reinterpret_cast<BYTE*>(&area), sizeof(area), &size));
    if (!aperture && type) aperture = SUCCEEDED(type->GetBlob(MF_MT_GEOMETRIC_APERTURE,
        reinterpret_cast<BYTE*>(&area), sizeof(area), &size));
    u32 x = 0, y = 0;
    if (aperture) {
        if (size != sizeof(area) || area.OffsetX.fract || area.OffsetY.fract ||
            area.OffsetX.value < 0 || area.OffsetY.value < 0 ||
            area.Area.cx <= 0 || area.Area.cy <= 0 ||
            static_cast<u32>(area.OffsetX.value) + static_cast<u32>(area.Area.cx) > width ||
            static_cast<u32>(area.OffsetY.value) + static_cast<u32>(area.Area.cy) > height) {
            status = MF_E_INVALIDMEDIATYPE;
        } else {
            x = static_cast<u32>(area.OffsetX.value); y = static_cast<u32>(area.OffsetY.value);
            width = static_cast<u32>(area.Area.cx); height = static_cast<u32>(area.Area.cy);
        }
    }
    UINT32 matrix = MFVideoTransferMatrix_Unknown, range = MFNominalRange_Unknown;
    if (type) {
        type->GetUINT32(MF_MT_YUV_MATRIX, &matrix);
        type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range);
    }
    release(type);
    if (!width || !height || width > 2048 || height > 2048 || ((x | y) & 1u)) status = MF_E_INVALIDMEDIATYPE;
    r->status = status;
    if (FAILED(status)) return false;
    r->frame.crop_x = x; r->frame.crop_y = y;
    r->frame.width = width; r->frame.height = height;
    r->frame.bt709 = matrix == MFVideoTransferMatrix_BT709;
    r->frame.full_range = range == MFNominalRange_0_255;
    return true;
}
} // namespace

bool mp4_mf_gpu_initialize(Mp4MfGpuReference* r, ConstSlice<u8> encoded,
                           ID3D11Device* device) noexcept {
    if (!r || !encoded.data || !encoded.count || !device) return false;
    mp4_mf_gpu_release(r);
    IStream* stream = SHCreateMemStream(encoded.data, encoded.count);
    IMFByteStream* bytes = nullptr;
    IMFAttributes* attributes = nullptr;
    IMFAttributes* stream_attributes = nullptr;
    IMFDXGIDeviceManager* manager = nullptr;
    IMFMediaType* type = nullptr;
    UINT token = 0;
    HRESULT status = stream ? MFCreateMFByteStreamOnStream(stream, &bytes) : E_OUTOFMEMORY;
    if (SUCCEEDED(status)) status = bytes->QueryInterface(__uuidof(IMFAttributes), reinterpret_cast<void**>(&stream_attributes));
    if (SUCCEEDED(status)) status = stream_attributes->SetString(MF_BYTESTREAM_CONTENT_TYPE, L"video/mp4");
    if (SUCCEEDED(status)) status = stream_attributes->SetString(MF_BYTESTREAM_ORIGIN_NAME, L"memory.mp4");
    if (SUCCEEDED(status)) status = MFCreateDXGIDeviceManager(&token, &manager);
    if (SUCCEEDED(status)) status = manager->ResetDevice(device, token);
    if (SUCCEEDED(status)) status = MFCreateAttributes(&attributes, 2);
    if (SUCCEEDED(status)) status = attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, manager);
    if (SUCCEEDED(status)) status = attributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);
    if (SUCCEEDED(status)) status = MFCreateSourceReaderFromByteStream(bytes, attributes, &r->reader);
    if (SUCCEEDED(status)) status = r->reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    if (SUCCEEDED(status)) status = r->reader->SetStreamSelection(video_stream, TRUE);
    if (SUCCEEDED(status)) status = MFCreateMediaType(&type);
    if (SUCCEEDED(status)) status = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(status)) status = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (SUCCEEDED(status)) status = r->reader->SetCurrentMediaType(video_stream, nullptr, type);
    release(type); release(manager); release(attributes); release(stream_attributes); release(bytes); release(stream);
    r->status = status;
    return SUCCEEDED(status) && read_format(r);
}

Mp4DecodedFrame mp4_mf_gpu_read(Mp4MfGpuReference* r, bool loop) noexcept {
    Mp4DecodedFrame result{};
    result.error = Mp4DecodeError::sample;
    if (!r || !r->reader) return result;
    release(r->frame.texture); release(r->sample);
    for (u32 attempt = 0; attempt < 64; ++attempt) {
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        r->status = r->reader->ReadSample(video_stream, 0, nullptr, &flags, &timestamp, &r->sample);
        if (FAILED(r->status)) return result;
        if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) && !read_format(r)) return result;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            release(r->sample);
            if (!loop) { result.error = Mp4DecodeError::end_of_stream; return result; }
            PROPVARIANT position{};
            position.vt = VT_I8;
            r->status = r->reader->SetCurrentPosition(GUID{}, position);
            if (FAILED(r->status)) return result;
            loop = false;
            continue;
        }
        if (!r->sample) continue;
        IMFMediaBuffer* buffer = nullptr;
        IMFDXGIBuffer* dxgi = nullptr;
        r->status = r->sample->GetBufferByIndex(0, &buffer);
        if (SUCCEEDED(r->status)) r->status = buffer->QueryInterface(__uuidof(IMFDXGIBuffer), reinterpret_cast<void**>(&dxgi));
        if (SUCCEEDED(r->status)) r->status = dxgi->GetResource(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&r->frame.texture));
        if (SUCCEEDED(r->status)) r->status = dxgi->GetSubresourceIndex(&r->frame.subresource);
        release(dxgi); release(buffer);
        if (FAILED(r->status)) return result;
        LONGLONG duration = 0;
        r->sample->GetSampleDuration(&duration);
        result.width = r->frame.width; result.height = r->frame.height;
        result.duration_us = duration > 0 ? static_cast<u64>(duration) / 10 : 0;
        result.gpu = &r->frame;
        result.error = Mp4DecodeError::none;
        return result;
    }
    r->status = E_UNEXPECTED;
    return result;
}

void mp4_mf_gpu_release(Mp4MfGpuReference* r) noexcept {
    if (!r) return;
    release(r->frame.texture); release(r->sample); release(r->reader);
    *r = Mp4MfGpuReference{};
}

IMFTransform* mp4_mf_gpu_transform(Mp4MfGpuReference* r) noexcept {
    IMFSourceReaderEx* reader = nullptr;
    IMFTransform* transform = nullptr;
    GUID category{};
    if (r && r->reader && SUCCEEDED(r->reader->QueryInterface(__uuidof(IMFSourceReaderEx),
        reinterpret_cast<void**>(&reader)))) {
        reader->GetTransformForStream(video_stream, 0, &category, &transform);
        reader->Release();
    }
    return transform;
}
} // namespace maicord
