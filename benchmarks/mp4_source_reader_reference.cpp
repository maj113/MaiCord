#include "mp4_source_reader_reference.hpp"

#include "mp4_mft_decoder.hpp"

#include "maicord/core/memory.hpp"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlwapi.h>

namespace maicord {
namespace {

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

bool reserve_rgba8(Mp4ReferenceDecoder* decoder, u32 width, u32 height) noexcept {
    if (!decoder || width == 0 || height == 0 ||
        width > kMp4DecodeMaximumDimension ||
        height > kMp4DecodeMaximumDimension) {
        return false;
    }
    const u64 bytes = static_cast<u64>(width) * height * 4u;
    if (bytes > ~0u) {
        return false;
    }
    if (decoder->rgba8 && decoder->rgba8_capacity >= bytes) {
        decoder->width = width;
        decoder->height = height;
        decoder->row_pitch = width * 4u;
        return true;
    }
    const u32 capacity = static_cast<u32>((bytes + 65'535u) & ~65'535ull);
    void* allocation = VirtualAlloc(nullptr, capacity, MEM_RESERVE | MEM_COMMIT,
                                    PAGE_READWRITE);
    if (!allocation) {
        return false;
    }
    if (decoder->rgba8) {
        VirtualFree(decoder->rgba8, 0, MEM_RELEASE);
    }
    decoder->rgba8 = allocation;
    decoder->rgba8_capacity = capacity;
    decoder->width = width;
    decoder->height = height;
    decoder->row_pitch = width * 4u;
    return true;
}

bool configure_output(IMFSourceReader* reader, u16 requested_width,
                      u16 requested_height) noexcept {
    IMFMediaType* output = nullptr;
    HRESULT status = MFCreateMediaType(&output);
    if (SUCCEEDED(status)) {
        status = output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    }
    if (SUCCEEDED(status)) {
        status = output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    }
    if (SUCCEEDED(status) && requested_width != 0 && requested_height != 0) {
        status = MFSetAttributeSize(output, MF_MT_FRAME_SIZE, requested_width,
                                    requested_height);
    }
    if (SUCCEEDED(status)) {
        status = reader->SetCurrentMediaType(
                                             static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                             nullptr, output);
    }
    release_unknown(output);
    return SUCCEEDED(status);
}

bool read_output_shape(IMFSourceReader* reader, Mp4ReferenceDecoder* decoder) noexcept {
    IMFMediaType* current = nullptr;
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 numerator = 0;
    UINT32 denominator = 0;
    HRESULT status = reader->GetCurrentMediaType(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current);
    if (SUCCEEDED(status)) {
        status = MFGetAttributeSize(current, MF_MT_FRAME_SIZE, &width, &height);
    }
    if (SUCCEEDED(status) &&
        SUCCEEDED(MFGetAttributeRatio(current, MF_MT_FRAME_RATE, &numerator,
                                      &denominator)) &&
        numerator != 0 && denominator != 0) {
        decoder->fallback_duration_us =
            maximum<u64>(1, (static_cast<u64>(denominator) * 1'000'000ull +
                             numerator - 1u) /
                                numerator);
    }
    release_unknown(current);
    return SUCCEEDED(status) && reserve_rgba8(decoder, width, height);
}

bool rewind_reader(IMFSourceReader* reader) noexcept {
    PROPVARIANT position{};
    const GUID time_format{};
    position.vt = VT_I8;
    position.hVal.QuadPart = 0;
    return SUCCEEDED(reader->SetCurrentPosition(time_format, position));
}

bool copy_bgrx_frame(Mp4ReferenceDecoder* decoder, IMFMediaBuffer* buffer) noexcept {
    IMF2DBuffer* buffer_2d = nullptr;
    BYTE* source = nullptr;
    LONG pitch = 0;
    bool locked_2d = false;
    HRESULT status = buffer->QueryInterface(__uuidof(IMF2DBuffer),
                                            reinterpret_cast<void**>(&buffer_2d));
    if (SUCCEEDED(status) && buffer_2d) {
        status = buffer_2d->Lock2D(&source, &pitch);
        locked_2d = SUCCEEDED(status);
    } else {
        DWORD maximum = 0;
        DWORD current = 0;
        status = buffer->Lock(&source, &maximum, &current);
        pitch = static_cast<LONG>(decoder->width * 4u);
        if (SUCCEEDED(status) &&
            current < static_cast<u64>(pitch) * decoder->height) {
            status = E_UNEXPECTED;
        }
    }
    if (FAILED(status) || !source || pitch == 0 ||
        static_cast<u64>(pitch < 0 ? -static_cast<i64>(pitch) : pitch) <
            static_cast<u64>(decoder->width) * 4u) {
        if (locked_2d) {
            buffer_2d->Unlock2D();
        } else if (source) {
            buffer->Unlock();
        }
        release_unknown(buffer_2d);
        return false;
    }

    auto* destination = static_cast<u8*>(decoder->rgba8);
    for (u32 y = 0; y < decoder->height; ++y) {
        const BYTE* input = source + static_cast<ptrdiff_t>(y) * pitch;
        u8* output = destination + static_cast<u64>(y) * decoder->row_pitch;
        for (u32 x = 0; x < decoder->width; ++x) {
            output[x * 4u + 0u] = input[x * 4u + 2u];
            output[x * 4u + 1u] = input[x * 4u + 1u];
            output[x * 4u + 2u] = input[x * 4u + 0u];
            output[x * 4u + 3u] = 255u;
        }
    }
    if (locked_2d) {
        buffer_2d->Unlock2D();
    } else {
        buffer->Unlock();
    }
    release_unknown(buffer_2d);
    return true;
}

} // namespace

bool mp4_reference_thread_start() noexcept {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) {
        return false;
    }
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
        if (SUCCEEDED(initialized)) {
            CoUninitialize();
        }
        return false;
    }
    return true;
}

