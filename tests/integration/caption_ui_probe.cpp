// Window caption through the real renderer: the caption pass draws above a
// shell whose content moved down by the caption height. Checks the band,
// app icon, glyph, hover and update badge pixels focused and unfocused, that
// content pixels keep their place relative to the content origin, and the
// sign-in screen with its Settings entry. Synthetic data only; frames go to
// MAICORD_UI_PROBE_CAPTURE_DIR when set.
#include "probe_capture.hpp"
#include "../../src/app/app_icon.hpp"
#include "../../src/render/d3d11_renderer_caption.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/render/d3d11_renderer.hpp"
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
bool draw(D3D11Renderer& renderer, const Theme& theme) noexcept {
    // Presenting first builds the current view's paint and text.
    if (!renderer_present(&renderer, theme))
        return false;
    const float black[4]{0, 0, 0, 1};
    renderer.graphics.context->ClearRenderTargetView(renderer.graphics.target, black);
    if (!d3d11_renderer_draw_stream(&renderer, theme, true, true, nullptr))
        return false;
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
    const UiCaptionBarLayout layout = ui_caption_bar_layout(theme, width, {}, height);
    const auto px = [&](float dips) noexcept { return static_cast<u32>(dips * scale); };
    const UiRect minimize = layout.buttons[0], close = layout.buttons[2];
    // A point inside the close hover fill, clear of the X strokes.
    const u32 fill_x = px(close.left + theme.scale.caption_button_inset) + 2;
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
            !matches(read(image, px(layout.buttons[1].left + theme.scale.caption_button_inset) + 2, fill_y),
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
        const UiCaptionBarLayout items = ui_caption_bar_layout(theme, width, state, height);
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
            !matches(read(image, px(items.update.left + theme.scale.caption_button_inset) + 2, fill_y),
                     over(tokens.hover, over(tokens.badge_ready_fill, background)), 6))
            return fail(64);
        if (!probe_write_capture(image, shots[shot].file))
            return fail(70 + shot);
    }
    if (title_ink[0] < 20 || title_ink[1] < 20)
        return fail(80);
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
    log_u64(LogLevel::info, "probe", "caption_height_px", caption);
    return true;
}

} // namespace maicord
