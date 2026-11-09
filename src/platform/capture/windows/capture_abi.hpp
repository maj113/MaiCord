#pragma once

// The Windows Runtime and COM ABI that screen capture (Windows.Graphics.Capture) and stream audio (WASAPI process
// loopback) call, declared here instead of through the SDK projection headers, the way the toast path does
// (src/platform/desktop/toast_abi.hpp, whose object and call helpers this reuses): objects are opaque and called by
// vtable slot. The process loopback declarations are the SDK's from Windows 10 2004 on, newer than the
// _WIN32_WINNT the build targets, so they live here too. tests/platform/capture/capture_abi.cpp checks every IID,
// slot, class name, constant and layout against the SDK.
#include "../../desktop/toast_abi.hpp"
#include "maicord/sys/ole.hpp"

namespace maicord::capture_abi {

using toast_abi::Object;
using toast_abi::StringReference;
using toast_abi::call;
using toast_abi::query;
using toast_abi::release;

inline constexpr wchar_t kCaptureItemClass[] = L"Windows.Graphics.Capture.GraphicsCaptureItem";
inline constexpr wchar_t kFramePoolClass[] = L"Windows.Graphics.Capture.Direct3D11CaptureFramePool";
inline constexpr wchar_t kCaptureAccessClass[] = L"Windows.Graphics.Capture.GraphicsCaptureAccess";

// IGraphicsCaptureItemInterop (IUnknown): CreateForWindow(HWND, REFIID, void**), CreateForMonitor(HMONITOR, ...).
inline constexpr GUID kIidCaptureItemInterop{
    0x3628e81b, 0x3cac, 0x4c60, {0xb7, 0xf4, 0x23, 0xce, 0x0e, 0x0c, 0x33, 0x56}};
inline constexpr u32 kCreateForWindow = 3, kCreateForMonitor = 4;
inline constexpr GUID kIidCaptureItem{0x79c3f95b, 0x31f7, 0x4ec2, {0xa4, 0x64, 0x63, 0x2e, 0xf5, 0xd3, 0x07, 0x60}};
// IGraphicsCaptureItem::get_Size(SizeInt32*)
inline constexpr u32 kItemSize = 7;
// IDirect3D11CaptureFramePoolStatics2::CreateFreeThreaded(IDirect3DDevice*, DirectXPixelFormat, INT32 buffers,
// SizeInt32, IDirect3D11CaptureFramePool**): UniversalApiContract 7, Windows 10 1809.
inline constexpr GUID kIidFramePoolStatics2{
    0x589b103f, 0x6bbc, 0x5df5, {0xa9, 0x91, 0x02, 0xe2, 0x8b, 0x3b, 0x66, 0xd5}};
inline constexpr u32 kCreateFreeThreaded = 6;
// IDirect3D11CaptureFramePool: Recreate(device, format, buffers, size), TryGetNextFrame(frame**),
// add_FrameArrived(handler, token*), remove_FrameArrived(token), CreateCaptureSession(item, session**).
inline constexpr u32 kPoolRecreate = 6, kPoolTryGetNextFrame = 7, kPoolAddFrameArrived = 8,
                     kPoolRemoveFrameArrived = 9, kPoolCreateSession = 10;
// IDirect3D11CaptureFrame: get_Surface(IDirect3DSurface**), get_SystemRelativeTime(TimeSpan*),
// get_ContentSize(SizeInt32*).
inline constexpr u32 kFrameSurface = 6, kFrameTime = 7, kFrameContentSize = 8;
// IGraphicsCaptureSession::StartCapture(); Session2 (2004): put_IsCursorCaptureEnabled(boolean); Session3 (Windows 11,
// Server 2022): put_IsBorderRequired(boolean); Session5 (24H2): put_MinUpdateInterval(TimeSpan).
inline constexpr u32 kSessionStart = 6, kSessionPutCursor = 7, kSessionPutBorder = 7, kSessionPutMinInterval = 7;
inline constexpr GUID kIidCaptureSession2{
    0x2c39ae40, 0x7d2e, 0x5044, {0x80, 0x4e, 0x8b, 0x67, 0x99, 0xd4, 0xcf, 0x9e}};
inline constexpr GUID kIidCaptureSession3{
    0xf2cdd966, 0x22ae, 0x5ea1, {0x95, 0x96, 0x3a, 0x28, 0x93, 0x44, 0xc3, 0xbe}};
inline constexpr GUID kIidCaptureSession5{
    0x67c0ea62, 0x1f85, 0x5061, {0x92, 0x5a, 0x23, 0x9b, 0xe0, 0xac, 0x09, 0xcb}};
// IGraphicsCaptureAccessStatics::RequestAccessAsync(GraphicsCaptureAccessKind, IAsyncOperation<...>**).
inline constexpr GUID kIidCaptureAccessStatics{
    0x743ed370, 0x06ec, 0x5040, {0xa5, 0x8a, 0x90, 0x1f, 0x0f, 0x75, 0x70, 0x95}};
inline constexpr u32 kRequestAccess = 6;
inline constexpr i32 kAccessBorderless = 0; // GraphicsCaptureAccessKind_Borderless
// IAsyncInfo::get_Status(AsyncStatus*): 0 started, then completed, canceled or error.
inline constexpr GUID kIidAsyncInfo{0x00000036, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
inline constexpr u32 kAsyncStatus = 7;
inline constexpr i32 kAsyncStarted = 0;
// IClosable::Close()
inline constexpr GUID kIidClosable{0x30d5a829, 0x7fa4, 0x4026, {0x83, 0xbb, 0xd7, 0x5b, 0xae, 0x4e, 0xa9, 0x9e}};
inline constexpr u32 kClose = 6;
// IDirect3DDxgiInterfaceAccess (IUnknown)::GetInterface(REFIID, void**)
inline constexpr GUID kIidDxgiInterfaceAccess{
    0xa9b3d012, 0x3df2, 0x4ee3, {0xb8, 0xd1, 0x86, 0x95, 0xf4, 0x57, 0xd3, 0xc1}};
inline constexpr u32 kGetInterface = 3;
// TypedEventHandler<Direct3D11CaptureFramePool*, IInspectable*>: Invoke(sender, args) after IUnknown.
inline constexpr GUID kIidFrameArrivedHandler{
    0x51a947f7, 0x79cf, 0x5a3e, {0xa3, 0xa5, 0x12, 0x89, 0xcf, 0xa6, 0xdf, 0xe8}};
inline constexpr GUID kIidAgileObject{0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
// DirectXPixelFormat_B8G8R8A8UIntNormalized (DXGI_FORMAT_B8G8R8A8_UNORM).
inline constexpr i32 kPixelFormatBgra8 = 87;

// Windows.Graphics.SizeInt32 goes by value in one 64-bit register: width in the low half.
inline u64 size_argument(i32 width, i32 height) noexcept {
    return static_cast<u64>(static_cast<u32>(width)) | static_cast<u64>(static_cast<u32>(height)) << 32;
}
struct SizeInt32 {
    i32 width, height;
};

// d3d11.dll: CreateDirect3D11DeviceFromDXGIDevice(IDXGIDevice*, IInspectable**), resolved at run time.
using CreateDirect3DDeviceFunction = HRESULT(__stdcall*)(IUnknown* dxgi_device, Object** device) noexcept;

// <audioclientactivationparams.h> (Windows 10 2004): the activation parameters of a process loopback client.
inline constexpr wchar_t kProcessLoopbackDevice[] = L"VAD\\Process_Loopback";
inline constexpr u32 kActivationProcessLoopback = 1; // AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK
inline constexpr u32 kLoopbackIncludeTree = 0;       // PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
inline constexpr u32 kLoopbackExcludeTree = 1;       // PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
struct ActivationParams {
    u32 activation_type;
    u32 target_process_id;
    u32 loopback_mode;
};
// <mmdeviceapi.h>: IActivateAudioInterfaceCompletionHandler::ActivateCompleted(operation) and
// IActivateAudioInterfaceAsyncOperation::GetActivateResult(HRESULT*, IUnknown**), each the first slot after IUnknown.
inline constexpr GUID kIidActivateCompletionHandler{
    0x41d949ab, 0x9862, 0x444a, {0x80, 0xf6, 0xc2, 0x61, 0x33, 0x4d, 0xa5, 0xeb}};
inline constexpr u32 kGetActivateResult = 3;
// mmdevapi.dll: ActivateAudioInterfaceAsync, resolved at run time.
using ActivateAudioInterfaceFunction = HRESULT(__stdcall*)(const wchar_t* path, const IID& iid, PROPVARIANT* parameters,
                                                           IUnknown* handler, Object** operation) noexcept;

// A free-threaded COM callback object (an event handler or completion handler): IUnknown, IAgileObject (WinRT and
// WASAPI call it on their own threads, unmarshalled) and the one method, which sets `event`. It owns a duplicate of
// the event, so a call racing the owner's teardown never touches freed memory; the last Release frees it.
struct SignalCallback {
    void* const* vtable;
    volatile LONG references;
    HANDLE event;
    GUID iid;
};
// Makes one with a reference for the caller; null on failure.
SignalCallback* signal_callback_create(HANDLE event, const GUID& iid) noexcept;
inline void signal_callback_release(SignalCallback*& callback) noexcept {
    release(reinterpret_cast<Object*&>(callback));
}

} // namespace maicord::capture_abi
