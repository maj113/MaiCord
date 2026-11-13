#include "maicord/platform/encode/gpu_stream_encoder.hpp"

#include "access_unit.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/sys/d3d11.hpp"
#include "maicord/sys/dxgi.hpp"
#include "maicord/sys/initguid.hpp"
#include "maicord/sys/media_foundation.hpp"

// The Windows Go Live encoder: the capture's BGRA texture is scaled and converted to NV12 by two draws on the shared
// immediate context (bgra_nv12.hlsl), in a context state of the encoder's own so the renderer's bindings are untouched,
// and the NV12 texture goes to the GPU vendor's hardware Media Foundation transform through the DXGI device manager.
// Hardware transforms are asynchronous: the encoder thread waits on the transform's events, feeding the newest
// converted picture when it asks for input and delivering units when it has output. Software transforms are never
// enumerated (MFT_ENUM_FLAG_HARDWARE only).

namespace maicord {
namespace {

// Indexed by GpuStreamCodec.
const GUID* const kCodecFormats[]{&MFVideoFormat_H264, &MFVideoFormat_HEVC, &MFVideoFormat_AV1};
// GUIDs the SDK does not declare with DEFINE_GUID at the Windows 10 floor the sys headers are checked at:
// MFT_ENUM_ADAPTER_LUID (<mfapi.h>, 1607 and later, with MFTEnum2 itself, which is loaded at run time),
// MF_TRANSFORM_ASYNC and MF_TRANSFORM_ASYNC_UNLOCK (defined in <mftransform.h>), and the ICodecAPI properties
// (<codecapi.h> names them through __uuidof).
constexpr GUID kAdapterLuid{0x1d39518c, 0xe220, 0x4da8, {0xa0, 0x7f, 0xba, 0x17, 0x25, 0x52, 0xd6, 0xb1}};
constexpr GUID kTransformAsync{0xf81a699a, 0x649a, 0x497d, {0x8c, 0x73, 0x29, 0xf8, 0xfe, 0xd6, 0xad, 0x7a}};
constexpr GUID kTransformAsyncUnlock{0xe5666d6b, 0x3422, 0x4eb6, {0xa4, 0x21, 0xda, 0x7d, 0xb1, 0xf8, 0xe2, 0x07}};
constexpr GUID kRateControlMode{0x1c0608e9, 0x370c, 0x4710, {0x8a, 0x58, 0xcb, 0x61, 0x81, 0xc4, 0x24, 0x23}};
constexpr GUID kMeanBitrate{0xf7222374, 0x2144, 0x4815, {0xb5, 0x50, 0xa3, 0x7f, 0x8e, 0x12, 0xee, 0x52}};
constexpr GUID kLowLatency{0x9c27891a, 0xed7a, 0x40e1, {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}};
constexpr GUID kGopSize{0x95f31b26, 0x95a4, 0x41aa, {0x93, 0x03, 0x24, 0x6a, 0x7f, 0xc6, 0xee, 0xf1}};
constexpr GUID kBPictures{0x8d390aac, 0xdc5c, 0x4200, {0xb5, 0x7f, 0x81, 0x4d, 0x04, 0xba, 0xba, 0xb2}};
constexpr GUID kForceKeyframe{0x398c1b98, 0x8353, 0x475a, {0x9e, 0xf2, 0x8f, 0x26, 0x5d, 0x26, 0x03, 0x45}};

constexpr u32 kEnumHardware = 0x4, kEnumSortAndFilter = 0x40;
constexpr MediaEventType kNeedInput = 601, kHaveOutput = 602;
constexpr u32 kProgressive = 2, kMatrixBt709 = 1, kRange16To235 = 2, kPrimariesBt709 = 2, kTransfer709 = 5;
constexpr u32 kRateControlCbr = 0;
// H.264 profiles tried in order: constrained baseline is what Discord's video negotiation names (profile-level-id
// 42e01f); an encoder without it gets baseline, then main. H.265 Main and AV1 Main (the default) are 8-bit 4:2:0.
constexpr u32 kH264Profiles[]{256, 66, 77};
constexpr u32 kH265ProfileMain = 1;
// Keyframes come from requests (PLI/FIR), not a schedule; the GOP only bounds how long a lost request can go unanswered.
constexpr u32 kGopFrames = 3000;
constexpr u32 kSlots = 6;
constexpr u8 kNone = 255;
constexpr u32 kParameterBytes = 1024;

// The transform's set-up runs once per encoder (and per probe): compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
// mfplat.dll, loaded with the encoder: the client never imports Media Foundation.
struct TypeInfo {
    GUID major, subtype;
};
struct Api {
    HMODULE library;
    HRESULT(WINAPI* startup)(ULONG, DWORD);
    HRESULT(WINAPI* shutdown)();
    HRESULT(WINAPI* enumerate)(GUID, UINT32, const TypeInfo*, const TypeInfo*, IMFAttributes*, IMFActivate***,
                               UINT32*);
    HRESULT(WINAPI* attributes)(IMFAttributes**, UINT32);
    HRESULT(WINAPI* media_type)(IMFMediaType**);
    HRESULT(WINAPI* sample)(IMFSample**);
    HRESULT(WINAPI* memory_buffer)(DWORD, IMFMediaBuffer**);
    HRESULT(WINAPI* surface_buffer)(const IID&, IUnknown*, UINT, BOOL, IMFMediaBuffer**);
    HRESULT(WINAPI* device_manager)(UINT*, IMFDXGIDeviceManager**);
};

// Api's members' names, in order.
constexpr char kFunctionNames[] = "MFStartup\0MFShutdown\0MFTEnum2\0MFCreateAttributes\0MFCreateMediaType\0"
                                  "MFCreateSample\0MFCreateMemoryBuffer\0MFCreateDXGISurfaceBuffer\0"
                                  "MFCreateDXGIDeviceManager\0";

// MFStartup on success; api_close undoes it.
bool api_open(Api* api) noexcept {
    api->library = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!api->library)
        return false;
    // Api is one function pointer per name.
    void** slot = reinterpret_cast<void**>(&api->startup);
    bool found = true;
    for (NameList names{kFunctionNames}; found && names; names.next(), ++slot)
        found = (*slot = reinterpret_cast<void*>(GetProcAddress(api->library, names.at))) != nullptr;
    if (found && SUCCEEDED(api->startup(MF_VERSION, MFSTARTUP_LITE)))
        return true;
    FreeLibrary(api->library);
    api->library = nullptr;
    return false;
}

void api_close(Api* api) noexcept {
    if (!api->library)
        return;
    api->shutdown();
    FreeLibrary(api->library);
    api->library = nullptr;
}

// Threads that never initialized COM run in the implicit MTA while one exists; this one lives with the process.
void ensure_mta() noexcept {
    static volatile LONG done;
    if (InterlockedCompareExchange(&done, 1, 0) == 0) {
        CO_MTA_USAGE_COOKIE cookie{};
        CoIncrementMTAUsage(&cookie);
    }
}

bool adapter_luid(ID3D11Device* device, LUID* luid) noexcept {
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC desc{};
    const bool ok = SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi))) &&
                    SUCCEEDED(dxgi->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc));
    com_release(adapter);
    com_release(dxgi);
    *luid = desc.AdapterLuid;
    return ok;
}

