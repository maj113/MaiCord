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

// <dxgiformat.h>
enum DXGI_FORMAT {
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
    DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    DXGI_FORMAT_R32_TYPELESS = 39,
    DXGI_FORMAT_R32_UINT = 42,
    DXGI_FORMAT_R8G8_UNORM = 49,
    DXGI_FORMAT_R8_UNORM = 61,
    DXGI_FORMAT_B8G8R8A8_UNORM = 87,
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB = 91,
    DXGI_FORMAT_NV12 = 103,
    DXGI_FORMAT_P010 = 104,
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
struct DXGI_OUTPUT_DESC;
enum DXGI_SWAP_EFFECT {
    DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL = 3,
};
enum DXGI_SWAP_CHAIN_FLAG {
    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT = 64,
};
struct DXGI_SWAP_CHAIN_DESC;
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
    MAICORD_COM_UNUSED(SetMaximumFrameLatency);
    MAICORD_COM_UNUSED(GetMaximumFrameLatency);
};
// <dxgi1_2.h>
enum DXGI_ALPHA_MODE {
    DXGI_ALPHA_MODE_PREMULTIPLIED = 1,
    DXGI_ALPHA_MODE_FORCE_DWORD = 0xffffffff
};
struct __declspec(uuid("05008617-fbfd-4051-a790-144884b4f6a9")) __declspec(novtable) IDXGIDevice2 : IDXGIDevice1 {
    MAICORD_COM_UNUSED(OfferResources);
    MAICORD_COM_UNUSED(ReclaimResources);
    MAICORD_COM_UNUSED(EnqueueSetEvent);
};
enum DXGI_SCALING {
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
    MAICORD_COM_UNUSED(SetMaximumFrameLatency);
    virtual HRESULT __stdcall GetMaximumFrameLatency(UINT* pMaxLatency) = 0;
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
