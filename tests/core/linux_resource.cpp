#include "maicord/core/memory.hpp"
#include "maicord/core/resource.hpp"
#include "../support/test_output.hpp"

// The program resources on Linux (src/core/linux/resource.cpp): the executable's icon (an ICO file, id 1 as in
// config/maicord.rc) and the two notification sound assets are linked into the image and read by the ids of
// config/notification_sounds.rc.in; any other id is empty. (Windows reads the same ids from the RCDATA of the
// executable, which only the application's own image carries.)

namespace maicord {
namespace {

u32 g_failures = 0;

void check(bool valid, const char* label) noexcept {
    test_check(&g_failures, valid, label);
}

} // namespace
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_linux_resource_entry) {
    using namespace maicord;
    const u32 ids[] = {20481u, 20482u};
    for (const u32 id : ids) {
        const ConstSlice<u8> data = program_resource(nullptr, id);
        check(data.data && data.count > 16 && bytes_equal(data.data, "MCSP", 4),
              "a notification sound resource starts with the MCSP magic");
    }
    const ConstSlice<u8> first = program_resource(nullptr, 20481), second = program_resource(nullptr, 20482);
    check(first.data != second.data, "the two sounds are different resources");
    check(program_resource(nullptr, 20481).data == first.data, "a resource is the same bytes on every call");
    // The program's icon (resource group 1 of config/maicord.rc): an ICO file whose images are PNGs.
    const ConstSlice<u8> icon = program_resource(nullptr, 1);
    const u32 images = icon.data && icon.count >= 6 ? icon.data[4] | icon.data[5] << 8 : 0;
    bool valid = images != 0 && icon.data[0] == 0 && icon.data[1] == 0 && icon.data[2] == 1 && icon.data[3] == 0 &&
                 icon.count >= 6 + images * 16;
    for (u32 i = 0; valid && i < images; ++i) {
        const u8* entry = icon.data + 6 + i * 16;
        const u32 bytes = entry[8] | entry[9] << 8 | entry[10] << 16 | static_cast<u32>(entry[11]) << 24;
        const u32 offset = entry[12] | entry[13] << 8 | entry[14] << 16 | static_cast<u32>(entry[15]) << 24;
        valid = offset <= icon.count && bytes <= icon.count - offset && bytes > 8 &&
                bytes_equal(icon.data + offset, "\x89PNG\r\n\x1a\n", 8);
    }
    check(valid, "the icon is an ICO file of PNG images");
    check(program_resource(nullptr, 2).data == nullptr && program_resource(nullptr, 2).count == 0, "an unknown id is empty");
    test_exit(g_failures, "linux_resource");
}
