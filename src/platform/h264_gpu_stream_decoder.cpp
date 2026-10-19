#include "maicord/platform/h264_gpu_stream_decoder.hpp"
#include "maicord/media/h264_stream.hpp"
#include "maicord/media/video_display.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/initguid.hpp"
#include "maicord/sys/dxva.hpp"
#include "h264_dxva_parameters.hpp"

namespace maicord {
namespace {

struct Input {
    u8* data;
    u32 bytes, capacity, rtp_timestamp;
    bool intact, resynchronize;
};

struct Gpu {
    ID3D11Device* device;
    ID3D11DeviceContext* immediate;
    ID3D11VideoDevice* video;
    ID3D11VideoContext* context;
    ID3D11Multithread* guard;
    ID3D11VideoDecoder* decoder;
    ID3D11Texture2D* surfaces;
    ID3D11VideoDecoderOutputView* views[32];
    ID3D11Texture2D* display;
    ID3D11ShaderResourceView* luma;
    ID3D11ShaderResourceView* chroma;
    u32 coded_width, coded_height, feedback;
    u8 surface_count;
};

constexpr HRESULT kDrawingBusy = static_cast<HRESULT>(0x8876021C); // D3DERR_WASSTILLDRAWING
constexpr u32 kBusyRetries = 200;                                     // ~200 ms of 1 ms waits.

bool busy(HRESULT status) noexcept {
    return status == E_PENDING || status == kDrawingBusy || status == DXGI_ERROR_WAS_STILL_DRAWING;
}

} // namespace

struct H264StreamDecoder {
    H264StreamState state;
    H264StreamPicture picture;
    Gpu gpu;
    GpuVideoFrame frame;
    H264StreamDecoderOptions options;
    H264StreamDecoderStats stats;
    u16 display_width, display_height;
    SRWLOCK lock;
    HANDLE wake, thread;
    Input queue[kH264StreamQueueFrames];
    Input work;
    u32 head, count;
    u64 last_request_ms;
    bool stop, resynchronize_pending;
};

namespace {

void release_buffer(Input* input) noexcept {
    if (input->data) VirtualFree(input->data, 0, MEM_RELEASE);
    *input = {};
}

bool reserve(Input* input, u32 bytes) noexcept {
    if (input->capacity >= bytes) return true;
    const u32 capacity = (bytes + 0xFFFFu) & ~0xFFFFu;
    auto* data = static_cast<u8*>(VirtualAlloc(nullptr, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!data) return false;
    if (input->data) VirtualFree(input->data, 0, MEM_RELEASE);
    input->data = data;
    input->capacity = capacity;
    return true;
}

void release_decoder_objects(Gpu* gpu) noexcept {
    for (auto*& view : gpu->views) com_release(view);
    com_release(gpu->surfaces);
    com_release(gpu->decoder);
    com_release(gpu->chroma);
    com_release(gpu->luma);
    com_release(gpu->display);
    gpu->surface_count = 0;
    gpu->coded_width = gpu->coded_height = 0;
}

void release_gpu(Gpu* gpu) noexcept {
    release_decoder_objects(gpu);
    com_release(gpu->guard);
    com_release(gpu->context);
    com_release(gpu->video);
    com_release(gpu->immediate);
    com_release(gpu->device);
}

HRESULT create_decoder(Gpu* gpu, const H264Sps& sps, u8 surfaces_needed) noexcept {
    release_decoder_objects(gpu);
    HRESULT status = S_OK;
    if (!gpu->video) status = gpu->device->QueryInterface(__uuidof(ID3D11VideoDevice),
                                                          reinterpret_cast<void**>(&gpu->video));
    if (SUCCEEDED(status) && !gpu->context)
        status = gpu->immediate->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void**>(&gpu->context));
    if (SUCCEEDED(status) && !gpu->guard)
        status = gpu->immediate->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&gpu->guard));
    D3D11_VIDEO_DECODER_DESC description{D3D11_DECODER_PROFILE_H264_VLD_NOFGT, sps.coded_width, sps.coded_height,
                                         DXGI_FORMAT_NV12};
    if (SUCCEEDED(status)) {
        BOOL supported = FALSE;
        status = gpu->video->CheckVideoDecoderFormat(&description.Guid, DXGI_FORMAT_NV12, &supported);
        if (SUCCEEDED(status) && !supported) status = DXGI_ERROR_UNSUPPORTED;
    }
    if (SUCCEEDED(status)) {
        // Resolution/level support (1440p, 4K) differs per GPU; ask the driver
        // before allocating a large DPB when the runtime offers the query.
        ID3D11VideoDevice1* video1 = nullptr;
        if (SUCCEEDED(gpu->video->QueryInterface(__uuidof(ID3D11VideoDevice1), reinterpret_cast<void**>(&video1)))) {
            DXGI_RATIONAL rate{60, 1};
            if (sps.time_scale && sps.units_in_tick && sps.units_in_tick < 0x80000000u)
                rate = {sps.time_scale, sps.units_in_tick * 2u};
            UINT caps = 0;
            if (SUCCEEDED(video1->GetVideoDecoderCaps(&description.Guid, sps.coded_width, sps.coded_height, &rate, 0,
                                                      nullptr, &caps)) &&
                (caps & D3D11_VIDEO_DECODER_CAPS_UNSUPPORTED))
                status = DXGI_ERROR_UNSUPPORTED;
            video1->Release();
        }
    }
    D3D11_VIDEO_DECODER_CONFIG selected{};
    if (SUCCEEDED(status)) {
        UINT count = 0;
        status = gpu->video->GetVideoDecoderConfigCount(&description, &count);
        bool found = false;
        for (UINT i = 0; SUCCEEDED(status) && i < count; ++i) {
            D3D11_VIDEO_DECODER_CONFIG candidate{};
            status = gpu->video->GetVideoDecoderConfig(&description, i, &candidate);
            if (SUCCEEDED(status) && candidate.ConfigBitstreamRaw == 2 &&
                bytes_equal(&candidate.guidConfigBitstreamEncryption, &DXVA_NoEncrypt, sizeof(GUID))) {
                selected = candidate;
                found = true;
                break;
            }
        }
        if (SUCCEEDED(status) && !found) status = DXGI_ERROR_UNSUPPORTED;
    }
    if (SUCCEEDED(status)) status = gpu->video->CreateVideoDecoder(&description, &selected, &gpu->decoder);
    D3D11_TEXTURE2D_DESC texture{};
    texture.Width = sps.coded_width;
    texture.Height = sps.coded_height;
    texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_NV12;
    texture.SampleDesc.Count = 1;
    if (SUCCEEDED(status)) {
        texture.ArraySize = maximum<u32>(selected.ConfigMinRenderTargetBuffCount, surfaces_needed);
        texture.BindFlags = D3D11_BIND_DECODER;
        status = texture.ArraySize > 32 ? DXGI_ERROR_UNSUPPORTED
                                        : gpu->device->CreateTexture2D(&texture, nullptr, &gpu->surfaces);
    }
    if (SUCCEEDED(status)) {
        D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC view{};
        view.DecodeProfile = description.Guid;
        view.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
        for (u32 i = 0; i < texture.ArraySize && SUCCEEDED(status); ++i) {
            view.Texture2D.ArraySlice = i;
            status = gpu->video->CreateVideoDecoderOutputView(gpu->surfaces, &view, &gpu->views[i]);
        }
    }
    if (SUCCEEDED(status)) {
        // The lease outlives the DPB slice, so each output picture is copied
        // into this shader-readable texture the renderer converts in place.
        D3D11_TEXTURE2D_DESC display = texture;
        display.ArraySize = 1;
        display.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        status = gpu->device->CreateTexture2D(&display, nullptr, &gpu->display);
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8_UNORM;
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipLevels = 1;
        view.Texture2DArray.ArraySize = 1;
        if (SUCCEEDED(status))
            status = gpu->device->CreateShaderResourceView(gpu->display, &view, &gpu->luma);
        view.Format = DXGI_FORMAT_R8G8_UNORM;
        if (SUCCEEDED(status))
            status = gpu->device->CreateShaderResourceView(gpu->display, &view, &gpu->chroma);
        if (FAILED(status) && status != E_OUTOFMEMORY)
            status = DXGI_ERROR_UNSUPPORTED;
    }
    if (FAILED(status)) {
        release_decoder_objects(gpu);
        return status;
    }
    gpu->surface_count = static_cast<u8>(texture.ArraySize);
    gpu->coded_width = sps.coded_width;
    gpu->coded_height = sps.coded_height;
    return S_OK;
}