void mp4_reference_thread_stop() noexcept {
    MFShutdown();
    CoUninitialize();
}

bool mp4_reference_initialize(Mp4ReferenceDecoder* decoder, ConstSlice<u8> encoded,
                            u16 requested_width,
                            u16 requested_height,
                            ID3D11Device* video_device) noexcept {
    if (!decoder || !encoded.data || encoded.count == 0 ||
        ((requested_width == 0) != (requested_height == 0)) ||
        requested_width > kMp4DecodeMaximumDimension ||
        requested_height > kMp4DecodeMaximumDimension) {
        return false;
    }
    mp4_reference_release(decoder);
    auto* direct = static_cast<Mp4DirectDecoder*>(VirtualAlloc(
        nullptr, sizeof(Mp4DirectDecoder), MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE));
    if (direct && ((video_device && mp4_direct_decoder_initialize(direct, encoded, video_device)) ||
                   mp4_direct_decoder_initialize(direct, encoded))) {
        decoder->direct_decoder = direct;
        decoder->rgba8 = direct->rgba8;
        decoder->gpu = direct->gpu;
        decoder->rgba8_capacity = direct->rgba8_capacity;
        decoder->width = direct->width;
        decoder->height = direct->height;
        decoder->row_pitch = direct->row_pitch;
        decoder->ready = true;
        return true;
    }
    if (direct) {
        mp4_direct_decoder_release(direct);
        VirtualFree(direct, 0, MEM_RELEASE);
    }
    return mp4_reference_initialize_source_reader(
        decoder, encoded, requested_width, requested_height);
}

bool mp4_reference_initialize_source_reader(Mp4ReferenceDecoder* decoder,
                                          ConstSlice<u8> encoded,
                                          u16 requested_width,
                                          u16 requested_height) noexcept {
    if (!decoder || !encoded.data || encoded.count == 0 ||
        ((requested_width == 0) != (requested_height == 0)) ||
        requested_width > kMp4DecodeMaximumDimension ||
        requested_height > kMp4DecodeMaximumDimension) {
        return false;
    }
    mp4_reference_release(decoder);
    IStream* stream = SHCreateMemStream(encoded.data, encoded.count);
    IMFByteStream* byte_stream = nullptr;
    IMFAttributes* byte_stream_attributes = nullptr;
    IMFAttributes* attributes = nullptr;
    IMFSourceReader* reader = nullptr;
    HRESULT status = stream ? S_OK : E_OUTOFMEMORY;
    if (SUCCEEDED(status)) {
        status = MFCreateMFByteStreamOnStream(stream, &byte_stream);
    }
    // An in-memory stream has no URL extension for the byte-stream handler to
    // inspect. Chromium obtains the same information from the HTTP response;
    // preserve it explicitly here so Media Foundation selects the MPEG-4
    // handler instead of failing source-reader creation.
    if (SUCCEEDED(status)) {
        status = byte_stream->QueryInterface(
            __uuidof(IMFAttributes),
            reinterpret_cast<void**>(&byte_stream_attributes));
    }
    if (SUCCEEDED(status)) {
        status = byte_stream_attributes->SetString(MF_BYTESTREAM_CONTENT_TYPE,
                                                   L"video/mp4");
    }
    if (SUCCEEDED(status)) {
        status = byte_stream_attributes->SetString(MF_BYTESTREAM_ORIGIN_NAME,
                                                   L"memory.mp4");
    }
    if (SUCCEEDED(status)) {
        status = MFCreateAttributes(&attributes, 3);
    }
    if (SUCCEEDED(status)) {
        status = attributes->SetUINT32(
            MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    }
    if (SUCCEEDED(status)) {
        status = MFCreateSourceReaderFromByteStream(byte_stream, attributes,
                                                    &reader);
    }
    if (SUCCEEDED(status)) {
        status = reader->SetStreamSelection(
                                            static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS),
                                            FALSE);
    }
    if (SUCCEEDED(status)) {
        status = reader->SetStreamSelection(
                                            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
                                            TRUE);
    }
    if (SUCCEEDED(status) &&
        !configure_output(reader, requested_width, requested_height)) {
        status = E_FAIL;
    }
    if (SUCCEEDED(status)) {
        decoder->reader = reader;
        reader = nullptr;
        decoder->fallback_duration_us = 33'334;
        if (!read_output_shape(decoder->reader, decoder)) {
            status = E_FAIL;
        }
    }
    release_unknown(reader);
    release_unknown(attributes);
    release_unknown(byte_stream_attributes);
    release_unknown(byte_stream);
    release_unknown(stream);
    if (FAILED(status)) {
        mp4_reference_release(decoder);
        return false;
    }
    decoder->ready = true;
    return true;
}

