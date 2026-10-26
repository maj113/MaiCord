#pragma once

/* <stddef.h> for /X builds, as the compiler declares it (x64); see maicord/sys/sdk.hpp. */

#include "maicord/sys/sdk.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <stddef.h>
#endif

typedef unsigned __int64 size_t;

#define offsetof(t, d) __builtin_offsetof(t, d)
