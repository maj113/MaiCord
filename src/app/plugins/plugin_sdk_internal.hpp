#pragma once
// Shared by plugin_sdk.cpp and its parts (plugin_sdk_*.cpp); private to them.
// Plugin SDK host, ABI 5: lifecycle, settings pages and keybinds, commands,
// pre-send hooks, storage, timers and interface lookup. Menus, slots and
// decorators live in sdk_surfaces.cpp; read, events, actions and
// effects in sdk_data.cpp.
#include "sdk_internal.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/color_hex.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/hash.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace plugin_sdk_detail {}
using namespace plugin_sdk_detail;
namespace plugin_sdk_detail {

using namespace plugin_sdk;

bool store_save(NativePluginSdkBinding& binding) noexcept;
extern const mc_store_v1 kStore;

} // namespace plugin_sdk_detail
} // namespace maicord
