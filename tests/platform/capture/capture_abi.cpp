// Checks src/platform/capture/windows/capture_abi.hpp against the SDK's headers: the interface IDs screen capture and
// stream audio query or implement, the vtable slot of every method they call, the runtime class names, the constants
// and the process loopback activation parameters' layout. Compiled against the SDK (MAICORD_SDK_INCLUDES).
// The process loopback declarations are behind NTDDI_WIN10_FE in the SDK; the build's _WIN32_WINNT names Windows 10.
#define NTDDI_VERSION 0x0A00000A
#include "../../../src/platform/capture/windows/capture_abi.hpp"
#include "maicord/core/memory.hpp"
#include <Windows.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <windows.graphics.capture.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace maicord {
namespace {
namespace sdk = ABI::Windows::Graphics::Capture;
namespace local = capture_abi;

using SlotFunction = HRESULT(STDMETHODCALLTYPE*)(void*);
template <int Slot> HRESULT STDMETHODCALLTYPE slot_index(void*) noexcept {
    return Slot;
}
template <int Count> struct SlotTable {
    static void fill(SlotFunction* table) noexcept {
        table[Count - 1] = &slot_index<Count - 1>;
        SlotTable<Count - 1>::fill(table);
    }
};
template <> struct SlotTable<0> {
    static void fill(SlotFunction*) noexcept {
    }
};
struct SlotObject {
    SlotFunction* vtable;
};
template <class T> T* as(SlotObject& object) noexcept {
    return reinterpret_cast<T*>(&object);
}
bool at(HRESULT reached, u32 slot) noexcept {
    return reached == static_cast<HRESULT>(slot);
}
template <class Sdk> bool same_iid(const GUID& local_iid) noexcept {
    return bytes_equal(&local_iid, &__uuidof(Sdk), sizeof(GUID));
}
template <usize N, usize M> bool same_name(const wchar_t (&local_name)[N], const wchar_t (&sdk_name)[M]) noexcept {
    return N == M && bytes_equal(local_name, sdk_name, N * sizeof(wchar_t));
}

static_assert(local::kPixelFormatBgra8 ==
              static_cast<i32>(ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized));
static_assert(local::kAccessBorderless == static_cast<i32>(sdk::GraphicsCaptureAccessKind_Borderless));
static_assert(local::kAsyncStarted == static_cast<i32>(ABI::Windows::Foundation::AsyncStatus::Started));
static_assert(local::kActivationProcessLoopback == AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK &&
              local::kLoopbackIncludeTree == PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE &&
              local::kLoopbackExcludeTree == PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE);
static_assert(sizeof(local::ActivationParams) == sizeof(AUDIOCLIENT_ACTIVATION_PARAMS) &&
              __builtin_offsetof(local::ActivationParams, target_process_id) ==
                  __builtin_offsetof(AUDIOCLIENT_ACTIVATION_PARAMS, ProcessLoopbackParams.TargetProcessId) &&
              __builtin_offsetof(local::ActivationParams, loopback_mode) ==
                  __builtin_offsetof(AUDIOCLIENT_ACTIVATION_PARAMS, ProcessLoopbackParams.ProcessLoopbackMode));
static_assert(sizeof(local::SizeInt32) == sizeof(ABI::Windows::Graphics::SizeInt32));
} // namespace

