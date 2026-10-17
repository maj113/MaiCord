#include "maicord/ui/search_scene.hpp"
#include "maicord/ui/search_avatar_media.hpp"
#include "maicord/ui/theme_styles.hpp"
#include "maicord/ui/background_surface.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/app/client_theme.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/scene.hpp"
#include "maicord/ui/message_text_presentation.hpp"
#include "maicord/ui/navigation_view.hpp"

namespace maicord {
namespace {
struct SearchSceneFixture {
    u32 calls;
    bool variable = false;
    float invalid_height = 0;
    bool override_height = false;
    bool scope_current = true;
};
bool resolve_search_scene(void* opaque, u32 index, UiSearchRow* row) noexcept {
    ++static_cast<SearchSceneFixture*>(opaque)->calls;
    *row = {71,
            index + 101,
            utf8_literal("Original synthetic author"),
            utf8_literal("# native-search-fixture"),
            utf8_literal(
                "Search result body wraps within the right pane, preserving hit and context identities."),
            utf8_literal("Match 2026-10-03 12:30"),
            index % 3 == 1,
            index % 3 == 0};
    row->author_id = 900 + index;
    row->avatar_hash = utf8_literal("0123456789abcdef0123456789abcdef");
    const auto& fixture = *static_cast<SearchSceneFixture*>(opaque);
    if (fixture.variable)
        row->body_height = index % 2 ? 120.f : 24.f;
    if (fixture.override_height)
        row->body_height = fixture.invalid_height;
    return true;
}
bool choice_source_current(void* context) noexcept {
    return *static_cast<const bool*>(context);
}
void search_filter_choices(void (*check)(bool, const char*) noexcept) noexcept {
    UiSearchScene scene{};
    SearchSceneFixture fixture{};
    UiSearchSource results{&fixture, 24, 1, resolve_search_scene};
    UiSearchFilterChoiceView rows[kUiSearchFilterChoiceMaximum]{};
    for (u32 index = 0; index < kUiSearchFilterChoiceMaximum; ++index)
        rows[index] = {900 + index, index, utf8_literal("Original cached name"),
                       utf8_literal("Distinct identity detail"), utf8_literal("123456789")};
    rows[1].label = utf8_literal("Unicode café");
    bool live = true;
    UiSearchFilterChoiceSource choices{&live,
                                       {rows, kUiSearchFilterChoiceMaximum},
                                       1,
                                       kUiSearchFilterNoSelection,
                                       utf8_literal("From a user"),
                                       utf8_literal("Use Up and Down to select a cached name."),
                                       choice_source_current};
    UiSearchContent content{};
    content.edit.active = content.panel = content.suggestions = true;
    content.revision = 1;
    content.total = 24;
    content.phase = UiSearchPhase::ready;
    content.filter_choices = &choices;
    const UiRect viewport{0, 0, 360, 3300}, title{0, 0, 360, 48};
    bool valid = ui_search_scene_update(&scene, &results, &content, viewport, title);
    check(valid && scene.choice_total == 10 && scene.choice_count > 0 && scene.choice_count < 10 &&
              scene.widgets.count <= 48 && scene.count == 24 &&
              ui_widget_manager_focus(&scene.widgets, scene.edit),
          "search choice: bounded ten-choice source reuses existing widget bank with maximum result rows");
    bool no_prefix = true;
    for (u32 slot = 0; slot < scene.widgets.count; ++slot) {
        const auto action = scene.targets[slot].action;
        no_prefix = no_prefix &&
                    !(action >= UiSearchAction::filter_from && action <= UiSearchAction::filter_pinned) &&
                    (action != UiSearchAction::filter_value ||
                     !(scene.widgets.storage[slot].flags & ui_widget_focusable));
    }
    check(no_prefix, "search choice: contextual values replace fixed prefix suggestions");
    UiRect rectangle{}, clip{};
    const bool initial = ui_search_scene_filter_choice_geometry(scene, 0, &rectangle, &clip);
    check(initial && ui_rect_equal(clip, ui_rect_intersection(rectangle, scene.chooser)),
          "search choice: visible geometry clip belongs to the requested row instead of its ancestor "
          "menu");
    const auto old_target = initial ? ui_search_scene_hit_test(scene, (clip.left + clip.right) * .5f,
                                                               (clip.top + clip.bottom) * .5f)
                                    : UiSearchTarget{};
    check(initial && old_target.action == UiSearchAction::filter_value && old_target.source_index == 0 &&
              old_target.source_revision == 1 && !old_target.channel_id && !old_target.message_id &&
              ui_search_scene_target_current(scene, old_target),
          "search choice: pointer receipt carries exact source index and publication without invented "
          "result identity");
    bool reachable = true;
    for (u16 selected = 0; selected < 10; ++selected) {
        choices.selected = selected;
        ++choices.revision;
        reachable = reachable && ui_search_scene_update(&scene, &results, &content, viewport, title) &&
                    ui_search_scene_filter_choice_geometry(scene, selected, &rectangle, &clip) &&
                    ui_rect_equal(clip, rectangle) &&
                    ui_search_scene_hit_test(scene, (rectangle.left + rectangle.right) * .5f,
                                             (rectangle.top + rectangle.bottom) * .5f)
                            .source_index == selected &&
                    scene.widgets.focused.slot == scene.edit.slot;
    }
    check(reachable && scene.choice_first > 0 && scene.choice_first + scene.choice_count == 10 &&
              !ui_search_scene_target_current(scene, old_target),
          "search choice: all ten keyboard selections reveal full rows while editor focus and "
          "stale-publication guards remain");
    UiTextCommand commands[192]{};
    UiTextCommandStream stream{};
    UiTextOutput output{};
    PaintStream paint{};
    valid = paint_stream_initialize(&paint) && ui_text_command_stream_initialize(&stream, {commands, 192});
    output = {&stream, nullptr};
    valid = valid && paint_stream_reset(&paint, 360, 3300) &&
            ui_search_scene_paint(scene, kDefaultTheme, &paint, nullptr, 96) &&
            ui_search_scene_text_output(scene, kDefaultTheme, &output, nullptr);
    const u32 revision = scene.revision, layout = scene.widgets.layout.revision, calls = fixture.calls;
    const auto generation = scene.widgets.generation;
    const auto text_signature = stream.signature;
    const u32 text_count = stream.count, quad_count = paint.count;
    for (u32 frame = 0; valid && frame < 64; ++frame) {
        valid = ui_search_scene_update(&scene, &results, &content, viewport, title);
        ui_text_command_stream_reset(&stream);
        valid = valid && paint_stream_reset(&paint, 360, 3300) &&
                ui_search_scene_paint(scene, kDefaultTheme, &paint, nullptr, 96) &&
                ui_search_scene_text_output(scene, kDefaultTheme, &output, nullptr) &&
                stream.signature == text_signature && stream.count == text_count &&
                paint.count == quad_count;
    }
    check(valid && scene.revision == revision && scene.widgets.layout.revision == layout &&
              scene.widgets.generation == generation && fixture.calls == calls,
          "search choice: sixty-four unchanged frames preserve layout, row copies, paint count and text "
          "signature");
    live = false;
    ui_text_command_stream_reset(&stream);
    check(!ui_search_scene_current(scene) &&
              ui_search_scene_hit_test(scene, rectangle.left + 1, rectangle.top + 1).action ==
                  UiSearchAction::none &&
              !ui_search_scene_target_current(scene, old_target) &&
              ui_search_scene_text_output(scene, kDefaultTheme, &output, nullptr) && !stream.count,
          "search choice: revoked host source admits no stale text or pointer receipt");
    live = true;
    choices.selected = 0;
    ++choices.revision;
    const UiRect narrow{0, 0, 200, 240}, narrow_title{0, 0, 200, 48};
    valid =
        ui_search_scene_update(&scene, &results, &content, narrow, narrow_title) && scene.choice_count > 0;
    bool clipped = valid;
    for (u16 index = scene.choice_first; index < scene.choice_first + scene.choice_count; ++index)
        clipped = clipped && ui_search_scene_filter_choice_geometry(scene, index, &rectangle, &clip) &&
                  ui_rect_equal(ui_rect_intersection(rectangle, clip), rectangle) &&
                  rectangle.bottom <= scene.chooser.bottom && rectangle.right <= 200;
    check(clipped,
          "search choice: short narrow viewport contains every actionable row inside the native chooser");
    const auto barrier = ui_search_scene_hit_test(scene, scene.chooser.left + 1, scene.chooser.top + 1);
    check(barrier.action == UiSearchAction::none &&
              ui_search_scene_contains(scene, scene.chooser.left + 1, scene.chooser.top + 1),
          "search choice: nonactionable menu padding remains an input barrier over results");
    UiResolvedThemeStyles styles{};
    UiThemeTypography typography{};
    typography.body_size = 24;
    UiThemeMetrics metrics{};
    metrics.menu_padding = 12;
    metrics.menu_icon = 24;
    metrics.menu_gap = 12;
    metrics.menu_row_height = 44;
    metrics.menu_detail_height = 72;
    metrics.menu_radius = 14;
    metrics.control_height = 40;
    choices.selected = 9;
    ++choices.revision;
    valid = ui_theme_styles_apply(&styles, typography, metrics).ok() &&
            ui_search_scene_bind_theme_styles(&scene, &styles) &&
            ui_search_scene_update(&scene, &results, &content, {0, 0, 240, 480}, {0, 0, 240, 48}) &&
            ui_search_scene_filter_choice_geometry(scene, 9, &rectangle, &clip);
    check(valid && ui_rect_height(rectangle) >= 87 && scene.choice_count > 0 &&
              scene.choice_first + scene.choice_count == 10 &&
              ui_rect_equal(ui_rect_intersection(rectangle, clip), rectangle),
          "search choice: large typography and themed padding reserve complete detail rows in a narrow "
          "window");
    ui_text_command_stream_reset(&stream);
    valid = valid && ui_search_scene_text_output(scene, kLightTheme, &output, nullptr);
    bool text_clipped = valid;
    for (u32 index = 0; index < stream.count; ++index) {
        const auto& command = stream.storage[index];
        if (command.owner.handle.slot >= scene.widgets.count ||
            scene.targets[command.owner.handle.slot].action != UiSearchAction::filter_value)
            continue;
        text_clipped = text_clipped &&
                       ui_rect_equal(ui_rect_intersection(command.clip, scene.chooser), command.clip) &&
                       command.wrapping == UiTextWrapping::no_wrap;
    }
    check(text_clipped, "search choice: themed label and detail glyph commands retain exact chooser clips "
                        "and ellipsis policy");
    ui_search_scene_bind_theme_styles(&scene, nullptr);
    choices.hint = utf8_literal("No cached names match. Enter an ID.");
    choices.rows = {};
    choices.selected = kUiSearchFilterNoSelection;
    ++choices.revision;
    valid = ui_search_scene_update(&scene, &results, &content, narrow, narrow_title);
    no_prefix = true;
    for (u32 slot = 0; slot < scene.widgets.count; ++slot) {
        const auto action = scene.targets[slot].action;
        no_prefix = no_prefix && action != UiSearchAction::filter_value &&
                    !(action >= UiSearchAction::filter_from && action <= UiSearchAction::filter_pinned);
    }
    ui_text_command_stream_reset(&stream);
    valid = valid && ui_search_scene_text_output(scene, kDefaultTheme, &output, nullptr);
    bool hint = false;
    for (u32 index = 0; index < stream.count; ++index)
        hint = hint || utf8_equal(stream.storage[index].text, choices.hint);
    check(valid && scene.choice_count == 0 && no_prefix && hint,
          "search choice: empty recognized context shows truthful guidance and no prefix fallback actions");
    choices.rows = {rows, 11};
    ++choices.revision;
    check(!ui_search_scene_update(&scene, &results, &content, narrow, narrow_title) &&
              !ui_search_scene_current(scene),
          "search choice: oversized publication is rejected before reading rows");
    choices.rows = {nullptr, 1};
    ++choices.revision;
    check(!ui_search_scene_update(&scene, &results, &content, narrow, narrow_title),
          "search choice: missing row storage fails closed");
    choices.rows = {rows, 10};
    choices.selected = 10;
    ++choices.revision;
    check(!ui_search_scene_update(&scene, &results, &content, narrow, narrow_title),
          "search choice: selected index outside publication fails closed");
    choices.selected = 0;
    rows[0].label = {nullptr, 2};
    ++choices.revision;
    check(!ui_search_scene_update(&scene, &results, &content, narrow, narrow_title) && !scene.active,
          "search choice: new publication validates candidate UTF8 before admission");
    paint_stream_release(&paint);
    ui_search_scene_release(&scene);
}
void search_overlay_materials(void (*check)(bool, const char*) noexcept) noexcept {
    UiSearchScene scene{};
    UiSearchContent content{};
    content.edit.active = content.panel = true;
    content.revision = 1;
    PaintStream paint{}, expected{};
    bool valid = paint_stream_initialize(&paint) && paint_stream_initialize(&expected);
    for (u32 mode = 0; valid && mode < 4; ++mode) {
        content.suggestions = mode == 1;
        content.filters = mode == 2;
        content.sort_menu = mode == 3;
        ++content.revision;
        valid = ui_search_scene_update(&scene, nullptr, &content, {312, 0, 1200, 800}, {312, 0, 1200, 48});
        const u32 scene_revision = scene.revision, layout_revision = scene.widgets.layout.revision;
        const u16 generation = scene.widgets.generation;
        for (u32 variant = 0; valid && variant < 3; ++variant) {
            auto preferences = client_preferences_defaults();
            preferences.theme =
                variant == 2 ? ClientTheme::custom_light_gradient : ClientTheme::custom_gradient;
            preferences.gradient.intensity = 60;
            const Theme theme = variant ? client_theme_projection(preferences) : kDefaultTheme;
            valid = paint_stream_reset(&paint, 1200, 800) &&
                    ui_search_scene_paint(scene, theme, &paint, nullptr, 96);
            const auto material = [&](UiRect rectangle, bool panel) noexcept {
                const PaintQuad* quad = nullptr;
                for (u32 i = 0; i < paint.count; ++i)
                    if (ui_rect_equal({paint.quads[i].left, paint.quads[i].top, paint.quads[i].right,
                                       paint.quads[i].bottom},
                                      rectangle)) {
                        quad = &paint.quads[i];
                        break;
                    }
                if (!quad)
                    return false;
                if (variant)
                    return quad->border_width == -7 && quad->reserved_x == -1 &&
                           // Choosers float at popover elevation; trigger buttons keep their material.
                           quad->reserved_y ==
                               (panel ? ui_elevation_gradient_share(theme, UiElevation::popover)
                                      : (1 - ui_background_tier_opacity(theme, UiBackgroundTier::highest)) * .9f) &&
                           quad->radius == (panel ? 6.f : 8.f) && quad->fill.alpha == 1;
                return paint_stream_reset(&expected, 1200, 800) &&
                       ui_background_paint_surface(
                           &expected, rectangle, kDefaultTheme,
                           panel ? kDefaultTheme.surface_high : kDefaultTheme.surface,
                           panel ? kDefaultTheme.interactive : kDefaultTheme.surface_high,
                           panel ? 6.f : 8.f, 1) &&
                       expected.count == 1 && bytes_equal(quad, &expected.quads[0], sizeof(PaintQuad));
            };
            if (mode == 1 || mode == 2)
                valid = valid && material(scene.chooser, true);
            else {
                const auto* filter = ui_widget_manager_layout_node(scene.widgets, scene.buttons[2]);
                const auto* sort = ui_widget_manager_layout_node(scene.widgets, scene.buttons[8]);
                valid = valid && filter && sort && material(filter->rectangle, false) &&
                        material(sort->rectangle, false);
                if (mode == 3)
                    valid = valid && material(scene.sort_chooser, true);
            }
            valid = valid && paint.rejected == 0 && paint.clip_depth == 1 &&
                    scene.revision == scene_revision && scene.widgets.layout.revision == layout_revision &&
                    scene.widgets.generation == generation;
        }
    }
    check(valid, "search_scene_suggestions_filters_sort_and_trigger_controls_use_palette_material_without_"
                 "flat_or_layout_change");
    paint_stream_release(&paint);
    paint_stream_release(&expected);
    ui_search_scene_release(&scene);
}
} // namespace
namespace {
struct RichSearchFixture {
    MessageMediaPolicyResult media_policy;
    mutable MessageMediaOwner last_media_owner;
    MessageRecord records[30];
    u32 order[30];
    u8 model_text[8192];
    MessageRichRecord rich_records[60];
    u16 rich_order[60];
    u8 rich_text[32768];
    MessageHandle display_order[32];
    u32 heights[32];
    u64 fenwick[32];
    MessageStore store;
    MessageRichStore rich;
    UiVariableList list;
    MessageTimeline timeline;
    u32 resolves;
    bool current;
    MessageTextCommand commands[256];
    char display[8192];
    TextStyleSpan styles[256];
    MessageMarkdownSemantic semantics[128];
    MessageTextMeasurement measurements[48];
    MessageTextSpoilerReveal reveals[16];
    u32 scratch[8192];
};
RichSearchFixture rich_search_fixture;
MessageMediaPolicyResult rich_search_media_policy(const void* context,
                                                  const MessageMediaOwner& owner) noexcept {
    const auto& fixture = *static_cast<const RichSearchFixture*>(context);
    fixture.last_media_owner = owner;
    return fixture.media_policy;
}
bool rich_search_row(void* opaque, u32 index, UiSearchRow* output) noexcept {
    auto& f = *static_cast<RichSearchFixture*>(opaque);
    ++f.resolves;
    if (!output || index >= 32)
        return false;
    const auto* record = message_store_resolve(f.store, f.display_order[index]);
    if (!record)
        return false;
    *output = {record->channel_id,
               record->id,
               utf8_literal("Original author"),
               record->channel_id == 71 ? utf8_literal("# source-one") : utf8_literal("# source-two"),
               utf8_literal("Legacy excerpt must not replace the native rich source"),
               {},
               true,
               true};
    output->author_id = record->author_id;
    output->avatar_hash = utf8_literal("0123456789abcdef0123456789abcdef");
    return true;
}
u32 count_actions(const UiSearchScene& scene, UiSearchAction action, bool save = false) noexcept {
    u32 count = 0;
    for (u32 i = 1; i < scene.widgets.count; ++i)
        count += scene.targets[i].action == action && scene.targets[i].save == save;
    return count;
}
bool text_emitted(const UiSearchScene& scene, Utf8View expected) noexcept {
    static UiTextCommand commands[256];
    UiTextCommandStream stream{};
    UiTextOutput output{};
    if (!ui_text_command_stream_initialize(&stream, {commands, 256}))
        return false;
    output = {&stream, nullptr};
    if (!ui_search_scene_text_output(scene, kDefaultTheme, &output, nullptr))
        return false;
    for (u32 i = 0; i < stream.count; ++i)
        if (utf8_equal(commands[i].text, expected))
            return true;
    return false;
}
UiRect action_rectangle(const UiSearchScene& scene, UiSearchAction action, u32 nth = 0,
                        bool save = false) noexcept {
    for (u32 i = 1; i < scene.widgets.count; ++i) {
        if (scene.targets[i].action != action || scene.targets[i].save != save || nth--)
            continue;
        const auto* node =
            ui_widget_manager_layout_node(scene.widgets, {static_cast<u16>(i), scene.widgets.generation});
        return node ? node->rectangle : UiRect{};
    }
    return {};
}
void search_tab_layouts(void (*check)(bool, const char*) noexcept, UiSearchScene& search,
                        UiSearchSource& source, PaintStream& paint) noexcept {
    UiSearchContent content{};
    content.edit.active = content.panel = content.tabs = true;
    content.phase = UiSearchPhase::ready;
    content.total = 32;
    content.revision = 10;
    content.tab = content.result_tab = 1;
    content.tab_known = 0b00011;
    content.tab_totals[0] = 1234;
    content.tab_totals[1] = 5;
    const UiRect viewport{0, 0, 1200, 800}, title{0, 0, 1200, 48};
    search.scroll_dips = 0;
    bool ready = ui_search_scene_update(&search, &source, &content, viewport, title);
    UiSearchTarget tile{};
    UiRect cover{}, clip{};
    const bool tiled = ready && search.layout == UiSearchLayout::grid &&
                       ui_search_scene_visible_tile_count(search) == 2 &&
                       ui_search_scene_visible_tile(search, 0, &tile) &&
                       ui_search_scene_rich_preview_geometry(search, tile, &cover, &clip);
    check(
        tiled && tile.action == UiSearchAction::rich_item && tile.message_id == 129 && !tile.snapshot &&
            tile.rich_kind == MessageSceneRichItemKind::attachment && tile.rich_index == 0 &&
            ui_rect_width(clip) - ui_rect_height(clip) < .01f &&
            ui_rect_height(clip) - ui_rect_width(clip) < .01f &&
            ui_rect_width(cover) >= ui_rect_width(clip) && ui_search_scene_target_current(search, tile) &&
            ui_search_scene_hit_test(search, (clip.left + clip.right) * .5f, (clip.top + clip.bottom) * .5f)
                    .rich_index == 0,
        "search tabs: media grid tiles the previewable outer and forwarded attachments as square cover "
        "tiles");
    check(ready && search.count == 0 &&
              !ui_rect_has_area(ui_search_scene_content_clip(search, search.body)) &&
              count_actions(search, UiSearchAction::tab) == 5 &&
              text_emitted(search, utf8_literal("1.2k")) && text_emitted(search, utf8_literal("5")) &&
              text_emitted(search, utf8_literal("Media")),
          "search tabs: item layouts hide the message projection while the tab strip shows known counts");
    MessageMediaCommand commands[16]{};
    MessageMediaCommandStream media{};
    bool scoped = message_media_command_stream_initialize(&media, {commands, 16}) &&
                  paint_stream_reset(&paint, 1200, 800) &&
                  ui_search_scene_paint(search, kDefaultTheme, &paint, nullptr, 96, &media) &&
                  media.count == 2;
    for (u32 i = 0; scoped && i < media.count; ++i)
        scoped = commands[i].search_result && commands[i].owner.manager == &search.widgets &&
                 commands[i].kind == MessageMediaCommandKind::attachment &&
                 (commands[i].id == 500 || (commands[i].id == 502 && commands[i].snapshot));
    check(scoped, "search tabs: grid tiles emit scoped search media owned by their tile widgets");
    // Tiles round with the radius token: the placeholder plate and the media
    // mask (the cover rectangle's centred square) share theme.scale.radius_md.
    bool rounded = media.count == 2;
    for (u32 i = 0; rounded && i < media.count; ++i) {
        const auto r = commands[i].rectangle;
        const float side = ui_rect_width(r) < ui_rect_height(r) ? ui_rect_width(r) : ui_rect_height(r);
        rounded = commands[i].tile_radius == static_cast<u8>(kDefaultTheme.scale.radius_md) &&
                  side - ui_rect_width(clip) < .01f && ui_rect_width(clip) - side < .01f;
    }
    bool plate = false;
    for (u32 i = 0; i < paint.count; ++i)
        plate = plate || (ui_rect_equal({paint.quads[i].left, paint.quads[i].top, paint.quads[i].right,
                                         paint.quads[i].bottom},
                                        clip) &&
                          paint.quads[i].radius == kDefaultTheme.scale.radius_md);
    check(rounded && plate, "search tabs: media tiles and their media masks use the medium radius token");
    const auto tab = action_rectangle(search, UiSearchAction::tab, 2);
    const auto tab_target =
        ui_search_scene_hit_test(search, (tab.left + tab.right) * .5f, (tab.top + tab.bottom) * .5f);
    check(tab_target.action == UiSearchAction::tab && tab_target.source_index == 2,
          "search tabs: each tab is a pointer target carrying its tab index");

    auto& fixture = rich_search_fixture;
    auto policy = search.media_policy;
    const auto repaint_media = [&]() noexcept {
        return message_media_command_stream_initialize(&media, {commands, 16}) &&
               paint_stream_reset(&paint, 1200, 800) &&
               ui_search_scene_paint(search, kDefaultTheme, &paint, nullptr, 96, &media);
    };
    fixture.media_policy = {MessageExplicitFilter::blur, true};
    ++policy.revision;
    ready = ui_search_scene_set_media_policy(&search, policy) &&
            ui_search_scene_update(&search, &source, &content, viewport, title);
    check(ready && repaint_media() && media.count == 2 &&
              commands[0].obscure == static_cast<u8>(MessageMediaObscure::explicit_content) &&
              commands[1].obscure == static_cast<u8>(MessageMediaObscure::explicit_content) &&
              fixture.last_media_owner.forwarded && fixture.last_media_owner.channel_id == 72 &&
              !fixture.last_media_owner.author_id && fixture.last_media_owner.source_channel_id == 777 &&
              fixture.last_media_owner.source_message_id == 888,
          "search media policy: grid forwards retain destination and source context for outer and child "
          "media");
    auto* media_scene = ui_search_scene_message_scene(&search);
    check(media_scene &&
              message_scene_reveal_media(
                  media_scene, message_media_reveal_key(129, MessageMediaSource::attachment, 0)) &&
              repaint_media() && media.count == 2 && !commands[0].obscure &&
              commands[1].obscure == static_cast<u8>(MessageMediaObscure::explicit_content),
          "search media policy: grid shares native reveals without revealing the forwarded child");
    fixture.media_policy.explicit_reveal_allowed = false;
    ++policy.revision;
    ready = ui_search_scene_set_media_policy(&search, policy) &&
            ui_search_scene_update(&search, &source, &content, viewport, title);
    check(ready && repaint_media() && media.count == 2 &&
              commands[0].obscure == static_cast<u8>(MessageMediaObscure::explicit_content),
          "search media policy: changed eligibility re-obscures already revealed media on an unchanged "
          "publication");
    fixture.media_policy.filter = MessageExplicitFilter::block;
    ++policy.revision;
    ready = ui_search_scene_set_media_policy(&search, policy) &&
            ui_search_scene_update(&search, &source, &content, viewport, title);
    check(
        ready && repaint_media() && !media.count &&
            text_emitted(search, utf8_literal("Explicit content blocked")) &&
            ui_search_scene_hit_test(search, (clip.left + clip.right) * .5f, (clip.top + clip.bottom) * .5f)
                    .action == UiSearchAction::rich_item,
        "search media policy: blocked grid omits all media while preserving a consumed activation target");
    fixture.media_policy = {MessageExplicitFilter::show, true};
    ++policy.revision;
    ui_search_scene_set_media_policy(&search, policy);
    ready = ui_search_scene_update(&search, &source, &content, viewport, title);

    content.tab = content.result_tab = 2;
    ++content.revision;
    ready = ui_search_scene_update(&search, &source, &content, viewport, title);
    const auto save = action_rectangle(search, UiSearchAction::rich_item, 1, true);
    const auto saved =
        ui_search_scene_hit_test(search, (save.left + save.right) * .5f, (save.top + save.bottom) * .5f);
    check(
        ready && search.layout == UiSearchLayout::files &&
            count_actions(search, UiSearchAction::result) == 3 &&
            count_actions(search, UiSearchAction::rich_item, true) == 3 && saved.save &&
            saved.rich_index == 1 && text_emitted(search, utf8_literal("notes.txt")) &&
            text_emitted(search, utf8_literal("2 KB")) && !ui_search_scene_visible_tile_count(search),
        "search tabs: files rows list every attachment with name, size, a jump row and a download action");

    content.tab = content.result_tab = 3;
    ++content.revision;
    ready = ui_search_scene_update(&search, &source, &content, viewport, title);
    check(ready && search.layout == UiSearchLayout::links &&
              count_actions(search, UiSearchAction::result) > 4 &&
              text_emitted(search, utf8_literal("outer.example")) &&
              text_emitted(search, utf8_literal("https://outer.example/path")) &&
              ui_search_scene_scroll_maximum(search) > 0,
          "search tabs: links rows show each message's link and hostname and scroll as one list");
    const float before = search.scroll_dips;
    check(ui_search_scene_scroll(&search, 120) && search.scroll_dips == 120 && before == 0 &&
              ui_search_scene_message_scene(search) &&
              ui_search_scene_message_scene(search)->timeline->scroll_units == 0,
          "search tabs: item layouts own their scroll without moving the hidden projection");

    content.tab = 1;
    content.result_tab = 0;
    content.tabs = true;
    ++content.revision;
    search.scroll_dips = 0;
    ready = ui_search_scene_update(&search, &source, &content, viewport, title);
    check(ready && search.layout == UiSearchLayout::list && search.count > 0 &&
              search.body.top == search.panel.top + 144,
          "search tabs: a pending tab keeps the published message results until its page arrives");
}
void rich_search_scene(void (*check)(bool, const char*) noexcept) noexcept {
    auto& f = rich_search_fixture;
    f.resolves = 0;
    f.current = true;
    f.media_policy = {MessageExplicitFilter::show, true};
    constexpr auto body =
        utf8_literal("**Outer source** ||outer secret|| [outer](https://outer.example/path)");
    bool ready = message_store_initialize(&f.store, {f.records, 30}, {f.order, 30},
                                          {f.model_text, sizeof(f.model_text)}) &&
                 message_rich_store_initialize(&f.rich, {f.rich_records, 60}, {f.rich_order, 60},
                                               {f.rich_text, sizeof(f.rich_text)});
    for (u32 i = 0; ready && i < 30; ++i)
        ready = message_store_upsert(&f.store,
                                     {100 + i, 71 + i % 2, 9, 1700000000000ull + i * 1000, 0, body, 0})
                    .ok();
    f.display_order[0] = f.display_order[1] = message_store_find(f.store, 129).handle;
    for (u32 i = 0; i < 30; ++i)
        f.display_order[i + 2] = message_store_find(f.store, 100 + i).handle;
    const auto* first = message_store_resolve(f.store, f.display_order[0]);
    GatewayMessageAttachment image{}, file{}, quoted{};
    image.id = 500;
    image.flags = kGatewayMediaExplicitFlag;
    image.size = 128;
    image.filename = utf8_literal("original.png");
    image.content_type = utf8_literal("image/png");
    image.url = utf8_literal("https://cdn.discordapp.com/attachments/72/500/original.png");
    image.width = 96;
    image.height = 64;
    image.has_width = image.has_height = true;
    file.id = 501;
    file.size = 2048;
    file.filename = utf8_literal("notes.txt");
    file.content_type = utf8_literal("text/plain");
    file.url = utf8_literal("https://cdn.discordapp.com/attachments/72/501/notes.txt");
    quoted = image;
    quoted.id = 502;
    quoted.filename = utf8_literal("quoted.png");
    quoted.url = utf8_literal("https://cdn.discordapp.com/attachments/72/502/quoted.png");
    GatewayMessageAttachment attachments[]{image, file};
    GatewayMessageEvent event{};
    event.id = 129;
    event.fields = gateway_message_field_attachments | gateway_message_field_message_snapshots |
                   gateway_message_field_message_reference;
    event.reference.type = 1;
    event.reference.channel_id = 777;
    event.reference.message_id = 888;
    event.attachments = {attachments, 2};
    event.snapshot.present = true;
    event.snapshot.content =
        utf8_literal("**Frozen source** ||snapshot secret|| [inside](https://snapshot.example/path)");
    event.snapshot.fields = gateway_message_field_content | gateway_message_field_attachments;
    event.snapshot.attachments = {&quoted, 1};
    ready =
        ready && first && message_rich_store_apply(&f.rich, first->id, first->revision, event).ok() &&
        ui_variable_list_initialize(&f.list, {f.heights, 32}, {f.fenwick, 32}, 30, ui_extent_units(125)) &&
        message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(125),
                                    ui_extent_units(800), false) &&
        message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
        ui_variable_list_initialize(&f.list, {f.heights, 32}, {f.fenwick, 32}, 32, ui_extent_units(125));
    UiSearchSource source{&f, 32, 1, rich_search_row,
                          +[](void* p) noexcept { return static_cast<RichSearchFixture*>(p)->current; }};
    source.rich_timeline = &f.timeline;
    source.rich_order = {f.display_order, 32};
    UiSearchContent content{};
    content.edit.active = content.panel = true;
    content.phase = UiSearchPhase::ready;
    content.total = 32;
    content.revision = 1;
    UiSearchScene search{};
    check(ui_search_scene_set_media_policy(&search, {&f, rich_search_media_policy, 1}) &&
              !search.rich_projection,
          "search media policy: binding precedes lazy native projection");
    ready =
        ready && ui_search_scene_update(&search, &source, &content, {0, 0, 1200, 800}, {0, 0, 1200, 48});
    auto* native = ui_search_scene_message_scene(&search);
    check(ready && native && native->result_cards && native->count <= 24 &&
              native->rows[0].message_id == 129 && native->rows[1].message_id == 129 &&
              message_store_find(f.store, 100).ok() && message_scene_source_count(*native) == 32 &&
              f.store.count == 30 && native->timeline->list->count == 32,
          "search rich: one retained projection preserves ranked repeated contexts and canonical model "
          "lookup");
    if (!ready || !native) {
        ui_search_scene_release(&search);
        return;
    }
    const auto& outer = native->rows[0];
    const auto* child = message_scene_snapshot_row(*native, outer);
    UiSearchTarget image_target{UiSearchAction::rich_item, 72, 129};
    image_target.source_revision = 1;
    image_target.source_index = 0;
    UiRect image_rect{}, image_clip{};
    const bool image_geometry =
        ui_search_scene_rich_preview_geometry(search, image_target, &image_rect, &image_clip);
    auto quote_target = image_target;
    quote_target.snapshot = true;
    UiRect quote_rect{}, quote_clip{};
    const bool quote_geometry =
        ui_search_scene_rich_preview_geometry(search, quote_target, &quote_rect, &quote_clip);
    const auto* jump = ui_widget_manager_layout_node(search.widgets, search.rows[0]);
    check(image_geometry && quote_geometry && child && jump && image_rect.top != quote_rect.top &&
              ui_search_scene_hit_test(search, image_clip.left + 1, image_clip.top + 1).action ==
                  UiSearchAction::rich_item &&
              !ui_search_scene_hit_test(search, image_clip.left + 1, image_clip.top + 1).snapshot &&
              ui_search_scene_hit_test(search, quote_clip.left + 1, quote_clip.top + 1).snapshot &&
              ui_search_scene_hit_test(search, (jump->rectangle.left + jump->rectangle.right) * .5f,
                                       (jump->rectangle.top + jump->rectangle.bottom) * .5f)
                      .action == UiSearchAction::result &&
              ui_search_scene_hit_test(search, search.row_rectangles[0].left + 4,
                                       search.row_rectangles[0].top + 4)
                      .action == UiSearchAction::none,
          "search rich: outer media snapshot media and explicit Jump are independent clipped pointer "
          "targets");
    check(ui_search_scene_rich_widget_target(
              search, message_scene_rich_item_widget(outer, MessageSceneRichItemKind::attachment, 0))
                      .action == UiSearchAction::rich_item &&
              ui_search_scene_rich_widget_target(search, outer.widget).action == UiSearchAction::none,
          "search rich: keyboard rich item ownership cannot fall through into card Jump");
    bool save_owned = false;
    for (u32 i = 0; i < search.widgets.count; ++i) {
        const auto target = search.targets[i];
        if (!target.save || target.source_index != 0 || target.snapshot || target.rich_index != 1)
            continue;
        const auto handle = UiWidgetHandle{static_cast<u16>(i), search.widgets.generation};
        const auto* node = ui_widget_manager_layout_node(search.widgets, handle);
        save_owned =
            node && ui_widget_manager_focus(&search.widgets, handle) &&
            ui_search_scene_target_current(search, target) &&
            ui_search_scene_hit_test(search, node->rectangle.left + 1, node->rectangle.top + 1).save;
    }
    check(save_owned, "search rich: painted file Save has a distinct keyboard and pointer receipt instead "
                      "of Open or Jump");
    MessageTextPresentation p{};
    ChatStateStore state{};
    NavigationView navigation{};
    const bool text_ready =
        message_text_presentation_initialize(&p, {f.commands, 256}, {f.display, 8192}, {f.styles, 256},
                                             {f.semantics, 128}, {f.measurements, 48}, {f.reveals, 16},
                                             {f.scratch, 8192}) &&
        message_text_presentation_build(&p, *native, state, nullptr, navigation, kDefaultTheme, 1700000000)
            .ok();
    const MessageTextCommand* outer_command = nullptr;
    const MessageTextCommand* child_command = nullptr;
    for (u32 i = 0; text_ready && i < p.command_count; ++i) {
        if (p.commands[i].owner.handle.slot == outer.content.slot)
            outer_command = &p.commands[i];
        if (child && p.commands[i].owner.handle.slot == child->content.slot)
            child_command = &p.commands[i];
    }
    bool outer_hidden = false, child_hidden = false, distinct_links = false;
    for (u32 i = 0; outer_command && i < outer_command->styles.count; ++i)
        outer_hidden |= (outer_command->styles[i].flags & text_style_spoiler) != 0;
    for (u32 i = 0; child_command && i < child_command->styles.count; ++i)
        child_hidden |= (child_command->styles[i].flags & text_style_spoiler) != 0;
    for (u32 i = 0; child_command && i < child_command->semantics.count; ++i) {
        const auto& semantic = child_command->semantics[i];
        if (semantic.kind != MessageMarkdownSemanticKind::link)
            continue;
        const MessageTextHit hit{129,
                                 semantic.target,
                                 first->revision,
                                 semantic.source_offset,
                                 semantic.source_bytes,
                                 semantic.kind,
                                 true};
        distinct_links = utf8_equal(message_text_presentation_hit_source(f.store, &f.rich, hit),
                                    utf8_literal("https://snapshot.example/path"));
    }
    check(
        text_ready && outer_command && child_command && outer_hidden && child_hidden && distinct_links &&
            utf8_equal(message_text_presentation_command_source(f.store, &f.rich, *outer_command), body) &&
            utf8_equal(message_text_presentation_command_source(f.store, &f.rich, *child_command),
                       event.snapshot.content),
        "search rich: full native markdown source spoilers and snapshot links retain independent exact "
        "ownership");
    PaintStream paint{};
    MessageMediaCommand commands[32]{};
    MessageMediaCommandStream media{};
    bool painted = paint_stream_initialize(&paint) &&
                   message_media_command_stream_initialize(&media, {commands, 32}) &&
                   paint_stream_reset(&paint, 1200, 800) &&
                   ui_search_scene_paint(search, kDefaultTheme, &paint, nullptr, 96, &media);
    bool scoped_outer = false, scoped_child = false;
    bool exact_publication = true;
    for (u32 i = 0; painted && i < media.count; ++i) {
        exact_publication &=
            !media.storage[i].search_result || media.storage[i].message_revision == first->revision;
        scoped_outer |=
            media.storage[i].search_result && !media.storage[i].snapshot && media.storage[i].id == 500;
        scoped_child |=
            media.storage[i].search_result && media.storage[i].snapshot && media.storage[i].id == 502;
    }
    check(painted && !paint.rejected && paint.clip_depth == 1 && scoped_outer && scoped_child &&
              exact_publication,
          "search rich: native rich geometry emits scoped outer and snapshot media commands without a "
          "duplicate renderer");
    ui_search_scene_refresh_rich(&search);
    const u32 resolves = f.resolves, layouts = native->layout_passes,
              preparations = native->preparation_passes, widgets = native->widgets.count,
              revision = search.revision;
    bool warm = true;
    for (u32 i = 0; i < 64; ++i)
        warm &= ui_search_scene_update(&search, &source, &content, {0, 0, 1200, 800}, {0, 0, 1200, 48}) &&
                ui_search_scene_refresh_rich(&search);
    check(warm && resolves == f.resolves && layouts == native->layout_passes &&
              preparations == native->preparation_passes && widgets == native->widgets.count &&
              revision == search.revision,
          "search rich: 64 unchanged frames resolve allocate and relayout no result projection");
    f.current = false;
    check(!ui_search_scene_target_current(search, image_target) && !ui_search_scene_message_scene(search) &&
              !ui_search_scene_rich_preview_geometry(search, image_target, &image_rect, &image_clip),
          "search rich: source retirement fences retained native targets text and media geometry");
    f.current = true;
    ++source.revision;
    check(ui_search_scene_update(&search, &source, &content, {0, 0, 320, 480}, {0, 0, 320, 48}) &&
              !ui_search_scene_target_current(search, image_target) &&
              ui_rect_width(ui_search_scene_message_scene(search)->viewport) > 200,
          "search rich: narrow source republication preserves usable geometry and retires prior pointer "
          "receipts");
    SearchAvatarMedia avatar{};
    const auto* narrow_native = ui_search_scene_message_scene(search);
    check(narrow_native && search_avatar_media_resolve(search, 0, &avatar) &&
              avatar.owner.manager == &narrow_native->widgets &&
              avatar.owner.handle.slot == narrow_native->rows[0].avatar.slot &&
              ui_rect_height(avatar.rectangle) == 40,
          "search rich: avatar keeps native header ownership even when the independent Jump footer is "
          "clipped");
    check(ui_search_scene_scroll(&search, ui_search_scene_scroll_maximum(search)) && search.first > 0 &&
              ui_search_scene_message_scene(search)->count <= kUiSearchVisibleRows,
          "search rich: scrolling reaches the ranked page tail with bounded materialized rich rows");
    search_tab_layouts(check, search, source, paint);
    paint_stream_release(&paint);
    ui_search_scene_release(&search);
    check(!search.rich_projection && !search.rich_projection_storage.base,
          "search rich: release retires the single lazy projection and all borrowed source pointers");
}
void rich_search_gallery(void (*check)(bool, const char*) noexcept) noexcept {
    auto& f = rich_search_fixture;
    f.current = true;
    f.media_policy = {MessageExplicitFilter::blur, true};
    bool ready = message_store_initialize(&f.store, {f.records, 30}, {f.order, 30},
                                          {f.model_text, sizeof(f.model_text)}) &&
                 message_rich_store_initialize(&f.rich, {f.rich_records, 60}, {f.rich_order, 60},
                                               {f.rich_text, sizeof(f.rich_text)});
    const auto inserted = message_store_upsert(&f.store, {129, 72, 9, 0, 0, {}});
    const auto* record = inserted.ok() ? message_store_resolve(f.store, inserted.handle) : nullptr;
    GatewayMessageEmbed embeds[3]{};
    for (auto& embed : embeds) {
        embed.type = GatewayMessageEmbedType::rich;
        embed.url = utf8_literal("https://gallery.example/post");
    }
    embeds[0].description = utf8_literal("Gallery caption");
    embeds[1].image.url = utf8_literal("https://gallery.example/explicit.png");
    embeds[1].image.width = embeds[1].image.height = 300;
    embeds[1].flags = kGatewayMediaExplicitFlag;
    embeds[2].image.url = utf8_literal("https://gallery.example/clear.png");
    embeds[2].image.width = embeds[2].image.height = 300;
    GatewayMessageEvent event{};
    event.id = 129;
    event.fields = gateway_message_field_embeds | gateway_message_field_message_reference |
                   gateway_message_field_message_snapshots;
    event.embeds = {embeds, 3};
    event.reference.type = 1;
    event.reference.channel_id = 777;
    event.reference.message_id = 888;
    event.snapshot.present = true;
    event.snapshot.fields = gateway_message_field_embeds;
    event.snapshot.embeds = {embeds, 3};
    ready = ready && record && message_rich_store_apply(&f.rich, 129, record->revision, event).ok() &&
            ui_variable_list_initialize(&f.list, {f.heights, 32}, {f.fenwick, 32}, 1, ui_extent_units(57)) &&
            message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(57),
                                        ui_extent_units(1600), false) &&
            message_timeline_bind_rich_store(&f.timeline, &f.rich);
    f.display_order[0] = inserted.handle;
    UiSearchSource source{&f, 1, 1, rich_search_row};
    source.rich_timeline = &f.timeline;
    source.rich_order = {f.display_order, 1};
    UiSearchContent content{};
    content.edit.active = content.panel = true;
    content.phase = UiSearchPhase::ready;
    content.revision = 1;
    UiSearchScene search{};
    ui_search_scene_set_media_policy(&search, {&f, rich_search_media_policy, 1});
    ready = ready && ui_search_scene_update(&search, &source, &content, {0, 0, 1200, 1600},
                                           {0, 0, 1200, 48});
    auto* native = ui_search_scene_message_scene(&search);
    const auto* outer = native ? message_scene_find_row(*native, 0) : nullptr;
    bool members = ready && outer, keyboard = members, gaps = members;
    for (u32 scope = 0; members && scope < 2; ++scope) {
        const auto* row = scope ? message_scene_snapshot_row(*native, *outer) : outer;
        keyboard = keyboard && row;
        if (!row) {
            members = false;
            break;
        }
        const auto host_widget = message_scene_rich_item_widget(*row, MessageSceneRichItemKind::embed, 0);
        keyboard = keyboard && ui_widget_manager_focus(&native->widgets, host_widget);
        const auto focused = ui_search_scene_rich_widget_target(search, native->widgets.focused);
        keyboard = keyboard && focused.rich_index == 1 && focused.snapshot == (scope != 0) &&
                   focused.resource == MediaEmbedResource::image;
        UiRect cells[2]{};
        for (u32 tile = 0; tile < 2; ++tile) {
            UiSearchTarget target{UiSearchAction::rich_item, 72, 129};
            target.source_revision = 1;
            target.source_index = 0;
            target.rich_kind = MessageSceneRichItemKind::embed;
            target.rich_index = static_cast<u16>(tile + 1);
            target.snapshot = scope != 0;
            target.resource = MediaEmbedResource::image;
            UiRect clip{};
            const bool geometry = ui_search_scene_rich_preview_geometry(search, target, &cells[tile], &clip);
            const auto hit = ui_search_scene_hit_test(search, (clip.left + clip.right) * .5f,
                                                     (clip.top + clip.bottom) * .5f);
            members = members && geometry && ui_rect_has_area(clip) && ui_search_target_equal(hit, target);
        }
        const auto gap = ui_search_scene_hit_test(search, (cells[0].right + cells[1].left) * .5f,
                                                 (cells[0].top + cells[0].bottom) * .5f);
        gaps = gaps && gap.action == UiSearchAction::none;
    }
    check(members, "search gallery: pointer and preview geometry preserve each merged member and snapshot");
    check(keyboard, "search gallery: keyboard host targets the first actual image of an imageless host");
    check(gaps, "search gallery: spacing gaps cannot activate an image or fall through to the host URL");
    ui_search_scene_release(&search);
}
} // namespace
void test_search_scene(void (*check)(bool, const char*) noexcept) noexcept {
    search_overlay_materials(check);
    search_filter_choices(check);
    rich_search_scene(check);
    rich_search_gallery(check);
    UiSearchScene scene{};
    SearchSceneFixture fixture{};
    UiSearchSource source{&fixture, 125, 1, resolve_search_scene, +[](void* opaque) noexcept {
                              return static_cast<SearchSceneFixture*>(opaque)->scope_current;
                          }};
    UiSearchContent content{};
    content.edit = {utf8_literal("from:alice release"), 0, 18, true, true, true};
    content.total = 987;
    content.phase = UiSearchPhase::ready;
    content.status = utf8_literal("Click a message to jump to its conversation.");
    content.heading = utf8_literal("Search Original Guild");
    content.placeholder = utf8_literal("Search Original Guild");
    content.panel = true;
    content.next = true;
    content.revision = 1;
    const UiRect viewport{312, 0, 1200, 800}, title{312, 0, 1200, 48};
    check(!scene.storage.arena.base && ui_search_scene_update(&scene, &source, &content, viewport, title) &&
              scene.active && scene.count < 10 && fixture.calls == 125 + scene.count,
          "search_scene_lazy_bounded_height_index_and_visible_projection_125_contexts");
    if (!scene.active)
        return;
    SearchAvatarMedia avatar{};
    check(search_avatar_media_resolve(scene, 0, &avatar) && avatar.key.id == 900 &&
              avatar.key.variant == user_avatar_variant(scene.visible[0].avatar_hash) &&
              avatar.owner.manager == &scene.widgets &&
              ui_rect_equal(avatar.rectangle,
                            {scene.row_rectangles[0].left + 12, scene.row_rectangles[0].top + 12,
                             scene.row_rectangles[0].left + 44, scene.row_rectangles[0].top + 44}) &&
              search_avatar_media_current(scene, avatar.key),
          "search_scene_avatar_uses_exact_result_author_hash_and_borrowed_clipped_widget");
    // Close is a square icon button; its hit target is the whole square.
    const auto close = ui_search_close_rectangle(scene.panel);
    check(ui_rect_width(close) == kUiSearchCloseExtent && ui_rect_height(close) == kUiSearchCloseExtent &&
              close.right == scene.panel.right - 8 &&
              ui_search_scene_hit_test(scene, close.left + 2, close.bottom - 2).action == UiSearchAction::close,
          "search_scene_close_is_a_32_dip_square_icon_button");
    // Result headers in a 360-DIP pane: the timestamp keeps its full width and
    // follows a short name; a long name ellipsizes first.
    {
        const float left = 60, right = 60 + 260;
        const auto short_name = message_result_card_header_layout(left, right, 40, 120);
        const auto long_name = message_result_card_header_layout(left, right, 400, 120);
        const auto narrow = message_result_card_header_layout(left, left + 100, 400, 160);
        check(short_name.timestamp_width == 120 && short_name.name.right == left + 41 &&
                  short_name.timestamp.left == short_name.name.right + kMessageResultCardHeaderGap &&
                  long_name.timestamp_width == 120 && long_name.timestamp.right == right &&
                  long_name.timestamp.left == right - 120 &&
                  long_name.name.right == long_name.timestamp.left - kMessageResultCardHeaderGap &&
                  narrow.name.right - narrow.name.left >= kMessageResultCardNameMinimum &&
                  narrow.timestamp.right == left + 100 &&
                  message_result_card_header_layout(left, right, 40, 0).timestamp_width == 0,
              "search_scene_result_header_timestamp_fits_and_long_names_truncate_first");
    }
    fixture.scope_current = false;
    check(!search_avatar_media_current(scene, avatar.key) &&
              ui_search_scene_hit_test(scene, scene.field.left + 8, scene.field.top + 8).action ==
                  UiSearchAction::none,
          "search_scene_account_scope_retirement_rejects_media_and_hits_without_publication");
    fixture.scope_current = true;
    PaintStream paint{};
    bool flat = paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1200, 800) &&
                ui_search_scene_paint(scene, kDefaultTheme, &paint, nullptr, 96);
    bool fallback = false;
    for (u32 i = 0; flat && i < paint.count; ++i) {
        const auto& quad = paint.quads[i];
        flat &= quad.border_width != -3 && quad.border_width != -7 && quad.border_width != -8;
        fallback |= ui_rect_equal({quad.left, quad.top, quad.right, quad.bottom}, avatar.rectangle) &&
                    quad.fill.red == kDefaultTheme.avatar_placeholder.red && quad.fill.alpha == 1;
    }
    check(flat && fallback,
          "search_scene_flat_theme_retains_native_materials_and_neutral_avatar_until_resident");
    paint_stream_release(&paint);
    const auto gutter =
        ui_scrollbar_geometry(scene.body, scene.scroll_dips, scene.row_offsets[scene.measured_count]);
    check(gutter.visible && scene.row_rectangles[0].right <= gutter.track.left &&
              ui_search_body_text_width(scene) ==
                  scene.row_rectangles[0].right - 12 - (scene.row_rectangles[0].left + 52),
          "search_scene_cards_and_measured_body_keep_the_shared_scrollbar_gutter_disjoint");
    const u32 calls = fixture.calls, revision = scene.widgets.layout.revision;
    const auto base = scene.storage.arena.base;
    check(ui_search_scene_update(&scene, &source, &content, viewport, title) && fixture.calls == calls &&
              scene.widgets.layout.revision == revision && scene.storage.arena.base == base,
          "search_scene_unchanged_update_no_resolve_layout_allocation");
    ++content.revision;
    content.edit.caret_visible = false;
    check(ui_search_scene_update(&scene, &source, &content, viewport, title) && fixture.calls == calls &&
              scene.widgets.layout.revision == revision,
          "search_scene_caret_only_update_keeps_retained_widgets_and_source");
    const auto first = scene.rows[0];
    const auto* row = ui_widget_manager_layout_node(scene.widgets, first);
    const auto hit =
        row ? ui_search_scene_hit_test(scene, row->rectangle.left + 10, row->rectangle.top + 10)
            : UiSearchTarget{};
    check(hit.action == UiSearchAction::result && hit.channel_id == 71 && hit.message_id == 101,
          "search_scene_result_activation_contains_actual_source_identity");
    check(ui_search_scene_contains(scene, scene.panel.right - 4, scene.panel.bottom - 4) &&
              !ui_search_scene_contains(scene, scene.panel.left - 4, scene.panel.top + 150),
          "search_scene_occlusion_owns_actual_panel_geometry_only");
    check(ui_search_scene_scroll(&scene, ui_search_scene_scroll_maximum(scene)) && scene.first > 100 &&
              scene.count < 10 && scene.scroll_dips == ui_search_scene_scroll_maximum(scene),
          "search_scene_scroll_window_is_bounded_and_clamped");
    UiTextCommand commands[128]{};
    UiTextCommandStream stream{};
    UiTextOutput output{&stream, nullptr};
    check(ui_text_command_stream_initialize(&stream, {commands, 128}) &&
              ui_search_scene_text_output(scene, kDefaultTheme, &output, nullptr),
          "search_scene_native_text_stream_emits_author_channel_body_context");
    bool status_visible = false;
    bool scoped_heading = false, wrapped_excerpt = false;
    for (u32 i = 0; i < stream.count; ++i) {
        status_visible |= commands[i].text.data == content.status.data;
        scoped_heading |= commands[i].text.data == content.heading.data;
        if (commands[i].owner.handle.generation &&
            scene.targets[commands[i].owner.handle.slot].action == UiSearchAction::result &&
            commands[i].text.data == scene.visible[0].body.data)
            wrapped_excerpt |= commands[i].wrapping == UiTextWrapping::wrap &&
                               commands[i].rectangle.bottom - commands[i].rectangle.top == 48 &&
                               commands[i].clip.top >= scene.body.top &&
                               commands[i].clip.bottom <= scene.body.bottom;
    }
    check(scoped_heading && wrapped_excerpt,
          "search_scene_scoped_heading_and_compact_wrapped_excerpt_remain_clipped_to_results_viewport");
    content.offset = 50;
    content.total_estimated = true;
    ++content.revision;
    check(ui_search_scene_update(&scene, &source, &content, viewport, title) && scene.page_count == 5 &&
              bytes_equal(scene.count_text, "About ", 6) && scene.storage.arena.base == base &&
              scene.widgets.count <= 48,
          "search_scene_estimated_total_and_numbered_pagination_use_bounded_retained_storage");
    const auto* page = ui_widget_manager_layout_node(scene.widgets, scene.pages[3]);
    const auto page_hit =
        page ? ui_search_scene_hit_test(scene, (page->rectangle.left + page->rectangle.right) * .5f,
                                        (page->rectangle.top + page->rectangle.bottom) * .5f)
             : UiSearchTarget{};
    check(page_hit.action == UiSearchAction::page && page_hit.page_offset == 75 &&
              !ui_search_target_equal({UiSearchAction::page, 0, 0, 50}, {UiSearchAction::page, 0, 0, 75}),
          "search_scene_numbered_page_hits_carry_exact_offset_without_message_identity_aliasing");
    check(ui_text_command_stream_initialize(&stream, {commands, 128}) &&
              ui_search_scene_text_output(scene, kDefaultTheme, &output, nullptr),
          "search_scene_page_projection_is_current");
    u32 centered_pages = 0;
    for (u32 i = 0; i < stream.count; ++i) {
        const auto owner = commands[i].owner.handle;
        if (owner.generation && scene.targets[owner.slot].action == UiSearchAction::page)
            centered_pages += commands[i].horizontal_alignment == UiTextAlignment::center &&
                              commands[i].vertical_alignment == UiTextAlignment::center;
    }
    const auto* first_page = ui_widget_manager_layout_node(scene.widgets, scene.pages[0]);
    const auto* last_page = ui_widget_manager_layout_node(scene.widgets, scene.pages[scene.page_count - 1]);
    check(centered_pages == scene.page_count && first_page && last_page &&
              first_page->rectangle.left + last_page->rectangle.right ==
                  scene.panel.left + scene.panel.right,
          "search_scene_pagination_group_and_measured_native_text_are_centered_without_character_guessing");
    content.sort = 1;
    content.sort_menu = true;
    ++content.revision;
    check(ui_search_scene_update(&scene, &source, &content, viewport, title) &&
              scene.targets[scene.buttons[4].slot].action == UiSearchAction::oldest,
          "search_scene_sort_controls_retain_distinct_native_actions");
    const auto* sort_option = ui_widget_manager_layout_node(scene.widgets, scene.buttons[4]);
    check(
        sort_option && sort_option->rectangle.left >= scene.sort_chooser.left &&
            sort_option->rectangle.bottom <= scene.sort_chooser.bottom &&
            ui_search_scene_hit_test(scene, sort_option->rectangle.left + 8, sort_option->rectangle.top + 8)
                    .action == UiSearchAction::oldest,
        "search_scene_checked_sort_popup_has_bounded_options_and_real_sort_hit");
    check(ui_text_command_stream_initialize(&stream, {commands, 128}) &&
              ui_search_scene_text_output(scene, kDefaultTheme, &output, nullptr),
          "search_scene_sort_popup_text_projection_succeeds");
    bool left_author_retained = false, sort_clip_clean = true;
    for (u32 i = 0; i < stream.count; ++i) {
        const auto h = commands[i].owner.handle;
        if (h.generation && scene.targets[h.slot].action == UiSearchAction::result) {
            sort_clip_clean &=
                !ui_rect_has_area(ui_rect_intersection(commands[i].clip, scene.sort_chooser));
            left_author_retained |=
                commands[i].text.data == scene.visible[0].author.data && ui_rect_has_area(commands[i].clip);
        }
    }
    check(left_author_retained && sort_clip_clean,
          "search_scene_sort_menu_occludes_right_text_without_erasing_visible_author_left_of_popup");
    ++source.revision;
    check(ui_search_scene_hit_test(scene, scene.field.left + 10, scene.field.top + 10).action ==
              UiSearchAction::none,
          "search_scene_source_revision_guard_rejects_stale_activation");
    ++content.revision;
    content.sort_menu = false;
    content.filters = true;
    check(ui_search_scene_update(&scene, &source, &content, viewport, title) &&
              ui_text_command_stream_initialize(&stream, {commands, 128}) &&
              ui_search_scene_text_output(scene, kDefaultTheme, &output, nullptr),
          "search_scene_filter_chooser_projects_real_actions");
    bool chooser_clean = true, status_hidden = true;
    for (u32 i = 0; i < stream.count; ++i) {
        const auto owner = commands[i].owner.handle;
        if (owner.generation && scene.targets[owner.slot].action == UiSearchAction::result)
            chooser_clean &= !ui_rect_has_area(ui_rect_intersection(commands[i].clip, scene.chooser));
        status_hidden &= commands[i].text.data != content.status.data;
    }
    check(chooser_clean, "search_scene_chooser_occludes_result_glyphs");
    bool avatars_clean = true;
    for (u32 i = 0; i < scene.count; ++i) {
        SearchAvatarMedia media{};
        if (search_avatar_media_resolve(scene, i, &media))
            avatars_clean &= !ui_rect_has_area(ui_rect_intersection(media.clip, scene.chooser));
    }
    check(avatars_clean, "search_scene_chooser_occludes_profile_media_using_the_same_actual_popup_bounds");
    check(status_visible && status_hidden, "search_scene_chooser_suppresses_overlapping_status_glyphs");
    content.filters = false;
    content.suggestions = true;
    ++content.revision;
    check(ui_search_scene_update(&scene, &source, &content, viewport, title) &&
              scene.chooser.top == scene.field.bottom + 8 && scene.chooser.right <= viewport.right,
          "search_scene_five_suggestions_are_anchored_to_query_not_results_header");
    u32 suggestions = 0;
    bool more = false;
    for (u32 i = 1; i < scene.widgets.count; ++i) {
        const auto a = scene.targets[i].action;
        suggestions += (a >= UiSearchAction::filter_from && a <= UiSearchAction::filter_mentions) ||
                       a == UiSearchAction::more_filters;
        more |= a == UiSearchAction::more_filters;
    }
    check(suggestions == 5 && more,
          "search_scene_initial_suggestions_offer_four_prefixes_and_honest_more_filters");
    bool covered_rows_unfocusable = true;
    for (u32 i = 0; i < scene.count; ++i)
        covered_rows_unfocusable &=
            (scene.widgets.storage[scene.rows[i].slot].flags & ui_widget_focusable) == 0;
    check(covered_rows_unfocusable, "search_scene_open_suggestions_do_not_keyboard_focus_covered_results");
    UiSearchScene variable{};
    SearchSceneFixture variable_fixture{};
    variable_fixture.variable = true;
    UiSearchSource variable_source{&variable_fixture, 6, 1, resolve_search_scene};
    UiSearchContent variable_content = content;
    variable_content.suggestions = false;
    variable_content.filters = false;
    check(ui_search_scene_update(&variable, &variable_source, &variable_content, viewport, title) &&
              ui_rect_height(variable.row_rectangles[0]) == 72 &&
              ui_rect_height(variable.row_rectangles[1]) == 168 &&
              variable.group_rectangles[0].bottom == variable.row_rectangles[0].top &&
              variable.row_offsets[2] == 284,
          "search_scene_measured_body_heights_produce_compact_variable_cards_and_separate_group_heading");
    const auto indexed_calls = variable_fixture.calls;
    check(ui_search_scene_scroll(&variable, 132) && variable.first == 1 &&
              variable_fixture.calls <= indexed_calls + 6,
          "search_scene_variable_height_scroll_uses_retained_prefix_index_and_visible_resolves");
    ui_search_scene_release(&variable);
    variable_fixture.invalid_height = 465;
    variable_fixture.override_height = true;
    ++variable_source.revision;
    check(!ui_search_scene_update(&variable, &variable_source, &variable_content, viewport, title),
          "search_scene_rejects_overmaximum_authority_body_height");
    ui_search_scene_release(&variable);
    variable_fixture.invalid_height = -1;
    ++variable_source.revision;
    check(!ui_search_scene_update(&variable, &variable_source, &variable_content, viewport, title),
          "search_scene_rejects_negative_authority_body_height");
    ui_search_scene_release(&variable);
    variable_fixture.override_height = false;
    ++variable_source.revision;
    check(ui_search_scene_update(&variable, &variable_source, &variable_content, viewport, title),
          "search_scene_nonfinite_replacement_fixture_has_live_prior_geometry");
    variable_fixture.override_height = true;
    const u32 nan_bits = 0x7fc00000u;
    copy_bytes(&variable_fixture.invalid_height, &nan_bits, sizeof(nan_bits));
    ++variable_source.revision;
    check(!ui_search_scene_update(&variable, &variable_source, &variable_content, viewport, title) &&
              !variable.active &&
              ui_search_scene_hit_test(variable, 1000, 200).action == UiSearchAction::none,
          "search_scene_rejects_nonfinite_height_without_publishing_stale_hit_geometry");
    ui_search_scene_release(&variable);
    const u32 infinity_bits = 0x7f800000u;
    copy_bytes(&variable_fixture.invalid_height, &infinity_bits, sizeof(infinity_bits));
    ++variable_source.revision;
    check(!ui_search_scene_update(&variable, &variable_source, &variable_content, viewport, title) &&
              !variable.active,
          "search_scene_rejects_infinite_height_before_prefix_arithmetic");
    ui_search_scene_release(&variable);
    content.suggestions = false;
    content.filters = true;
    ++content.revision;
    UiSearchScene detached_header{};
    UiSearchContent header_content = content;
    header_content.filters = false;
    check(ui_search_scene_update(&detached_header, &source, &header_content, {0, 60, 700, 650},
                                 {300, 0, 700, 60}) &&
              detached_header.viewport.top == 60 && detached_header.panel.top == 60 &&
              ui_search_scene_hit_test(detached_header, detached_header.field.left + 8, 24).action ==
                  UiSearchAction::edit &&
              ui_search_scene_hit_test(detached_header, detached_header.field.right - 8, 24).action ==
                  UiSearchAction::submit,
          "search_scene_header_above_content_accepts_edit_and_submit_pointer_hits");
    const auto* submit = ui_widget_manager_layout_node(detached_header.widgets, detached_header.buttons[0]);
    UiRect header_anchor{};
    check(submit &&
              ui_rect_equal(ui_rect_intersection(submit->rectangle, submit->clip_rectangle),
                            submit->rectangle) &&
              ui_widget_manager_focus(&detached_header.widgets, detached_header.edit) &&
              ui_widget_manager_move_focus(&detached_header.widgets, false).slot ==
                  detached_header.buttons[0].slot &&
              ui_widget_manager_context_anchor(detached_header.widgets, &header_anchor) &&
              ui_rect_equal(header_anchor, submit->rectangle),
          "search_scene_detached_header_submit_keeps_visible_keyboard_focus_and_tooltip_anchor");
    check(!ui_search_scene_contains(detached_header, 20, 24) &&
              ui_text_command_stream_initialize(&stream, {commands, 128}) &&
              ui_search_scene_text_output(detached_header, kDefaultTheme, &output, nullptr),
          "search_scene_detached_header_retains_text_without_claiming_unowned_title_space");
    bool header_text_visible = false;
    for (u32 i = 0; i < stream.count; ++i)
        header_text_visible |=
            commands[i].owner.handle.slot == detached_header.edit.slot &&
            ui_rect_has_area(ui_rect_intersection(commands[i].clip, detached_header.text));
    check(header_text_visible, "search_scene_detached_header_edit_text_has_a_visible_clip");
    ui_search_scene_release(&detached_header);
    UiSearchScene explicit_header{};
    UiShellHeaderGeometry projected_header{};
    projected_header.title = {312, 0, 1200, 48};
    projected_header.search_field = {880, 8, 1184, 40};
    projected_header.member_toggle = {840, 8, 872, 40};
    check(ui_search_scene_update(&explicit_header, &source, &header_content, viewport, title,
                                 &projected_header) &&
              ui_rect_equal(explicit_header.field, projected_header.search_field) &&
              explicit_header.field.right == 1184 &&
              ui_search_scene_hit_test(explicit_header, 1180, 24).action == UiSearchAction::submit,
          "search_scene_explicit_shell_geometry_uses_actual_query_without_reserved_right_slot");
    ui_search_scene_release(&explicit_header);
    UiSearchScene native_minimum{};
    check(
        ui_search_scene_update(&native_minimum, &source, &content, {312, 0, 872, 480}, {312, 0, 632, 48}) &&
            native_minimum.overlay && ui_rect_width(native_minimum.panel) == 360 &&
            native_minimum.field.left >= 312 && native_minimum.field.right <= 632,
        "search_scene_native_minimum_preserves_navigation_members_and_header_field");
    ui_search_scene_release(&native_minimum);
    UiSearchScene compact{};
    UiSearchSource short_source{&fixture, 3, 1, resolve_search_scene};
    check(ui_search_scene_update(&compact, &short_source, &content, {0, 0, 360, 240}, {0, 0, 360, 48}),
          "search_scene_compact_native_overlay_layout");
    bool clipped = true;
    for (u32 i = 1; i < compact.widgets.count; ++i) {
        const auto action = compact.targets[i].action;
        if (action < UiSearchAction::filter_from || action > UiSearchAction::filter_pinned)
            continue;
        const auto* node = ui_widget_manager_layout_node(compact.widgets,
                                                         {static_cast<u16>(i), compact.widgets.generation});
        clipped &= node && node->rectangle.bottom <= compact.chooser.bottom;
    }
    for (u32 i = 0; i < compact.page_count; ++i) {
        const auto* node = ui_widget_manager_layout_node(compact.widgets, compact.pages[i]);
        clipped &= node && node->rectangle.top >= compact.panel.bottom - 44 &&
                   node->rectangle.bottom <= compact.panel.bottom;
    }
    check(clipped, "search_scene_short_filter_chooser_children_do_not_escape_surface");
    ui_search_scene_release(&compact);
    ui_search_scene_release(&scene);
    check(!scene.storage.arena.base && !scene.active, "search_scene_release_clears_all_borrowed_owners");
}
} // namespace maicord
