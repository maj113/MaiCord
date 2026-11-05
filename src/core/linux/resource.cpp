// core/resource.hpp on Linux: the data the Windows build keeps as RCDATA resources, linked into the image with #embed
// (the file paths are relative to this file).
#include "maicord/core/resource.hpp"

namespace maicord {
namespace {

constexpr u8 kMessageAlert[] = {
#embed "../../../assets/sounds/message-alert.mcp"
};
constexpr u8 kFocusedMessageAlert[] = {
#embed "../../../assets/sounds/focused-message-alert.mcp"
};
// The icon file generated at build time (tools/generate_app_icon.py): app_icon.inc beside it is its #embed (a quoted
// #embed is looked up beside the file that names it, not on the include path).
constexpr u8 kApplicationIcon[] = {
#include "app_icon.inc"
};
static_assert(sizeof(kApplicationIcon) > 22, "the generated icon did not embed");

} // namespace

ConstSlice<u8> program_resource(HMODULE, u32 id) noexcept {
    // The ids of config/notification_sounds.rc.in, and the icon group of config/maicord.rc.
    switch (id) {
    case 1:
        return {kApplicationIcon, sizeof(kApplicationIcon)};
    case 20481:
        return {kMessageAlert, sizeof(kMessageAlert)};
    case 20482:
        return {kFocusedMessageAlert, sizeof(kFocusedMessageAlert)};
    default:
        return {};
    }
}

} // namespace maicord