Mp4DecodedFrame mp4_reference_read(Mp4ReferenceDecoder* decoder,
                                 bool loop_at_end) noexcept {
    if (!decoder || !decoder->ready || (!decoder->rgba8 && !decoder->gpu)) {
        return failure(Mp4DecodeError::invalid_input);
    }
    if (decoder->direct_decoder) {
        auto* direct = static_cast<Mp4DirectDecoder*>(
            decoder->direct_decoder);
        const Mp4DecodedFrame frame =
            mp4_direct_decoder_read(direct, loop_at_end);
        decoder->rgba8 = direct->rgba8;
        decoder->rgba8_capacity = direct->rgba8_capacity;
        decoder->width = direct->width;
        decoder->height = direct->height;
        decoder->row_pitch = direct->row_pitch;
        return frame;
    }
    if (!decoder->reader) {
        return failure(Mp4DecodeError::invalid_input);
    }
    bool rewound = false;
    for (u32 attempt = 0; attempt < 32; ++attempt) {
        DWORD stream_index = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        IMFSample* sample = nullptr;
        const HRESULT read = decoder->reader->ReadSample(
            static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
            &stream_index, &flags,
            &timestamp, &sample);
        if (FAILED(read)) {
            release_unknown(sample);
            return failure(Mp4DecodeError::sample);
        }
        if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0 &&
            !read_output_shape(decoder->reader, decoder)) {
            release_unknown(sample);
            return failure(Mp4DecodeError::dimensions);
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            release_unknown(sample);
            if (!loop_at_end) {
                return failure(Mp4DecodeError::end_of_stream);
            }
            if (rewound || !rewind_reader(decoder->reader)) {
                return failure(Mp4DecodeError::sample);
            }
            rewound = true;
            continue;
        }
        if (!sample) {
            continue;
        }
        IMFMediaBuffer* buffer = nullptr;
        const HRESULT contiguous = sample->ConvertToContiguousBuffer(&buffer);
        LONGLONG duration_100ns = 0;
        const HRESULT duration_status = sample->GetSampleDuration(&duration_100ns);
        const bool copied = SUCCEEDED(contiguous) && buffer &&
                            copy_bgrx_frame(decoder, buffer);
        release_unknown(buffer);
        release_unknown(sample);
        if (!copied) {
            return failure(Mp4DecodeError::buffer);
        }
        const u64 duration_us =
            SUCCEEDED(duration_status) && duration_100ns > 0
                ? maximum<u64>(1, static_cast<u64>(duration_100ns) / 10u)
                : decoder->fallback_duration_us;
        return Mp4DecodedFrame{
            ConstSlice<u8>{static_cast<const u8*>(decoder->rgba8),
                           decoder->row_pitch * decoder->height},
            duration_us, decoder->width, decoder->height, decoder->row_pitch,
            Mp4DecodeError::none};
    }
    return failure(Mp4DecodeError::sample);
}

void mp4_reference_release(Mp4ReferenceDecoder* decoder) noexcept {
    if (!decoder) {
        return;
    }
    if (decoder->direct_decoder) {
        auto* direct = static_cast<Mp4DirectDecoder*>(
            decoder->direct_decoder);
        mp4_direct_decoder_release(direct);
        VirtualFree(direct, 0, MEM_RELEASE);
        decoder->rgba8 = nullptr;
    }
    release_unknown(decoder->reader);
    if (decoder->rgba8) {
        VirtualFree(decoder->rgba8, 0, MEM_RELEASE);
    }
    *decoder = Mp4ReferenceDecoder{};
}

} // namespace maicord
