#pragma once

#include "maicord/core/base.hpp"
#include "maicord/core/win32_types.hpp"

// LINK supplies the base of the image containing this translation unit, even
// when it came from a static library. This does not acquire a loader reference.
// IMAGE_DOS_HEADER, completed by <winnt.h> where a source parses it.
struct _IMAGE_DOS_HEADER;
extern "C" _IMAGE_DOS_HEADER __ImageBase;

namespace maicord {

inline HINSTANCE module_instance() noexcept {
    return reinterpret_cast<HINSTANCE>(&__ImageBase);
}

} // namespace maicord
