#pragma once
#include "../../src/render/picker_glyph_cache.hpp"
#include "maicord/ui/emoji_catalog.hpp"

namespace maicord {
// A wide picker used to reserve one batch per emoji, exhausting the message
// budget and discarding all message text even before any atlas eviction.
// Distinct row/card clips must also merge when they do not cut any ink;
// 160 such message commands exceed the 128-batch budget without this merging.
// Uncut shell and picker labels must also share batches within their layers.
inline bool picker_text_capacity_probe(D3D11Renderer& renderer) noexcept {
    UiMediaPickerScene scene{};
    Theme theme = kDefaultTheme;
    // Exercise cached foreground RGB/alpha as well as color-atlas quads.
    theme.text_primary = {.25f, .5f, .75f, .5f};
    constexpr u32 command_count = 160;
    MessageTextCommand commands[command_count]{};
    MessageTextPresentation messages{};
    messages.commands = {commands, command_count};
    messages.command_count = command_count;
    messages.active = true;
    struct Restore {
        D3D11Renderer& renderer;
        UiMediaPickerScene& scene;
        const UiMediaPickerScene* picker;
        const MessageTextPresentation* messages;
        ~Restore() noexcept {
            renderer.media_picker_scene = picker;
            renderer.message_text = messages;
            renderer.retained_presentation.valid = false;
            renderer.content_text_prefix = {};
            content_text_retention_invalidate(&renderer.content_text_retention);
            ui_media_picker_scene_release(&scene);
        }
    } restore{renderer, scene, renderer.media_picker_scene, renderer.message_text};
    const auto area = renderer.message_scene->viewport;
    for (u32 i = 0; i < command_count; ++i) {
        auto& command = commands[i];
        command.text = utf8_literal("Visible message text");
        command.rectangle = {area.left + 4, area.top + 4 + (i % 20) * 20.f,
                             area.left + 200, area.top + 24 + (i % 20) * 20.f};
        command.clip = area;
        command.clip.right -= i * .1f;
        command.color = kDefaultTheme.text_primary;
    }
    // Keep a real cut through the glyphs on each edge among the mergeable rows.
    commands[0].clip.left = commands[0].rectangle.left + 24;
    commands[1].clip.right = commands[1].rectangle.left + 50;
    commands[2].clip.top = commands[2].rectangle.top + 12;
    commands[3].clip.bottom = commands[3].rectangle.top + 12;
    renderer.message_text = &messages;
    const auto catalog = unicode_emoji_catalog();
    UiMediaPickerSource source{nullptr, 0x71, 1, catalog.count, 1, {},
        +[](void*, u32 index, UiMediaPickerItem* out) noexcept {
            const auto entries = unicode_emoji_catalog();
            if (index >= entries.count) return false;
            *out = {};
            out->id = index + 1;
            out->kind = UiMediaPickerItemKind::emoji;
            out->label = entries[index].name();
            // A mixed cell must preserve both monochrome and color glyphs on reuse.
            out->glyph = index ? entries[index].glyph() : utf8_literal("A\xF0\x9F\x98\x80");
            out->enabled = true;
            return true;
        }};
    UiMediaPickerContent content{};
    content.account_scope = 0x71;
    content.query_generation = content.revision = 1;
    content.kind = UiMediaPickerKind::emoji;
    content.phase = UiMediaPickerPhase::ready;
    content.available_tabs = 7;
    content.edit.active = true;
    const float scale = renderer.dpi / 96.f;
    const auto* anchor = message_composer_scene_layout_node(*renderer.message_composer_scene,
                                                           MessageComposerControl::gif);
    if (!anchor || !ui_media_picker_scene_update(&scene, &source, &content,
            {0, 0, renderer.width / scale, renderer.height / scale}, anchor->rectangle, 760))
        return false;
    renderer.media_picker_scene = &scene;
    surface_motion_probe_detail::Readback readback;
    if (!readback.initialize(renderer)) return false;
    u64 initial = 0;
    for (u32 page = 0; page < 12; ++page) {
        if (page) ui_media_picker_scene_scroll(&scene, scene.cell_pitch * 8 + .37f);
        renderer.retained_presentation.valid = false;
        if (!renderer_rebuild_content_text(&renderer, theme) ||
            renderer.content_text->quad_count < command_count * 16 || renderer.content_text->batch_count > 18 ||
            !surface_motion_probe_detail::draw_fixed(renderer) || !readback.read(renderer, 0))
            return false;
        // A warm rebuild reuses centered, unclipped cell-local glyphs. It must
        // preserve the cold pixels even at fractional scrolling positions.
        if (!renderer.picker_glyph_cache) return false;
        const u32 hits = renderer.picker_glyph_cache->hits;
        content_text_retention_invalidate(&renderer.content_text_retention);
        if (!renderer_rebuild_content_text(&renderer, theme) ||
            renderer.picker_glyph_cache->hits <= hits ||
            !surface_motion_probe_detail::draw_fixed(renderer) || !readback.read(renderer, 1) ||
            !bytes_equal(readback.images, readback.images + readback.bytes, readback.bytes))
            return false;
        for (u32 edge = 0; edge < 4; ++edge) {
            bool kept_clip = false;
            for (u32 b = 0; b < renderer.content_text->batch_count; ++b)
                kept_clip |= ui_rect_equal(renderer.content_text->batches[b].clip, commands[edge].clip);
            if (!kept_clip) return false;
        }
        u64 hash = 0;
        const u32 right = static_cast<u32>(maximum(0.f, scene.panel.left * scale - 4));
        for (u32 y = 0; y < renderer.height; ++y)
            hash = surface_motion_probe_detail::bytes_hash(hash, readback.images + y * renderer.width * 4,
                                                           right * 4);
        if (!page) initial = hash;
        else if (hash != initial) return false;
    }
    return true;
}
} // namespace maicord
