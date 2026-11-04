#pragma once

/* <stddef.h> for -nostdinc builds, as the compiler declares it (x64); see maicord/sys/sdk.hpp. */

#include "maicord/sys/sdk.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <stddef.h>
#endif

#if defined(__linux__)
typedef __SIZE_TYPE__ size_t;
#else
typedef unsigned __int64 size_t;
#endif

#define offsetof(t, d) __builtin_offsetof(t, d)
