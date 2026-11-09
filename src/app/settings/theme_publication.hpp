#pragma once

#include "maicord/app/settings/client_theme.hpp"
#include "maicord/ui/theme/styles.hpp"

namespace maicord::app_detail {
// The hot accessor retains only a small zero-initialized palette and pointer.
// Resolved numeric styles acquire one page on the first custom publication;
// they do not initialize or expand the large Application .data aggregate.
struct ThemePublication {
    // Raw bytes, not a Theme member: Theme's nonzero member defaults would put this object in .data.
    // Always written whole (set_theme) before `ready`, and never read before it.
    alignas(Theme) u8 theme_bytes[sizeof(Theme)];
    const Theme& theme() const noexcept { return *__builtin_launder(reinterpret_cast<const Theme*>(theme_bytes)); }
    Theme& set_theme(const Theme& value) noexcept {
        return *::new (static_cast<void*>(theme_bytes)) Theme(value);
    }
    BackgroundTheme previous_gradient{};
    UiResolvedThemeStyles* styles{};
    // A change of colours glides (client_theme_glide) from the look that showed (ease_from_bytes), from ease_start_ms
    // (application_now_us in milliseconds) over ease_ms, one frame at a time (application_theme_schedule_tick), unless
    // motion is reduced or the window can't show it. A custom theme with variants follows the clock
    // (client_theme_document_at): it is published again at each minute (schedule_next_ms, clock_monotonic_milliseconds),
    // or as a development folder's simulation runs it.
    alignas(Theme) u8 ease_from_bytes[sizeof(Theme)];
    u64 ease_start_ms{}, schedule_next_ms{};
    u64 schedule_ms{}; // when the last publication read the clock (clock_monotonic_milliseconds)
    u32 ease_ms{};
    u32 schedule_sample{}; // the last publication's minute, day and month as one count (months of 32 days)
    // The theme's effect's month and phase knobs as last seen (application_effects_drawn_knob, client_theme_follow).
    float follow_month{}, follow_phase{};
    bool follow_changed{}; // the knobs moved since the last publication
    ClientTheme previous_kind{};
    bool ready{}, custom{}, publishing{}, scheduled{};
};
extern ThemePublication theme_publication;

inline void remember_preferences(ThemePublication& publication,
                                 const ClientPreferences& preferences) noexcept {
    publication.previous_gradient = preferences.gradient;
    publication.previous_kind = preferences.theme;
}
// Accessibility's Saturation (percent): the interface colours (palette, tokens and the background gradient) moved
// toward their own luma; images, video and user colours keep theirs.
inline void theme_desaturate(Theme* theme, u32 percent) noexcept {
    if (percent >= 100)
        return;
    static_assert(__builtin_offsetof(Theme, base_lowest) == 0 && __builtin_offsetof(Theme, tokens) % sizeof(Color) == 0 &&
                  sizeof(UiThemeTokens) % sizeof(Color) == 0);
    const float s = static_cast<float>(percent) / 100.f;
    auto* color = reinterpret_cast<Color*>(theme);
    for (u32 i = 0, count = (__builtin_offsetof(Theme, tokens) + sizeof(UiThemeTokens)) / sizeof(Color); i < count;
         ++i) {
        auto& c = color[i];
        const float luma = .2126f * c.red + .7152f * c.green + .0722f * c.blue;
        c.red = luma + (c.red - luma) * s;
        c.green = luma + (c.green - luma) * s;
        c.blue = luma + (c.blue - luma) * s;
    }
    auto& background = theme->background;
    for (u32 i = 0; i < background.color_count && i < sizeof(background.colors) / sizeof(background.colors[0]); ++i) {
        const i32 rgb = static_cast<i32>(background.colors[i]), r = rgb >> 16 & 255, g = rgb >> 8 & 255, b = rgb & 255;
        const i32 luma = (r * 54 + g * 183 + b * 19) >> 8, scale = static_cast<i32>(percent);
        const auto mix = [&](i32 channel) noexcept { return static_cast<u32>(luma + (channel - luma) * scale / 100); };
        background.colors[i] = mix(r) << 16 | mix(g) << 8 | mix(b);
    }
}
} // namespace maicord::app_detail
