#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/win32_types.hpp"

#if defined(_WIN32)
// LINK supplies the base of the image containing this translation unit, even
// when it came from a static library. This does not acquire a loader reference.
// IMAGE_DOS_HEADER, completed by <winnt.h> where a source parses it.
struct _IMAGE_DOS_HEADER;
extern "C" _IMAGE_DOS_HEADER __ImageBase;
#else
// The ELF header the linker places at the base of the image.
extern "C" const char __ehdr_start[];
#endif

namespace maicord {

#if defined(_WIN32)
inline HINSTANCE module_instance() noexcept {
    return reinterpret_cast<HINSTANCE>(&__ImageBase);
}
#endif

// The image containing this translation unit as an opaque module handle: what the account host and the resource
// readers take for "this program". No loader reference is acquired.
inline HMODULE module_image() noexcept {
#if defined(_WIN32)
    return reinterpret_cast<HMODULE>(&__ImageBase);
#else
    return const_cast<char*>(__ehdr_start);
#endif
}

} // namespace maicord
