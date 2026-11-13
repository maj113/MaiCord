#include "gpu_stream_decoder_backend.hpp"
#include "av1_dxva_parameters.hpp"
#include "h264_dxva_parameters.hpp"
#include "hevc_dxva_parameters.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/initguid.hpp"
#include "maicord/sys/dxva.hpp"
// DXVA submission, once per frame: the slice copy is one memcpy per slice. Compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {

constexpr HRESULT kDrawingBusy = static_cast<HRESULT>(0x8876021C); // D3DERR_WASSTILLDRAWING
constexpr u32 kBusyRetries = 200;                                  // ~200 ms of 1 ms waits.
constexpr u32 kGpuStreamSliceBatch = 256;
constexpr u32 kControlBytes = kGpuStreamSliceBatch * u32{sizeof(DXVA_Slice_HEVC_Short)};
static_assert(kAv1StreamTiles * sizeof(DXVA_Tile_AV1) <= kControlBytes);

bool busy(HRESULT status) noexcept {
    return status == E_PENDING || status == kDrawingBusy || status == DXGI_ERROR_WAS_STILL_DRAWING;
}

struct GpuStreamDecoderDesc {
    const GUID* profile;
    u32 width, height;                 // Coded size.
    u32 surface_width, surface_height; // Allocation size (codec alignment).
    DXGI_RATIONAL rate;
    u8 bitstream_raw; // Short-slice ConfigBitstreamRaw value of the profile.
    u8 surfaces;
};
// Writes the bitstream and control entries of units [first, first + n) for the largest n <= limit that fits capacity;
// returns n (0: unit first does not fit) and the bitstream bytes.
using GpuStreamBatchFill = u32 (*)(const void* source, u32 first, u8* bitstream, u32 capacity, void* entries,
                                   u32 limit, u32* bytes) noexcept;
struct GpuStreamSubmission {
    u8 surface;
    ConstSlice<u8> parameters, matrix; // matrix may be empty.
    u32 units;                         // Slices or tiles.
    u32 entry_bytes, entry_limit;      // Control entry size and the most per batch.
    const void* source;
    GpuStreamBatchFill fill;
};

// Whether a DXVA call succeeded. A failure is counted (a removed or reset device ends decoding: stats.device_lost),
// except that a decoder (re)creation the GPU refuses for its geometry or profile gives the stream up (*unsupported).
bool succeeded(GpuStream* stream, HRESULT status, bool* unsupported = nullptr) noexcept {
    if (SUCCEEDED(status))
        return true;
    if (unsupported && (status == DXGI_ERROR_UNSUPPORTED || status == E_INVALIDARG)) {
        *unsupported = true;
        return false;
    }
    saturating_increment(stream->stats.gpu_failures);
    if (status == DXGI_ERROR_DEVICE_REMOVED || status == DXGI_ERROR_DEVICE_RESET ||
        stream->device->GetDeviceRemovedReason() != S_OK)
        stream->stats.device_lost = true;
    return false;
}

void gpu_stream_release_decoder(GpuStream* stream) noexcept {
    if (stream->guard)
        stream->guard->Enter();
    gpu_decoder_surfaces_release(&stream->surfaces);
    com_release(stream->decoder);
    if (stream->guard)
        stream->guard->Leave();
    reset_value(&stream->frame);
    stream->presented = 255;
    stream->slice_entries = 0;
    stream->coded_width = stream->coded_height = 0;
}

// Recreates the DXVA decoder and its shader-readable surface array.
// DXGI_ERROR_UNSUPPORTED when the GPU cannot decode this geometry.
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
        // before allocating a large DPB. Windows 10's runtime has the query.
        ID3D11VideoDevice1* video1 = nullptr;
        stream->video->QueryInterface(__uuidof(ID3D11VideoDevice1), reinterpret_cast<void**>(&video1));
        DXGI_RATIONAL rate = desc.rate;
        UINT caps = 0;
        if (SUCCEEDED(video1->GetVideoDecoderCaps(desc.profile, desc.width, desc.height, &rate, 0, nullptr, &caps)) &&
            (caps & D3D11_VIDEO_DECODER_CAPS_UNSUPPORTED))
            status = DXGI_ERROR_UNSUPPORTED;
        video1->Release();
    }
    D3D11_VIDEO_DECODER_CONFIG selected{};
    if (SUCCEEDED(status))
        status = gpu_decoder_select_config(stream->video, description, desc.bitstream_raw, &selected);
    if (SUCCEEDED(status))
        status = stream->video->CreateVideoDecoder(&description, &selected, &stream->decoder);
    if (SUCCEEDED(status)) {
        const u32 count = maximum<u32>(selected.ConfigMinRenderTargetBuffCount, desc.surfaces);
        status =
            gpu_decoder_surfaces_create(&stream->surfaces, stream->device, stream->video, *desc.profile,
                                        desc.surface_width, desc.surface_height, count);
    }
    if (FAILED(status)) {
        gpu_stream_release_decoder(stream);
        return status;
    }
    stream->coded_width = desc.width;
    stream->coded_height = desc.height;
    return S_OK;
}

