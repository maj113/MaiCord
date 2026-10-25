#include "maicord/ui/media_picker.hpp"
#include "maicord/ui/media_picker_presentation.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/scrollbar.hpp"
#include "maicord/ui/regional_flag.hpp"
#include "maicord/ui/emoji_catalog.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/app/client_theme.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
struct PickerFixture {
    u32 calls, count;
    u64 epoch;
    UiMediaPickerItemKind kind;
    bool bad;
    u32 bad_index = ~u32{0};
    u32 disabled_first = ~u32{0}, disabled_count = 0;
    u32 hidden_first = ~u32{0}, hidden_count = 0;
    bool hidden_requested = false;
    bool missing_asset = false;
    bool favoritable = false, favorite = false, favorite_pending = false, favorite_failed = false;
    Utf8View glyph{};
    u32 regional_flag = 0;
};
bool resolve_picker(void* opaque, u32 index, UiMediaPickerItem* item) noexcept {
    auto& fixture = *static_cast<PickerFixture*>(opaque);
    ++fixture.calls;
    if (!item || index >= fixture.count)
        return false;
    *item = {fixture.epoch + index + 1,
             fixture.epoch + index + 1001,
             utf8_literal("Original native media"),
             utf8_literal("Synthetic catalog fixture"),
             0,
             320,
             180,
             fixture.kind,
             true};
    if (fixture.bad || index == fixture.bad_index)
        item->id = 0;
    item->enabled =
        !(index >= fixture.disabled_first && index - fixture.disabled_first < fixture.disabled_count);
    fixture.hidden_requested =
        fixture.hidden_requested ||
        (index >= fixture.hidden_first && index - fixture.hidden_first < fixture.hidden_count);
    if (fixture.missing_asset)
        item->asset_id = 0;
    item->asset_flags = 0x0201;
    item->favoritable = fixture.favoritable;
    item->favorite = fixture.favorite;
    item->favorite_pending = fixture.favorite_pending;
    item->favorite_failed = fixture.favorite_failed;
    item->glyph = fixture.glyph;
    item->regional_flag = fixture.regional_flag;
    return true;
}
void test_picker_scrollbars(void (*check)(bool, const char*) noexcept) noexcept {
    const UiRect viewport{0, 0, 400, 200};
    const auto top = ui_scrollbar_geometry(viewport, 0, 800);
    const auto bottom = ui_scrollbar_geometry(viewport, 600, 800);
    check(!ui_scrollbar_geometry(viewport, 0, 200).visible &&
              !ui_scrollbar_geometry(viewport, 0, 0).visible && top.visible && top.maximum_scroll == 600 &&
              ui_rect_height(top.thumb) == 48 && bottom.thumb.bottom == 196 &&
              ui_scrollbar_scroll_from_pointer(top, top.thumb.top + 5, 5) == 0 &&
              ui_scrollbar_scroll_from_pointer(bottom, bottom.thumb.top + 5, 5) == 600,
          "media picker scrollbar: overflow-only proportional thumb preserves exact drag endpoints");
    const auto tiny = ui_scrollbar_geometry({0, 0, 3, 3}, 0, 100);
    check(tiny.visible && tiny.thumb.left >= 0 && tiny.thumb.right <= 3 && tiny.thumb.top >= 0 &&
              tiny.thumb.bottom <= 3,
          "media picker scrollbar: tiny valid viewport keeps thumb and gutter inside owner bounds");
    float nan = 0;
    const u32 nan_bits = 0x7fc00000u;
    copy_bytes(&nan, &nan_bits, sizeof(nan_bits));
    check(!ui_scrollbar_geometry(viewport, 0, nan).visible &&
              !ui_scrollbar_geometry(viewport, nan, 800).visible &&
              ui_scrollbar_scroll_from_pointer(top, nan, 0) == 0,
          "media picker scrollbar: nonfinite offsets and extents fail closed under fast math");
    PaintStream paint{};
    bool colors = paint_stream_initialize(&paint) && paint_stream_reset(&paint, 400, 200);
    const UiScrollbarVisualState states[]{UiScrollbarVisualState::normal, UiScrollbarVisualState::hovered,
                                          UiScrollbarVisualState::pressed};
    const Color expected[]{kDefaultTheme.interactive, kDefaultTheme.interactive_hover,
                           kDefaultTheme.text_secondary};
    for (u32 i = 0; colors && i < 3; ++i) {
        const u32 before = paint.count;
        colors = ui_scrollbar_paint(&paint, top, kDefaultTheme, states[i]) && paint.count == before + 1 &&
                 bytes_equal(&paint.quads[before].fill, &expected[i], sizeof(Color)) &&
                 paint.quads[before].top == top.thumb.top && paint.quads[before].bottom == top.thumb.bottom;
    }
    check(colors, "media picker scrollbar: reusable idle hover pressed colors preserve geometry");
    paint_stream_release(&paint);
    UiMediaPickerScene scene{};
    PickerFixture fixture{0, 120, 100, UiMediaPickerItemKind::gif, false};
    UiMediaPickerSource source{&fixture, 3, 7, fixture.count, 1, {}, resolve_picker};
    UiMediaPickerContent content{};
    content.account_scope = 3;
    content.query_generation = 7;
    content.revision = 1;
    content.kind = UiMediaPickerKind::gif;
    content.phase = UiMediaPickerPhase::ready;
    content.available_tabs = 7;
    content.edit.active = true;
    const UiRect pane{0, 0, 1000, 800}, anchor{560, 700, 940, 740};
    check(ui_media_picker_scene_update(&scene, &source, &content, pane, anchor) &&
              ui_media_picker_scene_scrollbar(scene).visible &&
              ui_media_picker_scene_set_scrollbar_state(&scene, false, UiScrollbarVisualState::pressed),
          "media picker scrollbar: scene owns current overflow geometry and typed press feedback");
    const u32 generation = scene.generation, calls = fixture.calls;
    check(ui_media_picker_scene_set_scrollbar_state(&scene, false, UiScrollbarVisualState::hovered) &&
              scene.generation == generation && fixture.calls == calls,
          "media picker scrollbar: visual feedback does not reproject widgets or catalog");
    content.phase = UiMediaPickerPhase::failed;
    ++content.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, pane, anchor) &&
              scene.content_height == 0 && !ui_media_picker_scene_scrollbar(scene).visible &&
              scene.cell_count == 0 && scene.body_scrollbar_state == UiScrollbarVisualState::normal &&
              !ui_media_picker_scene_scroll(&scene, 100),
          "media picker scrollbar: unavailable catalog with retained source has no fake extent or "
          "draggable thumb");
    content.phase = UiMediaPickerPhase::empty;
    ++content.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, pane, anchor) &&
              !ui_media_picker_scene_scrollbar(scene).visible,
          "media picker scrollbar: empty phase suppresses retained source overflow");
    content.phase = UiMediaPickerPhase::ready;
    ++content.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, pane, anchor),
          "media picker finite geometry: valid borrowed scene initializes before rejection");
    const auto original_panel = scene.panel;
    const auto original_viewport = scene.viewport;
    const auto original_bank = scene.storage.arena.base;
    const u32 original_generation = scene.generation, original_calls = fixture.calls;
    UiRect invalid_viewport = pane;
    copy_bytes(&invalid_viewport.bottom, &nan_bits, sizeof(nan_bits));
    check(!ui_media_picker_scene_update(&scene, &source, &content, invalid_viewport, anchor) &&
              ui_media_picker_scene_current(scene) && ui_rect_equal(scene.panel, original_panel) &&
              ui_rect_equal(scene.viewport, original_viewport) &&
              scene.storage.arena.base == original_bank && scene.generation == original_generation &&
              fixture.calls == original_calls,
          "media picker finite geometry: NaN viewport rejected before mutation allocation source calls or "
          "casts");
    UiRect invalid_anchor = anchor;
    const u32 infinity_bits = 0x7f800000u;
    copy_bytes(&invalid_anchor.left, &infinity_bits, sizeof(infinity_bits));
    check(!ui_media_picker_scene_update(&scene, &source, &content, pane, invalid_anchor) &&
              ui_media_picker_scene_current(scene) && scene.generation == original_generation &&
              ui_rect_equal(scene.panel, original_panel) && fixture.calls == original_calls &&
              ui_media_picker_scene_update(&scene, &source, &content, pane, anchor),
          "media picker finite geometry: infinite anchor leaves prior valid scope intact and accepts later "
          "valid update");
    ui_media_picker_scene_release(&scene);
}
void test_picker_emoji(void (*check)(bool, const char*) noexcept) noexcept {
    UiMediaPickerScene scene{};
    PickerFixture fixture{0, 1000, 100, UiMediaPickerItemKind::emoji, false};
    fixture.missing_asset = true;
    fixture.glyph = utf8_literal("😀");
    UiMediaPickerSource source{&fixture, 3, 7, fixture.count, 1, {}, resolve_picker};
    UiMediaPickerContent content{};
    content.account_scope = 3;
    content.query_generation = 7;
    content.revision = 1;
    content.kind = UiMediaPickerKind::emoji;
    content.phase = UiMediaPickerPhase::ready;
    content.available_tabs = 7;
    content.edit.active = true;
    const UiRect pane{0, 0, 1000, 800}, anchor{560, 700, 940, 740};
    check(
        ui_media_picker_scene_update(&scene, &source, &content, pane, anchor) && scene.columns == 10 &&
            scene.body.bottom == scene.footer.top && scene.cell_count <= kUiMediaPickerVisibleCells &&
            scene.storage.arena.committed <= 128 * 1024 && ui_media_picker_scene_select(&scene, 0) &&
            ui_media_picker_scene_move_selection(&scene, 0, 1) && scene.selected_index == 10,
        "media picker emoji: full shared body and ten-column logical navigation use bounded native grid");
    UiMediaPickerMediaSlot media{};
    check(scene.cell_count && !ui_media_picker_scene_media(scene, 0, &media),
          "media picker emoji: Unicode glyph has no fake custom-media request");
    UiTextCommand commands[160]{};
    UiTextCommandStream stream{};
    UiTextOutput output{&stream, nullptr};
    bool emitted = ui_text_command_stream_initialize(&stream, {commands, 160}) &&
                   ui_media_picker_scene_text_output(scene, kDefaultTheme, &output, nullptr);
    u32 glyphs = 0;
    for (u32 i = 0; emitted && i < stream.count; ++i)
        if (utf8_equal(commands[i].text, fixture.glyph) && commands[i].owner.handle.slot != scene.root.slot) {
            ++glyphs;
            emitted = commands[i].style_flags == text_style_picker_grid_emoji &&
                      commands[i].horizontal_alignment == UiTextAlignment::center &&
                      commands[i].clip.top >= scene.body.top &&
                      commands[i].clip.bottom <= scene.body.bottom;
        }
    check(emitted && glyphs == scene.cell_count,
          "media picker emoji: Unicode uses native color-glyph style with exact clipped cell ownership");
    fixture.glyph = utf8_literal("🇺🇸");
    fixture.regional_flag = regional_flag_sequence(fixture.glyph);
    ++source.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, pane, anchor) &&
              ui_media_picker_scene_media(scene, 0, &media) && media.kind == UiMediaPickerItemKind::emoji &&
              media.asset_id == 0 && media.regional_flag == fixture.regional_flag &&
              media.fit == UiMediaPickerFit::contain,
          "media picker emoji: admitted regional flag is a typed contained media slot not a custom "
          "snowflake");
    fixture.glyph = utf8_literal("😀");
    ++source.revision;
    check(!ui_media_picker_scene_update(&scene, &source, &content, pane, anchor) && !scene.active,
          "media picker emoji: mismatched Unicode and regional-flag identity retires projection");
    fixture.regional_flag = 0;
    fixture.glyph = utf8_literal("😀");
    ++source.revision;
    auto incompatible = ui_media_picker_style();
    incompatible.metrics.panel_height = 200;
    incompatible.metrics.content_inset = incompatible.metrics.header_gap = 24;
    incompatible.metrics.tab_height = 48;
    incompatible.metrics.search_height = 64;
    incompatible.metrics.footer_height = 80;
    const auto defaults = ui_media_picker_style();
    check(
        ui_media_picker_scene_update(&scene, &source, &content, pane, anchor, &incompatible) &&
            bytes_equal(&scene.style, &defaults, sizeof(defaults)) &&
            scene.body.bottom == scene.footer.top && scene.cell_count <= kUiMediaPickerVisibleCells,
        "media picker emoji: impossible override falls back as whole style without shortening common body");
    fixture.count = 272;
    source.count = 272;
    UiMediaPickerSection sections[272]{};
    for (u32 i = 0; i < 272; ++i)
        sections[i] = {i + 1ull, i, 1, utf8_literal("Original section"), true};
    source.sections = {sections, 272};
    ++source.revision;
    check(
        ui_media_picker_scene_update(&scene, &source, &content, pane, anchor) &&
            ui_media_picker_scene_scroll_section(&scene, 272) && scene.header_count &&
            scene.projection->headers[0].section_index > 256 &&
            scene.cell_count <= kUiMediaPickerVisibleCells &&
            scene.header_count <= kUiMediaPickerVisibleSections &&
            scene.rail_count <= kUiMediaPickerVisibleSections &&
            scene.widgets.count <= kUiMediaPickerWidgetCapacity,
        "media picker emoji: beyond-256 section indices stay typed and visible projection remains bounded");
    check(ui_media_picker_scene_scroll(&scene, -scene.scroll_dips) &&
              ui_media_picker_scene_scroll(&scene, 9) &&
              scene.projection->headers[0].rectangle.top == scene.body.top &&
              scene.pinned_bottom == scene.body.top + scene.style.metrics.section_height &&
              ui_media_picker_scene_scrollbar(scene, true).visible,
          "media picker emoji: fractional section scroll retains full heading coordinates and a true "
          "overflow rail");
    bool clipped = ui_text_command_stream_initialize(&stream, {commands, 160}) &&
                   ui_media_picker_scene_text_output(scene, kDefaultTheme, &output, nullptr);
    for (u32 i = 0; clipped && i < stream.count; ++i) {
        const auto h = commands[i].owner.handle;
        if (h.generation && h.slot < kUiMediaPickerWidgetCapacity &&
            commands[i].text.data == sections[0].label.data)
            clipped =
                commands[i].clip.top >= scene.body.top && commands[i].clip.bottom <= scene.body.bottom;
    }
    check(clipped, "media picker emoji: partial section headings clip without moving or reshaping their "
                   "logical rectangle");
    ui_media_picker_scene_release(&scene);
}
void test_picker_rail_and_scroll_reuse(void (*check)(bool, const char*) noexcept) noexcept {
    UiMediaPickerScene scene{};
    PickerFixture fixture{0, 200, 10'000, UiMediaPickerItemKind::emoji, false};
    fixture.missing_asset = true;
    fixture.glyph = utf8_literal("😀");
    UiMediaPickerSource source{&fixture, 3, 7, fixture.count, 1, {}, resolve_picker};
    UiMediaPickerContent content{};
    content.account_scope = 3;
    content.query_generation = 7;
    content.revision = 1;
    content.kind = UiMediaPickerKind::emoji;
    content.phase = UiMediaPickerPhase::ready;
    content.available_tabs = 7;
    content.edit.active = true;
    const UiRect pane{0, 0, 1000, 800}, anchor{560, 700, 940, 740};
    bool valid = ui_media_picker_scene_update(&scene, &source, &content, pane, anchor);
    const auto bank = scene.storage.arena.base;
    const auto committed = scene.storage.arena.committed;
    const UiRect original_panel = scene.panel;
    const u32 original_generation = scene.generation, original_calls = fixture.calls;
    source.count = kUiMediaPickerMaximumItems + 1;
    check(!ui_media_picker_scene_update(&scene, &source, &content, pane, anchor) &&
              scene.generation == original_generation && fixture.calls == original_calls &&
              scene.storage.arena.base == bank && ui_rect_equal(scene.panel, original_panel),
          "media picker warm admission: unchanged revision cannot bypass basic source item bounds");
    source.count = fixture.count;
    check(ui_media_picker_scene_update(&scene, &source, &content, pane, anchor) &&
              scene.generation == original_generation && fixture.calls == original_calls,
          "media picker warm admission: restored unchanged publication retains safely borrowed scene");
    for (u32 step = 0; valid && step < 96; ++step) {
        const u32 calls = fixture.calls;
        valid = ui_media_picker_scene_scroll(&scene, 1) && fixture.calls - calls <= scene.columns &&
                scene.storage.arena.base == bank && scene.storage.arena.committed == committed;
        for (u32 i = 0; valid && i < scene.cell_count; ++i) {
            const auto& cell = scene.projection->cells[i];
            valid = cell.clip.top >= scene.body.top && cell.clip.bottom <= scene.body.bottom &&
                    cell.item.id == fixture.epoch + cell.source_index + 1;
        }
    }
    check(valid, "media picker emoji: rapid fractional scroll resolves only entering row, preserves "
                 "clipped identities and allocates no new scene storage");
    const u32 prior_calls = fixture.calls;
    fixture.epoch += 1000;
    ++source.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, pane, anchor) &&
              fixture.calls - prior_calls == scene.cell_count &&
              scene.projection->cells[0].item.id ==
                  fixture.epoch + scene.projection->cells[0].source_index + 1,
          "media picker emoji: publication revision expires every borrowed cached descriptor");
    UiMediaPickerSection sections[10]{};
    for (u32 i = 0; i < 10; ++i) {
        sections[i] = {100ull + i, i * 20, 20, utf8_literal("Typed category"), true};
        sections[i].visual = static_cast<UiMediaPickerSectionVisual>(i + 1);
    }
    sections[0].visual = UiMediaPickerSectionVisual::guild;
    sections[0].guild_id = 77;
    sections[0].icon_variant = 42;
    sections[0].icon_animated = true;
    source.sections = {sections, 10};
    ++source.revision;
    UiMediaPickerRailMediaSlot media{};
    check(ui_media_picker_scene_update(&scene, &source, &content, pane, anchor) &&
              ui_media_picker_scene_rail_media(scene, 0, &media) && media.guild_id == 77 &&
              media.variant == 42 && media.animated && media.target.item_id == 100 &&
              media.target.action == UiMediaPickerAction::section_jump &&
              ui_rect_width(media.rectangle) == ui_rect_height(media.rectangle) &&
              media.rectangle.left >= scene.rail.left && media.rectangle.right <= scene.rail.right &&
              media.clip.top >= scene.rail.top && media.clip.bottom <= scene.rail.bottom &&
              !ui_media_picker_scene_rail_media(scene, 1, &media),
          "media picker rail: guild artwork identity is typed, clipped and fenced separately from emoji");
    bool square_rail = true;
    for (u32 i = 0; i < scene.rail_count; ++i) {
        const auto& artwork = scene.projection->rail[i].rectangle;
        square_rail = square_rail && ui_rect_width(artwork) == ui_rect_height(artwork) &&
                      ui_rect_width(artwork) >= scene.style.metrics.icon_extent &&
                      artwork.left >= scene.rail.left && artwork.right <= scene.rail.right;
    }
    PaintStream rail_paint{};
    bool no_rail_thumb = paint_stream_initialize(&rail_paint) &&
                         paint_stream_reset(&rail_paint, pane.right, pane.bottom) &&
                         ui_media_picker_scene_paint(scene, kDefaultTheme, &rail_paint, nullptr, 96);
    const auto rail_bar = ui_media_picker_scene_scrollbar(scene, true);
    for (u32 i = 0; no_rail_thumb && i < rail_paint.count; ++i) {
        const auto& q = rail_paint.quads[i];
        no_rail_thumb = !ui_rect_equal({q.left, q.top, q.right, q.bottom}, rail_bar.thumb);
    }
    check(square_rail && rail_bar.visible && no_rail_thumb &&
              ui_media_picker_scene_scroll(&scene, 16, true),
          "media picker rail: square centered artwork keeps its complete width, scrolls without a painted "
          "thumb");
    paint_stream_release(&rail_paint);
    ++source.revision;
    check(!ui_media_picker_scene_rail_media(scene, 0, &media),
          "media picker rail: stale publication cannot expose prior guild artwork");
    sections[0].visual = UiMediaPickerSectionVisual::nature;
    check(!ui_media_picker_scene_update(&scene, &source, &content, pane, anchor),
          "media picker rail: category cannot borrow a guild artwork identity");
    ui_media_picker_scene_release(&scene);
    // The official category list as emoji: every category glyph is a distinct
    // catalog emoji; generic (search) and guild sections have none.
    bool emoji_glyphs = !ui_media_picker_section_glyph(UiMediaPickerSectionVisual::generic).bytes &&
                        !ui_media_picker_section_glyph(UiMediaPickerSectionVisual::guild).bytes;
    for (u32 i = 1; emoji_glyphs && i < static_cast<u32>(UiMediaPickerSectionVisual::guild); ++i) {
        const auto glyph = ui_media_picker_section_glyph(static_cast<UiMediaPickerSectionVisual>(i));
        emoji_glyphs = glyph.bytes && emoji_catalog_find_glyph(glyph) <= 0xFFFEu;
        for (u32 j = 1; emoji_glyphs && j < i; ++j)
            emoji_glyphs = !utf8_equal(glyph, ui_media_picker_section_glyph(static_cast<UiMediaPickerSectionVisual>(j)));
    }
    check(emoji_glyphs, "media picker rail: every category is a distinct emoji from the emoji catalog");
}
void test_picker_favorites(void (*check)(bool, const char*) noexcept) noexcept {
    const Theme amoled_theme = ui_theme_amoled();
    UiMediaPickerScene scene{};
    PickerFixture fixture{0, 65'536, 10'000, UiMediaPickerItemKind::gif, false};
    fixture.favoritable = true;
    UiMediaPickerSource source{&fixture, 3, 7, fixture.count, 1, {}, resolve_picker};
    UiMediaPickerContent content{};
    content.account_scope = 3;
    content.query_generation = 7;
    content.revision = 1;
    content.kind = UiMediaPickerKind::gif;
    content.phase = UiMediaPickerPhase::ready;
    content.available_tabs = 7;
    content.edit.active = true;
    const UiRect viewport{0, 0, 1000, 800}, anchor{560, 700, 940, 740};
    UiRect button{};
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) &&
              !ui_media_picker_scene_favorite(scene, 0, &button) &&
              ui_media_picker_scene_selected_favorite_target(scene).action == UiMediaPickerAction::none,
          "media picker favorites: toolbar is hidden until a real GIF cell is hovered or focused");
    if (!scene.active)
        return;
    const auto first = scene.projection->cells[0];
    const float send_x = first.rectangle.left + 12, send_y = first.rectangle.top + 12;
    const auto bank = scene.storage.arena.base;
    const u32 calls = fixture.calls, widgets = scene.widgets.count;
    const u32 chrome_widgets = widgets - scene.cell_count;
    check(ui_media_picker_scene_hover(&scene, send_x, send_y, 100) &&
              ui_media_picker_scene_favorite(scene, 0, &button) && fixture.calls == calls &&
              scene.widgets.count == widgets && button.left >= first.clip.left &&
              button.top >= first.clip.top && button.right <= first.clip.right &&
              button.bottom <= first.clip.bottom && ui_rect_width(button) < ui_rect_width(first.rectangle),
          "media picker favorites: one inset clipped subregion reuses the visible cell widget");
    const float x = (button.left + button.right) * .5f, y = (button.top + button.bottom) * .5f;
    auto favorite = ui_media_picker_scene_hit_test(scene, x, y);
    auto send = ui_media_picker_scene_hit_test(scene, send_x, send_y);
    check(favorite.action == UiMediaPickerAction::favorite && favorite.item_id == first.item.id &&
              favorite.account_scope == 3 && favorite.query_generation == 7 &&
              favorite.source_revision == source.revision &&
              favorite.scene_generation == scene.generation && send.action == UiMediaPickerAction::item &&
              send.item_id == favorite.item_id && !ui_media_picker_target_equal(send, favorite) &&
              ui_media_picker_scene_hit_test(scene, button.left, button.top).action ==
                  UiMediaPickerAction::favorite &&
              ui_media_picker_scene_hit_test(scene, button.right, y).action == UiMediaPickerAction::item &&
              ui_media_picker_scene_hit_test(scene, x, button.bottom).action == UiMediaPickerAction::item,
          "media picker favorites: half-open exact button bounds cannot share the Send action");
    check(
        ui_media_picker_scene_pointer_down(&scene, x, y) &&
            ui_media_picker_target_equal(ui_media_picker_scene_pointer_up(&scene, x, y), favorite) &&
            ui_media_picker_scene_pointer_down(&scene, x, y) &&
            ui_media_picker_scene_pointer_up(&scene, send_x, send_y).action == UiMediaPickerAction::none &&
            ui_media_picker_scene_pointer_down(&scene, send_x, send_y) &&
            ui_media_picker_scene_pointer_up(&scene, x, y).action == UiMediaPickerAction::none,
        "media picker favorites: capture requires the same subregion action on release in both directions");
    check(ui_media_picker_scene_hover(&scene, x, y, 200),
          "media picker favorites: moving into the star hovers its own action");

    PaintStream paint{};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1000, 800) &&
              ui_media_picker_scene_paint(scene, amoled_theme, &paint, nullptr, 96),
          "media picker favorites: themed scene background paints before actual native media");
    const u32 background_quads = paint.count;
    // Stand-in native media is inserted between the background and foreground.
    check(paint_stream_push(&paint, first.clip.left, first.clip.top, first.clip.right, first.clip.bottom,
                            amoled_theme.surface) &&
              ui_media_picker_scene_paint_foreground(scene, amoled_theme, &paint),
          "media picker favorites: toolbar and original curved star paint above native media");
    bool toolbar_after_media = false, star_after_media = false;
    for (u32 i = background_quads + 1; i < paint.count; ++i) {
        const auto& q = paint.quads[i];
        toolbar_after_media = toolbar_after_media ||
                              (q.left == button.left && q.top == button.top && q.right == button.right &&
                               q.bottom == button.bottom &&
                               q.radius == scene.style.control_radius - scene.style.metrics.emoji_padding &&
                               bytes_equal(&q.fill, &amoled_theme.base_lower, sizeof(Color)));
        star_after_media = star_after_media ||
                           (q.fill.alpha == amoled_theme.text_primary.alpha &&
                            q.fill.red == amoled_theme.text_primary.red && q.left >= button.left &&
                            q.right <= button.right && q.top >= button.top && q.bottom <= button.bottom);
    }
    check(toolbar_after_media && star_after_media && paint.clip_depth == 1 && !paint.rejected,
          "media picker favorites: theme-owned button radius and foreground star stay inside actual hit "
          "bounds");

    check(ui_media_picker_scene_select(&scene, 0) &&
              ui_media_picker_scene_selected_favorite_target(scene).action == UiMediaPickerAction::favorite,
          "media picker favorites: focused selected GIF exposes separate keyboard Favorite and Enter "
          "targets");
    content.edit.focused = true;
    ++content.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) &&
              ui_media_picker_scene_selected_favorite_target(scene).action == UiMediaPickerAction::none &&
              !ui_media_picker_scene_favorite(scene, 0, &button),
          "media picker favorites: retained selection cannot steal a shortcut or show toolbar from Search "
          "focus");
    content.edit.focused = false;
    ++content.revision;
    fixture.favorite_pending = true;
    ++source.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) &&
              ui_media_picker_scene_favorite(scene, 0, &button) &&
              !scene.projection->cells[0].item.favorite &&
              ui_media_picker_scene_hit_test(scene, x, y).action == UiMediaPickerAction::none &&
              !ui_media_picker_scene_pointer_down(&scene, x, y) &&
              ui_media_picker_scene_selected_favorite_target(scene).action == UiMediaPickerAction::none &&
              ui_media_picker_scene_hit_test(scene, send_x, send_y).action == UiMediaPickerAction::item,
          "media picker favorites: pending never fabricates confirmation or falls through to Send");
    paint_stream_reset(&paint, 1000, 800);
    check(ui_media_picker_scene_paint_foreground(scene, amoled_theme, &paint) && paint.clip_depth == 1,
          "media picker favorites: pending paints bounded static status without a continuous spinner");
    bool pending_accent = false;
    for (u32 i = 0; i < paint.count; ++i)
        pending_accent = pending_accent || (paint.quads[i].fill.alpha > 0 &&
                                            paint.quads[i].fill.red == amoled_theme.accent.red &&
                                            paint.quads[i].fill.blue == amoled_theme.accent.blue);
    check(!pending_accent,
          "media picker favorites: unconfirmed pending star never paints confirmed accent");
    fixture.favorite_pending = false;
    fixture.favorite_failed = true;
    fixture.favorite = true; // Failed removal keeps the owner's confirmed favorite.
    ++source.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) &&
              scene.projection->cells[0].item.favorite &&
              ui_media_picker_scene_selected_favorite_target(scene).action ==
                  UiMediaPickerAction::favorite &&
              ui_media_picker_scene_hover(&scene, x, y, 1000),
          "media picker favorites: failed async mutation retains confirmed state and a retryable action");
    const bool retry_captured = ui_media_picker_scene_pointer_down(&scene, x, y);
    ++source.revision;
    check(retry_captured && !ui_media_picker_scene_favorite(scene, 0, &button) &&
              ui_media_picker_scene_selected_favorite_target(scene).action == UiMediaPickerAction::none &&
              ui_media_picker_scene_pointer_up(&scene, x, y).action == UiMediaPickerAction::none,
          "media picker favorites: unpublished source generation blocks old favorite capture and keyboard "
          "target");
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor),
          "media picker favorites: fresh owner revision republishes state");
    const u32 before_deep = fixture.calls;
    check(ui_media_picker_scene_scroll(&scene, 1'000'000.f) && scene.cell_count <= 48 &&
              scene.widgets.count == scene.cell_count + chrome_widgets && scene.widgets.count <= 96 &&
              fixture.calls - before_deep <= 48 && scene.projection->cells[0].source_index > 1000 &&
              scene.storage.arena.base == bank && scene.storage.arena.committed <= 128 * 1024,
          "media picker favorites: deep GIF catalog keeps bounded cells/widgets/64KiB without favorite "
          "widgets");
    bool distant_bounds = true;
    u32 visible_buttons = 0;
    for (u32 i = 0; i < scene.cell_count; ++i) {
        const auto& distant = scene.projection->cells[i];
        ui_media_picker_scene_hover(&scene, (distant.clip.left + distant.clip.right) * .5f,
                                    (distant.clip.top + distant.clip.bottom) * .5f, 2000);
        UiRect full_button{};
        if (ui_media_picker_scene_favorite(scene, i, &button, &full_button)) {
            ++visible_buttons;
            const auto hit = ui_media_picker_scene_hit_test(scene, (button.left + button.right) * .5f,
                                                            (button.top + button.bottom) * .5f);
            distant_bounds = distant_bounds &&
                             ui_rect_equal(button, ui_rect_intersection(full_button, distant.clip)) &&
                             hit.action == UiMediaPickerAction::favorite &&
                             hit.item_id == distant.item.id && hit.source_index == distant.source_index;
        }
    }
    paint_stream_reset(&paint, 1000, 800);
    check(distant_bounds && visible_buttons && fixture.calls - before_deep <= 48 &&
              ui_media_picker_scene_paint(scene, amoled_theme, &paint, nullptr, 96) &&
              ui_media_picker_scene_paint_foreground(scene, amoled_theme, &paint) &&
              paint.clip_depth == 1 && !paint.rejected && paint.count < kMaxPaintQuads,
          "media picker favorites: clipped distant stars preserve full geometry, exact visible source and "
          "bounded paint");
    ++content.query_generation;
    ++content.revision;
    check(!ui_media_picker_scene_favorite(scene, 0, &button) &&
              ui_media_picker_scene_selected_favorite_target(scene).action == UiMediaPickerAction::none,
          "media picker favorites: query replacement instantly retires favorite geometry and target");
    source.query_generation = content.query_generation;
    ++source.revision;
    fixture.kind = UiMediaPickerItemKind::category;
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) &&
              ui_media_picker_scene_select(&scene, 0) &&
              !ui_media_picker_scene_favorite(scene, 0, &button) &&
              ui_media_picker_scene_selected_favorite_target(scene).action == UiMediaPickerAction::none,
          "media picker favorites: provider category flag cannot turn navigation into favorite mutation");
    fixture.kind = UiMediaPickerItemKind::gif;
    fixture.disabled_first = 0;
    fixture.disabled_count = 1;
    ++source.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) &&
              !ui_media_picker_scene_favorite(scene, 0, &button) &&
              ui_media_picker_scene_hit_test(scene, send_x, send_y).action == UiMediaPickerAction::none,
          "media picker favorites: known unavailable record exposes neither favorite nor Send");
    fixture.disabled_count = 0;
    fixture.missing_asset = true;
    fixture.favorite = true;
    fixture.favorite_failed = false;
    ++source.revision;
    auto dense = ui_media_picker_style();
    dense.metrics.panel_width = 240;
    dense.metrics.panel_height = 768;
    dense.metrics.gap = 2;
    dense.metrics.gif_category_height = 48;
    dense.metrics.gif_target_width = 96;
    dense.metrics.icon_extent = 28;
    dense.metrics.footer_height = 64;
    check(ui_media_picker_scene_update(&scene, &source, &content, {0, 0, 1000, 1000}, {560, 900, 940, 940},
                                       &dense) &&
              ui_media_picker_scene_edge_selection(&scene, false) &&
              ui_media_picker_scene_select(&scene, 4) && scene.cell_count > 20,
          "media picker favorites: dense unknown-asset gallery exercises large confirmed glyphs");
    if (scene.cell_count > 4) {
        const auto& captured = scene.projection->cells[2];
        const auto& hovered = scene.projection->cells[0];
        check(ui_media_picker_scene_pointer_down(&scene, captured.rectangle.left + 12,
                                                 captured.rectangle.top + 12) &&
                  ui_media_picker_scene_hover(&scene, hovered.rectangle.left + 12,
                                              hovered.rectangle.top + 12, 3000) &&
                  ui_media_picker_scene_favorite(scene, 0, &button) &&
                  ui_media_picker_scene_favorite(scene, 2, &button) &&
                  ui_media_picker_scene_favorite(scene, 4, &button),
              "media picker favorites: distinct hover focus and capture expose three confirmed stars");
        paint_stream_reset(&paint, 1000, 1000);
        check(ui_media_picker_scene_paint(scene, amoled_theme, &paint, nullptr, 96) &&
                  ui_media_picker_scene_paint_foreground(scene, amoled_theme, &paint) &&
                  paint.count < kMaxPaintQuads && !paint.rejected && paint.clip_depth == 1 &&
                  scene.cell_count <= 48 && scene.widgets.count <= 96 &&
                  scene.storage.arena.committed <= 128 * 1024,
              "media picker favorites: three solid stars and dense neutral placeholders retain hard native "
              "budgets");
    }
    paint_stream_release(&paint);
    ui_media_picker_scene_release(&scene);
}
void test_picker_style(void (*check)(bool, const char*) noexcept) noexcept {
    const Theme amoled_theme = ui_theme_amoled();
    UiMediaPickerScene scene{};
    PickerFixture fixture{0, 256, 500'000, UiMediaPickerItemKind::sticker, false};
    UiMediaPickerSection sections[]{{1, 0, 128, utf8_literal("Guild stickers"), true},
                                    {2, 128, 128, utf8_literal("Sticker pack"), true}};
    UiMediaPickerSource source{&fixture, 9, 4, fixture.count, 1, {sections, 2}, resolve_picker};
    UiMediaPickerContent content{};
    content.account_scope = 9;
    content.query_generation = 4;
    content.revision = 1;
    content.kind = UiMediaPickerKind::sticker;
    content.phase = UiMediaPickerPhase::ready;
    content.available_tabs = 7;
    content.edit.active = content.edit.focused = true;
    content.placeholder = utf8_literal("Search stickers");
    const UiRect viewport{0, 0, 1000, 800}, anchor{560, 700, 940, 740};
    const auto defaults = ui_media_picker_style();
    check(ui_media_picker_style_valid(defaults) && sizeof(UiMediaPickerStyle) == 136 &&
              ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) &&
              ui_rect_width(scene.panel) == 498 && ui_rect_height(scene.panel) == 486 &&
              scene.tabs.top == scene.panel.top + 16 && scene.tabs.bottom == scene.panel.top + 46 &&
              scene.field.top == scene.panel.top + 58 && scene.field.bottom == scene.panel.top + 98 &&
              scene.body.top == scene.panel.top + 110 && scene.footer.top == scene.panel.bottom - 48 &&
              scene.footer.left == scene.rail.right && scene.rail.bottom == scene.panel.bottom,
          "media picker style: shared-token default preserves original chrome geometry");
    if (!scene.active)
        return;
    const auto bank = scene.storage.arena.base;
    const auto first = scene.projection->cells[0];
    const float x = (first.clip.left + first.clip.right) * .5f;
    const float y = (first.clip.top + first.clip.bottom) * .5f;
    const u32 generation = scene.generation;
    check(ui_media_picker_scene_pointer_down(&scene, x, y),
          "media picker style: exact tile capture begins before theme geometry changes");
    auto custom = defaults;
    custom.metrics.panel_width = 400;
    custom.metrics.panel_height = 520;
    custom.metrics.padding = 16;
    custom.metrics.gap = 10;
    custom.metrics.content_inset = 12;
    custom.metrics.tab_height = 36;
    custom.metrics.search_height = 44;
    custom.metrics.footer_height = 56;
    custom.metrics.section_height = 32;
    custom.metrics.rail_width = 52;
    custom.metrics.rail_pitch = 44;
    custom.metrics.scrollbar_gutter = 16;
    custom.panel_radius = 18;
    custom.control_radius = 9;
    custom.edit_radius = 10;
    custom.selection_width = 2;
    custom.category_opacity = .3f;
    const u32 calls = fixture.calls;
    check(ui_media_picker_style_valid(custom) &&
              ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor, &custom) &&
              scene.storage.arena.base == bank && scene.storage.arena.committed <= 128 * 1024 &&
              scene.generation != generation && !scene.widgets.captured.generation &&
              ui_media_picker_scene_pointer_up(&scene, x, y).action == UiMediaPickerAction::none &&
              fixture.calls - calls <= kUiMediaPickerVisibleCells &&
              scene.cell_count <= kUiMediaPickerVisibleCells && scene.widgets.count <= 96,
          "media picker style: owned override rebuilds bounded geometry and fences old capture");
    const auto custom_cell = scene.projection->cells[0];
    UiMediaPickerMediaSlot media{};
    check(ui_rect_width(scene.panel) == 400 && ui_rect_height(scene.panel) == 520 &&
              scene.field.top == scene.tabs.bottom + custom.metrics.header_gap && ui_rect_height(scene.field) == 44 &&
              ui_rect_width(scene.rail) == 52 && custom_cell.rectangle.top == scene.body.top + 32 &&
              custom_cell.rectangle.left == scene.body.left + custom.metrics.list_inset &&
              ui_media_picker_scene_media(scene, 0, &media) && media.radius == 9 &&
              media.fit == UiMediaPickerFit::contain &&
              media.rectangle.left == custom_cell.rectangle.left + 1 &&
              media.rectangle.right == custom_cell.rectangle.right - 1 &&
              ui_rect_equal(media.clip, ui_rect_intersection(media.rectangle, custom_cell.clip)) &&
              scene.widgets.focused.slot == scene.edit.slot,
          "media picker style: density, section, art mask and exclusive edit focus use one snapshot");
    const u32 custom_generation = scene.generation, custom_calls = fixture.calls;
    custom.control_radius = 11; // Mutating caller memory cannot mutate a published scene.
    check(scene.style.control_radius == 9 &&
              ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor, &scene.style) &&
              scene.generation == custom_generation && fixture.calls == custom_calls,
          "media picker style: caller value is copied and identical owned style does not resolve again");
    PaintStream paint{};
    UiTextCommand commands[128]{};
    UiTextCommandStream stream{};
    UiTextOutput output{&stream, nullptr};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1000, 800) &&
              ui_media_picker_scene_paint(scene, amoled_theme, &paint, nullptr, 120) &&
              ui_media_picker_scene_paint_foreground(scene, amoled_theme, &paint) &&
              ui_text_command_stream_initialize(&stream, {commands, 128}) &&
              ui_media_picker_scene_text_output(scene, amoled_theme, &output, nullptr) &&
              paint.clip_depth == 1 && !paint.rejected && !stream.rejected,
          "media picker style: non-default chrome and text paint with balanced native clips");
    bool panel_radius = false, cell_radius = false, text_clipped = true;
    for (u32 i = 0; i < paint.count; ++i) {
        const auto& quad = paint.quads[i];
        panel_radius = panel_radius ||
                       (quad.left == scene.panel.left && quad.top == scene.panel.top && quad.radius == 18);
        cell_radius = cell_radius || (quad.left == custom_cell.rectangle.left &&
                                      quad.top == custom_cell.rectangle.top && quad.radius == media.radius);
    }
    for (u32 i = 0; i < stream.count; ++i)
        text_clipped = text_clipped && commands[i].clip.left >= scene.panel.left &&
                       commands[i].clip.right <= scene.panel.right &&
                       commands[i].clip.top >= scene.panel.top &&
                       commands[i].clip.bottom <= scene.panel.bottom;
    check(panel_radius && !cell_radius && text_clipped,
          "media picker style: panel radius applies, resting stickers paint no tile and text stays inside the panel");
    auto preferences = client_preferences_defaults();
    preferences.theme = ClientTheme::custom_gradient;
    const Theme gradient = client_theme_projection(preferences);
    bool themed_panel = paint_stream_reset(&paint, 1000, 800) &&
                        ui_media_picker_scene_paint(scene, gradient, &paint, nullptr, 120);
    bool panel_material = false;
    u32 band_materials = 0, opaque_bands = 0;
    const float band_share = (1 - ui_background_tier_opacity(gradient, UiBackgroundTier::lower)) * .9f;
    const auto covers = [](const PaintQuad& q, UiRect band) noexcept {
        return ui_rect_has_area(band) && q.left <= band.left && q.top <= band.top && q.right >= band.right &&
               q.bottom >= band.bottom;
    };
    for (u32 i = 0; themed_panel && i < paint.count; ++i) {
        const auto& q = paint.quads[i];
        if (ui_rect_equal({q.left, q.top, q.right, q.bottom}, scene.panel))
            panel_material =
                q.border_width == -7 && q.reserved_x < 0 && q.fill.alpha == 1 &&
                q.reserved_y == ui_elevation_gradient_share(gradient, UiElevation::popover);
        else if (covers(q, scene.rail) || covers(q, scene.footer)) {
            band_materials += q.border_width == -7 && q.reserved_y == band_share;
            opaque_bands += bytes_equal(&q.fill, &gradient.base_lower, sizeof(Color)) && q.border_width != -7;
        }
    }
    check(themed_panel && panel_material && paint.clip_depth == 1,
          "media picker style: window inherits opaque global gradient with palette border alongside search "
          "field");
    check(themed_panel && band_materials == 1u + ui_rect_has_area(scene.footer) && !opaque_bands,
          "media picker style: the category rail and preview bar take the gradient on the lower tier instead "
          "of a flat plate");
    paint_stream_release(&paint);
    const u32 before_scroll = fixture.calls;
    check(ui_media_picker_scene_scroll(&scene, scene.cell_pitch * 3 + 17) &&
              fixture.calls - before_scroll <= 48 && ui_media_picker_scene_media(scene, 0, &media) &&
              ui_rect_equal(media.clip,
                            ui_rect_intersection(media.rectangle, scene.projection->cells[0].clip)) &&
              ui_media_picker_scene_scroll_section(&scene, 2) && scene.header_count &&
              scene.projection->headers[0].rectangle.top == scene.body.top,
          "media picker style: partial-row media clipping and section jump follow custom logical inset");
    auto invalid = custom;
    const u32 nan_bits = 0x7fc00001u;
    CopyMemory(&invalid.metrics.gap, &nan_bits, sizeof(nan_bits));
    check(!ui_media_picker_style_valid(invalid) &&
              ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor, &invalid) &&
              bytes_equal(&scene.style, &defaults, sizeof(defaults)) && scene.storage.arena.base == bank,
          "media picker style: non-finite override falls back atomically without allocating a new bank");
    invalid = defaults;
    invalid.metrics.panel_width = 4096;
    const u32 fallback_generation = scene.generation, fallback_calls = fixture.calls;
    check(!ui_media_picker_style_valid(invalid) &&
              ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor, &invalid) &&
              scene.generation == fallback_generation && fixture.calls == fallback_calls,
          "media picker style: invalid preferred extent coalesces to the already published defaults");
    auto narrow = defaults;
    narrow.metrics.panel_height = 200;
    narrow.metrics.content_inset = narrow.metrics.header_gap = 24;
    narrow.metrics.tab_height = 48;
    narrow.metrics.search_height = 64;
    narrow.metrics.footer_height = 80;
    const UiRect narrow_viewport{640, 0, 896, 800}, narrow_anchor{800, 700, 870, 740};
    check(ui_media_picker_style_valid(narrow) &&
              ui_media_picker_scene_update(&scene, &source, &content, narrow_viewport, narrow_anchor,
                                           &narrow) &&
              bytes_equal(&scene.style, &defaults, sizeof(defaults)) && ui_rect_width(scene.panel) == 240,
          "media picker style: safe scalar metrics still fall back when a narrow pane cannot contain art");
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) &&
              ui_media_picker_scene_scroll_section(&scene, 1),
          "media picker style: default geometry restores first group for unavailable-art fixture");
    fixture.disabled_first = 0;
    fixture.disabled_count = 1;
    ++source.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) &&
              !ui_media_picker_scene_media(scene, 0, &media) &&
              !ui_widget_manager_input_enabled(scene.widgets, scene.projection->cells[0].widget),
          "media picker style: known disabled format never requests an asset or exposes activation");
    ui_text_command_stream_reset(&stream);
    const bool unavailable_presented =
        paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1000, 800) &&
        ui_media_picker_scene_paint(scene, kDefaultTheme, &paint, nullptr, 96) &&
        ui_media_picker_scene_text_output(scene, kDefaultTheme, &output, nullptr);
    bool unavailable_label = false;
    for (u32 i = 0; i < stream.count; ++i)
        unavailable_label = unavailable_label || utf8_equal(commands[i].text, utf8_literal("Unavailable"));
    check(unavailable_presented && unavailable_label,
          "media picker style: disabled item visibly explains unavailable media");
    fixture.disabled_count = 0;
    fixture.missing_asset = true;
    ++source.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) &&
              !ui_media_picker_scene_media(scene, 0, &media),
          "media picker style: missing asset token provides no fabricated media request");
    paint_stream_reset(&paint, 1000, 800);
    ui_text_command_stream_reset(&stream);
    const bool missing_presented = ui_media_picker_scene_paint(scene, kDefaultTheme, &paint, nullptr, 96) &&
                                   ui_media_picker_scene_text_output(scene, kDefaultTheme, &output, nullptr);
    unavailable_label = false;
    for (u32 i = 0; i < stream.count; ++i)
        unavailable_label = unavailable_label || utf8_equal(commands[i].text, utf8_literal("Unavailable"));
    bool neutral_placeholder = false;
    const auto missing = scene.projection->cells[0].rectangle;
    const float missing_extent =
        minimum(scene.style.metrics.icon_extent,
                (ui_rect_height(missing) - scene.style.metrics.text_inset * 2) * .5f);
    const float missing_left = (missing.left + missing.right - missing_extent) * .5f;
    const float missing_top = (missing.top + missing.bottom - missing_extent) * .5f;
    for (u32 i = 0; i < paint.count; ++i) {
        const auto& q = paint.quads[i];
        neutral_placeholder =
            neutral_placeholder ||
            (q.left == missing_left && q.top == missing_top && q.right == missing_left + missing_extent &&
             q.bottom == missing_top + missing_extent && ui_icon_quad(q) &&
             ui_icon_quad_data(q).icon == static_cast<u32>(UiIcon::image) &&
             q.fill.alpha == kDefaultTheme.text_secondary.alpha * .65f);
    }
    check(missing_presented && !unavailable_label && neutral_placeholder && paint.clip_depth == 1 &&
              !paint.rejected,
          "media picker style: unknown asset has a static centered neutral glyph without unavailable or "
          "loading claims");
    fixture.missing_asset = false;
    fixture.disabled_first = 0;
    fixture.disabled_count = fixture.count;
    source.sections = {};
    ++source.revision;
    auto dense = defaults;
    dense.metrics.panel_width = 240;
    dense.metrics.panel_height = 768;
    check(ui_media_picker_scene_update(&scene, &source, &content, {0, 0, 1000, 1000}, {560, 900, 940, 940},
                                       &dense) &&
              ui_media_picker_scene_scroll(&scene, 29),
          "media picker style: dense unavailable catalog projects partial edge rows");
    paint_stream_reset(&paint, 1000, 1000);
    ui_text_command_stream_reset(&stream);
    check(scene.cell_count == 16 && scene.cell_count <= kUiMediaPickerVisibleCells &&
              scene.body.bottom == scene.footer.top &&
              scene.widgets.count <= kUiMediaPickerWidgetCapacity &&
              ui_media_picker_scene_paint(scene, kDefaultTheme, &paint, nullptr, 96) &&
              ui_media_picker_scene_paint_foreground(scene, kDefaultTheme, &paint) &&
              ui_media_picker_scene_text_output(scene, kDefaultTheme, &output, nullptr) &&
              paint.count < kMaxPaintQuads && !paint.rejected && !stream.rejected && paint.clip_depth == 1,
          "media picker style: full-body unavailable cells retain native paint/text capacity and clips");
    fixture.disabled_count = 0;
    fixture.missing_asset = true;
    ++source.revision;
    paint_stream_reset(&paint, 1000, 1000);
    ui_text_command_stream_reset(&stream);
    check(ui_media_picker_scene_update(&scene, &source, &content, {0, 0, 1000, 1000}, {560, 900, 940, 940},
                                       &dense) &&
              scene.cell_count == 16 && scene.cell_count <= kUiMediaPickerVisibleCells &&
              scene.body.bottom == scene.footer.top &&
              ui_media_picker_scene_paint(scene, amoled_theme, &paint, nullptr, 96) &&
              ui_media_picker_scene_paint_foreground(scene, amoled_theme, &paint) &&
              ui_media_picker_scene_text_output(scene, amoled_theme, &output, nullptr) &&
              !ui_media_picker_scene_media(scene, 0, &media) && !paint.rejected && !stream.rejected &&
              paint.count < kMaxPaintQuads && paint.clip_depth == 1 && scene.widgets.count <= 96 &&
              scene.storage.arena.committed <= 128 * 1024,
          "media picker style: full-body unknown asset glyphs keep native stream and scene budgets without "
          "fabricated requests");
    paint_stream_release(&paint);
    ui_media_picker_scene_release(&scene);
}
UiMediaPickerContent picker_content(UiMediaPickerKind kind, u64 scope = 7, u64 query = 1) noexcept {
    UiMediaPickerContent content{};
    content.account_scope = scope;
    content.query_generation = query;
    content.revision = 1;
    content.kind = kind;
    content.phase = UiMediaPickerPhase::ready;
    content.available_tabs = 7;
    content.edit.active = true;
    return content;
}
void test_picker_resize(void (*check)(bool, const char*) noexcept) noexcept {
    UiMediaPickerScene scene{};
    PickerFixture fixture{0, 32, 900, UiMediaPickerItemKind::gif, false};
    UiMediaPickerSource source{&fixture, 7, 1, fixture.count, 1, {}, resolve_picker};
    auto content = picker_content(UiMediaPickerKind::gif);
    content.back = true;
    auto style = ui_media_picker_style();
    style.metrics.panel_width = 400;
    style.metrics.padding = 16;
    const UiRect viewport{0, 0, 1000, 800}, anchor{560, 700, 940, 740};
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor, &style, 600) &&
              ui_rect_width(scene.panel) == 600 && scene.panel.right == anchor.right && scene.cell_count != 0 &&
              scene.projection->cells[0].rectangle.left == scene.body.left + style.metrics.gif_gutter &&
              ui_media_picker_scene_scroll(&scene, 40),
          "media picker resize: requested width keeps the masonry gutter and right anchor");
    const auto bank = scene.storage.arena.base;
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor, &style, 10) &&
              ui_rect_width(scene.panel) == 400 && scene.scroll_dips == 40 &&
              scene.storage.arena.base == bank && scene.query_generation == source.query_generation,
          "media picker resize: theme minimum retains query scroll and bounded storage");
    check(ui_media_picker_scene_update(&scene, &source, &content, {0, 0, 350, 800}, anchor, &style, 900) &&
              ui_rect_width(scene.panel) == 334 && scene.panel.left == style.metrics.viewport_inset &&
              scene.panel.right == 350 - style.metrics.viewport_inset,
          "media picker resize: narrow viewport takes precedence over preferred width and minimum");
    content.kind = UiMediaPickerKind::emoji;
    content.back = false;
    content.available_tabs = ui_media_picker_tab_bit(content.kind);
    content.variant = UiMediaPickerVariant::reaction;
    source.count = 0;
    ++source.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor, nullptr, 900) &&
              ui_rect_width(scene.panel) == kUiMediaPickerReactionWidth,
          "media picker resize: reaction popout ignores expression drawer preference");
    ui_media_picker_scene_release(&scene);
}
// Fixed cells: the column count follows the width, never the cell size.
void test_picker_fixed_cells(void (*check)(bool, const char*) noexcept) noexcept {
    UiMediaPickerScene scene{};
    PickerFixture fixture{0, 400, 100, UiMediaPickerItemKind::emoji, false};
    fixture.missing_asset = true;
    fixture.glyph = utf8_literal("😀");
    UiMediaPickerSection sections[]{{1, 0, 200, utf8_literal("Original People"), true},
                                    {2, 200, 200, utf8_literal("Original Nature"), true}};
    sections[0].visual = UiMediaPickerSectionVisual::people;
    sections[1].visual = UiMediaPickerSectionVisual::nature;
    UiMediaPickerSource source{&fixture, 7, 1, fixture.count, 1, {sections, 2}, resolve_picker};
    auto content = picker_content(UiMediaPickerKind::emoji);
    const UiRect viewport{0, 0, 2400, 900}, anchor{2000, 850, 2380, 890};
    // Discord: 498 DIP fits nine 48 DIP cells beside the 48 DIP rail; from 538
    // DIP the drawer uses 56 DIP cells (48 DIP emoji).
    struct Width {
        float width;
        u16 columns;
        float cell;
    } const widths[]{{498, 9, 48}, {537, 9, 48}, {538, 8, 56}, {800, 13, 56}, {1200, 20, 56}};
    bool columns = true, cells = true, glyphs = true;
    UiTextCommand commands[512]{};
    UiTextCommandStream stream{};
    UiTextOutput output{&stream, nullptr};
    for (const auto& w : widths) {
        columns = columns &&
                  ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor, nullptr, w.width) &&
                  ui_rect_width(scene.panel) == w.width && scene.columns == w.columns &&
                  scene.cell_width == w.cell && scene.cell_pitch == w.cell;
        for (u32 i = 0; columns && i < scene.cell_count; ++i) {
            const auto& r = scene.projection->cells[i].rectangle;
            cells = cells && ui_rect_width(r) == w.cell && ui_rect_height(r) == w.cell &&
                    r.right <= scene.body.right - scene.style.metrics.scrollbar_gutter &&
                    r.left >= scene.body.left + scene.style.metrics.list_inset;
        }
        glyphs = glyphs && ui_text_command_stream_initialize(&stream, {commands, 512}) &&
                 ui_media_picker_scene_text_output(scene, kDefaultTheme, &output, nullptr);
        const u32 expected = w.cell > 48 ? text_style_picker_large_emoji : text_style_picker_grid_emoji;
        for (u32 i = 0; glyphs && i < stream.count; ++i)
            if (utf8_equal(commands[i].text, fixture.glyph) && commands[i].owner.handle.slot != scene.root.slot &&
                commands[i].rectangle.left >= scene.body.left)
                glyphs = commands[i].style_flags == expected && ui_rect_width(commands[i].rectangle) == w.cell;
    }
    check(columns, "media picker cells: emoji column count follows width at 498, 537, 538, 800 and 1200 DIP");
    check(cells, "media picker cells: every emoji cell keeps its fixed size, left-aligned in the grid");
    check(glyphs && text_style_font_size(16, text_style_picker_grid_emoji) == 40 &&
              text_style_font_size(16, text_style_picker_large_emoji) == 48,
          "media picker cells: emoji glyphs use Discord's medium and large sizes with their cells");
    // Rows step with the dynamic column count: one grid ignoring section breaks.
    UiMediaPickerSection navigation[]{{1, 0, 10, utf8_literal("Original People"), true},
                                      {2, 10, 3, utf8_literal("Original Nature"), true},
                                      {3, 13, 12, utf8_literal("Original Food"), true}};
    fixture.count = source.count = 25;
    source.sections = {navigation, 3};
    ++source.revision;
    const auto move = [&](i32 h, i32 v) noexcept {
        return ui_media_picker_scene_move_selection(&scene, h, v) ? scene.selected_index : ~u32{0};
    };
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor, nullptr, 498) &&
              scene.columns == 9 && ui_media_picker_scene_select(&scene, 7) && move(0, 1) == 9 &&
              move(0, 1) == 10 && move(0, 1) == 13 && move(0, -1) == 10 && move(0, -1) == 9 &&
              move(0, -1) == 0 && ui_media_picker_scene_select(&scene, 8) && move(1, 0) == 9 &&
              move(1, 0) == 10 && move(-1, 0) == 9 && ui_media_picker_scene_select(&scene, 12) &&
              move(0, 1) == 15 && move(0, -1) == 12 && ui_media_picker_scene_select(&scene, 22) &&
              move(0, -1) == 13 && ui_media_picker_scene_select(&scene, 15) &&
              ui_media_picker_scene_row_edge_selection(&scene, false) && scene.selected_index == 13 &&
              ui_media_picker_scene_row_edge_selection(&scene, true) && scene.selected_index == 21,
          "media picker keyboard: nine columns clamp Up/Down to short rows and wrap Left/Right across "
          "sections");
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor, nullptr, 800) &&
              scene.columns == 13 && ui_media_picker_scene_select(&scene, 4) && move(0, 1) == 12 &&
              move(0, 1) == 15 && move(0, 1) == ~u32{0} && ui_media_picker_scene_select(&scene, 9) &&
              move(1, 0) == 10,
          "media picker keyboard: thirteen columns reflow rows and stop at the grid's last row");
    // Footer: the inspected item's ":name:" and source; the first item by default.
    UiMediaPickerSection labelled[]{{4, 0, 25, utf8_literal("Original People"), true}};
    source.sections = {labelled, 1};
    ++source.revision;
    // A new query clears the keyboard selection.
    content.query_generation = source.query_generation = 2;
    ++content.revision;
    char name[96]{};
    const u32 name_bytes = emoji_shortcode_key(fixture.glyph, {name, sizeof(name)});
    bool footer = ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor, nullptr, 498) &&
                  name_bytes && scene.cell_count > 3;
    const auto title = ui_media_picker_scene_footer_title(scene);
    footer = footer && ui_media_picker_scene_inspected(scene) == &scene.projection->cells[0] &&
             title.bytes == name_bytes + 2 && title.data[0] == ':' && title.data[title.bytes - 1] == ':' &&
             bytes_equal(title.data + 1, name, name_bytes) &&
             utf8_equal(ui_media_picker_scene_footer_detail(scene), utf8_literal("from Synthetic catalog fixture"));
    if (footer) {
        const auto hovered = scene.projection->cells[3].clip;
        footer = ui_media_picker_scene_hover(&scene, hovered.left + 4, hovered.top + 4, 10) &&
                 ui_media_picker_scene_inspected(scene) == &scene.projection->cells[3];
    }
    fixture.glyph = {};
    fixture.missing_asset = false;
    ++source.revision;
    UiMediaPickerMediaSlot art{};
    footer = footer && ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor, nullptr, 498) &&
             utf8_equal(ui_media_picker_scene_footer_title(scene), utf8_literal(":Original native media:")) &&
             ui_media_picker_scene_footer_media(scene, &art) &&
             ui_rect_width(art.rectangle) == scene.style.metrics.footer_art &&
             art.rectangle.left == scene.footer.left + scene.style.metrics.padding &&
             art.clip.top >= scene.footer.top && art.clip.bottom <= scene.footer.bottom;
    check(footer, "media picker footer: inspects the first item, then the hovered one, as :name: and its "
                  "source with a 28 DIP preview");
    ui_media_picker_scene_release(&scene);
    check(!scene.storage.arena.base && !scene.tiles && !scene.active,
          "media picker cells: a released picker keeps no storage");
}
// GIF results: Discord's masonry adds a column per 200 DIP (at most eight) and
// places each tile in the shortest column; landing tiles share 110 DIP.
bool resolve_masonry(void* opaque, u32 index, UiMediaPickerItem* item) noexcept {
    if (!resolve_picker(opaque, index, item))
        return false;
    item->height = 90 + index % 3 * 90;
    return true;
}
void test_picker_masonry(void (*check)(bool, const char*) noexcept) noexcept {
    UiMediaPickerScene scene{};
    PickerFixture fixture{0, 120, 4000, UiMediaPickerItemKind::category, false};
    UiMediaPickerSource source{&fixture, 7, 1, fixture.count, 1, {}, resolve_masonry};
    auto content = picker_content(UiMediaPickerKind::gif);
    const UiRect viewport{0, 0, 2400, 1000}, anchor{2000, 900, 2380, 940};
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) && scene.columns == 2 &&
              !scene.masonry && scene.cell_height == 110 &&
              ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor, nullptr, 2000) &&
              scene.columns == 6,
          "media picker masonry: landing categories use 110 DIP tiles, two columns at 498 DIP, capped at six");
    fixture.kind = UiMediaPickerItemKind::gif;
    content.back = true;
    ++content.revision;
    ++source.revision;
    struct Width {
        float width;
        u16 columns;
    } const widths[]{{498, 2}, {600, 3}, {1000, 5}, {2000, 8}};
    bool columns = true;
    for (const auto& w : widths)
        columns = columns &&
                  ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor, nullptr, w.width) &&
                  scene.masonry && scene.columns == w.columns &&
                  scene.cell_width == (w.width - 24 - 12 * (w.columns - 1)) / w.columns;
    check(columns, "media picker masonry: results add a column per 200 DIP up to eight");
    bool placed = ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor, nullptr, 600) &&
                  scene.tile_count == fixture.count;
    float heights[3]{12, 12, 12};
    for (u32 i = 0; placed && i < scene.tile_count; ++i) {
        u32 shortest = 0;
        for (u32 c = 1; c < 3; ++c)
            if (heights[c] < heights[shortest])
                shortest = c;
        const auto& tile = scene.tiles[i];
        const float height = scene.cell_width * (static_cast<float>(90 + i % 3 * 90) / 320);
        placed = tile.column == shortest && tile.top == heights[shortest] && tile.height == height;
        heights[shortest] += height + 12;
    }
    bool cells = placed && scene.cell_count;
    for (u32 i = 0; cells && i < scene.cell_count; ++i) {
        const auto& cell = scene.projection->cells[i];
        const auto& tile = scene.tiles[cell.source_index];
        cells = cell.rectangle.left == scene.grid_left + tile.column * scene.column_pitch &&
                ui_rect_height(cell.rectangle) == tile.height;
    }
    check(placed && cells, "media picker masonry: tiles fill the shortest column with aspect-true heights");
    bool keys = ui_media_picker_scene_select(&scene, 0) && ui_media_picker_scene_move_selection(&scene, 0, 1) &&
                scene.tiles[scene.selected_index].column == 0 && scene.selected_index > 2;
    keys = keys && ui_media_picker_scene_move_selection(&scene, 0, -1) && scene.selected_index == 0 &&
           ui_media_picker_scene_move_selection(&scene, 1, 0) && scene.tiles[scene.selected_index].column == 1;
    check(keys, "media picker masonry: Up/Down stay in the column and Right reaches the next column");
    const u32 resolves = scene.counters.resolves, layouts = scene.counters.masonry_layouts;
    const u32 allocations = scene.counters.allocations;
    const auto bank = scene.storage.arena.base;
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor, nullptr, 1000) &&
              scene.columns == 5 && scene.counters.masonry_layouts == layouts + 1 &&
              scene.counters.resolves - resolves <= scene.cell_count &&
              scene.counters.allocations == allocations && scene.storage.arena.base == bank,
          "media picker masonry: a resize re-places tiles, resolving only visible cells and allocating nothing");
    ui_media_picker_scene_release(&scene);
}
// Resize cost: one rebuild per width, only visible cells resolved, no
// allocation after the first update.
void test_picker_resize_cost(void (*check)(bool, const char*) noexcept) noexcept {
    UiMediaPickerScene scene{};
    PickerFixture fixture{0, 3000, 100, UiMediaPickerItemKind::emoji, false};
    fixture.missing_asset = true;
    fixture.glyph = utf8_literal("😀");
    UiMediaPickerSection sections[30]{};
    for (u32 i = 0; i < 30; ++i)
        sections[i] = {i + 1ull, i * 100, 100, utf8_literal("Original category"), true};
    UiMediaPickerSource source{&fixture, 7, 1, fixture.count, 1, {sections, 30}, resolve_picker};
    auto content = picker_content(UiMediaPickerKind::emoji);
    const UiRect viewport{0, 0, 3000, 900}, anchor{2600, 850, 2980, 890};
    bool valid = ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor);
    const u32 allocations = scene.counters.allocations;
    const auto bank = scene.storage.arena.base;
    const auto committed = scene.storage.arena.committed;
    const float widths[]{560, 700, 900, 1300, 1700, 640, 498};
    for (float width : widths) {
        const auto before = scene.counters;
        valid = valid &&
                ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor, nullptr, width) &&
                scene.counters.rebuilds == before.rebuilds + 1 &&
                scene.counters.resolves - before.resolves == scene.cell_count &&
                scene.counters.cells - before.cells == scene.cell_count &&
                scene.cell_count <= kUiMediaPickerVisibleCells && scene.counters.allocations == allocations &&
                scene.storage.arena.base == bank && scene.storage.arena.committed == committed;
    }
    check(valid && allocations == 1,
          "media picker resize cost: each width rebuilds once, resolves only visible cells and allocates "
          "nothing");
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor, nullptr, 2900) &&
              ui_rect_width(scene.panel) < 2900 && scene.cell_count <= kUiMediaPickerVisibleCells &&
              scene.counters.allocations == allocations,
          "media picker resize cost: an oversized drawer shrinks to the columns its cells can hold");
    ui_media_picker_scene_release(&scene);
}
} // namespace
void test_media_picker(void (*check)(bool, const char*) noexcept) noexcept {
    const Theme amoled_theme = ui_theme_amoled();
    test_picker_scrollbars(check);
    test_picker_rail_and_scroll_reuse(check);
    test_picker_emoji(check);
    test_picker_style(check);
    test_picker_favorites(check);
    test_picker_resize(check);
    test_picker_fixed_cells(check);
    test_picker_masonry(check);
    test_picker_resize_cost(check);
    UiMediaPickerScene scene{};
    PickerFixture fixture{0, 65'536, 10, UiMediaPickerItemKind::category, false};
    UiMediaPickerSource source{&fixture, 7, 1, fixture.count, 1, {}, resolve_picker};
    UiMediaPickerContent content{};
    content.account_scope = 7;
    content.query_generation = 1;
    content.revision = 1;
    content.kind = UiMediaPickerKind::gif;
    content.phase = UiMediaPickerPhase::ready;
    content.available_tabs = 7;
    content.placeholder = utf8_literal("Search GIFs");
    content.status = utf8_literal("Browse categories");
    content.edit.active = true;
    const UiRect viewport{0, 0, 1000, 800}, anchor{560, 700, 940, 740};
    check(!scene.storage.arena.base &&
              ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) &&
              scene.cell_count && scene.cell_count <= kUiMediaPickerVisibleCells &&
              fixture.calls == scene.cell_count && scene.storage.arena.committed <= 128 * 1024 &&
              scene.columns == 2,
          "media picker: category gallery commits one bounded bank and resolves only visible records out "
          "of65536");
    if (!scene.active)
        return;
    check(scene.projection->cells[0].rectangle.top == scene.body.top + scene.style.metrics.gif_gutter,
          "media picker: first gallery row keeps the masonry gutter beneath the divider");
    const auto bank = scene.storage.arena.base;
    const auto calls = fixture.calls, generation = scene.generation;
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) &&
              fixture.calls == calls && scene.generation == generation && scene.storage.arena.base == bank,
          "media picker: unchanged frame needs no allocation, layout or source resolve");
    ++content.revision;
    content.edit.caret_visible = true;
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) &&
              fixture.calls == calls && scene.generation == generation,
          "media picker: shared edit caret updates preserve gallery widgets and media requests");
    const auto first = scene.projection->cells[0];
    const float x = first.clip.left + 10, y = first.clip.top + 10;
    auto hit = ui_media_picker_scene_hit_test(scene, x, y);
    check(hit.action == UiMediaPickerAction::category && hit.item_id == first.item.id &&
              hit.account_scope == 7 && hit.query_generation == 1 && hit.source_revision == 1 &&
              hit.scene_generation == scene.generation,
          "media picker: landing category returns exact bank/query/item/generation activation");
    UiMediaPickerMediaSlot media{};
    check(ui_media_picker_scene_media(scene, 0, &media) && media.asset_id == first.item.asset_id &&
              media.fit == UiMediaPickerFit::cover && media.opacity < 1 && media.asset_flags == 0x0201 &&
              ui_rect_equal(media.clip, first.clip) && media.radius == kUiControlCornerRadius,
          "media picker: visible preview request shares category cover geometry, dimming and corner mask");
    check(ui_media_picker_scene_hover(&scene, x, y, 1000) &&
              !ui_media_picker_scene_hover(&scene, x + 1, y + 1, 1200) &&
              scene.widgets.hover_started_ms == 1000,
          "media picker: hover timer comes from caller clock and survives motion within same tile");
    check(ui_media_picker_scene_pointer_down(&scene, x, y) &&
              ui_media_picker_target_equal(ui_media_picker_scene_pointer_up(&scene, x, y), hit),
          "media picker: pointer down and up admit only the same live tile");
    const bool stale_capture = ui_media_picker_scene_pointer_down(&scene, x, y);
    ++source.revision;
    check(stale_capture && ui_media_picker_scene_hit_test(scene, x, y).action == UiMediaPickerAction::none &&
              !ui_media_picker_scene_media(scene, 0, &media) &&
              ui_media_picker_scene_pointer_up(&scene, x, y).action == UiMediaPickerAction::none,
          "media picker: unpublished source revision rejects selection and preview requests before reuse");
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor),
          "media picker: updated source rebinds");
    const u32 before_scroll = fixture.calls;
    check(!ui_media_picker_scene_scroll(&scene, scene.content_height) && fixture.calls == before_scroll,
          "media picker: oversized scroll delta is rejected without resolving catalog");
    check(ui_media_picker_scene_scroll(&scene, 1'000'000.f) && scene.cell_count &&
              scene.cell_count <= kUiMediaPickerVisibleCells &&
              fixture.calls - before_scroll <= kUiMediaPickerVisibleCells &&
              scene.projection->cells[0].source_index > 1000 && scene.storage.arena.base == bank,
          "media picker: deep scroll projects actual distant catalog indices");
    // Logical extent may exceed one valid pointer/wheel delta. Reach its end
    // through bounded admitted increments and measure every projection.
    const float end_scroll = maximum(0.f, scene.content_height - ui_rect_height(scene.body));
    bool incremental_bounds = true;
    for (u32 step = 0; step < 4 && scene.scroll_dips < end_scroll; ++step) {
        const u32 prior_calls = fixture.calls;
        incremental_bounds = ui_media_picker_scene_scroll(&scene, kMaximumPaintCoordinate) &&
                             scene.cell_count && scene.cell_count <= kUiMediaPickerVisibleCells &&
                             fixture.calls - prior_calls <= kUiMediaPickerVisibleCells &&
                             scene.storage.arena.base == bank && incremental_bounds;
    }
    check(incremental_bounds && scene.scroll_dips == end_scroll,
          "media picker: admitted incremental scrolling reaches exact inset-aware endpoint with bounded "
          "projections");
    check(ui_media_picker_scene_edge_selection(&scene, false) && scene.selected_index == 0 &&
              ui_media_picker_scene_move_selection(&scene, 0, 1) && scene.selected_index == 2 &&
              ui_media_picker_scene_page_selection(&scene, false) && scene.selected_index > 2,
          "media picker: Home, directional movement and paging preserve typed gallery selection");
    check(ui_media_picker_scene_edge_selection(&scene, false) && scene.scroll_dips == 0 &&
              scene.projection->cells[0].rectangle.top == scene.body.top + scene.style.metrics.gif_gutter,
          "media picker: Home restores top inset after deep scrolling");
    content.edit.focused = true;
    ++content.revision;
    const u32 focus_calls = fixture.calls, focus_generation = scene.generation;
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) &&
              scene.widgets.focused.slot == scene.edit.slot &&
              scene.widgets.focused.generation == scene.edit.generation && scene.selected_index == 0 &&
              fixture.calls == focus_calls && scene.generation == focus_generation,
          "media picker: focus-only edit update retains selected item without moving focus off search or "
          "resolving catalog");
    check(ui_media_picker_scene_select(&scene, 2) && scene.widgets.focused.slot == scene.edit.slot &&
              scene.widgets.focused.generation == scene.edit.generation && scene.selected_index == 2,
          "media picker: source projection rebuild preserves exclusive search focus while a tile stays "
          "selected");
    content.edit.focused = false;
    ++content.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) &&
              scene.widgets.focused.slot != scene.edit.slot && scene.widgets.focused.generation,
          "media picker: releasing edit ownership restores the selected tile focus");
    ++content.query_generation;
    ++content.revision;
    check(!ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) &&
              ui_media_picker_scene_hit_test(scene, x, y).action == UiMediaPickerAction::none,
          "media picker: different query cannot borrow previous query source bank");
    source.query_generation = content.query_generation;
    ++source.revision;
    fixture.epoch += 100'000;
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) &&
              scene.scroll_dips == 0 && scene.selected_index == ~u32{0},
          "media picker: new query clears old selection and scroll without reallocating bank");

    fixture.count = source.count = 120;
    fixture.kind = UiMediaPickerItemKind::sticker;
    UiMediaPickerSection sections[]{{101, 0, 60, utf8_literal("Original Guild"), true},
                                    {102, 60, 60, utf8_literal("Original Pack"), true}};
    source.sections = {sections, 2};
    ++source.revision;
    content.kind = UiMediaPickerKind::sticker;
    content.placeholder = utf8_literal("Search stickers");
    ++content.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) && scene.columns == 4 &&
              scene.rail_count == 2 && scene.header_count &&
              ui_media_picker_scene_media(scene, 0, &media) && media.fit == UiMediaPickerFit::contain &&
              media.rectangle.left > scene.projection->cells[0].rectangle.left &&
              ui_media_picker_scene_scroll_section(&scene, 102) &&
              scene.projection->cells[0].source_index >= 60,
          "media picker: sticker groups use real section rail, contained art and exact section navigation");
    check(scene.header_count && scene.projection->headers[0].section_index == 1 &&
              scene.projection->headers[0].rectangle.top == scene.body.top &&
              ui_media_picker_scene_scroll_section(&scene, 101) && scene.scroll_dips == 0 &&
              scene.projection->headers[0].rectangle.top == scene.body.top &&
              scene.projection->cells[0].rectangle.top == scene.body.top + 32,
          "media picker: section jumps put the section's header at the top above its first row");
    sections[1].expanded = false;
    ++source.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) &&
              !ui_media_picker_scene_select(&scene, 75),
          "media picker: collapsed owner section cannot activate hidden sticker records");
    UiTextCommand commands[128]{};
    UiTextCommandStream stream{};
    UiTextOutput output{&stream, nullptr};
    PaintStream paint{};
    check(ui_text_command_stream_initialize(&stream, {commands, 128}) &&
              ui_media_picker_scene_text_output(scene, amoled_theme, &output, nullptr) &&
              paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1000, 800) &&
              ui_media_picker_scene_paint(scene, amoled_theme, &paint, nullptr, 96) &&
              paint.clip_depth == 1 && paint.rejected == 0,
          "media picker: existing themed paint/text primitives emit bounded clips and native chrome");
    bool clipped = true;
    for (u32 i = 0; i < stream.count; ++i)
        clipped = clipped && commands[i].clip.left >= scene.panel.left &&
                  commands[i].clip.right <= scene.panel.right && commands[i].clip.top >= scene.panel.top &&
                  commands[i].clip.bottom <= scene.panel.bottom;
    check(clipped, "media picker: every native glyph command stays within its owned panel");
    paint_stream_release(&paint);
    content.available_tabs = ui_media_picker_tab_bit(UiMediaPickerKind::sticker);
    ++content.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) &&
              !scene.tab_widgets[0].generation && !scene.tab_widgets[2].generation &&
              scene.tab_widgets[1].generation,
          "media picker: unsupported tabs are absent rather than enabled no-op controls");
    const float usable_offset=scene.scroll_dips;
    fixture.bad_index=scene.projection->cells[scene.cell_count-1].source_index + scene.columns;
    check(!ui_media_picker_scene_scroll(&scene,scene.cell_pitch*3) && scene.active &&
              ui_media_picker_scene_current(scene) && scene.scroll_dips==usable_offset && scene.cell_count,
          "media picker: failed new row restores the usable viewport without dismissing the pane");
    fixture.bad_index=~u32{0};
    check(ui_media_picker_scene_scroll(&scene,scene.cell_pitch*3) && scene.active,
          "media picker: scrolling resumes after a temporary row projection failure");
    fixture.bad = true;
    ++source.revision;
    check(!ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) && !scene.active &&
              !scene.cell_count && !ui_media_picker_scene_cells(scene).count,
          "media picker: invalid provider record retires the whole projection without partial clickable "
          "tiles");
    fixture.bad = false;
    ++source.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor),
          "media picker: valid catalog can reopen the existing bounded bank after failed projection");
    content.available_tabs = 7;
    ++content.revision;
    const UiRect narrow_viewport{640, 0, 896, 800}, narrow_anchor{800, 700, 870, 740};
    check(ui_media_picker_scene_update(&scene, &source, &content, narrow_viewport, narrow_anchor) &&
              ui_rect_width(scene.panel) == 240,
          "media picker: available pane reproduces minimum gallery width");
    bool tabs_fit = true;
    float previous_right = scene.tabs.left;
    for (u32 i = 0; i < 3; ++i) {
        const auto* node = ui_widget_manager_layout_node(scene.widgets, scene.tab_widgets[i]);
        tabs_fit =
            tabs_fit && node && node->rectangle.left >= previous_right &&
            node->rectangle.right <= scene.tabs.right && ui_rect_width(node->rectangle) <= 80 &&
            ui_media_picker_scene_hit_test(scene, (node->rectangle.left + node->rectangle.right) * .5f,
                                           (node->rectangle.top + node->rectangle.bottom) * .5f)
                    .action == UiMediaPickerAction::tab;
        if (node)
            previous_right = node->rectangle.right;
    }
    check(tabs_fit, "media picker: all three labeled tabs stay reachable inside240DIP panel");
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor),
          "media picker: wider gallery restores");
    fixture.disabled_first = 0;
    fixture.disabled_count = 2;
    u32 navigation_calls = fixture.calls;
    check(ui_media_picker_scene_edge_selection(&scene, false) && scene.selected_index == 2 &&
              fixture.calls - navigation_calls <= kUiMediaPickerVisibleCells + 3,
          "media picker: Home skips disabled items while resolving only candidate prefix and viewport");
    fixture.disabled_first = 10;
    fixture.disabled_count = 1;
    check(ui_media_picker_scene_select(&scene, 9),
          "media picker: keyboard starts before disabled vector item");
    navigation_calls = fixture.calls;
    check(ui_media_picker_scene_move_selection(&scene, 1, 0) && scene.selected_index == 11 &&
              fixture.calls - navigation_calls <= kUiMediaPickerVisibleCells + 2,
          "media picker: Arrow advances past disabled vector without trapping selection");
    fixture.disabled_first = 58;
    fixture.disabled_count = 2;
    check(ui_media_picker_scene_edge_selection(&scene, true) && scene.selected_index == 57,
          "media picker: End skips collapsed trailing section and disabled expanded tail");
    fixture.disabled_first = ~u32{0};
    fixture.disabled_count = 0;
    check(ui_media_picker_scene_select(&scene, 0), "media picker: Page starts at enabled first record");
    const u32 page_target =
        maximum(1u, static_cast<u32>(ui_rect_height(scene.body) / scene.cell_pitch)) * scene.columns;
    fixture.disabled_first = page_target;
    fixture.disabled_count = 2;
    check(ui_media_picker_scene_page_selection(&scene, false) && scene.selected_index == page_target + 2,
          "media picker: Page resolves next enabled record beyond disabled destination");
    fixture.disabled_first = ~u32{0};
    fixture.disabled_count = 0;
    check(ui_media_picker_scene_select(&scene, 55),
          "media picker: selection precedes newly collapsed section");
    sections[0].expanded = false;
    sections[1].expanded = true;
    fixture.hidden_first = 0;
    fixture.hidden_count = 60;
    ++source.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) &&
              ui_media_picker_scene_move_selection(&scene, 1, 0) && scene.selected_index == 60 &&
              !fixture.hidden_requested,
          "media picker: keyboard jumps collapsed metadata without resolving hidden catalog records");
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1000, 800) &&
              ui_media_picker_scene_paint(scene, kDefaultTheme, &paint, nullptr, 96) &&
              ui_media_picker_scene_paint_foreground(scene, kDefaultTheme, &paint) && paint.clip_depth == 1,
          "media picker: native selection foreground uses balanced body clips");
    // Discord: stickers rest on the panel; the inspected one gets a filled
    // backdrop, never an outline.
    bool idle_surface = false, selected_fill = false, outline = false;
    if (scene.cell_count >= 2) {
        const auto first_rect = scene.projection->cells[0].rectangle;
        const auto idle_rect = scene.projection->cells[1].rectangle;
        for (u32 i = 0; i < paint.count; ++i) {
            const auto& quad = paint.quads[i];
            idle_surface = idle_surface || (quad.left == idle_rect.left && quad.top == idle_rect.top &&
                                            quad.right == idle_rect.right && quad.bottom == idle_rect.bottom &&
                                            quad.fill.alpha > 0);
            const bool on_first = quad.left == first_rect.left && quad.top == first_rect.top &&
                               quad.right == first_rect.right && quad.bottom == first_rect.bottom;
            selected_fill = selected_fill || (on_first && quad.fill.alpha > 0);
            outline = outline || (on_first && quad.fill.alpha == 0 && quad.border_width == scene.style.selection_width);
        }
    }
    check(!idle_surface && selected_fill && !outline,
          "media picker: idle stickers rest on the panel and the selected one is filled without an outline");
    paint_stream_release(&paint);
    fixture.disabled_first = 60;
    fixture.disabled_count = 60;
    ++source.revision;
    check(ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor),
          "media picker: disabled catalog projection updates");
    navigation_calls = fixture.calls;
    check(!ui_media_picker_scene_edge_selection(&scene, false) && fixture.calls - navigation_calls == 60 &&
              !fixture.hidden_requested,
          "media picker: all-disabled catalog terminates exact expanded prefix without exposing hidden or "
          "disabled actions");
    ++content.account_scope;
    ++content.revision;
    check(!ui_media_picker_scene_update(&scene, &source, &content, viewport, anchor) &&
              !ui_media_picker_scene_cells(scene).count,
          "media picker: old account bank cannot expose previews or content in replacement account");
    ui_media_picker_scene_release(&scene);
    check(!scene.storage.arena.base && !scene.active && !scene.source && !scene.widgets.count,
          "media picker: explicit owner retirement releases widgets and borrowed media projections");
}
} // namespace maicord
