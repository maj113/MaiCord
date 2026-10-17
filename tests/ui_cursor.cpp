#include "maicord/ui/scene_cursor.hpp"
#include "maicord/ui/text_form.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

UiPoint center(UiRect r) noexcept {
    return {(r.left + r.right) * .5f, (r.top + r.bottom) * .5f};
}

struct WalkResult {
    u32 buttons{}, fields{};
    bool ok{true};
};

// Every enabled, hit-testable and unoccluded widget resolves by its declared
// kind: buttons are hands and text fields I-beams, both through the generic
// widget resolver and through the scene's own resolver.
template <class Resolve>
WalkResult walk_widgets(const UiWidgetManager& manager, Resolve resolve) noexcept {
    WalkResult result{};
    constexpr u16 interactive = ui_widget_enabled | ui_widget_hit_test;
    for (u32 i = 0; i < manager.count; ++i) {
        const UiWidget& widget = manager.storage[i];
        const UiWidgetHandle handle{static_cast<u16>(i), widget.generation};
        if ((widget.flags & interactive) != interactive || !ui_widget_manager_input_enabled(manager, handle))
            continue;
        const auto* node = ui_widget_manager_layout_node(manager, handle);
        const UiRect visible = node ? ui_rect_intersection(node->rectangle, node->clip_rectangle) : UiRect{};
        if (!ui_rect_has_area(visible))
            continue;
        const UiPoint point = center(visible);
        const auto hit = ui_widget_manager_hit_test(manager, point.x, point.y);
        if (hit.slot != handle.slot || hit.generation != handle.generation)
            continue;
        const UiCursor expected = widget.kind == UiWidgetKind::button       ? UiCursor::hand
                                  : widget.kind == UiWidgetKind::text_field ? UiCursor::ibeam
                                                                            : UiCursor::arrow;
        result.buttons += widget.kind == UiWidgetKind::button;
        result.fields += widget.kind == UiWidgetKind::text_field;
        result.ok = result.ok && ui_widget_cursor(manager, handle) == expected && resolve(point) == expected;
    }
    return result;
}

struct OwnerFixture {
    UiWidget widgets[2]{};
    UiLayoutNode nodes[2]{};
    UiWidgetManager manager{};
    UiWidgetHandle root{};
    bool initialize() noexcept {
        if (!ui_widget_manager_initialize(&manager, {widgets, 2}, {nodes, 2}))
            return false;
        const auto added = ui_widget_manager_add(
            &manager, ui_widget_invalid_handle(),
            {1, UiWidgetKind::button, static_cast<u16>(ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable),
             {utf8_literal("Owner"), ui_layout_style(), {}, 0, 1}});
        root = added.handle;
        return added.ok() && ui_widget_manager_layout(&manager, {800, 600}).ok();
    }
};

bool measure(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* value) noexcept {
    const auto valid = utf8_validate(request.text);
    if (!value || !valid.valid || request.constraint.width <= 0)
        return false;
    const float natural = static_cast<float>(valid.codepoints) * 7;
    const u32 columns = maximum(1u, static_cast<u32>(request.constraint.width / 7));
    const u32 lines = request.no_wrap ? 1u : maximum(1u, (valid.codepoints + columns - 1) / columns);
    *value = {{minimum(natural, request.constraint.width), static_cast<float>(lines) * 16}, 12, lines};
    return true;
}

