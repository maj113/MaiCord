#include "maicord/ui/popup_scene.hpp"
#include "maicord/ui/regional_flag.hpp"

#include "maicord/ui/text_style.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/widget_scroll.hpp"
#include "maicord/ui/list_navigation.hpp"
#include "maicord/ui/virtual_list.hpp"

namespace maicord {
namespace {

inline constexpr u32 kPopupRootId = 1;
inline constexpr u32 kPopupPanelId = 2;
inline constexpr UiThemeMetrics kDefaultMenuMetrics{};
const UiThemeMetrics& menu_metrics(const UiResolvedThemeStyles* styles) noexcept {
    return styles && styles->ready ? styles->metrics : kDefaultMenuMetrics;
}
// Any ordinary row with a leading icon gives icon-less rows the same label
// column and full row height, so mixed menus keep one rhythm.
bool menu_icon_column(ConstSlice<PopupItem> items) noexcept {
    for (u32 index = 0; index < items.count; ++index) {
        const auto presentation = popup_item_menu_presentation(items[index]);
        if (popup_item_menu_row(items[index]) && presentation.icon_plus_one && !presentation.trailing_icon)
            return true;
    }
    return false;
}

// Mention lists mix member avatars with @everyone, @here and role rows: those
// leading-glyph rows then take the avatar row's height and label column.
bool leading_icon_row(const PopupItem& item) noexcept {
    const auto presentation = popup_item_menu_presentation(item);
    return popup_item_menu_row(item) && presentation.icon_plus_one && !presentation.trailing_icon;
}
bool menu_avatar_column(ConstSlice<PopupItem> items) noexcept {
    for (u32 index = 0; index < items.count; ++index)
        if (popup_item_menu_row(items[index]) && (items[index].flags & popup_item_avatar))
            return true;
    return false;
}

bool storage_bytes(u32 capacity, usize* total_bytes) noexcept {
    if (capacity == 0 || static_cast<usize>(capacity) > ~usize{0} / sizeof(PopupItem)) {
        return false;
    }
    const usize item_bytes = static_cast<usize>(capacity) * sizeof(PopupItem);
    const usize alignment_mask = alignof(PopupRowWidget) - 1;
    if (item_bytes > ~usize{0} - alignment_mask) {
        return false;
    }
    const usize row_offset = (item_bytes + alignment_mask) & ~alignment_mask;
    if (static_cast<usize>(capacity) > (~usize{0} - row_offset) / sizeof(PopupRowWidget)) {
        return false;
    }
    *total_bytes = row_offset + static_cast<usize>(capacity) * sizeof(PopupRowWidget);
    return true;
}

bool materialize_owned_storage(PopupScene* scene) noexcept {
    if (!scene->owned_storage.base || scene->owned_capacity == 0 || scene->items.data ||
        scene->rows.data) {
        return false;
    }
    const ArenaMark mark = arena_mark(&scene->owned_storage);
    PopupItem* items = arena_allocate<PopupItem>(&scene->owned_storage, scene->owned_capacity);
    PopupRowWidget* rows = arena_allocate<PopupRowWidget>(&scene->owned_storage, scene->owned_capacity);
    if (!items || !rows) {
        arena_rewind(&scene->owned_storage, mark);
        return false;
    }
    scene->items = Slice<PopupItem>{items, scene->owned_capacity};
    scene->rows = Slice<PopupRowWidget>{rows, scene->owned_capacity};
    return true;
}

bool contains_rectangle(UiRect outer, UiRect inner) noexcept {
    return ui_rect_valid_or_empty(outer) && ui_rect_valid_or_empty(inner) && inner.left >= outer.left &&
           inner.top >= outer.top && inner.right <= outer.right && inner.bottom <= outer.bottom;
}

UiLayoutStyle root_style() noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.flow = UiLayoutFlow::overlay;
    style.width = ui_layout_fill();
    style.height = ui_layout_fill();
    style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_allow_overlap);
    return style;
}

UiLayoutStyle panel_style(float left, float top, float width, float height, bool overlay, bool scrollable,
                          float feedback_height, const UiResolvedThemeStyles* styles = nullptr) noexcept {
    const float padding = menu_metrics(styles).menu_padding;
    UiLayoutStyle style = ui_layout_style();
    style.flow = overlay ? UiLayoutFlow::overlay : UiLayoutFlow::vertical;
    style.width = ui_layout_fixed(width);
    style.height = ui_layout_fixed(height);
    style.margin = UiInsets{left, top, 0.0f, 0.0f};
    style.padding = UiInsets{padding, padding, padding, padding + feedback_height};
    // A popup is an independently sized overlay; stretching a fixed-width
    // child would make the menu inherit the full root width.
    style.horizontal_alignment = UiLayoutAlignment::start;
    style.flags =
        static_cast<u16>(ui_layout_node_visible | ui_layout_node_clip_children |
                         ui_layout_node_allow_overlap | (scrollable ? ui_layout_node_scroll_vertical : 0));
    return style;
}

bool quick_reaction_strip(ConstSlice<PopupItem> items) noexcept {
    if (items.count <= kPopupQuickReactionCount) {
        return false;
    }
    for (u32 index = 0; index < items.count; ++index) {
        const bool quick = (items[index].flags & popup_item_quick_reaction) != 0;
        if (quick != (index < kPopupQuickReactionCount)) {
            return false;
        }
    }
    return true;
}

float context_item_top(ConstSlice<PopupItem> items, u32 target) noexcept {
    float top = kPopupQuickReactionTop + kPopupQuickReactionExtent + kPopupQuickReactionBottomGap;
    for (u32 index = kPopupQuickReactionCount; index < target; ++index) {
        top += popup_item_menu_row_height(items[index], false);
        if (index + 1 < items.count && (items[index + 1].flags & popup_item_separator_before) != 0) {
            top += kPopupContextSeparatorGap;
        }
    }
    if (target >= kPopupQuickReactionCount && (items[target].flags & popup_item_separator_before) != 0 &&
        target == kPopupQuickReactionCount) {
        top += kPopupContextSeparatorGap;
    }
    return top;
}

