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

} // namespace

ConstSlice<u8> program_resource(HMODULE, u32 id) noexcept {
    // The ids of config/notification_sounds.rc.in.
    switch (id) {
    case 20481:
        return {kMessageAlert, sizeof(kMessageAlert)};
    case 20482:
        return {kFocusedMessageAlert, sizeof(kFocusedMessageAlert)};
    default:
        return {};
    }
}

} // namespace maicord
