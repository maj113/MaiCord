#pragma once

#include "maicord/app/client_theme.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/theme_styles.hpp"

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
    ClientTheme previous_kind{};
    bool ready{}, custom{}, publishing{};
};
extern ThemePublication theme_publication;

inline void remember_preferences(ThemePublication& publication,
                                 const ClientPreferences& preferences) noexcept {
    publication.previous_gradient = preferences.gradient;
    publication.previous_kind = preferences.theme;
}
} // namespace maicord::app_detail
