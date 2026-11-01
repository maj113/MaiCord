#include "showcase_probe_internal.hpp"

namespace maicord {
namespace {

// --- Generated artwork ---------------------------------------------------

struct Rgb {
    float r, g, b;
};
constexpr Rgb hex(u32 v) noexcept {
    return {((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f};
}
float clamp01(float x) noexcept {
    return x < 0 ? 0 : x > 1 ? 1 : x;
}
Rgb mix(Rgb a, Rgb b, float t) noexcept {
    t = clamp01(t);
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}
float smooth(float e0, float e1, float x) noexcept {
    const float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3 - 2 * t);
}
float root(float x) noexcept {
    if (x <= 0)
        return 0;
    union {
        float f;
        u32 u;
    } v{x};
    v.u = 0x1fbd1df5u + (v.u >> 1);
    float y = v.f;
    for (u32 i = 0; i < 3; ++i)
        y = .5f * (y + x / y);
    return y;
}
float sine(float x) noexcept {
    constexpr float pi = 3.14159265f, tau = 6.28318531f;
    const float turns = x / tau;
    x -= tau * static_cast<float>(static_cast<i32>(turns + (turns >= 0 ? .5f : -.5f)));
    if (x > pi / 2)
        x = pi - x;
    else if (x < -pi / 2)
        x = -pi - x;
    const float x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42)));
}
u32 hash32(u32 x) noexcept {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}
void put(u8* pixel, Rgb c) noexcept {
    pixel[0] = static_cast<u8>(clamp01(c.r) * 255 + .5f);
    pixel[1] = static_cast<u8>(clamp01(c.g) * 255 + .5f);
    pixel[2] = static_cast<u8>(clamp01(c.b) * 255 + .5f);
    pixel[3] = 255;
}
float disc(float x, float y, float cx, float cy, float r, float soft) noexcept {
    const float dx = x - cx, dy = y - cy;
    return 1 - smooth(r - soft, r + soft, root(dx * dx + dy * dy));
}
// Distance from (x, y) to the segment a-b.
float segment(float x, float y, float ax, float ay, float bx, float by) noexcept {
    const float px = x - ax, py = y - ay, dx = bx - ax, dy = by - ay;
    const float t = clamp01((px * dx + py * dy) / (dx * dx + dy * dy));
    const float ex = px - dx * t, ey = py - dy * t;
    return root(ex * ex + ey * ey);
}
// Sparse twinkling points.
float stars(float x, float y, u32 seed, float density) noexcept {
    const i32 cx = static_cast<i32>(x / 14), cy = static_cast<i32>(y / 14);
    const u32 h = hash32(static_cast<u32>(cx) * 7919u ^ static_cast<u32>(cy) * 104729u ^ seed);
    if (static_cast<float>(h & 1023) > density * 1023)
        return 0;
    const float sx = static_cast<float>(cx * 14 + 2 + static_cast<i32>((h >> 10) % 10));
    const float sy = static_cast<float>(cy * 14 + 2 + static_cast<i32>((h >> 16) % 10));
    return disc(x, y, sx, sy, .6f + static_cast<float>((h >> 24) % 3) * .35f, .7f) *
           (.5f + static_cast<float>(h >> 28) / 30.f);
}

} // namespace

