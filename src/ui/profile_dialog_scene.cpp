#include "maicord/ui/profile_dialog_scene.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/sys/crt.hpp"
#include "maicord/media/asset.hpp"

namespace maicord {
namespace {
// Layout constants in DIPs: the image-crop dialog (editor 350 or 200 tall) and
// the collectible pickers (3 tile columns with gutters, plus a preview panel).
constexpr float kCropWidth = 480, kPickerWidth = 620, kSide = 24, kTop = 16, kTitleLine = 28, kBodyGap = 8;
constexpr float kToolsPad = 16, kTool = 24, kSliderWidth = 128, kSliderGap = 8, kRotateBox = 36;
constexpr float kFooterPad = 16, kFooterButton = 38, kFooterGap = 8, kClose = 32;
constexpr float kMaskBorder = 5, kSliderTrack = 6, kSliderThumb = 16;
constexpr float kTile = 80, kTileGap = 12, kSectionHeader = 28, kPreviewPanel = 280, kPreviewBox = 208;
constexpr float kPickerHeight = 420;

enum Role : u8 {
    role_none,
    role_panel,
    role_title,
    role_close,
    role_editor,
    role_slider,
    role_small_icon,
    role_large_icon,
    role_rotate,
    role_reset,
    role_cancel,
    role_apply,
    role_grid,
    role_tile,
    role_section,
    role_preview,
    role_preview_label,
    role_status,
    role_small_preview,
};

bool crop(const UiProfileDialogScene& s) noexcept {
    return s.content.kind == UiProfileDialogKind::crop;
}
UiRect panel_rect(UiProfileDialogKind kind, UiRect viewport, float height) noexcept {
    const float width = minimum(kind == UiProfileDialogKind::crop ? kCropWidth : kPickerWidth,
                                maximum(240.f, ui_rect_width(viewport) - 32));
    const float left = viewport.left + (ui_rect_width(viewport) - width) * .5f;
    const float top = viewport.top + maximum(16.f, (ui_rect_height(viewport) - height) * .5f);
    return {left, top, left + width, top + height};
}
float crop_height(UiRect viewport) noexcept {
    return kTop + kTitleLine + kBodyGap * 2 + image_crop_editor_height(ui_rect_height(viewport)) + kToolsPad * 2 +
           kTool + kFooterButton + kFooterPad * 2;
}
UiProfileDialogPart* add(UiProfileDialogScene* s, UiWidgetHandle parent, const UiRect& origin, const UiRect& r, Role role,
                         UiProfileDialogAction action = UiProfileDialogAction::none, const Utf8View& text = {},
                         u16 source = 0, u32 style = 0, bool clip = false) noexcept {
    if (s->part_count >= kUiProfileDialogPartCapacity || !ui_rect_has_area(r))
        return nullptr;
    auto layout = ui_layout_style();
    layout.width = ui_layout_fixed(ui_rect_width(r));
    layout.height = ui_layout_fixed(ui_rect_height(r));
    layout.margin.left = r.left - origin.left;
    layout.margin.top = r.top - origin.top;
    layout.horizontal_alignment = UiLayoutAlignment::start;
    layout.vertical_alignment = UiLayoutAlignment::start;
    layout.flags = ui_layout_node_visible | ui_layout_node_allow_overlap | (clip ? ui_layout_node_clip_children : 0) |
                   (text.bytes ? ui_layout_node_text_content : 0);
    const u32 id = 2 + s->part_count;
    const bool interactive = action != UiProfileDialogAction::none;
    const auto widget = ui_widget_manager_add(
        &s->widgets, parent,
        {id, clip ? UiWidgetKind::container : interactive ? UiWidgetKind::button : UiWidgetKind::text,
         static_cast<u16>(ui_widget_enabled | (interactive ? ui_widget_hit_test | ui_widget_focusable : 0)),
         {utf8_literal("ProfileDialogPart"), layout, {}, 0, id}});
    if (!widget.ok())
        return nullptr;
    auto& part = s->parts[s->part_count++];
    new (&part) UiProfileDialogPart{};
    part.widget = widget.handle;
    part.text = text;
    part.action = action;
    part.source = source;
    part.style = style;
    part.role = role;
    return &part;
}
// The panel with its title and close button, shared by every dialog kind.
bool add_frame(UiProfileDialogScene* s, UiWidgetHandle root, const UiRect& viewport, const UiRect& panel) noexcept {
    s->rectangle = panel;
    return add(s, root, viewport, panel, role_panel) &&
           add(s, root, viewport, {panel.left + kSide, panel.top + kTop, panel.right - kSide - kClose, panel.top + kTop + kTitleLine},
               role_title, UiProfileDialogAction::none, s->content.title, 0, text_style_heading_2 | text_style_bold) &&
           add(s, root, viewport,
               {panel.right - kTop - kClose, panel.top + kTop - 2, panel.right - kTop, panel.top + kTop - 2 + kClose},
               role_close, UiProfileDialogAction::close);
}
const UiLayoutNode* node(const UiProfileDialogScene& s, const UiProfileDialogPart& part) noexcept {
    return ui_widget_manager_layout_node(s.widgets, part.widget);
}
UiRect visible(const UiProfileDialogScene& s, const UiProfileDialogPart& part) noexcept {
    const auto* n = node(s, part);
    return n ? ui_rect_intersection(n->rectangle, n->clip_rectangle) : UiRect{};
}
bool hovered(const UiProfileDialogScene& s, const UiProfileDialogPart& part) noexcept {
    return ui_widget_handle_equal(s.widgets.hovered, part.widget);
}
bool focused(const UiProfileDialogScene& s, const UiProfileDialogPart& part) noexcept {
    return ui_widget_handle_equal(s.widgets.focused, part.widget);
}
bool enabled(const UiProfileDialogScene& s, const UiProfileDialogPart& part) noexcept {
    const auto& c = s.content;
    switch (part.role) {
    case role_apply:
        return c.can_apply && !c.busy;
    case role_reset:
        return c.can_reset && !c.busy;
    case role_slider:
    case role_rotate:
    case role_editor:
        return c.image_ready && !c.busy;
    default:
        return !c.busy || part.role == role_cancel || part.role == role_close;
    }
}
u32 grid_columns(float width) noexcept {
    const u32 columns = static_cast<u32>((width + kTileGap) / (kTile + kTileGap));
    return columns < 1 ? 1 : columns > 3 ? 3 : columns;
}
// Apply and Cancel, right-aligned on the footer row at top.
bool add_buttons(UiProfileDialogScene* s, UiWidgetHandle root, const UiRect& viewport, float right, float top) noexcept {
    const UiSettingsStyleTokens t(kDefaultTheme.styles);
    const float apply_w = ui_settings_button_width(
        t, UiSettingsButtonSize::regular, ui_settings_text_width(s->content.apply_label, kUiSettingsDescriptionEm) * 1.08f);
    const float cancel_w = ui_settings_button_width(
        t, UiSettingsButtonSize::regular, ui_settings_text_width(utf8_literal("Cancel"), kUiSettingsDescriptionEm) * 1.08f);
    return add(s, root, viewport, {right - apply_w, top, right, top + kFooterButton}, role_apply,
               UiProfileDialogAction::apply, s->content.apply_label, 0, text_style_subtext | text_style_bold) &&
           add(s, root, viewport, {right - apply_w - kFooterGap - cancel_w, top, right - apply_w - kFooterGap, top + kFooterButton},
               role_cancel, UiProfileDialogAction::cancel, utf8_literal("Cancel"), 0, text_style_subtext | text_style_bold);
}
bool build_crop(UiProfileDialogScene* s, UiWidgetHandle root, UiRect viewport) noexcept {
    const auto panel = panel_rect(UiProfileDialogKind::crop, viewport, crop_height(viewport));
    if (!add_frame(s, root, viewport, panel))
        return false;
    const float left = panel.left + kSide, right = panel.right - kSide;
    s->crop_viewport = ui_profile_dialog_crop_editor(viewport);
    const auto editor = s->crop_viewport;
    if (!add(s, root, viewport, editor, role_editor, UiProfileDialogAction::pan))
        return false;
    // Tools: centred zoom control (36 DIP inline padding balances the rotate
    // button), the rotate button at the end.
    const float tools_top = editor.bottom + kToolsPad;
    const float group = 16 + kSliderGap + kSliderWidth + kSliderGap + kTool;
    const float group_left = editor.left + 36 + (ui_rect_width(editor) - 36 - group) * .5f;
    const float middle = tools_top + kTool * .5f;
    s->slider_track = {group_left + 16 + kSliderGap, middle - kSliderTrack * .5f,
                       group_left + 16 + kSliderGap + kSliderWidth, middle + kSliderTrack * .5f};
    if (!add(s, root, viewport, {group_left, middle - 8, group_left + 16, middle + 8}, role_small_icon) ||
        !add(s, root, viewport,
             {s->slider_track.left - kSliderThumb * .5f, middle - kSliderThumb * .5f,
              s->slider_track.right + kSliderThumb * .5f, middle + kSliderThumb * .5f},
             role_slider, UiProfileDialogAction::zoom) ||
        !add(s, root, viewport,
             {s->slider_track.right + kSliderGap, tools_top, s->slider_track.right + kSliderGap + kTool,
              tools_top + kTool},
             role_large_icon) ||
        !add(s, root, viewport, {editor.right - kRotateBox, middle - kRotateBox * .5f, editor.right, middle + kRotateBox * .5f},
             role_rotate, UiProfileDialogAction::rotate))
        return false;
    const float top = tools_top + kTool + kToolsPad + kFooterPad;
    return add(s, root, viewport, {left, top, left + 64, top + kFooterButton}, role_reset, UiProfileDialogAction::reset,
               utf8_literal("Reset"), 0, text_style_subtext | text_style_bold) &&
           add_buttons(s, root, viewport, right, top);
}
bool build_picker(UiProfileDialogScene* s, UiWidgetHandle root, UiRect viewport) noexcept {
    const float height = minimum(kTop + kTitleLine + 16 + kPickerHeight + kFooterButton + kFooterPad * 2,
                                 maximum(240.f, ui_rect_height(viewport) - 32));
    const auto panel = panel_rect(s->content.kind, viewport, height);
    if (!add_frame(s, root, viewport, panel))
        return false;
    const float left = panel.left + kSide, right = panel.right - kSide;
    const float body_top = panel.top + kTop + kTitleLine + 16;
    const float footer_top = panel.bottom - kFooterPad - kFooterButton;
    const bool side = ui_rect_width(panel) - kSide * 2 >= 3 * kTile + 2 * kTileGap + 16 + kPreviewPanel;
    const float grid_right = side ? right - kPreviewPanel - 16 : right;
    s->grid_viewport = {left, body_top, grid_right, footer_top - kFooterPad};
    auto* grid = add(s, root, viewport, s->grid_viewport, role_grid, UiProfileDialogAction::none, {}, 0, 0, true);
    if (!grid)
        return false;
    // One section: "Your Decorations" / "Your Effects" (owned and None).
    const u32 columns = grid_columns(ui_rect_width(s->grid_viewport));
    const float tile_w = (ui_rect_width(s->grid_viewport) - (columns - 1) * kTileGap) / columns;
    const u32 count = s->content.items.count;
    const u32 rows = (count + columns - 1) / columns;
    s->grid_height = kSectionHeader + rows * (kTile + kTileGap);
    s->grid_scroll = maximum(0.f, minimum(s->grid_scroll, s->grid_height - ui_rect_height(s->grid_viewport)));
    const UiRect origin{s->grid_viewport.left, s->grid_viewport.top - s->grid_scroll, s->grid_viewport.right,
                        s->grid_viewport.bottom};
    // The header scrolls out of view with its tiles (no part when hidden).
    add(s, grid->widget, origin, {origin.left, origin.top, origin.right, origin.top + kSectionHeader - 8}, role_section,
        UiProfileDialogAction::none,
        s->content.kind == UiProfileDialogKind::decoration ? utf8_literal("Your Decorations")
                                                           : utf8_literal("Your Effects"),
        0, text_style_bold);
    for (u32 i = 0; i < count && i < kUiProfileDialogItemCapacity; ++i) {
        const u32 row = i / columns, column = i % columns;
        const float x = origin.left + column * (tile_w + kTileGap);
        const float y = origin.top + kSectionHeader + row * (kTile + kTileGap);
        if (y + kTile < s->grid_viewport.top || y > s->grid_viewport.bottom)
            continue;
        if (!add(s, grid->widget, origin, {x, y, x + tile_w, y + kTile}, role_tile, UiProfileDialogAction::item,
                 s->content.items[i].sku ? Utf8View{} : utf8_literal("None"), static_cast<u16>(i)))
            return false;
    }
    if (side) {
        const float px = right - kPreviewPanel;
        const UiRect box{px, body_top + 24, px + kPreviewBox, body_top + 24 + kPreviewBox};
        if (!add(s, root, viewport, box, role_preview) ||
            (s->content.preview_label.bytes &&
             !add(s, root, viewport, {box.left, box.bottom + 8, right, box.bottom + 30}, role_preview_label,
                  UiProfileDialogAction::none, s->content.preview_label, 0, text_style_bold)))
            return false;
        // Four small previews beside the large one.
        if (s->content.kind == UiProfileDialogKind::decoration) {
            const float sizes[] = {40, 32, 40, 32};
            float y = box.top + 8;
            for (u32 i = 0; i < 4; ++i) {
                const float cx = box.right + 12 + 8 + 20;
                if (!add(s, root, viewport, {cx - sizes[i] * .5f, y, cx + sizes[i] * .5f, y + sizes[i]},
                         role_small_preview, UiProfileDialogAction::none, {}, static_cast<u16>(i)))
                    return false;
                y += sizes[i] + 12;
            }
        }
    }
    if (s->content.status.bytes &&
        !add(s, root, viewport, {left, footer_top, right - 200, footer_top + kFooterButton}, role_status,
             UiProfileDialogAction::none, s->content.status, 0, text_style_subtext))
        return false;
    return add_buttons(s, root, viewport, right, footer_top);
}
const UiProfileDialogPart* find(const UiProfileDialogScene& s, Role role, u32 source = 0) noexcept {
    for (u32 i = 0; i < s.part_count; ++i)
        if (s.parts[i].role == role && s.parts[i].source == source)
            return &s.parts[i];
    return nullptr;
}
UiRect offset(ImageCropRect r, UiRect origin) noexcept {
    return {origin.left + r.left, origin.top + r.top, origin.left + r.right, origin.top + r.bottom};
}
// A decoration covers its avatar at 1.2x.
UiRect decoration_rect(UiRect avatar) noexcept {
    const float grow = ui_rect_width(avatar) * .1f;
    return {avatar.left - grow, avatar.top - grow, avatar.right + grow, avatar.bottom + grow};
}
} // namespace

UiRect ui_profile_dialog_crop_editor(UiRect viewport) noexcept {
    const auto panel = panel_rect(UiProfileDialogKind::crop, viewport, crop_height(viewport));
    const float top = panel.top + kTop + kTitleLine + kBodyGap * 2;
    return {panel.left + kSide + 1, top, panel.right - kSide - 1, top + image_crop_editor_height(ui_rect_height(viewport))};
}

bool ui_profile_dialog_update(UiProfileDialogScene* s, const UiProfileDialogContent& content, UiRect viewport) noexcept {
    if (!s || content.kind == UiProfileDialogKind::none || !ui_rect_has_area(viewport))
        return false;
    // The host rebuilds every frame; a press, hover or focus carries over to the
    // rebuilt part with the same role and source, or a click never completes.
    struct {
        u8 role;
        u16 source;
        bool valid;
    } kept[3]{};
    if (s->active) {
        const UiWidgetHandle handles[3]{s->widgets.captured, s->widgets.hovered, s->widgets.focused};
        for (u32 k = 0; k < 3; ++k)
            for (u32 i = 0; i < s->part_count; ++i)
                if (ui_widget_handle_equal(s->parts[i].widget, handles[k]))
                    kept[k] = {s->parts[i].role, s->parts[i].source, true};
    }
    if (!s->active) {
        if (!ui_widget_manager_initialize(&s->widgets, {s->widget_storage, kUiProfileDialogPartCapacity + 4},
                                          {s->node_storage, kUiProfileDialogPartCapacity + 4}))
            return false;
        s->grid_scroll = 0;
    } else
        ui_widget_manager_reset(&s->widgets);
    copy_value(&s->content, content);
    s->viewport = viewport;
    s->part_count = 0;
    auto style = ui_layout_fill_style(ui_layout_node_visible | ui_layout_node_allow_overlap);
    const auto root = ui_widget_manager_add(&s->widgets, ui_widget_invalid_handle(),
                                            {1, UiWidgetKind::container, ui_widget_enabled,
                                             {utf8_literal("ProfileDialog"), style, {}, 0, 1}});
    if (!root.ok())
        return false;
    const bool built = crop(*s) ? build_crop(s, root.handle, viewport) : build_picker(s, root.handle, viewport);
    if (!built || !ui_widget_manager_layout_rectangle(&s->widgets, viewport).ok())
        return false;
    bool (*const restore[3])(UiWidgetManager*, UiWidgetHandle) noexcept{
        ui_widget_manager_capture, ui_widget_manager_hover, ui_widget_manager_focus};
    for (u32 k = 0; k < 3; ++k)
        if (const auto* part = kept[k].valid ? find(*s, static_cast<Role>(kept[k].role), kept[k].source) : nullptr)
            restore[k](&s->widgets, part->widget);
    s->active = true;
    ++s->revision;
    return true;
}

void ui_profile_dialog_close(UiProfileDialogScene* s) noexcept {
    if (!s || !s->active)
        return;
    s->active = false;
    reset_value(&s->content);
    s->part_count = 0;
    ui_widget_manager_focus(&s->widgets, ui_widget_invalid_handle());
    ++s->revision;
}

UiWidgetHandle ui_profile_dialog_hit_test(const UiProfileDialogScene& s, float x, float y) noexcept {
    if (!s.active)
        return ui_widget_invalid_handle();
    const auto target = ui_widget_manager_hit_test(s.widgets, x, y);
    for (u32 i = 0; i < s.part_count; ++i)
        if (ui_widget_handle_equal(s.parts[i].widget, target))
            return enabled(s, s.parts[i]) ? target : ui_widget_invalid_handle();
    return target;
}

UiProfileDialogAction ui_profile_dialog_action(const UiProfileDialogScene& s, UiWidgetHandle widget,
                                               u32* source) noexcept {
    for (u32 i = 0; i < s.part_count; ++i) {
        const auto& part = s.parts[i];
        if (!ui_widget_handle_equal(part.widget, widget))
            continue;
        if (source)
            *source = part.source;
        return enabled(s, part) ? part.action : UiProfileDialogAction::none;
    }
    return UiProfileDialogAction::none;
}

float ui_profile_dialog_zoom_at(const UiProfileDialogScene& s, float x) noexcept {
    const float width = ui_rect_width(s.slider_track);
    if (!(width > 0))
        return 0;
    const float t = (x - s.slider_track.left) / width;
    return t < 0 ? 0 : t > 1 ? 1 : t;
}

bool ui_profile_dialog_scroll(UiProfileDialogScene* s, float delta) noexcept {
    if (!s || !s->active || crop(*s))
        return false;
    const float next =
        maximum(0.f, minimum(s->grid_scroll + delta, maximum(0.f, s->grid_height - ui_rect_height(s->grid_viewport))));
    if (next == s->grid_scroll)
        return true;
    s->grid_scroll = next;
    // A copy: the update replaces s->content from it.
    UiProfileDialogContent content;
    copy_value(&content, s->content);
    return ui_profile_dialog_update(s, content, s->viewport);
}

bool ui_profile_dialog_media(const UiProfileDialogScene& s, u32 index, ProfileMedia* out) noexcept {
    if (!s.active || !out)
        return false;
    reset_value(out);
    const auto& c = s.content;
    if (crop(s)) {
        if (index > 1 || !c.image_ready || !media_asset_key_valid(c.image))
            return false;
        const auto editor = s.crop_viewport;
        const auto image = offset(image_crop_image_rect(c.crop), editor);
        const auto mask = offset(image_crop_mask_rect(c.crop), editor);
        ProfileMedia& media = *out;
        media.key = c.image;
        media.domain = 1;
        // The image rectangle is already in the turned orientation; the
        // quad's orientation flags map it onto the unturned texture.
        media.rotation = c.crop.rotation;
        media.rectangle = image;
        if (index == 0) {
            // Outside the mask the image is dimmed with a 60% overlay.
            media.clip = editor;
            media.opacity = .4f;
        } else if (c.crop.use == ImageCropUse::avatar) {
            media.clip = editor;
            media.mask = mask;
            media.mask_radius = ui_rect_width(mask) * .5f;
        } else
            media.clip = ui_rect_intersection(editor, mask);
        return true;
    }
    // Pickers: tile images, then the large and small previews.
    u32 cursor = 0;
    for (u32 i = 0; i < s.part_count; ++i) {
        const auto& part = s.parts[i];
        const bool tile = part.role == role_tile && part.source < c.items.count && c.items[part.source].sku;
        const bool preview = part.role == role_preview || part.role == role_small_preview;
        if (!tile && !preview)
            continue;
        const auto* n = node(s, part);
        if (!n)
            continue;
        const auto r = n->rectangle;
        const auto clip = ui_rect_intersection(n->clip_rectangle, s.rectangle);
        const bool decoration = c.kind == UiProfileDialogKind::decoration;
        if (tile) {
            if (cursor++ != index)
                continue;
            const auto& item = c.items[part.source];
            ProfileMedia& media = *out;
            media.key = item.key;
            copy_value(&media.url, item.url);
            media.clip = clip;
            if (decoration) {
                const float cx = (r.left + r.right) * .5f, cy = (r.top + r.bottom) * .5f;
                media.rectangle = {cx - 32, cy - 32, cx + 32, cy + 32};
            } else {
                media.rectangle = r;
                media.mask = r;
                media.mask_radius = 4;
            }
            return true;
        }
        // Previews: the avatar, then the selected decoration over it.
        const float scale = part.role == role_preview ? .6f : 1.f / 1.2f;
        const float side = ui_rect_width(r) * scale;
        const float cx = (r.left + r.right) * .5f, cy = part.role == role_preview ? r.top + ui_rect_height(r) * .45f
                                                                                    : (r.top + r.bottom) * .5f;
        const UiRect avatar{cx - side * .5f, cy - side * .5f, cx + side * .5f, cy + side * .5f};
        const bool has_decoration = decoration && c.selected < c.items.count && c.items[c.selected].sku;
        if (cursor++ == index) {
            if (!media_asset_key_valid(c.avatar.key))
                return false;
            ProfileMedia& media = *out;
            copy_value(&media, c.avatar);
            media.rectangle = avatar;
            media.clip = clip;
            media.circular = true;
            return true;
        }
        if (has_decoration && cursor++ == index) {
            const auto& item = c.items[c.selected];
            ProfileMedia& media = *out;
            copy_value(&media.key, item.key);
            media.url = item.url;
            media.rectangle = decoration_rect(avatar);
            media.clip = clip;
            return true;
        }
    }
    return false;
}

bool ui_profile_dialog_paint(const UiProfileDialogScene& s, const Theme& theme, PaintStream* paint) noexcept {
    if (!s.active)
        return true;
    const UiSettingsThemeTokens t(theme);
    bool ok = paint_stream_push(paint, s.viewport,
                                theme.tokens.overlay.scrim) &&
              ui_surface_paint(paint, s.rectangle, theme, UiElevation::dialog, theme.scale.radius_md);
    for (u32 i = 0; ok && i < s.part_count; ++i) {
        const auto& part = s.parts[i];
        const auto r = visible(s, part);
        if (!ui_rect_has_area(r))
            continue;
        UiControlState state{enabled(s, part), hovered(s, part), false, focused(s, part) && s.focus_visible};
        switch (part.role) {
        case role_editor:
            ok = paint_stream_push_rounded(paint, r, theme.tokens.overlay.media,
                                           theme.scale.radius_sm);
            if (ok && !s.content.image_ready)
                ok = ui_icon_paint(paint, s.content.failed ? UiIcon::warning : UiIcon::image,
                                   {(r.left + r.right) * .5f - 16, (r.top + r.bottom) * .5f - 16,
                                    (r.left + r.right) * .5f + 16, (r.top + r.bottom) * .5f + 16},
                                   s.content.failed ? theme.tokens.text.danger : theme.text_secondary);
            break;
        case role_small_icon:
        case role_large_icon:
            ok = ui_icon_paint(paint, UiIcon::image, r, theme.text_secondary);
            break;
        case role_slider: {
            const auto track = s.slider_track;
            const float value = (s.content.crop.zoom - kImageCropMinimumZoom) / (kImageCropMaximumZoom - kImageCropMinimumZoom);
            const float x = track.left + ui_rect_width(track) * (value < 0 ? 0 : value > 1 ? 1 : value);
            const float cy = (track.top + track.bottom) * .5f;
            ok = paint_stream_push_rounded(paint, track, t.color.slider_track,
                                           kSliderTrack * .5f) &&
                 paint_stream_push_rounded(paint, track.left, track.top, x, track.bottom, t.color.brand,
                                           kSliderTrack * .5f) &&
                 paint_stream_push_rounded_border(paint, x - kSliderThumb * .5f, cy - kSliderThumb * .5f,
                                                  x + kSliderThumb * .5f, cy + kSliderThumb * .5f, t.color.thumb,
                                                  state.focused ? t.color.focus_ring : t.color.border_subtle,
                                                  kSliderThumb * .5f, state.focused ? 2.f : 1.f);
            break;
        }
        case role_rotate:
        case role_close: {
            const Color fill = state.hovered && state.enabled ? theme.tokens.state.hover : Color{};
            if (fill.alpha)
                ok = paint_stream_push_rounded(paint, r, fill, theme.scale.radius_sm);
            const float pad = 6;
            ok = ok && ui_icon_paint(paint, part.role == role_rotate ? UiIcon::retry : UiIcon::close,
                                     {r.left + pad, r.top + pad, r.right - pad, r.bottom - pad},
                                     ui_settings_color_alpha(state.hovered ? t.color.text_strong : t.color.text_subtle,
                                                             state.enabled ? 1.f : .5f));
            break;
        }
        case role_apply:
        case role_cancel:
            ok = ui_settings_button_paint(paint, t, r,
                                          part.role == role_apply ? UiSettingsButtonVariant::primary
                                                                  : UiSettingsButtonVariant::secondary,
                                          state);
            break;
        case role_tile: {
            ok = paint_stream_push_rounded(paint, r,
                                           state.hovered ? theme.tokens.state.hover : theme.base_lowest,
                                           theme.scale.radius_sm);
            if (ok && part.source < s.content.items.count && !s.content.items[part.source].sku) {
                const float cx = (r.left + r.right) * .5f, cy = (r.top + r.bottom) * .5f - 8;
                ok = ui_icon_paint(paint, UiIcon::close, {cx - 12, cy - 12, cx + 12, cy + 12}, theme.text_secondary);
            }
            break;
        }
        case role_preview:
            ok = paint_stream_push_rounded(paint, r, theme.base_lowest, theme.scale.radius_sm);
            break;
        default:
            break;
        }
    }
    return ok;
}

bool ui_profile_dialog_foreground(const UiProfileDialogScene& s, const Theme& theme, PaintStream* paint) noexcept {
    if (!s.active)
        return true;
    bool ok = true;
    if (crop(s) && s.content.image_ready) {
        // The mask: a 5 DIP white border inside the crop shape.
        const auto mask = offset(image_crop_mask_rect(s.content.crop), s.crop_viewport);
        const bool round = s.content.crop.use == ImageCropUse::avatar;
        ok = paint_stream_push_rounded_border(paint, mask, {},
                                              theme.tokens.text.on_accent, round ? ui_rect_width(mask) * .5f : 0,
                                              kMaskBorder);
        if (ok)
            if (const auto* editor = find(s, role_editor); editor && focused(s, *editor) && s.focus_visible) {
                const auto r = visible(s, *editor);
                ok = paint_stream_push_rounded_border(paint, r.left - 2, r.top - 2, r.right + 2, r.bottom + 2, {},
                                                      ui_settings_focus_color(theme), theme.scale.radius_sm + 2, 2);
            }
        return ok;
    }
    for (u32 i = 0; ok && i < s.part_count; ++i) {
        const auto& part = s.parts[i];
        if (part.role != role_tile)
            continue;
        const auto r = visible(s, part);
        const bool selected = part.source == s.content.selected;
        const bool ring = selected || (focused(s, part) && s.focus_visible);
        if (ring && ui_rect_has_area(r)) {
            const auto* n = node(s, part);
            const auto full = n ? n->rectangle : r;
            ok = paint_stream_push_clip(paint, r) &&
                 paint_stream_push_rounded_border(paint, full, {},
                                                  selected ? theme.tokens.emphasis.fill : ui_settings_focus_color(theme),
                                                  theme.scale.radius_sm, 2) &&
                 paint_stream_pop_clip(paint);
        }
    }
    return ok;
}

bool ui_profile_dialog_text_output(const UiProfileDialogScene& s, const Theme& theme, UiTextOutput* output) noexcept {
    if (!s.active)
        return true;
    const UiSettingsThemeTokens t(theme);
    for (u32 i = 0; i < s.part_count; ++i) {
        const auto& part = s.parts[i];
        if (!part.text.bytes)
            continue;
        const auto* n = node(s, part);
        if (!n)
            return false;
        auto r = n->rectangle;
        const auto clip = ui_rect_intersection(n->clip_rectangle, s.rectangle);
        if (!ui_rect_intersects(r, clip))
            continue;
        UiControlState state{enabled(s, part), hovered(s, part), false, false};
        Color color = theme.text_primary;
        auto alignment = UiTextAlignment::start;
        switch (part.role) {
        case role_apply:
            color = ui_settings_button_text(t, UiSettingsButtonVariant::primary, state);
            alignment = UiTextAlignment::center;
            break;
        case role_cancel:
            color = ui_settings_button_text(t, UiSettingsButtonVariant::secondary, state);
            alignment = UiTextAlignment::center;
            break;
        case role_reset:
            // A text button, disabled until the image is edited.
            color = ui_settings_color_alpha(state.hovered ? t.color.text_strong : t.color.text_default,
                                            state.enabled ? 1.f : .5f);
            break;
        case role_tile:
            color = theme.text_secondary;
            alignment = UiTextAlignment::center;
            r.top = r.bottom - 28;
            break;
        case role_status:
            color = s.content.failed ? theme.tokens.text.danger : theme.text_secondary;
            break;
        case role_section:
        case role_preview_label:
            color = t.color.text_strong;
            break;
        default:
            break;
        }
        if (!ui_text_output_push(output, {part.text,
                                          {&s.widgets, part.widget},
                                          r,
                                          clip,
                                          color,
                                          part.style,
                                          alignment,
                                          UiTextAlignment::center,
                                          TextWrapping::no_wrap,
                                          TextOverflow::ellipsis}))
            return false;
    }
    return true;
}

} // namespace maicord