// The hardware encoders of `codec` on the adapter, best first (the system's merit order). The caller releases each
// activate and frees the array with CoTaskMemFree.
HRESULT enumerate(const Api& api, const LUID& luid, GpuStreamCodec codec, IMFActivate*** activates,
                  UINT32* count) noexcept {
    *activates = nullptr;
    *count = 0;
    IMFAttributes* filter = nullptr;
    HRESULT h = api.attributes(&filter, 1);
    if (SUCCEEDED(h))
        h = filter->SetBlob(kAdapterLuid, reinterpret_cast<const UINT8*>(&luid), sizeof(luid));
    const TypeInfo input{MFMediaType_Video, MFVideoFormat_NV12};
    const TypeInfo output{MFMediaType_Video, *kCodecFormats[static_cast<u8>(codec)]};
    if (SUCCEEDED(h))
        h = api.enumerate(MFT_CATEGORY_VIDEO_ENCODER, kEnumHardware | kEnumSortAndFilter, &input, &output, filter,
                          activates, count);
    com_release(filter);
    return h;
}

void release_activates(IMFActivate** activates, UINT32 count) noexcept {
    for (UINT32 i = 0; i < count; ++i)
        activates[i]->Release();
    if (activates)
        CoTaskMemFree(activates);
}

HRESULT set_value(ICodecAPI* api, const GUID& key, u32 value, VARTYPE type = VT_UI4) noexcept {
    VARIANT variant{};
    variant.vt = type;
    if (type == VT_BOOL)
        variant.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    else
        variant.ulVal = value;
    return api->SetValue(&key, &variant);
}

HRESULT video_type(const Api& api, const GUID& subtype, u32 width, u32 height, u32 fps, IMFMediaType** type) noexcept {
    HRESULT h = api.media_type(type);
    IMFMediaType* t = *type;
    if (SUCCEEDED(h))
        h = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(h))
        h = t->SetGUID(MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(h))
        h = t->SetUINT64(MF_MT_FRAME_SIZE, u64{width} << 32 | height);
    if (SUCCEEDED(h))
        h = t->SetUINT64(MF_MT_FRAME_RATE, u64{fps ? fps : 60} << 32 | 1);
    if (SUCCEEDED(h))
        h = t->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, u64{1} << 32 | 1);
    if (SUCCEEDED(h))
        h = t->SetUINT32(MF_MT_INTERLACE_MODE, kProgressive);
    // What the conversion writes; encoders that signal colour put it in the VUI / sequence header.
    if (SUCCEEDED(h))
        h = t->SetUINT32(MF_MT_YUV_MATRIX, kMatrixBt709);
    if (SUCCEEDED(h))
        h = t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, kRange16To235);
    if (SUCCEEDED(h))
        h = t->SetUINT32(MF_MT_VIDEO_PRIMARIES, kPrimariesBt709);
    if (SUCCEEDED(h))
        h = t->SetUINT32(MF_MT_TRANSFER_FUNCTION, kTransfer709);
    return h;
}

// One running hardware transform and what it was configured for.
struct Transform {
    IMFActivate* activate;
    IMFTransform* transform;
    IMFMediaEventGenerator* events;
    IMFShutdown* shutdown;
    ICodecAPI* codec_api;
    IMFSample* output_sample; // ours, when the transform does not provide samples
    IMFMediaBuffer* output_buffer;
    DWORD input_id, output_id;
    u32 width, height, bitrate;
    bool provides_samples;
};