void test_generic_widgets(Check check) noexcept {
    UiWidget widgets[8]{};
    UiLayoutNode nodes[8]{};
    UiWidgetManager manager{};
    auto style = ui_layout_style();
    style.flow = UiLayoutFlow::vertical;
    auto row = ui_layout_style();
    row.width = ui_layout_fill();
    row.height = ui_layout_fixed(40);
    const auto add = [&](UiWidgetHandle parent, u32 id, UiWidgetKind kind, u16 flags, UiLayoutStyle s) noexcept {
        return ui_widget_manager_add(&manager, parent, {id, kind, flags, {utf8_literal("Cursor"), s, {}, 0, id}})
            .handle;
    };
    constexpr u16 live = ui_widget_enabled | ui_widget_hit_test;
    bool built = ui_widget_manager_initialize(&manager, {widgets, 8}, {nodes, 8});
    const auto root = add(ui_widget_invalid_handle(), 1, UiWidgetKind::container, ui_widget_none, style);
    const auto button = add(root, 2, UiWidgetKind::button, live, row);
    const auto field = add(root, 3, UiWidgetKind::text_field, live, row);
    const auto disabled = add(root, 4, UiWidgetKind::button, ui_widget_hit_test, row);
    const auto label = add(root, 5, UiWidgetKind::text, live, row);
    const auto blocked = add(root, 6, UiWidgetKind::container, ui_widget_block_input, row);
    const auto inner = add(blocked, 7, UiWidgetKind::button, live, row);
    built = built && inner.generation && ui_widget_manager_layout(&manager, {200, 400}).ok();
    const auto at = [&](UiWidgetHandle handle) noexcept {
        const auto* node = ui_widget_manager_layout_node(manager, handle);
        return node ? center(node->rectangle) : UiPoint{-1, -1};
    };
    check(built && ui_widget_manager_cursor(manager, at(button).x, at(button).y) == UiCursor::hand &&
              ui_widget_manager_cursor(manager, at(field).x, at(field).y) == UiCursor::ibeam &&
              ui_widget_manager_cursor(manager, at(disabled).x, at(disabled).y) == UiCursor::arrow &&
              ui_widget_manager_cursor(manager, at(label).x, at(label).y) == UiCursor::arrow &&
              ui_widget_manager_cursor(manager, at(inner).x, at(inner).y) == UiCursor::arrow &&
              ui_widget_manager_cursor(manager, 100, 390) == UiCursor::arrow &&
              ui_widget_cursor(manager, ui_widget_invalid_handle()) == UiCursor::arrow,
          "cursor: enabled buttons are hands, fields I-beams; disabled, passive, blocked and empty are arrows");
    check(ui_action_cursor(true) == UiCursor::hand && ui_action_cursor(false) == UiCursor::arrow &&
              ui_action_cursor(false, true) == UiCursor::ibeam && ui_action_cursor(true, true) == UiCursor::ibeam,
          "cursor: actionable targets are hands unless they edit text");
}

void test_media_controls(Check check) noexcept {
    UiAudioPlaybackState audio{};
    audio.owner = 7;
    audio.generation = 8;
    audio.revision = 9;
    audio.elapsed_ms = 30000;
    audio.duration_ms = 120000;
    audio.volume = 40;
    audio.playing = audio.can_play = audio.can_seek = audio.can_volume = audio.can_download = audio.has_duration =
        true;
    audio.title = utf8_literal("Original recording.ogg");
    OwnerFixture owner{};
    UiAudioScene scene{};
    audio.voice = true;
    const UiRect card{20, 40, 420, 40 + ui_audio_card_height(kDefaultUiAudioControlsStyle, 400, true)};
    const bool projected = owner.initialize() &&
                           ui_audio_scene_project(&scene, audio, card, card, {&owner.manager, owner.root},
                                                  UiAudioSourceSurface::timeline, kDefaultUiAudioControlsStyle,
                                                  kDefaultTheme);
    const auto* play = ui_audio_controls_find(scene.projection, UiAudioControl::play_pause);
    check(projected && play && ui_audio_scene_cursor(scene, center(play->bounds)) == UiCursor::hand &&
              ui_audio_scene_cursor(scene, {card.right + 10, card.top}) == UiCursor::arrow,
          "cursor: the selected voice-note player presents a hand on Play, never the prior I-beam");

    const UiVideoPlaybackState video{7, 8, 9, 30000, 120000, 25, false, false, false, false, true, true, true, true};
    const UiRect preview{0, 0, 640, 360};
    UiVideoScene player{};
    UiVideoSceneStyle style{};
    bool video_ok = ui_video_scene_project(&player, video, preview, preview, {}, style, kDefaultTheme) &&
                    (ui_video_scene_hover(&player, center(preview)), true);
    u32 video_hands = 0;
    for (const auto& control : player.projection.controls) {
        if (!control.visible || !control.enabled || !ui_rect_has_area(control.bounds))
            continue;
        const auto point = center(control.bounds);
        if (ui_video_scene_hit(player, point) != control.id)
            continue;
        const bool label = control.kind == UiVideoControlKind::label;
        video_hands += !label;
        video_ok = video_ok && ui_video_scene_cursor(player, point) == ui_action_cursor(!label);
    }
    check(video_ok && video_hands >= 3 && ui_video_scene_cursor(player, {700, 400}) == UiCursor::arrow,
          "cursor: video player buttons and tracks are hands");
}

