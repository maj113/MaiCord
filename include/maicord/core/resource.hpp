#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/core/module.hpp"

#if defined(_WIN32)
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#endif

// Data that ships inside the executable and is read by a numeric id: the RCDATA resources of the PE image on Windows
// (inline, config/notification_sounds.rc.in lists them), tables of data linked into the ELF image on Linux
// (src/core/linux/resource.cpp). The bytes are read-only and live as long as the image.

namespace maicord {

#if defined(_WIN32)
// The resource `id` of `module` (null: this program), empty when there is none.
inline ConstSlice<u8> program_resource(HMODULE module, u32 id) noexcept {
    if (!module)
        module = module_image();
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    const u32 count = resource ? SizeofResource(module, resource) : 0;
    HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
    const auto* data = loaded ? static_cast<const u8*>(LockResource(loaded)) : nullptr;
    return data ? ConstSlice<u8>{data, count} : ConstSlice<u8>{};
}
#else
// The resource `id`; `module` is ignored (the image has one table), empty when there is none.
ConstSlice<u8> program_resource(HMODULE module, u32 id) noexcept;
#endif

} // namespace maicord
