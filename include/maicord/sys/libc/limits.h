#pragma once

/* <limits.h> for /X builds, as the compiler declares it; see maicord/sys/sdk.hpp. */

#include "maicord/sys/sdk.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <limits.h>
#endif

#define INT_MAX __INT_MAX__
#define UINT_MAX (__INT_MAX__ *2U +1U)
