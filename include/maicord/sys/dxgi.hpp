#pragma once

// DXGI, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/com.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <dxgi1_4.h>
#endif

// <dxgi.h>
#define DXGI_USAGE_RENDER_TARGET_OUTPUT 0x00000020UL

typedef UINT DXGI_USAGE;

MAICORD_SYS_TYPES_BEGIN
struct DXGI_ADAPTER_DESC;
struct DXGI_MODE_DESC;
struct DXGI_OUTPUT_DESC;
struct DXGI_PRESENT_PARAMETERS;
struct DXGI_QUERY_VIDEO_MEMORY_INFO;
struct DXGI_RATIONAL;
struct DXGI_SAMPLE_DESC;
struct DXGI_SWAP_CHAIN_DESC1;
struct DXGI_SWAP_CHAIN_DESC;
struct IDXGIAdapter1;
struct IDXGIAdapter2;
struct IDXGIAdapter3;
struct IDXGIAdapter;
struct IDXGIDevice1;
struct IDXGIDevice2;
struct IDXGIDevice3;
struct IDXGIDevice;
struct IDXGIDeviceSubObject;
struct IDXGIFactory1;
struct IDXGIFactory2;
struct IDXGIFactory;
struct IDXGIObject;
struct IDXGIOutput;
struct IDXGISwapChain1;
struct IDXGISwapChain2;
struct IDXGISwapChain;
struct _D3DCOLORVALUE;

