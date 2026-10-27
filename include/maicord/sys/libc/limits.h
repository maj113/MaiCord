#pragma once

/* <limits.h> for -nostdinc builds, as the compiler declares it; see maicord/sys/sdk.hpp. */

#include "maicord/sys/sdk.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <limits.h>
#endif

#define CHAR_BIT __CHAR_BIT__
#define INT_MAX __INT_MAX__
#define UINT_MAX (__INT_MAX__ *2U +1U)