// Fills the driver's mapped buffers directly from the validated input. A
// picture larger than one bitstream buffer (4K keyframes) is submitted in
// several slice batches inside the same Begin/EndFrame, each carrying the
// picture parameters as DXVA requires per submission.
HRESULT submit(H264StreamDecoder* decoder, const H264StreamDecode& decode, ConstSlice<u8> source) noexcept {
    Gpu* gpu = &decoder->gpu;
    const H264StreamPicture& picture = decoder->picture;
    if (decode.surface >= gpu->surface_count || !gpu->views[decode.surface]) return E_INVALIDARG;
    for (u32 i = 0; i < picture.slice_count; ++i) {
        const auto range = picture.slices[i];
        if (!range.size || range.offset > source.count || range.size > source.count - range.offset)
            return E_INVALIDARG;
    }
    DXVA_PicParams_H264 parameters{};
    DXVA_Qmatrix_H264 matrix{};
    h264_dxva_picture_parameters(picture.sps, picture.pps, picture.first, picture.all_intra,
                                 decoder->state.references, decode.plan.decode_order, decode.surface,
                                 gpu->feedback, &parameters, &matrix);
    HRESULT status = S_OK;
    for (u32 attempt = 0;; ++attempt) {
        gpu->guard->Enter();
        status = gpu->context->DecoderBeginFrame(gpu->decoder, gpu->views[decode.surface], 0, nullptr);
        if (!busy(status) || attempt == kBusyRetries) break;
        gpu->guard->Leave();
        Sleep(1);
    }
    if (FAILED(status)) {
        gpu->guard->Leave();
        return status;
    }
    u32 next = 0;
    while (SUCCEEDED(status) && next < picture.slice_count) {
        D3D11_VIDEO_DECODER_BUFFER_DESC buffers[4]{};
        status = dxva_upload_buffer(gpu->context, gpu->decoder, D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS,
                                    &parameters, sizeof(parameters), &buffers[0]);
        if (SUCCEEDED(status))
            status = dxva_upload_buffer(gpu->context, gpu->decoder,
                                        D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX, &matrix,
                                        sizeof(matrix), &buffers[1]);
        if (FAILED(status)) break;
        UINT bit_capacity = 0, slice_capacity = 0;
        void* bit_data = nullptr;
        void* slice_data = nullptr;
        status = gpu->context->GetDecoderBuffer(gpu->decoder, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM, &bit_capacity,
                                                &bit_data);
        if (FAILED(status)) break;
        HRESULT slice_status = gpu->context->GetDecoderBuffer(gpu->decoder, D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL,
                                                              &slice_capacity, &slice_data);
        const bool slice_acquired = SUCCEEDED(slice_status);
        u32 offset = 0, count = 0;
        if (slice_acquired && bit_data && slice_data) {
            auto* bytes = static_cast<u8*>(bit_data);
            auto* slices = static_cast<DXVA_Slice_H264_Short*>(slice_data);
            const u32 slice_limit = slice_capacity / u32{sizeof(DXVA_Slice_H264_Short)};
            while (next + count < picture.slice_count && count < slice_limit) {
                const auto range = picture.slices[next + count];
                const u64 end = u64{offset} + 3u + range.size;
                if (end > bit_capacity) break;
                slices[count] = {offset, range.size + 3u, 0};
                bytes[offset] = 0;
                bytes[offset + 1u] = 0;
                bytes[offset + 2u] = 1;
                copy_bytes(bytes + offset + 3u, source.data + range.offset, range.size);
                offset = static_cast<u32>(end);
                ++count;
            }
            if (!count) slice_status = E_OUTOFMEMORY; // One slice exceeds the driver buffer.
            else {
                const u32 aligned = static_cast<u32>(minimum<u64>((u64{offset} + 127u) & ~127ull, bit_capacity));
                clear_bytes(bytes + offset, aligned - offset);
                slices[count - 1u].SliceBytesInBuffer += aligned - offset;
                offset = aligned;
            }
        } else if (slice_acquired) slice_status = E_OUTOFMEMORY;
        const HRESULT bit_released =
            gpu->context->ReleaseDecoderBuffer(gpu->decoder, D3D11_VIDEO_DECODER_BUFFER_BITSTREAM);
        const HRESULT slice_released =
            slice_acquired ? gpu->context->ReleaseDecoderBuffer(gpu->decoder, D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL)
                           : S_OK;
        status = FAILED(slice_status) ? slice_status : FAILED(bit_released) ? bit_released : slice_released;
        if (FAILED(status)) break;
        buffers[2].BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
        buffers[2].DataSize = offset;
        buffers[3].BufferType = D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL;
        buffers[3].DataSize = count * u32{sizeof(DXVA_Slice_H264_Short)};
        status = gpu->context->SubmitDecoderBuffers(gpu->decoder, 4, buffers);
        next += count;
    }
    const HRESULT ended = gpu->context->DecoderEndFrame(gpu->decoder);
    gpu->guard->Leave();
    return FAILED(status) ? status : ended;
}