// One picture into surface in one Begin/EndFrame: the parameters (and matrix) with each batch of units that fits the
// driver's bitstream and control buffers.
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
        const HRESULT result = gpu_decoder_upload_buffer(stream->context, stream->decoder, type, data.data,
                                                         data.count, description);
        if (SUCCEEDED(result))
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

// H.264/HEVC short slices (one entry layout) from Annex B NAL ranges of source.
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

// DXVA HEVC surfaces are allocated in 64-pixel units; the decoder and the conformance crop keep the coded size.
u32 surface_extent(u32 coded) noexcept {
    return (coded + 63u) & ~63u;
}

} // namespace

bool h264_gpu_stream_create(GpuStream* stream, const H264Sps& sps, u8 surfaces) noexcept {
    DXGI_RATIONAL rate{60, 1};
    if (sps.time_scale && sps.units_in_tick && sps.units_in_tick < 0x80000000u)
        rate = {sps.time_scale, sps.units_in_tick * 2u};
    const HRESULT status = gpu_stream_create_decoder(
        stream, {&D3D11_DECODER_PROFILE_H264_VLD_NOFGT, sps.coded_width, sps.coded_height, sps.coded_width,
                 sps.coded_height, rate, 2, surfaces});
    return succeeded(stream, status, &stream->h264.state.unsupported);
}

bool h264_gpu_stream_submit(GpuStream* stream, const H264StreamPicture& picture, const H264StreamDecode& decode,
                            ConstSlice<u8> bytes) noexcept {
    DXVA_PicParams_H264 parameters{};
    DXVA_Qmatrix_H264 matrix{};
    h264_dxva_picture_parameters(picture.sps, picture.pps, picture.first, picture.all_intra,
                                 stream->h264.state.references, decode.plan.decode_order, decode.surface,
                                 stream->feedback, &parameters, &matrix);
    const HRESULT status = gpu_stream_submit_slices(
        stream, decode.surface, {reinterpret_cast<const u8*>(&parameters), sizeof(parameters)},
        {reinterpret_cast<const u8*>(&matrix), sizeof(matrix)}, bytes, picture.slices, picture.slice_count);
    return succeeded(stream, status);
}

bool hevc_gpu_stream_create(GpuStream* stream, const HevcSps& sps, u8 surfaces) noexcept {
    const HRESULT status = gpu_stream_create_decoder(
        stream, {&D3D11_DECODER_PROFILE_HEVC_VLD_MAIN, sps.coded_width, sps.coded_height,
                 surface_extent(sps.coded_width), surface_extent(sps.coded_height), {60, 1}, 1, surfaces});
    return succeeded(stream, status, &stream->hevc.state.unsupported);
}

bool hevc_gpu_stream_submit(GpuStream* stream, const HevcStreamPicture& picture, const HevcStreamDecode& decode,
                            ConstSlice<u8> bytes) noexcept {
    HevcStreamState* state = &stream->hevc.state;
    const HevcConfiguration& config = state->sets.active;
    DXVA_PicParams_HEVC parameters;
    DXVA_Qmatrix_HEVC matrix{};
    const HevcReferenceState none{};
    if (!hevc_dxva_picture_parameters(config.sps, config.pps, picture.header, picture.intra,
                                      decode.restart ? none : state->references, decode.plan, decode.surface,
                                      stream->feedback, &parameters, &matrix)) {
        ++state->damaged;
        return false;
    }
    const HRESULT status = gpu_stream_submit_slices(
        stream, decode.surface, {reinterpret_cast<const u8*>(&parameters), sizeof(parameters)},
        {reinterpret_cast<const u8*>(&matrix), config.sps.scaling_enabled ? u32{sizeof(matrix)} : 0u}, bytes,
        picture.slices, picture.slice_count);
    return succeeded(stream, status);
}

// Hardware decode here is 8-bit 4:2:0 into NV12; P010 is not negotiated.
bool av1_gpu_stream_decodable(const GpuStream&, const Av1SequenceHeader& sequence) noexcept {
    return sequence.profile == 0 && sequence.bit_depth == 8 && !sequence.mono_chrome &&
           sequence.subsampling_x && sequence.subsampling_y && sequence.max_width <= 4096 &&
           sequence.max_height <= 4096;
}

