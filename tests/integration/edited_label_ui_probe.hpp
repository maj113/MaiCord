#pragma once

#include "ui_surface_motion_measurement.hpp"
#include "../../src/render/d3d11/renderer_layout.hpp"
#include "maicord/app/chat/messages/view_runtime.hpp"
#include "maicord/render/text/content_text_retention.hpp"
#include "maicord/model/chat/message_rich_store.hpp"
#include "maicord/ui/chat/messages/text_presentation.hpp"

namespace maicord {
namespace edited_label_probe_detail {
inline constexpr u64 kBase = 1'271'000'000'700'000'000ull;
inline constexpr u64 kEditedMs = 1'700'000'100'000ull;
struct Row {
    Utf8View text;
    bool edited;
};
// Short rows come first: a label that wraps or sits on a line of its own moves only the rows after it.
inline constexpr Row kRows[] = {
    {utf8_literal("Edited message text."), true},
    {utf8_literal("A message nobody has edited."), false},
    {utf8_literal("**Bold** text and a `code` span, edited."), true},
    {utf8_literal("Another plain message."), false},
    {utf8_literal("This edited message is long enough that its last line fills up and the label has to wrap like a "
                  "word when there is no room left on that line for it, so keep reading a little longer than "
                  "that and then there are even more words here."),
     true},
    {Utf8View{}, true},
    {utf8_literal("```\nfenced code\n```"), true},
};
inline constexpr u32 kRowCount = sizeof(kRows) / sizeof(kRows[0]);

inline const MessageTextCommand* label_command(const D3D11Renderer& renderer, u64 message_id) noexcept {
    const auto* text = renderer.message_text;
    for (u32 i = 0; text && i < text->command_count; ++i) {
        const auto& command = text->commands[i];
        if (command.message_id != message_id)
            continue;
        for (u32 s = 0; s < command.semantics.count; ++s)
            if (command.semantics[s].kind == MessageMarkdownSemanticKind::edited)
                return &command;
    }
    return nullptr;
}

// The label's first hit region of a message in the drawn text, in DIPs.
inline bool label_region(const D3D11Renderer& renderer, u64 message_id, UiRect* region, u64* target) noexcept {
    const RendererTextStorage& storage = *renderer.content_text;
    for (u32 i = 0; i < storage.content_hit_region_count; ++i) {
        const TextHitRegion& hit = storage.content_hit_regions[i];
        if (hit.semantic_index >= storage.content_semantic_count)
            continue;
        const MessageTextSemantic& semantic = storage.content_semantics[hit.semantic_index];
        if (semantic.kind != MessageMarkdownSemanticKind::edited || semantic.message_id != message_id)
            continue;
        *region = {hit.left, hit.top, hit.right, hit.bottom};
        *target = semantic.target;
        return true;
    }
    return false;
}

// The rightmost column of `area` (DIPs) that differs from the corner of the area's empty end.
inline float rightmost_ink(const surface_motion_probe_detail::Readback& rb, u32 image, UiRect area,
                           float scale) noexcept {
    const auto region = raster_clip_rectangle(area, scale, rb.width, rb.height);
    if (region.right <= region.left || region.bottom <= region.top)
        return area.left;
    const u8* reference = rb.images + image * rb.bytes + (region.top * rb.width + region.right - 1) * 4;
    float right = area.left;
    for (i32 y = region.top; y < region.bottom; ++y)
        for (i32 x = region.left; x < region.right; ++x)
            if (surface_motion_probe_detail::distance(rb.images + image * rb.bytes + (y * rb.width + x) * 4,
                                                      reference) > 40)
                right = maximum(right, static_cast<float>(x + 1) / scale);
    return right;
}
} // namespace edited_label_probe_detail

// Edited messages draw "(edited)" small and muted after the last glyph of their text (below a code block, on a line
// of its own for a message without text), unedited ones draw as before: the same layout with and without edit times
// differs only where the labels are. The real hit test finds each label with its edit time, and a click target of
// the text (a link) never reports it. Writes edited-label-before.bmp (no edit times: what the client drew before it
// showed them) and edited-label-after.bmp when MAICORD_UI_PROBE_CAPTURE_DIR names a directory.
inline bool edited_label_ui_probe(D3D11Renderer& renderer, MessageViewRuntime& view, MessageRichStore& rich,
                                  MessageScene& scene, UserStore& users, bool (*rebuild_model)() noexcept,
                                  bool (*capture)(const wchar_t*) noexcept) noexcept {
    using namespace surface_motion_probe_detail;
    using namespace edited_label_probe_detail;
    u32 stage = 1;
    struct Failure {
        u32& stage;
        ~Failure() noexcept {
            if (stage)
                log_u64(LogLevel::error, "probe", "edited_label_failure_stage", stage);
        }
    } failure{stage};
    if (!rebuild_model || !capture || renderer.message_scene != &scene || !renderer.content_text)
        return false;
    const auto publish = [&](bool edited) noexcept {
        for (u32 i = 0; i < kRowCount; ++i) {
            MessageInput input{};
            input.id = kBase + i;
            input.channel_id = 111;
            input.author_id = kBase + 10'000 + i;
            input.timestamp_ms = snowflake_timestamp_ms(input.id);
            input.edited_timestamp_ms = edited && kRows[i].edited ? kEditedMs : 0;
            input.content = kRows[i].text;
            if (!message_store_upsert(&view.store, input).ok() ||
                !user_store_upsert(&users, input.author_id, utf8_literal("Native Author")).ok())
                return false;
        }
        return true;
    };
    if (!message_timeline_reset(&view.timeline) || !publish(true) ||
        !message_view_runtime_reindex(&view, ui_extent_units(kDefaultUiBootstrapMetrics.message_row_height), 1, true)
             .ok() ||
        !message_view_runtime_bind_rich_store(&view, &rich).ok() || !message_timeline_set_scroll(&view.timeline, 0, false) ||
        !rebuild_model() || !rebuild_model())
        return false;
    Readback readback{};
    if (!readback.initialize(renderer))
        return false;
    const float scale = static_cast<float>(renderer.dpi) / 96.f;
    const auto frame = [&](u32 index) noexcept {
        content_text_retention_invalidate(&renderer.content_text_retention);
        renderer.retained_presentation.valid = false;
        const float clear[]{kDefaultTheme.base_lowest.red, kDefaultTheme.base_lowest.green,
                            kDefaultTheme.base_lowest.blue, 1.f};
        if (!renderer_rebuild_content_text(&renderer, kDefaultTheme) ||
            !d3d11_renderer_paint_layout(&renderer, kDefaultTheme,
                                         renderer_capture_presentation_stamp(renderer, kDefaultTheme)))
            return false;
        renderer.graphics.context->ClearRenderTargetView(renderer.graphics.surface.target, clear);
        return d3d11_renderer_draw_stream(&renderer, kDefaultTheme, false, false, nullptr, 10000) &&
               readback.read(renderer, index);
    };
    // Every row on screen, and the geometry of the edited frame.
    UiRect labels[kRowCount]{}, rows[kRowCount]{}, lines[kRowCount]{};
    u64 targets[kRowCount]{};
    stage = 2;
    if (!frame(0))
        return false;
    for (u32 i = 0; i < kRowCount; ++i) {
        const u64 id = kBase + i;
        const auto found = message_store_find(view.store, id);
        const auto* row = found.ok() ? message_scene_find_row(&scene, found.order_index) : nullptr;
        const auto* node = row ? message_scene_layout_node(scene, row->widget) : nullptr;
        if (!node || !ui_rect_has_area(ui_rect_intersection(node->rectangle, scene.viewport)))
            return log_u64(LogLevel::error, "probe", "edited_label_row_not_visible", i), false;
        rows[i] = node->rectangle;
        const auto* command = label_command(renderer, id);
        if (command)
            lines[i] = command->rectangle;
        const bool has_label = label_region(renderer, id, &labels[i], &targets[i]);
        stage = 3;
        if (has_label != kRows[i].edited || (command != nullptr) != kRows[i].edited)
            return log_u64(LogLevel::error, "probe", "edited_label_wrong_rows", i), false;
        if (has_label && (targets[i] != kEditedMs / 1000 || !ui_rect_has_area(labels[i])))
            return false;
    }
    stage = 4;
    if (!capture(L"edited-label-after.bmp"))
        return false;
    // The real hit test: the label of each edited row reports its message and edit time; a point on the text
    // before it does not, and the label is no click target (a link would be).
    for (u32 i = 0; i < kRowCount; ++i) {
        if (!kRows[i].edited)
            continue;
        const float x = (labels[i].left + labels[i].right) * .5f, y = (labels[i].top + labels[i].bottom) * .5f;
        const auto hit = renderer_edited_label_hit_test(renderer, x, y);
        const auto content = renderer_content_hit_test(renderer, static_cast<i32>(x * scale), static_cast<i32>(y * scale));
        stage = 5;
        if (!hit.valid || hit.message_id != kBase + i || hit.edited_unix_seconds != kEditedMs / 1000 ||
            !ui_rect_contains(hit.rectangle, x, y) || content.valid)
            return log_u64(LogLevel::error, "probe", "edited_label_hit_failed", i), false;
        const bool beside = labels[i].left - 12.f > lines[i].left;
        const auto before = renderer_edited_label_hit_test(renderer, labels[i].left - 12.f, y);
        if (beside && before.valid && before.message_id == kBase + i && ui_rect_contains(before.rectangle, labels[i].left - 12.f, y))
            return log_u64(LogLevel::error, "probe", "edited_label_hit_outside", i), false;
    }
    // The same rows without edit times: what the client drew before.
    stage = 6;
    if (!publish(false) || !rebuild_model() || !rebuild_model() || !frame(1) ||
        !capture(L"edited-label-before.bmp"))
        return false;
    for (u32 i = 0; i < kRowCount; ++i) {
        stage = 7;
        UiRect none{};
        u64 time = 0;
        if (label_command(renderer, kBase + i) || label_region(renderer, kBase + i, &none, &time))
            return log_u64(LogLevel::error, "probe", "edited_label_left_behind", i), false;
    }
    // Only the labels differ: the plain rows above every wrapped label match pixel for pixel, the labelled rows
    // differ at their labels, and each label starts after the text's last ink.
    u32 label_pixels = 0, plain_pixels = 0;
    for (u32 i = 0; i < 4; ++i) {
        stage = 8;
        const UiRect area = kRows[i].edited ? labels[i] : rows[i];
        const u32 changed = [&]() noexcept {
            const auto region = raster_clip_rectangle(area, scale, readback.width, readback.height);
            u32 count = 0;
            for (i32 y = maximum(0, region.top); y < region.bottom; ++y)
                for (i32 x = maximum(0, region.left); x < region.right; ++x) {
                    const u32 at = (y * readback.width + x) * 4;
                    count += distance(readback.images + at, readback.images + readback.bytes + at) > 8 ? 1u : 0u;
                }
            return count;
        }();
        if (kRows[i].edited) {
            label_pixels += changed;
            const UiRect text_line{lines[i].left, labels[i].top, labels[i].left, labels[i].bottom};
            const float text_end = rightmost_ink(readback, 1, {lines[i].left, labels[i].top, scene.viewport.right, labels[i].bottom}, scale);
            log_u64(LogLevel::info, "probe", "edited_label_pixels", changed);
            if (changed < 8 || labels[i].left + 1.f < text_end - 0.5f || !ui_rect_has_area(text_line))
                return log_u64(LogLevel::error, "probe", "edited_label_not_after_text", i), false;
        } else {
            plain_pixels += changed;
        }
    }
    log_u64(LogLevel::info, "probe", "edited_label_plain_row_pixels_changed", plain_pixels);
    if (label_pixels < 16 || plain_pixels)
        return false;
    stage = 0;
    log_write(LogLevel::info, "probe", "edited_label_inline_below_code_alone_tooltip_hit_and_pixels_passed");
    return true;
}
} // namespace maicord