void update_frame(H264StreamDecoder* decoder, const H264Sps& sps) noexcept {
    // Unspecified matrix: HD camera/screen encoders use BT.709, SD BT.601.
    const bool bt709 = sps.matrix == 1 || (sps.matrix == 2 && sps.height >= 720);
    decoder->frame = {decoder->gpu.display, 0, sps.crop_x, sps.crop_y, sps.width, sps.height, bt709, sps.full_range,
                      0, {}, decoder->gpu.luma, decoder->gpu.chroma};
    const auto extent = video_display_extent(sps.width, sps.height, sps.sar_width, sps.sar_height);
    decoder->display_width = extent.width;
    decoder->display_height = extent.height;
}

void request_keyframe(H264StreamDecoder* decoder) noexcept {
    if (decoder->state.unsupported || decoder->stats.device_lost) return;
    const u64 now = GetTickCount64();
    if (decoder->last_request_ms && now - decoder->last_request_ms < kH264StreamKeyframeIntervalMs) return;
    decoder->last_request_ms = now ? now : 1;
    saturating_increment(decoder->stats.keyframe_requests);
    if (decoder->options.sink.request_keyframe)
        decoder->options.sink.request_keyframe(decoder->options.sink.context, decoder->options.ssrc);
}

void present_ready(H264StreamDecoder* decoder) noexcept {
    H264StreamOutput output{};
    while (h264_stream_pop(&decoder->state, &output)) {
        Gpu* gpu = &decoder->gpu;
        if (!gpu->display || output.surface >= gpu->surface_count) continue;
        gpu->guard->Enter();
        gpu->immediate->CopySubresourceRegion(gpu->display, 0, 0, 0, 0, gpu->surfaces, output.surface, nullptr);
        gpu->guard->Leave();
        saturating_increment(decoder->stats.presented);
        if (decoder->options.sink.frame) {
            const H264StreamFrame frame{&decoder->frame, output.rtp_timestamp, decoder->display_width,
                                        decoder->display_height, output.keyframe};
            decoder->options.sink.frame(decoder->options.sink.context, decoder->options.ssrc, frame);
        }
    }
}

