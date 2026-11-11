#include "maicord/core/memory.hpp"
#include "maicord/core/resource.hpp"
#include "../support/test_output.hpp"

// The program resources on Linux (src/core/linux/resource.cpp): the two notification sound assets are linked into the
// image and read by the ids of config/notification_sounds.rc.in; any other id, the icon group 1 of config/maicord.rc
// included (Linux draws the brand mark instead), is empty. (Windows reads the same ids from the RCDATA of the
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
    check(!program_resource(nullptr, 1).data && !program_resource(nullptr, 2).data && !program_resource(nullptr, 2).count,
          "an unknown id is empty");
    test_exit(g_failures, "linux_resource");
}
