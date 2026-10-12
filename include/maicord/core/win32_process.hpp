#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/win32_types.hpp"

// Declared as <intrin.h> declares them, without parsing it.
extern "C" unsigned __int64 __readgsqword(unsigned long offset);
extern "C" unsigned long __readgsdword(unsigned long offset);
#pragma intrinsic(__readgsqword, __readgsdword)

#if !defined(_M_X64)
#error MaiCord's TEB/PEB accessors require native Windows x64.
#endif

namespace maicord {

// Native Windows x64 ABI: GS:[0x60] is TEB.ProcessEnvironmentBlock;
// PEB+0x30 is ProcessHeap. These private offsets are isolated here and checked
// against the public APIs by MaiCordProcessBenchmark. Revalidate for a new ABI.
__forceinline HANDLE process_heap() noexcept {
    const auto* peb = reinterpret_cast<const u8*>(__readgsqword(0x60));
    return *reinterpret_cast<HANDLE const*>(peb + 0x30);
}

// TEB.ClientId.UniqueProcess. The Win32 API returns its low DWORD.
__forceinline u32 process_id() noexcept {
    return static_cast<u32>(__readgsqword(0x40));
}

// Mutable TEB.LastErrorValue: read at the failure site, never cache it.
__forceinline u32 thread_last_error() noexcept {
    return __readgsdword(0x68);
}

} // namespace maicord