void gpu_failure(H264StreamDecoder* decoder, HRESULT status) noexcept {
    saturating_increment(decoder->stats.gpu_failures);
    decoder->stats.last_status = status;
    if (status == DXGI_ERROR_DEVICE_REMOVED || status == DXGI_ERROR_DEVICE_RESET ||
        (decoder->gpu.device && decoder->gpu.device->GetDeviceRemovedReason() != S_OK))
        decoder->stats.device_lost = true;
    h264_stream_reset(&decoder->state);
}

void decode_input(H264StreamDecoder* decoder, ConstSlice<u8> bytes, u32 rtp_timestamp, bool intact,
                  bool resynchronize) noexcept {
    H264StreamState* state = &decoder->state;
    if (resynchronize) h264_stream_lost(state);
    if (decoder->stats.device_lost) {
        saturating_increment(decoder->stats.dropped);
        return;
    }
    auto scanner = h264_stream_scanner(bytes);
    for (;;) {
        const H264ScanStatus scanned = h264_stream_scan(&state->sets, &scanner, &decoder->picture);
        if (scanned == H264ScanStatus::end) break;
        if (scanned == H264ScanStatus::damaged || !intact) {
            ++state->damaged;
            if (state->active && !state->waiting) h264_stream_lost(state);
            request_keyframe(decoder);
            continue;
        }
        H264StreamDecode decode{};
        const H264StreamAction action = h264_stream_prepare(state, decoder->picture, &decode);
        if (action == H264StreamAction::unsupported) continue;
        if (action != H264StreamAction::decode) {
            request_keyframe(decoder);
            continue;
        }
        present_ready(decoder); // Drain before an IDR or a texture replacement.
        if (decode.reinitialize) {
            const HRESULT status = create_decoder(&decoder->gpu, decoder->picture.sps, decode.surfaces_needed);
            h264_stream_set_surfaces(state, decoder->gpu.surface_count);
            if (FAILED(status)) {
                decoder->stats.last_status = status;
                if (status == DXGI_ERROR_UNSUPPORTED || status == E_INVALIDARG) {
                    state->unsupported = true;
                    h264_stream_lost(state);
                } else gpu_failure(decoder, status);
                continue;
            }
            saturating_increment(decoder->stats.reinitializations);
        }
        if (decoder->picture.first.idr) update_frame(decoder, decoder->picture.sps);
        const HRESULT status = submit(decoder, decode, bytes);
        if (FAILED(status)) {
            gpu_failure(decoder, status);
            request_keyframe(decoder);
            continue;
        }
        if (++decoder->gpu.feedback == 0) ++decoder->gpu.feedback;
        h264_stream_commit(state, decoder->picture, decode, rtp_timestamp);
        decoder->last_request_ms = 0; // A keyframe arrived or the stream is healthy.
        present_ready(decoder);
    }
}

