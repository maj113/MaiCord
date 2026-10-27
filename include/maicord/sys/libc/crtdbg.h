#pragma once

/* <crtdbg.h> (maiopus's SILK assertions), as the SDK declares it without _DEBUG; see
   maicord/sys/sdk.hpp. */

#include "maicord/sys/sdk.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <crtdbg.h>
#endif

#define _ASSERTE(expr) ((void)0)