UiLayoutStyle row_style(const PopupItem& item, u32 index, bool quick_strip, ConstSlice<PopupItem> items,
                        float content_width, const UiResolvedThemeStyles* styles = nullptr) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.flow = UiLayoutFlow::overlay;
    if (items.count && (items[0].flags & popup_item_command_header)) {
        const auto& metrics = menu_metrics(styles);
        const bool close = (item.flags & popup_item_command_close) != 0;
        style.width = ui_layout_fixed(close ? metrics.menu_row_height : content_width);
        style.height = ui_layout_fixed(index < 2 ? metrics.menu_row_height : metrics.menu_detail_height);
        style.margin.left = close ? maximum(0.f, content_width - metrics.menu_row_height) : 0.f;
        style.margin.top = index < 2 ? 0.f : (index - 2) * metrics.menu_detail_height;
        style.horizontal_alignment = UiLayoutAlignment::start;
        return style;
    }
    if (items.count && (items[0].flags & popup_item_command_category)) {
        const bool rail = (item.flags & popup_item_command_category) != 0;
        u32 ordinal = 0;
        for (u32 n = 0; n < index; ++n)
            if (((items[n].flags & popup_item_command_category) != 0) == rail)
                ++ordinal;
        style.width = ui_layout_fixed(rail ? 40.0f : maximum(1.0f, content_width - 48.0f));
        style.height = ui_layout_fixed(rail ? 40.0f : kUiMenuDetailRowHeight);
        style.margin.left = 0.0f;
        style.margin.top = ordinal * (rail ? 40.0f : kUiMenuDetailRowHeight);
        style.horizontal_alignment = UiLayoutAlignment::start;
        return style;
    }
    if (quick_strip) {
        style.horizontal_alignment = UiLayoutAlignment::start;
        if (index < kPopupQuickReactionCount) {
            constexpr float strip_width =
                static_cast<float>(kPopupQuickReactionCount) * kPopupQuickReactionExtent +
                static_cast<float>(kPopupQuickReactionCount - 1) * kPopupQuickReactionGap;
            style.width = ui_layout_fixed(kPopupQuickReactionExtent);
            style.height = ui_layout_fixed(kPopupQuickReactionExtent);
            style.margin.left =
                maximum(0.0f, (content_width - strip_width) * 0.5f) +
                static_cast<float>(index) * (kPopupQuickReactionExtent + kPopupQuickReactionGap);
            style.margin.top = kPopupQuickReactionTop;
        } else {
            style.width = ui_layout_fixed(content_width);
            style.height = ui_layout_fixed(popup_item_menu_row_height(item, false));
            style.margin.top = context_item_top(items, index);
        }
        return style;
    }
    style.width = ui_layout_fill();
    const bool compact = !menu_icon_column(items);
    const bool avatar_height = (item.flags & popup_item_avatar) || (leading_icon_row(item) && menu_avatar_column(items));
    style.height =
        ui_layout_fixed(avatar_height
                            ? maximum(kPopupAvatarRowHeight, popup_item_menu_row_height(item, compact, styles))
                            : popup_item_menu_row_height(item, compact, styles));
    style.margin.top =
        (item.flags & popup_item_separator_before) != 0 ? menu_metrics(styles).menu_gap : 0.0f;
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    return style;
}

UiLayoutStyle label_style(const PopupItem& item, const UiResolvedThemeStyles* styles = nullptr) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fill();
    style.height = ui_layout_fill();
    const bool emoji = (item.flags & popup_item_emoji) != 0;
    style.padding = emoji ? UiInsets{}
                        : UiInsets{menu_metrics(styles).menu_gap, menu_metrics(styles).menu_padding - 1.f,
                                   menu_metrics(styles).menu_gap, menu_metrics(styles).menu_padding - 1.f};
    if ((item.flags & popup_item_custom_emoji) != 0) {
        // Custom emoji are media rather than text. Give the media a real
        // centered layout node so the renderer never computes glyph position.
        style.width = ui_layout_fixed(kPickerEmojiEmSize);
        style.height = ui_layout_fixed(kPickerEmojiEmSize);
        style.horizontal_alignment = UiLayoutAlignment::center;
        style.vertical_alignment = UiLayoutAlignment::center;
    }
    style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_text_content);
    return style;
}

bool item_enabled(const PopupItem& item) noexcept {
    return (item.flags & popup_item_enabled) != 0;
}

bool scrollable_menu(PopupKind kind) noexcept {
    return kind == PopupKind::autocomplete || kind == PopupKind::context_menu ||
           kind == PopupKind::popover || kind == PopupKind::settings_choice;
}

u32 first_enabled(const PopupScene& scene) noexcept {
    for (u32 index = 0; index < scene.item_count; ++index) {
        if (item_enabled(scene.items[index])) {
            return index;
        }
    }
    return ~0u;
}

PopupSceneResult failure(PopupScene* scene, PopupSceneError error) noexcept {
    if (scene)
        saturating_increment(scene->rejected);
    return PopupSceneResult{error, false};
}

float popup_height(ConstSlice<PopupItem> items, const UiResolvedThemeStyles* styles = nullptr) noexcept {
    if (items.count && (items[0].flags & popup_item_command_header))
        return 2.f * menu_metrics(styles).menu_padding + menu_metrics(styles).menu_row_height +
               (items.count > 2 ? items.count - 2 : 0) * menu_metrics(styles).menu_detail_height;
    if (items.count && (items[0].flags & popup_item_command_category))
        return 420.0f;
    if (quick_reaction_strip(items)) {
        float height = 2.0f * menu_metrics(styles).menu_padding + kPopupQuickReactionTop +
                       kPopupQuickReactionExtent + kPopupQuickReactionBottomGap;
        for (u32 index = kPopupQuickReactionCount; index < items.count; ++index) {
            if ((items[index].flags & popup_item_separator_before) != 0) {
                height += kPopupContextSeparatorGap;
            }
            height += popup_item_menu_row_height(items[index], false);
        }
        return height;
    }
    float height = 2.0f * menu_metrics(styles).menu_padding;
    const bool compact = !menu_icon_column(items);
    const bool avatars = menu_avatar_column(items);
    for (u32 index = 0; index < items.count; ++index) {
        height +=
            (items[index].flags & popup_item_separator_before) != 0 ? menu_metrics(styles).menu_gap : 0.0f;
        height +=
            ((items[index].flags & popup_item_avatar) || (avatars && leading_icon_row(items[index]))
                 ? maximum(kPopupAvatarRowHeight, popup_item_menu_row_height(items[index], compact, styles))
                 : popup_item_menu_row_height(items[index], compact, styles));
    }
    return height;
}

UiRect place_popup(UiRect anchor, UiRect viewport, float width, float height, bool prefer_above,
                   bool below_only = false) noexcept {
    const float minimum_x = viewport.left + kPopupOuterMargin;
    const float maximum_x = maximum(minimum_x, viewport.right - kPopupOuterMargin - width);
    const float minimum_y = viewport.top + kPopupOuterMargin;
    const float maximum_y = maximum(minimum_y, viewport.bottom - kPopupOuterMargin - height);
    float left = minimum(maximum(anchor.left, minimum_x), maximum_x);
    float top = anchor.bottom;
    if (below_only)
        return UiRect{left, top, left + width, top + height};
    // A pointer context menu (a cursor-sized anchor) opens right of the cursor
    // and flips left of it when it would overflow; it shifts up rather than
    // flipping above the cursor.
    if (ui_rect_width(anchor) <= 1.f && ui_rect_height(anchor) <= 1.f && !prefer_above) {
        if (anchor.left + width > viewport.right - kPopupOuterMargin && anchor.left - width >= minimum_x)
            left = anchor.left - width;
        top = minimum(maximum(anchor.bottom, minimum_y), maximum_y);
        return UiRect{left, top, left + width, top + height};
    }
    const bool fits_below = top + height <= viewport.bottom - kPopupOuterMargin;
    const bool fits_above = anchor.top - height >= minimum_y;
    if ((prefer_above && fits_above) || (!fits_below && fits_above)) {
        top = anchor.top - height;
    }
    top = minimum(maximum(top, minimum_y), maximum_y);
    return UiRect{left, top, left + width, top + height};
}

