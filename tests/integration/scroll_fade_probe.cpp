// The message list draws no fade at its bottom. The rows above the composer show the same picture scrolled up from the
// newest message as at the bottom. The probe compares those rows under a package effect, under the built-in Stars and
// under the default theme, in columns that no message text reaches. The composer overlay must also paint the same
// number of quads in both states.
#include "ui_probe_internal.hpp"
#include "background_theme_internal.hpp"
#include "maicord_effect_reference.hpp"
#include "probe_present.hpp"
#include "../../src/render/d3d11/renderer_draw.hpp"
#include "maicord/app/chat/messages/view_runtime.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/render/effects/background_effect.hpp"

namespace maicord::ui_probe_detail {
namespace {

using background_theme_detail::PixelReadback;

bool fail(u32 step) noexcept {
    log_u64(LogLevel::error, "probe", "scroll_fade_failed_step", step);
    return false;
}

// Lays out the current scroll, then draws one whole frame at a fixed effect clock.
bool draw(D3D11Renderer& r, const Theme& theme) noexcept {
    return rebuild_probe_content_text() && probe_present(&r, theme) && renderer_set_background_phase(&r, 0) &&
           d3d11_renderer_draw_stream(&r, theme, true, true, nullptr);
}

// The region's pixels, RGB, row by row.
bool read_region(D3D11Renderer& r, const UiRect& region, u8* out, u32 capacity, u32* count) noexcept {
    PixelReadback pixels{r};
    if (!pixels.readable)
        return false;
    const float scale = r.dpi / 96.f;
    const u32 left = static_cast<u32>(region.left * scale), right = static_cast<u32>(region.right * scale);
    const u32 top = static_cast<u32>(region.top * scale) + r.target_origin_y;
    const u32 bottom = static_cast<u32>(region.bottom * scale) + r.target_origin_y;
    if (left >= right || top >= bottom || right > pixels.width || bottom > pixels.height ||
        (right - left) * (bottom - top) * 3 > capacity)
        return false;
    u32 at = 0;
    for (u32 y = top; y < bottom; ++y) {
        const auto* row = static_cast<const u8*>(pixels.mapped.pData) + y * pixels.mapped.RowPitch;
        for (u32 x = left; x < right; ++x) {
            const u8* p = row + x * 4;
            out[at++] = p[pixels.bgra ? 2 : 0];
            out[at++] = p[1];
            out[at++] = p[pixels.bgra ? 0 : 2];
        }
    }
    *count = at / 3;
    return true;
}

// The quads that the last layout painted between the edit overlay and the overlay seal: chip icons and the composer
// overlay (caret, typing dots).
u32 overlay_quads(const D3D11Renderer& r) noexcept {
    u32 quads = 0;
    for (u32 b = r.edit_overlay_paint_first_batch; b < r.overlay_paint_first_batch && b < r.paint_stream.batch_count; ++b)
        quads += r.paint_stream.batches[b].quad_count;
    return quads;
}

// Pixels where a channel differs by more than 2 levels.
u32 differing(const u8* a, const u8* b, u32 count) noexcept {
    u32 found = 0;
    for (u32 i = 0; i < count; ++i) {
        bool differs = false;
        for (u32 c = 0; c < 3; ++c) {
            const i32 delta = static_cast<i32>(a[i * 3 + c]) - b[i * 3 + c];
            differs = differs || delta > 2 || delta < -2;
        }
        found += differs;
    }
    return found;
}

// Pixels within 3 levels of `color`.
u32 matching(const u8* pixels, u32 count, const float* color) noexcept {
    u32 found = 0;
    for (u32 i = 0; i < count; ++i) {
        bool same = true;
        for (u32 c = 0; c < 3; ++c) {
            const i32 delta = static_cast<i32>(pixels[i * 3 + c]) - static_cast<i32>(color[c] * 255 + .5f);
            same = same && delta >= -3 && delta <= 3;
        }
        found += same;
    }
    return found;
}

constexpr u32 kRegionBytes = 3 * 1024 * 96;
u8 g_bottom[kRegionBytes], g_away[kRegionBytes];

// Draws the region at the bottom and scrolled away, under `theme`. `step` numbers the failures. The composer overlay
// paints the same quads in both states, or the comparison fails.
bool compare(D3D11Renderer& r, const Theme& theme, const UiRect& region, u64 bottom_units, u64 away_units,
             u32 step, u32* changed, u32* count, const wchar_t* capture_bottom, const wchar_t* capture_away) noexcept {
    auto& timeline = g_probe.message_view.timeline;
    u32 bottom_count = 0, away_count = 0;
    if (!message_timeline_set_scroll(&timeline, bottom_units, true) || !draw(r, theme) || !timeline.follow_bottom)
        return fail(step);
    const u32 bottom_overlay = overlay_quads(r);
    if (!read_region(r, region, g_bottom, kRegionBytes, &bottom_count) || !probe_capture_frame(capture_bottom, theme) ||
        !message_timeline_set_scroll(&timeline, away_units, false) || !draw(r, theme) || timeline.follow_bottom)
        return fail(step);
    const u32 away_overlay = overlay_quads(r);
    log_u64(LogLevel::info, "probe", "scroll_fade_overlay_quads_bottom", bottom_overlay);
    log_u64(LogLevel::info, "probe", "scroll_fade_overlay_quads_away", away_overlay);
    if (!read_region(r, region, g_away, kRegionBytes, &away_count) || !probe_capture_frame(capture_away, theme) ||
        away_count != bottom_count || away_overlay != bottom_overlay)
        return fail(step + 1);
    *count = bottom_count;
    *changed = differing(g_bottom, g_away, bottom_count);
    log_u64(LogLevel::info, "probe", "scroll_fade_region_pixels", bottom_count);
    log_u64(LogLevel::info, "probe", "scroll_fade_changed_pixels", *changed);
    return true;
}

} // namespace

bool scroll_fade_ui_probe() noexcept {
    D3D11Renderer& r = g_probe.renderer;
    // One author's short rows, grouped under one header: no text reaches the compared columns. The probe scene holds
    // 32 rows, so 24 rows of three lines make the list taller than its view.
    constexpr u64 base = 1'280'000'000'200'000'000ull;
    for (u32 i = 0; i < 24; ++i) {
        const auto stored = message_store_upsert(&g_probe.message_view.store,
                                                 {base + i, 111, base + 10000, snowflake_timestamp_ms(base + i), 0,
                                                  utf8_literal("Row\nrow\nrow"), 0});
        if (!stored.ok())
            return fail(1);
    }
    if (!user_store_upsert(&g_probe.users, base + 10000, utf8_literal("Native Author")).ok() ||
        !message_view_runtime_reindex(&g_probe.message_view, ui_extent_units(kUiBootstrapMessageRowHeight), 1, true)
             .ok() ||
        !draw(r, kDefaultTheme))
        return fail(2);
    auto& timeline = g_probe.message_view.timeline;
    const u64 bottom = timeline.list->total_units > timeline.viewport_units
                           ? timeline.list->total_units - timeline.viewport_units
                           : 0;
    const u64 away = bottom > 200 * kUiExtentUnitsPerDip ? bottom - 200 * kUiExtentUnitsPerDip : 0;
    const UiLayoutNode* list = r.ui_scene ? ui_scene_node(*r.ui_scene, UiSemantic::message_view) : nullptr;
    if (!bottom || !away || !list)
        return fail(3);
    const UiRect& view = list->rectangle;
    // The 32 DIPs above the list's bottom and the 8 DIPs below it: the background shows there in both states.
    const float top = view.bottom - 32, below = view.bottom + 8;
    // Clear of the rows' text (it starts 72 DIPs into the list); short of the 16 DIP scrollbar.
    const float text_end = view.left + 140, list_end = view.right - 24;
    // The reference package draws the window's left half only.
    const float half = r.width * 48.f / r.dpi;
    const UiRect package_region{maximum(text_end, half - 120), top, half - 8, below};
    const UiRect stars_region{text_end, top, list_end, below};
    log_u64(LogLevel::info, "probe", "scroll_fade_list_left", static_cast<u64>(view.left));
    log_u64(LogLevel::info, "probe", "scroll_fade_list_right", static_cast<u64>(view.right));
    log_u64(LogLevel::info, "probe", "scroll_fade_list_bottom", static_cast<u64>(view.bottom));
    log_u64(LogLevel::info, "probe", "scroll_fade_window_half", static_cast<u64>(half));
    if (!(package_region.right - package_region.left >= 24) || !(stars_region.right - stars_region.left >= 64))
        return fail(4);

    // A flat theme with a package effect: the rows above the composer show the effect, not the flat colour.
    constexpr float kCoverage = .6f;
    BackgroundEffectSpec spec{};
    spec.bytecode = {g_maicord_effect_reference, sizeof(g_maicord_effect_reference)};
    spec.identity = hash64_bytes(kHash64Seed, g_maicord_effect_reference, sizeof(g_maicord_effect_reference)) | 1;
    spec.blend = EffectBlend::normal;
    spec.knobs[0][0] = kCoverage;
    const EffectSettings settings{1.f, 1.f, EffectBlend::normal, 4, 2, EffectBudget::high, false};
    const auto selected = renderer_select_effect(&r, spec, settings);
    if (selected.state != EffectSelect::changed || !r.effect.selected)
        return fail(5);
    const Theme& theme = kDefaultTheme;
    u32 changed = 0, count = 0;
    const bool package = compare(r, theme, package_region, bottom, away, 6, &changed, &count,
                                 L"scroll-fade-package-bottom.bmp", L"scroll-fade-package-up.bmp");
    const bool drawn = r.effect.active;
    renderer_clear_effect(&r);
    if (!package)
        return false;
    // The effect drew there: the region is the accent mixed over the chat background.
    const float mix[]{theme.accent.red * kCoverage + theme.base_low.red * (1 - kCoverage),
                      theme.accent.green * kCoverage + theme.base_low.green * (1 - kCoverage),
                      theme.accent.blue * kCoverage + theme.base_low.blue * (1 - kCoverage)};
    if (!drawn || matching(g_bottom, count, mix) < count * 9 / 10)
        return fail(8);
    if (changed)
        return fail(9);

    // A flat theme with the built-in Stars at full strength and density: no fade covers the stars.
    Theme stars = kDefaultTheme;
    stars.background = {};
    stars.background.effect = BackgroundEffect::stars;
    stars.background.speed = 50;
    stars.background.strength = stars.background.density = 100;
    if (!compare(r, stars, stars_region, bottom, away, 10, &changed, &count, L"scroll-fade-stars-bottom.bmp",
                 L"scroll-fade-stars-up.bmp"))
        return false;
    const float flat[]{stars.base_low.red, stars.base_low.green, stars.base_low.blue};
    if (r.background_fx.source != BackgroundFxSource::builtin || matching(g_bottom, count, flat) == count)
        return fail(12);
    if (changed)
        return fail(13);

    // The default theme: the rows above the composer match too, and the overlay paints no extra quad scrolled away.
    if (!compare(r, theme, stars_region, bottom, away, 14, &changed, &count, L"scroll-fade-flat-bottom.bmp",
                 L"scroll-fade-flat-up.bmp") ||
        changed)
        return fail(16);
    if (!message_timeline_set_scroll(&timeline, bottom, true) || !draw(r, kDefaultTheme))
        return fail(17);
    log_write(LogLevel::info, "probe", "scroll_fade_rows_above_composer_match_under_effects_passed");
    return true;
}

} // namespace maicord::ui_probe_detail
