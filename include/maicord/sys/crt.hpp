#pragma once

// The CRT functions the compiler expands inline (memcpy, memmove, memset), declared as the SDK declares them; see
// maicord/sys/sdk.hpp. C as well as C++: maiopus includes it through sys/libc. memcmp, abs and _alloca are in
// crt_extra.hpp.

#include "maicord/sys/sdk.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <string.h>
#include <new>
#endif

// <vcruntime.h>
#if defined(__cplusplus)
#define NULL 0
#else
#define NULL ((void*)0)
#endif

#if defined(__linux__)
typedef __SIZE_TYPE__ size_t;
#else
typedef unsigned __int64 size_t;
#endif

#if defined(__cplusplus)
extern "C" {
#endif

// <vcruntime_string.h>
void* __cdecl memcpy(void* _Dst, void const* _Src, size_t _Size);
void* __cdecl memmove(void* _Dst, void const* _Src, size_t _Size);
void* __cdecl memset(void* _Dst, int _Val, size_t _Size);

#if defined(__cplusplus)
}

// <vcruntime_new.h>: placement new.
#ifndef __PLACEMENT_NEW_INLINE
#define __PLACEMENT_NEW_INLINE
[[nodiscard]] inline void* __cdecl operator new(size_t _Size, void* _Where) noexcept {
    (void)_Size;
    return _Where;
}
#endif
#endif