// Height of an ordinary menu: its content, capped to the viewport, or for a
// header popout to the space below its anchor. *below reports whether the
// below-only placement applies (it falls back when not even a compact row fits).
float menu_height(float content, UiRect anchor, UiRect viewport, float minimum_height, bool* below) noexcept {
    if (*below) {
        const float available = viewport.bottom - kPopupOuterMargin - anchor.bottom;
        if (available >= minimum_height)
            return minimum(content, available);
        *below = false;
    }
    return minimum(content, maximum(minimum_height, ui_rect_height(viewport) - 2.f * kPopupOuterMargin));
}

bool reveal_selection(PopupScene* scene, bool* changed) noexcept {
    if (!scrollable_menu(scene->kind) || scene->selected_index >= scene->item_count)
        return true;
    const auto revealed = ui_widget_reveal(&scene->widgets, scene->rows[scene->selected_index].widget);
    const auto* panel = popup_scene_layout_node(*scene, scene->panel);
    if (!revealed.ok() || !panel)
        return false;
    scene->scroll_offset = panel->scroll_offset.y;
    *changed = *changed || revealed.scrolled != 0;
    if (revealed.scrolled)
        saturating_increment(scene->revision);
    return true;
}

struct PopupPageSource {
    const PopupScene* scene;
    const UiLayoutNode* panel;
    u32 quick_index;
    bool quick_strip;
};

u32 page_source_index(const PopupPageSource& source, u32 index) noexcept {
    return !source.quick_strip         ? index
           : index                     ? index + kPopupQuickReactionCount - 1
           : source.quick_index != ~0u ? source.quick_index
                                       : 0;
}

bool page_eligible(void* context, u32 index) noexcept {
    const auto& source = *static_cast<const PopupPageSource*>(context);
    if (source.quick_strip && index == 0 && source.quick_index == ~0u)
        return false;
    return item_enabled(source.scene->items[page_source_index(source, index)]);
}

UiListRowBounds page_bounds(void* context, u32 index) noexcept {
    const auto& source = *static_cast<const PopupPageSource*>(context);
    const auto* row =
        popup_scene_layout_node(*source.scene, source.scene->rows[page_source_index(source, index)].widget);
    if (!row)
        return {};
    // Recover retained logical bounds independently of the current clip. The
    // range can exceed one coordinate extent, so convert directly into u64.
    const double first =
        maximum(0.0, static_cast<double>(row->rectangle.top) + source.panel->scroll_offset.y -
                         source.panel->content_rectangle.top);
    const double last = first + ui_rect_height(row->rectangle);
    return {static_cast<u64>(first * kUiExtentUnitsPerDip + .5),
            static_cast<u64>(last * kUiExtentUnitsPerDip + .5)};
}

} // namespace

bool popup_scene_initialize(PopupScene* scene, Slice<PopupItem> items, Slice<PopupRowWidget> rows,
                            Slice<UiWidget> widgets, Slice<UiLayoutNode> layout_nodes) noexcept {
    if (!scene || !items.data || items.count == 0 || !rows.data || rows.count < items.count ||
        !widgets.data || widgets.count < 2 || !layout_nodes.data || layout_nodes.count < widgets.count) {
        return false;
    }
    *scene = PopupScene{};
    scene->items = items;
    scene->rows = rows;
    scene->selected_index = ~0u;
    return ui_widget_manager_initialize(&scene->widgets, widgets, layout_nodes);
}

bool popup_scene_initialize_owned(PopupScene* scene, u32 item_capacity, Slice<UiWidget> widgets,
                                  Slice<UiLayoutNode> layout_nodes) noexcept {
    usize reserve_bytes = 0;
    if (!scene || scene->owned_storage.base || !widgets.data || widgets.count < 2 || !layout_nodes.data ||
        layout_nodes.count < widgets.count || !storage_bytes(item_capacity, &reserve_bytes)) {
        return false;
    }
    *scene = PopupScene{};
    scene->selected_index = ~0u;
    if (!ui_widget_manager_initialize(&scene->widgets, widgets, layout_nodes) ||
        !arena_initialize(&scene->owned_storage, reserve_bytes, 0, 4 * 1024)) {
        *scene = PopupScene{};
        return false;
    }
    scene->owned_capacity = item_capacity;
    return true;
}

void popup_scene_release(PopupScene* scene) noexcept {
    if (!scene) {
        return;
    }
    arena_release(&scene->owned_storage);
    *scene = PopupScene{};
}

