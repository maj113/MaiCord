#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/menu_row.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/ui/presence.hpp"
#include "maicord/ui/message_context_menu.hpp"
#include "maicord/ui/text_edit_menu.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
bool inside(UiRect outer, UiRect inner) noexcept {
    return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right &&
           inner.bottom <= outer.bottom;
}

bool painted_slot(const PaintStream& paint, UiRect expected_clip) noexcept {
    for (u32 index = 0; index < paint.batch_count; ++index) {
        const auto& batch = paint.batches[index];
        if (!batch.quad_count || batch.clip_index >= paint.clip_count)
            continue;
        const auto& clip = paint.clips[batch.clip_index];
        if (ui_rect_equal({clip.left, clip.top, clip.right, clip.bottom}, expected_clip))
            return true;
    }
    return false;
}

struct MenuFixture {
    PopupItem items[4]{};
    PopupRowWidget rows[4]{};
    UiWidget widgets[10]{};
    UiLayoutNode nodes[10]{};
    PopupScene scene{};
    ~MenuFixture() noexcept {
        popup_scene_release(&scene);
    }
};
} // namespace

void test_menu_row(void (*check)(bool, const char*) noexcept) noexcept {
    static_assert(sizeof(PopupItem) == 64 && offsetof(PopupItem, regional_flag) == 34);
    {
        PaintStream icons{};
        bool bounded = paint_stream_initialize(&icons);
        constexpr float extents[]{20, 22, 24};
        for (float extent : extents) {
            const UiRect slot{8, 8, 8 + extent, 8 + extent};
            for (u32 id = 0; bounded && id < static_cast<u32>(UiIcon::count); ++id) {
                bounded =
                    paint_stream_reset(&icons, 40, 40) &&
                    ui_icon_paint(&icons, static_cast<UiIcon>(id), slot, kDefaultTheme.text_primary) &&
                    icons.count == 1 && !icons.rejected && ui_icon_quad(icons.quads[0]) &&
                    ui_icon_quad_data(icons.quads[0]).icon == id;
                for (u32 i = 0; bounded && i < icons.count; ++i) {
                    const auto& q = icons.quads[i];
                    bounded = ui_rect_valid({q.left, q.top, q.right, q.bottom}) &&
                              inside(slot, {q.left, q.top, q.right, q.bottom});
                }
            }
        }
        const u32 retained = icons.count;
        bounded = bounded &&
                  !ui_icon_paint(&icons, UiIcon::count, {8, 8, 32, 32}, kDefaultTheme.text_primary) &&
                  icons.count == retained;
        check(bounded,
              "icons: every pack icon is one atlas quad inside its 20/22/24-DIP slot");
        paint_stream_release(&icons);
    }
    const UiRect content{8, 5, 232, 31};
    const UiRect clip{0, 0, 240, 36};
    UiMenuRowGeometry trailing{};
    const auto copy = ui_menu_row_presentation(UiIcon::copy, false, UiMenuIconPlacement::trailing);
    check(ui_menu_row_project(content, clip, copy, true, true, &trailing) &&
              trailing.primary.right <= trailing.detail.left &&
              trailing.detail.right <= trailing.shortcut.left &&
              trailing.shortcut.right <= trailing.icon.left && inside(content, trailing.icon) &&
              inside(content, trailing.primary) && inside(content, trailing.detail) &&
              inside(content, trailing.shortcut),
          "menu row: trailing icon, detail, shortcut and label share nonoverlapping slots");

    UiMenuRowGeometry leading{};
    const auto command = ui_menu_row_presentation(UiIcon::command, true);
    const UiRect tall{8, 5, 232, 51}, cropped{0, 18, 240, 56};
    check(ui_menu_row_project(tall, cropped, command, true, false, &leading) &&
              leading.icon.right <= leading.primary.left && leading.primary.bottom == leading.detail.top &&
              leading.primary.left == leading.detail.left &&
              leading.primary.right == leading.detail.right && leading.clip.top == cropped.top &&
              inside(tall, leading.icon),
          "menu row: leading symbol and stacked helper text share a cropped authoritative clip");
    UiMenuRowGeometry narrow{};
    check(ui_menu_row_project({0, 0, 80, 36}, {0, 0, 80, 36}, copy, true, true, &narrow) &&
              ui_rect_valid_or_empty(narrow.primary) && narrow.primary.right <= narrow.detail.left &&
              narrow.detail.right <= narrow.shortcut.left && narrow.shortcut.right <= narrow.icon.left,
          "menu row: narrow content reduces reserved slots without overlapping text or icon");
    UiMenuRowPresentation invalid{kUiMenuIconInvalid, 0, 0};
    check(!ui_menu_row_project(content, clip, invalid, false, false, &narrow),
          "menu row: invalid original symbol identity fails before projection");
    check(ui_menu_row_project({8, 5, 8, 31}, clip, copy, true, true, &narrow) &&
              !ui_rect_has_area(narrow.primary) && !ui_rect_has_area(narrow.detail) &&
              !ui_rect_has_area(narrow.shortcut),
          "menu row: admitted empty label slots are skipped while the row remains present");

    MenuFixture fixture{};
    PopupItem source[2]{
        {utf8_literal("Copy a long native message label"), 1, popup_item_enabled, popup_shortcut_copy},
        {utf8_literal("/bump"), 2, popup_item_enabled | popup_item_command_row}};
    source[0].presentation = copy;
    source[1].detail = utf8_literal("Bump first · First");
    bool valid = popup_scene_initialize(&fixture.scene, {fixture.items, 4}, {fixture.rows, 4},
                                        {fixture.widgets, 10}, {fixture.nodes, 10}) &&
                 popup_scene_open(&fixture.scene, PopupKind::context_menu, 7, {source, 2}, {10, 10, 11, 11},
                                  {0, 0, 320, 240}, 240)
                     .ok();
    UiMenuRowGeometry first{}, second{};
    const auto* first_row =
        valid ? popup_scene_layout_node(fixture.scene, fixture.rows[0].widget) : nullptr;
    const auto* second_row =
        valid ? popup_scene_layout_node(fixture.scene, fixture.rows[1].widget) : nullptr;
    valid = valid && popup_scene_menu_row_geometry(fixture.scene, 0, &first) &&
            popup_scene_menu_row_geometry(fixture.scene, 1, &second) && first_row && second_row &&
            ui_rect_height(first_row->rectangle) == kUiMenuRowHeight &&
            ui_rect_height(second_row->rectangle) == kUiMenuDetailRowHeight &&
            first.shortcut.right <= first.icon.left && second.icon.right <= second.primary.left &&
            popup_scene_selected_action(fixture.scene) == 1;
    check(valid, "menu row: real popup scene uses shared row heights and icon placement");
    {
        MenuFixture mixed{};
        PopupItem rows[3]{{utf8_literal("Reply"), 1, popup_item_enabled},
                          {utf8_literal("Mark unread"), 2, popup_item_enabled},
                          {utf8_literal("Copy text"), 3, popup_item_enabled}};
        rows[0].presentation = ui_menu_row_presentation(UiIcon::reply);
        rows[2].presentation = ui_menu_row_presentation(UiIcon::copy);
        UiMenuRowGeometry iconic{}, plain{}, lone{};
        bool aligned = popup_scene_initialize(&mixed.scene, {mixed.items, 4}, {mixed.rows, 4},
                                              {mixed.widgets, 10}, {mixed.nodes, 10}) &&
                       popup_scene_open(&mixed.scene, PopupKind::context_menu, 9, {rows, 3}, {10, 10, 11, 11},
                                        {0, 0, 320, 240}, 240)
                           .ok() &&
                       popup_scene_menu_row_geometry(mixed.scene, 0, &iconic) &&
                       popup_scene_menu_row_geometry(mixed.scene, 1, &plain) && !ui_rect_has_area(plain.icon) &&
                       plain.primary.left == iconic.primary.left;
        const auto* iconic_row = aligned ? popup_scene_layout_node(mixed.scene, mixed.rows[0].widget) : nullptr;
        const auto* plain_row = aligned ? popup_scene_layout_node(mixed.scene, mixed.rows[1].widget) : nullptr;
        aligned = aligned && iconic_row && plain_row &&
                  ui_rect_height(plain_row->rectangle) == ui_rect_height(iconic_row->rectangle);
        // A menu without any icon keeps its labels at the content edge.
        aligned = aligned &&
                  popup_scene_open(&mixed.scene, PopupKind::context_menu, 9, {rows + 1, 1}, {10, 10, 11, 11},
                                   {0, 0, 320, 240}, 240)
                      .ok() &&
                  popup_scene_menu_row_geometry(mixed.scene, 0, &lone) && lone.primary.left < plain.primary.left;
        check(aligned, "menu row: icon-less rows share the label column and row height of icon rows in the same menu");
    }

    {
        PaintStream themed_paint{};
        Theme gradient = kDefaultTheme;
        gradient.background = background_theme_defaults();
        bool themed = valid && paint_stream_initialize(&themed_paint) &&
                      paint_stream_reset(&themed_paint, 320, 240) &&
                      popup_presentation_build(fixture.scene, gradient, &themed_paint);
        // Shadow plates precede the panel plate.
        const auto& plate = themed_paint.quads[kUiSurfaceShadowLayers];
        check(themed && themed_paint.count > kUiSurfaceShadowLayers && plate.fill.alpha == 1 &&
                  plate.border_width == -7 && plate.reserved_x == -1 && plate.fill.red == gradient.base_lower.red &&
                  themed_paint.rejected == 0 && themed_paint.clip_depth == 1,
              "menu row: themed panel is an opaque popover-elevation gradient surface with a palette edge");
        gradient.background = {};
        themed = themed && paint_stream_reset(&themed_paint, 320, 240) &&
                 popup_presentation_build(fixture.scene, gradient, &themed_paint);
        check(themed && themed_paint.quads[kUiSurfaceShadowLayers].fill.alpha == kDefaultTheme.base_lower.alpha,
              "menu row: disabled background preserves original opaque panel");
        paint_stream_release(&themed_paint);
    }

    PaintStream paint{};
    UiTextCommand commands[8]{};
    UiTextCommandStream text{};
    UiTextOutput text_output{&text, nullptr};
    // Initialize text independently so a paint assertion cannot invalidate
    // later color or narrow-avatar fixtures by leaving their stream unbound.
    const bool text_initialized = ui_text_command_stream_initialize(&text, {commands, 8});
    valid = valid && paint_stream_initialize(&paint) && paint_stream_reset(&paint, 320, 240) &&
            popup_presentation_build(fixture.scene, kDefaultTheme, &paint) &&
            painted_slot(paint, ui_menu_row_slot_clip(first, first.icon)) &&
            painted_slot(paint, ui_menu_row_slot_clip(second, second.icon)) && text_initialized &&
            popup_text_presentation_build_output(fixture.scene, kDefaultTheme, &text_output) && text.count == 4;
    bool authoritative = valid;
    bool primary_seen = false, helper_seen = false, shortcut_seen = false;
    for (u32 index = 0; authoritative && index < text.count; ++index) {
        const auto& emitted = commands[index];
        const bool original_owner = emitted.owner.manager == &fixture.scene.widgets &&
                                    (emitted.owner.handle.slot == fixture.rows[0].label.slot ||
                                     emitted.owner.handle.slot == fixture.rows[1].label.slot);
        authoritative =
            original_owner && emitted.owner.handle.generation == fixture.scene.widgets.generation;
        if (utf8_equal(emitted.text, source[0].label))
            primary_seen = ui_rect_equal(emitted.rectangle, first.primary) &&
                           ui_rect_equal(emitted.clip, ui_menu_row_slot_clip(first, first.primary));
        if (utf8_equal(emitted.text, source[1].detail))
            helper_seen = ui_rect_equal(emitted.rectangle, second.detail) &&
                          ui_rect_equal(emitted.clip, ui_menu_row_slot_clip(second, second.detail));
        if (utf8_equal(emitted.text, utf8_literal("Ctrl+C")))
            shortcut_seen = ui_rect_equal(emitted.rectangle, first.shortcut);
    }
    check(authoritative && primary_seen && helper_seen && shortcut_seen,
          "menu row: painter and text consume shared geometry with the real retained label owners");

    source[0].flags |= popup_item_checked;
    valid = popup_scene_open(&fixture.scene, PopupKind::context_menu, 7, {source, 2}, {10, 10, 11, 11},
                             {0, 0, 320, 240}, 240)
                .ok();
    PopupItemLayout checked_layout{};
    valid = valid && popup_scene_menu_row_geometry(fixture.scene, 0, &first) &&
            popup_scene_resolve_item_layout(fixture.scene, 0, &checked_layout);
    const auto checked_content = checked_layout.label ? checked_layout.label->content_rectangle : UiRect{};
    const float mark_extent = minimum(kUiMenuIconExtent, ui_rect_width(checked_content) * .5f);
    const float mark_top = (checked_content.top + checked_content.bottom - mark_extent) * .5f;
    const UiRect mark{checked_content.right - mark_extent, mark_top, checked_content.right,
                      mark_top + mark_extent};
    ui_text_command_stream_reset(&text);
    check(valid && first.primary.right <= first.shortcut.left && first.shortcut.right <= first.icon.left &&
              first.icon.right <= mark.left && paint_stream_reset(&paint, 320, 240) &&
              popup_presentation_build(fixture.scene, kDefaultTheme, &paint) &&
              painted_slot(paint, ui_rect_intersection(mark, checked_layout.clip_rectangle)) &&
              popup_text_presentation_build_output(fixture.scene, kDefaultTheme, &text_output) && text.count == 4,
          "menu row: checked mark has a separate painted trailing slot after existing icon and shortcut");
    source[0].status = static_cast<u8>(UiPresence::offline);
    valid = popup_scene_open(&fixture.scene, PopupKind::context_menu, 7, {source, 2}, {10, 10, 11, 11},
                             {0, 0, 320, 240}, 240)
                .ok() &&
            popup_scene_menu_row_geometry(fixture.scene, 0, &first) &&
            paint_stream_reset(&paint, 320, 240) &&
            popup_presentation_build(fixture.scene, kDefaultTheme, &paint);
    bool ring_visible = false;
    for (u32 i = 0; valid && i < paint.count; ++i) {
        const auto& quad = paint.quads[i];
        ring_visible |= inside(first.icon, {quad.left, quad.top, quad.right, quad.bottom}) &&
                        quad.fill.red == kDefaultTheme.text_secondary.red &&
                        quad.fill.green == kDefaultTheme.text_secondary.green &&
                        quad.fill.blue == kDefaultTheme.text_secondary.blue;
    }
    check(valid && ring_visible,
          "menu row: highlighted Invisible ring uses readable popup foreground contrast");
    source[0].status = 0;
    source[0].flags &= static_cast<u16>(~popup_item_checked);

    bool selection_changed = false;
    check(popup_scene_move_selection(&fixture.scene, 1, &selection_changed) && selection_changed &&
              popup_scene_selected_action(fixture.scene) == 2,
          "menu row: icon decoration preserves existing keyboard action and selection ownership");
    source[0].flags = popup_item_danger;
    valid = popup_scene_open(&fixture.scene, PopupKind::context_menu, 7, {source, 2}, {10, 10, 11, 11},
                             {0, 0, 320, 240}, 240)
                .ok();
    ui_text_command_stream_reset(&text);
    valid = valid && popup_scene_selected_action(fixture.scene) == 2 &&
            popup_text_presentation_build_output(fixture.scene, kDefaultTheme, &text_output);
    bool disabled_color = false;
    for (u32 index = 0; valid && index < text.count; ++index) {
        if (utf8_equal(commands[index].text, source[0].label)) {
            const auto color = commands[index].color;
            disabled_color = color.red == kDefaultTheme.text_secondary.red &&
                             color.green == kDefaultTheme.text_secondary.green &&
                             color.blue == kDefaultTheme.text_secondary.blue;
        }
    }
    check(valid && disabled_color,
          "menu row: disabled danger action keeps disabled colors and is skipped by keyboard selection");

    const u32 specialized_revision = fixture.scene.revision;
    source[0].flags = popup_item_enabled | popup_item_emoji;
    check(!popup_scene_open(&fixture.scene, PopupKind::context_menu, 8, {source, 2}, {10, 10, 11, 11},
                            {0, 0, 320, 240}, 240)
                  .ok() &&
              fixture.scene.revision == specialized_revision,
          "menu row: picker adapters reject conflicting standard-row metadata before replacing a popup");
    source[0].flags = popup_item_danger;

    const u32 revision = fixture.scene.revision;
    const auto retained_row = fixture.rows[0].widget;
    source[0].presentation = invalid;
    check(!popup_scene_open(&fixture.scene, PopupKind::context_menu, 8, {source, 2}, {10, 10, 11, 11},
                            {0, 0, 320, 240}, 240)
                  .ok() &&
              fixture.scene.owner_id == 7 && fixture.scene.revision == revision &&
              fixture.rows[0].widget.generation == retained_row.generation,
          "menu row: invalid icon metadata preserves the current popup and its generation");
    fixture.rows[0].widget.generation ^= 1;
    check(!popup_scene_menu_row_geometry(fixture.scene, 0, &first),
          "menu row: recycled row handles cannot emit icon or text geometry");
    fixture.rows[0].widget = retained_row;
    fixture.rows[0].source_index = 3;
    check(!popup_scene_menu_row_geometry(fixture.scene, 0, &first),
          "menu row: a tampered source index cannot project another retained row's text or icon");
    fixture.rows[0].source_index = 0;
    paint_stream_release(&paint);

    MessageRecord message{};
    message.id = 10;
    message.channel_id = 7;
    message.author_id = 9;
    MessageContextMenuModel context{};
    bool decorated = message_context_menu_build(&context, message, true, true, true, nullptr, true).ok();
    for (u32 index = 0; index < context.count; ++index) {
        const auto& item = context.items[index];
        if (item.flags & popup_item_quick_reaction)
            decorated = decorated && item.presentation.icon_plus_one == 0;
        else
            decorated = decorated && item.presentation.icon_plus_one && !item.presentation.trailing_icon;
    }
    check(decorated, "menu row: message context actions use leading originals and quick reactions retain "
                     "their adapter");
    {
        PopupItem items[kMessageContextMaximumItems]{};
        PopupRowWidget rows[kMessageContextMaximumItems]{};
        UiWidget widgets[2 * kMessageContextMaximumItems + 2]{};
        UiLayoutNode nodes[2 * kMessageContextMaximumItems + 2]{};
        PopupScene complete{};
        PaintStream combined{};
        bool fits =
            popup_scene_initialize(&complete, {items, kMessageContextMaximumItems},
                                   {rows, kMessageContextMaximumItems},
                                   {widgets, 2 * kMessageContextMaximumItems + 2},
                                   {nodes, 2 * kMessageContextMaximumItems + 2}) &&
            popup_scene_open(&complete, PopupKind::context_menu, 7, {context.items, context.count},
                             {10, 10, 11, 11}, {0, 0, 640, 1000}, 300)
                .ok() &&
            paint_stream_initialize(&combined) && paint_stream_reset(&combined, 640, 1000);
        UiResolvedThemeStyles picker_styles{};
        picker_styles.ready = true;
        picker_styles.geometry_revision = 1;
        picker_styles.metrics.menu_padding = 16.f;
        picker_styles.metrics.menu_row_height = 64.f;
        picker_styles.metrics.menu_gap = 24.f;
        const auto picker_rectangle = complete.rectangle;
        const u16 picker_generation = complete.widgets.generation;
        const u32 picker_layout_revision = complete.widgets.layout.revision;
        const auto* quick_node = fits ? popup_scene_layout_node(complete, rows[0].widget) : nullptr;
        const UiRect quick_rectangle = quick_node ? quick_node->rectangle : UiRect{};
        check(fits && popup_scene_bind_theme_styles(&complete, &picker_styles) && !complete.themed_menu &&
                  ui_rect_equal(complete.rectangle, picker_rectangle) &&
                  complete.widgets.generation == picker_generation &&
                  complete.widgets.layout.revision == picker_layout_revision && quick_node &&
                  ui_rect_equal(quick_node->rectangle, quick_rectangle),
              "popup theme: specialized quick reaction geometry remains independent of ordinary menu "
              "metrics");
        // Reserve half the shared stream for the underlying native shell.
        for (u32 i = 0; fits && i < kMaxPaintQuads / 2; ++i)
            fits = paint_stream_push(&combined, 0, 0, 1, 1, kDefaultTheme.surface);
        fits = fits && popup_presentation_build(complete, kDefaultTheme, &combined) &&
               combined.count <= kMaxPaintQuads && !combined.rejected && combined.clip_depth == 1;
        check(fits, "native icons: full authoritative context menu fits shared 512-quad stream with shell "
                    "headroom");
        paint_stream_release(&combined);
        popup_scene_release(&complete);
    }
    PopupItem edit[kTextEditMenuItems]{};
    decorated =
        text_edit_menu_build({true, true, true, true, true, true, true, false}, {edit, kTextEditMenuItems});
    for (const auto& item : edit)
        decorated = decorated && item.presentation.icon_plus_one && !item.presentation.trailing_icon;
    check(decorated, "menu row: shared edit menu carries original symbols alongside command shortcuts");

    MenuFixture avatar{};
    PopupItem avatar_item{utf8_literal("/bump"), 1,
                          popup_item_enabled | popup_item_command_row | popup_item_avatar};
    avatar_item.media_id = 9;
    avatar_item.detail = utf8_literal("Bump first · First");
    UiMenuRowGeometry avatar_geometry{};
    PopupMediaCommand avatar_media{};
    valid = popup_scene_initialize(&avatar.scene, {avatar.items, 4}, {avatar.rows, 4}, {avatar.widgets, 10},
                                   {avatar.nodes, 10}) &&
            popup_scene_open(&avatar.scene, PopupKind::autocomplete, 7, {&avatar_item, 1},
                             {10, 180, 250, 220}, {0, 0, 320, 240}, 240)
                .ok() &&
            popup_scene_menu_row_geometry(avatar.scene, 0, &avatar_geometry) &&
            !ui_rect_has_area(avatar_geometry.icon) &&
            avatar_geometry.primary.bottom == avatar_geometry.detail.top &&
            popup_media_presentation_resolve(avatar.scene, 0, &avatar_media) ==
                PopupMediaPresentationStatus::ready &&
            avatar_media.avatar && avatar_geometry.primary.left > avatar_media.rectangle.right;
    check(valid,
          "menu row: existing command-avatar ownership replaces the leading symbol without text collision");
    ui_text_command_stream_reset(&text);
    valid = popup_scene_open(&avatar.scene, PopupKind::autocomplete, 7, {&avatar_item, 1},
                             {10, 180, 58, 220}, {0, 0, 320, 240}, 48)
                .ok() &&
            avatar.scene.active && popup_scene_menu_row_geometry(avatar.scene, 0, &avatar_geometry) &&
            !ui_rect_has_area(avatar_geometry.primary) &&
            popup_text_presentation_build_output(avatar.scene, kDefaultTheme, &text_output) && text.count == 0 &&
            popup_media_presentation_resolve(avatar.scene, 0, &avatar_media) ==
                PopupMediaPresentationStatus::ready;
    check(valid, "menu row: narrow avatar autocomplete keeps media and safely omits empty text slots");
    {
        UiResolvedThemeStyles styles{};
        styles.ready = true;
        styles.revision = styles.geometry_revision = styles.paint_revision = 1;
        UiMenuRowGeometry defaults{};
        check(ui_menu_row_project(content, clip, copy, true, true, &defaults, &styles) &&
                  ui_rect_equal(defaults.icon, trailing.icon) &&
                  ui_rect_equal(defaults.primary, trailing.primary),
              "menu row: resolved default metrics preserve exact slot geometry");
        styles.metrics.menu_padding = 10.f;
        styles.metrics.menu_gap = 12.f;
        styles.metrics.menu_icon = 24.f;
        styles.metrics.menu_compact_height = 40.f;
        styles.metrics.menu_row_height = 48.f;
        styles.metrics.menu_detail_height = 72.f;
        styles.metrics.menu_radius = 14.f;
        UiMenuRowGeometry custom{};
        check(ui_menu_row_project(content, clip, copy, true, true, &custom, &styles) &&
                  ui_rect_width(custom.icon) == 24.f && custom.shortcut.right + 12.f == custom.icon.left &&
                  custom.detail.right + 12.f == custom.shortcut.left &&
                  custom.primary.right + 12.f == custom.detail.left,
              "menu row: resolved icon extent and gaps drive every reserved slot");
        MenuFixture themed{};
        PopupItem entries[4]{
            {utf8_literal("Plain"), 10, popup_item_enabled},
            {utf8_literal("Icon"), 11, popup_item_enabled},
            {utf8_literal("Detail"), 12, popup_item_enabled},
            {utf8_literal("Separated"), 13, popup_item_enabled | popup_item_separator_before}};
        entries[1].presentation = ui_menu_row_presentation(UiIcon::copy);
        entries[2].presentation = ui_menu_row_presentation(UiIcon::command, true);
        entries[2].detail = utf8_literal("Retained helper text");
        const UiPopupFeedback feedback{utf8_literal("Copied feedback"), {36.f, 6.f}, true};
        bool initialized =
            popup_scene_initialize(&themed.scene, {themed.items, 4}, {themed.rows, 4}, {themed.widgets, 10},
                                   {themed.nodes, 10}) &&
            popup_scene_open(&themed.scene, PopupKind::context_menu, 77, {entries, 4}, {10, 10, 11, 11},
                             {0, 0, 320, 160}, 240.f, false, 0.f, 0.f, 5, &feedback)
                .ok();
        bool selected{};
        initialized &= popup_scene_select_index(&themed.scene, 2, &selected);
        const u16 generation = themed.scene.widgets.generation;
        const UiWidgetHandle selected_widget = themed.rows[2].widget;
        const u32 opens = themed.scene.opens;
        const float scroll = themed.scene.scroll_offset;
        bool rebound = initialized && popup_scene_bind_theme_styles(&themed.scene, &styles) &&
                       themed.scene.owner_id == 77 && themed.scene.selected_index == 2 &&
                       popup_scene_selected_action(themed.scene) == 12 &&
                       themed.scene.widgets.generation == generation && themed.scene.opens == opens &&
                       themed.rows[2].widget.slot == selected_widget.slot &&
                       themed.rows[2].widget.generation == selected_widget.generation &&
                       themed.scene.scroll_offset == scroll && themed.scene.feedback.error &&
                       utf8_equal(themed.scene.feedback.text, feedback.text);
        // The icon row gives its plain neighbours the full row height of the shared column.
        const float expected_heights[]{48.f, 48.f, 72.f, 48.f};
        for (u32 i = 0; i < 4; ++i) {
            const auto* node = popup_scene_layout_node(themed.scene, themed.rows[i].widget);
            rebound &= node && ui_rect_height(node->rectangle) == expected_heights[i] &&
                       utf8_equal(themed.items[i].label, entries[i].label);
        }
        const auto* panel = popup_scene_layout_node(themed.scene, themed.scene.panel);
        rebound &= panel && panel->spec.style.padding.left == 10.f &&
                   popup_feedback_rectangle(themed.scene).left == panel->rectangle.left + 10.f;
        UiLayoutDiagnostic diagnostics[16]{};
        rebound &= ui_layout_engine_validate(themed.scene.widgets.layout, {diagnostics, 16}).clean();
        check(rebound, "popup theme: active reflow consumes actual row metrics and preserves owner "
                       "selection widgets edit feedback and scroll");
        const u32 layout_revision = themed.scene.widgets.layout.revision;
        const u32 scene_revision = themed.scene.revision;
        Theme theme = kDefaultTheme;
        theme.styles = &styles;
        PaintStream styled{};
        bool painted = paint_stream_initialize(&styled) && paint_stream_reset(&styled, 320, 160) &&
                       popup_presentation_build(themed.scene, theme, &styled) &&
                       styled.count > kUiSurfaceShadowLayers && styled.quads[kUiSurfaceShadowLayers].radius == 14.f &&
                       styled.quads[0].radius == 14.f + kUiSurfaceShadowStep * kUiSurfaceShadowLayers;
        ++styles.revision;
        ++styles.paint_revision;
        styles.metrics.menu_radius = 18.f;
        painted &= !popup_scene_bind_theme_styles(&themed.scene, &styles) &&
                   themed.scene.widgets.layout.revision == layout_revision &&
                   themed.scene.revision == scene_revision && paint_stream_reset(&styled, 320, 160) &&
                   popup_presentation_build(themed.scene, theme, &styled) &&
                   styled.quads[kUiSurfaceShadowLayers].radius == 18.f;
        check(painted, "popup theme: paint-only radius publication preserves retained geometry and updates "
                       "actual panel paint");
        paint_stream_release(&styled);
        ++styles.geometry_revision;
        styles.metrics.menu_gap = 16.f;
        bool restored = popup_scene_bind_theme_styles(&themed.scene, &styles) &&
                        !popup_scene_bind_theme_styles(&themed.scene, &styles) &&
                        themed.scene.widgets.generation == generation &&
                        popup_scene_bind_theme_styles(&themed.scene, nullptr);
        const auto* restored_row = popup_scene_layout_node(themed.scene, themed.rows[0].widget);
        restored &= restored_row && restored_row->spec.style.height.value == kUiMenuRowHeight &&
                    popup_scene_selected_action(themed.scene) == 12;
        check(restored,
              "popup theme: geometry publication reflows once and restoring defaults preserves selection "
              "authority");
    }
    {
        // The server header popout stays below the header, shortened to the
        // space left and scrolled, instead of sliding up over the header.
        MenuFixture header{};
        PopupItem rows[4]{{utf8_literal("Invite people"), 1, popup_item_enabled},
                          {utf8_literal("Server settings"), 2, popup_item_enabled},
                          {utf8_literal("Create channel"), 3, popup_item_enabled},
                          {utf8_literal("Leave server"), 4, popup_item_enabled}};
        const UiRect anchor{10, 40, 230, 64};
        const UiRect short_viewport{0, 0, 320, 64 + 80 + kPopupOuterMargin};
        bool below = popup_scene_initialize(&header.scene, {header.items, 4}, {header.rows, 4},
                                            {header.widgets, 10}, {header.nodes, 10}) &&
                     popup_scene_open_below(&header.scene, PopupKind::context_menu, 5, {rows, 4}, anchor,
                                            short_viewport, 220)
                         .ok() &&
                     header.scene.anchored_below && header.scene.rectangle.top == anchor.bottom &&
                     header.scene.rectangle.bottom == short_viewport.bottom - kPopupOuterMargin &&
                     header.scene.scroll_max > 0;
        check(below, "popup placement: header popout opens below its header, capped and scrollable");
        // A follow-up menu replacing it (same anchor) keeps the placement; an
        // unrelated menu does not inherit it.
        below = popup_scene_open(&header.scene, PopupKind::context_menu, 5, {rows, 3}, anchor, short_viewport, 220)
                    .ok() &&
                header.scene.anchored_below && header.scene.rectangle.top == anchor.bottom;
        const bool unrelated =
            popup_scene_close(&header.scene) &&
            popup_scene_open(&header.scene, PopupKind::context_menu, 6, {rows, 4}, {10, 100, 230, 124},
                             short_viewport, 220)
                .ok() &&
            !header.scene.anchored_below && header.scene.rectangle.top < 124;
        check(below && unrelated, "popup placement: follow-up menus keep the header placement, others do not");
    }
}
} // namespace maicord