void test_capture_abi(void (*check)(bool, const char*) noexcept) noexcept {
    using FrameArrived = ABI::Windows::Foundation::ITypedEventHandler<sdk::Direct3D11CaptureFramePool*, IInspectable*>;
    check(same_iid<IGraphicsCaptureItemInterop>(local::kIidCaptureItemInterop) &&
              same_iid<sdk::IGraphicsCaptureItem>(local::kIidCaptureItem) &&
              same_iid<sdk::IDirect3D11CaptureFramePoolStatics2>(local::kIidFramePoolStatics2) &&
              same_iid<sdk::IGraphicsCaptureSession2>(local::kIidCaptureSession2) &&
              same_iid<sdk::IGraphicsCaptureSession3>(local::kIidCaptureSession3) &&
              same_iid<sdk::IGraphicsCaptureSession5>(local::kIidCaptureSession5) &&
              same_iid<sdk::IGraphicsCaptureAccessStatics>(local::kIidCaptureAccessStatics) &&
              same_iid<IAsyncInfo>(local::kIidAsyncInfo) &&
              same_iid<ABI::Windows::Foundation::IClosable>(local::kIidClosable) &&
              same_iid<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>(
                  local::kIidDxgiInterfaceAccess) &&
              same_iid<FrameArrived>(local::kIidFrameArrivedHandler) && same_iid<IAgileObject>(local::kIidAgileObject) &&
              same_iid<IActivateAudioInterfaceCompletionHandler>(local::kIidActivateCompletionHandler),
          "capture ABI: interface IDs match the SDK");
    check(same_name(local::kCaptureItemClass, RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem) &&
              same_name(local::kFramePoolClass, RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool) &&
              same_name(local::kCaptureAccessClass, RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureAccess) &&
              same_name(local::kProcessLoopbackDevice, VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK),
          "capture ABI: runtime class names and the loopback device path match the SDK");

    SlotFunction table[24]{};
    SlotTable<24>::fill(table);
    SlotObject object{table};
    auto* interop = as<IGraphicsCaptureItemInterop>(object);
    check(at(interop->CreateForWindow(nullptr, __uuidof(IUnknown), nullptr), local::kCreateForWindow) &&
              at(interop->CreateForMonitor(nullptr, __uuidof(IUnknown), nullptr), local::kCreateForMonitor),
          "capture ABI: IGraphicsCaptureItemInterop slots");
    check(at(as<sdk::IGraphicsCaptureItem>(object)->get_Size(nullptr), local::kItemSize),
          "capture ABI: IGraphicsCaptureItem slots");
    check(at(as<sdk::IDirect3D11CaptureFramePoolStatics2>(object)->CreateFreeThreaded(
                 nullptr, ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized, 2, {1, 1}, nullptr),
             local::kCreateFreeThreaded),
          "capture ABI: IDirect3D11CaptureFramePoolStatics2 slots");
    auto* pool = as<sdk::IDirect3D11CaptureFramePool>(object);
    EventRegistrationToken token{};
    check(at(pool->Recreate(nullptr, ABI::Windows::Graphics::DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized, 2,
                            {1, 1}),
             local::kPoolRecreate) &&
              at(pool->TryGetNextFrame(nullptr), local::kPoolTryGetNextFrame) &&
              at(pool->add_FrameArrived(nullptr, &token), local::kPoolAddFrameArrived) &&
              at(pool->remove_FrameArrived(token), local::kPoolRemoveFrameArrived) &&
              at(pool->CreateCaptureSession(nullptr, nullptr), local::kPoolCreateSession),
          "capture ABI: IDirect3D11CaptureFramePool slots");
    auto* frame = as<sdk::IDirect3D11CaptureFrame>(object);
    check(at(frame->get_Surface(nullptr), local::kFrameSurface) &&
              at(frame->get_SystemRelativeTime(nullptr), local::kFrameTime) &&
              at(frame->get_ContentSize(nullptr), local::kFrameContentSize),
          "capture ABI: IDirect3D11CaptureFrame slots");
    check(at(as<sdk::IGraphicsCaptureSession>(object)->StartCapture(), local::kSessionStart) &&
              at(as<sdk::IGraphicsCaptureSession2>(object)->put_IsCursorCaptureEnabled(false), local::kSessionPutCursor) &&
              at(as<sdk::IGraphicsCaptureSession3>(object)->put_IsBorderRequired(false), local::kSessionPutBorder) &&
              at(as<sdk::IGraphicsCaptureSession5>(object)->put_MinUpdateInterval({1}), local::kSessionPutMinInterval),
          "capture ABI: IGraphicsCaptureSession slots");
    check(at(as<sdk::IGraphicsCaptureAccessStatics>(object)->RequestAccessAsync(sdk::GraphicsCaptureAccessKind_Borderless,
                                                                                   nullptr),
             local::kRequestAccess),
          "capture ABI: IGraphicsCaptureAccessStatics slots");
    check(at(as<IAsyncInfo>(object)->get_Status(nullptr), local::kAsyncStatus) &&
              at(as<ABI::Windows::Foundation::IClosable>(object)->Close(), local::kClose),
          "capture ABI: IAsyncInfo and IClosable slots");
    check(at(as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>(object)->GetInterface(
                 __uuidof(IUnknown), nullptr),
             local::kGetInterface) &&
              at(as<IActivateAudioInterfaceAsyncOperation>(object)->GetActivateResult(nullptr, nullptr),
                 local::kGetActivateResult),
          "capture ABI: IDirect3DDxgiInterfaceAccess and IActivateAudioInterfaceAsyncOperation slots");
    // The callback object's one method is the first after IUnknown, as Invoke and ActivateCompleted are.
    check(at(as<FrameArrived>(object)->Invoke(nullptr, nullptr), 3) &&
              at(as<IActivateAudioInterfaceCompletionHandler>(object)->ActivateCompleted(nullptr), 3),
          "capture ABI: the callbacks' method is slot 3");
    check(local::size_argument(1920, 1080) == (u64{1080} << 32 | 1920), "capture ABI: SizeInt32 by value, width low");
}

} // namespace maicord