// Frees the decoder, surfaces and display copy; the stream then waits for an
// IDR (parameter sets are kept) and recreates them at its geometry.
bool release_idle(H264StreamDecoder* decoder) noexcept {
    Gpu* gpu = &decoder->gpu;
    if (!gpu->decoder && !gpu->surfaces && !gpu->display) return false;
    present_ready(decoder); // Nothing decoded may be lost silently.
    if (gpu->guard) gpu->guard->Enter();
    release_decoder_objects(gpu);
    if (gpu->guard) gpu->guard->Leave();
    h264_stream_reset(&decoder->state);
    h264_stream_set_surfaces(&decoder->state, 0);
    decoder->frame = {};
    decoder->last_request_ms = 0; // The first picture after release asks at once.
    saturating_increment(decoder->stats.idle_releases);
    return true;
}

void publish_stats(H264StreamDecoder* decoder) noexcept {
    const H264StreamState& state = decoder->state;
    auto& stats = decoder->stats;
    stats.decoded = state.decoded;
    stats.dropped = state.dropped;
    stats.damaged = state.damaged;
    stats.gaps = state.gaps;
    stats.late = state.late;
    stats.coded_width = decoder->gpu.coded_width;
    stats.coded_height = decoder->gpu.coded_height;
    stats.surfaces = decoder->gpu.surface_count;
    stats.unsupported = state.unsupported;
}

DWORD WINAPI decode_main(void* parameter) {
    auto* decoder = static_cast<H264StreamDecoder*>(parameter);
    for (;;) {
        // Sleep indefinitely without DXVA objects; otherwise wake once when
        // the stream has been input-idle long enough to release them.
        const DWORD wait = decoder->gpu.decoder ? kH264StreamIdleReleaseMs : INFINITE;
        if (WaitForSingleObject(decoder->wake, wait) == WAIT_TIMEOUT) {
            AcquireSRWLockExclusive(&decoder->lock);
            const bool idle = !decoder->count && !decoder->stop;
            ReleaseSRWLockExclusive(&decoder->lock);
            if (idle && release_idle(decoder)) {
                AcquireSRWLockExclusive(&decoder->lock);
                publish_stats(decoder);
                ReleaseSRWLockExclusive(&decoder->lock);
            }
            continue;
        }
        for (;;) {
            AcquireSRWLockExclusive(&decoder->lock);
            if (decoder->stop) {
                ReleaseSRWLockExclusive(&decoder->lock);
                return 0;
            }
            if (!decoder->count) {
                ReleaseSRWLockExclusive(&decoder->lock);
                break;
            }
            // Swap buffers so the producer reuses the previous work buffer.
            Input& slot = decoder->queue[decoder->head];
            const Input taken = slot;
            slot.data = decoder->work.data;
            slot.capacity = decoder->work.capacity;
            slot.bytes = 0;
            decoder->work = taken;
            --decoder->count;
            // An empty queue reuses the spare in this slot on the next push.
            if (decoder->count) decoder->head = (decoder->head + 1u) % kH264StreamQueueFrames;
            ReleaseSRWLockExclusive(&decoder->lock);
            decode_input(decoder, {decoder->work.data, decoder->work.bytes}, decoder->work.rtp_timestamp,
                         decoder->work.intact, decoder->work.resynchronize);
            AcquireSRWLockExclusive(&decoder->lock);
            publish_stats(decoder);
            ReleaseSRWLockExclusive(&decoder->lock);
        }
    }
}

} // namespace

