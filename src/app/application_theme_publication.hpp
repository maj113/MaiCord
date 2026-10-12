#pragma once

#include "maicord/app/client_theme.hpp"
#include "maicord/ui/theme_styles.hpp"

namespace maicord::app_detail {
// The hot accessor retains only a small zero-initialized palette and pointer.
// Resolved numeric styles acquire one page on the first custom publication;
// they do not initialize or expand the large Application .data aggregate.
struct ThemePublication {
    Theme theme{};
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
