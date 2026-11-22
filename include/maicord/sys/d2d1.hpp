#pragma once

// Direct2D's common types (<dcommon.h>) that DirectComposition takes, declared as the SDK declares them; see
// maicord/sys/sdk.hpp.

#include "maicord/sys/sdk.hpp"
#include "maicord/sys/com.hpp"

#if defined(MAICORD_SDK_INCLUDES)
#include <Windows.h>
#include <d2d1.h>
#endif

MAICORD_SYS_TYPES_BEGIN
struct D2D_RECT_F {
    FLOAT left;
    FLOAT top;
    FLOAT right;
    FLOAT bottom;
};
struct D2D_MATRIX_3X2_F {
    union {
        struct {
            FLOAT m11;
            FLOAT m12;
            FLOAT m21;
            FLOAT m22;
            FLOAT dx;
            FLOAT dy;
        };
        struct {
            FLOAT _11, _12;
            FLOAT _21, _22;
            FLOAT _31, _32;
        };
        FLOAT m[3] [2];
    };
};
MAICORD_SYS_TYPES_END