void transform_release(Transform* t) noexcept {
    if (t->shutdown)
        t->shutdown->Shutdown();
    com_release(t->shutdown);
    com_release(t->events);
    com_release(t->codec_api);
    com_release(t->output_buffer);
    com_release(t->output_sample);
    com_release(t->transform);
    if (t->activate) {
        t->activate->ShutdownObject();
        t->activate->Release();
    }
    reset_value(t);
}

HRESULT output_type(const Api& api, Transform* t, GpuStreamCodec codec, u32 width, u32 height, u32 fps,
                    u32 bitrate) noexcept {
    // AV1 takes the encoder's default profile (Main: 8-bit 4:2:0); the others try theirs in order.
    const u32* profiles = codec == GpuStreamCodec::h264 ? kH264Profiles : &kH265ProfileMain;
    const u32 attempts = codec == GpuStreamCodec::h264 ? 3 : 1;
    HRESULT h = E_FAIL;
    for (u32 i = 0; i < attempts && FAILED(h); ++i) {
        IMFMediaType* type = nullptr;
        h = video_type(api, *kCodecFormats[static_cast<u8>(codec)], width, height, fps, &type);
        if (SUCCEEDED(h))
            h = type->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        if (SUCCEEDED(h) && codec != GpuStreamCodec::av1)
            h = type->SetUINT32(MF_MT_MPEG2_PROFILE, profiles[i]);
        if (SUCCEEDED(h))
            h = t->transform->SetOutputType(t->output_id, type, 0);
        com_release(type);
    }
    return h;
}

HRESULT transform_open(const Api& api, IMFDXGIDeviceManager* manager, const LUID& luid, GpuStreamCodec codec,
                       const GpuStreamEncoderSettings& settings, Transform* t) noexcept {
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT h = enumerate(api, luid, codec, &activates, &count);
    if (SUCCEEDED(h) && !count)
        h = MF_E_INVALIDMEDIATYPE;
    if (SUCCEEDED(h)) {
        t->activate = activates[0];
        t->activate->AddRef();
        h = t->activate->ActivateObject(__uuidof(IMFTransform), reinterpret_cast<void**>(&t->transform));
    }
    release_activates(activates, count);
    IMFAttributes* attributes = nullptr;
    UINT32 asynchronous = 0;
    if (SUCCEEDED(h))
        h = t->transform->GetAttributes(&attributes);
    if (SUCCEEDED(h) && (FAILED(attributes->GetUINT32(kTransformAsync, &asynchronous)) || !asynchronous))
        h = E_NOTIMPL; // Hardware encoders are asynchronous; anything else is not one.
    if (SUCCEEDED(h))
        h = attributes->SetUINT32(kTransformAsyncUnlock, TRUE);
    com_release(attributes);
    if (SUCCEEDED(h) && FAILED(t->transform->GetStreamIDs(1, &t->input_id, 1, &t->output_id)))
        t->input_id = t->output_id = 0;
    if (SUCCEEDED(h))
        h = t->transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager));
    if (SUCCEEDED(h))
        h = t->transform->QueryInterface(__uuidof(ICodecAPI), reinterpret_cast<void**>(&t->codec_api));
    if (SUCCEEDED(h)) {
        // Best effort: an encoder without one of these keeps its default.
        set_value(t->codec_api, kRateControlMode, kRateControlCbr);
        set_value(t->codec_api, kLowLatency, 1, VT_BOOL);
        set_value(t->codec_api, kBPictures, 0);
        set_value(t->codec_api, kGopSize, kGopFrames);
        set_value(t->codec_api, kMeanBitrate, settings.bitrate);
        h = output_type(api, t, codec, settings.width, settings.height, settings.max_fps, settings.bitrate);
    }
    IMFMediaType* input = nullptr;
    if (SUCCEEDED(h))
        h = video_type(api, MFVideoFormat_NV12, settings.width, settings.height, settings.max_fps, &input);
    if (SUCCEEDED(h))
        h = t->transform->SetInputType(t->input_id, input, 0);
    com_release(input);
    MFT_OUTPUT_STREAM_INFO info{};
    if (SUCCEEDED(h))
        h = t->transform->GetOutputStreamInfo(t->output_id, &info);
    t->provides_samples = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
    if (SUCCEEDED(h) && !t->provides_samples) {
        // At least a generous picture: the transform's own figure can be the input frame size.
        const DWORD bytes = info.cbSize > settings.width * settings.height ? info.cbSize : settings.width * settings.height;
        h = api.sample(&t->output_sample);
        if (SUCCEEDED(h))
            h = api.memory_buffer(bytes, &t->output_buffer);
        if (SUCCEEDED(h))
            h = t->output_sample->AddBuffer(t->output_buffer);
    }
    if (SUCCEEDED(h))
        h = t->transform->QueryInterface(__uuidof(IMFMediaEventGenerator), reinterpret_cast<void**>(&t->events));
    if (SUCCEEDED(h))
        h = t->transform->QueryInterface(__uuidof(IMFShutdown), reinterpret_cast<void**>(&t->shutdown));
    if (SUCCEEDED(h))
        h = t->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (SUCCEEDED(h))
        h = t->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    t->width = settings.width;
    t->height = settings.height;
    t->bitrate = settings.bitrate;
    if (FAILED(h))
        transform_release(t);
    return h;
}