namespace showcase_probe_detail {

// A palette study: dusk sky, sun, layered hills and their reflection.
void paint_landscape(u8* out, u32 out_w, u32 out_h) noexcept {
    // Painted in 1280-wide reference units at any output size.
    const float k = 1280.f / out_w;
    const float w = 1280.f, h = out_h * k;
    const Rgb sky_top = hex(0x1d1747), sky_mid = hex(0x6d2f72), horizon = hex(0xf4925a);
    const Rgb hills[3]{hex(0x8a3a6e), hex(0x55265e), hex(0x2a1640)};
    const float water = h * .74f;
    for (u32 y = 0; y < out_h; ++y)
        for (u32 x = 0; x < out_w; ++x) {
            const float fx = (x + .5f) * k, fy = (y + .5f) * k;
            const bool below = fy > water;
            // The reflection mirrors the scene about the shoreline.
            const float sy = below ? water - (fy - water) * 1.6f : fy;
            const float t = sy / water;
            Rgb c = t < .55f ? mix(sky_top, sky_mid, t / .55f) : mix(sky_mid, horizon, (t - .55f) / .45f);
            c = mix(c, hex(0xfff1e0), stars(fx, sy, 7, .08f) * (1 - t) * .9f);
            const float sun_x = w * .66f, sun_y = water * .78f, sun_r = h * .12f;
            const float dx = fx - sun_x, dy = sy - sun_y, d = root(dx * dx + dy * dy);
            c = mix(c, hex(0xffb36b), (1 - smooth(sun_r, sun_r * 3.2f, d)) * .45f);
            c = mix(c, hex(0xffd88a), disc(fx, sy, sun_x, sun_y, sun_r, 1.2f));
            for (u32 layer = 0; layer < 3; ++layer) {
                const float base = water * (.62f + layer * .12f);
                const float ridge = base - h * (.07f - layer * .015f) *
                                               (sine(fx * (.006f + layer * .004f) + layer * 1.7f) +
                                                .45f * sine(fx * (.019f + layer * .007f) + layer * 2.9f));
                c = mix(c, mix(hills[layer], horizon, .18f - layer * .06f),
                        smooth(ridge - .8f, ridge + .8f, sy));
            }
            if (below) {
                const float ripple = .5f + .5f * sine(fy * .55f + sine(fx * .021f) * 2.4f);
                c = mix(c, hex(0x120c2c), .35f + .25f * ripple * clamp01((fy - water) / (h - water) + .3f));
            }
            put(out + (y * out_w + x) * 4, c);
        }
}

// The three gradient presets as cards on a dark panel.
void paint_gallery(u8* out, u32 out_w, u32 out_h) noexcept {
    const float k = 1024.f / out_w;
    const float w = 1024.f, h = out_h * k;
    const u32 palettes[3][3]{
        {0x6935b8, 0x164d80, 0x16836f}, {0x7a284b, 0xa65732, 0x362062}, {0x15335f, 0x167d89, 0x363269}};
    const Rgb panel = hex(0x17171d);
    const float margin = w * .05f, gap = w * .035f;
    const float card_w = (w - margin * 2 - gap * 2) / 3, top = h * .14f, bottom = h * .86f,
                radius = w * .025f;
    for (u32 y = 0; y < out_h; ++y)
        for (u32 x = 0; x < out_w; ++x) {
            const float fx = (x + .5f) * k, fy = (y + .5f) * k;
            Rgb c = mix(panel, hex(0x22222b), fy / h);
            for (u32 i = 0; i < 3; ++i) {
                const float left = margin + i * (card_w + gap), right = left + card_w;
                const float qx = fx < left + radius    ? left + radius - fx
                                 : fx > right - radius ? fx - right + radius
                                                       : 0;
                const float qy = fy < top + radius      ? top + radius - fy
                                 : fy > bottom - radius ? fy - bottom + radius
                                                        : 0;
                const float edge = root(qx * qx + qy * qy) - radius;
                const float inside = 1 - smooth(-.8f, .8f, edge);
                if (inside <= 0)
                    continue;
                // Diagonal three-stop gradient, as the theme's background draws it.
                const float t = clamp01(((fx - left) / card_w * .45f + (fy - top) / (bottom - top) * .55f));
                Rgb g = t < .5f ? mix(hex(palettes[i][0]), hex(palettes[i][1]), t * 2)
                                : mix(hex(palettes[i][1]), hex(palettes[i][2]), t * 2 - 1);
                g = mix(g, hex(0xffffff), stars(fx, fy, 31 + i, .12f) * .8f);
                // A frosted message bubble on each card.
                const float bx0 = left + card_w * .12f, bx1 = right - card_w * .12f;
                const float by0 = top + (bottom - top) * .62f, by1 = top + (bottom - top) * .82f;
                if (fx > bx0 && fx < bx1 && fy > by0 && fy < by1)
                    g = mix(g, hex(0xffffff), .16f);
                const float line = by0 + (by1 - by0) * .38f;
                if (fx > bx0 + 12 && fx < bx0 + (bx1 - bx0) * .7f && fy > line - 2.5f && fy < line + 2.5f)
                    g = mix(g, hex(0xffffff), .55f);
                c = mix(c, g, inside);
            }
            put(out + (y * out_w + x) * 4, c);
        }
}

// Abstract avatar: two-tone gradient with a soft disc and a ring.
void paint_avatar(u8* out, u32 s, u32 a, u32 b, u32 variant) noexcept {
    for (u32 y = 0; y < s; ++y)
        for (u32 x = 0; x < s; ++x) {
            const float fx = (x + .5f) / s, fy = (y + .5f) / s;
            Rgb c = mix(hex(a), hex(b), fx * .5f + fy * .5f);
            const float cx = .5f + (variant & 1 ? .12f : -.1f), cy = .42f + (variant & 2 ? .08f : -.04f);
            c = mix(c, hex(0xffffff), disc(fx, fy, cx, cy, .2f + (variant % 3) * .03f, .01f) * .28f);
            const float dx = fx - .5f, dy = fy - .62f, d = root(dx * dx + dy * dy);
            if (variant % 4 >= 2)
                c = mix(c, hex(b), (1 - smooth(.012f, .03f, d < .34f ? .34f - d : d - .34f)) * .55f);
            else
                c = mix(c, hex(a), disc(fx, fy, .5f, 1.05f, .45f, .01f) * .6f);
            put(out + (y * s + x) * 4, c);
        }
}

// The MaiCord Lab icon: the app's tile and stroke, drawn here.
void paint_logo(u8* out, u32 s) noexcept {
    const float p[5][2]{{49.3f, 125}, {49.3f, 54.6f}, {88, 96.8f}, {126.7f, 54.6f}, {126.7f, 125}};
    for (u32 y = 0; y < s; ++y)
        for (u32 x = 0; x < s; ++x) {
            const float fx = (x + .5f) * 176 / s, fy = (y + .5f) * 176 / s;
            float d = 1e9f;
            for (u32 i = 0; i < 4; ++i) {
                const float e = segment(fx, fy, p[i][0], p[i][1], p[i + 1][0], p[i + 1][1]);
                d = e < d ? e : d;
            }
            Rgb c = mix(hex(0x5865f2), hex(0x7a5cff), fy / 176);
            c = mix(c, hex(0xffffff), 1 - smooth(11.5f, 13.5f, d));
            put(out + (y * s + x) * 4, c);
        }
}

// Guild icon: gradient tile with a simple geometric mark.
void paint_guild_icon(u8* out, u32 s, u32 a, u32 b, u32 shape) noexcept {
    for (u32 y = 0; y < s; ++y)
        for (u32 x = 0; x < s; ++x) {
            const float fx = (x + .5f) / s, fy = (y + .5f) / s;
            Rgb c = mix(hex(a), hex(b), fy);
            float mark = 0;
            if (shape == 0)
                mark = disc(fx, fy, .5f, .5f, .22f, .012f) - disc(fx, fy, .5f, .5f, .12f, .012f);
            else if (shape == 1) {
                const float dx = fx > .5f ? fx - .5f : .5f - fx, dy = fy > .5f ? fy - .5f : .5f - fy;
                mark = 1 - smooth(.22f, .235f, dx + dy);
            } else if (shape == 2)
                mark = (1 - smooth(.012f, .03f, segment(fx, fy, .28f, .66f, .5f, .3f))) +
                       (1 - smooth(.012f, .03f, segment(fx, fy, .5f, .3f, .72f, .66f)));
            else if (shape == 3)
                mark = disc(fx, fy, .4f, .45f, .16f, .012f) + disc(fx, fy, .6f, .55f, .16f, .012f) * .7f;
            else
                mark = (1 - smooth(.015f, .03f, segment(fx, fy, .26f, .6f, .74f, .6f))) +
                       disc(fx, fy, .5f, .42f, .1f, .012f);
            put(out + (y * s + x) * 4, mix(c, hex(0xffffff), clamp01(mark) * .9f));
        }
}

// Profile banner: soft dusk waves.
void paint_banner(u8* out, u32 w, u32 h) noexcept {
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const float fx = static_cast<float>(x), fy = static_cast<float>(y);
            Rgb c = mix(hex(0x3b2a7a), hex(0xd2648a), fx / w * .7f + fy / h * .3f);
            for (u32 i = 0; i < 3; ++i) {
                const float crest =
                    h * (.45f + i * .16f) + h * .08f * sine(fx * (.012f + i * .006f) + i * 2.1f);
                c = mix(c, mix(hex(0xffc58f), hex(0x2a1b5c), i * .45f),
                        smooth(crest - 1, crest + 1, fy) * .35f);
            }
            c = mix(c, hex(0xffffff), stars(fx, fy, 3, .07f) * .7f);
            put(out + (y * w + x) * 4, c);
        }
}