// <dxgiformat.h>
enum DXGI_FORMAT {
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R32G32B32A32_TYPELESS = 1,
    DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
    DXGI_FORMAT_R32G32B32A32_UINT = 3,
    DXGI_FORMAT_R32G32B32A32_SINT = 4,
    DXGI_FORMAT_R32G32B32_TYPELESS = 5,
    DXGI_FORMAT_R32G32B32_FLOAT = 6,
    DXGI_FORMAT_R32G32B32_UINT = 7,
    DXGI_FORMAT_R32G32B32_SINT = 8,
    DXGI_FORMAT_R16G16B16A16_TYPELESS = 9,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
    DXGI_FORMAT_R16G16B16A16_UNORM = 11,
    DXGI_FORMAT_R16G16B16A16_UINT = 12,
    DXGI_FORMAT_R16G16B16A16_SNORM = 13,
    DXGI_FORMAT_R16G16B16A16_SINT = 14,
    DXGI_FORMAT_R32G32_TYPELESS = 15,
    DXGI_FORMAT_R32G32_FLOAT = 16,
    DXGI_FORMAT_R32G32_UINT = 17,
    DXGI_FORMAT_R32G32_SINT = 18,
    DXGI_FORMAT_R32G8X24_TYPELESS = 19,
    DXGI_FORMAT_D32_FLOAT_S8X24_UINT = 20,
    DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS = 21,
    DXGI_FORMAT_X32_TYPELESS_G8X24_UINT = 22,
    DXGI_FORMAT_R10G10B10A2_TYPELESS = 23,
    DXGI_FORMAT_R10G10B10A2_UNORM = 24,
    DXGI_FORMAT_R10G10B10A2_UINT = 25,
    DXGI_FORMAT_R11G11B10_FLOAT = 26,
    DXGI_FORMAT_R8G8B8A8_TYPELESS = 27,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB = 29,
    DXGI_FORMAT_R8G8B8A8_UINT = 30,
    DXGI_FORMAT_R8G8B8A8_SNORM = 31,
    DXGI_FORMAT_R8G8B8A8_SINT = 32,
    DXGI_FORMAT_R16G16_TYPELESS = 33,
    DXGI_FORMAT_R16G16_FLOAT = 34,
    DXGI_FORMAT_R16G16_UNORM = 35,
    DXGI_FORMAT_R16G16_UINT = 36,
    DXGI_FORMAT_R16G16_SNORM = 37,
    DXGI_FORMAT_R16G16_SINT = 38,
    DXGI_FORMAT_R32_TYPELESS = 39,
    DXGI_FORMAT_D32_FLOAT = 40,
    DXGI_FORMAT_R32_FLOAT = 41,
    DXGI_FORMAT_R32_UINT = 42,
    DXGI_FORMAT_R32_SINT = 43,
    DXGI_FORMAT_R24G8_TYPELESS = 44,
    DXGI_FORMAT_D24_UNORM_S8_UINT = 45,
    DXGI_FORMAT_R24_UNORM_X8_TYPELESS = 46,
    DXGI_FORMAT_X24_TYPELESS_G8_UINT = 47,
    DXGI_FORMAT_R8G8_TYPELESS = 48,
    DXGI_FORMAT_R8G8_UNORM = 49,
    DXGI_FORMAT_R8G8_UINT = 50,
    DXGI_FORMAT_R8G8_SNORM = 51,
    DXGI_FORMAT_R8G8_SINT = 52,
    DXGI_FORMAT_R16_TYPELESS = 53,
    DXGI_FORMAT_R16_FLOAT = 54,
    DXGI_FORMAT_D16_UNORM = 55,
    DXGI_FORMAT_R16_UNORM = 56,
    DXGI_FORMAT_R16_UINT = 57,
    DXGI_FORMAT_R16_SNORM = 58,
    DXGI_FORMAT_R16_SINT = 59,
    DXGI_FORMAT_R8_TYPELESS = 60,
    DXGI_FORMAT_R8_UNORM = 61,
    DXGI_FORMAT_R8_UINT = 62,
    DXGI_FORMAT_R8_SNORM = 63,
    DXGI_FORMAT_R8_SINT = 64,
    DXGI_FORMAT_A8_UNORM = 65,
    DXGI_FORMAT_R1_UNORM = 66,
    DXGI_FORMAT_R9G9B9E5_SHAREDEXP = 67,
    DXGI_FORMAT_R8G8_B8G8_UNORM = 68,
    DXGI_FORMAT_G8R8_G8B8_UNORM = 69,
    DXGI_FORMAT_BC1_TYPELESS = 70,
    DXGI_FORMAT_BC1_UNORM = 71,
    DXGI_FORMAT_BC1_UNORM_SRGB = 72,
    DXGI_FORMAT_BC2_TYPELESS = 73,
    DXGI_FORMAT_BC2_UNORM = 74,
    DXGI_FORMAT_BC2_UNORM_SRGB = 75,
    DXGI_FORMAT_BC3_TYPELESS = 76,
    DXGI_FORMAT_BC3_UNORM = 77,
    DXGI_FORMAT_BC3_UNORM_SRGB = 78,
    DXGI_FORMAT_BC4_TYPELESS = 79,
    DXGI_FORMAT_BC4_UNORM = 80,
    DXGI_FORMAT_BC4_SNORM = 81,
    DXGI_FORMAT_BC5_TYPELESS = 82,
    DXGI_FORMAT_BC5_UNORM = 83,
    DXGI_FORMAT_BC5_SNORM = 84,
    DXGI_FORMAT_B5G6R5_UNORM = 85,
    DXGI_FORMAT_B5G5R5A1_UNORM = 86,
    DXGI_FORMAT_B8G8R8A8_UNORM = 87,
    DXGI_FORMAT_B8G8R8X8_UNORM = 88,
    DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM = 89,
    DXGI_FORMAT_B8G8R8A8_TYPELESS = 90,
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB = 91,
    DXGI_FORMAT_B8G8R8X8_TYPELESS = 92,
    DXGI_FORMAT_B8G8R8X8_UNORM_SRGB = 93,
    DXGI_FORMAT_BC6H_TYPELESS = 94,
    DXGI_FORMAT_BC6H_UF16 = 95,
    DXGI_FORMAT_BC6H_SF16 = 96,
    DXGI_FORMAT_BC7_TYPELESS = 97,
    DXGI_FORMAT_BC7_UNORM = 98,
    DXGI_FORMAT_BC7_UNORM_SRGB = 99,
    DXGI_FORMAT_AYUV = 100,
    DXGI_FORMAT_Y410 = 101,
    DXGI_FORMAT_Y416 = 102,
    DXGI_FORMAT_NV12 = 103,
    DXGI_FORMAT_P010 = 104,
    DXGI_FORMAT_P016 = 105,
    DXGI_FORMAT_420_OPAQUE = 106,
    DXGI_FORMAT_YUY2 = 107,
    DXGI_FORMAT_Y210 = 108,
    DXGI_FORMAT_Y216 = 109,
    DXGI_FORMAT_NV11 = 110,
    DXGI_FORMAT_AI44 = 111,
    DXGI_FORMAT_IA44 = 112,
    DXGI_FORMAT_P8 = 113,
    DXGI_FORMAT_A8P8 = 114,
    DXGI_FORMAT_B4G4R4A4_UNORM = 115,
    DXGI_FORMAT_P208 = 130,
    DXGI_FORMAT_V208 = 131,
    DXGI_FORMAT_V408 = 132,
    DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE = 189,
    DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE = 190,
    DXGI_FORMAT_A4B4G4R4_UNORM = 191,
    DXGI_FORMAT_FORCE_UINT = 0xffffffff
};
// <dxgicommon.h>
struct DXGI_RATIONAL {
    UINT Numerator;
    UINT Denominator;
};
struct DXGI_SAMPLE_DESC {
    UINT Count;
    UINT Quality;
};
// <dxgitype.h>
struct _D3DCOLORVALUE {
    float r;
    float g;
    float b;
    float a;
};
typedef _D3DCOLORVALUE D3DCOLORVALUE;
enum DXGI_MODE_SCANLINE_ORDER {
    DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED = 0,
    DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE = 1,
    DXGI_MODE_SCANLINE_ORDER_UPPER_FIELD_FIRST = 2,
    DXGI_MODE_SCANLINE_ORDER_LOWER_FIELD_FIRST = 3
};
enum DXGI_MODE_SCALING {
    DXGI_MODE_SCALING_UNSPECIFIED = 0,
    DXGI_MODE_SCALING_CENTERED = 1,
    DXGI_MODE_SCALING_STRETCHED = 2
};
enum DXGI_MODE_ROTATION {
    DXGI_MODE_ROTATION_UNSPECIFIED = 0,
    DXGI_MODE_ROTATION_IDENTITY = 1,
    DXGI_MODE_ROTATION_ROTATE90 = 2,
    DXGI_MODE_ROTATION_ROTATE180 = 3,
    DXGI_MODE_ROTATION_ROTATE270 = 4
};
struct DXGI_MODE_DESC {
    UINT Width;
    UINT Height;
    DXGI_RATIONAL RefreshRate;
    DXGI_FORMAT Format;
    DXGI_MODE_SCANLINE_ORDER ScanlineOrdering;
    DXGI_MODE_SCALING Scaling;
};
// <dxgi.h>
struct DXGI_ADAPTER_DESC {
    WCHAR Description[128];
    UINT VendorId;
    UINT DeviceId;
    UINT SubSysId;
    UINT Revision;
    SIZE_T DedicatedVideoMemory;
    SIZE_T DedicatedSystemMemory;
    SIZE_T SharedSystemMemory;
    LUID AdapterLuid;
};
struct DXGI_OUTPUT_DESC {
    WCHAR DeviceName[32];
    RECT DesktopCoordinates;
    BOOL AttachedToDesktop;
    DXGI_MODE_ROTATION Rotation;
    HMONITOR Monitor;
};
enum DXGI_SWAP_EFFECT {
    DXGI_SWAP_EFFECT_DISCARD = 0,
    DXGI_SWAP_EFFECT_SEQUENTIAL = 1,
    DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL = 3,
    DXGI_SWAP_EFFECT_FLIP_DISCARD = 4
};
enum DXGI_SWAP_CHAIN_FLAG {
    DXGI_SWAP_CHAIN_FLAG_NONPREROTATED = 1,
    DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH = 2,
    DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE = 4,
    DXGI_SWAP_CHAIN_FLAG_RESTRICTED_CONTENT = 8,
    DXGI_SWAP_CHAIN_FLAG_RESTRICT_SHARED_RESOURCE_DRIVER = 16,
    DXGI_SWAP_CHAIN_FLAG_DISPLAY_ONLY = 32,
    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT = 64,
    DXGI_SWAP_CHAIN_FLAG_FOREGROUND_LAYER = 128,
    DXGI_SWAP_CHAIN_FLAG_FULLSCREEN_VIDEO = 256,
    DXGI_SWAP_CHAIN_FLAG_YUV_VIDEO = 512,
    DXGI_SWAP_CHAIN_FLAG_HW_PROTECTED = 1024,
    DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING = 2048,
    DXGI_SWAP_CHAIN_FLAG_RESTRICTED_TO_ALL_HOLOGRAPHIC_DISPLAYS = 4096
};
struct DXGI_SWAP_CHAIN_DESC {
    DXGI_MODE_DESC BufferDesc;
    DXGI_SAMPLE_DESC SampleDesc;
    DXGI_USAGE BufferUsage;
    UINT BufferCount;
    HWND OutputWindow;
    BOOL Windowed;
    DXGI_SWAP_EFFECT SwapEffect;
    UINT Flags;
};
struct __declspec(uuid("aec22fb8-76f3-4639-9be0-28eb43a67a2e")) __declspec(novtable) IDXGIObject : IUnknown {
    MAICORD_COM_UNUSED(SetPrivateData);
    virtual HRESULT __stdcall SetPrivateDataInterface(const GUID& Name, const IUnknown* pUnknown) = 0;
    MAICORD_COM_UNUSED(GetPrivateData);
    virtual HRESULT __stdcall GetParent(const IID& riid, void** ppParent) = 0;
};
struct __declspec(uuid("3d3e0379-f9de-4d58-bb6c-18d62992f1a6")) __declspec(novtable) IDXGIDeviceSubObject : IDXGIObject {
    virtual HRESULT __stdcall GetDevice(const IID& riid, void** ppDevice) = 0;
};
struct __declspec(uuid("2411e7e1-12ac-4ccf-bd14-9798e8534dc0")) __declspec(novtable) IDXGIAdapter : IDXGIObject {
    MAICORD_COM_UNUSED(EnumOutputs);
    virtual HRESULT __stdcall GetDesc(DXGI_ADAPTER_DESC* pDesc) = 0;
    MAICORD_COM_UNUSED(CheckInterfaceSupport);
};
struct __declspec(uuid("ae02eedb-c735-4690-8d52-5a8dc20213aa")) __declspec(novtable) IDXGIOutput : IDXGIObject {
    virtual HRESULT __stdcall GetDesc(DXGI_OUTPUT_DESC* pDesc) = 0;
};
struct __declspec(uuid("310d36a0-d2e7-4c0a-aa04-6a9d23b8886a")) __declspec(novtable) IDXGISwapChain : IDXGIDeviceSubObject {
    virtual HRESULT __stdcall Present(UINT SyncInterval, UINT Flags) = 0;
    virtual HRESULT __stdcall GetBuffer(UINT Buffer, const IID& riid, void** ppSurface) = 0;
    MAICORD_COM_UNUSED(SetFullscreenState);
    MAICORD_COM_UNUSED(GetFullscreenState);
    virtual HRESULT __stdcall GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) = 0;
    virtual HRESULT __stdcall ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
        UINT SwapChainFlags) = 0;
    MAICORD_COM_UNUSED(ResizeTarget);
    MAICORD_COM_UNUSED(GetContainingOutput);
    MAICORD_COM_UNUSED(GetFrameStatistics);
    MAICORD_COM_UNUSED(GetLastPresentCount);
};
struct __declspec(uuid("7b7166ec-21c7-44ae-b21a-c9ae321ae369")) __declspec(novtable) IDXGIFactory : IDXGIObject {
    MAICORD_COM_UNUSED(EnumAdapters);
    MAICORD_COM_UNUSED(MakeWindowAssociation);
    MAICORD_COM_UNUSED(GetWindowAssociation);
    MAICORD_COM_UNUSED(CreateSwapChain);
    MAICORD_COM_UNUSED(CreateSoftwareAdapter);
};
struct __declspec(uuid("54ec77fa-1377-44e6-8c32-88fd5f44c84c")) __declspec(novtable) IDXGIDevice : IDXGIObject {
    virtual HRESULT __stdcall GetAdapter(IDXGIAdapter** pAdapter) = 0;
    MAICORD_COM_UNUSED(CreateSurface);
    MAICORD_COM_UNUSED(QueryResourceResidency);
    MAICORD_COM_UNUSED(SetGPUThreadPriority);
    MAICORD_COM_UNUSED(GetGPUThreadPriority);
};
struct __declspec(uuid("770aae78-f26f-4dba-a829-253c83d1b387")) __declspec(novtable) IDXGIFactory1 : IDXGIFactory {
    MAICORD_COM_UNUSED(EnumAdapters1);
    MAICORD_COM_UNUSED(IsCurrent);
};
struct __declspec(uuid("29038f61-3839-4626-91fd-086879011a05")) __declspec(novtable) IDXGIAdapter1 : IDXGIAdapter {
    MAICORD_COM_UNUSED(GetDesc1);
};
struct __declspec(uuid("77db970f-6276-48ba-ba28-070143b4392c")) __declspec(novtable) IDXGIDevice1 : IDXGIDevice {
    virtual HRESULT __stdcall SetMaximumFrameLatency(UINT MaxLatency) = 0;
    MAICORD_COM_UNUSED(GetMaximumFrameLatency);
};
// <dxgi1_2.h>
enum DXGI_ALPHA_MODE {
    DXGI_ALPHA_MODE_UNSPECIFIED = 0,
    DXGI_ALPHA_MODE_PREMULTIPLIED = 1,
    DXGI_ALPHA_MODE_STRAIGHT = 2,
    DXGI_ALPHA_MODE_IGNORE = 3,
    DXGI_ALPHA_MODE_FORCE_DWORD = 0xffffffff
};
struct __declspec(uuid("05008617-fbfd-4051-a790-144884b4f6a9")) __declspec(novtable) IDXGIDevice2 : IDXGIDevice1 {
    MAICORD_COM_UNUSED(OfferResources);
    MAICORD_COM_UNUSED(ReclaimResources);
    MAICORD_COM_UNUSED(EnqueueSetEvent);
};
enum DXGI_SCALING {
    DXGI_SCALING_STRETCH = 0,
    DXGI_SCALING_NONE = 1,
    DXGI_SCALING_ASPECT_RATIO_STRETCH = 2
};
struct DXGI_SWAP_CHAIN_DESC1 {
    UINT Width;
    UINT Height;
    DXGI_FORMAT Format;
    BOOL Stereo;
    DXGI_SAMPLE_DESC SampleDesc;
    DXGI_USAGE BufferUsage;
    UINT BufferCount;
    DXGI_SCALING Scaling;
    DXGI_SWAP_EFFECT SwapEffect;
    DXGI_ALPHA_MODE AlphaMode;
    UINT Flags;
};
struct DXGI_PRESENT_PARAMETERS {
    UINT DirtyRectsCount;
    RECT* pDirtyRects;
    RECT* pScrollRect;
    POINT* pScrollOffset;
};
struct __declspec(uuid("790a45f7-0d42-4876-983a-0a55cfe6f4aa")) __declspec(novtable) IDXGISwapChain1 : IDXGISwapChain {
    MAICORD_COM_UNUSED(GetDesc1);
    MAICORD_COM_UNUSED(GetFullscreenDesc);
    MAICORD_COM_UNUSED(GetHwnd);
    MAICORD_COM_UNUSED(GetCoreWindow);
    virtual HRESULT __stdcall Present1(UINT SyncInterval, UINT PresentFlags,
        const DXGI_PRESENT_PARAMETERS* pPresentParameters) = 0;
    MAICORD_COM_UNUSED(IsTemporaryMonoSupported);
    MAICORD_COM_UNUSED(GetRestrictToOutput);
    MAICORD_COM_UNUSED(SetBackgroundColor);
    MAICORD_COM_UNUSED(GetBackgroundColor);
    MAICORD_COM_UNUSED(SetRotation);
    MAICORD_COM_UNUSED(GetRotation);
};
struct __declspec(uuid("50c83a1c-e072-4c48-87b0-3630fa36a6d0")) __declspec(novtable) IDXGIFactory2 : IDXGIFactory1 {
    MAICORD_COM_UNUSED(IsWindowedStereoEnabled);
    MAICORD_COM_UNUSED(CreateSwapChainForHwnd);
    MAICORD_COM_UNUSED(CreateSwapChainForCoreWindow);
    MAICORD_COM_UNUSED(GetSharedResourceAdapterLuid);
    MAICORD_COM_UNUSED(RegisterStereoStatusWindow);
    MAICORD_COM_UNUSED(RegisterStereoStatusEvent);
    MAICORD_COM_UNUSED(UnregisterStereoStatus);
    MAICORD_COM_UNUSED(RegisterOcclusionStatusWindow);
    MAICORD_COM_UNUSED(RegisterOcclusionStatusEvent);
    MAICORD_COM_UNUSED(UnregisterOcclusionStatus);
    virtual HRESULT __stdcall CreateSwapChainForComposition(IUnknown* pDevice, const DXGI_SWAP_CHAIN_DESC1* pDesc,
        IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) = 0;
};
struct __declspec(uuid("0AA1AE0A-FA0E-4B84-8644-E05FF8E5ACB5")) __declspec(novtable) IDXGIAdapter2 : IDXGIAdapter1 {
    MAICORD_COM_UNUSED(GetDesc2);
};
// <dxgi1_3.h>
struct __declspec(uuid("6007896c-3244-4afd-bf18-a6d3beda5023")) __declspec(novtable) IDXGIDevice3 : IDXGIDevice2 {
    virtual void __stdcall Trim() = 0;
};
struct __declspec(uuid("a8be2ac4-199f-4946-b331-79599fb98de7")) __declspec(novtable) IDXGISwapChain2 : IDXGISwapChain1 {
    MAICORD_COM_UNUSED(SetSourceSize);
    MAICORD_COM_UNUSED(GetSourceSize);
    virtual HRESULT __stdcall SetMaximumFrameLatency(UINT MaxLatency) = 0;
    MAICORD_COM_UNUSED(GetMaximumFrameLatency);
    virtual HANDLE __stdcall GetFrameLatencyWaitableObject() = 0;
};
// <dxgi1_4.h>
enum DXGI_MEMORY_SEGMENT_GROUP {
    DXGI_MEMORY_SEGMENT_GROUP_LOCAL = 0,
    DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL = 1
};
struct DXGI_QUERY_VIDEO_MEMORY_INFO {
    UINT64 Budget;
    UINT64 CurrentUsage;
    UINT64 AvailableForReservation;
    UINT64 CurrentReservation;
};
struct __declspec(uuid("645967A4-1392-4310-A798-8053CE3E93FD")) __declspec(novtable) IDXGIAdapter3 : IDXGIAdapter2 {
    MAICORD_COM_UNUSED(RegisterHardwareContentProtectionTeardownStatusEvent);
    MAICORD_COM_UNUSED(UnregisterHardwareContentProtectionTeardownStatus);
    virtual HRESULT __stdcall QueryVideoMemoryInfo(UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
        DXGI_QUERY_VIDEO_MEMORY_INFO* pVideoMemoryInfo) = 0;
};
MAICORD_SYS_TYPES_END
