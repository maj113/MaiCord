#pragma once

#include "maicord/ui/media/picker.hpp"

namespace maicord::ui_media_picker_detail {

// Discord's sticker grid: at most 360 DIP wide it uses 80 DIP previews 4 DIP
// apart; every preview node adds 1 DIP padding.
constexpr float kStickerCompactWidth = 360, kStickerCompactScale = 80.f / 96, kStickerNodePadding = 1;
inline float clamped(float value, float low, float high) noexcept {
    return maximum(low, minimum(value, high));
}
// Range comparisons also reject NaN and infinities: every bound is finite.
inline bool bounded(float value, float low, float high) noexcept {
    return value >= low && value <= high;
}
struct PickerLayout {
    UiRect panel, tabs, field, text, body, rail, footer;
    float cell_pitch, cell_width, cell_height, column_pitch, grid_left;
    u16 columns;
    bool masonry;
};
// Leading list padding: none above sticky headers (Discord), the list inset
// above plain results and the gutter above GIFs.
inline float content_top(const UiMediaPickerScene& scene) noexcept {
    const auto& m = scene.style.metrics;
    return scene.kind == UiMediaPickerKind::gif               ? m.gif_gutter
           : scene.source && scene.source->sections.count ? 0
                                                          : m.list_inset;
}
inline bool inline_results(const UiMediaPickerScene& scene) noexcept {
    return scene.variant == UiMediaPickerVariant::inline_results;
}
// The scroll axis: the inline GIF strip scrolls sideways, every other body down.
inline float body_extent(const UiMediaPickerScene& scene) noexcept {
    return inline_results(scene) && scene.kind == UiMediaPickerKind::gif ? ui_rect_width(scene.body)
                                                                        : ui_rect_height(scene.body);
}
// Where an item sits: its section, its row within the section and its column.
struct GridPosition {
    u32 section, row, column;
};
inline u32 section_first(const UiMediaPickerScene& scene, u32 section) noexcept {
    return scene.source->sections.count ? scene.source->sections[section].first : 0;
}
// Items shown in a section: collapsed sections show none.
inline u32 section_items(const UiMediaPickerScene& scene, u32 section) noexcept {
    if (!scene.source->sections.count)
        return scene.source->count;
    const auto& s = scene.source->sections[section];
    return s.expanded ? s.count : 0;
}

bool finite_rectangle(const UiRect& r) noexcept;
bool picker_layout(const UiMediaPickerStyle& style, const UiMediaPickerSource* source,
                   UiMediaPickerKind kind, UiMediaPickerVariant variant, bool back, UiRect viewport,
                   UiRect anchor, float preferred_width, PickerLayout* output) noexcept;
bool inline_layout(const UiMediaPickerStyle& style, const UiMediaPickerSource* source,
                   const UiMediaPickerContent& content, UiRect viewport, UiRect anchor,
                   PickerLayout* output) noexcept;
bool label_valid(const Utf8View& text) noexcept;
bool source_valid(const UiMediaPickerSource* source, const UiMediaPickerContent& content) noexcept;
float section_extent(const UiMediaPickerScene& scene, u32 index) noexcept;
bool item_valid(const UiMediaPickerItem& item) noexcept;
UiMediaPickerTarget target(const UiMediaPickerScene& scene, UiWidgetHandle widget) noexcept;
void synchronize_focus(UiMediaPickerScene& scene) noexcept;
void refresh_footer(UiMediaPickerScene& scene) noexcept;
bool rebuild(UiMediaPickerScene& scene, bool reuse = false) noexcept;
bool grid_locate(const UiMediaPickerScene& scene, u32 index, GridPosition* position) noexcept;
u32 grid_step(const UiMediaPickerScene& scene, u32 index, i32 rows) noexcept;
bool item_position(const UiMediaPickerScene& scene, u32 index, float* top, float* height) noexcept;
bool safe_rebuild(UiMediaPickerScene& scene) noexcept;
bool select_index(UiMediaPickerScene& scene, u32 index, float top, float height) noexcept;
bool select_available(UiMediaPickerScene& scene, u32 first, i32 direction) noexcept;
u32 masonry_step(const UiMediaPickerScene& scene, u32 index, i32 horizontal, i32 vertical) noexcept;

} // namespace maicord::ui_media_picker_detail
