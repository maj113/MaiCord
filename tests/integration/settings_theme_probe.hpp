#pragma once
#include "background_theme_probe.hpp"
#include "probe_present.hpp"
#include "maicord/app/client_theme.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/render/layer_presentation.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "../../src/render/d3d11_renderer_draw.hpp"

#include "maicord/sys/d3d11.hpp"

namespace maicord {
namespace settings_theme_probe_detail {
inline bool palette(void*, u64, u32 choice, BackgroundTheme* out) noexcept {
    auto prefs = client_preferences_defaults();
    prefs.theme = static_cast<ClientTheme>(choice);
    const auto theme = client_theme_projection(prefs);
    *out = theme.background;
    if (!background_theme_valid(*out)) {
        const auto color = theme.base_low;
        const u32 rgb = (static_cast<u32>(color.red * 255 + .5f) << 16) |
                        (static_cast<u32>(color.green * 255 + .5f) << 8) |
                        static_cast<u32>(color.blue * 255 + .5f);
        *out = {{rgb, rgb, 0, 0, 0}, 0, 2, 100, BackgroundEffect::none, 50, {}};
    }
    return true;
}
struct Pixels {
    D3D11Renderer& r;
    ID3D11Texture2D* staging{};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    u32 width{}, height{};
    bool bgra{}, ready{};
    explicit Pixels(D3D11Renderer& renderer) noexcept : r(renderer) {
        ID3D11Resource* resource = nullptr;
        ID3D11Texture2D* source = nullptr;
        if (!r.graphics.target)
            return;
        r.graphics.target->GetResource(&resource);
        const HRESULT queried = resource ? resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                                                    reinterpret_cast<void**>(&source))
                                         : E_FAIL;
        com_release(resource);
        if (FAILED(queried))
            return;
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        width = desc.Width;
        height = desc.Height;
        bgra = desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
        if (!bgra && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
            com_release(source);
            return;
        }
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(r.graphics.device->CreateTexture2D(&desc, nullptr, &staging))) {
            r.graphics.context->CopyResource(staging, source);
            ready = SUCCEEDED(r.graphics.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
        }
        com_release(source);
    }
    ~Pixels() noexcept {
        if (ready)
            r.graphics.context->Unmap(staging, 0);
        com_release(staging);
    }
    bool point(float x, float y, u32* out) const noexcept {
        const float scale = r.dpi / 96.f;
        const i32 px = static_cast<i32>(x * scale), py = static_cast<i32>(y * scale);
        if (!out || !ready || px < 0 || py < 0 || static_cast<u32>(px) >= width ||
            static_cast<u32>(py) >= height)
            return false;
        const auto* pixel =
            static_cast<const u8*>(mapped.pData) + static_cast<usize>(py) * mapped.RowPitch + px * 4;
        *out = (static_cast<u32>(pixel[bgra ? 2 : 0]) << 16) | (static_cast<u32>(pixel[1]) << 8) |
               pixel[bgra ? 0 : 2];
        return true;
    }
    bool ink(UiRect area, Color color) const noexcept {
        const u32 expected = (static_cast<u32>(color.red * 255 + .5f) << 16) |
                             (static_cast<u32>(color.green * 255 + .5f) << 8) |
                             static_cast<u32>(color.blue * 255 + .5f);
        u32 found = 0;
        for (float y = area.top; y < area.bottom; y += 1)
            for (float x = area.left; x < area.right; x += 1) {
                u32 value = 0;
                if (!point(x, y, &value))
                    continue;
                bool close = true;
                for (u32 channel = 0; channel < 3; ++channel) {
                    const i32 delta = static_cast<i32>((value >> (channel * 8)) & 255) -
                                      static_cast<i32>((expected >> (channel * 8)) & 255);
                    close = close && delta >= -12 && delta <= 12;
                }
                if (close && ++found >= 4)
                    return true;
            }
        return false;
    }
};
} // namespace settings_theme_probe_detail
inline bool settings_theme_ui_probe(BackgroundThemeProbeContext c) noexcept {
    using namespace settings_theme_probe_detail;
    auto& r = c.renderer;
    UiSettingsScene scene{};
    UiPage page{};
    UiPageRegistry registry{};
    const auto* previous = r.settings_scene;
    const auto* previous_editor = r.theme_editor_scene;
    const float previous_opacity = r.layer_opacity.settings_page;
    struct Restore {
        D3D11Renderer& r;
        UiSettingsScene& scene;
        const UiSettingsScene* previous;
        const UiThemeEditorScene* editor;
        float opacity;
        ~Restore() noexcept {
            renderer_bind_settings_scene(&r, previous);
            renderer_bind_theme_editor_scene(&r, editor);
            renderer_set_layer_opacity(&r, RendererLayerSurface::settings_page, opacity);
            ui_settings_scene_release(&scene);
        }
    } restore{r, scene, previous, previous_editor, previous_opacity};
    const Utf8View presets[]{utf8_literal("Dark"),   utf8_literal("AMOLED"),
                             utf8_literal("Aurora"), utf8_literal("Sunset"),
                             utf8_literal("Ocean"),  utf8_literal("Custom Gradient"),
                             utf8_literal("Light"),  utf8_literal("Custom Light")};
    char number_text[101][4]{};
    Utf8View numbers[101]{};
    for (u32 i = 0; i <= 100; ++i) {
        u32 at = 0;
        if (i >= 100)
            number_text[i][at++] = '1';
        if (i >= 10)
            number_text[i][at++] = static_cast<char>('0' + (i / 10) % 10);
        number_text[i][at++] = static_cast<char>('0' + i % 10);
        numbers[i] = {number_text[i], at};
    }
    UiSettingsControl controls[]{
        {1, utf8_literal("Theme"), {}, UiSettingsControlKind::palette, true, false, {presets, 8}, 2},
        {2,
         utf8_literal("Gradient intensity"),
         {},
         UiSettingsControlKind::slider,
         true,
         false,
         {numbers, 101},
         40},
        {3,
         utf8_literal("Animation speed"),
         {},
         UiSettingsControlKind::slider,
         true,
         false,
         {numbers, 101},
         50},
        {4,
         utf8_literal("Preview value"),
         {},
         UiSettingsControlKind::slider,
         true,
         false,
         {numbers, 101},
         75}};
    UiSettingsPageContent content{utf8_literal("Appearance"),
                                  utf8_literal("Original offline preset and slider fixture"),
                                  {controls, 4},
                                  1};
    int context = 0;
    const UiRect viewport{0, 0, r.width * 96.f / r.dpi, r.height * 96.f / r.dpi};
    if (!ui_page_registry_initialize(&registry, {&page, 1}) ||
        !ui_page_registry_register(&registry, {{1, 1}, utf8_literal("Appearance"), &content}).ok() ||
        !ui_settings_scene_bind_palette(&scene, {&context, palette, 1}) ||
        !ui_settings_scene_update(&scene, &registry, viewport))
        return false;
    renderer_bind_theme_editor_scene(&r, nullptr);
    renderer_bind_settings_scene(&r, &scene);
    if (r.settings_scene != &scene)
        return false;
    renderer_set_layer_opacity(&r, RendererLayerSurface::settings_page, 1);
    auto theme_preferences = client_preferences_defaults();
    theme_preferences.theme = ClientTheme::custom_gradient;
    Theme theme = client_theme_projection(theme_preferences);
    const auto draw = [&]() noexcept {
        return renderer_rebuild_content_text(&r, theme) && probe_present(&r, theme) &&
               d3d11_renderer_draw_stream(&r, theme, true, true, nullptr);
    };
    if (!draw() || r.settings_page_paint_first_batch >= r.settings_page_paint_end_batch ||
        r.settings_page_text_first_batch >= r.settings_page_text_end_batch)
        return false;
    UiTextCommand commands[64]{};
    UiTextCommandStream stream{};
    UiTextOutput stream_output{&stream, nullptr};
    u32 sidebar = 0, card = 0;
    {
        Pixels pixels(r);
        if (!ui_text_command_stream_initialize(&stream, {commands, 64}) ||
            !ui_settings_scene_text_output(scene, theme, &stream_output))
            return false;
        u32 labels = 0;
        for (u32 i = 0; i < stream.count; ++i)
            for (const auto label : presets)
                if (utf8_equal(commands[i].text, label)) {
                    if (!pixels.ink(ui_rect_intersection(commands[i].rectangle, commands[i].clip),
                                    commands[i].color))
                        return false;
                    ++labels;
                }
        UiSettingsControlGeometry cards{};
        if (labels != 8 ||
            !ui_settings_scene_control_geometry(scene, {UiSettingsTargetKind::control, {1, 1}, 1}, &cards))
            return false;
        const auto tokens = ui_settings_tokens(theme);
        const auto tile = ui_swatch_rect(tokens, cards.field, 2, ui_swatch_columns(tokens, ui_rect_width(cards.field)));
        if (!pixels.point(20, 20, &sidebar) ||
            !pixels.point((tile.left + tile.right) * .5f, (tile.top + tile.bottom) * .5f, &card))
            return false;
        UiSettingsSliderGeometry slider{};
        if (!ui_settings_scene_slider_geometry(scene, {UiSettingsTargetKind::control, {1, 1}, 2},
                                               &slider) ||
            !pixels.ink(slider.thumb, tokens.color.thumb))
            return false;
    }
    renderer_set_layer_opacity(&r, RendererLayerSurface::settings_page, .5f);
    if (!probe_present(&r, theme) || !d3d11_renderer_draw_stream(&r, theme, true, true, nullptr))
        return false;
    {
        Pixels pixels(r);
        u32 current_sidebar = 0, current_card = 0;
        UiSettingsControlGeometry cards{};
        const auto tokens = ui_settings_tokens(theme);
        const bool cards_valid = ui_settings_scene_control_geometry(scene, {UiSettingsTargetKind::control, {1, 1}, 1},
                                                                    &cards);
        const auto tile = ui_swatch_rect(tokens, cards.field, 2, ui_swatch_columns(tokens, ui_rect_width(cards.field)));
        if (!cards_valid || !pixels.point(20, 20, &current_sidebar) ||
            !pixels.point((tile.left + tile.right) * .5f, (tile.top + tile.bottom) * .5f, &current_card) ||
            current_sidebar != sidebar || current_card == card)
            return false;
    }
    renderer_set_layer_opacity(&r, RendererLayerSurface::settings_page, 1);
    return draw() && (!c.capture || c.capture(L"background-theme-settings.bmp", theme));
}
} // namespace maicord
