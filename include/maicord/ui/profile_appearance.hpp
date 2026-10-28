#pragma once
#include "maicord/model/user_profile.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/text_style.hpp"
namespace maicord {
inline UiTextFace profile_name_face(u16 id) noexcept {
    static constexpr UiTextFace faces[] = {
        UiTextFace::normal,     UiTextFace::bangers,  UiTextFace::biorhyme, UiTextFace::cherry_bomb,
        UiTextFace::chicle,     UiTextFace::compagnon, UiTextFace::museo,    UiTextFace::medieval,
        UiTextFace::pixelify,   UiTextFace::ribes,    UiTextFace::sinistre, UiTextFace::normal,
        UiTextFace::zilla,      UiTextFace::playpen,  UiTextFace::techno,   UiTextFace::new_rocker,
        UiTextFace::kalam};
    return id < sizeof(faces) / sizeof(faces[0]) ? faces[id] : UiTextFace::normal;
}
inline UiTextEffect profile_name_effect(const NameStyle& style, bool animate,
                                        u64 started_ms = 0) noexcept {
    UiTextEffect result{};
    result.kind = static_cast<u8>(style.effect_id <= 8 ? style.effect_id : 0);
    result.count = minimum<u8>(style.color_count, 8);
    if (result.kind && !result.count) {
        result.count = 1;
        result.colors[0] = 0xFFFFFF;
    }
    result.animate = animate;
    result.started_ms = started_ms;
    for (u32 i = 0; i < style.color_count && i < 8; ++i)
        result.colors[i] = style.colors[i];
    return result;
}
} // namespace maicord

