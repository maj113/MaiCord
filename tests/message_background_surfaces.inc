#pragma once
#include "maicord/core/memory.hpp"
#include "maicord/ui/message_presentation.hpp"
#include "maicord/ui/scrollbar.hpp"
#include "maicord/ui/background_presentation.hpp"

namespace maicord {
inline void test_message_background_surfaces(const MessageScene& original, u64 rich_message_id,
                                             void (*check)(bool, const char*) noexcept) noexcept {
    // Admission alone includes overscan rows, whose rich controls may be
    // outside the paint clip. Build an independent viewport scene and reveal
    // a real component without mutating the caller's timeline/focus/handles.
    MessageTimeline timeline = original.timeline ? *original.timeline : MessageTimeline{};
    MessageScene scene{};
    MessageSceneRowWidget rows[16]{};
    UiWidget widgets[512]{};
    UiLayoutNode nodes[512]{};
    struct SceneRelease {
        MessageScene& scene;
        ~SceneRelease() noexcept {
            message_scene_release(&scene);
        }
    } scene_release{scene};
    const auto found =
        timeline.store ? message_store_find(*timeline.store, rich_message_id) : MessageStoreResult{};
    bool visible =
        found.ok() && timeline.list &&
        message_scene_initialize(&scene, &timeline, {rows, 16}, {widgets, 512}, {nodes, 512}) &&
        message_timeline_set_scroll(
            &timeline, ui_variable_list_prefix_units(*timeline.list, found.order_index), false) &&
        message_scene_update(&scene, original.viewport).ok();
    const auto* row = visible ? message_scene_find_row(scene, found.order_index) : nullptr;
    const auto* card =
        row ? message_scene_rich_item_layout_node(scene, *row, MessageSceneRichItemKind::component, 0)
            : nullptr;
    if (card) {
        const float advance = maximum(0.f, card->rectangle.top - original.viewport.top - 16);
        visible = message_timeline_set_scroll(&timeline, timeline.scroll_units + ui_extent_units(advance),
                                              false) &&
                  message_scene_update(&scene, original.viewport).ok();
        row = visible ? message_scene_find_row(scene, found.order_index) : nullptr;
        card =
            row ? message_scene_rich_item_layout_node(scene, *row, MessageSceneRichItemKind::component, 0)
                : nullptr;
        visible =
            visible && card && ui_rect_has_area(ui_rect_intersection(card->rectangle, original.viewport));
    } else
        visible = false;
    check(visible,
          "message gradient rich component is actually visible in independent current viewport scene");
    PaintStream flat{}, gradient{};
    MessageMediaCommand flat_commands[64]{}, gradient_commands[64]{};
    MessageMediaCommandStream flat_media{}, gradient_media{};
    MessagePresentationResult flat_result{}, gradient_result{};
    struct Release {
        PaintStream &a, &b;
        ~Release() noexcept {
            paint_stream_release(&a);
            paint_stream_release(&b);
        }
    } release{flat, gradient};
    Theme theme = kDefaultTheme;
    theme.background = background_theme_defaults();
    bool ready =
        paint_stream_initialize(&flat) && paint_stream_initialize(&gradient) && paint_stream_reset(&flat) &&
        paint_stream_reset(&gradient) &&
        message_media_command_stream_initialize(&flat_media, {flat_commands, 64}) &&
        message_media_command_stream_initialize(&gradient_media, {gradient_commands, 64}) && visible &&
        message_presentation_build(scene, kDefaultTheme, &flat, &flat_media, &flat_result) &&
        message_presentation_build(scene, theme, &gradient, &gradient_media, &gradient_result);
    bool geometry = ready && flat.count == gradient.count && flat.clip_count == gradient.clip_count &&
                    flat.batch_count == gradient.batch_count &&
                    flat_media.count == gradient_media.count;
    check(ready, "message gradient fixture initializes and both real presentations succeed");
    check(geometry, "message gradient fixture preserves quad clip batch and media counts");
    const auto scrollbar =
        ready ? ui_scrollbar_geometry(scene.viewport, ui_extent_dips(scene.timeline->scroll_units),
                                      ui_extent_dips(scene.timeline->list->total_units))
              : UiScrollbarGeometry{};
    u32 materials = 0, scrollbar_materials = 0;
    bool component_fill = false;
    for (u32 i = 0; geometry && i < flat.count; ++i) {
        const auto& a = flat.quads[i];
        const auto& b = gradient.quads[i];
        if (card && ui_rect_equal({b.left, b.top, b.right, b.bottom}, card->rectangle))
            component_fill |= b.border_width == 1 && bytes_equal(&b.fill, &kDefaultTheme.tokens.fill.muted, sizeof(Color)) &&
                              bytes_equal(&a, &b, sizeof(PaintQuad));
        geometry = a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom &&
                   a.radius == b.radius;
        if (b.border_width == -7) {
            // The only unrecorded material is the final normal thumb. Its
            // palette edge is deliberately wider than the flat zero border;
            // never grant that exception to message cards or media artwork.
            const bool thumb = scrollbar.visible && i + 1 == flat.count &&
                               ui_rect_equal({a.left, a.top, a.right, a.bottom}, scrollbar.thumb);
            if (thumb) {
                ++scrollbar_materials;
                geometry = geometry && a.radius == 4 && a.border_width == 0 && a.reserved_x == 0 &&
                           a.reserved_y == 0 && b.reserved_x == -.60f &&
                           b.reserved_y ==
                               (1 - ui_background_tier_opacity(theme, UiBackgroundTier::highest)) * .90f &&
                           bytes_equal(&a.fill, &kDefaultTheme.interactive, sizeof(Color)) &&
                           bytes_equal(&a.border, &kDefaultTheme.interactive, sizeof(Color)) &&
                           bytes_equal(&a.fill, &b.fill, sizeof(Color)) &&
                           bytes_equal(&a.border, &b.border, sizeof(Color));
            } else {
                ++materials;
                geometry = geometry && b.fill.alpha == 1 && b.reserved_y > 0 &&
                           (b.reserved_x == a.border_width || b.reserved_x == -a.border_width);
            }
        } else {
            // Media placeholders, native icons, authored accents and status
            // decorations must stay exact; only row hover is a glass layer.
            const bool glass = a.radius == 0 && a.fill.alpha == .55f;
            geometry = geometry && (glass || bytes_equal(&a, &b, sizeof(PaintQuad)));
        }
    }
    geometry = geometry && scrollbar_materials == (scrollbar.visible ? 1u : 0u);
    check(geometry, "message gradient per-quad geometry material metadata and nonmaterial colors match");
    bool media_keys = ready && flat_media.count == gradient_media.count;
    for (u32 i = 0; media_keys && i < flat_media.count; ++i) {
        const auto& a = flat_commands[i];
        const auto& b = gradient_commands[i];
        media_keys = a.id == b.id && a.kind == b.kind && ui_rect_equal(a.rectangle, b.rectangle) &&
                     ui_rect_equal(a.clip, b.clip) && a.owner.manager == b.owner.manager &&
                     a.owner.handle.slot == b.owner.handle.slot &&
                     a.owner.handle.generation == b.owner.handle.generation &&
                     a.message_revision == b.message_revision && a.format == b.format &&
                     a.embed_index == b.embed_index && a.embed_resource == b.embed_resource &&
                     a.animated == b.animated && a.ordinary_video == b.ordinary_video;
    }
    check(media_keys, "message gradient actual media keys resources clip and owners match");
    check(component_fill, "message link button keeps its translucent neutral fill over background themes");
    check(materials != 0, "message gradient admitted rich fixture emits at least one real material card");
    check(geometry && media_keys && materials && !gradient.rejected &&
              gradient.count <= kMaxPaintQuads && !gradient_media.rejected,
          "message gradient materials preserve real geometry media semantic paint and widget ownership");
}
} // namespace maicord
