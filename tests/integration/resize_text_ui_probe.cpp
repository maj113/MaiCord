// Text keeps its glyph spacing while the window width changes by one pixel.
// At 125% a centred surface (the settings modal, the caption title) moves by
// half a device pixel per pixel of width. A text block lands on whole pixels
// as one, so every glyph of a word moves by the same whole number of pixels;
// rounding each glyph on its own moved some letters and not their neighbours.
// Checks the settings page title's glyph quads and the caption title's pixels
// at consecutive widths. Frames go to MAICORD_UI_PROBE_CAPTURE_DIR when set.
#include "probe_capture.hpp"
#include "probe_present.hpp"
#include "settings/catalog.hpp"
#include "../../src/render/d3d11/renderer_caption.hpp"
#include "../../src/render/d3d11/renderer_draw.hpp"
#include "maicord/core/log.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "maicord/ui/shell/caption_bar.hpp"
#include "maicord/ui/widgets/kit.hpp"

namespace maicord {
namespace {

constexpr u32 kDpi = 120, kHeight = 882;
// Odd and even widths in both orders, at the owner's window size and a smaller one.
constexpr u32 kWidths[]{1446, 1447, 1448, 1201, 1202};
constexpr u32 kGlyphs = 64, kColumns = 1024;

bool fail(u32 stage) noexcept {
    log_u64(LogLevel::error, "probe", "resize_text_ui_probe_stage", stage);
    return false;
}

struct Settings {
    UiPage pages[kSettingsPanelCount];
    UiSettingsPageContent contents[kSettingsPanelCount];
    UiSettingsControl controls[kSettingsPanelCount];
    UiKitMeasure measure;
    UiPageRegistry registry;
    UiSettingsScene scene;
};
Settings g_settings;

// Device-pixel left edges of the glyphs inside `box` (a single-line title).
u32 glyph_lefts(const D3D11Renderer& renderer, UiRect box, float scale, float* lefts) noexcept {
    const RendererTextStorage* text = renderer.content_text;
    u32 count = 0;
    for (u32 i = 0; text && i < text->quad_count; ++i) {
        const GlyphQuad& q = text->quads[i];
        const float x = (q.left + q.right) * .5f, y = (q.top + q.bottom) * .5f;
        // Monochrome glyphs only: decorations have negative u, colour tiles start at 2.
        if (q.u0 < 0 || q.u0 >= 2 || x < box.left || x > box.right || y < box.top || y > box.bottom)
            continue;
        if (count == kGlyphs)
            return 0;
        lefts[count++] = q.left * scale;
    }
    return count;
}

// The same word at the two widths: one whole-pixel translation for every glyph.
bool same_spacing(const float* a, u32 a_count, const float* b, u32 b_count, u32 width) noexcept {
    if (a_count < 5 || a_count != b_count)
        return false;
    const float shift = b[0] - a[0];
    const float whole = static_cast<float>(static_cast<i32>(shift + (shift < 0 ? -.5f : .5f)));
    bool same = shift - whole < .01f && whole - shift < .01f;
    for (u32 i = 1; i < a_count; ++i) {
        const float error = (b[i] - a[i]) - shift;
        if (error > .01f || error < -.01f) {
            log_u64(LogLevel::error, "probe", "resize_text_spacing_width", width);
            log_u64(LogLevel::error, "probe", "resize_text_spacing_glyph", i);
            same = false;
        }
    }
    return same;
}

bool settings_titles(D3D11Renderer& renderer, UiTextMeasureCache& cache, bool (*resize)(u32, u32) noexcept,
                     bool (*capture)(const wchar_t*) noexcept) noexcept {
    auto& s = g_settings;
    struct Restore {
        D3D11Renderer& renderer;
        Settings& s;
        ~Restore() noexcept {
            renderer_bind_settings_scene(&renderer, nullptr);
            ui_settings_scene_release(&s.scene);
            ui_page_registry_clear(&s.registry);
        }
    } restore{renderer, s};
    if (!ui_page_registry_initialize(&s.registry, {s.pages, kSettingsPanelCount}))
        return fail(1);
    for (u32 panel = 0; panel < kSettingsPanelCount; ++panel) {
        const auto& source = settings_catalog_panel(static_cast<SettingsPanel>(panel));
        s.controls[panel] = {1000u + panel, source.title, source.summary, UiSettingsControlKind::toggle, true, false};
        s.contents[panel] = {source.title, source.summary, {&s.controls[panel], 1}, 1};
        if (!ui_page_registry_register(&s.registry, {{kSettingsNativeOwner, panel + 1u}, source.title, &s.contents[panel]})
                 .ok())
            return fail(2);
    }
    if (!ui_settings_scene_set_presentation(&s.scene, UiSettingsPresentation::modal) ||
        !ui_settings_scene_bind_navigation(&s.scene, settings_catalog_navigation(true), 1))
        return fail(3);
    ui_kit_measure_cache(&s.measure, &cache);
    ui_settings_scene_bind_measure(&s.scene, &s.measure);
    const UiPageKey appearance{kSettingsNativeOwner, static_cast<u64>(SettingsPanel::appearance) + 1};
    if (!ui_page_registry_select(&s.registry, appearance).ok())
        return fail(4);
    const float scale = static_cast<float>(kDpi) / 96.f;
    float lefts[2][kGlyphs]{};
    u32 counts[2]{};
    u32 builds = 0;
    for (u32 i = 0; i < sizeof(kWidths) / sizeof(kWidths[0]); ++i) {
        const u32 width = kWidths[i];
        const bool step = i && kWidths[i - 1] + 1 >= width && width + 1 >= kWidths[i - 1];
        if (!resize(width, kHeight))
            return fail(10 + i);
        renderer_bind_settings_scene(&renderer, &s.scene);
        const UiRect viewport{0, 0, width / scale, kHeight / scale};
        if (!ui_settings_scene_update(&s.scene, &s.registry, viewport) ||
            !renderer_rebuild_content_text(&renderer, kDefaultTheme) || !probe_present(&renderer, kDefaultTheme))
            return fail(20 + i);
        const UiRect column = s.scene.layout.column;
        // The page title's 30 DIP line at the top of the column.
        const UiRect title{column.left, column.top, column.right, column.top + 30};
        counts[i & 1] = glyph_lefts(renderer, title, scale, lefts[i & 1]);
        wchar_t name[] = L"resize-settings-0000.bmp";
        for (u32 digit = 0, value = width; digit < 4; ++digit, value /= 10)
            name[19 - digit] = static_cast<wchar_t>('0' + value % 10);
        if (!capture(name))
            return fail(30 + i);
        for (u32 glyph = 0; glyph < counts[i & 1]; ++glyph)
            log_u64(LogLevel::info, "probe", "resize_text_settings_glyph_x", static_cast<u64>(lefts[i & 1][glyph] + .5f));
        // Each width against the one before it (a pixel narrower or wider):
        // the same spacing, from the same atlas tiles with no new raster.
        if (step && !same_spacing(lefts[(i - 1) & 1], counts[(i - 1) & 1], lefts[i & 1], counts[i & 1], width))
            return fail(40 + i);
        if (step && renderer.glyph_text.cache_builds != builds)
            return fail(45 + i);
        builds = renderer.glyph_text.cache_builds;
    }
    return true;
}

// The full frame as presentation draws it: content stream, then the caption.
bool draw_with_caption(D3D11Renderer& renderer, const Theme& theme) noexcept {
    if (!renderer_present(&renderer, theme))
        return false;
    const float black[4]{0, 0, 0, 1};
    renderer.graphics.context->ClearRenderTargetView(renderer.graphics.target, black);
    d3d11_renderer_caption_invalidate(&renderer);
    if (!d3d11_renderer_draw_stream(&renderer, theme, true, true, nullptr))
        return false;
    renderer.present_full = true;
    d3d11_renderer_caption_pending(&renderer, theme);
    const bool drawn = d3d11_renderer_caption_draw(&renderer, theme);
    renderer.present_full = false;
    return drawn;
}

// Which columns of the caption's title span hold ink, from its first inked column.
u32 caption_ink(D3D11Renderer& renderer, u32 caption, float scale, bool* ink) noexcept {
    const Theme& theme = kDefaultTheme;
    ProbeTargetReadback image{};
    if (!draw_with_caption(renderer, theme) || probe_target_readback(renderer, &image) ||
        image.height != renderer.height + caption)
        return 0;
    const UiCaptionBarLayout layout =
        ui_caption_bar_layout(theme, static_cast<float>(renderer.width) / scale, {}, caption / scale, scale);
    const u32 first = static_cast<u32>(layout.title.left * scale), last = static_cast<u32>(layout.title.right * scale);
    if (last <= first || last > image.width)
        return 0;
    // Ink differs from the band's leading edge, which holds no title.
    const auto* bytes = static_cast<const u8*>(image.mapped.pData);
    i32 start = -1;
    u32 count = 0;
    for (u32 x = first; x < last; ++x) {
        bool inked = false;
        for (u32 y = caption / 4; y < caption * 3 / 4 && !inked; ++y) {
            const u8* row = bytes + y * image.mapped.RowPitch;
            i32 difference = 0;
            for (u32 channel = 0; channel < 3; ++channel) {
                const i32 delta = static_cast<i32>(row[x * 4 + channel]) - row[4 + channel];
                difference += delta < 0 ? -delta : delta;
            }
            inked = difference > 36;
        }
        if (start < 0 && !inked)
            continue;
        if (start < 0)
            start = static_cast<i32>(x);
        if (count == kColumns)
            return 0;
        ink[count++] = inked;
    }
    while (count && !ink[count - 1])
        --count;
    // Each inked run's first column on screen: the title's letter positions.
    for (u32 x = 0; x < count; ++x)
        if (ink[x] && (!x || !ink[x - 1]))
            log_u64(LogLevel::info, "probe", "resize_text_caption_ink_x", static_cast<u64>(start) + x);
    return count;
}

bool caption_titles(D3D11Renderer& renderer, bool (*resize)(u32, u32) noexcept) noexcept {
    const float scale = static_cast<float>(kDpi) / 96.f;
    const u32 caption = static_cast<u32>(ui_caption_bar_height(kDefaultTheme) * scale + .5f);
    renderer_set_caption_height(&renderer, caption);
    struct Restore {
        D3D11Renderer& renderer;
        ~Restore() noexcept { renderer_set_caption_height(&renderer, 0); }
    } restore{renderer};
    static bool ink[2][kColumns];
    u32 counts[2]{};
    for (u32 i = 0; i < sizeof(kWidths) / sizeof(kWidths[0]); ++i) {
        const u32 width = kWidths[i];
        if (!resize(width, kHeight - caption) ||
            !renderer_set_caption(&renderer, {UiCaptionPart::none, UiCaptionPart::none, true, false,
                                              UiCaptionUpdate::hidden, false, 0},
                                  utf8_literal("Jamosis Lounge")))
            return fail(50 + i);
        counts[i & 1] = caption_ink(renderer, caption, scale, ink[i & 1]);
        if (counts[i & 1] < 40)
            return fail(60 + i);
        if (i && kWidths[i - 1] + 1 >= width && width + 1 >= kWidths[i - 1]) {
            bool same = counts[0] == counts[1];
            for (u32 x = 0; same && x < counts[0]; ++x)
                same = ink[0][x] == ink[1][x];
            if (!same)
                return fail(70 + i);
        }
    }
    return true;
}

} // namespace

bool resize_text_ui_probe(D3D11Renderer& renderer, UiTextMeasureCache& cache, bool (*resize)(u32, u32) noexcept,
                          bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (!resize || !capture || renderer.login_visible)
        return fail(0);
    const u32 dpi = renderer.dpi, width = renderer.width, height = renderer.height;
    renderer_set_dpi(&renderer, kDpi);
    // Both surfaces run, so a failure reports each of them.
    const bool settings = settings_titles(renderer, cache, resize, capture);
    const bool valid = caption_titles(renderer, resize) && settings;
    renderer_set_dpi(&renderer, dpi);
    const bool restored = resize(width, height);
    log_write(valid ? LogLevel::info : LogLevel::error, "probe",
              valid ? "resize_text_glyph_spacing_kept" : "resize_text_glyph_spacing_changed");
    return valid && restored;
}

} // namespace maicord
