#pragma once
#include "maicord/text/layout_tables.hpp"
namespace maicord {
// Prepared record lookup; membership needs no expanded coverage count.
u32 coverage_index(const u8* records,u16 count,u16 format,u16 glyph) noexcept;
}
