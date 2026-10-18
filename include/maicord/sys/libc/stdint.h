#pragma once

/* <stdint.h> for /X builds, as the compiler declares it; see maicord/sys/sdk.hpp. */

#include "maicord/sys/sdk.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <stdint.h>
#endif

typedef short int16_t;
typedef int int32_t;
typedef long long int64_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;

#define INT64_MIN (-9223372036854775807i64 - 1)
#define UINT32_MAX 0xffffffffui32
#define UINT64_MAX 0xffffffffffffffffui64
