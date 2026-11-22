#include "notification_audio_system.hpp"

// always_inline: the link-time optimizer puts each call back into the worker (notification_audio.cpp), which the size
// option would otherwise leave as calls of four outlined functions.
namespace maicord::notification_audio_system {

__attribute__((always_inline)) HRESULT enter_apartment() noexcept {
    return CoInitializeEx(nullptr, COINIT_MULTITHREADED);
}

__attribute__((always_inline)) void leave_apartment() noexcept {
    CoUninitialize();
}

__attribute__((always_inline)) HRESULT create_enumerator(IMMDeviceEnumerator** enumerator) noexcept {
    return CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator),
                            reinterpret_cast<void**>(enumerator));
}

__attribute__((always_inline)) DWORD wait(u32 handle_count, const HANDLE* handles) noexcept {
    return WaitForMultipleObjects(handle_count, handles, FALSE, INFINITE);
}

} // namespace maicord::notification_audio_system
