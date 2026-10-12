#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/account_menu.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/background_surface.hpp"

namespace maicord {
static_assert(static_cast<u32>(UiAccountMenuAction::status_online) == 0x41430004 &&
              static_cast<u32>(UiAccountMenuAction::status_invisible) == 0x41430007 &&
              static_cast<u32>(UiAccountMenuAction::downloads) == 0x41430008 &&
              static_cast<u32>(UiAccountMenuAction::log_out) == 0x41430009);
void test_account_menu(void (*check)(bool, const char*) noexcept) noexcept {
    UiAccountMenuModel model{};
    char display[]{'N', 'a', 't', 'i', 'v', 'e'};
    UiAccountMenuIdentity identity{9,     {display, sizeof(display)}, utf8_literal("native.user"), 0,
                                   false, UiPresence::unknown};
    check(ui_account_menu_build(&model, identity) && model.count == 5,
          "account menu: bounded identity and real profile/edit/copy operations build");
    display[0] = 'X';
    check(utf8_equal(model.items[0].label, utf8_literal("Native")) &&
              utf8_equal(model.items[0].detail, utf8_literal("native.user")) && model.items[0].status == 0,
          "account menu: labels are copied and unavailable desired status remains unknown");
    check(ui_account_menu_action_enabled(model, static_cast<u32>(UiAccountMenuAction::view_profile)) &&
              ui_account_menu_action_enabled(model,
                                             static_cast<u32>(UiAccountMenuAction::view_profile_header)) &&
              ui_account_menu_action_enabled(model, static_cast<u32>(UiAccountMenuAction::edit_profile)) &&
              ui_account_menu_action_enabled(model, static_cast<u32>(UiAccountMenuAction::copy_user_id)) &&
              ui_account_menu_action_enabled(model, static_cast<u32>(UiAccountMenuAction::downloads)) &&
              !ui_account_menu_action_enabled(model, static_cast<u32>(UiAccountMenuAction::log_out)) &&
              !ui_account_menu_action_enabled(model, 999),
          "account menu: only published typed operations are admitted and unavailable logout is hidden");
    bool unique_actions = true;
    for (u32 i = 0; i < model.count; ++i)
        for (u32 j = 0; j < i; ++j)
            unique_actions = unique_actions && model.items[i].action != model.items[j].action;
    UiAccountMenuModel authorized{};
    const bool authorized_built =
        ui_account_menu_build(&authorized, {9, utf8_literal("Native"), utf8_literal("native.user"), 0,
                                            false, UiPresence::offline, true, false, true});
    for (u32 i = 0; authorized_built && i < authorized.count; ++i)
        for (u32 j = 0; j < i; ++j)
            unique_actions = unique_actions && authorized.items[i].action != authorized.items[j].action;
    const auto& logout = authorized.items[9];
    bool full_actions = authorized_built && authorized.count == 10;
    for (u32 action = static_cast<u32>(UiAccountMenuAction::view_profile_header);
         action <= static_cast<u32>(UiAccountMenuAction::log_out); ++action)
        full_actions = full_actions && ui_account_menu_action_enabled(authorized, action);
    check(unique_actions && full_actions && utf8_equal(authorized.items[0].label, utf8_literal("Native")) &&
              utf8_equal(authorized.items[8].label, utf8_literal("Downloads")) &&
              utf8_equal(logout.label, utf8_literal("Log out")) &&
              logout.flags == (popup_item_enabled | popup_item_separator_before | popup_item_danger) &&
              ui_menu_row_icon(logout.presentation) == UiIcon::log_out &&
              popup_item_menu_row_height(logout) == kUiMenuRowHeight,
          "account menu: full authorized identity/profile/ID/four-status/Downloads/logout bank has unique "
          "stable actions and a separated danger row with shared original icon and dimensions");
    constexpr u32 widget_capacity = kUiAccountMenuItemCapacity * 2 + 2;
    UiWidget widgets[widget_capacity]{};
    UiLayoutNode nodes[widget_capacity]{};
    PopupItem items[kUiAccountMenuItemCapacity]{};
    PopupRowWidget rows[kUiAccountMenuItemCapacity]{};
    PopupScene popup{};
    check(popup_scene_initialize(&popup, {items, kUiAccountMenuItemCapacity},
                                 {rows, kUiAccountMenuItemCapacity}, {widgets, widget_capacity},
                                 {nodes, widget_capacity}) &&
              ui_account_menu_open_popup(&popup, model, {12, 560, 250, 600}, {0, 0, 800, 640}).ok(),
          "account menu: existing popup bank anchors familiar menu above footer");
    PaintStream themed{};
    Theme gradient = kDefaultTheme;
    gradient.background = background_theme_defaults();
    const bool painted = paint_stream_initialize(&themed) && paint_stream_reset(&themed, 800, 640) &&
                         popup_presentation_build(popup, gradient, &themed);
    // The popover's drop shadow plates come first; the panel plate follows.
    const auto& plate = themed.quads[kUiSurfaceShadowLayers];
    check(painted && popup.panel_material == PopupPanelMaterial::gradient && themed.count > kUiSurfaceShadowLayers &&
              plate.border_width == -7 && plate.reserved_x == -1 && plate.fill.alpha == 1 &&
              plate.radius == kUiMenuCornerRadius && themed.rejected == 0 && themed.clip_depth == 1,
          "account menu: typed opt-in paints one opaque clipped global-gradient panel and palette edge");
    bool flat =
        paint_stream_reset(&themed, 800, 640) && popup_presentation_build(popup, kDefaultTheme, &themed);
    check(flat && themed.quads[kUiSurfaceShadowLayers].border_width == 1 &&
              bytes_equal(&themed.quads[kUiSurfaceShadowLayers].fill, &kDefaultTheme.base_lower, sizeof(Color)),
          "account menu: gradient opt-in keeps flat theme's original opaque panel colors");
    paint_stream_release(&themed);
    UiMenuRowGeometry header{}, edit{};
    check(popup_scene_menu_row_geometry(popup, 0, &header) &&
              popup_scene_menu_row_geometry(popup, 2, &edit) &&
              header.primary.bottom <= header.detail.top && edit.icon.right <= edit.primary.left &&
              popup.rectangle.bottom <= popup.anchor.top,
          "account menu: identity has separate name/username slots and action original icon respects label "
          "geometry");
    bool changed = false;
    check(popup_scene_select_index(&popup, 2, &changed) &&
              popup_scene_selected_action(popup) == static_cast<u32>(UiAccountMenuAction::edit_profile),
          "account menu: reusable popup keyboard selection exposes real edit operation");
    const auto published = model.items[0].label;
    const char invalid[]{static_cast<char>(0xff)};
    check(!ui_account_menu_build(&model, {9, {invalid, 1}, {}, 0, false}) &&
              utf8_equal(published, model.items[0].label),
          "account menu: invalid identity leaves published copied model intact");
    check(ui_account_menu_build(&model, {9, model.items[0].label, model.items[0].detail, 0, false}) &&
              utf8_equal(model.items[0].label, utf8_literal("Native")),
          "account menu: in-place refresh safely accepts old owned label views");
    popup_scene_close(&popup);
    check(popup_scene_open(&popup, PopupKind::popover, 9, ui_account_menu_items(model), {12, 560, 250, 600},
                           {0, 0, 800, 640}, kUiAccountMenuWidth, true)
                  .ok() &&
              popup.panel_material == PopupPanelMaterial::tinted,
          "account menu: generic popup successor resets copied panel material without inheriting account "
          "styling");
    popup_scene_close(&popup);
    check(ui_account_menu_build(&model, {9, utf8_literal("Native"), utf8_literal("native.user"), 0, false,
                                         UiPresence::offline, true}) &&
              model.count == 9 && model.items[7].status == 4 &&
              (model.items[7].flags & popup_item_checked) &&
              ui_account_menu_action_enabled(model,
                                             static_cast<u32>(UiAccountMenuAction::status_invisible)) &&
              popup_scene_open(&popup, PopupKind::popover, 9, ui_account_menu_items(model),
                               {12, 560, 250, 600}, {0, 0, 800, 640}, kUiAccountMenuWidth, true)
                  .ok(),
          "account menu: real backend enables four native status rows with Invisible separately checked");
    UiMenuRowGeometry status_row{};
    check(popup_scene_menu_row_geometry(popup, 7, &status_row) &&
              status_row.icon.right <= status_row.primary.left,
          "account menu: shared presence glyph has bounded slot separate from status label");
    check(!model.items[7].detail.bytes && !model.items[7].presentation.secondary_line,
          "account menu: confirmed status has no pending update cue");
    popup_scene_close(&popup);
    check(ui_account_menu_build(&model, {9, utf8_literal("Native"), utf8_literal("native.user"), 0, false,
                                         UiPresence::offline, true, true}) &&
              model.count == 9 && utf8_equal(model.items[7].detail, utf8_literal("Updating status…")) &&
              model.items[7].presentation.secondary_line && !model.items[4].detail.bytes &&
              !model.items[5].detail.bytes && !model.items[6].detail.bytes &&
              popup_scene_open(&popup, PopupKind::popover, 9, ui_account_menu_items(model),
                               {12, 560, 250, 600}, {0, 0, 800, 640}, kUiAccountMenuWidth, true)
                  .ok(),
          "account menu: pending requested status has one bounded secondary cue without adding rows");
    check(popup_scene_menu_row_geometry(popup, 7, &status_row) &&
              status_row.primary.bottom <= status_row.detail.top && ui_rect_has_area(status_row.detail) &&
              status_row.icon.right <= status_row.primary.left,
          "account menu: pending status cue occupies a separate detail slot without colliding with label");
    popup_scene_close(&popup);
    PopupItemLayout identity_row{}, invisible_row{};
    const bool compact_opened =
        ui_account_menu_open_popup(&popup, model, {12, 164, 250, 180}, {0, 0, 320, 180}).ok();
    check(compact_opened && popup.scroll_max > 0 && popup.rectangle.bottom <= 172 &&
              popup_scene_resolve_item_layout(popup, 0, &identity_row) && identity_row.visible &&
              popup_scene_resolve_item_layout(popup, 7, &invisible_row) && !invisible_row.visible &&
              popup_scene_select_index(&popup, 7, &changed) && changed &&
              popup_scene_resolve_item_layout(popup, 7, &invisible_row) && invisible_row.visible &&
              invisible_row.row->rectangle.bottom <= invisible_row.clip_rectangle.bottom &&
              popup_scene_selected_action(popup) == static_cast<u32>(UiAccountMenuAction::status_invisible),
          "account menu: short native popover starts at identity and keyboard reveals final real status "
          "action");
    PopupItemLayout downloads_row{};
    check(popup_scene_select_index(&popup, 8, &changed) && changed &&
              popup_scene_resolve_item_layout(popup, 8, &downloads_row) && downloads_row.visible &&
              downloads_row.row->rectangle.bottom <= downloads_row.clip_rectangle.bottom &&
              popup_scene_selected_action(popup) == static_cast<u32>(UiAccountMenuAction::downloads),
          "account menu: keyboard reveals Downloads after all existing status actions in compact menu");
    PaintStream compact_paint{};
    const bool compact_painted = paint_stream_initialize(&compact_paint) &&
                                 paint_stream_reset(&compact_paint, 320, 180) &&
                                 popup_presentation_build(popup, kDefaultTheme, &compact_paint);
    const auto* compact_thumb =
        compact_painted && compact_paint.count ? &compact_paint.quads[compact_paint.count - 1] : nullptr;
    check(compact_thumb && compact_thumb->radius == 4 &&
              compact_thumb->left >= popup.rectangle.right - 12 &&
              compact_thumb->right <= popup.rectangle.right && compact_thumb->top >= popup.rectangle.top &&
              compact_thumb->bottom <= popup.rectangle.bottom &&
              popup_scene_select_index(&popup, 0, &changed) && changed && popup.scroll_offset == 0,
          "account menu: clipped status popover paints bounded scrollbar and reveals identity on return");
    paint_stream_release(&compact_paint);
    popup_scene_close(&popup);
    PopupItemLayout logout_row{};
    UiMenuRowGeometry logout_geometry{};
    const bool logout_revealed =
        ui_account_menu_open_popup(&popup, authorized, {12, 164, 250, 180}, {0, 0, 320, 180}).ok() &&
        popup.scroll_max > 0 && popup_scene_select_index(&popup, 9, &changed) &&
        popup_scene_resolve_item_layout(popup, 9, &logout_row) && logout_row.visible &&
        popup_scene_menu_row_geometry(popup, 9, &logout_geometry) &&
        logout_row.row->rectangle.bottom <= logout_row.clip_rectangle.bottom &&
        logout_geometry.icon.right <= logout_geometry.primary.left &&
        popup_scene_selected_action(popup) == static_cast<u32>(UiAccountMenuAction::log_out);
    popup_scene_close(&popup);
    authorized.items[9].flags &= static_cast<u16>(~popup_item_enabled);
    const bool disabled_rejected =
        !ui_account_menu_action_enabled(authorized, static_cast<u32>(UiAccountMenuAction::log_out));
    const bool withdrawn =
        ui_account_menu_build(&authorized, {9, utf8_literal("Native"), utf8_literal("native.user"), 0,
                                            false, UiPresence::offline, true, false, false}) &&
        authorized.count == 9 &&
        !ui_account_menu_action_enabled(authorized, static_cast<u32>(UiAccountMenuAction::log_out)) &&
        ui_account_menu_action_enabled(authorized, static_cast<u32>(UiAccountMenuAction::downloads)) &&
        ui_account_menu_action_enabled(authorized, static_cast<u32>(UiAccountMenuAction::status_invisible));
    check(
        ui_account_menu_build(&model,
                              {9, utf8_literal("Native"), {}, 0, false, UiPresence::unknown, true}) &&
            model.count == 5 &&
            !ui_account_menu_action_enabled(model, static_cast<u32>(UiAccountMenuAction::status_online)) &&
            !ui_account_menu_action_enabled(model, static_cast<u32>(UiAccountMenuAction::log_out)) &&
            logout_revealed && disabled_rejected && withdrawn,
        "account menu: authorized logout keyboard reveals in clipped native popup; disabled or withdrawn "
        "authority rejects logout while existing actions remain and unavailable controls stay hidden");
    UiAccountMenuIdentity plain{9, utf8_literal("Native"), utf8_literal("native.user"), 0, false,
                                UiPresence::online, true};
    plain.developer_mode = false;
    check(ui_account_menu_build(&model, plain) && model.count == 8 &&
              !ui_account_menu_action_enabled(model, static_cast<u32>(UiAccountMenuAction::copy_user_id)) &&
              model.items[3].action == static_cast<u32>(UiAccountMenuAction::status_online) &&
              (model.items[3].flags & popup_item_separator_before) && (model.items[3].flags & popup_item_checked) &&
              model.items[7].action == static_cast<u32>(UiAccountMenuAction::downloads),
          "account menu: Copy user ID is offered only with Developer Mode on");
    UiAccountMenuIdentity status = plain;
    status.custom_status_available = true;
    check(ui_account_menu_build(&model, status) && model.count == 9 &&
              model.items[3].action == static_cast<u32>(UiAccountMenuAction::custom_status) &&
              utf8_equal(model.items[3].label, utf8_literal("Set custom status")) && !model.items[3].detail.bytes,
          "account menu: Set custom status follows Edit profile when no status is set");
    char long_status[200];
    for (auto& c : long_status)
        c = 'a';
    long_status[127] = '\xC3';
    long_status[128] = '\xA9';
    status.custom_status = {long_status, sizeof(long_status)};
    const bool edited = ui_account_menu_build(&model, status) && model.count == 10 &&
                        utf8_equal(model.items[3].label, utf8_literal("Edit custom status")) &&
                        model.items[3].detail.bytes == 127 && model.items[3].presentation.secondary_line &&
                        model.items[4].action == static_cast<u32>(UiAccountMenuAction::clear_custom_status);
    status.custom_status = {};
    check(edited && ui_account_menu_build(&model, status) && model.count == 9,
          "account menu: a set status shows on its row, cut at a whole character, with Clear custom status");
    popup_scene_release(&popup);
}
} // namespace maicord