bool popup_scene_bind_theme_styles(PopupScene* scene, const UiResolvedThemeStyles* styles) noexcept {
    if (!scene || (styles && !styles->ready))
        return false;
    const u32 revision = styles ? styles->geometry_revision : 0;
    if (scene->theme_styles == styles && scene->theme_geometry_revision == revision)
        return false;
    if (!scene->active || !scene->themed_menu) {
        scene->theme_styles = styles;
        scene->theme_geometry_revision = revision;
        return true;
    }
    // Reuse the retained rows directly. Reopening would reset input authority,
    // selection, edit state and a descriptor slice which may alias scene.items.
    const ConstSlice<PopupItem> items{scene->items.data, scene->item_count};
    const bool command_selected = items.count && (items[0].flags & popup_item_command_header);
    const auto layout = [&](UiWidgetHandle handle) noexcept {
        auto* widget = ui_widget_manager_resolve(&scene->widgets, handle);
        return widget ? ui_layout_engine_resolve(&scene->widgets.layout, widget->layout) : nullptr;
    };
    auto* panel = layout(scene->panel);
    if (!panel)
        return false;
    for (u32 i = 0; i < items.count; ++i)
        if (!layout(scene->rows[i].widget) || !layout(scene->rows[i].label))
            return false;
    const float padding = menu_metrics(styles).menu_padding;
    const float feedback_height = scene->feedback_reserved ? scene->feedback.style.height : 0.f;
    UiRect rectangle{};
    if (scene->kind == PopupKind::autocomplete) {
        const float bottom = minimum(scene->anchor.top - kUiAutocompleteGap, scene->viewport.bottom);
        const float available = maximum(0.f, bottom - scene->viewport.top - kPopupOuterMargin);
        const float left = maximum(scene->anchor.left, scene->viewport.left);
        const float right = minimum(scene->anchor.right, scene->viewport.right);
        if (available <= 2.f * padding || right - left <= 2.f * padding)
            return false;
        rectangle = {left, bottom - minimum(popup_height(items, styles), available), right, bottom};
    } else {
        const float width = minimum(scene->preferred_width, maximum(96.f, ui_rect_width(scene->viewport) -
                                                                              2.f * kPopupOuterMargin));
        bool below = scene->anchored_below;
        const float height = menu_height(popup_height(items, styles) + feedback_height, scene->anchor,
                                         scene->viewport, menu_metrics(styles).menu_compact_height + 2.f * padding,
                                         &below);
        rectangle = place_popup(scene->anchor, scene->viewport, width, height, scene->prefer_above, below);
    }
    panel->spec.style =
        panel_style(rectangle.left - scene->viewport.left, rectangle.top - scene->viewport.top,
                    ui_rect_width(rectangle), ui_rect_height(rectangle), command_selected,
                    !command_selected && scrollable_menu(scene->kind), feedback_height, styles);
    if (command_selected) {
        auto* body = layout(ui_widget_manager_find(scene->widgets, 0x10001u));
        if (!body)
            return false;
        body->spec.style.width = ui_layout_fixed(maximum(1.f, ui_rect_width(rectangle) - 2.f * padding));
        body->spec.style.height = ui_layout_fixed(maximum(0.f,
            ui_rect_height(rectangle) - 2.f * padding - menu_metrics(styles).menu_row_height));
        body->spec.style.margin.top = menu_metrics(styles).menu_row_height;
    }
    for (u32 i = 0; i < items.count; ++i) {
        layout(scene->rows[i].widget)->spec.style =
            row_style(items[i], i, false, items, ui_rect_width(rectangle) - 2.f * padding, styles);
        layout(scene->rows[i].label)->spec.style = label_style(items[i], styles);
    }
    if (!ui_widget_manager_layout_rectangle(&scene->widgets, scene->viewport).ok())
        return false;
    UiWidgetScrollGeometry scroll{};
    if (!command_selected && scrollable_menu(scene->kind) &&
        !ui_widget_scroll_geometry(scene->widgets, scene->panel, &scroll))
        return false;
    scene->scroll_max = scroll.maximum.y;
    scene->scroll_offset = panel->scroll_offset.y;
    scene->rectangle = rectangle;
    scene->theme_styles = styles;
    scene->theme_geometry_revision = revision;
    saturating_increment(scene->revision);
    return true;
}

