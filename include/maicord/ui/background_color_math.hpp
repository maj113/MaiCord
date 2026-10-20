#pragma once
#include "maicord/ui/theme.hpp"

namespace maicord {
struct BackgroundHsl {
    float hue, saturation, lightness;
};
struct BackgroundOklab {
    double lightness, a, b;
};
inline float background_color_unit(float value) noexcept {
    return value >= 0 ? minimum(value, 1.f) : 0;
}
inline Color background_color_rgb24(u32 rgb) noexcept {
    return {((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, 1};
}
// Fixed, local arithmetic avoids a CRT math import or lookup-table owner. All
// callers use the closed unit interval; bisection has a fixed iteration bound.
__declspec(noinline) inline double background_color_root(double value, u32 degree) noexcept {
    if (degree < 2 || degree > 12)
        return 0;
    if (value <= 0)
        return 0;
    if (value >= 1)
        return 1;
    double low = 0, high = 1;
    for (u32 iteration = 0; iteration < 32; ++iteration) {
        const double middle = (low + high) * .5;
        double power = middle;
        for (u32 i = 1; i < degree; ++i)
            power *= middle;
        if (power < value)
            low = middle;
        else
            high = middle;
    }
    return (low + high) * .5;
}
__declspec(noinline) inline double background_color_linear(float channel) noexcept {
    const double value = background_color_unit(channel);
    if (value <= .04045)
        return value / 12.92;
    const double scaled = (value + .055) / 1.055;
    const double squared = scaled * scaled;
    return squared * background_color_root(squared, 5);
}
__declspec(noinline) inline float background_color_srgb(double channel) noexcept {
    if (!(channel > 0))
        return 0;
    if (channel >= 1)
        return 1;
    if (channel <= .0031308)
        return static_cast<float>(12.92 * channel);
    const double squared = channel * channel;
    return static_cast<float>(1.055 * background_color_root(squared * squared * channel, 12) - .055);
}
inline double background_color_luminance(Color color) noexcept {
    return .2126 * background_color_linear(color.red) + .7152 * background_color_linear(color.green) +
           .0722 * background_color_linear(color.blue);
}
inline BackgroundHsl background_color_hsl(Color color) noexcept {
    const float red = background_color_unit(color.red), green = background_color_unit(color.green),
                blue = background_color_unit(color.blue);
    const float high = maximum(red, maximum(green, blue)), low = minimum(red, minimum(green, blue));
    const float delta = high - low, lightness = (high + low) * .5f;
    if (!delta)
        return {0, 0, lightness};
    const float saturation = delta / (lightness < .5f ? high + low : 2 - high - low);
    float hue = red == high     ? (green - blue) / delta
                : green == high ? 2 + (blue - red) / delta
                                : 4 + (red - green) / delta;
    hue *= 60;
    if (hue < 0)
        hue += 360;
    return {hue, saturation, lightness};
}
inline Color background_color_from_hsl(BackgroundHsl hsl) noexcept {
    const float saturation = background_color_unit(hsl.saturation),
                lightness = background_color_unit(hsl.lightness);
    if (!saturation)
        return {lightness, lightness, lightness, 1};
    float hue = hsl.hue >= 0 && hsl.hue <= 360 ? hsl.hue / 60 : 0;
    if (hue == 6)
        hue = 0;
    const float chroma = (1 - (lightness * 2 > 1 ? lightness * 2 - 1 : 1 - lightness * 2)) * saturation;
    const u32 sector = static_cast<u32>(hue);
    const float fraction = hue - static_cast<float>(sector);
    const float intermediate = chroma * (sector & 1 ? 1 - fraction : fraction),
                offset = lightness - chroma * .5f;
    Color result{};
    switch (sector) {
    case 0:
        result = {chroma, intermediate, 0, 1};
        break;
    case 1:
        result = {intermediate, chroma, 0, 1};
        break;
    case 2:
        result = {0, chroma, intermediate, 1};
        break;
    case 3:
        result = {0, intermediate, chroma, 1};
        break;
    case 4:
        result = {intermediate, 0, chroma, 1};
        break;
    default:
        result = {chroma, 0, intermediate, 1};
        break;
    }
    result.red += offset;
    result.green += offset;
    result.blue += offset;
    return result;
}
inline BackgroundOklab background_color_oklab(Color color) noexcept {
    const double red = background_color_linear(color.red), green = background_color_linear(color.green),
                 blue = background_color_linear(color.blue);
    const double l = background_color_root(.4122214708 * red + .5363325363 * green + .0514459929 * blue, 3);
    const double m = background_color_root(.2119034982 * red + .6806995451 * green + .1073969566 * blue, 3);
    const double s = background_color_root(.0883024619 * red + .2817188376 * green + .6299787005 * blue, 3);
    return {.2104542553 * l + .7936177850 * m - .0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + .4505937099 * s,
            .0259040371 * l + .7827717662 * m - .8086757660 * s};
}
inline Color background_color_from_oklab(BackgroundOklab lab, float alpha = 1) noexcept {
    const double l = lab.lightness + .3963377774 * lab.a + .2158037573 * lab.b,
                 m = lab.lightness - .1055613458 * lab.a - .0638541728 * lab.b,
                 s = lab.lightness - .0894841775 * lab.a - 1.2914855480 * lab.b;
    const double lc = l * l * l, mc = m * m * m, sc = s * s * s;
    return {background_color_srgb(4.0767416621 * lc - 3.3077115913 * mc + .2309699292 * sc),
            background_color_srgb(-1.2684380046 * lc + 2.6097574011 * mc - .3413193965 * sc),
            background_color_srgb(-.0041960863 * lc - .7034186147 * mc + 1.7076147010 * sc), alpha};
}
// CSS color-mix normalizes original 100% plus tint B%; B is not the final share.
inline Color background_color_mix(Color original, BackgroundOklab anchor, float tint_percent) noexcept {
    if (!(tint_percent > 0))
        return original;
    const double share = minimum(tint_percent, 100.f) / (100.0 + minimum(tint_percent, 100.f));
    const auto base = background_color_oklab(original);
    return background_color_from_oklab({base.lightness + (anchor.lightness - base.lightness) * share,
                                        base.a + (anchor.a - base.a) * share,
                                        base.b + (anchor.b - base.b) * share},
                                       original.alpha);
}
struct BackgroundColorAnchors {
    Color base, text;
    float base_percent, text_percent;
};
inline bool background_color_anchors(const BackgroundTheme& draft, bool light,
                                     BackgroundColorAnchors* output) noexcept {
    if (!output || !background_theme_valid(draft))
        return false;
    Color darkest = background_color_rgb24(draft.colors[0]), lightest = darkest;
    double minimum_luminance = background_color_luminance(darkest), maximum_luminance = minimum_luminance;
    for (u32 i = 1; i < draft.color_count; ++i) {
        const Color color = background_color_rgb24(draft.colors[i]);
        const double luminance = background_color_luminance(color);
        if (luminance < minimum_luminance) {
            minimum_luminance = luminance;
            darkest = color;
        }
        if (luminance > maximum_luminance) {
            maximum_luminance = luminance;
            lightest = color;
        }
    }
    auto base = background_color_hsl(light ? lightest : darkest), text = base;
    // An achromatic source has no hue to saturate; preserve its neutral anchor.
    if (base.saturation)
        base.saturation = 1;
    base.lightness = light ? .9f : .05f;
    // Lightness is rounded to two decimal places.
    const double text_lightness = light ? .05 - draft.intensity * .0005 : .9 + draft.intensity * .001;
    text.lightness = static_cast<float>(static_cast<u32>(text_lightness * 100 + .500000001) / 100.0);
    *output = {background_color_from_hsl(base), background_color_from_hsl(text),
               light ? 100 - .4f * draft.intensity : 30 - .2f * draft.intensity, light ? 30.f : 40.f};
    return true;
}
inline u32 background_color_pack(Color color) noexcept {
    const u32 red = static_cast<u32>(background_color_unit(color.red) * 255 + .5f),
              green = static_cast<u32>(background_color_unit(color.green) * 255 + .5f),
              blue = static_cast<u32>(background_color_unit(color.blue) * 255 + .5f);
    return (red << 16) | (green << 8) | blue;
}
inline u32 background_color_rotate_hue(u32 rgb, float degrees) noexcept {
    auto hsl = background_color_hsl(background_color_rgb24(rgb));
    if (!(degrees >= -360 && degrees <= 360))
        return rgb & 0xffffff;
    hsl.hue += degrees;
    if (hsl.hue >= 360)
        hsl.hue -= 360;
    if (hsl.hue < 0)
        hsl.hue += 360;
    return background_color_pack(background_color_from_hsl(hsl));
}
inline u32 background_color_lrgb_midpoint(u32 first, u32 second) noexcept {
    const auto a = background_color_rgb24(first), b = background_color_rgb24(second);
    return background_color_pack(
        {static_cast<float>(background_color_root((a.red * a.red + b.red * b.red) * .5, 2)),
         static_cast<float>(background_color_root((a.green * a.green + b.green * b.green) * .5, 2)),
         static_cast<float>(background_color_root((a.blue * a.blue + b.blue * b.blue) * .5, 2)), 1});
}
inline bool background_theme_add_color(const BackgroundTheme& draft, BackgroundTheme* output) noexcept {
    if (!output || !background_theme_valid(draft) || draft.color_count == 5)
        return false;
    auto next = draft;
    next.colors[next.color_count] = background_color_rotate_hue(next.colors[next.color_count - 1], 15);
    ++next.color_count;
    *output = next;
    return true;
}
inline u16 background_theme_snap_angle(float angle) noexcept {
    if (!(angle >= 0))
        return 0;
    angle = minimum(angle, 360.f);
    constexpr u16 targets[]{90, 180, 270};
    for (const u16 target : targets)
        if (angle >= target - 10 && angle <= target + 10)
            return target;
    return static_cast<u16>(angle + .5f);
}
inline u32 background_color_random_index(u32& state, u32 count) noexcept {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<u32>((static_cast<u64>(state) * count) >> 32);
}
// Randomized harmony palette: only palette/angle/intensity change; the caller's
// effect profile stays intact.
inline bool background_theme_randomized(const BackgroundTheme& draft, u32 seed,
                                        BackgroundTheme* output) noexcept {
    if (!output || !background_theme_valid(draft))
        return false;
    constexpr u32 seeds[]{0x94e0cf, 0x9af0b1, 0x9a90ff, 0x9a53ff, 0xfda6e4, 0xffe6c0, 0xefb4aa,
                          0x56b69f, 0x29c566, 0x5348ca, 0x6d24d4, 0xca48c8, 0xf0ae29, 0xdf4232};
    auto next = draft;
    for (auto& color : next.colors)
        color = 0;
    // Avalanche small/adjacent clock seeds before drawing the discrete choices.
    u32 state = seed + 0x9e3779b9;
    state = (state ^ (state >> 16)) * 0x7feb352d;
    state = (state ^ (state >> 15)) * 0x846ca68b;
    state ^= state >> 16;
    if (!state)
        state = 0x9e3779b9;
    next.intensity = static_cast<u8>(20 * (background_color_random_index(state, 4) + 1));
    next.angle_degrees = static_cast<u16>(45 * background_color_random_index(state, 8));
    const bool pair = background_color_random_index(state, 5) == 0;
    const u32 selected = background_color_random_index(state, 14), color = seeds[selected];
    if (pair) {
        u32 other = background_color_random_index(state, 13);
        if (other >= selected)
            ++other;
        next.color_count = 2;
        next.colors[0] = color;
        next.colors[1] = seeds[other];
    } else {
        next.color_count = 3;
        switch (background_color_random_index(state, 4)) {
        case 0:
            next.colors[0] = background_color_rotate_hue(color, -30);
            next.colors[1] = color;
            next.colors[2] = background_color_rotate_hue(color, 30);
            break;
        case 1:
            next.colors[0] = color;
            next.colors[2] = background_color_rotate_hue(color, 180);
            next.colors[1] = background_color_lrgb_midpoint(next.colors[0], next.colors[2]);
            break;
        case 2:
            next.colors[0] = color;
            next.colors[1] = background_color_rotate_hue(color, 150);
            next.colors[2] = background_color_rotate_hue(color, 210);
            break;
        default:
            next.colors[0] = color;
            next.colors[1] = background_color_rotate_hue(color, 120);
            next.colors[2] = background_color_rotate_hue(color, 240);
            break;
        }
    }
    *output = next;
    return true;
}
} // namespace maicord
