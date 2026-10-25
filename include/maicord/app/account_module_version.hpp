#pragma once
#include "maicord/core/base.hpp"

namespace maicord {

// Version of the AccountModuleApi layout and call semantics. The host refuses any other
// value: raise it whenever the interface or the meaning of a call changes.
inline constexpr u32 kAccountModuleApiVersion = 142;

} // namespace maicord