namespace {
PopupSceneResult open_popup(PopupScene* scene, PopupKind kind, u64 owner_id, ConstSlice<PopupItem> items,
                            UiRect anchor, UiRect viewport, float preferred_width, bool prefer_above,
                            float grid_scroll_fraction, float category_scroll_fraction, u32 source_revision,
                            const UiPopupFeedback* feedback, bool below_request) noexcept {
    const u32 available_items =
        scene && scene->items.data ? scene->items.count : (scene ? scene->owned_capacity : 0);
    if (!scene || kind == PopupKind::none || kind > PopupKind::settings_choice || owner_id == 0 ||
        !items.data || items.count == 0 || items.count > available_items ||
        !ui_rect_valid_or_empty(anchor) || !ui_rect_valid_or_empty(viewport) ||
        !ui_rect_has_area(viewport) || !ui_coordinate_valid(preferred_width) ||
        (kind != PopupKind::autocomplete && preferred_width < 96.0f) ||
        // Command palette pitches: 56 DIP body rows, 40 DIP category rail.
        !ui_coordinate_valid(grid_scroll_fraction) || grid_scroll_fraction < 0.0f ||
        (!(items[0].flags & popup_item_command_header) && grid_scroll_fraction >= 56.0f) ||
        !ui_coordinate_valid(category_scroll_fraction) ||
        category_scroll_fraction < 0 || category_scroll_fraction >= 40.0f) {
        return failure(scene, PopupSceneError::invalid_input);
    }
    const bool commands = (items[0].flags & popup_item_command_category) != 0;
    const bool command_selected = (items[0].flags & popup_item_command_header) != 0;
    UiPopupFeedback admitted_feedback = feedback ? *feedback : UiPopupFeedback{};
    if (feedback && admitted_feedback.style.height == 0 && admitted_feedback.style.text_inset == 0)
        admitted_feedback.style = ui_popup_feedback_style();
    if (feedback &&
        (commands || kind == PopupKind::autocomplete || (!feedback->text.data && feedback->text.bytes) ||
         feedback->text.bytes > 256 || !utf8_validate(feedback->text).valid ||
         !ui_coordinate_valid(admitted_feedback.style.height) || admitted_feedback.style.height < 24 ||
         admitted_feedback.style.height > 96 || !ui_coordinate_valid(admitted_feedback.style.text_inset) ||
         admitted_feedback.style.text_inset < 0 ||
         admitted_feedback.style.text_inset * 2 >= admitted_feedback.style.height))
        return failure(scene, PopupSceneError::invalid_input);
    const float feedback_height = feedback ? admitted_feedback.style.height : 0;
    // Command autocomplete adds independent rail/body viewports. Preflight the
    // whole tree before resetting a retained popup or materializing its arrays.
    const u32 fixed_widgets = commands ? 4u : command_selected ? 3u : 2u;
    if (items.count > (kInvalidUiWidgetSlot - fixed_widgets) / kPopupWidgetsPerItem)
        return failure(scene, PopupSceneError::capacity);
    bool has_quick_reaction = false;
    for (u32 index = 0; index < items.count; ++index) {
        const PopupItem& item = items[index];
        if (!ui_menu_row_presentation_valid(item.presentation) ||
            (!popup_item_menu_row(item) &&
             (item.presentation.icon_plus_one || item.presentation.secondary_line ||
              item.presentation.trailing_icon)) ||
            (item.presentation.icon_plus_one &&
             (item.flags & (popup_item_avatar | popup_item_emoji_preview | popup_item_guild_icon)))) {
            return failure(scene, PopupSceneError::invalid_input);
        }
        if (item.regional_flag) {
            char target[kRegionalFlagSvgTargetBytes];
            if (!(item.flags & popup_item_emoji_preview) || item.media_id ||
                (item.flags & (popup_item_custom_emoji | popup_item_avatar | popup_item_guild_icon)) ||
                !regional_flag_svg_target(item.regional_flag, {target, sizeof(target)}))
                return failure(scene, PopupSceneError::invalid_input);
        }
        has_quick_reaction = has_quick_reaction || (item.flags & popup_item_quick_reaction) != 0;
        if (item.action == 0 || !item.label.data || item.label.bytes == 0 ||
            (item.flags &
             static_cast<u16>(~(popup_item_enabled | popup_item_danger | popup_item_separator_before |
                                popup_item_checked | popup_item_emoji | popup_item_custom_emoji |
                                popup_item_quick_reaction | popup_item_avatar | popup_item_emoji_preview |
                                popup_item_guild_icon | popup_item_command_row |
                                popup_item_command_category | popup_item_command_header |
                                popup_item_command_close))) != 0 ||
            ((!item.detail.data && item.detail.bytes) || !utf8_validate(item.detail).valid) ||
            item.status > 4 ||
            (item.status && !(item.flags & popup_item_avatar) &&
             (!popup_item_menu_row(item) || !item.presentation.icon_plus_one)) ||
            ((item.flags & popup_item_avatar) &&
             (!item.media_id || item.status > 4 ||
              (item.flags & (popup_item_emoji | popup_item_custom_emoji | popup_item_quick_reaction)))) ||
            ((item.flags & popup_item_custom_emoji) != 0 &&
             ((item.flags & popup_item_emoji) == 0 || item.media_id == 0))) {
            return failure(scene, PopupSceneError::invalid_input);
        }
        for (u32 previous = 0; previous < index; ++previous) {
            if (items[previous].action == item.action) {
                return failure(scene, PopupSceneError::invalid_input);
            }
        }
    }
    const bool quick_strip = quick_reaction_strip(items);
    const auto* styles = !commands && !quick_strip ? scene->theme_styles : nullptr;
    const float padding = menu_metrics(styles).menu_padding;
    const bool scrollable = !commands && !command_selected && scrollable_menu(kind);
    if (has_quick_reaction && (kind != PopupKind::context_menu || !quick_strip)) {
        return failure(scene, PopupSceneError::invalid_input);
    }
    const u32 required_widgets = fixed_widgets + items.count * kPopupWidgetsPerItem;
    if (!ui_widget_manager_reserve(&scene->widgets, required_widgets)) {
        return failure(scene, PopupSceneError::capacity);
    }
    if ((!scene->items.data || !scene->rows.data) && !materialize_owned_storage(scene)) {
        return failure(scene, PopupSceneError::capacity);
    }
    UiRect rectangle{};
    bool anchored_below = false;
    if (kind == PopupKind::autocomplete) {
        // Every composer suggestion uses the same surface bounds and gap.
        // Constrain height to the space above it; generic menu placement can
        // otherwise move a tall list down across the editor.
        const float bottom = minimum(anchor.top - kUiAutocompleteGap, viewport.bottom);
        const float available = maximum(0.0f, bottom - viewport.top - kPopupOuterMargin);
        const float left = maximum(anchor.left, viewport.left);
        const float right = minimum(anchor.right, viewport.right);
        if (available <= 2.0f * padding || right - left <= 2.0f * padding)
            return {PopupSceneError::none, popup_scene_close(scene)};
        rectangle = {left, bottom - minimum(popup_height(items, styles), available), right, bottom};
    } else {
        const float width =
            minimum(preferred_width, maximum(96.0f, ui_rect_width(viewport) - 2.0f * kPopupOuterMargin));
        // A follow-up menu replacing a header popout keeps its placement.
        bool below = !commands && !quick_strip &&
                     (below_request || (scene->anchored_below && ui_rect_equal(anchor, scene->anchor)));
        const float height = menu_height(popup_height(items, styles) + feedback_height, anchor, viewport,
                                         menu_metrics(styles).menu_compact_height + 2.0f * padding, &below);
        anchored_below = below;
        rectangle = place_popup(anchor, viewport, width, height, prefer_above, below);
    }
    // Reflowing live composer suggestions is a content update, not a new
    // popup lifetime. Keep the entrance clock stable while rows/size change.
    const bool continuing_suggestions = scene->active && scene->kind == PopupKind::autocomplete &&
                                        kind == PopupKind::autocomplete && scene->owner_id == owner_id;
    const float width = ui_rect_width(rectangle);
    const float height = ui_rect_height(rectangle);
    ui_widget_manager_reset(&scene->widgets);
    const UiWidgetResult root = ui_widget_manager_add_prevalidated(
        &scene->widgets, ui_widget_invalid_handle(),
        UiWidgetDesc{
            kPopupRootId, UiWidgetKind::container, ui_widget_none,
            UiLayoutNodeSpec{utf8_literal("PopupRoot"), root_style(), UiSize{}, 0.0f, kPopupRootId}});
    const UiWidgetResult panel =
        root.ok() ? ui_widget_manager_add_prevalidated(
                        &scene->widgets, root.handle,
                        UiWidgetDesc{kPopupPanelId,
                                     scrollable ? UiWidgetKind::scroll_view : UiWidgetKind::container,
                                     ui_widget_none,
                                     UiLayoutNodeSpec{utf8_literal("PopupPanel"),
                                                      panel_style(rectangle.left - viewport.left,
                                                                  rectangle.top - viewport.top, width,
                                                                  height, quick_strip || commands ||
                                                                      (items[0].flags & popup_item_command_header),
                                                                  scrollable, feedback_height, styles),
                                                      UiSize{}, 0.0f, kPopupPanelId}})
                  : UiWidgetResult{};
    if (!root.ok() || !panel.ok()) {
        return failure(scene, PopupSceneError::widget);
    }
    UiWidgetHandle command_rail{}, command_body{};
    if (commands || command_selected) {
        for (u32 column = command_selected ? 1u : 0u; column != 2; ++column) {
            auto style = ui_layout_style();
            style.flow = UiLayoutFlow::overlay;
            style.width =
                ui_layout_fixed(command_selected ? maximum(1.f, width - 2.f * padding) :
                    column ? maximum(1.0f, width - 2 * kPopupPanelPadding - 48) : 48.0f);
            style.height = ui_layout_fixed(maximum(0.f, height - 2.f * padding -
                (command_selected ? menu_metrics(styles).menu_row_height : 0.f)));
            style.margin.left = column && !command_selected ? 48.0f : 0.0f;
            style.margin.top = command_selected ? menu_metrics(styles).menu_row_height : 0.f;
            style.horizontal_alignment = UiLayoutAlignment::start;
            style.flags = ui_layout_node_visible | ui_layout_node_clip_children |
                          ui_layout_node_scroll_vertical | ui_layout_node_allow_overlap |
                          ui_layout_node_allow_zero_size;
            const auto child = ui_widget_manager_add_prevalidated(
                &scene->widgets, panel.handle,
                {0x10000u + column,
                 UiWidgetKind::scroll_view,
                 ui_widget_none,
                 {utf8_literal("CommandViewport"), style, {}, 0, 0x10000u + column}});
            if (!child.ok())
                return failure(scene, PopupSceneError::widget);
            (column ? command_body : command_rail) = child.handle;
        }
    }
    for (u32 index = 0; index < items.count; ++index) {
        scene->items[index] = items[index];
        const u32 id = 3u + index * kPopupWidgetsPerItem;
        const bool enabled = item_enabled(items[index]);
        const UiWidgetResult row = ui_widget_manager_add_prevalidated(
            &scene->widgets,
            commands ? ((items[index].flags & popup_item_command_category) ? command_rail : command_body)
                     : command_selected && index >= 2 ? command_body : panel.handle,
            UiWidgetDesc{id, UiWidgetKind::button,
                         static_cast<u16>(ui_widget_hit_test | ui_widget_focusable |
                                          (enabled ? ui_widget_enabled : 0)),
                         UiLayoutNodeSpec{utf8_literal("PopupItem"),
                                          row_style(items[index], index, quick_strip, items,
                                                    width - 2.0f * padding, styles),
                                          UiSize{}, 0.0f, id}});
        const UiWidgetResult label =
            row.ok() ? ui_widget_manager_add_prevalidated(
                           &scene->widgets, row.handle,
                           UiWidgetDesc{id + 1u, UiWidgetKind::text, ui_widget_enabled,
                                        UiLayoutNodeSpec{utf8_literal("PopupItemLabel"),
                                                         label_style(items[index], styles), UiSize{},
                                                         0.0f, id + 1u}})
                     : UiWidgetResult{};
        if (!row.ok() || !label.ok()) {
            return failure(scene, PopupSceneError::widget);
        }
        scene->rows[index] = PopupRowWidget{row.handle, label.handle, index};
    }
    if (!ui_widget_manager_layout_rectangle(&scene->widgets, viewport).ok()) {
        return failure(scene, PopupSceneError::layout);
    }
    if (commands || command_selected) {
        const auto* rail = ui_widget_manager_resolve(&scene->widgets, command_rail);
        const auto* body = ui_widget_manager_resolve(&scene->widgets, command_body);
        if ((!command_selected && (!rail ||
            !ui_layout_engine_set_scroll(&scene->widgets.layout, rail->layout,
                                         {0, category_scroll_fraction}))) || !body ||
            !ui_layout_engine_set_scroll(&scene->widgets.layout, body->layout, {0, grid_scroll_fraction}) ||
            !ui_widget_manager_layout_rectangle(&scene->widgets, viewport).ok())
            return failure(scene, PopupSceneError::layout);
    }
    UiWidgetScrollGeometry scroll_geometry{};
    if (scrollable && !ui_widget_scroll_geometry(scene->widgets, panel.handle, &scroll_geometry))
        return failure(scene, PopupSceneError::layout);
    scene->root = root.handle;
    scene->panel = panel.handle;
    scene->viewport = viewport;
    scene->anchor = anchor;
    scene->rectangle = rectangle;
    scene->owner_id = owner_id;
    scene->source_revision = source_revision;
    scene->feedback = admitted_feedback;
    scene->feedback_reserved = feedback != nullptr;
    scene->item_count = items.count;
    scene->selected_index = first_enabled(*scene);
    scene->preferred_width = preferred_width;
    scene->scroll_offset = 0.0f;
    scene->scroll_max = scroll_geometry.maximum.y;
    scene->kind = kind;
    scene->themed_menu = styles != nullptr || (!commands && !quick_strip);
    scene->theme_geometry_revision = scene->theme_styles ? scene->theme_styles->geometry_revision : 0;
    scene->panel_material = PopupPanelMaterial::tinted;
    scene->prefer_above = prefer_above;
    scene->anchored_below = anchored_below;
    scene->active = true;
    if (!continuing_suggestions)
        saturating_increment(scene->opens);
    saturating_increment(scene->revision);
    if (scene->selected_index != ~0u) {
        bool revealed = false;
        if (!ui_widget_manager_focus(&scene->widgets, scene->rows[scene->selected_index].widget) ||
            !reveal_selection(scene, &revealed))
            return failure(scene, PopupSceneError::layout);
    }
    return PopupSceneResult{PopupSceneError::none, true};
}
} // namespace

