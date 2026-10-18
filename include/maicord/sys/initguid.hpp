// <initguid.h>: DEFINE_GUID defines the GUIDs of the headers included after
// this one instead of declaring them. Like <initguid.h>, no include guard.

#include "maicord/sys/com.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <initguid.h>
#else
#define INITGUID
#undef DEFINE_GUID
#define DEFINE_GUID(name, l, w1, w2, b1, b2, b3, b4, b5, b6, b7, b8)                                                  \
    extern "C" const GUID __declspec(selectany) name = {l, w1, w2, {b1, b2, b3, b4, b5, b6, b7, b8}}
#endif