// Game key art: a glass canyon at night with a glowing orb.
void paint_game_art(u8* out, u32 s) noexcept {
    for (u32 y = 0; y < s; ++y)
        for (u32 x = 0; x < s; ++x) {
            const float fx = static_cast<float>(x) / s, fy = static_cast<float>(y) / s;
            Rgb c = mix(hex(0x0b1e3a), hex(0x1f6f8b), fy);
            c = mix(c, hex(0xffffff), stars(fx * s * 1.5f, fy * s * 1.5f, 11, .1f) * (1 - fy));
            c = mix(c, hex(0x9ff3ff), disc(fx, fy, .62f, .34f, .1f, .01f));
            c = mix(c, hex(0x5fd3e6),
                    (1 - smooth(.1f, .3f, root((fx - .62f) * (fx - .62f) + (fy - .34f) * (fy - .34f)))) *
                        .35f);
            const float left = .78f - .5f * fx, right = .78f - .5f * (1 - fx);
            const float wall = fy > (left > right ? left : right) ? 1.f : 0.f;
            c = mix(c, mix(hex(0x143b5a), hex(0x2c8fa8), fx), wall * .85f);
            put(out + (y * s + x) * 4, c);
        }
}

// Album cover: concentric rings.
void paint_record(u8* out, u32 s) noexcept {
    for (u32 y = 0; y < s; ++y)
        for (u32 x = 0; x < s; ++x) {
            const float fx = (x + .5f) / s - .5f, fy = (y + .5f) / s - .5f;
            const float d = root(fx * fx + fy * fy);
            Rgb c = mix(hex(0xff7a59), hex(0x8e2de2), (fx + fy) * .7f + .5f);
            c = mix(c, hex(0x14101f), (1 - smooth(.33f, .335f, d)) * (.6f + .15f * sine(d * 180)));
            c = mix(c, hex(0xffd166), disc(fx, fy, 0, 0, .07f, .004f));
            put(out + (y * s + x) * 4, c);
        }
}

} // namespace showcase_probe_detail
} // namespace maicord
