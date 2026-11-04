#pragma once

/* <stdint.h> for -nostdinc builds, as the compiler declares it; see maicord/sys/sdk.hpp. */

#include "maicord/sys/sdk.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <stdint.h>
#endif

#if defined(__linux__)
/* clang on Linux defines __STDC_VERSION__, so maiopus's opus_types.h takes the real-<stdint.h> branch. */
typedef signed char int8_t;
#endif
typedef short int16_t;
typedef int int32_t;
typedef long long int64_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