H264StreamDecoder* h264_stream_decoder_create(const H264StreamDecoderOptions& options) noexcept {
    if (!options.device) return nullptr;
    auto* decoder = static_cast<H264StreamDecoder*>(
        VirtualAlloc(nullptr, sizeof(H264StreamDecoder), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!decoder) return nullptr;
    decoder->options = options;
    decoder->state.output.leased = 255;
    h264_stream_reset(&decoder->state);
    InitializeSRWLock(&decoder->lock);
    decoder->gpu.device = options.device;
    options.device->AddRef();
    options.device->GetImmediateContext(&decoder->gpu.immediate);
    if (!decoder->gpu.immediate) {
        h264_stream_decoder_destroy(decoder);
        return nullptr;
    }
    if (!options.synchronous) {
        decoder->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (decoder->wake)
            decoder->thread = CreateThread(nullptr, 256u * 1024u, decode_main, decoder,
                                           STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!decoder->thread) {
            h264_stream_decoder_destroy(decoder);
            return nullptr;
        }
    }
    return decoder;
}

bool h264_stream_decoder_push(H264StreamDecoder* decoder, ConstSlice<u8> annexb, u32 rtp_timestamp,
                              bool intact) noexcept {
    // Bound before copying: an oversized frame is dropped like a lost one.
    if (!decoder || !annexb.data || !annexb.count || annexb.count > kH264StreamMaximumFrameBytes) return false;
    if (decoder->options.synchronous) {
        saturating_increment(decoder->stats.received);
        const bool resynchronize = decoder->resynchronize_pending;
        decoder->resynchronize_pending = false;
        decode_input(decoder, annexb, rtp_timestamp, intact, resynchronize);
        publish_stats(decoder);
        return true;
    }
    AcquireSRWLockExclusive(&decoder->lock);
    saturating_increment(decoder->stats.received);
    bool resynchronize = decoder->resynchronize_pending;
    if (decoder->count == kH264StreamQueueFrames) {
        // The decoder fell behind: queued latency is worse than a freeze.
        decoder->count = 0;
        saturating_increment(decoder->stats.overflows);
        resynchronize = true;
    }
    Input& slot = decoder->queue[(decoder->head + decoder->count) % kH264StreamQueueFrames];
    if (!reserve(&slot, annexb.count)) {
        decoder->resynchronize_pending = true;
        ReleaseSRWLockExclusive(&decoder->lock);
        return false;
    }
    copy_bytes(slot.data, annexb.data, annexb.count);
    slot.bytes = annexb.count;
    slot.rtp_timestamp = rtp_timestamp;
    slot.intact = intact;
    slot.resynchronize = resynchronize;
    decoder->resynchronize_pending = false;
    ++decoder->count;
    ReleaseSRWLockExclusive(&decoder->lock);
    SetEvent(decoder->wake);
    return true;
}

bool h264_stream_decoder_release_idle(H264StreamDecoder* decoder) noexcept {
    if (!decoder || !decoder->options.synchronous) return false;
    const bool released = release_idle(decoder);
    if (released) {
        publish_stats(decoder);
    }
    return released;
}

H264StreamDecoderStats h264_stream_decoder_stats(const H264StreamDecoder* decoder) noexcept {
    if (!decoder) return {};
    auto* lock = const_cast<SRWLOCK*>(&decoder->lock);
    AcquireSRWLockShared(lock);
    const H264StreamDecoderStats stats = decoder->stats;
    ReleaseSRWLockShared(lock);
    return stats;
}

void h264_stream_decoder_destroy(H264StreamDecoder* decoder) noexcept {
    if (!decoder) return;
    if (decoder->thread) {
        AcquireSRWLockExclusive(&decoder->lock);
        decoder->stop = true;
        ReleaseSRWLockExclusive(&decoder->lock);
        SetEvent(decoder->wake);
        WaitForSingleObject(decoder->thread, INFINITE);
        CloseHandle(decoder->thread);
    }
    if (decoder->wake) CloseHandle(decoder->wake);
    release_gpu(&decoder->gpu);
    for (auto& input : decoder->queue) release_buffer(&input);
    release_buffer(&decoder->work);
    VirtualFree(decoder, 0, MEM_RELEASE);
}

} // namespace maicord