#pragma clang attribute pop

// One NV12 surface the conversion writes and the transform reads, with the views of both and a sample over it.
struct Slot {
    ID3D11Texture2D* texture;
    ID3D11RenderTargetView* luma_target;
    ID3D11RenderTargetView* chroma_target;
    ID3D11ShaderResourceView* luma;
    ID3D11ShaderResourceView* chroma;
    IMFSample* sample;
    u64 timestamp_us;  // of the picture in it
    u64 submitted_us;  // of its latest input to the transform
    bool in_flight;    // input whose output has not come yet
};

// One plane's draw (bgra_nv12.hlsl).
struct EncodeConstants {
    float extent[2], step[2];
    u32 taps, chroma, reserved[2];
};
static_assert(sizeof(EncodeConstants) == 32);

u32 taps_for(float scale) noexcept {
    const u32 taps = static_cast<u32>(scale + 0.999f);
    return taps < 1 ? 1 : taps > 8 ? 8 : taps;
}

} // namespace

struct GpuStreamEncoder {
    GpuStreamEncoderOptions options; // settings under lock
    SRWLOCK lock;
    Api api;
    LUID luid;
    ID3D11Device* device;
    ID3D11DeviceContext* immediate;
    ID3D11DeviceContext1* context1;
    ID3D11Multithread* guard;
    ID3DDeviceContextState* state;
    ID3D11VertexShader* vertex;
    ID3D11PixelShader* pixel; // either plane: the vertex shader tells it which (bgra_nv12.hlsl)
    ID3D11SamplerState* sampler;
    ID3D11Buffer* constants;
    IMFDXGIDeviceManager* manager;
    Slot slots[kSlots];
    u32 slot_width, slot_height;
    u8 newest, held, pending;
    Transform mft;
    u32 need_input;
    u64 last_accepted_us;
    u32 serial; // pictures made the newest, for the preview's sequence
    bool accepted, keyframe_requested, stop, reopen, ended, opened_once;
    HANDLE thread, opened;
    HRESULT open_status;
    GpuStreamEncoderStats stats;
    // Encoder thread only.
    EncodedUnitBuffer unit;
    u8 parameters[kParameterBytes]; // H.264/H.265 parameter sets or the AV1 sequence header, for keyframes without
    u32 parameter_bytes;
};

namespace {

u64 now_us() noexcept {
    return clock_microseconds(clock_initialize(), 0, clock_now());
}

// Under the lock: the pending picture into the transform when it asked for input.
void feed(GpuStreamEncoder* e) noexcept {
    Transform& t = e->mft;
    if (!t.transform || !e->need_input || e->pending == kNone)
        return;
    // A picture of the new extent waits for the transform reopened at it (its first NeedInput feeds it).
    if (e->slot_width != t.width || e->slot_height != t.height)
        return;
    Slot& slot = e->slots[e->pending];
    e->pending = kNone;
    if (e->options.settings.bitrate != t.bitrate &&
        SUCCEEDED(set_value(t.codec_api, kMeanBitrate, e->options.settings.bitrate)))
        t.bitrate = e->options.settings.bitrate;
    if (e->keyframe_requested && SUCCEEDED(set_value(t.codec_api, kForceKeyframe, 1)))
        e->keyframe_requested = false;
    slot.sample->SetSampleTime(static_cast<LONGLONG>(slot.timestamp_us * 10));
    const u32 fps = e->options.settings.max_fps ? e->options.settings.max_fps : 60;
    slot.sample->SetSampleDuration(static_cast<LONGLONG>(10'000'000 / fps));
    if (SUCCEEDED(t.transform->ProcessInput(t.input_id, slot.sample, 0))) {
        --e->need_input;
        slot.in_flight = true;
        slot.submitted_us = slot.timestamp_us;
    }
}

} // namespace

#include "gpu_stream_encoder_shared.inc"