PopupSceneResult popup_scene_open(PopupScene* scene, PopupKind kind, u64 owner_id,
                                  ConstSlice<PopupItem> items, UiRect anchor, UiRect viewport,
                                  float preferred_width, bool prefer_above, float grid_scroll_fraction,
                                  float category_scroll_fraction, u32 source_revision,
                                  const UiPopupFeedback* feedback) noexcept {
    return open_popup(scene, kind, owner_id, items, anchor, viewport, preferred_width, prefer_above,
                      grid_scroll_fraction, category_scroll_fraction, source_revision, feedback, false);
}

PopupSceneResult popup_scene_open_below(PopupScene* scene, PopupKind kind, u64 owner_id,
                                        ConstSlice<PopupItem> items, UiRect anchor, UiRect viewport,
                                        float preferred_width) noexcept {
    return open_popup(scene, kind, owner_id, items, anchor, viewport, preferred_width, false, 0.f, 0.f, 0,
                      nullptr, true);
}

bool popup_scene_set_panel_material(PopupScene* scene, PopupPanelMaterial material) noexcept {
    if (!scene || !scene->active || material > PopupPanelMaterial::gradient)
        return false;
    if (scene->panel_material != material) {
        scene->panel_material = material;
        ++scene->revision;
    }
    return true;
}

bool popup_scene_close(PopupScene* scene) noexcept {
    if (!scene) {
        return false;
    }
    const bool changed = scene->active;
    scene->active = false;
    scene->kind = PopupKind::none;
    scene->owner_id = 0;
    scene->source_revision = 0;
    scene->feedback = {};
    scene->feedback_reserved = false;
    scene->item_count = 0;
    scene->selected_index = ~0u;
    scene->scroll_offset = 0.0f;
    scene->scroll_max = 0.0f;
    ui_widget_manager_reset(&scene->widgets);
    if (changed) {
        saturating_increment(scene->closes);
        saturating_increment(scene->revision);
    }
    return changed;
}

PopupHit popup_scene_hit_test(const PopupScene& scene, float x, float y) noexcept {
    if (!scene.active || !ui_coordinate_valid(x) || !ui_coordinate_valid(y)) {
        return PopupHit{};
    }
    const bool inside = ui_rect_contains(scene.rectangle, x, y);
    for (u32 remaining = scene.item_count; remaining != 0; --remaining) {
        const u32 index = remaining - 1;
        PopupItemLayout layout{};
        if (popup_scene_resolve_item_layout(scene, index, &layout) &&
            ui_rect_contains(layout.row->rectangle, x, y) &&
            ui_rect_contains(layout.clip_rectangle, x, y)) {
            return PopupHit{scene.items[index].action, index, true, item_enabled(scene.items[index])};
        }
    }
    return PopupHit{0, ~0u, inside, false};
}

