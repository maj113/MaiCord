// The icon names plugins give their buttons and decoration nodes ("lucide/star") as UiIcon values.
#include "surfaces_internal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/widgets/icon.hpp"

namespace maicord {
namespace {

constexpr char kIconNames[] =
#define MAICORD_ICON(name, source) source "\0"
#include "maicord/ui/widgets/icon_ids.inc"
#undef MAICORD_ICON
;

} // namespace

// Cold (a plugin names an icon once per button or decoration): a scan of the 150-odd names in declaration
// order, which is also the order the first of two equal names wins.
u16 plugin_icon(Utf8View name) noexcept {
    const char* at = kIconNames;
    for (u32 id = 0; id < static_cast<u32>(UiIcon::count); ++id) {
        u32 bytes = 0;
        while (at[bytes]) ++bytes;
        if (bytes == name.bytes && bytes_equal(at, name.data, bytes)) return static_cast<u16>(id);
        at += bytes + 1;
    }
    return plugin_surfaces_detail::kNoIcon;
}

} // namespace maicord
