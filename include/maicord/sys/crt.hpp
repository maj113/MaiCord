#pragma once

// The CRT functions the compiler expands inline, declared as the SDK declares them; see
// maicord/sys/sdk.hpp. C as well as C++: maiopus includes it through sys/libc.

#include "maicord/sys/sdk.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <new>
#include <malloc.h>
#endif

// <vcruntime.h>
#if defined(__cplusplus)
#define NULL 0
#else
#define NULL ((void*)0)
#endif

typedef unsigned __int64 size_t;

#if defined(__cplusplus)
extern "C" {
#endif

// <vcruntime_string.h>
[[nodiscard]] int __cdecl memcmp(void const* _Buf1, void const* _Buf2, size_t _Size);
void* __cdecl memcpy(void* _Dst, void const* _Src, size_t _Size);
void* __cdecl memmove(void* _Dst, void const* _Src, size_t _Size);
void* __cdecl memset(void* _Dst, int _Val, size_t _Size);

// <stdlib.h>
[[nodiscard]] int __cdecl abs(int _Number);

// <malloc.h>
void* __cdecl _alloca(size_t _Size);

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