bool popup_scene_set_hover(PopupScene* scene, float x, float y, bool* changed) noexcept {
    if (!scene || !changed || !scene->active || !ui_coordinate_valid(x) || !ui_coordinate_valid(y)) {
        return false;
    }
    const PopupHit hit = popup_scene_hit_test(*scene, x, y);
    const u32 next = hit.enabled ? hit.source_index : ~0u;
    // Padding, disabled rows and leaving the surface only end pointer hover.
    // Keep the last actionable row highlighted so Enter and the next arrow
    // continue from the selection the user can still see.
    *changed = next != ~0u && next != scene->selected_index;
    if (next == ~0u)
        return ui_widget_manager_hover(&scene->widgets, ui_widget_invalid_handle());
    const auto target = scene->rows[next].widget;
    if (!ui_widget_manager_focus(&scene->widgets, target) ||
        !ui_widget_manager_hover(&scene->widgets, target))
        return false;
    scene->selected_index = next;
    return true;
}

bool popup_scene_move_selection(PopupScene* scene, i32 direction, bool* changed) noexcept {
    if (!scene || !changed || !scene->active || direction == 0 || scene->item_count == 0) {
        return false;
    }
    const u32 old = scene->selected_index;
    u32 cursor = old != ~0u ? old : (direction > 0 ? scene->item_count - 1u : 0u);
    for (u32 step = 0; step < scene->item_count; ++step) {
        cursor = direction > 0 ? (cursor + 1u) % scene->item_count
                               : (cursor == 0 ? scene->item_count - 1u : cursor - 1u);
        if (item_enabled(scene->items[cursor])) {
            scene->selected_index = cursor;
            ui_widget_manager_focus(&scene->widgets, scene->rows[cursor].widget);
            ui_widget_manager_hover(&scene->widgets, scene->rows[cursor].widget);
            *changed = cursor != old;
            return reveal_selection(scene, changed);
        }
    }
    *changed = false;
    return true;
}

bool popup_scene_scroll(PopupScene* scene, float delta, bool* changed) noexcept {
    if (!scene || !changed || !scene->active || !scrollable_menu(scene->kind) ||
        !ui_coordinate_valid(delta)) {
        return false;
    }
    UiWidget* panel = ui_widget_manager_resolve(&scene->widgets, scene->panel);
    const auto* panel_node = popup_scene_layout_node(*scene, scene->panel);
    // Command children own their own offsets.
    if (!panel || !panel_node || !(panel_node->spec.style.flags & ui_layout_node_scroll_vertical))
        return false;
    const float next = minimum(maximum(scene->scroll_offset + delta, 0.0f), scene->scroll_max);
    *changed = next != scene->scroll_offset;
    if (!*changed) {
        return true;
    }
    if (!ui_layout_engine_set_scroll(&scene->widgets.layout, panel->layout, UiPoint{0.0f, next}) ||
        !ui_widget_manager_layout_rectangle(&scene->widgets, scene->viewport).ok()) {
        return false;
    }
    scene->scroll_offset = next;
    saturating_increment(scene->revision);
    return true;
}

bool popup_scene_page_selection(PopupScene* scene, bool reverse, bool* changed) noexcept {
    if (!scene || !changed || !scene->active || !scene->item_count ||
        scene->item_count > scene->items.count || scene->item_count > scene->rows.count ||
        !scrollable_menu(scene->kind) || (scene->items[0].flags & popup_item_command_category))
        return false;
    const auto* panel = popup_scene_layout_node(*scene, scene->panel);
    if (!panel || !(panel->spec.style.flags & ui_layout_node_scroll_vertical))
        return false;
    const bool quick_strip = quick_reaction_strip({scene->items.data, scene->item_count});
    u32 quick_index = ~0u;
    if (quick_strip) {
        if (scene->selected_index < kPopupQuickReactionCount &&
            item_enabled(scene->items[scene->selected_index]))
            quick_index = scene->selected_index;
        else
            for (u32 index = 0; index < kPopupQuickReactionCount; ++index)
                if (item_enabled(scene->items[index])) {
                    quick_index = index;
                    break;
                }
    }
    PopupPageSource source{scene, panel, quick_index, quick_strip};
    const u32 count = scene->item_count - (quick_strip ? kPopupQuickReactionCount - 1 : 0);
    const u32 current = scene->selected_index >= scene->item_count ? kUiListNoSelection
                        : !quick_strip                             ? scene->selected_index
                        : scene->selected_index < kPopupQuickReactionCount
                            ? 0
                            : scene->selected_index - kPopupQuickReactionCount + 1;
    const u64 viewport = ui_extent_units(ui_rect_height(panel->content_rectangle));
    const auto target = ui_list_page(count, current, ui_extent_units(scene->scroll_offset), viewport,
                                     viewport + ui_extent_units(scene->scroll_max), reverse, page_bounds,
                                     page_eligible, &source);
    if (!target.ok())
        return false;
    const u32 index = page_source_index(source, target.index);
    const float offset = minimum(ui_extent_dips(target.scroll_units), scene->scroll_max);
    bool scrolled = false;
    if (!popup_scene_scroll(scene, offset - scene->scroll_offset, &scrolled))
        return false;
    *changed = scrolled || scene->selected_index != index;
    scene->selected_index = index;
    return ui_widget_manager_focus(&scene->widgets, scene->rows[index].widget) &&
           ui_widget_manager_hover(&scene->widgets, scene->rows[index].widget);
}

bool popup_scene_select_index(PopupScene* scene, u32 index, bool* changed) noexcept {
    if (!scene || !changed || !scene->active || index >= scene->item_count ||
        !item_enabled(scene->items[index])) {
        return false;
    }
    const u32 previous = scene->selected_index;
    scene->selected_index = index;
    ui_widget_manager_focus(&scene->widgets, scene->rows[index].widget);
    ui_widget_manager_hover(&scene->widgets, scene->rows[index].widget);
    *changed = previous != index;
    return reveal_selection(scene, changed);
}

u32 popup_scene_selected_action(const PopupScene& scene) noexcept {
    return scene.active && scene.selected_index < scene.item_count &&
                   item_enabled(scene.items[scene.selected_index])
               ? scene.items[scene.selected_index].action
               : 0;
}

const UiLayoutNode* popup_scene_layout_node(const PopupScene& scene, UiWidgetHandle widget) noexcept {
    return ui_widget_manager_layout_node(scene.widgets, widget);
}

