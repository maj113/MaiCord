// Window caption through the real renderer: the caption pass draws above a
// shell whose content moved down by the caption height. Checks the band,
// app icon, glyph, hover and update badge pixels focused and unfocused, that
// content pixels keep their place relative to the content origin, the
// sign-in screen with its Settings entry, the bar's own tooltip below it, a
// content scrim dimming the bar but not its window buttons, and a gradient
// backdrop continuing across the seam. Full-target frames leave an unchanged
// bar in place, and redraws reuse its uploaded quads (draws and builds are
// counted). Synthetic data only; frames
// go to MAICORD_UI_PROBE_CAPTURE_DIR when set.
#include "probe_capture.hpp"
#include "../../src/app/app_icon.hpp"
#include "../../src/render/d3d11_renderer_caption.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/ui/background_presentation.hpp"
#include "../../src/render/d3d11_renderer_draw.hpp"

namespace maicord {
u32 caption_ui_probe_stage;
namespace {

struct Pixel {
    u8 b, g, r, a;
};

Pixel read(const ProbeTargetReadback& image, u32 x, u32 y) noexcept {
    const auto* row = static_cast<const u8*>(image.mapped.pData) + y * image.mapped.RowPitch;
    return *reinterpret_cast<const Pixel*>(row + x * 4);
}

// The colour a source-over blend of `top` onto opaque `base` leaves.
Color over(Color top, Color base) noexcept {
    const float a = top.alpha;
    return {top.red * a + base.red * (1 - a), top.green * a + base.green * (1 - a),
            top.blue * a + base.blue * (1 - a), 1};
}

bool matches(Pixel pixel, Color color, i32 tolerance = 3) noexcept {
    const auto channel = [&](u8 value, float expected) noexcept {
        const i32 target = static_cast<i32>(expected * 255.0f + .5f);
        const i32 delta = static_cast<i32>(value) - target;
        return delta <= tolerance && -delta <= tolerance;
    };
    return channel(pixel.r, color.red) && channel(pixel.g, color.green) && channel(pixel.b, color.blue);
}

bool equal(Pixel a, Pixel b) noexcept {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

bool fail(u32 stage) noexcept {
    caption_ui_probe_stage = stage;
    log_u64(LogLevel::error, "probe", "caption_ui_probe_stage", stage);
    return false;
}

// The full frame as presentation draws it: content stream, then the caption.
// The fixture shell has no opaque root, so each capture starts from a clear target.
// `scrim` stands in for an overlay's full-window scrim in the content draw.
bool draw(D3D11Renderer& renderer, const Theme& theme, const Color* scrim = nullptr) noexcept {
    // Presenting first builds the current view's paint and text.
    if (!renderer_present(&renderer, theme))
        return false;
    const float black[4]{0, 0, 0, 1};
    renderer.graphics.context->ClearRenderTargetView(renderer.graphics.target, black);
    d3d11_renderer_caption_invalidate(&renderer);
    if (!d3d11_renderer_draw_stream(&renderer, theme, true, true, nullptr))
        return false;
    if (scrim)
        d3d11_renderer_caption_note_scrim(&renderer, *scrim, 1);
    renderer.present_full = true;
    d3d11_renderer_caption_pending(&renderer, theme);
    const bool drawn = d3d11_renderer_caption_draw(&renderer, theme);
    renderer.present_full = false;
    return drawn;
}

// Pixels in `box` that differ from `color`.
u32 ink_pixels(const ProbeTargetReadback& image, UiRect box, float scale, Color color) noexcept {
    u32 count = 0;
    for (u32 y = static_cast<u32>(box.top * scale); y < static_cast<u32>(box.bottom * scale); ++y)
        for (u32 x = static_cast<u32>(box.left * scale); x < static_cast<u32>(box.right * scale); ++x)
            count += matches(read(image, x, y), color, 12) ? 0u : 1u;
    return count;
}

} // namespace

bool caption_ui_probe(D3D11Renderer& renderer, bool (*relayout)(u32 content_height) noexcept,
                      bool (*show_login)(bool visible) noexcept) noexcept {
    const Theme& theme = kDefaultTheme;
    const auto& tokens = theme.tokens.caption;
    const float scale = static_cast<float>(renderer.dpi) / 96.0f;
    const u32 client_height = renderer.height;
    // Content pixels before the caption exists, sampled down the shell.
    constexpr u32 kSamples = 8;
    Pixel before[kSamples]{};
    u32 sample_x[kSamples]{}, sample_y[kSamples]{};
    {
        ProbeTargetReadback image{};
        if (!draw(renderer, theme) || probe_target_readback(renderer, &image))
            return fail(1);
        for (u32 i = 0; i < kSamples; ++i) {
            sample_x[i] = renderer.width * (2 * i + 1) / (2 * kSamples);
            sample_y[i] = 8 + i * 48;
            before[i] = read(image, sample_x[i], sample_y[i]);
        }
    }
    const u32 caption = static_cast<u32>(ui_caption_bar_height(theme) * scale + .5f);
    renderer_set_caption_height(&renderer, caption);
    if (!relayout(client_height - caption) || renderer.target_origin_y != caption)
        return fail(2);

    const float width = static_cast<float>(renderer.width) / scale;
    const float height = static_cast<float>(caption) / scale;
    const UiCaptionBarLayout layout = ui_caption_bar_layout(theme, width, {}, height, scale);
    const auto px = [&](float dips) noexcept { return static_cast<u32>(dips * scale); };
    const UiRect minimize = layout.buttons[0];
    // A point inside the close hover fill, clear of the X strokes.
    const u32 fill_x = px(layout.fills[2].left) + 2;
    const u32 fill_y = caption / 2;
    // The minimize glyph's horizontal stroke.
    const u32 glyph_x = px((minimize.left + minimize.right) * .5f);
    const Color background = over(tokens.background, theme.base_lowest);

    // The exe's icon, decoded once like the client does.
    const u32 icon_extent = static_cast<u32>(theme.scale.caption_icon * scale + .5f);
    static u32 icon[kAppIconLargestExtent * kAppIconLargestExtent];
    if (!renderer_set_caption(&renderer, {}, utf8_literal("Native Lab")) ||
        !app_icon_decode(GetModuleHandleW(nullptr), icon_extent, icon) ||
        !renderer_set_caption_icon(&renderer, icon, icon_extent))
        return fail(3);

    struct Shot {
        const wchar_t* file;
        UiCaptionBarState state;
    };
    constexpr auto none = UiCaptionPart::none;
    const Shot shots[]{
        {L"titlebar_focused.bmp", {none, none, true, false, UiCaptionUpdate::hidden, false, 0}},
        {L"titlebar_unfocused.bmp", {none, none, false, false, UiCaptionUpdate::hidden, false, 0}},
        {L"titlebar_close_hover.bmp",
         {UiCaptionPart::close, none, true, false, UiCaptionUpdate::hidden, false, 0}},
        {L"titlebar_maximized_pressed.bmp",
         {UiCaptionPart::maximize, UiCaptionPart::maximize, true, true, UiCaptionUpdate::hidden, false, 0}},
        {L"titlebar_update_downloading.bmp",
         {none, none, true, false, UiCaptionUpdate::downloading, false, .42f}},
        {L"titlebar_update_ready_hover.bmp",
         {UiCaptionPart::update, none, true, false, UiCaptionUpdate::ready, false, 1}},
    };
    u32 title_ink[2]{};
    for (u32 shot = 0; shot < sizeof(shots) / sizeof(shots[0]); ++shot) {
        const UiCaptionBarState& state = shots[shot].state;
        if (!renderer_set_caption(&renderer, state, utf8_literal("Native Lab")) || !draw(renderer, theme))
            return fail(10 + shot);
        ProbeTargetReadback image{};
        if (probe_target_readback(renderer, &image) || image.height != renderer.height + caption)
            return fail(20 + shot);
        // The band's leading edge, clear of the title.
        if (!matches(read(image, 1, caption - 1), background))
            return fail(30 + shot);
        // Content rows sit exactly one caption lower.
        for (u32 i = 0; i < kSamples; ++i)
            if (!equal(read(image, sample_x[i], sample_y[i] + caption), before[i]))
                return fail(40 + shot);
        // The minimize stroke: the strongest ink on its button's centre column.
        Pixel stroke = read(image, glyph_x, 0);
        i32 contrast = -1;
        for (u32 y = 0; y < caption; ++y) {
            const Pixel p = read(image, glyph_x, y);
            const i32 c = static_cast<i32>(p.r) + p.g + p.b;
            if (c > contrast)
                contrast = c, stroke = p;
        }
        const Color ink = state.active ? tokens.icon : over(tokens.icon_inactive, background);
        if (!matches(stroke, ink, 8))
            return fail(50 + shot);
        if (shot == 2 && !matches(read(image, fill_x, fill_y), over(tokens.close_hover, background)))
            return fail(60);
        if (shot == 3 &&
            !matches(read(image, px(layout.fills[1].left) + 2, fill_y),
                     over(tokens.pressed, background)))
            return fail(61);
        if (shot < 2) {
            // Title glyphs are drawn and dim with the window.
            const UiRect title = layout.title;
            for (u32 y = caption / 4; y < caption * 3 / 4; ++y)
                for (u32 x = px(title.left); x < px(title.right); ++x)
                    title_ink[shot] += matches(read(image, x, y), background, 12) ? 0u : 1u;
        }
        // The app icon at the leading edge.
        if (ink_pixels(image, layout.icon, scale, background) < icon_extent * icon_extent / 4)
            return fail(62);
        const UiCaptionBarLayout items = ui_caption_bar_layout(theme, width, state, height, scale);
        if (state.update == UiCaptionUpdate::downloading) {
            // Progress fill at 20% of the track, the bare track at 90%.
            const float icon_px = theme.scale.caption_icon;
            const float left = (items.update.left + items.update.right - icon_px) * .5f;
            const float bar =
                (items.update.top + items.update.bottom + icon_px) * .5f + theme.scale.space_1 + .5f;
            if (!matches(read(image, px(left + icon_px * .2f), px(bar)), tokens.badge_progress, 12) ||
                !matches(read(image, px(left + icon_px * .9f), px(bar)),
                         over(tokens.badge_track, background), 12))
                return fail(63);
        }
        if (state.update == UiCaptionUpdate::ready &&
            !matches(read(image, px(items.update_fill.left) + 2, fill_y),
                     over(tokens.hover, over(tokens.badge_ready_fill, background)), 6))
            return fail(64);
        if (!probe_write_capture(image, shots[shot].file))
            return fail(70 + shot);
    }
    if (title_ink[0] < 20 || title_ink[1] < 20)
        return fail(80);
    // A view's glyph leads its centred title ("Direct Messages" with the DM
    // icon): the group's ink starts half the glyph and its gap further left.
    {
        const UiCaptionBarState state{none, none, true, false, UiCaptionUpdate::hidden, false, 0};
        const auto leading_ink = [&](const UiCaptionTitle& title, bool capture) noexcept -> i32 {
            ProbeTargetReadback image{};
            if (!renderer_set_caption(&renderer, state, title) || !draw(renderer, theme) ||
                probe_target_readback(renderer, &image))
                return -1;
            i32 first = -1;
            for (u32 x = px(layout.title.left); first < 0 && x < px(layout.title.right); ++x)
                for (u32 y = caption / 4; y < caption * 3 / 4; ++y)
                    if (!matches(read(image, x, y), background, 12)) {
                        first = static_cast<i32>(x);
                        break;
                    }
            if (capture && !probe_write_capture(image, L"titlebar_direct_messages.bmp"))
                return -1;
            return first;
        };
        const i32 plain = leading_ink({utf8_literal("Direct Messages"), UiIcon::count, {}}, false);
        const i32 glyph = leading_ink({utf8_literal("Direct Messages"), UiIcon::direct_message, {}}, true);
        if (plain < 0 || glyph < 0 ||
            plain - glyph < static_cast<i32>((kUiCaptionTitleGlyph + kUiCaptionTitleGap) * .25f * scale))
            return fail(81);
        if (!renderer_set_caption(&renderer, state, utf8_literal("Native Lab")))
            return fail(82);
    }
    // The sign-in screen: Settings and a ready update in the caption.
    {
        const UiCaptionBarState state{none, none, true, false, UiCaptionUpdate::ready, true, 1};
        if (!show_login(true) || !renderer_set_caption(&renderer, state, utf8_literal("MaiCord")) ||
            !draw(renderer, theme))
            return fail(81);
        ProbeTargetReadback image{};
        const UiCaptionBarLayout items = ui_caption_bar_layout(theme, width, state, height);
        const bool written = !probe_target_readback(renderer, &image) &&
                             ink_pixels(image, items.settings, scale, background) > 8 &&
                             probe_write_capture(image, L"login_settings_entry.bmp");
        if (!show_login(false) || !written)
            return fail(82);
    }
    // Presentation itself: once both buffers hold the frame, a hover change
    // presents only the caption rows.
    const auto hover = [&](UiCaptionPart part) noexcept {
        return renderer_set_caption(&renderer, {part, none, true, false, UiCaptionUpdate::hidden, false, 0},
                                    utf8_literal("Native Lab")) &&
               renderer_present(&renderer, theme);
    };
    if (!renderer_present(&renderer, theme) || !renderer_present(&renderer, theme) ||
        !hover(UiCaptionPart::minimize))
        return fail(90);
    const u64 partial = renderer.partial_present_draws;
    if (!hover(UiCaptionPart::close) || renderer.partial_present_draws != partial + 1)
        return fail(91);

    // The bar's tooltip hangs into the content below the close button, and
    // the content repaints those pixels once it hides.
    {
        UiCaptionBarState state{UiCaptionPart::close, none, true, false, UiCaptionUpdate::hidden, false, 0};
        ProbeTargetReadback plain{}, shown{};
        if (!renderer_set_caption(&renderer, state, utf8_literal("Native Lab")) || !draw(renderer, theme) ||
            probe_target_readback(renderer, &plain))
            return fail(92);
        state.tooltip = UiCaptionPart::close;
        if (!renderer_set_caption(&renderer, state, utf8_literal("Native Lab")) || !draw(renderer, theme) ||
            probe_target_readback(renderer, &shown))
            return fail(93);
        // Rows below the close button change; the leading half stays.
        u32 below = 0, elsewhere = 0;
        const u32 rows = caption + px(48);
        for (u32 y = caption; y < rows; ++y) {
            for (u32 x = renderer.width - px(120); x < renderer.width; ++x)
                below += equal(read(plain, x, y), read(shown, x, y)) ? 0u : 1u;
            for (u32 x = 0; x < renderer.width / 2; ++x)
                elsewhere += equal(read(plain, x, y), read(shown, x, y)) ? 0u : 1u;
        }
        // The presented rectangle reaches down over the tooltip.
        RECT dirty{};
        if (!d3d11_renderer_caption_dirty_rect(renderer, &dirty) ||
            dirty.bottom <= static_cast<LONG>(caption + px(16)))
            return fail(98);
        if (!probe_write_capture(shown, L"titlebar_tooltip_close.bmp"))
            return fail(95);
        if (below < px(40) * px(16) || elsewhere)
            return fail(94);
        presentation_damage_commit(&renderer.presentation_damage);
        state.tooltip = none;
        if (!renderer_set_caption(&renderer, state, utf8_literal("Native Lab")) ||
            presentation_damage_empty(renderer.presentation_damage))
            return fail(96);
        if (!draw(renderer, theme))
            return fail(97);
    }

    // Under a content scrim the band, icon and title dim with it; the window
    // buttons, their hover fill included, stay above it.
    {
        const UiCaptionBarState state{UiCaptionPart::close, none, true, false, UiCaptionUpdate::hidden, false, 0};
        const Color scrim = theme.tokens.overlay.scrim;
        ProbeTargetReadback image{};
        if (!renderer_set_caption(&renderer, state, utf8_literal("Native Lab")) || !draw(renderer, theme, &scrim) ||
            probe_target_readback(renderer, &image))
            return fail(105);
        Pixel stroke = read(image, glyph_x, 0);
        i32 contrast = -1;
        for (u32 y = 0; y < caption; ++y) {
            const Pixel p = read(image, glyph_x, y);
            const i32 c = static_cast<i32>(p.r) + p.g + p.b;
            if (c > contrast)
                contrast = c, stroke = p;
        }
        if (!matches(read(image, 1, caption - 1), over(scrim, background)) ||
            !matches(read(image, fill_x, fill_y), over(tokens.close_hover, over(scrim, background))) ||
            !matches(stroke, tokens.icon, 8) ||
            ink_pixels(image, layout.icon, scale, over(scrim, background)) < icon_extent * icon_extent / 4)
            return fail(106);
        if (!probe_write_capture(image, L"titlebar_scrim.bmp"))
            return fail(107);
        if (!draw(renderer, theme))
            return fail(108);
    }

    // A cleared target draws the bar from its uploaded quads, pixel for pixel;
    // a change builds them once. Full-target content frames leave the bar in
    // place, unless the tooltip hangs into them.
    {
        UiCaptionBarState state{none, none, true, false, UiCaptionUpdate::hidden, false, 0};
        ProbeTargetReadback first{}, again{};
        if (!renderer_set_caption(&renderer, state, utf8_literal("Native Lab")) || !draw(renderer, theme) ||
            probe_target_readback(renderer, &first))
            return fail(110);
        const u32 builds = d3d11_renderer_caption_builds(renderer);
        if (!draw(renderer, theme) || !draw(renderer, theme) || probe_target_readback(renderer, &again) ||
            d3d11_renderer_caption_builds(renderer) != builds)
            return fail(111);
        for (u32 y = 0; y < caption; ++y)
            for (u32 x = 0; x < renderer.width; ++x)
                if (!equal(read(first, x, y), read(again, x, y)))
                    return fail(112);
        state.hovered = UiCaptionPart::minimize;
        if (!renderer_set_caption(&renderer, state, utf8_literal("Native Lab")) || !draw(renderer, theme) ||
            !draw(renderer, theme) || d3d11_renderer_caption_builds(renderer) != builds + 1)
            return fail(113);
        // Settled on both buffers: full content frames draw no caption.
        const auto full_frame = [&]() noexcept {
            presentation_damage_invalidate_full(&renderer.presentation_damage);
            return renderer_present(&renderer, theme);
        };
        // Two presents settle both buffers; the next back buffer is read
        // before two full content frames repaint both again.
        ProbeTargetReadback settled{};
        if (!renderer_present(&renderer, theme) || !renderer_present(&renderer, theme) ||
            probe_target_readback(renderer, &settled))
            return fail(114);
        const u32 draws = d3d11_renderer_caption_draws(renderer);
        const u64 full = renderer.full_present_draws;
        if (!full_frame() || !full_frame() || renderer.full_present_draws != full + 2 ||
            d3d11_renderer_caption_draws(renderer) != draws || d3d11_renderer_caption_builds(renderer) != builds + 1)
            return fail(118);
        // The buffer they drew into still holds the bar it showed before.
        {
            ProbeTargetReadback kept{};
            if (probe_target_readback(renderer, &kept))
                return fail(121);
            for (u32 y = 0; y < caption; ++y)
                for (u32 x = 0; x < renderer.width; ++x)
                    if (!equal(read(settled, x, y), read(kept, x, y)))
                        return fail(122);
        }
        state.hovered = state.tooltip = UiCaptionPart::close;
        if (!renderer_set_caption(&renderer, state, utf8_literal("Native Lab")) || !draw(renderer, theme) ||
            d3d11_renderer_caption_builds(renderer) != builds + 2)
            return fail(115);
        // The tooltip covers content: every full frame draws it again.
        RECT dirty{};
        if (!renderer_present(&renderer, theme) || !renderer_present(&renderer, theme))
            return fail(119);
        const u32 tooltip_draws = d3d11_renderer_caption_draws(renderer);
        if (!full_frame() || d3d11_renderer_caption_draws(renderer) != tooltip_draws + 1 ||
            !draw(renderer, theme) || d3d11_renderer_caption_builds(renderer) != builds + 2 ||
            !d3d11_renderer_caption_dirty_rect(renderer, &dirty) ||
            dirty.bottom <= static_cast<LONG>(caption + px(16)))
            return fail(116);
        state.hovered = state.tooltip = none;
        if (!renderer_set_caption(&renderer, state, utf8_literal("Native Lab")) || !draw(renderer, theme))
            return fail(117);
    }

    // A gradient theme: the band is the shell's backdrop, so the gradient
    // runs on across the seam into the guild rail below.
    {
        Theme gradient = kDefaultTheme;
        gradient.background = background_theme_defaults();
        const UiCaptionBarState state{none, none, true, false, UiCaptionUpdate::hidden, false, 0};
        ProbeTargetReadback image{};
        if (!renderer_set_caption(&renderer, state, utf8_literal("Native Lab")) || !draw(renderer, gradient) ||
            probe_target_readback(renderer, &image))
            return fail(100);
        const Color flat = over(gradient.tokens.caption.background, gradient.base_lowest);
        u32 columns = 0, continuous = 0, tinted = 0;
        for (u32 x = 2; x < px(64); x += 3) {
            const Pixel above = read(image, x, caption - 1), below = read(image, x, caption);
            const auto close_to = [](u8 a, u8 b) noexcept { return (a > b ? a - b : b - a) <= 4; };
            ++columns;
            continuous += close_to(above.r, below.r) && close_to(above.g, below.g) && close_to(above.b, below.b);
            tinted += matches(read(image, x, caption / 2), flat, 6) ? 0u : 1u;
        }
        log_u64(LogLevel::info, "probe", "caption_seam_continuous_columns", continuous);
        if (!columns || continuous * 4 < columns * 3 || tinted * 4 < columns * 3)
            return fail(101);
        if (!probe_write_capture(image, L"titlebar_gradient_seam.bmp"))
            return fail(102);
        // With Stars, captured for review. The theme change builds the bar
        // once; the moving effect redraws it without building.
        const u32 builds = d3d11_renderer_caption_builds(renderer);
        gradient.background.effect = BackgroundEffect::stars;
        ProbeTargetReadback stars{};
        if (!renderer_set_background_phase(&renderer, 3.5f) || !draw(renderer, gradient) ||
            probe_target_readback(renderer, &stars) ||
            !probe_write_capture(stars, L"titlebar_gradient_stars.bmp") ||
            d3d11_renderer_caption_builds(renderer) != builds + 1)
            return fail(103);
        // Effect draws rebind only what they replace: stars add sparse light,
        // every other content and caption pixel stays as without them.
        u32 changed = 0;
        for (u32 y = 0; y < image.height; ++y)
            for (u32 x = 0; x < renderer.width; ++x)
                changed += equal(read(image, x, y), read(stars, x, y)) ? 0u : 1u;
        log_u64(LogLevel::info, "probe", "caption_stars_changed_pixels", changed);
        if (!changed || changed * 20 > renderer.width * image.height)
            return fail(123);
        // The reused quads under the moving effect match a fresh build.
        ProbeTargetReadback reused{}, rebuilt{};
        if (!draw(renderer, gradient) || probe_target_readback(renderer, &reused) || !draw(renderer, theme) ||
            !draw(renderer, gradient) || probe_target_readback(renderer, &rebuilt) ||
            d3d11_renderer_caption_builds(renderer) != builds + 3)
            return fail(124);
        for (u32 y = 0; y < caption; ++y)
            for (u32 x = 0; x < renderer.width; ++x)
                if (!equal(read(reused, x, y), read(rebuilt, x, y)))
                    return fail(125);
        if (!renderer_present(&renderer, gradient) || !renderer_present(&renderer, gradient))
            return fail(109);
        const u32 draws = d3d11_renderer_caption_draws(renderer);
        const u64 full = renderer.full_present_draws;
        if (!renderer_set_background_phase(&renderer, 4.5f) || !renderer_present(&renderer, gradient) ||
            renderer.full_present_draws != full + 1 || d3d11_renderer_caption_draws(renderer) != draws + 1 ||
            d3d11_renderer_caption_builds(renderer) != builds + 3)
            return fail(120);
        renderer_set_background_phase(&renderer, 0);
        if (!draw(renderer, theme))
            return fail(104);
    }
    log_u64(LogLevel::info, "probe", "caption_height_px", caption);
    return true;
}

} // namespace maicord
