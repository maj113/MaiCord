#pragma once

// The rarely used CRT functions, declared as the SDK declares them; see maicord/sys/sdk.hpp. Kept apart from
// crt.hpp, which nearly every unit includes, because each redeclaration of a compiler-known function costs
// parse time. C as well as C++: maiopus includes it through sys/libc.

#include "maicord/sys/sdk.hpp"
#include "maicord/sys/crt.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <malloc.h>
#endif

#if defined(__cplusplus)
extern "C" {
#endif

// <vcruntime_string.h>
[[nodiscard]] int __cdecl memcmp(void const* _Buf1, void const* _Buf2, size_t _Size);

// <stdlib.h>
[[nodiscard]] int __cdecl abs(int _Number);

// <malloc.h>
void* __cdecl _alloca(size_t _Size);

#if defined(__cplusplus)
}
#endif
