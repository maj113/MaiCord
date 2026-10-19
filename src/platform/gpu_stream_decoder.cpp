#include "gpu_stream_decoder_backend.hpp"
#include "maicord/media/video_display.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/initguid.hpp"
#include "maicord/sys/dxva.hpp"

namespace maicord {

struct GpuStreamDecoder : GpuStream {};

namespace {

constexpr HRESULT kDrawingBusy = static_cast<HRESULT>(0x8876021C); // D3DERR_WASSTILLDRAWING
constexpr u32 kBusyRetries = 200;                                  // ~200 ms of 1 ms waits.
constexpr u32 kGpuStreamSliceBatch = 256;
constexpr u32 kControlBytes = kGpuStreamSliceBatch * u32{sizeof(DXVA_Slice_HEVC_Short)};
static_assert(kAv1StreamTiles * sizeof(DXVA_Tile_AV1) <= kControlBytes);

bool busy(HRESULT status) noexcept {
    return status == E_PENDING || status == kDrawingBusy || status == DXGI_ERROR_WAS_STILL_DRAWING;
}

// Bitstream configuration (H.264 short slices 2, HEVC short slices 1, AV1 1) without encryption.
HRESULT select_config(ID3D11VideoDevice* video, const D3D11_VIDEO_DECODER_DESC& description,
                      u8 bitstream_raw, D3D11_VIDEO_DECODER_CONFIG* selected) noexcept {
    UINT count = 0;
    HRESULT status = video->GetVideoDecoderConfigCount(&description, &count);
    for (UINT i = 0; SUCCEEDED(status) && i < count; ++i) {
        D3D11_VIDEO_DECODER_CONFIG candidate{};
        status = video->GetVideoDecoderConfig(&description, i, &candidate);
        if (SUCCEEDED(status) && candidate.ConfigBitstreamRaw == bitstream_raw &&
            bytes_equal(&candidate.guidConfigBitstreamEncryption, &DXVA_NoEncrypt, sizeof(GUID))) {
            *selected = candidate;
            return S_OK;
        }
    }
    return FAILED(status) ? status : DXGI_ERROR_UNSUPPORTED;
}

struct Backend {
    const GUID* profile;
    u8 bitstream_raw;
    void (*reset)(GpuStream* stream) noexcept;
    void (*decode)(GpuStream* stream, ConstSlice<u8> bytes, u32 rtp_timestamp, bool intact,
                   bool resynchronize) noexcept;
    void (*drain)(GpuStream* stream) noexcept;
    void (*stats)(const GpuStream& stream, GpuStreamDecoderStats* stats) noexcept;
};
// Indexed by GpuStreamCodec.
constexpr Backend kBackends[]{
    {&D3D11_DECODER_PROFILE_H264_VLD_NOFGT, 2, h264_gpu_stream_reset, h264_gpu_stream_decode,
     h264_gpu_stream_drain, h264_gpu_stream_stats},
    {&D3D11_DECODER_PROFILE_HEVC_VLD_MAIN, 1, hevc_gpu_stream_reset, hevc_gpu_stream_decode,
     hevc_gpu_stream_drain, hevc_gpu_stream_stats},
    {&D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0, 1, av1_gpu_stream_reset, av1_gpu_stream_decode,
     av1_gpu_stream_drain, av1_gpu_stream_stats}};

const Backend& backend(const GpuStream& stream) noexcept {
    return kBackends[static_cast<u8>(stream.options.codec)];
}

void release_buffer(GpuStreamInput* input) noexcept {
    if (input->data)
        VirtualFree(input->data, 0, MEM_RELEASE);
    *input = {};
}

bool reserve(GpuStreamInput* input, u32 bytes) noexcept {
    if (input->capacity >= bytes)
        return true;
    const u32 capacity = (bytes + 0xFFFFu) & ~0xFFFFu;
    auto* data =
        static_cast<u8*>(VirtualAlloc(nullptr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!data)
        return false;
    if (input->data)
        VirtualFree(input->data, 0, MEM_RELEASE);
    input->data = data;
    input->capacity = capacity;
    return true;
}

// Frees the decoder and surfaces; the stream then waits for a keyframe
// (parameter sets are kept) and recreates them at its geometry.
bool release_idle(GpuStream* stream) noexcept {
    if (!stream->decoder && !stream->surfaces.texture)
        return false;
    backend(*stream).drain(stream); // Nothing decoded may be lost silently.
    gpu_stream_release_decoder(stream);
    backend(*stream).reset(stream);
    stream->last_request_ms = 0; // The first picture after release asks at once.
    saturating_increment(stream->stats.idle_releases);
    return true;
}

void publish_stats(GpuStream* stream) noexcept {
    auto& stats = stream->stats;
    backend(*stream).stats(*stream, &stats);
    stats.coded_width = stream->coded_width;
    stats.coded_height = stream->coded_height;
    stats.surfaces = stream->surfaces.count;
}

DWORD WINAPI decode_main(void* parameter) {
    auto* stream = static_cast<GpuStream*>(parameter);
    for (;;) {
        // Sleep indefinitely without DXVA objects; otherwise wake once when
        // the stream has been input-idle long enough to release them.
        const DWORD wait = stream->decoder ? kGpuStreamIdleReleaseMs : INFINITE;
        if (WaitForSingleObject(stream->wake, wait) == WAIT_TIMEOUT) {
            AcquireSRWLockExclusive(&stream->lock);
            const bool idle = !stream->count && !stream->stop;
            ReleaseSRWLockExclusive(&stream->lock);
            if (idle && release_idle(stream)) {
                AcquireSRWLockExclusive(&stream->lock);
                publish_stats(stream);
                ReleaseSRWLockExclusive(&stream->lock);
            }
            continue;
        }
        for (;;) {
            AcquireSRWLockExclusive(&stream->lock);
            if (stream->stop) {
                ReleaseSRWLockExclusive(&stream->lock);
                return 0;
            }
            if (!stream->count) {
                ReleaseSRWLockExclusive(&stream->lock);
                break;
            }
            // Swap buffers so the producer reuses the previous work buffer.
            GpuStreamInput& slot = stream->queue[stream->head];
            const GpuStreamInput taken = slot;
            slot.data = stream->work.data;
            slot.capacity = stream->work.capacity;
            slot.bytes = 0;
            stream->work = taken;
            --stream->count;
            // An empty queue reuses the spare in this slot on the next push.
            if (stream->count)
                stream->head = (stream->head + 1u) % kGpuStreamQueueFrames;
            ReleaseSRWLockExclusive(&stream->lock);
            backend(*stream).decode(stream, {stream->work.data, stream->work.bytes},
                                    stream->work.rtp_timestamp, stream->work.intact,
                                    stream->work.resynchronize);
            AcquireSRWLockExclusive(&stream->lock);
            publish_stats(stream);
            ReleaseSRWLockExclusive(&stream->lock);
        }
    }
}

} // namespace

HRESULT gpu_stream_create_decoder(GpuStream* stream, const GpuStreamDecoderDesc& desc) noexcept {
    gpu_stream_release_decoder(stream);
    HRESULT status = S_OK;
    if (!stream->video)
        status = stream->device->QueryInterface(__uuidof(ID3D11VideoDevice),
                                                reinterpret_cast<void**>(&stream->video));
    if (SUCCEEDED(status) && !stream->context)
        status = stream->immediate->QueryInterface(__uuidof(ID3D11VideoContext),
                                                   reinterpret_cast<void**>(&stream->context));
    if (SUCCEEDED(status) && !stream->guard)
        status = stream->immediate->QueryInterface(__uuidof(ID3D11Multithread),
                                                   reinterpret_cast<void**>(&stream->guard));
    D3D11_VIDEO_DECODER_DESC description{*desc.profile, desc.width, desc.height, DXGI_FORMAT_NV12};
    if (SUCCEEDED(status)) {
        BOOL supported = FALSE;
        status = stream->video->CheckVideoDecoderFormat(desc.profile, DXGI_FORMAT_NV12, &supported);
        if (SUCCEEDED(status) && !supported)
            status = DXGI_ERROR_UNSUPPORTED;
    }
    if (SUCCEEDED(status)) {
        // Resolution/level support (1440p, 4K) differs per GPU; ask the driver
        // before allocating a large DPB when the runtime offers the query.
        ID3D11VideoDevice1* video1 = nullptr;
        if (SUCCEEDED(stream->video->QueryInterface(__uuidof(ID3D11VideoDevice1),
                                                    reinterpret_cast<void**>(&video1)))) {
            DXGI_RATIONAL rate = desc.rate;
            UINT caps = 0;
            if (SUCCEEDED(video1->GetVideoDecoderCaps(desc.profile, desc.width, desc.height, &rate, 0,
                                                      nullptr, &caps)) &&
                (caps & D3D11_VIDEO_DECODER_CAPS_UNSUPPORTED))
                status = DXGI_ERROR_UNSUPPORTED;
            video1->Release();
        }
    }
    D3D11_VIDEO_DECODER_CONFIG selected{};
    if (SUCCEEDED(status))
        status = select_config(stream->video, description, desc.bitstream_raw, &selected);
    if (SUCCEEDED(status))
        status = stream->video->CreateVideoDecoder(&description, &selected, &stream->decoder);
    if (SUCCEEDED(status)) {
        const u32 count = maximum<u32>(selected.ConfigMinRenderTargetBuffCount, desc.surfaces);
        status =
            gpu_decoder_surfaces_create(&stream->surfaces, stream->device, stream->video, *desc.profile,
                                        desc.surface_width, desc.surface_height, count, count);
    }
    if (FAILED(status)) {
        gpu_stream_release_decoder(stream);
        return status;
    }
    stream->coded_width = desc.width;
    stream->coded_height = desc.height;
    return S_OK;
}

HRESULT gpu_stream_submit(GpuStream* stream, const GpuStreamSubmission& submission) noexcept {
    const u8 surface = submission.surface;
    if (surface >= stream->surfaces.count || !stream->surfaces.views[surface] || !submission.units ||
        submission.entry_bytes > kControlBytes)
        return E_INVALIDARG;
    HRESULT status = S_OK;
    for (u32 attempt = 0;; ++attempt) {
        stream->guard->Enter();
        status = stream->context->DecoderBeginFrame(stream->decoder, stream->surfaces.views[surface], 0,
                                                    nullptr);
        if (!busy(status) || attempt == kBusyRetries)
            break;
        stream->guard->Leave();
        Sleep(1);
    }
    if (FAILED(status)) {
        stream->guard->Leave();
        return status;
    }
    u64 uploaded = 0;
    const auto upload = [&](D3D11_VIDEO_DECODER_BUFFER_TYPE type, ConstSlice<u8> data,
                            D3D11_VIDEO_DECODER_BUFFER_DESC* description) noexcept {
        UINT capacity = 0;
        void* destination = nullptr;
        HRESULT result = stream->context->GetDecoderBuffer(stream->decoder, type, &capacity, &destination);
        if (FAILED(result))
            return result;
        const bool fits = destination && data.count <= capacity;
        if (fits)
            copy_bytes(destination, data.data, data.count);
        result = stream->context->ReleaseDecoderBuffer(stream->decoder, type);
        if (!fits)
            return E_OUTOFMEMORY;
        *description = {};
        description->BufferType = type;
        description->DataSize = data.count;
        uploaded += data.count;
        return result;
    };
    if (!stream->slice_entries) {
        // The control buffer capacity bounds each batch; drivers size it per decoder.
        UINT capacity = 0;
        void* mapped = nullptr;
        status = stream->context->GetDecoderBuffer(
            stream->decoder, D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, &capacity, &mapped);
        if (SUCCEEDED(status))
            status = stream->context->ReleaseDecoderBuffer(stream->decoder,
                                                           D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL);
        stream->slice_entries = static_cast<u16>(
            minimum<u32>(minimum<u32>(capacity, kControlBytes) / submission.entry_bytes, submission.entry_limit));
        if (SUCCEEDED(status) && !stream->slice_entries)
            status = E_OUTOFMEMORY;
    }
    u32 next = 0;
    while (SUCCEEDED(status) && next < submission.units) {
        D3D11_VIDEO_DECODER_BUFFER_DESC buffers[4]{};
        u32 count = 0;
        status = upload(D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS, submission.parameters,
                        &buffers[count++]);
        if (SUCCEEDED(status) && submission.matrix.count)
            status = upload(D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, submission.matrix,
                            &buffers[count++]);
        if (FAILED(status))
            break;
        UINT bit_capacity = 0;
        void* bit_data = nullptr;
        status = stream->context->GetDecoderBuffer(stream->decoder, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM,
                                                   &bit_capacity, &bit_data);
        if (FAILED(status))
            break;
        // Control entries are built here, then copied after the bitstream
        // buffer is released: drivers may share upload memory between the two.
        alignas(8) u8 entries[kControlBytes];
        u32 bytes = 0;
        const u32 batch =
            bit_data ? submission.fill(submission.source, next, static_cast<u8*>(bit_data), bit_capacity,
                                       entries, minimum<u32>(stream->slice_entries, submission.units - next),
                                       &bytes)
                     : 0;
        const HRESULT released =
            stream->context->ReleaseDecoderBuffer(stream->decoder, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM);
        status = !batch ? E_OUTOFMEMORY : released; // One unit exceeds the driver buffer.
        if (FAILED(status))
            break;
        buffers[count].BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
        buffers[count++].DataSize = bytes;
        uploaded += bytes;
        status = upload(D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL, {entries, batch * submission.entry_bytes},
                        &buffers[count++]);
        if (SUCCEEDED(status))
            status = stream->context->SubmitDecoderBuffers(stream->decoder, count, buffers);
        next += batch;
    }
    const HRESULT ended = stream->context->DecoderEndFrame(stream->decoder);
    stream->guard->Leave();
    stream->stats.uploaded_bytes += uploaded;
    if (++stream->feedback == 0)
        ++stream->feedback;
    return FAILED(status) ? status : ended;
}

HRESULT gpu_stream_submit_slices(GpuStream* stream, u8 surface, ConstSlice<u8> parameters,
                                 ConstSlice<u8> matrix, ConstSlice<u8> source,
                                 const H264StreamNalRange* slices, u32 slice_count) noexcept {
    for (u32 i = 0; i < slice_count; ++i) {
        const auto range = slices[i];
        if (!range.size || range.offset > source.count || range.size > source.count - range.offset)
            return E_INVALIDARG;
    }
    struct Source {
        const u8* data;
        const H264StreamNalRange* slices;
    } const context{source.data, slices};
    // Each slice behind a start code; a zero tail pads the batch to 128 bytes (DXVA HEVC).
    const auto fill = [](const void* opaque, u32 first, u8* bytes, u32 capacity, void* control, u32 limit,
                         u32* written) noexcept -> u32 {
        const auto& from = *static_cast<const Source*>(opaque);
        auto* entries = static_cast<DXVA_Slice_HEVC_Short*>(control);
        u32 offset = 0, batch = 0;
        for (; batch < limit; ++batch) {
            const auto range = from.slices[first + batch];
            const u64 end = u64{offset} + 3u + range.size;
            if (end + 1u > capacity) // Room for at least one tail byte.
                break;
            entries[batch] = {offset, range.size + 3u, 0};
            bytes[offset] = 0;
            bytes[offset + 1u] = 0;
            bytes[offset + 2u] = 1;
            copy_bytes(bytes + offset + 3u, from.data + range.offset, range.size);
            offset = static_cast<u32>(end);
        }
        if (batch) {
            const u32 aligned = static_cast<u32>(minimum<u64>((u64{offset} + 128u) & ~127ull, capacity));
            clear_bytes(bytes + offset, aligned - offset);
            entries[batch - 1u].SliceBytesInBuffer += aligned - offset;
            offset = aligned;
        }
        *written = offset;
        return batch;
    };
    static_assert(sizeof(DXVA_Slice_H264_Short) == sizeof(DXVA_Slice_HEVC_Short));
    return gpu_stream_submit(stream, {surface, parameters, matrix, slice_count,
                                      u32{sizeof(DXVA_Slice_HEVC_Short)}, kGpuStreamSliceBatch, &context, fill});
}

void gpu_stream_set_format(GpuStream* stream, u32 crop_x, u32 crop_y, u32 width, u32 height, u8 matrix,
                           bool full_range, u32 sar_width, u32 sar_height) noexcept {
    // Unspecified matrix: HD camera/screen encoders use BT.709, SD BT.601.
    const bool bt709 = matrix == 1 || (matrix == 2 && height >= 720);
    stream->frame = {stream->surfaces.texture, 0, crop_x, crop_y, width, height, bt709, full_range, 0, {}};
    const auto extent = video_display_extent(width, height, sar_width, sar_height);
    stream->display_width = extent.width;
    stream->display_height = extent.height;
}

void gpu_stream_present(GpuStream* stream, u8 surface, u32 rtp_timestamp, bool keyframe) noexcept {
    if (!stream->surfaces.texture || surface >= stream->surfaces.count)
        return;
    stream->frame.texture = stream->surfaces.texture;
    stream->frame.subresource = surface;
    stream->frame.luma = stream->surfaces.luma[surface];
    stream->frame.chroma = stream->surfaces.chroma[surface];
    stream->presented = surface;
    saturating_increment(stream->stats.presented);
    if (stream->options.sink.frame) {
        const GpuStreamFrame frame{&stream->frame, rtp_timestamp, stream->display_width,
                                   stream->display_height, keyframe};
        stream->options.sink.frame(stream->options.sink.context, stream->options.ssrc, frame);
    }
}

u32 gpu_stream_held(const GpuStream& stream) noexcept {
    const u32 hold =
        static_cast<u32>(InterlockedCompareExchange(const_cast<volatile LONG*>(&stream.hold), 0, 0));
    u32 mask = stream.presented < 32 ? 1u << stream.presented : 0u;
    if (hold && hold <= 32)
        mask |= 1u << (hold - 1u);
    return mask;
}

void gpu_stream_request_keyframe(GpuStream* stream, bool unsupported) noexcept {
    if (unsupported || stream->stats.device_lost)
        return;
    const u64 now = GetTickCount64();
    if (stream->last_request_ms && now - stream->last_request_ms < kGpuStreamKeyframeIntervalMs)
        return;
    stream->last_request_ms = now ? now : 1;
    saturating_increment(stream->stats.keyframe_requests);
    if (stream->options.sink.request_keyframe)
        stream->options.sink.request_keyframe(stream->options.sink.context, stream->options.ssrc);
}

bool gpu_stream_failure(GpuStream* stream, HRESULT status) noexcept {
    saturating_increment(stream->stats.gpu_failures);
    stream->stats.last_status = status;
    if (status == DXGI_ERROR_DEVICE_REMOVED || status == DXGI_ERROR_DEVICE_RESET ||
        (stream->device && stream->device->GetDeviceRemovedReason() != S_OK))
        stream->stats.device_lost = true;
    return stream->stats.device_lost;
}

void gpu_stream_release_decoder(GpuStream* stream) noexcept {
    if (stream->guard)
        stream->guard->Enter();
    gpu_decoder_surfaces_release(&stream->surfaces);
    com_release(stream->decoder);
    if (stream->guard)
        stream->guard->Leave();
    stream->frame = {};
    stream->presented = 255;
    stream->slice_entries = 0;
    stream->coded_width = stream->coded_height = 0;
}

u32 gpu_stream_decoder_codecs(ID3D11Device* device) noexcept {
    ID3D11VideoDevice* video = nullptr;
    if (!gpu_decoder_surfaces_supported(device) ||
        FAILED(device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(&video))))
        return 0;
    u32 codecs = 0;
    for (u32 codec = 0; codec < sizeof(kBackends) / sizeof(kBackends[0]); ++codec) {
        const Backend& entry = kBackends[codec];
        BOOL supported = FALSE;
        D3D11_VIDEO_DECODER_CONFIG config{};
        if (SUCCEEDED(video->CheckVideoDecoderFormat(entry.profile, DXGI_FORMAT_NV12, &supported)) &&
            supported &&
            SUCCEEDED(select_config(video, {*entry.profile, 1920, 1080, DXGI_FORMAT_NV12},
                                    entry.bitstream_raw, &config)))
            codecs |= 1u << codec;
    }
    video->Release();
    return codecs;
}

GpuStreamDecoder* gpu_stream_decoder_create(const GpuStreamDecoderOptions& options) noexcept {
    if (!options.device || options.codec > GpuStreamCodec::av1)
        return nullptr;
    auto* stream = static_cast<GpuStreamDecoder*>(
        VirtualAlloc(nullptr, sizeof(GpuStreamDecoder), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!stream)
        return nullptr;
    stream->options = options;
    stream->presented = 255;
    backend(*stream).reset(stream);
    InitializeSRWLock(&stream->lock);
    stream->device = options.device;
    options.device->AddRef();
    options.device->GetImmediateContext(&stream->immediate);
    if (!stream->immediate) {
        gpu_stream_decoder_destroy(stream);
        return nullptr;
    }
    if (!options.synchronous) {
        stream->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (stream->wake)
            stream->thread =
                CreateThread(nullptr, 256u * 1024u, decode_main, static_cast<GpuStream*>(stream),
                             STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!stream->thread) {
            gpu_stream_decoder_destroy(stream);
            return nullptr;
        }
    }
    return stream;
}

bool gpu_stream_decoder_push(GpuStreamDecoder* stream, ConstSlice<u8> annexb, u32 rtp_timestamp,
                             bool intact) noexcept {
    // Bound before copying: an oversized frame is dropped like a lost one.
    if (!stream || !annexb.data || !annexb.count || annexb.count > kGpuStreamMaximumFrameBytes)
        return false;
    if (stream->options.synchronous) {
        saturating_increment(stream->stats.received);
        const bool resynchronize = stream->resynchronize_pending;
        stream->resynchronize_pending = false;
        backend(*stream).decode(stream, annexb, rtp_timestamp, intact, resynchronize);
        publish_stats(stream);
        return true;
    }
    AcquireSRWLockExclusive(&stream->lock);
    saturating_increment(stream->stats.received);
    bool resynchronize = stream->resynchronize_pending;
    if (stream->count == kGpuStreamQueueFrames) {
        // The decoder fell behind: queued latency is worse than a freeze.
        stream->count = 0;
        saturating_increment(stream->stats.overflows);
        resynchronize = true;
    }
    GpuStreamInput& slot = stream->queue[(stream->head + stream->count) % kGpuStreamQueueFrames];
    if (!reserve(&slot, annexb.count)) {
        stream->resynchronize_pending = true;
        ReleaseSRWLockExclusive(&stream->lock);
        return false;
    }
    copy_bytes(slot.data, annexb.data, annexb.count);
    slot.bytes = annexb.count;
    slot.rtp_timestamp = rtp_timestamp;
    slot.intact = intact;
    slot.resynchronize = resynchronize;
    stream->resynchronize_pending = false;
    ++stream->count;
    ReleaseSRWLockExclusive(&stream->lock);
    SetEvent(stream->wake);
    return true;
}

void gpu_stream_decoder_hold(GpuStreamDecoder* stream, u32 subresource) noexcept {
    if (stream && subresource < 32)
        InterlockedExchange(&stream->hold, static_cast<LONG>(subresource + 1u));
}

bool gpu_stream_decoder_release_idle(GpuStreamDecoder* stream) noexcept {
    if (!stream || !stream->options.synchronous || !release_idle(stream))
        return false;
    publish_stats(stream);
    return true;
}

GpuStreamDecoderStats gpu_stream_decoder_stats(const GpuStreamDecoder* stream) noexcept {
    if (!stream)
        return {};
    auto* lock = const_cast<SRWLOCK*>(&stream->lock);
    AcquireSRWLockShared(lock);
    const GpuStreamDecoderStats stats = stream->stats;
    ReleaseSRWLockShared(lock);
    return stats;
}

void gpu_stream_decoder_destroy(GpuStreamDecoder* stream) noexcept {
    if (!stream)
        return;
    if (stream->thread) {
        AcquireSRWLockExclusive(&stream->lock);
        stream->stop = true;
        ReleaseSRWLockExclusive(&stream->lock);
        SetEvent(stream->wake);
        WaitForSingleObject(stream->thread, INFINITE);
        CloseHandle(stream->thread);
    }
    if (stream->wake)
        CloseHandle(stream->wake);
    gpu_stream_release_decoder(stream);
    com_release(stream->guard);
    com_release(stream->context);
    com_release(stream->video);
    com_release(stream->immediate);
    com_release(stream->device);
    for (auto& input : stream->queue)
        release_buffer(&input);
    release_buffer(&stream->work);
    VirtualFree(stream, 0, MEM_RELEASE);
}

} // namespace maicord
