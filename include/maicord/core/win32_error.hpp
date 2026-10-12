#pragma once

#include "maicord/core/base.hpp"

#include <winerror.h>

namespace maicord {

constexpr bool win32_error_is_missing_file(u32 error) noexcept {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

} // namespace maicord