void test_poll(Check check) noexcept {
    UiTextMeasureRecord records[128]{};
    UiTextMeasureCache cache{};
    UiPollAnswer answers[3]{};
    answers[0] = {7, utf8_literal("A quiet walk beside the river")};
    answers[1] = {42, utf8_literal("An original idea")};
    answers[2] = {900, utf8_literal("Something else entirely")};
    UiPollState state{};
    state.owner = 11;
    state.generation = 12;
    state.revision = state.content_revision = 1;
    state.question = utf8_literal("Which original activity should we choose together?");
    state.answers = {answers, 3};
    state.supported = state.can_vote = state.can_view_results = state.can_view_voters = true;
    answers[0].selected = true;
    UiPollProjection projection{};
    const bool projected = ui_text_measure_cache_initialize(&cache, {records, 128}, measure, nullptr) &&
                           ui_poll_controls_project(state, {0, 0, 420, 0}, kDefaultUiPollStyle, kDefaultTheme,
                                                    &cache, 1, &projection);
    u32 hands = 0;
    bool exact = projected;
    const UiPollTarget targets[]{{UiPollControl::answer, 7},  {UiPollControl::answer, 42},
                                 {UiPollControl::answer, 900}, {UiPollControl::vote, 0},
                                 {UiPollControl::results, 0}};
    for (const auto target : targets) {
        if (!ui_poll_target_visible(projection, target) || !ui_poll_target_enabled(projection, target))
            continue;
        ++hands;
        exact = exact && ui_poll_controls_cursor(projection, center(ui_poll_target_rectangle(projection, target))) ==
                             UiCursor::hand;
    }
    check(exact && hands >= 4 && ui_poll_controls_cursor(projection, center(projection.geometry.question)) ==
                                     UiCursor::arrow,
          "cursor: every enabled poll answer and vote button is a hand; the question is an arrow");
}

void test_text_form(Check check) noexcept {
    TextFormScene scene{};
    UiTextMeasureRecord records[32]{};
    UiTextMeasureCache cache{};
    TextFormContent content{};
    content.title = utf8_literal("Rename channel");
    content.label = utf8_literal("Channel name");
    content.cancel = utf8_literal("Cancel");
    content.accept = utf8_literal("Save");
    content.text = utf8_literal("general");
    content.can_accept = true;
    content.single_line = true;
    const UiRect viewport{0, 0, 640, 480};
    const auto resolve = [&](UiPoint point) noexcept {
        return ui_widget_manager_cursor(scene.widgets, point.x, point.y);
    };
    const bool built = ui_text_measure_cache_initialize(&cache, {records, 32}, measure, nullptr) &&
                       text_form_update(&scene, content, viewport, &cache);
    const auto walked = built ? walk_widgets(scene.widgets, resolve) : WalkResult{0, 0, false};
    check(built && walked.ok && walked.buttons == 2 && walked.fields == 1,
          "cursor: dialog buttons are hands and the editor an I-beam");
    content.can_accept = false;
    const auto* accept = built && text_form_update(&scene, content, viewport, &cache)
                             ? text_form_node(scene, TextFormElement::accept)
                             : nullptr;
    check(accept && resolve(center(accept->rectangle)) == UiCursor::arrow,
          "cursor: a disabled dialog button is an arrow");
    text_form_release(&scene);
}

