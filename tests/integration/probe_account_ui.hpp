#pragma once
#include "application_state.hpp"
#include "probe_capture.hpp"
#include "probe_glyph_ink.hpp"
#include "../../src/app/account_ui_runtime.hpp"
#include "../../src/render/d3d11_renderer_draw.hpp"
#include "../../src/render/d3d11_renderer_layout.hpp"
#include "maicord/render/renderer_presentation.hpp"

namespace maicord::app_detail {
// Points the account UI's renderer text storage at its shaping scratch and caps, as the
// shipping runtime does, with the probe fixture's own hit span, hit region and decoration
// arrays (sized kTextHitSpanMaximum, kRendererContentHitRegionCapacity and
// kRendererTextDecorationCapacity).
inline RendererTextStorage& probe_bind_renderer_text(AccountUiRuntime& ui, TextHitSpan* hit_spans,
                                                     TextHitRegion* hit_scratch,
                                                     TextDecoration* decorations) noexcept {
    auto& text = ui.renderer_text;
    text.quads = ui.renderer_text_quads;
    text.batches = ui.renderer_text_batches;
    text.utf16 = ui.response_shape_scratch.shape.utf16;
    text.glyphs = ui.response_shape_scratch.shape.glyphs;
    text.runs = ui.response_shape_scratch.shape.runs;
    text.clusters = ui.response_shape_scratch.shape.clusters;
    text.hit_spans = hit_spans;
    text.hit_region_scratch = hit_scratch;
    text.decorations = decorations;
    text.content_semantics = ui.renderer_content_semantics;
    text.content_hit_regions = ui.renderer_content_hit_regions;
    text.quad_capacity = kRendererContentGlyphCapacity;
    text.batch_capacity = kRendererContentBatchCapacity;
    text.utf16_capacity = text.glyph_capacity = text.cluster_capacity = kRendererContentTextUnitCapacity;
    text.run_capacity = kRendererContentRunCapacity;
    text.hit_span_capacity = kTextHitSpanMaximum;
    text.hit_region_scratch_capacity = text.content_hit_region_capacity = kRendererContentHitRegionCapacity;
    text.decoration_capacity = kRendererTextDecorationCapacity;
    text.content_semantic_capacity = kRendererContentSemanticCapacity;
    return text;
}

// The frame capture the composer and message-link probes share: project the scene, draw the
// shipping frame and read that exact target before any Present (flip-model Present advances to
// another back buffer). Fails with stages 101-106 when the typing-indicator `notice` is missing,
// mis-measured or inkless, or the `source_message`/`target_message` bodies have no ink, then
// hands the mapped pixels to `contract` and writes the capture. `require(ok, stage)` reports a
// failed stage.
template <typename Project, typename Require, typename Contract>
bool probe_capture_composer_frame(const wchar_t* filename, const Theme& theme, Utf8View notice,
                                  u64 source_message, u64 target_message, Project project,
                                  Require require, Contract contract) noexcept {
    if (!project())
        return false;
    auto& r = g_app.renderer;
    if (!d3d11_renderer_paint_layout(&r, theme, renderer_capture_presentation_stamp(r, theme)) ||
        !d3d11_renderer_draw_stream(&r, theme, true, true, nullptr, 10000))
        return require(false, 103);
    UiRect notice_bounds{};
    if (notice.bytes) {
        const auto* node = message_composer_scene_layout_node(g_app.message_composer_scene,
                                                              MessageComposerControl::typing_indicator);
        if (!node)
            return false;
        const auto bounds = ui_rect_intersection(node->content_rectangle, node->clip_rectangle);
        notice_bounds = bounds;
        if (!utf8_equal(message_composer_scene_typing_label(g_app.message_composer_scene), notice))
            return require(false, 104);
        const TextStyleSpan style{0, notice.bytes, text_style_subtext};
        UiTextMeasureValue measured{};
        const UiTextMeasureRequest request{{0x4d53474c494e4b55ull, 1, 1, 0, 1},
                                           notice,
                                           {&style, 1},
                                           {ui_rect_width(bounds), ui_rect_height(bounds)},
                                           true};
        if (!ui_rect_has_area(bounds) ||
            !renderer_text_measure_backend(&g_app.renderer, request, &measured) ||
            measured.line_count != 1 || measured.size.width > ui_rect_width(bounds) + .01f ||
            measured.size.height > ui_rect_height(bounds) + .01f)
            return require(false, 101);
    }
    ProbeTargetReadback readback;
    if (const u32 failure = probe_target_readback(r, &readback))
        return failure == 2 ? require(false, 102) : false;
    const auto& mapped = readback.mapped;
    if (notice.bytes && !probe_glyph_ink(mapped, notice_bounds))
        return require(false, 105);
    bool body_present{}, body_pixels{};
    const auto& text = g_app.account_ui->message_text;
    for (u32 i = 0; i < text.command_count; ++i) {
        const auto& command = text.commands[i];
        if (command.measurement_kind != MessageTextMeasurementKind::content || !command.text.bytes ||
            (command.message_id != source_message && command.message_id != target_message))
            continue;
        const auto bounds = ui_rect_intersection(command.rectangle, command.clip);
        if (!ui_rect_has_area(bounds))
            continue;
        body_present = true;
        body_pixels = body_pixels || probe_glyph_ink(mapped, bounds);
    }
    if (g_app.message_view.store.count && (!body_present || !body_pixels))
        return require(false, 106);
    if (!contract(mapped))
        return false;
    return probe_write_capture(readback, filename);
}
} // namespace maicord::app_detail
