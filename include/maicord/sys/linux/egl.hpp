#pragma once

// EGL 1.5 as libEGL.so.1 declares it, without <EGL/egl.h>: the types and values the GPU backend uses and the table
// of entry points it loads at run time (maicord/gpu/gles). Add what you use together with its entry in the table.

#include "maicord/core/base.hpp"

namespace maicord::egl {

using Display = void*;
using Config = void*;
using Context = void*;
using Surface = void*;
using Int = i32;
using Boolean = u32;
using Enum = u32;

inline constexpr Int kFalse = 0;
inline constexpr Int kTrue = 1;
inline constexpr Int kNone = 0x3038;
inline constexpr Int kExtensions = 0x3055;
inline constexpr Int kVendor = 0x3053;
inline constexpr Int kVersion = 0x3054;
inline constexpr Int kRedSize = 0x3024;
inline constexpr Int kGreenSize = 0x3023;
inline constexpr Int kBlueSize = 0x3022;
inline constexpr Int kAlphaSize = 0x3021;
inline constexpr Int kSurfaceType = 0x3033;
inline constexpr Int kPbufferBit = 0x0001;
inline constexpr Int kRenderableType = 0x3040;
inline constexpr Int kOpenGlEs3Bit = 0x00000040;
inline constexpr Int kWidth = 0x3057;
inline constexpr Int kHeight = 0x3056;
inline constexpr Enum kOpenGlEsApi = 0x30A0;
inline constexpr Int kContextMajorVersion = 0x3098;
inline constexpr Int kContextMinorVersion = 0x30FB;
inline constexpr Int kPlatformSurfacelessMesa = 0x31DD;
inline constexpr Int kPresentOpaqueExt = 0x31DF;
inline constexpr Int kPlatformWaylandKhr = 0x31D8;
inline constexpr Int kWindowBit = 0x0004;
inline constexpr Int kBufferAgeExt = 0x313D;
inline constexpr Display kNoDisplay = nullptr;
inline constexpr Context kNoContext = nullptr;
inline constexpr Surface kNoSurface = nullptr;

// The functions the backend loads from libEGL.so.1 by name. eglGetPlatformDisplay is EGL 1.5; the loader falls back
// to eglGetPlatformDisplayEXT through eglGetProcAddress.
#define MAICORD_EGL_FUNCTIONS(X)                                                                                       \
    X(Int, eglGetError, ())                                                                                            \
    X(void*, eglGetProcAddress, (const char* name))                                                                    \
    X(Display, eglGetDisplay, (void* native_display))                                                                  \
    X(Display, eglGetPlatformDisplay, (Enum platform, void* native_display, const void* attributes))                   \
    X(Boolean, eglInitialize, (Display display, Int * major, Int * minor))                                             \
    X(Boolean, eglTerminate, (Display display))                                                                        \
    X(const char*, eglQueryString, (Display display, Int name))                                                        \
    X(Boolean, eglBindAPI, (Enum api))                                                                                 \
    X(Boolean, eglChooseConfig, (Display display, const Int* attributes, Config* configs, Int capacity, Int* count))   \
    X(Boolean, eglGetConfigAttrib, (Display display, Config config, Int attribute, Int * value))                       \
    X(Context, eglCreateContext, (Display display, Config config, Context share, const Int* attributes))               \
    X(Surface, eglCreatePbufferSurface, (Display display, Config config, const Int* attributes))                       \
    X(Boolean, eglMakeCurrent, (Display display, Surface draw, Surface read, Context context))                         \
    X(Boolean, eglDestroyContext, (Display display, Context context))                                                  \
    X(Boolean, eglDestroySurface, (Display display, Surface surface))                                                  \
    X(Surface, eglCreateWindowSurface, (Display display, Config config, void* native_window, const Int* attributes))   \
    X(Boolean, eglSwapBuffers, (Display display, Surface surface))                                                     \
    X(Boolean, eglSwapInterval, (Display display, Int interval))                                                       \
    X(Boolean, eglQuerySurface, (Display display, Surface surface, Int attribute, Int* value))

struct Api {
#define MAICORD_EGL_MEMBER(result, name, arguments) result(*name) arguments;
    MAICORD_EGL_FUNCTIONS(MAICORD_EGL_MEMBER)
#undef MAICORD_EGL_MEMBER
    // Optional (EGL_KHR_swap_buffers_with_damage or EGL_EXT_swap_buffers_with_damage): rectangles are x, y, width,
    // height with a bottom-left origin.
    Boolean (*eglSwapBuffersWithDamage)(Display display, Surface surface, const Int* rectangles, Int count);
};

} // namespace maicord::egl
