#pragma once

#include "maicord/sys/wasapi.hpp"

// The COM and wait calls of the Windows notification audio worker, in a source of their own
// (notification_audio_system.cpp) that tests/platform/audio/native_notification_audio_faults.cpp replaces at link time.
namespace maicord::notification_audio_system {

HRESULT enter_apartment() noexcept;
void leave_apartment() noexcept;
HRESULT create_enumerator(IMMDeviceEnumerator** enumerator) noexcept;
DWORD wait(u32 handle_count, const HANDLE* handles) noexcept;

} // namespace maicord::notification_audio_system
