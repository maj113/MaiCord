// An icon made from pixels, for the window and the shell integrations that need an HICON (win32.hpp).
#include "maicord/platform/window/win32.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/sys/gdi.hpp"

namespace maicord {
namespace {

constexpr u32 kIconMaximumExtent = 64;

} // namespace

HICON win32_create_icon(ConstSlice<u32> pixels, u32 extent) noexcept {
    if (!pixels.data || !extent || extent > kIconMaximumExtent || pixels.count < extent * extent)
        return nullptr;
    ScratchPages<u8[sizeof(BITMAPINFOHEADER) + kIconMaximumExtent * kIconMaximumExtent * 4 + kIconMaximumExtent * 8]>
        scratch;
    if (!scratch.data)
        return nullptr;
    u8* resource = *scratch.data;
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = static_cast<LONG>(extent);
    header.biHeight = static_cast<LONG>(extent * 2); // XOR image plus AND mask
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    // Fresh scratch pages are zero, so the AND mask is already clear.
    copy_bytes(resource, &header, sizeof(header));
    u8* rows = resource + sizeof(header);
    for (u32 y = 0; y < extent; ++y) // bottom-up
        copy_bytes(rows + (extent - 1 - y) * extent * 4, pixels.data + y * extent, extent * 4);
    const u32 mask_bytes = extent * ((extent + 31) / 32 * 4);
    const u32 total = static_cast<u32>(sizeof(header)) + extent * extent * 4 + mask_bytes;
    return CreateIconFromResourceEx(resource, total, TRUE, 0x00030000, static_cast<int>(extent),
                                    static_cast<int>(extent), LR_DEFAULTCOLOR);
}

} // namespace maicord
