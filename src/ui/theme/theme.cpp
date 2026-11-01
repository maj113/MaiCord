#include "maicord/core/color_hex.hpp"
#include "maicord/ui/theme/presets.hpp"
#include "maicord/ui/theme/styles.hpp"

namespace maicord {

constinit const Theme kDefaultTheme = ui_theme_default_preset();
constinit const Theme kLightTheme = ui_theme_light_preset();

Theme ui_theme_amoled() noexcept {
    Theme theme = kDefaultTheme;
    ui_theme_amoled_surfaces(theme);
    ui_theme_tokens_resolve(&theme, nullptr);
    return theme;
}

__declspec(noinline) bool background_theme_valid(const BackgroundTheme& value) noexcept {
    if (value.color_count < 1 || value.color_count > 5 || value.angle_degrees > 360 ||
        value.intensity > 100 || !background_effect_settings_valid(background_effect_settings(value)) ||
        static_cast<u8>(value.effect) >= kBackgroundEffectCount)
        return false;
    for (u32 i = 0; i < 5; ++i)
        if (value.colors[i] > 0xffffff || (i >= value.color_count && value.colors[i]))
            return false;
    return true;
}

bool background_theme_parse_colors(Utf8View text, BackgroundTheme* output) noexcept {
    if (!output || !text.data || text.bytes > 128)
        return false;
    BackgroundTheme next = *output;
    for (auto& color : next.colors)
        color = 0;
    u32 offset = 0, count = 0;
    const auto space = [](char c) noexcept { return c == ' ' || c == '\t'; };
    while (offset < text.bytes) {
        while (offset < text.bytes && space(text.data[offset]))
            ++offset;
        if (offset == text.bytes || count == 5)
            return false;
        const u32 start = offset;
        while (offset < text.bytes && !space(text.data[offset]) && text.data[offset] != ',')
            ++offset;
        u32 rgb24 = 0;
        if (!color_hex_parse_rgb24({text.data + start, offset - start}, &rgb24))
            return false;
        next.colors[count++] = rgb24;
        while (offset < text.bytes && space(text.data[offset]))
            ++offset;
        if (offset == text.bytes)
            break;
        if (text.data[offset++] != ',' || offset == text.bytes)
            return false;
    }
    next.color_count = static_cast<u8>(count);
    if (!background_theme_valid(next))
        return false;
    *output = next;
    return true;
}

} // namespace maicord
