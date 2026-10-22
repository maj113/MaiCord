#pragma once
#include "maicord/ui/emoji_catalog.hpp"

namespace maicord {
// A wide picker used to reserve one batch per emoji, exhausting the message
// budget and discarding all message text even before any atlas eviction.
inline bool picker_text_capacity_probe(D3D11Renderer& renderer) noexcept {
    UiMediaPickerScene scene{};
    MessageTextCommand commands[64]{};
    MessageTextPresentation messages{};
    messages.commands = {commands, 64};
    messages.command_count = 64;
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
    for (u32 i = 0; i < 64; ++i) {
        auto& command = commands[i];
        command.text = utf8_literal("Visible message text");
        command.rectangle = {area.left + 4, area.top + 4 + (i % 20) * 20.f,
                             area.left + 200, area.top + 24 + (i % 20) * 20.f};
        command.clip = area;
        command.clip.right -= i * .1f;
        command.color = kDefaultTheme.text_primary;
    }
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
            out->glyph = entries[index].glyph();
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
            {0, 0, renderer.width / scale, renderer.height / scale}, anchor->rectangle, nullptr, 760))
        return false;
    renderer.media_picker_scene = &scene;
    surface_motion_probe_detail::Readback readback;
    if (!readback.initialize(renderer)) return false;
    u64 initial = 0;
    for (u32 page = 0; page < 12; ++page) {
        if (page) ui_media_picker_scene_scroll(&scene, scene.cell_pitch * 8 + .37f);
        renderer.retained_presentation.valid = false;
        if (!renderer_rebuild_content_text(&renderer, kDefaultTheme) ||
            renderer.content_text->quad_count < 64 * 16 || renderer.content_text->batch_count > 90 ||
            !surface_motion_probe_detail::draw_fixed(renderer) || !readback.read(renderer, 0))
            return false;
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
