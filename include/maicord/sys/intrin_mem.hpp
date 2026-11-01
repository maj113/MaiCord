#pragma once

// The string intrinsic memory.hpp uses, declared as the SDK declares it; see maicord/sys/sdk.hpp. Kept apart from
// intrin.hpp so the units that include memory.hpp (nearly all) do not parse the interlocked, bit-scan and
// processor-register declarations they never call.

#include "maicord/sys/sdk.hpp"
#include "maicord/sys/crt.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <intrin.h>
#endif

#if defined(__cplusplus)
extern "C" {
#endif

// <intrin.h>
void __stosb(unsigned char*, unsigned char, size_t);

#if defined(__cplusplus)
}
#endif