namespace {

void slot_release(Slot* slot) noexcept {
    com_release(slot->sample);
    com_release(slot->chroma);
    com_release(slot->luma);
    com_release(slot->chroma_target);
    com_release(slot->luma_target);
    com_release(slot->texture);
    reset_value(slot);
}

void slots_release(GpuStreamEncoder* e) noexcept {
    for (Slot& slot : e->slots)
        slot_release(&slot);
    e->newest = e->held = e->pending = kNone;
    e->slot_width = e->slot_height = 0;
}

#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
bool slot_create(GpuStreamEncoder* e, Slot* slot) noexcept {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = e->slot_width;
    desc.Height = e->slot_height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    D3D11_RENDER_TARGET_VIEW_DESC target{};
    target.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    view.Texture2DArray = {0, 1, 0, 1};
    HRESULT h = e->device->CreateTexture2D(&desc, nullptr, &slot->texture);
    target.Format = view.Format = DXGI_FORMAT_R8_UNORM;
    if (SUCCEEDED(h))
        h = e->device->CreateRenderTargetView(slot->texture, &target, &slot->luma_target);
    if (SUCCEEDED(h))
        h = e->device->CreateShaderResourceView(slot->texture, &view, &slot->luma);
    target.Format = view.Format = DXGI_FORMAT_R8G8_UNORM;
    if (SUCCEEDED(h))
        h = e->device->CreateRenderTargetView(slot->texture, &target, &slot->chroma_target);
    if (SUCCEEDED(h))
        h = e->device->CreateShaderResourceView(slot->texture, &view, &slot->chroma);
    IMFMediaBuffer* buffer = nullptr;
    IMF2DBuffer* planar = nullptr;
    DWORD length = 0;
    if (SUCCEEDED(h))
        h = e->api.sample(&slot->sample);
    if (SUCCEEDED(h))
        h = e->api.surface_buffer(__uuidof(ID3D11Texture2D), slot->texture, 0, FALSE, &buffer);
    // Some encoders take the buffer's length as the picture's size: give it the NV12 frame's.
    if (SUCCEEDED(h) && SUCCEEDED(buffer->QueryInterface(__uuidof(IMF2DBuffer), reinterpret_cast<void**>(&planar))) &&
        SUCCEEDED(planar->GetContiguousLength(&length)))
        buffer->SetCurrentLength(length);
    com_release(planar);
    if (SUCCEEDED(h))
        h = slot->sample->AddBuffer(buffer);
    com_release(buffer);
    if (FAILED(h))
        slot_release(slot);
    return SUCCEEDED(h);
}

#pragma clang attribute pop

// A slot the next picture may be written into: never the newest (a keyframe request encodes it again), the one the
// preview holds, the pending one, or one the transform has not finished with.
Slot* free_slot(GpuStreamEncoder* e) noexcept {
    // Surfaces that exist first; a new one only when all of them are taken.
    Slot* fresh = nullptr;
    for (u8 i = 0; i < kSlots; ++i) {
        Slot& slot = e->slots[i];
        if (i == e->newest || i == e->held || i == e->pending || slot.in_flight)
            continue;
        if (slot.texture)
            return &slot;
        if (!fresh)
            fresh = &slot;
    }
    return fresh && slot_create(e, fresh) ? fresh : nullptr;
}

bool convert(GpuStreamEncoder* e, const CaptureFrame& frame, Slot* slot) noexcept {
    auto* source = static_cast<ID3D11Texture2D*>(frame.texture);
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || !(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) || !frame.width ||
        !frame.height || frame.width > desc.Width || frame.height > desc.Height)
        return false;
    ID3D11ShaderResourceView* view = nullptr;
    if (FAILED(e->device->CreateShaderResourceView(source, nullptr, &view)))
        return false;
    const float width = static_cast<float>(e->slot_width), height = static_cast<float>(e->slot_height);
    EncodeConstants constants{};
    constants.extent[0] = static_cast<float>(frame.width) / static_cast<float>(desc.Width);
    constants.extent[1] = static_cast<float>(frame.height) / static_cast<float>(desc.Height);
    // Source texels per output texel along the longer reduction; each bilinear tap covers two.
    const float scale_x = static_cast<float>(frame.width) / width, scale_y = static_cast<float>(frame.height) / height;
    const float scale = scale_x > scale_y ? scale_x : scale_y;
    e->guard->Enter();
    ID3DDeviceContextState* previous = nullptr;
    e->context1->SwapDeviceContextState(e->state, &previous);
    ID3D11DeviceContext* c = e->immediate;
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(e->vertex, nullptr, 0);
    c->VSSetConstantBuffers(0, 1, &e->constants);
    c->PSSetConstantBuffers(0, 1, &e->constants);
    c->PSSetSamplers(0, 1, &e->sampler);
    c->PSSetShaderResources(0, 1, &view);
    c->PSSetShader(e->pixel, nullptr, 0);
    bool drawn = true;
    // The luma plane, then the chroma plane at half the extent: a chroma texel spans two luma texels each way.
    for (u32 chroma = 0; chroma < 2 && drawn; ++chroma) {
        const float span = chroma ? 2.0f : 1.0f;
        constants.step[0] = constants.extent[0] / width * span;
        constants.step[1] = constants.extent[1] / height * span;
        constants.taps = taps_for(scale * 0.5f * span);
        constants.chroma = chroma;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        drawn = SUCCEEDED(c->Map(e->constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        if (drawn) {
            copy_bytes(mapped.pData, &constants, sizeof(constants));
            c->Unmap(e->constants, 0);
            const D3D11_VIEWPORT port{0, 0, width / span, height / span, 0, 1};
            c->OMSetRenderTargets(1, chroma ? &slot->chroma_target : &slot->luma_target, nullptr);
            c->RSSetViewports(1, &port);
            c->Draw(3, 0);
        }
    }
    // The state object keeps no reference to the capture's texture or the slot.
    ID3D11ShaderResourceView* const no_view = nullptr;
    c->PSSetShaderResources(0, 1, &no_view);
    c->OMSetRenderTargets(0, nullptr, nullptr);
    e->context1->SwapDeviceContextState(previous, nullptr);
    com_release(previous);
    // Submitted now, so the encoder (which may work on another engine) starts behind the conversion.
    c->Flush();
    e->guard->Leave();
    view->Release();
    return drawn;
}

void end(GpuStreamEncoder* e, HRESULT status) noexcept {
    AcquireSRWLockExclusive(&e->lock);
    e->ended = true;
    e->stats.failure = static_cast<u32>(status);
    ReleaseSRWLockExclusive(&e->lock);
    const bool lost = FAILED(e->device->GetDeviceRemovedReason());
    e->options.sink.ended(e->options.sink.context,
                          lost ? GpuStreamEncoderEnd::device_lost : GpuStreamEncoderEnd::failed);
}

// Under the lock: the codec configuration keyframes may need, from the transform's current output type.
void fetch_parameters(GpuStreamEncoder* e) noexcept {
    IMFMediaType* type = nullptr;
    UINT32 bytes = 0;
    if (SUCCEEDED(e->mft.transform->GetOutputCurrentType(e->mft.output_id, &type)) &&
        SUCCEEDED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &bytes)) && bytes <= kParameterBytes &&
        SUCCEEDED(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, e->parameters, kParameterBytes, &bytes)))
        e->parameter_bytes = bytes;
    com_release(type);
}

