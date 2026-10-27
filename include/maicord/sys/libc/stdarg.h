#pragma once

/* <stdarg.h> (maiopus), as the compiler declares it; see maicord/sys/sdk.hpp. */

#include "maicord/sys/sdk.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <stdarg.h>
#endif

typedef __builtin_va_list va_list;

#define va_start(ap, param) __builtin_va_start(ap, param)
#define va_end(ap) __builtin_va_end(ap)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
