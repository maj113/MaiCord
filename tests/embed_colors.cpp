#include "maicord/ui/message_presentation.hpp"

namespace maicord {
namespace {
constexpr u32 widget_count = kMessageSceneWidgetsPerRow + 1;
struct Fixture {
    MessageRecord record[1];
    u32 order[1];
    u8 text[64];
    MessageRichRecord rich_record[1];
    u16 rich_order[1];
    u8 rich_text[64];
    u32 heights[1];
    u64 fenwick[1];
    UiWidget widgets[widget_count];
    UiLayoutNode nodes[widget_count];
    MessageStore store;
    MessageRichStore rich;
    UiVariableList list;
    MessageTimeline timeline;
    MessageScene scene;
    PaintStream paint;
};
Fixture fixture;

u32 rgb(const Color& color) noexcept {
    return (static_cast<u32>(color.red * 255.0f + 0.5f) << 16u) |
           (static_cast<u32>(color.green * 255.0f + 0.5f) << 8u) |
           static_cast<u32>(color.blue * 255.0f + 0.5f);
}
}

void test_embed_colors(void (*check)(bool, const char*) noexcept) noexcept {
    Fixture& f = fixture;
    bool ready = message_store_initialize(&f.store, {f.record, 1}, {f.order, 1}, {f.text, sizeof(f.text)}) &&
        message_store_upsert(&f.store, {1, 7, 9, 0, 0, utf8_literal("embeds"), 0}).ok() &&
        message_rich_store_initialize(&f.rich, {f.rich_record, 1}, {f.rich_order, 1}, {f.rich_text, sizeof(f.rich_text)}) &&
        ui_variable_list_initialize(&f.list, {f.heights, 1}, {f.fenwick, 1}, 1, ui_extent_units(57)) &&
        message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(57), ui_extent_units(1000), false) &&
        message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
        message_scene_initialize_owned(&f.scene, &f.timeline, 1, 1, {f.widgets, widget_count}, {f.nodes, widget_count});
    GatewayMessageEmbed embeds[4]{};
    for (auto& embed : embeds) embed.type = GatewayMessageEmbedType::rich;
    const u32 colors[2][4]{{0x20B7B7u, 0xF45253u, 0, 0}, {0xFFFFFFu, 0x0055FFu, 0, 0}};
    GatewayMessageEvent event{};
    event.id = 1;
    event.fields = gateway_message_field_embeds;
    event.embeds = {embeds, 4};
    const UiRect viewport{0, 0, 740, 1000};
    u32 layout_passes = 0;
    u32 data_updates = 0;
    bool colors_exact = ready;
    for (u32 pass = 0; ready && pass < 2; ++pass) {
        for (u32 i = 0; i < 4; ++i) {
            embeds[i].color = colors[pass][i];
            embeds[i].has_color = i < 2 || i == (pass == 0 ? 2u : 3u);
        }
        ready = message_rich_store_apply(&f.rich, 1, f.record[0].revision, event).ok();
        const auto updated = message_scene_update(&f.scene, viewport);
        ready = ready && updated.ok();
        if (pass != 0) {
            const auto* row = message_scene_find_row(f.scene, 0);
            check(updated.changed && row && row->text_measured && row->rich_layout_measured &&
                  f.scene.layout_passes == layout_passes && f.scene.data_updates == data_updates + 1,
                  "embed_color_only_repaint_preserves_measurements_and_layout");
        }
        paint_stream_reset(&f.paint, viewport.right, viewport.bottom);
        MessageMediaCommand commands[1]{};
        MessageMediaCommandStream media{};
        MessagePresentationResult result{};
        ready = ready && message_media_command_stream_initialize(&media, {commands, 1}) &&
            message_presentation_build(f.scene, kDefaultTheme, &f.paint, &media, &result);
        const auto* row = message_scene_find_row(f.scene, 0);
        for (u32 i = 0; ready && row && i < 4; ++i) {
            const UiLayoutNode* item = message_scene_rich_item_layout_node(f.scene, *row, MessageSceneRichItemKind::embed, i);
            const u32 expected = embeds[i].has_color ? embeds[i].color : rgb(kDefaultTheme.interactive);
            bool found = false;
            for (u32 q = 0; item && q < f.paint.count; ++q) {
                const PaintQuad& quad = f.paint.quads[q];
                // The accent is the card's leading border: full height, clipped to its corners.
                if (quad.left == item->rectangle.left && quad.top == item->rectangle.top &&
                    quad.right - quad.left == 4.0f && quad.border_width == kPaintEdgeStripMode &&
                    quad.radius == kDefaultTheme.scale.radius_sm) {
                    found = rgb(quad.fill) == expected && rgb(quad.border) == expected && quad.fill.alpha == 1.0f;
                }
            }
            colors_exact = colors_exact && found;
        }
        if (pass == 0 && ready && row) {
            bool changed = false;
            ready = message_scene_set_text_metrics(&f.scene, 0, 0, 20, true,
                &row->rich_layout, &row->rich_item_layout, {}, &changed) && message_scene_update(&f.scene, viewport).ok();
            layout_passes = f.scene.layout_passes;
            data_updates = f.scene.data_updates;
        }
    }
    check(ready && colors_exact, "embed_distinct_rgb_black_and_absent_color_reach_paint_stream");
    paint_stream_release(&f.paint);
    message_scene_release(&f.scene);
}
} // namespace maicord