#include "maicord/ui/background_color_math.hpp"
namespace maicord {
// Profile theme colours are 0xRRGGBB; the dark/light decision follows the
// primary colour.
constexpr float profile_darkness(u32 rgb) noexcept {
    return 1.f - (.299f * ((rgb >> 16) & 255) + .587f * ((rgb >> 8) & 255) + .114f * (rgb & 255)) / 255.f;
}
constexpr bool profile_theme_dark(u32 primary) noexcept {
    return profile_darkness(primary) > .5f;
}
// CIELAB as chroma.js computes it (D65, its sRGB matrices).
struct ProfileLab {
    double l, a, b;
};
inline ProfileLab profile_lab(u32 rgb) noexcept {
    const Color c = background_color_rgb24(rgb);
    const double r = background_color_linear(c.red), g = background_color_linear(c.green),
                 b = background_color_linear(c.blue);
    const auto f = [](double t) noexcept {
        t = t > 0 ? (t < 1 ? t : 1) : 0;
        return t > .008856452 ? background_color_root(t, 3) : t / .12841855 + .137931034;
    };
    const double x = f((.4124564 * r + .3575761 * g + .1804375 * b) / .95047),
                 y = f(.2126729 * r + .7151522 * g + .0721750 * b),
                 z = f((.0193339 * r + .1191920 * g + .9503041 * b) / 1.08883);
    return {116 * y - 16, 500 * (x - y), 200 * (y - z)};
}
inline u32 profile_rgb(Color c) noexcept {
    const auto channel = [](float v) noexcept {
        return static_cast<u32>(background_color_unit(v) * 255.f + .5f);
    };
    return (channel(c.red) << 16) | (channel(c.green) << 8) | channel(c.blue);
}
inline u32 profile_from_lab(ProfileLab lab) noexcept {
    const auto f = [](double t) noexcept { return t > .206896552 ? t * t * t : .12841855 * (t - .137931034); };
    const double y0 = (lab.l + 16) / 116;
    const double x = .95047 * f(y0 + lab.a / 500), y = f(y0), z = 1.08883 * f(y0 - lab.b / 200);
    return profile_rgb({background_color_srgb(3.2404542 * x - 1.5371385 * y - .4985314 * z),
                        background_color_srgb(-.9692660 * x + 1.8760108 * y + .0415560 * z),
                        background_color_srgb(.0556434 * x - .2040259 * y + 1.0572252 * z), 1});
}
// WCAG contrast (chroma.contrast).
inline double profile_contrast(u32 first, u32 second) noexcept {
    const double a = background_color_luminance(background_color_rgb24(first)),
                 b = background_color_luminance(background_color_rgb24(second));
    return a > b ? (a + .05) / (b + .05) : (b + .05) / (a + .05);
}
// Profile gradient button colour: the primary colour darkened/brightened in Lab
// lightness steps of 18 until its contrast with white is within [7, 10].
inline u32 profile_button_color(u32 primary) noexcept {
    constexpr u32 white = 0xFFFFFF;
    u32 color = primary;
    double contrast = profile_contrast(white, color);
    for (u32 i = 0; i < 100; ++i) {
        const bool low = contrast < 7, high = contrast > 10;
        if (!low && !high)
            break;
        auto lab = profile_lab(color);
        // White is never darker than the colour: too low darkens, too high brightens.
        lab.l += low ? -18 : 18;
        color = profile_from_lab(lab);
        contrast = profile_contrast(white, color);
    }
    return color;
}
// Profile gradient modal background: the Lab midpoint of primary and secondary
// with its HSL lightness rounded and clamped to [0, .1] (dark) or [.8, 1].
inline u32 profile_modal_background(u32 primary, u32 secondary, bool dark) noexcept {
    const auto p = profile_lab(primary), s = profile_lab(secondary);
    const u32 mix = profile_from_lab({(p.l + s.l) * .5, (p.a + s.a) * .5, (p.b + s.b) * .5});
    auto hsl = background_color_hsl(background_color_rgb24(mix));
    float l = static_cast<float>(static_cast<i32>(hsl.lightness * 100 + .5f)) / 100;
    l = dark ? minimum(l, .1f) : maximum(.8f, minimum(l, 1.f));
    hsl.lightness = l;
    return profile_rgb(background_color_from_hsl(hsl));
}

// The per-server profile: guild values win when set, the accent colour is the
// user's. Theme colours apply to Nitro (premium type 2) profiles and to previews.
struct ProfileDisplay {
    Utf8View bio, pronouns, banner, avatar, decoration;
    u32 theme[2];
    u64 effect;
    // Banner colour: theme primary, else the accent; has_color false when neither.
    u32 color;
    bool themed, has_color, guild_banner, guild_avatar;
};
inline ProfileDisplay profile_display(const UserProfile& p, bool preview = false) noexcept {
    ProfileDisplay d{};
    d.bio = p.guild_bio.bytes ? p.guild_bio.view() : p.bio.view();
    d.pronouns = p.guild_pronouns.bytes ? p.guild_pronouns.view() : p.pronouns.view();
    d.guild_banner = p.guild_id && p.guild_banner.bytes;
    d.banner = d.guild_banner ? p.guild_banner.view() : p.banner.view();
    d.guild_avatar = p.guild_id && p.guild_appearance.avatar.bytes;
    d.avatar = d.guild_avatar ? p.guild_appearance.avatar.view() : p.avatar.view();
    d.decoration = p.guild_appearance.decoration.bytes ? p.guild_appearance.decoration.view()
                                                       : p.user_appearance.decoration.view();
    d.effect = p.guild_effect_sku_id ? p.guild_effect_sku_id : p.effect_sku_id;
    d.themed = (p.has_guild_theme || p.has_theme) && (preview || p.premium_type == 2);
    const u32* theme = p.has_guild_theme ? p.guild_theme_colors : p.theme_colors;
    if (d.themed) {
        d.theme[0] = theme[0];
        d.theme[1] = theme[1];
    }
    // The banner colour is theme_colors[0] ?? accent even without Nitro.
    const bool colors = p.has_guild_theme || p.has_theme;
    d.has_color = colors || p.accent_color;
    d.color = colors ? theme[0] : p.accent_color;
    return d;
}
} // namespace maicord
