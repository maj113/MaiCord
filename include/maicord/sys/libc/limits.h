#pragma once

/* <limits.h> for /X builds, as the compiler declares it; see maicord/sys/sdk.hpp. */

#include "maicord/sys/sdk.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <limits.h>
#endif

#define INT_MAX 2147483647
#define UINT_MAX 0xffffffff
