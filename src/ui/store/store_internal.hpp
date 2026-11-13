#pragma once

#include "maicord/ui/store/store_scene.hpp"

namespace maicord::store_detail {

// Metrics of the store pages, in DIPs.
inline constexpr float kCardMinWidth = 232, kChipHeight = 24, kFilterHeight = 32, kTabHeight = 40, kFieldHeight = 40, kButtonSmall = 32,
                       kButtonLarge = 40, kSwitchWidth = 48, kSwitchHeight = 24, kRadius = 12, kTailPadding = 60;
inline constexpr float kBadgeHeight = 18;
inline constexpr u32 kNoIcon = 255;

struct TypeMetrics {
    float em, line;
    u32 style;
    bool bold, mono;
};
const TypeMetrics& type_metrics(u8 type) noexcept;
float text_width(u8 type, const Utf8View& text) noexcept;
// The measure of the scene being laid out, painted or emitted.
extern const UiStoreMeasure* g_measure;
u32 text_lines(u8 type, const Utf8View& text, float width) noexcept;

inline bool hidden(const UiStoreNode& n) noexcept { return (n.flags & ui_store_hidden) != 0; }
bool interactive(const UiStoreNode& n) noexcept;
// The node's screen rectangle: the dialog tree is fixed, the page tree scrolls.
UiRect screen_rect(const UiStoreScene& s, u16 index) noexcept;
inline bool in_dialog(const UiStoreScene& s, u16 index) noexcept {
    return s.dialog_root != kUiStoreNone && index >= s.dialog_root;
}
// The pointer-hit node under (x, y) among the visible interactive nodes, or kUiStoreNone.
u16 hit_node(const UiStoreScene& s, float x, float y) noexcept;
u16 find_action(const UiStoreScene& s, u32 action) noexcept;
// The clip a node paints inside (page: the panel body; dialog: its panel).
UiRect clip_of(const UiStoreScene& s, u16 index) noexcept;

// A button's leading glyph: its icon, or the spinner of a busy button (drawn where the icon goes).
inline bool leaf_leading_glyph(const UiStoreNode& n) noexcept {
    return n.icon != kNoIcon || (n.flags & ui_store_busy);
}
// A button's content: leading glyph, label, trailing glyph (what a stretched button centres).
inline float leaf_text_content(const UiStoreNode& n) noexcept {
    float width = text_width(static_cast<u8>(UiStoreType::label_bold), n.text);
    if (leaf_leading_glyph(n))
        width += 24;
    if (n.icon2 != kNoIcon)
        width += 22;
    return width;
}
// The count badge after a tab's label.
inline float tab_badge_width(const UiStoreNode& n) noexcept {
    return maximum(kBadgeHeight, text_width(static_cast<u8>(UiStoreType::small_bold), n.text2) + 10);
}
inline UiRect tab_badge(const UiStoreNode& n, const UiRect& r, float label_width) noexcept {
    const float width = tab_badge_width(n);
    const float cy = (r.top + r.bottom) * .5f;
    const float left = r.left + label_width + 4;
    return {left, cy - kBadgeHeight * .5f, left + width, cy + kBadgeHeight * .5f};
}
inline UiRect search_clear_rect(const UiRect& r) noexcept {
    const float cy = (r.top + r.bottom) * .5f;
    return {r.right - 32, cy - 12, r.right - 8, cy + 12};
}
inline UiRect search_hint_rect(const UiStoreNode& n, const UiRect& r) noexcept {
    const float cy = (r.top + r.bottom) * .5f;
    const float width = text_width(static_cast<u8>(UiStoreType::eyebrow), n.text2) + 14;
    return {r.right - 8 - width, cy - 10, r.right - 8, cy + 10};
}
// Where the editor text (or placeholder) sits in a search field.
inline UiRect search_text_rect(const UiStoreNode& n, const UiRect& r) noexcept {
    const float right = n.value ? r.right - 40 : n.text2.bytes ? search_hint_rect(n, r).left - 8 : r.right - 12;
    return {r.left + 36, r.top, maximum(r.left + 37, right), r.bottom};
}

} // namespace maicord::store_detail