struct Delivery {
    GpuStreamAccessUnit unit;
    bool ready;
};

// Under the lock: one output of the transform into e->unit.
HRESULT take_output(GpuStreamEncoder* e, Delivery* delivery) noexcept {
    Transform& t = e->mft;
    MFT_OUTPUT_DATA_BUFFER output{t.output_id, t.output_sample, 0, nullptr};
    if (t.output_buffer)
        t.output_buffer->SetCurrentLength(0);
    DWORD status = 0;
    HRESULT h = t.transform->ProcessOutput(0, 1, &output, &status);
    com_release(output.pEvents);
    if (h == MF_E_TRANSFORM_STREAM_CHANGE) {
        // The encoder settled its output type: take it as offered.
        IMFMediaType* type = nullptr;
        h = t.transform->GetOutputAvailableType(t.output_id, 0, &type);
        if (SUCCEEDED(h))
            h = t.transform->SetOutputType(t.output_id, type, 0);
        com_release(type);
        e->parameter_bytes = 0;
        return h;
    }
    if (FAILED(h) || !output.pSample)
        return h == MF_E_TRANSFORM_NEED_MORE_INPUT ? S_OK : h;
    IMFSample* sample = output.pSample;
    LONGLONG time = 0;
    UINT32 clean = 0;
    sample->GetSampleTime(&time);
    const bool keyframe = SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean;
    const u64 timestamp_us = static_cast<u64>(time) / 10;
    IMFMediaBuffer* buffer = nullptr;
    BYTE* data = nullptr;
    DWORD bytes = 0;
    h = sample->ConvertToContiguousBuffer(&buffer);
    if (SUCCEEDED(h))
        h = buffer->Lock(&data, nullptr, &bytes);
    bool written = false;
    if (SUCCEEDED(h)) {
        const GpuStreamCodec codec = e->options.codec;
        if (codec == GpuStreamCodec::av1) {
            ConstSlice<u8> found{};
            written = encoded_av1_unit({data, bytes}, {e->parameters, e->parameter_bytes}, keyframe, &e->unit, &found);
            if (written && found.count && found.count <= kParameterBytes) {
                copy_bytes(e->parameters, found.data, found.count);
                e->parameter_bytes = found.count;
            }
        } else {
            if (keyframe && !e->parameter_bytes)
                fetch_parameters(e);
            written = encoded_annexb_unit(codec, {data, bytes}, {e->parameters, e->parameter_bytes}, keyframe,
                                          &e->unit);
        }
        buffer->Unlock();
    }
    com_release(buffer);
    if (t.provides_samples)
        sample->Release();
    // The transform is done with every input up to this picture.
    for (Slot& slot : e->slots)
        if (slot.in_flight && slot.submitted_us <= timestamp_us)
            slot.in_flight = false;
    repeat_newest(e);
    if (FAILED(h) || !written)
        return h;
    delivery->ready = true;
    delivery->unit = {{e->unit.data, e->unit.count},
                      timestamp_us,
                      static_cast<u32>(timestamp_us * 9 / 100),
                      static_cast<u16>(t.width),
                      static_cast<u16>(t.height),
                      e->options.codec,
                      keyframe};
    saturating_increment(e->stats.encoded);
    saturating_increment_if(keyframe, e->stats.keyframes);
    e->stats.bytes += e->unit.count;
    return S_OK;
}