UiRect popup_feedback_rectangle(const PopupScene& scene) noexcept {
    if (!scene.active || !scene.feedback_reserved)
        return {};
    const auto* panel = popup_scene_layout_node(scene, scene.panel);
    if (!panel)
        return {};
    const float padding = menu_metrics(scene.themed_menu ? scene.theme_styles : nullptr).menu_padding;
    auto r = panel->rectangle;
    r.left += padding;
    r.right -= padding;
    r.bottom -= padding;
    r.top = maximum(panel->rectangle.top + padding, r.bottom - scene.feedback.style.height);
    return ui_rect_intersection(r, scene.viewport);
}

UiRect popup_feedback_slider(const PopupScene& scene) noexcept {
    const UiRect r = popup_feedback_rectangle(scene);
    if (scene.feedback.slider_count < 2 || !ui_rect_has_area(r))
        return {};
    const float inset = scene.feedback.style.text_inset;
    return {r.left + inset, maximum(r.top, r.bottom - 30), r.right - inset, r.bottom - 6};
}

bool popup_scene_set_feedback_slider(PopupScene* scene, u16 value) noexcept {
    if (!scene || !scene->active || scene->feedback.slider_count < 2 || value >= scene->feedback.slider_count ||
        scene->feedback.slider_value == value)
        return false;
    scene->feedback.slider_value = value;
    saturating_increment(scene->revision);
    return true;
}

bool popup_scene_set_feedback(PopupScene* scene, Utf8View text, bool error) noexcept {
    if (!scene || !scene->active || !scene->feedback_reserved || text.bytes > 256 ||
        (!text.data && text.bytes) || !utf8_validate(text).valid)
        return false;
    if (!utf8_equal(scene->feedback.text, text) || scene->feedback.error != error) {
        scene->feedback.text = text;
        scene->feedback.error = error;
        saturating_increment(scene->revision);
    }
    return true;
}

bool popup_scene_resolve_item_layout(const PopupScene& scene, u32 index, PopupItemLayout* layout) noexcept {
    if (layout) {
        *layout = PopupItemLayout{};
    }
    if (!layout || !scene.active || index >= scene.item_count || index >= scene.items.count ||
        index >= scene.rows.count || scene.rows[index].source_index != index) {
        return false;
    }
    const UiLayoutNode* panel = popup_scene_layout_node(scene, scene.panel);
    const UiLayoutNode* row = popup_scene_layout_node(scene, scene.rows[index].widget);
    const UiLayoutNode* label = popup_scene_layout_node(scene, scene.rows[index].label);
    if (!panel || !row || !label || !ui_rect_has_area(panel->rectangle) ||
        !ui_rect_has_area(row->rectangle) || !ui_rect_has_area(label->rectangle) ||
        !contains_rectangle(row->content_rectangle, label->rectangle)) {
        return false;
    }
    UiRect clip = ui_rect_intersection(panel->rectangle, label->clip_rectangle);
    if (scene.feedback_reserved)
        clip.bottom = minimum(clip.bottom, popup_feedback_rectangle(scene).top);
    if (scene.item_count && (scene.items[0].flags & popup_item_command_category)) {
        UiRect body = panel->rectangle;
        body.left += kPopupPanelPadding;
        body.right -= kPopupPanelPadding;
        body.top += kPopupPanelPadding;
        body.bottom -= kPopupPanelPadding;
        if (scene.items[index].flags & popup_item_command_category)
            body.right = minimum(body.right, body.left + 48);
        else
            body.left += 48;
        clip = ui_rect_intersection(clip, body);
    }
    const UiRect visible = ui_rect_intersection(label->rectangle, clip);
    *layout = PopupItemLayout{row, label, visible, clip, ui_rect_has_area(visible)};
    return true;
}

bool popup_scene_menu_row_geometry(const PopupScene& scene, u32 index,
                                   UiMenuRowGeometry* geometry) noexcept {
    if (!geometry || index >= scene.item_count || index >= scene.items.count ||
        !popup_item_menu_row(scene.items[index])) {
        return false;
    }
    PopupItemLayout layout{};
    if (!popup_scene_resolve_item_layout(scene, index, &layout))
        return false;
    const PopupItem& item = scene.items[index];
    UiRect content = layout.label->content_rectangle;
    if (item.flags & (popup_item_avatar | popup_item_emoji_preview))
        content.left = minimum(content.right, layout.row->rectangle.left + 48);
    else if ((!popup_item_menu_presentation(item).icon_plus_one || popup_item_menu_presentation(item).trailing_icon) &&
             !(item.flags & popup_item_guild_icon) &&
             menu_icon_column({scene.items.data, minimum(scene.item_count, scene.items.count)})) {
        // Rows without a leading icon (a trailing submenu chevron included)
        // keep their label in the menu's icon column.
        const auto& metrics = menu_metrics(scene.themed_menu ? scene.theme_styles : nullptr);
        content.left = minimum(content.right, content.left + metrics.menu_icon + metrics.menu_gap);
    }
    // A checked row owns a separate trailing mark. Reserve it before projecting
    // existing icons, shortcut text and detail slots so their ordering stays intact.
    if (item.flags & popup_item_checked) {
        const float extent =
            minimum(menu_metrics(scene.themed_menu ? scene.theme_styles : nullptr).menu_icon,
                    ui_rect_width(content) * .5f);
        content.right = maximum(
            content.left, content.right - extent -
                              menu_metrics(scene.themed_menu ? scene.theme_styles : nullptr).menu_gap);
    }
    const auto presentation = popup_item_menu_presentation(item);
    const bool avatar_lane = presentation.icon_plus_one && !presentation.trailing_icon &&
                             !(item.flags & (popup_item_avatar | popup_item_emoji_preview | popup_item_guild_icon)) &&
                             menu_avatar_column({scene.items.data, minimum(scene.item_count, scene.items.count)});
    if (avatar_lane) {
        // Centre the glyph where member avatars sit; the label starts in their column.
        // popup_avatar_rectangle: a 24 DIP avatar 8 DIP into the row.
        const float avatar_center = layout.row->rectangle.left + 20;
        const float extent = menu_metrics(scene.themed_menu ? scene.theme_styles : nullptr).menu_icon;
        content.left = minimum(content.right, avatar_center - extent * .5f);
    }
    if (!ui_menu_row_project(content, layout.clip_rectangle, presentation, item.detail.bytes != 0,
                             item.shortcut > popup_shortcut_none && item.shortcut < popup_shortcut_count, geometry,
                             scene.themed_menu ? scene.theme_styles : nullptr))
        return false;
    if (avatar_lane) {
        const float label = minimum(content.right, layout.row->rectangle.left + 48);
        geometry->primary.left = maximum(geometry->primary.left, label);
        if (presentation.secondary_line)
            geometry->detail.left = maximum(geometry->detail.left, label);
    }
    return true;
}

} // namespace maicord
