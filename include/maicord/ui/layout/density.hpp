#pragma once

#include "maicord/core/base.hpp"

namespace maicord {

// Appearance > UI Density (appearance.ui_density): the space between server, channel and member list rows,
// process wide like the hour cycle. Standard is the official client's Default (and its value while unset).
enum class UiDensity : u8 { standard, compact, spacious };
inline UiDensity g_ui_density = UiDensity::standard;

// A channel row's head (and a voice channel's, above its users): the name's line (20, 24 or 28 DIPs) with 4 DIPs
// of padding and a 1-DIP gap above and below.
constexpr float ui_density_channel_head(UiDensity density) noexcept {
    return density == UiDensity::compact ? 30.0f : density == UiDensity::spacious ? 38.0f : 34.0f;
}
// A member or direct message list item: 38, 42 or 48 DIPs.
constexpr float ui_density_list_item(UiDensity density) noexcept {
    return density == UiDensity::compact ? 38.0f : density == UiDensity::spacious ? 48.0f : 42.0f;
}

} // namespace maicord
