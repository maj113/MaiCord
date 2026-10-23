#pragma once

#include "ui_surface_motion_measurement.hpp"
#include "../../src/render/d3d11_renderer_layout.hpp"
#include "maicord/app/message_view_runtime.hpp"
#include "maicord/ui/message_author.hpp"
#include "maicord/ui/native_text_candidate.hpp"

namespace maicord {
inline bool hover_retention_ui_probe(D3D11Renderer& renderer, MessageViewRuntime& view, MessageScene& scene,
                                     UserStore& users, bool (*rebuild_model)() noexcept) noexcept {
    using namespace surface_motion_probe_detail;
    u32 stage = 1;
    struct Failure {
        u32& stage;
        ~Failure() noexcept {
            if (stage)
                log_u64(LogLevel::error, "probe", "hover_retention_failure_stage", stage);
        }
    } failure{stage};
    const auto* previous_bar = renderer.message_action_bar;
    struct Restore {
        D3D11Renderer& renderer;
        const MessageActionBar* bar;
        ~Restore() noexcept {
            renderer.message_action_bar = bar;
        }
    } restore{renderer, previous_bar};
    renderer.message_action_bar = nullptr;
    if (!rebuild_model || renderer.message_scene != &scene || renderer.user_store != &users)
        return false;
    if (!renderer.ui_scene) return false;
    const auto* original_shell = renderer.ui_scene;
    auto shell = *original_shell;
    renderer.ui_scene = &shell;
    const auto baseline_stamp = renderer_capture_presentation_stamp(renderer, kDefaultTheme);
    shell.width += .25f;
    const auto resized_stamp = renderer_capture_presentation_stamp(renderer, kDefaultTheme);
    shell.width = original_shell->width;
    auto changed_theme = kDefaultTheme;
    changed_theme.text_primary.alpha *= .5f;
    const auto themed_stamp = renderer_capture_presentation_stamp(renderer, changed_theme);
    renderer.ui_scene = original_shell;
    // Both a four-byte tail and a complete theme block affect frame retention.
    if (!baseline_stamp.valid || !resized_stamp.valid || !themed_stamp.valid ||
        renderer_presentation_stamp_equal(baseline_stamp, resized_stamp) ||
        renderer_presentation_stamp_equal(baseline_stamp, themed_stamp) ||
        baseline_stamp.count != resized_stamp.count || baseline_stamp.count != themed_stamp.count)
        return false;
    constexpr u64 base = 1'270'000'000'100'000'000ull;
    for (u32 i = 0; i < 12; ++i)
        if (!message_store_upsert(&view.store,
                                  {base + i, 111, base + i + 10000, snowflake_timestamp_ms(base + i), 0,
                                   utf8_literal("Original native row for controlled hover retention."), 0})
                 .ok() ||
            !user_store_upsert(&users, base + i + 10000, utf8_literal("Native Author")).ok())
            return false;
    if (!message_view_runtime_reindex(&view, ui_extent_units(kDefaultUiBootstrapMetrics.message_row_height),
                                      1, true)
             .ok() ||
        !rebuild_model() || scene.count < 4)
        return false;
    const auto prepare = [&]() noexcept {
        return d3d11_renderer_paint_layout(&renderer, kDefaultTheme,
                                           renderer_capture_presentation_stamp(renderer, kDefaultTheme));
    };
    const auto draw_changed = [&]() noexcept {
        const float clear[]{kDefaultTheme.base_lowest.red, kDefaultTheme.base_lowest.green,
                            kDefaultTheme.base_lowest.blue, 1};
        renderer.graphics.context->ClearRenderTargetView(renderer.graphics.target, clear);
        return d3d11_renderer_draw_stream(&renderer, kDefaultTheme, true, false, nullptr, 10000);
    };
    ui_widget_manager_hover(&scene.widgets, ui_widget_invalid_handle());
    if (!renderer_rebuild_content_text(&renderer, kDefaultTheme) || !prepare() || !draw_changed())
        return false;
    // The owned scene also materializes preparation rows outside the viewport.
    // Exercise genuinely visible headers rather than assuming local row indices.
    u32 visible[3]{}, visible_count = 0;
    for (u32 i = 0; i < scene.count && visible_count < 3; ++i) {
        const auto& row = scene.rows[i];
        const auto* node = message_scene_layout_node(scene, row.widget);
        if (!node || row.grouped)
            continue;
        const auto clipped = ui_rect_intersection(node->rectangle, node->clip_rectangle);
        if (ui_rect_has_area(clipped) && ui_rect_height(clipped) >= ui_rect_height(node->rectangle) - .5f)
            visible[visible_count++] = i;
    }
    if (visible_count != 3)
        return false;
    const u32 prefix = renderer.content_text_prefix.quads, glyph_builds = renderer.glyph_text.cache_builds,
              text_builds = renderer.content_text->rebuilds, text_uploads = renderer.text_instance_uploads;
    const u64 prefix_hash =
        bytes_hash(1469598103934665603ull, renderer.content_text->quads, prefix * sizeof(GlyphQuad));
    const auto native_before = native_text_candidate_counts(renderer.glyph_text.system);
    stage = 2;
    for (u32 i = 0; i < 32; ++i) {
        const auto& row = scene.rows[visible[i % 3]];
        ui_widget_manager_hover(&scene.widgets, row.widget);
        scene.widgets.hover_started_ms = 1000 + i;
        if (!renderer_rebuild_content_text(&renderer, kDefaultTheme) || !prepare() || !draw_changed() ||
            renderer.content_text_prefix.quads != prefix ||
            renderer.content_text->rebuilds != text_builds ||
            renderer.glyph_text.cache_builds != glyph_builds ||
            renderer.text_instance_uploads != text_uploads ||
            bytes_hash(1469598103934665603ull, renderer.content_text->quads, prefix * sizeof(GlyphQuad)) !=
                prefix_hash)
            return false;
    }
    const auto native_after = native_text_candidate_counts(renderer.glyph_text.system);
    if (native_after.shaped != native_before.shaped || native_after.measured != native_before.measured)
        return false;
    stage = 3;
    const auto row = scene.rows[visible[1]];
    const auto* message = message_store_resolve(view.store, row.message);
    if (!message || row.grouped)
        return false;
    UserAppearance appearance{};
    appearance.user_id = message->author_id;
    appearance.fields = appearance_name;
    appearance.name.effect_id = 1;
    appearance.name.color_count = 2;
    appearance.name.colors[0] = 0x00eaff;
    appearance.name.colors[1] = 0x9058ff;
    if (!user_appearance_upsert(&users.appearance, appearance))
        return false;
    ui_widget_manager_hover(&scene.widgets, ui_widget_invalid_handle());
    if (!renderer_rebuild_content_text(&renderer, kDefaultTheme) || !prepare() || !draw_changed() ||
        renderer.text_effect_count)
        return false;
    const u32 idle_prefix = renderer.content_text_prefix.quads;
    const u64 idle_hash =
        bytes_hash(1469598103934665603ull, renderer.content_text->quads, idle_prefix * sizeof(GlyphQuad));
    UiRect name{};
    const auto* presentation = renderer.message_text;
    for (u32 i = 0; presentation && i < presentation->command_count; ++i) {
        MessageAuthorLayout header{};
        const auto& command = presentation->commands[i];
        if (command.message_id == message->id &&
            message_author_layout(scene, command, *renderer.chat_state, users, nullptr, kDefaultTheme,
                                  &header)) {
            name = ui_rect_intersection(header.name.rectangle, header.name.clip);
            break;
        }
    }
    Readback readback{};
    if (!ui_rect_has_area(name) || !readback.initialize(renderer) || !readback.read(renderer, 0))
        return false;
    stage = 4;
    ui_widget_manager_hover(&scene.widgets, row.widget);
    scene.widgets.hover_started_ms = 1000;
    if (!renderer_rebuild_content_text(&renderer, kDefaultTheme) || !prepare() || !draw_changed() ||
        renderer.text_effect_count != 1 || !readback.read(renderer, 1))
        return false;
    const auto clip = raster_clip_rectangle(name, renderer.dpi / 96.f, readback.width, readback.height);
    u32 changed_ink = 0;
    for (i32 y = clip.top; y < clip.bottom; ++y)
        for (i32 x = clip.left; x < clip.right; ++x) {
            const u32 p = (y * readback.width + x) * 4;
            const auto* idle = readback.images + p;
            if (idle[0] > 120 && idle[1] > 120 && idle[2] > 120 &&
                distance(idle, readback.images + readback.bytes + p) > 30)
                ++changed_ink;
        }
    if (changed_ink < 10)
        return false;
    // Moving the same effect to the final slot, then back, exercises both
    // a full upload and a short prefix after WRITE_DISCARD without stale data.
    renderer.text_effects.materials[kTextEffectCapacity - 1] = renderer.text_effects.materials[0];
    constexpr u32 slots[]{kTextEffectCapacity - 1, 0};
    for (u32 slot : slots) {
        renderer.text_effect_count = slot + 1;
        for (u32 i = 0; i < renderer.content_text->quad_count; ++i)
            if (renderer.content_text->quads[i].color.red < 0)
                renderer.content_text->quads[i].color.red = -static_cast<float>(slot + 1);
        renderer.uploaded_text_storage = nullptr;
        if (!draw_changed() || !readback.read(renderer, 3) ||
            bytes_hash(0, readback.images + readback.bytes, readback.bytes) !=
                bytes_hash(0, readback.images + readback.bytes * 3, readback.bytes))
            return false;
    }
    stage = 5;
    ui_widget_manager_hover(&scene.widgets, ui_widget_invalid_handle());
    if (!renderer_rebuild_content_text(&renderer, kDefaultTheme) || !prepare() || !draw_changed() ||
        renderer.text_effect_count || renderer.content_text_prefix.quads != idle_prefix ||
        bytes_hash(1469598103934665603ull, renderer.content_text->quads, idle_prefix * sizeof(GlyphQuad)) !=
            idle_hash ||
        !readback.read(renderer, 2))
        return false;
    for (i32 y = clip.top; y < clip.bottom; ++y)
        for (i32 x = clip.left; x < clip.right; ++x) {
            const u32 p = (y * readback.width + x) * 4;
            if (distance(readback.images + p, readback.images + readback.bytes * 2 + p) > 2)
                return false;
        }
    const auto cached = snapshot(renderer);
    for (u32 i = 0; i < 8; ++i)
        if (!renderer_rebuild_content_text(&renderer, kDefaultTheme) || !prepare() ||
            !draw_fixed(renderer) || !same(cached, snapshot(renderer)))
            return false;
    stage = 0;
    log_u64(LogLevel::info, "probe", "hover_retention_authored_native_changed_ink", changed_ink);
    log_write(LogLevel::info, "probe",
              "hover_retention_plain_rows_zero_shape_upload_and_authored_restore_passed");
    return true;
}
} // namespace maicord