// Encoder thread: (re)opens the transform when asked to, then serves its events until it stops.
DWORD WINAPI encoder_main(void* parameter) noexcept {
    auto* e = static_cast<GpuStreamEncoder*>(parameter);
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        AcquireSRWLockExclusive(&e->lock);
        const bool stop = e->stop;
        const bool open = !stop && (e->reopen || !e->mft.transform);
        const GpuStreamEncoderSettings settings = e->options.settings;
        Transform old = e->mft;
        if (open) {
            reset_value(&e->mft);
            e->reopen = false;
            e->need_input = 0;
        }
        ReleaseSRWLockExclusive(&e->lock);
        if (stop)
            break;
        if (open) {
            transform_release(&old);
            Transform fresh{};
            const HRESULT h = transform_open(e->api, e->manager, e->luid, e->options.codec, settings, &fresh);
            AcquireSRWLockExclusive(&e->lock);
            if (SUCCEEDED(h)) {
                e->mft = fresh;
                e->parameter_bytes = 0;
                saturating_increment_if(e->opened_once, e->stats.reconfigurations);
            }
            const bool first = !e->opened_once;
            e->opened_once = true;
            ReleaseSRWLockExclusive(&e->lock);
            if (first) {
                e->open_status = h;
                SetEvent(e->opened);
            }
            if (FAILED(h)) {
                if (!first)
                    end(e, h);
                break;
            }
            continue; // Picks up a stop or reconfiguration that came while opening.
        }
        IMFMediaEvent* event = nullptr;
        HRESULT h = e->mft.events->GetEvent(0, &event);
        MediaEventType type = 0;
        HRESULT status = S_OK;
        if (SUCCEEDED(h)) {
            event->GetType(&type);
            event->GetStatus(&status);
            event->Release();
            h = status;
        }
        if (FAILED(h)) {
            AcquireSRWLockExclusive(&e->lock);
            const bool expected = e->stop || e->reopen; // Shut down on purpose to wake this thread.
            ReleaseSRWLockExclusive(&e->lock);
            if (expected)
                continue;
            end(e, h);
            break;
        }
        Delivery delivery{};
        AcquireSRWLockExclusive(&e->lock);
        if (type == kNeedInput) {
            ++e->need_input;
            feed(e);
        } else if (type == kHaveOutput) {
            h = take_output(e, &delivery);
        }
        ReleaseSRWLockExclusive(&e->lock);
        if (FAILED(h)) {
            end(e, h);
            break;
        }
        if (delivery.ready)
            e->options.sink.unit(e->options.sink.context, delivery.unit);
    }
    AcquireSRWLockExclusive(&e->lock);
    Transform last = e->mft;
    reset_value(&e->mft);
    ReleaseSRWLockExclusive(&e->lock);
    transform_release(&last);
    if (SUCCEEDED(apartment))
        CoUninitialize();
    return 0;
}

// Open, probe, create and teardown run once per encoder or reconfiguration: compiled for size. The picture path
// (convert, feed, take_output, the event loop and submit) stays at full speed.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
// Shuts the running transform down so its event wait returns (stop or reconfiguration). Called after the flag is set.
void wake(GpuStreamEncoder* e) noexcept {
    AcquireSRWLockExclusive(&e->lock);
    IMFShutdown* shutdown = e->mft.shutdown;
    if (shutdown)
        shutdown->AddRef();
    ReleaseSRWLockExclusive(&e->lock);
    if (shutdown) {
        shutdown->Shutdown();
        shutdown->Release();
    }
}

bool create_pipeline(GpuStreamEncoder* e) noexcept {
    UINT support = 0;
    if (FAILED(e->device->CheckFormatSupport(DXGI_FORMAT_NV12, &support)) ||
        !(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET))
        return false;
    ID3D11Device1* device1 = nullptr;
    const D3D_FEATURE_LEVEL level = e->device->GetFeatureLevel();
    HRESULT h = e->device->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void**>(&device1));
    if (SUCCEEDED(h))
        h = device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr,
                                              &e->state);
    com_release(device1);
    if (SUCCEEDED(h))
        h = e->immediate->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&e->context1));
    if (SUCCEEDED(h))
        h = e->immediate->QueryInterface(__uuidof(ID3D11Multithread), reinterpret_cast<void**>(&e->guard));
    if (SUCCEEDED(h))
        h = gpu_vertex_shader_create(e->device, ShaderId::encode_vertex, {}, &e->vertex, nullptr, nullptr);
    if (SUCCEEDED(h))
        h = gpu_pixel_shader_create(e->device, ShaderId::encode_pixel, &e->pixel);
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = 0;
    if (SUCCEEDED(h))
        h = e->device->CreateSamplerState(&sampler, &e->sampler);
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(EncodeConstants);
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (SUCCEEDED(h))
        h = e->device->CreateBuffer(&buffer, nullptr, &e->constants);
    UINT token = 0;
    if (SUCCEEDED(h))
        h = e->api.device_manager(&token, &e->manager);
    if (SUCCEEDED(h))
        h = e->manager->ResetDevice(e->device, token);
    return SUCCEEDED(h);
}

void release(GpuStreamEncoder* e) noexcept {
    if (e->opened)
        CloseHandle(e->opened);
    slots_release(e);
    encoded_unit_release(&e->unit);
    com_release(e->manager);
    com_release(e->constants);
    com_release(e->sampler);
    com_release(e->pixel);
    com_release(e->vertex);
    com_release(e->guard);
    com_release(e->context1);
    com_release(e->state);
    com_release(e->immediate);
    com_release(e->device);
    api_close(&e->api);
    pages_release(e);
}

// The probe's cache: codecs by adapter.
struct ProbeEntry {
    LUID luid;
    u32 codecs;
};
SRWLOCK g_probe_lock;
ProbeEntry g_probes[4];
u32 g_probe_count;

} // namespace

