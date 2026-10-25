#include "maicord/ui/theme_presets.hpp"
#include "maicord/ui/theme_styles.hpp"

namespace maicord {

constinit const Theme kDefaultTheme = ui_theme_default_preset();
constinit const Theme kLightTheme = ui_theme_light_preset();

Theme ui_theme_amoled() noexcept {
    Theme theme = kDefaultTheme;
    ui_theme_amoled_surfaces(theme);
    ui_theme_tokens_resolve(&theme, nullptr);
    return theme;
}

} // namespace maicord
