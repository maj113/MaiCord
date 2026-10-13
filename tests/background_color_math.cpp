#include "maicord/ui/background_color_math.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/app/client_theme.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
bool close_color(float a, float b, float tolerance = .00002f) noexcept {
    return a - b < tolerance && b - a < tolerance;
}
bool same_color(Color a, Color b) noexcept {
    return bytes_equal(&a, &b, sizeof(Color));
}
} // namespace
void test_background_color_math(void (*check)(bool, const char*) noexcept) noexcept {
    check(close_color(static_cast<float>(background_color_linear(.5f)), .21404114f) &&
              close_color(background_color_srgb(.5), .73535698f),
          "theme color math: sRGB transfer agrees with independent nonlinear reference values");
    bool roundtrip = true;
    for (u32 channel = 0; channel <= 255; ++channel) {
        const float value = channel / 255.f;
        roundtrip = roundtrip && close_color(background_color_srgb(background_color_linear(value)), value);
    }
    check(roundtrip, "theme color math: every RGB8 channel round trips without a gamma-two approximation");
    {
        // Independently calculated IEC sRGB reference luminances for these
        // exact authored floats. Do not use the production transfer/helper to
        // evaluate its chosen control foreground's contrast.
        constexpr double blue_luminance = .17793665331266273;
        constexpr double yellow_luminance = .6937774884236055;
        constexpr double dark_accent_luminance = .004963822346898298;
        constexpr double dark_text_luminance = .9029841322512113;
        constexpr double dark_base_luminance = .004866682934855702;
        constexpr double light_text_luminance = .013483501169536744;
        constexpr double custom_text_luminance = .7715666475884054;
        constexpr Color white{1, 1, 1, 1}, black{0, 0, 0, 1};
        constexpr Color yellow{.95f, .85f, .4f, 1}, dark_accent{.03f, .06f, .12f, 1};
        auto custom = kDefaultTheme;
        custom.text_primary = custom.base_lowest = custom.surface = {.5f, .5f, .5f, 1};
        custom.text_secondary = {.83f, .9f, .98f, 1};
        auto limited = custom;
        limited.text_secondary = limited.text_primary;
        struct Fixture {
            Theme theme;
            Color fill, expected;
            double background_luminance, foreground_luminance;
            const char* label;
        };
        const Fixture fixtures[]{
            {kDefaultTheme, kDefaultTheme.accent, white, blue_luminance, 1,
             "filled text: default blue chooses white when both authored extremes fall below4.5"},
            {kLightTheme, kLightTheme.accent, kLightTheme.surface, blue_luminance, 1,
             "filled text: Light blue preserves its authored white surface at4.6065 contrast"},
            {kAmoledTheme, kAmoledTheme.accent, white, blue_luminance, 1,
             "filled text: AMOLED blue keeps its white on-accent text over the readable black base"},
            {kDefaultTheme, yellow, kDefaultTheme.base_lowest, yellow_luminance, dark_base_luminance,
             "filled text: bright yellow preserves readable dark authored text"},
            {kLightTheme, yellow, kLightTheme.text_primary, yellow_luminance, light_text_luminance,
             "filled text: Light bright yellow retains its own readable primary text"},
            {kDefaultTheme, dark_accent, kDefaultTheme.text_primary, dark_accent_luminance, dark_text_luminance,
             "filled text: dark accent preserves the readable authored light primary"},
            {custom, dark_accent, custom.text_secondary, dark_accent_luminance, custom_text_luminance,
             "filled text: custom authored secondary is preserved instead of replacing it with an endpoint"},
            {limited, yellow, black, yellow_luminance, 0,
             "filled text: insufficient custom palette on bright fill chooses pure black above4.5"},
        };
        for (const auto& fixture : fixtures) {
            const auto color = ui_surface_foreground(fixture.theme, fixture.fill);
            const double high = maximum(fixture.background_luminance, fixture.foreground_luminance);
            const double low = minimum(fixture.background_luminance, fixture.foreground_luminance);
            check(same_color(color, fixture.expected) && (high + .05) / (low + .05) >= 4.5,
                  fixture.label);
        }
    }
    const auto red = background_color_oklab({1, 0, 0, 1});
    check(close_color(static_cast<float>(red.lightness), .62795536f) &&
              close_color(static_cast<float>(red.a), .22486306f) &&
              close_color(static_cast<float>(red.b), .12584630f),
          "theme color math: primary red agrees with independent OKLab coordinates");
    const auto midpoint = background_color_mix({0, 0, 0, .37f}, background_color_oklab({1, 1, 1, 1}), 100);
    const auto quarter = background_color_mix({1, 1, 1, 1}, background_color_oklab({0, 0, 0, 1}), 25);
    check(close_color(midpoint.red, .38857286f) && close_color(midpoint.green, midpoint.red) &&
              close_color(midpoint.blue, midpoint.red) && midpoint.alpha == .37f &&
              close_color(quarter.red, .74320592f),
          "theme color math: original100 plus tint weights normalize in OKLab and preserve original alpha");
    BackgroundTheme draft{
        {0xff0000, 0x00ff00, 0x0000ff, 0, 0}, 90, 3, 20, BackgroundEffect::none, 50, 25, 35};
    const auto original = draft;
    BackgroundColorAnchors dark{}, light{};
    check(background_color_anchors(draft, false, &dark) && background_color_anchors(draft, true, &light) &&
              close_color(dark.base.red, 0) && close_color(dark.base.blue, .1f) &&
              close_color(light.base.red, .8f) && close_color(light.base.green, 1) &&
              close_color(dark.text.red, .84f) && close_color(dark.text.blue, 1) &&
              close_color(light.text.green, .08f) && dark.base_percent == 26 && light.base_percent == 92 &&
              dark.text_percent == 40 && light.text_percent == 30 &&
              bytes_equal(&draft, &original, sizeof(draft)),
          "theme color anchors: WCAG darkest blue/lightest green drive exact mode roles without changing "
          "stops");
    draft.color_count = 1;
    draft.colors[0] = 0x808080;
    draft.colors[1] = draft.colors[2] = 0;
    check(
        background_color_anchors(draft, false, &dark) && dark.base.red == dark.base.green &&
            dark.base.green == dark.base.blue && close_color(dark.base.red, .05f),
        "theme color anchors: achromatic single stops retain neutral anchors instead of invented red hue");
    const auto sentinel = dark;
    draft.color_count = 0;
    check(!background_color_anchors(draft, false, &dark) && bytes_equal(&sentinel, &dark, sizeof(dark)),
          "theme color anchors: invalid descriptors preserve the caller output");
    ClientPreferences preferences{};
    constexpr ClientTheme flat[]{ClientTheme::dark, ClientTheme::amoled, ClientTheme::light};
    const Theme* originals[]{&kDefaultTheme, &kAmoledTheme, &kLightTheme};
    bool unchanged = true;
    for (u32 i = 0; i < 3; ++i) {
        preferences.theme = flat[i];
        const auto projected = client_theme_projection(preferences);
        unchanged = unchanged && bytes_equal(&projected, originals[i], sizeof(Theme));
    }
    check(unchanged, "client theme projection: all flat theme bytes retain their original identity");
    preferences.theme = ClientTheme::custom_gradient;
    preferences.gradient = original;
    const auto dark20 = client_theme_projection(preferences);
    preferences.gradient.intensity = 100;
    const auto dark100 = client_theme_projection(preferences);
    preferences.gradient.intensity = 0;
    const auto dark0 = client_theme_projection(preferences);
    check(!same_color(dark20.surface, kDefaultTheme.surface) &&
              !same_color(dark20.text_primary, dark100.text_primary) &&
              !same_color(dark20.text_secondary, dark100.text_secondary) &&
              !same_color(dark0.surface, kDefaultTheme.surface) &&
              same_color(dark20.accent, kDefaultTheme.accent) &&
              same_color(dark20.notification, kDefaultTheme.notification) &&
              same_color(dark20.syntax_keyword, kDefaultTheme.syntax_keyword) &&
              background_theme_equal(dark20.background, original),
          "client theme projection: custom static materials and foreground respond to source intensity "
          "while brand/status/stops remain intact");
    preferences.theme = ClientTheme::custom_light_gradient;
    preferences.gradient = original;
    const auto light20 = client_theme_projection(preferences);
    check(!same_color(light20.surface, kLightTheme.surface) && light20.text_primary.red < .5f &&
              same_color(light20.accent, kLightTheme.accent) && light20.surface.alpha == 1,
          "client theme projection: Light custom materials remain opaque with mode-correct foreground and "
          "branded accents");
    check(background_color_rotate_hue(0xff0000, 15) == 0xff4000 &&
              background_color_rotate_hue(0x808080, 15) == 0x808080 &&
              background_color_lrgb_midpoint(0, 0xffffff) == 0xb4b4b4,
          "theme palette math: add rotation preserves HSL and complementary midpoint uses nonlinear LRGB");
    auto added = original;
    check(background_theme_add_color(original, &added) && added.color_count == 4 &&
              added.colors[3] == background_color_rotate_hue(original.colors[2], 15) &&
              added.angle_degrees == original.angle_degrees && added.intensity == original.intensity &&
              added.colors[0] == original.colors[0],
          "theme palette add: last-stop hue plus15 extends the actual ordered palette without changing "
          "settings");
    check(background_theme_snap_angle(80) == 90 && background_theme_snap_angle(100) == 90 &&
              background_theme_snap_angle(100.6f) == 101 && background_theme_snap_angle(170) == 180 &&
              background_theme_snap_angle(280) == 270 && background_theme_snap_angle(9) == 9 &&
              background_theme_snap_angle(351) == 351,
          "theme angle pointer: ten-degree quarter-turn snap retains unsnapped endpoints and out-of-band "
          "values");
    bool randomized = true, observed_pair = false, observed_harmony = false;
    auto source = original;
    source.effect = BackgroundEffect::comets;
    source.speed = 18;
    source.strength = 16;
    source.density = 25;
    for (u32 seed = 0; seed < 256; ++seed) {
        BackgroundTheme result{}, repeated{};
        randomized = randomized && background_theme_randomized(source, seed, &result) &&
                     background_theme_randomized(source, seed, &repeated) &&
                     background_theme_equal(result, repeated) && background_theme_valid(result) &&
                     (result.color_count == 2 || result.color_count == 3) && result.intensity >= 20 &&
                     result.intensity <= 80 && result.intensity % 20 == 0 &&
                     result.angle_degrees % 45 == 0 && result.angle_degrees < 360 &&
                     result.effect == source.effect && result.speed == source.speed &&
                     result.strength == source.strength && result.density == source.density;
        observed_pair = observed_pair || (result.color_count == 2 && result.colors[0] != result.colors[1]);
        observed_harmony = observed_harmony || result.color_count == 3;
    }
    check(randomized && observed_pair && observed_harmony &&
              background_theme_equal(original, preferences.gradient),
          "theme Surprise: bounded deterministic pair/harmony generation preserves source effect and "
          "caller palette with discrete angles/intensities");
}
} // namespace maicord