u32 gpu_stream_encoder_codecs(GpuDevice* device) noexcept {
    LUID luid{};
    if (!device || !adapter_luid(device, &luid))
        return 0;
    AcquireSRWLockExclusive(&g_probe_lock);
    for (u32 i = 0; i < g_probe_count; ++i)
        if (g_probes[i].luid.LowPart == luid.LowPart && g_probes[i].luid.HighPart == luid.HighPart) {
            const u32 codecs = g_probes[i].codecs;
            ReleaseSRWLockExclusive(&g_probe_lock);
            return codecs;
        }
    ensure_mta();
    u32 codecs = 0;
    Api api{};
    if (api_open(&api)) {
        IMFDXGIDeviceManager* manager = nullptr;
        UINT token = 0;
        if (SUCCEEDED(api.device_manager(&token, &manager)) && SUCCEEDED(manager->ResetDevice(device, token)))
            for (u8 codec = 0; codec <= static_cast<u8>(GpuStreamCodec::av1); ++codec) {
                // A transform that opens for 1080p60 at a stream's bitrate encodes this codec.
                Transform t{};
                if (SUCCEEDED(transform_open(api, manager, luid, static_cast<GpuStreamCodec>(codec),
                                             {1920, 1080, 60, 6'000'000}, &t)))
                    codecs |= 1u << codec;
                transform_release(&t);
            }
        com_release(manager);
        api_close(&api);
    }
    g_probes[g_probe_count % 4] = {luid, codecs};
    ++g_probe_count;
    if (g_probe_count > 4)
        g_probe_count = 4;
    ReleaseSRWLockExclusive(&g_probe_lock);
    return codecs;
}

GpuStreamEncoder* gpu_stream_encoder_create(const GpuStreamEncoderOptions& options) noexcept {
    if (!options.device || options.codec > GpuStreamCodec::av1 || !options.sink.unit || !options.sink.ended ||
        !settings_valid(options.settings))
        return nullptr;
    ensure_mta();
    auto* e = commit_pages<GpuStreamEncoder>();
    if (!e)
        return nullptr;
    e->options = options;
    e->newest = e->held = e->pending = kNone;
    e->device = options.device;
    e->device->AddRef();
    e->device->GetImmediateContext(&e->immediate);
    if (!adapter_luid(e->device, &e->luid) || !api_open(&e->api) || !create_pipeline(e)) {
        release(e);
        return nullptr;
    }
    e->opened = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (e->opened)
        e->thread = CreateThread(nullptr, 256u * 1024u, encoder_main, e, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!e->thread) {
        release(e);
        return nullptr;
    }
    WaitForSingleObject(e->opened, INFINITE);
    if (FAILED(e->open_status)) {
        WaitForSingleObject(e->thread, INFINITE);
        CloseHandle(e->thread);
        release(e);
        return nullptr;
    }
    return e;
}

#pragma clang attribute pop

bool gpu_stream_encoder_submit(GpuStreamEncoder* e, const CaptureFrame& frame) noexcept {
    if (!e || !frame.texture)
        return false;
    AcquireSRWLockExclusive(&e->lock);
    saturating_increment(e->stats.submitted);
    const GpuStreamEncoderSettings& s = e->options.settings;
    const u64 interval = s.max_fps ? 1'000'000u / s.max_fps : 0;
    // In time order, and no closer together than max_fps allows (a quarter interval of jitter forgiven).
    bool accept = !e->ended && !e->stop &&
                  (!e->accepted || (frame.timestamp_us > e->last_accepted_us &&
                                    frame.timestamp_us - e->last_accepted_us + interval / 4 >= interval));
    if (accept && (e->slot_width != s.width || e->slot_height != s.height)) {
        slots_release(e);
        e->slot_width = s.width;
        e->slot_height = s.height;
    }
    Slot* slot = accept ? free_slot(e) : nullptr;
    const bool queued = slot && convert(e, frame, slot);
    if (queued)
        enqueue(e, static_cast<u8>(slot - e->slots), frame.timestamp_us);
    else
        saturating_increment(e->stats.dropped);
    ReleaseSRWLockExclusive(&e->lock);
    return queued;
}

void gpu_stream_encoder_configure(GpuStreamEncoder* e, const GpuStreamEncoderSettings& settings) noexcept {
    if (!e || !settings_valid(settings))
        return;
    AcquireSRWLockExclusive(&e->lock);
    const bool extent = settings.width != e->options.settings.width || settings.height != e->options.settings.height;
    e->options.settings = settings;
    e->reopen = e->reopen || extent;
    ReleaseSRWLockExclusive(&e->lock);
    if (extent)
        wake(e);
}

bool gpu_stream_encoder_preview(GpuStreamEncoder* e, GpuVideoFrame* frame, u32* sequence) noexcept {
    if (!e || !frame || !sequence)
        return false;
    AcquireSRWLockExclusive(&e->lock);
    const bool ok = e->newest != kNone;
    if (ok) {
        Slot& slot = e->slots[e->newest];
        e->held = e->newest;
        slot.texture->AddRef();
        slot.luma->AddRef();
        slot.chroma->AddRef();
        *frame = {};
        frame->texture = slot.texture;
        frame->width = e->slot_width;
        frame->height = e->slot_height;
        frame->bt709 = true;
        frame->luma = slot.luma;
        frame->chroma = slot.chroma;
        *sequence = e->serial;
    }
    ReleaseSRWLockExclusive(&e->lock);
    return ok;
}

void gpu_stream_encoder_destroy(GpuStreamEncoder* e) noexcept {
    if (!e)
        return;
    AcquireSRWLockExclusive(&e->lock);
    e->stop = true;
    ReleaseSRWLockExclusive(&e->lock);
    wake(e);
    WaitForSingleObject(e->thread, INFINITE);
    CloseHandle(e->thread);
    release(e);
}

} // namespace maicord
