#pragma once

#include "ui_surface_motion_measurement.hpp"
#include "../../src/render/d3d11_renderer_layout.hpp"
#include "maicord/app/message_view_runtime.hpp"
#include "maicord/ui/message_component_layout.hpp"
#include "maicord/ui/background_presentation.hpp"

namespace maicord {
namespace message_components_probe_detail {
// Pixels in `area` (DIPs) whose channels spread by more than `chroma`: colour
// emoji ink, never the grey button fill or the white label.
inline u32 coloured(const surface_motion_probe_detail::Readback& rb, u32 image, UiRect area, float scale,
                    u32 chroma) noexcept {
    const auto region = raster_clip_rectangle(area, scale, rb.width, rb.height);
    u32 count = 0;
    for (i32 y = maximum(0, region.top); y < region.bottom; ++y)
        for (i32 x = maximum(0, region.left); x < region.right; ++x) {
            const u8* p = rb.images + image * rb.bytes + (y * rb.width + x) * 4;
            const u8 high = maximum(p[0], maximum(p[1], p[2])), low = minimum(p[0], minimum(p[1], p[2]));
            count += static_cast<u32>(high - low) > chroma ? 1u : 0u;
        }
    return count;
}
// Pixels in `area` farther than `threshold` from the pixel at `reference`.
inline u32 ink(const surface_motion_probe_detail::Readback& rb, u32 image, UiRect area, UiPoint reference,
               float scale, u32 threshold) noexcept {
    const auto region = raster_clip_rectangle(area, scale, rb.width, rb.height);
    const i32 rx = static_cast<i32>(reference.x * scale), ry = static_cast<i32>(reference.y * scale);
    if (rx < 0 || ry < 0 || rx >= static_cast<i32>(rb.width) || ry >= static_cast<i32>(rb.height))
        return 0;
    const u8* base = rb.images + image * rb.bytes + (ry * rb.width + rx) * 4;
    u32 count = 0;
    for (i32 y = maximum(0, region.top); y < region.bottom; ++y)
        for (i32 x = maximum(0, region.left); x < region.right; ++x)
            count += surface_motion_probe_detail::distance(rb.images + image * rb.bytes + (y * rb.width + x) * 4,
                                                            base) > threshold
                         ? 1u
                         : 0u;
    return count;
}
inline const u8* pixel(const surface_motion_probe_detail::Readback& rb, u32 image, UiPoint point,
                       float scale) noexcept {
    const i32 x = static_cast<i32>(point.x * scale), y = static_cast<i32>(point.y * scale);
    if (x < 0 || y < 0 || x >= static_cast<i32>(rb.width) || y >= static_cast<i32>(rb.height))
        return nullptr;
    return rb.images + image * rb.bytes + (y * rb.width + x) * 4;
}
} // namespace message_components_probe_detail

// An application reply with two secondary buttons, as BlueprintBot sends them:
// a Unicode emoji ahead of "Zoom In" and a custom emoji ahead of "Download".
// Through the real layout, text and draw path, both buttons must show their
// chrome and both emoji must reach the pixels.
inline bool message_components_ui_probe(D3D11Renderer& renderer, MessageViewRuntime& view, MessageRichStore& rich,
                                        MessageScene& scene, UserStore& users, bool (*rebuild_model)() noexcept,
                                        bool (*synchronize)() noexcept) noexcept {
    using namespace surface_motion_probe_detail;
    using namespace message_components_probe_detail;
    u32 stage = 1;
    struct Failure {
        u32& stage;
        ~Failure() noexcept {
            if (stage)
                log_u64(LogLevel::error, "probe", "message_components_failure_stage", stage);
        }
    } failure{stage};
    if (!rebuild_model || !synchronize || renderer.message_scene != &scene)
        return false;
    constexpr u64 id = 1'270'000'000'300'000'000ull, author = id + 10'000, emoji_id = 123;
    if (!message_store_upsert(&view.store, {id, 111, author, snowflake_timestamp_ms(id), 0, Utf8View{}, 0}).ok() ||
        !user_store_upsert(&users, author, utf8_literal("BlueprintBot")).ok() ||
        !message_view_runtime_reindex(&view, ui_extent_units(kDefaultUiBootstrapMetrics.message_row_height), 1, true)
             .ok())
        return false;
    GatewayMessageComponent components[4]{};
    components[0].type = gateway_component_action_row;
    components[0].parent = 0xFFFFu;
    components[1].type = gateway_component_button;
    components[1].parent = 0;
    components[1].style = 2;
    components[1].label = utf8_literal("Zoom In");
    components[1].custom_id = utf8_literal("zoom");
    components[1].emoji_name = utf8_literal("\xF0\x9F\x94\x8E"); // U+1F50E
    components[2].type = gateway_component_button;
    components[2].parent = 0;
    components[2].order = 1;
    components[2].style = 2;
    components[2].label = utf8_literal("Download");
    components[2].custom_id = utf8_literal("download");
    components[2].emoji_id = emoji_id;
    components[2].emoji_name = utf8_literal("blueprint");
    // An opaque fill: the emoji must draw above the button, not beneath it.
    components[3].type = gateway_component_button;
    components[3].parent = 0;
    components[3].order = 2;
    components[3].style = 1;
    components[3].label = utf8_literal("Go");
    components[3].custom_id = utf8_literal("go");
    components[3].emoji_name = utf8_literal("\xF0\x9F\x94\x8E");
    GatewayMessageEvent event{};
    event.id = id;
    event.fields = gateway_message_field_components;
    event.components = ConstSlice<GatewayMessageComponent>{components, 4};
    const auto found = message_store_find(view.store, id);
    const auto* record = found.ok() ? message_store_resolve(view.store, found.handle) : nullptr;
    if (!record || !message_rich_store_apply(&rich, id, record->revision, event).ok())
        return false;
    // The custom emoji: a solid magenta square, premultiplied.
    u8 square[16 * 16 * 4];
    for (u32 i = 0; i < 16 * 16; ++i) {
        square[i * 4] = 255;
        square[i * 4 + 1] = 0;
        square[i * 4 + 2] = 255;
        square[i * 4 + 3] = 255;
    }
    stage = 2;
    if (!renderer_upload_media_rgba8(&renderer, media_asset_custom_emoji(emoji_id, false), 16, 16, 16 * 4,
                                     {square, sizeof(square)})
             .ok() ||
        !rebuild_model() || !rebuild_model())
        return false;
    Readback readback{};
    if (!readback.initialize(renderer))
        return false;
    // The flat default theme, a background theme whose surfaces sample the
    // global palette, and that theme at 125%: every pass must show the chrome
    // and every emoji.
    Theme background = kDefaultTheme;
    background.background = background_theme_defaults();
    const Theme* themes[]{&kDefaultTheme, &background, &background};
    const wchar_t* captures[]{L"message-components-buttons.bmp", L"message-components-buttons-background.bmp",
                              L"message-components-buttons-125.bmp"};
    struct Dpi {
        D3D11Renderer& renderer;
        bool (*synchronize)() noexcept;
        ~Dpi() noexcept {
            if (renderer.dpi != 96) {
                renderer_set_dpi(&renderer, 96);
                synchronize();
            }
        }
    } dpi{renderer, synchronize};
    for (u32 pass = 0; pass < 3; ++pass) {
        const Theme& theme = *themes[pass];
        stage = 3 + pass * 10;
        if (pass == 2) {
            renderer_set_dpi(&renderer, 120);
            if (!synchronize() || !rebuild_model() || !rebuild_model())
                return false;
        }
        const MessageSceneRowWidget* row = nullptr;
        for (u32 i = 0; i < scene.count; ++i)
            if (scene.rows[i].message_id == id)
                row = &scene.rows[i];
        const UiLayoutNode* nodes[3]{};
        for (u32 i = 0; row && i < 3; ++i)
            nodes[i] = message_scene_rich_item_layout_node(scene, *row, MessageSceneRichItemKind::component, i + 1);
        if (!nodes[0] || !nodes[1] || !nodes[2] ||
            !ui_rect_equal(ui_rect_intersection(nodes[0]->rectangle, scene.viewport), nodes[0]->rectangle) ||
            !ui_rect_equal(ui_rect_intersection(nodes[2]->rectangle, scene.viewport), nodes[2]->rectangle))
            return false;
        const UiRect z = nodes[0]->rectangle, d = nodes[1]->rectangle, g = nodes[2]->rectangle;
        const float middle = (z.top + z.bottom) * .5f;
        const float scale = static_cast<float>(renderer.dpi) / 96.f;
        const float clear[]{theme.base_lowest.red, theme.base_lowest.green, theme.base_lowest.blue, 1.f};
        stage = 4 + pass * 10;
        if (!renderer_rebuild_content_text(&renderer, theme) ||
            !d3d11_renderer_paint_layout(&renderer, theme, renderer_capture_presentation_stamp(renderer, theme)))
            return false;
        renderer.graphics.context->ClearRenderTargetView(renderer.graphics.target, clear);
        if (!d3d11_renderer_draw_stream(&renderer, theme, false, false, nullptr, 10000) ||
            !readback.read(renderer, pass) || !readback.capture(pass, captures[pass]))
            return false;
        // The chrome: inside the button (clear of label and corner) differs
        // from the timeline just below it.
        const u8* fill = pixel(readback, pass, {z.right - 5.f, middle}, scale);
        const u8* outside = pixel(readback, pass, {z.right - 5.f, z.bottom + 3.f}, scale);
        stage = 5 + pass * 10;
        if (!fill || !outside || distance(fill, outside) < 6)
            return false;
        // The Unicode emoji leads the label: colour ink in the emoji lane.
        const UiRect lane{z.left + kMessageComponentButtonPadding, z.top + 4.f,
                          z.left + kMessageComponentButtonPadding + kMessageComponentEmojiExtent, z.bottom - 4.f};
        const u32 unicode_colour = coloured(readback, pass, lane, scale, 48);
        const u32 unicode_ink = ink(readback, pass, lane, {z.right - 5.f, middle}, scale, 40);
        // The custom emoji is the uploaded magenta square.
        const UiRect custom{d.left + kMessageComponentButtonPadding + 3.f, middle - 6.f,
                            d.left + kMessageComponentButtonPadding + kMessageComponentEmojiExtent - 3.f,
                            middle + 6.f};
        const u32 custom_colour = coloured(readback, pass, custom, scale, 160);
        // On the opaque primary fill the emoji still shows: ink against the
        // fill in its lane, measured from the fill beside the label.
        const UiRect go_lane{g.left + kMessageComponentButtonPadding, g.top + 4.f,
                             g.left + kMessageComponentButtonPadding + kMessageComponentEmojiExtent, g.bottom - 4.f};
        const u32 go_ink = ink(readback, pass, go_lane, {g.right - 4.f, middle}, scale, 60);
        log_u64(LogLevel::info, "probe", "message_components_unicode_emoji_colour_pixels", unicode_colour);
        log_u64(LogLevel::info, "probe", "message_components_unicode_emoji_ink_pixels", unicode_ink);
        log_u64(LogLevel::info, "probe", "message_components_custom_emoji_pixels", custom_colour);
        log_u64(LogLevel::info, "probe", "message_components_opaque_emoji_ink_pixels", go_ink);
        stage = 6 + pass * 10;
        if (unicode_ink < 20 || unicode_colour < 10)
            return false;
        stage = 7 + pass * 10;
        if (custom_colour < 40)
            return false;
        stage = 8 + pass * 10;
        if (go_ink < 20)
            return false;
    }
    stage = 0;
    return true;
}
} // namespace maicord
