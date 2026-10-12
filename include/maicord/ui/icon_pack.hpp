#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/text/outline.hpp"

namespace maicord {

// MIPK v1, written by tools/generate_icon_pack.py. Coordinates are 1/64 units
// on a 24-unit grid. A pack is a borrowed byte view validated once on open.
inline constexpr u32 kIconPackGrid = 24;
inline constexpr u32 kIconPackUnits = 64;
inline constexpr u32 kIconPackMaximumIcons = 1024;
inline constexpr u32 kIconMaximumOutlineCommands = 512;
inline constexpr u32 kIconMaximumStems = 8;

struct IconPack {
    const u8* data;
    u32 size;
    u32 icon_count;
};

enum class IconPackError : u8 {
    none,
    invalid_input,
    header,
    layout,
    record,
    commands,
    stems,
    names,
    index,
};

// Grid-unit stems recorded by the compiler for translation-only pixel fitting.
struct IconStem {
    float position;
    u8 axis; // 0: vertical stem at x, 1: horizontal stem at y.
    u8 length;
};

// Pixel-space placement: x = unit * scale + offset_x.
struct IconOutlineTransform {
    float scale;
    float offset_x;
    float offset_y;
    bool fill_closed; // Closed stroked contours are also filled (solid variant).
};

IconPackError icon_pack_open(ConstSlice<u8> bytes, IconPack* pack) noexcept;
const IconPack& icon_pack_builtin() noexcept;
bool icon_pack_find(const IconPack& pack, Utf8View name, u32* index) noexcept;
Utf8View icon_pack_name(const IconPack& pack, u32 index) noexcept;
u32 icon_pack_outline_count(const IconPack& pack, u32 index) noexcept;
u32 icon_pack_stems(const IconPack& pack, u32 index, IconStem* stems, u32 capacity) noexcept;
// Writes begin/line/cubic/end commands into outline->commands (capacity bound).
bool icon_pack_decode(const IconPack& pack, u32 index, const IconOutlineTransform& transform,
                      GlyphOutline* outline) noexcept;

// Active icon set: the built-in pack, optionally overlaid by a theme or plugin
// pack. Icons the overlay lacks fall back to the built-in record by name. The
// overlay bytes are borrowed and must outlive the registration.
struct IconSource {
    const IconPack* pack;
    u32 index;
    bool overlay;
};
IconPackError icon_pack_set_overlay(ConstSlice<u8> bytes) noexcept;
void icon_pack_clear_overlay() noexcept;
u32 icon_pack_generation() noexcept;
bool icon_pack_resolve(u32 builtin_index, IconSource* source) noexcept;

} // namespace maicord