bool av1_gpu_stream_create(GpuStream* stream) noexcept {
    Av1GpuStreamState* state = &stream->av1;
    const Av1SequenceHeader& sequence = state->sequence;
    // Surfaces cover whole superblocks of the largest frame.
    const u32 align = sequence.use_128x128_superblock ? 127u : 63u;
    const HRESULT status = gpu_stream_create_decoder(
        stream, {&D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0, sequence.max_width, sequence.max_height,
                 (sequence.max_width + align) & ~align, (sequence.max_height + align) & ~align, {60, 1}, 1,
                 state->surfaces_wanted});
    return succeeded(stream, status, &state->unsupported);
}

// One frame into surface. A batch's bitstream is the temporal unit span from
// its first to last tile, zero-padded to 128 bytes with at least one zero.
bool av1_gpu_stream_submit(GpuStream* stream, u8 surface, ConstSlice<u8> unit) noexcept {
    Av1GpuStreamState* state = &stream->av1;
    DXVA_PicParams_AV1 parameters;
    av1_dxva_picture_parameters(state->sequence, state->header, state->slot_surface, state->references, surface,
                                &parameters);
    struct Source {
        const u8* unit;
        const Av1Tile* tiles;
    } const source{unit.data, state->tiles};
    const auto fill = [](const void* opaque, u32 first, u8* bytes, u32 capacity, void* control, u32 limit,
                         u32* written) noexcept -> u32 {
        const auto& from = *static_cast<const Source*>(opaque);
        auto* entries = static_cast<DXVA_Tile_AV1*>(control);
        const u32 start = from.tiles[first].offset;
        u32 batch = 0, end = start;
        for (; batch < limit; ++batch) {
            const Av1Tile& tile = from.tiles[first + batch];
            const u64 tile_end = u64{tile.offset} + tile.size;
            if (tile.offset < start || ((tile_end - start + 128u) & ~127ull) > capacity)
                break;
            entries[batch] = {tile.offset - start, tile.size, tile.row, tile.column, 0, 0xff, 0};
            end = maximum(end, static_cast<u32>(tile_end));
        }
        if (batch) {
            *written = static_cast<u32>((u64{end} - start + 128u) & ~127ull);
            copy_bytes(bytes, from.unit + start, end - start);
            clear_bytes(bytes + (end - start), *written - (end - start));
        }
        return batch;
    };
    const HRESULT status = gpu_stream_submit(stream, {surface,
                                                      {reinterpret_cast<const u8*>(&parameters), sizeof(parameters)},
                                                      {},
                                                      state->tile_count,
                                                      u32{sizeof(DXVA_Tile_AV1)},
                                                      kAv1StreamTiles,
                                                      &source,
                                                      fill});
    return succeeded(stream, status);
}

namespace {

struct Codec {
    const GUID* profile;
    u8 bitstream_raw; // Short-slice ConfigBitstreamRaw value of the profile.
    StreamBackend backend;
};
// Indexed by GpuStreamCodec.
constexpr Codec kCodecs[]{
    {&D3D11_DECODER_PROFILE_H264_VLD_NOFGT, 2,
     {h264_gpu_stream_reset, h264_gpu_stream_decode, h264_gpu_stream_drain, gpu_stream_release_decoder,
      h264_gpu_stream_stats}},
    {&D3D11_DECODER_PROFILE_HEVC_VLD_MAIN, 1,
     {hevc_gpu_stream_reset, hevc_gpu_stream_decode, hevc_gpu_stream_drain, gpu_stream_release_decoder,
      hevc_gpu_stream_stats}},
    {&D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0, 1,
     {av1_gpu_stream_reset, av1_gpu_stream_decode, av1_gpu_stream_drain, gpu_stream_release_decoder,
      av1_gpu_stream_stats}}};

} // namespace

u32 gpu_stream_decoder_codecs(ID3D11Device* device) noexcept {
    ID3D11VideoDevice* video = nullptr;
    if (!gpu_decoder_surfaces_supported(device) ||
        FAILED(device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(&video))))
        return 0;
    u32 codecs = 0;
    for (u32 codec = 0; codec < sizeof(kCodecs) / sizeof(kCodecs[0]); ++codec) {
        const Codec& entry = kCodecs[codec];
        BOOL supported = FALSE;
        D3D11_VIDEO_DECODER_CONFIG config{};
        if (SUCCEEDED(video->CheckVideoDecoderFormat(entry.profile, DXGI_FORMAT_NV12, &supported)) &&
            supported &&
            SUCCEEDED(gpu_decoder_select_config(video, {*entry.profile, 1920, 1080, DXGI_FORMAT_NV12},
                                                entry.bitstream_raw, &config)))
            codecs |= 1u << codec;
    }
    video->Release();
    return codecs;
}

const StreamBackend* stream_backend(GpuDevice*, GpuStreamCodec codec) noexcept {
    return &kCodecs[static_cast<u8>(codec)].backend;
}

} // namespace maicord
#pragma clang attribute pop