void test_settings(Check check) noexcept {
    UiSettingsScene scene{};
    UiPage page{};
    UiPageRegistry registry{};
    UiSettingsEditor name{{utf8_literal("native-room"), 2, 5, true, true, true}, nullptr, utf8_literal("Name")};
    const Utf8View choices[]{utf8_literal("Off"), utf8_literal("5 seconds")};
    UiSettingsControl controls[]{
        {1, utf8_literal("Name"), {}, UiSettingsControlKind::edit, true, false, {}, 0, {}, &name},
        {2, utf8_literal("Slowmode"), {}, UiSettingsControlKind::choice, true, false, {choices, 2}, 0, {}},
        {3, utf8_literal("Age-restricted"), {}, UiSettingsControlKind::toggle, true, false},
        {4, utf8_literal("Locked"), {}, UiSettingsControlKind::toggle, false, false}};
    UiSettingsFooter footer{{}, utf8_literal("Save changes"), utf8_literal("Reset"), true, true, false, false};
    UiSettingsPageContent content{utf8_literal("Overview"), {}, {controls, 4}, 1, kUiSettingsPageFormat, nullptr,
                                  &footer};
    const UiPageKey key{9, 1};
    const UiRect viewport{0, 0, 1280, 900};
    const bool built = ui_page_registry_initialize(&registry, {&page, 1}) &&
                       ui_page_registry_register(&registry, {key, utf8_literal("Overview"), &content}).ok() &&
                       ui_settings_scene_update(&scene, &registry, viewport);
    const auto cursor_of = [&](u64 id) noexcept {
        UiSettingsControlGeometry geometry{};
        if (!ui_settings_scene_control_geometry(scene, {UiSettingsTargetKind::control, key, id}, &geometry))
            return UiCursor::resize_ns;
        const auto point = center(geometry.clip);
        return ui_settings_scene_cursor(scene, point.x, point.y);
    };
    UiSettingsControlGeometry name_geometry{};
    const bool named =
        built && ui_settings_scene_control_geometry(scene, {UiSettingsTargetKind::control, key, 1}, &name_geometry);
    const auto* save = built ? ui_widget_manager_layout_node(scene.widgets, scene.footer_save) : nullptr;
    check(named && save && cursor_of(1) == UiCursor::ibeam && cursor_of(2) == UiCursor::hand &&
              cursor_of(3) == UiCursor::hand &&
              ui_settings_scene_cursor(scene, center(name_geometry.clip).x, name_geometry.field.top - 4) !=
                  UiCursor::ibeam &&
              ui_settings_scene_cursor(scene, center(save->rectangle).x, center(save->rectangle).y) ==
                  UiCursor::hand &&
              ui_settings_scene_cursor(scene, 1, 899, true) == UiCursor::move,
          "cursor: settings editors are I-beams, choices, switches and footer buttons hands, drags move");
    // Every remaining button the scene materializes (close, save, reset, rows)
    // resolves to a hand at its centre unless it is a disabled row.
    bool rows = built;
    u32 buttons = 0;
    for (u32 i = 0; built && i < scene.widgets.count; ++i) {
        const auto& widget = scene.widgets.storage[i];
        const UiWidgetHandle handle{static_cast<u16>(i), widget.generation};
        if (widget.kind != UiWidgetKind::button || ui_widget_cursor(scene.widgets, handle) != UiCursor::hand)
            continue;
        const auto* node = ui_widget_manager_layout_node(scene.widgets, handle);
        const auto point = center(ui_rect_intersection(node->rectangle, node->clip_rectangle));
        const auto hit = ui_widget_manager_hit_test(scene.widgets, point.x, point.y);
        if (hit.slot != handle.slot)
            continue;
        ++buttons;
        rows = rows && ui_settings_scene_cursor(scene, point.x, point.y) == UiCursor::hand;
    }
    check(rows && buttons >= 3, "cursor: every live settings button is a hand");
    // A row's label and empty space are not its control: arrow, not hand.
    UiSettingsControlGeometry toggle{}, choice{};
    const bool rows_ok =
        built && ui_settings_scene_control_geometry(scene, {UiSettingsTargetKind::control, key, 3}, &toggle) &&
        ui_settings_scene_control_geometry(scene, {UiSettingsTargetKind::control, key, 2}, &choice);
    check(rows_ok && ui_settings_scene_cursor(scene, toggle.control.left + 8, center(toggle.field).y) == UiCursor::arrow &&
              ui_settings_scene_cursor(scene, choice.field.right + 16, center(choice.field).y) == UiCursor::arrow &&
              ui_settings_scene_cursor(scene, center(toggle.field).x, center(toggle.field).y) == UiCursor::hand,
          "cursor: settings rows show a hand only over their switch or select");
    ui_settings_scene_release(&scene);
}
} // namespace

void test_ui_cursor(Check check) noexcept {
    test_generic_widgets(check);
    test_media_controls(check);
    test_poll(check);
    test_text_form(check);
    test_settings(check);
}
} // namespace maicord
