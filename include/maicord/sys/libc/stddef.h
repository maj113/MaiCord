#pragma once

/* <stddef.h> for /X builds, as the CRT declares it (x64); see maicord/sys/sdk.hpp. */

#include "maicord/sys/sdk.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <stddef.h>
#endif

typedef unsigned __int64 size_t;

#if defined _MSC_VER && !defined _CRT_USE_BUILTIN_OFFSETOF
#ifdef __cplusplus
#define offsetof(s,m) ((::size_t)&reinterpret_cast<char const volatile&>((((s*)0)->m)))
#else
#define offsetof(s,m) ((size_t)&(((s*)0)->m))
#endif
#else
#define offsetof(s,m) __builtin_offsetof(s,m)
#endif
