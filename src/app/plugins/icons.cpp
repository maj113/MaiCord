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
struct IconName { u16 offset, bytes, id; };
static_assert(sizeof(IconName) == 6);
struct IconNames { IconName entries[static_cast<u32>(UiIcon::count)]; };
consteval IconNames icon_names() noexcept {
    IconNames names{};
    u32 at = 0, id = 0;
    for (auto& entry : names.entries) {
        entry.offset = static_cast<u16>(at);
        entry.id = static_cast<u16>(id++);
        while (kIconNames[at]) ++at;
        entry.bytes = static_cast<u16>(at - entry.offset);
        ++at;
    }
    // Stable length ordering preserves declaration order among possible duplicates.
    for (u32 i = 1; i < static_cast<u32>(UiIcon::count); ++i) {
        const auto value = names.entries[i];
        u32 j = i;
        while (j && names.entries[j - 1].bytes > value.bytes) {
            names.entries[j] = names.entries[j - 1];
            --j;
        }
        names.entries[j] = value;
    }
    return names;
}
static_assert(sizeof(kIconNames) < 65536);
constexpr auto kIconNameIndex = icon_names();

} // namespace

u16 plugin_icon(Utf8View name) noexcept {
    u32 first = 0, last = static_cast<u32>(UiIcon::count);
    while (first < last) {
        const u32 middle = first + (last - first) / 2;
        if (kIconNameIndex.entries[middle].bytes < name.bytes) first = middle + 1;
        else last = middle;
    }
    for (u32 i = first; i < static_cast<u32>(UiIcon::count); ++i) {
        const auto& entry = kIconNameIndex.entries[i];
        if (entry.bytes != name.bytes) break;
        if (bytes_equal(kIconNames + entry.offset, name.data, name.bytes)) return entry.id;
    }
    return plugin_surfaces_detail::kNoIcon;
}

} // namespace maicord
