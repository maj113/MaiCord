#include "maicord/ui/video_scene.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/message_presentation.hpp"
#include "maicord/render/content_text_retention.hpp"

namespace maicord {
namespace {
void test_video_pointer_tracks(void (*check)(bool, const char*) noexcept) noexcept {
    const UiVideoPlaybackState playback{7, 8, 9, 30000, 120000, 25, true,
                                        false, false, false, true, true, true, true};
    const UiRect preview{0, 0, 640, 240};
    constexpr float thumb_sizes[]{8.0f, 24.0f};
    constexpr u32 ranges[]{1, 4};
    for (const float thumb_size : thumb_sizes) {
        UiVideoScene scene{};
        UiVideoSceneStyle style{};
        style.thumb_size = thumb_size;
        const auto project = [&]() noexcept {
            return ui_video_scene_project(&scene, playback, preview, preview, {}, style, kDefaultTheme);
        };
        bool exact = project() && ui_video_scene_focus(&scene, UiVideoControl::volume, true) && project();
        for (const u32 index : ranges) {
            const auto bounds = scene.projection.controls[index].bounds;
            const float left = bounds.left + thumb_size * .5f, right = bounds.right - thumb_size * .5f;
            const float y = (bounds.top + bounds.bottom) * .5f;
            auto r = ui_video_scene_pointer(&scene, UiVideoPointerEvent::down, {left, y});
            exact = exact && r.request_capture && r.action_requested &&
                    (index == 1 ? r.action.position_ms == 0 : r.action.volume == 0);
            r = ui_video_scene_pointer(&scene, UiVideoPointerEvent::move, {left + (right - left) * .25f, y});
            exact = exact && r.action_requested &&
                    (index == 1 ? r.action.position_ms == 30000 : r.action.volume == 25) &&
                    ui_video_action_current(playback, r.action);
            r = ui_video_scene_pointer(&scene, UiVideoPointerEvent::up, {right, y});
            exact = exact && r.release_capture && r.action_requested &&
                    (index == 1 ? r.action.position_ms == 120000 : r.action.volume == 100);
            r = ui_video_scene_pointer(&scene, UiVideoPointerEvent::down, {bounds.left + 1, y});
            exact = exact && r.action_requested &&
                    (index == 1 ? r.action.position_ms == 0 : r.action.volume == 0);
            r = ui_video_scene_pointer(&scene, UiVideoPointerEvent::up, {bounds.right - 1, y});
            exact = exact && r.action_requested &&
                    (index == 1 ? r.action.position_ms == 120000 : r.action.volume == 100) &&
                    scene.projection.playback.elapsed_ms == playback.elapsed_ms &&
                    scene.projection.playback.volume == playback.volume;
        }
        check(exact, "video tracks: painted endpoints, current thumb and end gutters emit exact fenced seek/volume requests for host thumb sizes");
        ui_video_scene_blur(&scene);
        project();
        const auto bounds = scene.projection.controls[1].bounds;
        const float left = bounds.left + thumb_size * .5f, right = bounds.right - thumb_size * .5f;
        const UiRect clipped{left + (right - left) * .25f, preview.top, preview.right, preview.bottom};
        const bool ready = ui_video_scene_project(&scene, playback, preview, clipped, {}, style, kDefaultTheme);
        ui_video_scene_pointer(&scene, UiVideoPointerEvent::down, {(left + right) * .5f, (bounds.top + bounds.bottom) * .5f});
        const auto r = ui_video_scene_pointer(&scene, UiVideoPointerEvent::up, {-100, -100});
        check(ready && r.release_capture && r.action_requested && r.action.position_ms == 30000,
              "video tracks: a clipped captured seek clamps to its visible quarter-track without accessing the hidden prefix");
    }
}
void test_video_idle(void (*check)(bool, const char*) noexcept) noexcept {
    UiVideoPlaybackState playback{7,     8,     9,     3000, 120000, 40,   true,
                                  false, false, false, true, true,   true, true};
    const UiRect preview{0, 0, 480, 270};
    UiVideoScene scene{};
    UiVideoSceneStyle style{};
    style.fade_ms = 0;
    const auto project = [&]() noexcept {
        return ui_video_scene_project(&scene, playback, preview, preview, {}, style, kDefaultTheme);
    };
    const auto play = [&]() noexcept {
        const auto r = scene.projection.controls[0].bounds;
        return UiPoint{(r.left + r.right) * .5f, (r.top + r.bottom) * .5f};
    };
    check(project() && ui_video_scene_hover(&scene, {240, 100}) && !ui_video_scene_tick(&scene, 1000, false) &&
              scene.controls_transition.value == 1 && ui_video_scene_next_wake(scene, 1000, false) == 4000,
          "video idle: hovering a playing player arms the official three-second idle deadline");
    check(ui_video_scene_tick(&scene, 4000, false) && scene.idle && scene.controls_transition.value == 0 &&
              ui_video_scene_hit(scene, play()) == UiVideoControl::none && !ui_video_scene_next_wake(scene, 4000, false),
          "video idle: an idle pointer hides the bar and its targets without further wakes");
    check(ui_video_scene_activity(&scene, 4500) && !scene.idle && project() && scene.controls_transition.value == 1 &&
              ui_video_scene_hit(scene, play()) == UiVideoControl::play_pause &&
              ui_video_scene_next_wake(scene, 4500, true) == 7500,
          "video idle: pointer activity reveals the controls and restarts the countdown, also in reduced motion");
    playback.playing = false;
    project();
    check(!ui_video_scene_tick(&scene, 9000, false) && !scene.idle && scene.controls_transition.value == 1 &&
              !ui_video_scene_next_wake(scene, 9000, false),
          "video idle: a paused player keeps its hovered controls");
    playback.playing = true;
    project();
    ui_video_scene_pointer(&scene, UiVideoPointerEvent::down, play());
    check(!ui_video_scene_tick(&scene, 20000, false) && !scene.idle && scene.controls_transition.value == 1,
          "video idle: an active press keeps the controls pinned");
    ui_video_scene_pointer(&scene, UiVideoPointerEvent::cancel, {});
    ui_video_scene_hover(&scene, {240, 100});
    ui_video_scene_activity(&scene, 20000);
    style.idle_ms = kUiVideoControlsExpandedIdleMs;
    check(project() && ui_video_scene_next_wake(scene, 20000, false) == 21000 &&
              ui_video_scene_tick(&scene, 21000, false) && scene.idle,
          "video idle: the expanded player hides its controls after one second");
    const auto shown = ui_video_scene_player_key(&scene, UiVideoPlayerKey::seek_forward, 0, 21500);
    check(shown.handled && shown.action_requested && shown.action.position_ms == 8000 && !scene.idle &&
              scene.activity_ms == 21500,
          "video idle: player shortcuts reveal controls and emit fenced requests");
    ++playback.generation;
    check(project() && !scene.idle && !scene.activity_ms,
          "video idle: a new session never inherits idleness or activity");
}
void test_video_visibility(void (*check)(bool, const char*) noexcept,
                           UiWidgetReference text_owner) noexcept {
    UiVideoPlaybackState playback{7,     8,     9,     3000, 120000, 40,   true,
                                  false, false, false, true, true,   true, true};
    const UiRect preview{0, 0, 320, 240};
    UiVideoScene scene{};
    const auto project = [&]() noexcept {
        return ui_video_scene_project(&scene, playback, preview, preview, {}, {}, kDefaultTheme);
    };
    const auto center = [](UiRect bounds) noexcept {
        return UiPoint{(bounds.left + bounds.right) * .5f, (bounds.top + bounds.bottom) * .5f};
    };
    check(
        project() && scene.controls_transition.value == 0 && !ui_video_scene_next_wake(scene, 100, false) &&
            ui_video_scene_hit(scene, center(scene.projection.controls[0].bounds)) == UiVideoControl::none,
        "video visibility: playing off-hover preview has no bar, pause target or idle wake");
    const u64 semantic_revision = scene.text_revision;
    const u64 hidden_text_revision = ui_video_scene_text_paint_revision(scene);
    ui_video_scene_hover(&scene, {160, 30});
    ui_video_scene_tick(&scene, 100, false);
    ui_video_scene_tick(&scene, 160, false);
    check(scene.preview_hovered && scene.controls_transition.value == .5f &&
              scene.text_revision == semantic_revision &&
              ui_video_scene_text_paint_revision(scene) != hidden_text_revision &&
              ui_video_scene_next_wake(scene, 160, false) == 176,
          "video visibility: preview hover fades glyph alpha without changing control authority");
    UiTextCommand commands[kUiVideoControlCount]{};
    UiTextCommandStream stream{};
    UiTextOutput output{&stream, nullptr};
    check(ui_text_command_stream_initialize(&stream, {commands, kUiVideoControlCount}) &&
              ui_video_scene_text_output(scene, kDefaultTheme, &output, text_owner) && stream.count == 1 &&
              commands[0].owner.manager == text_owner.manager &&
              commands[0].owner.handle.generation == text_owner.handle.generation &&
              commands[0].color.alpha == kDefaultTheme.text_primary.alpha * .5f,
          "video visibility: midpoint glyph commands retain the real owner and match geometry opacity");
    playback.elapsed_ms += 10;
    check(project() && scene.controls_transition.value == .5f &&
              scene.controls_transition.started_ms == 100,
          "video visibility: time-only projection preserves the running transition");
    ui_video_scene_pointer(&scene, UiVideoPointerEvent::leave, {});
    ui_video_scene_tick(&scene, 160, false);
    ui_video_scene_tick(&scene, 220, false);
    check(scene.controls_transition.value == .25f && !scene.preview_hovered &&
              scene.hover == UiVideoControl::none,
          "video visibility: mouse leave reverses a half-fade continuously");
    ui_video_scene_tick(&scene, 280, false);
    check(scene.controls_transition.value == 0 && !ui_video_scene_next_wake(scene, 280, false),
          "video visibility: off-hover fade settles without a background timer");
    ui_video_scene_focus(&scene, UiVideoControl::seek, true);
    ui_video_scene_tick(&scene, 300, true);
    check(scene.controls_transition.value == 1 && !ui_video_scene_next_wake(scene, 300, true),
          "video visibility: keyboard focus reveals controls immediately in reduced motion");
    ui_video_scene_pointer(&scene, UiVideoPointerEvent::leave, {});
    ui_video_scene_tick(&scene, 305, false);
    check(scene.keyboard_focus && scene.focus == UiVideoControl::seek &&
              scene.controls_transition.value == 1 && !ui_video_scene_next_wake(scene, 305, false),
          "video visibility: native mouse leave preserves keyboard control ownership without polling");
    ui_video_scene_blur(&scene);
    ui_video_scene_tick(&scene, 310, true);
    const auto seek = center(scene.projection.controls[1].bounds);
    const auto down = ui_video_scene_pointer(&scene, UiVideoPointerEvent::down, seek);
    ui_video_scene_tick(&scene, 320, true);
    ui_video_scene_pointer(&scene, UiVideoPointerEvent::leave, {});
    ui_video_scene_tick(&scene, 330, false);
    check(down.request_capture && scene.capture == UiVideoControl::seek &&
              scene.controls_transition.value == 1 && !ui_video_scene_next_wake(scene, 330, false),
          "video visibility: range capture pins controls after the pointer leaves the preview");
    ui_video_scene_pointer(&scene, UiVideoPointerEvent::up, {-100, -100});
    ui_video_scene_tick(&scene, 340, true);
    check(scene.focus == UiVideoControl::seek && !scene.keyboard_focus &&
              scene.controls_transition.value == 0,
          "video visibility: retained mouse focus alone does not pin the controls");
    playback.playing = false;
    check(project() && ui_rect_has_area(scene.center_play) && scene.controls_transition.value == 0,
          "video visibility: paused preview retains center Play with the bar hidden");
    playback.buffering = true;
    check(project() && ui_rect_has_area(scene.buffering_indicator) &&
              !ui_rect_has_area(scene.center_play) && scene.controls_transition.value == 0 &&
              ui_video_scene_next_wake(scene, 352, false) == 384,
          "video visibility: buffering spinner is independent from the hidden bar");
    playback.buffering = false;
    playback.error = true;
    project();
    ui_video_scene_tick(&scene, 360, true);
    check(scene.projection.controls[5].visible && scene.controls_transition.value == 1 &&
              !ui_video_scene_next_wake(scene, 360, false),
          "video visibility: terminal error remains readable without an animation wake");
    playback.error = false;
    playback.playing = true;
    ++playback.generation;
    check(project() && scene.controls_transition.value == 0 && !scene.preview_hovered &&
              scene.capture == UiVideoControl::none && scene.focus == UiVideoControl::none,
          "video visibility: a new session cannot inherit transition or input ownership");
    const UiRect tiny{0, 0, 80, 60};
    check(ui_video_scene_project_play_only(&scene, playback, tiny, tiny, {}, {}, {}, kDefaultTheme) &&
              scene.controls_transition.value == 0,
          "video visibility: playing tiny preview hides its center Pause off-hover");
    ui_video_scene_hover(&scene, center(tiny));
    ui_video_scene_tick(&scene, 400, false);
    scene.clip = {0, 0, 1, 1};
    ui_video_scene_tick(&scene, 410, false);
    check(!scene.controls_transition.active && !ui_video_scene_next_wake(scene, 410, false),
          "video visibility: clipped-out controls retire the fade instead of polling");
}
UiWidgetReference poster_metadata(void (*check)(bool, const char*) noexcept) noexcept {
    struct Fixture {
        MessageRecord record[1];
        u32 order[1];
        u8 text[64];
        MessageRichRecord rich_record[1];
        u16 rich_order[1];
        u8 rich_text[1024];
        u32 heights[1];
        u64 fenwick[1];
        UiWidget widgets[kMessageSceneWidgetsPerRow + 1];
        UiLayoutNode nodes[kMessageSceneWidgetsPerRow + 1];
        MessageStore store;
        MessageRichStore rich;
        UiVariableList list;
        MessageTimeline timeline;
        MessageScene scene;
    };
    static Fixture f{};
    bool ready =
        message_store_initialize(&f.store, {f.record, 1}, {f.order, 1}, {f.text, 64}) &&
        message_store_upsert(&f.store, {1, 7, 9, 0, 0, utf8_literal("video posters"), 0}).ok() &&
        message_rich_store_initialize(&f.rich, {f.rich_record, 1}, {f.rich_order, 1},
                                      {f.rich_text, 1024}) &&
        ui_variable_list_initialize(&f.list, {f.heights, 1}, {f.fenwick, 1}, 1, ui_extent_units(57)) &&
        message_timeline_initialize(&f.timeline, &f.store, &f.list, ui_extent_units(57),
                                    ui_extent_units(1000), false) &&
        message_timeline_bind_rich_store(&f.timeline, &f.rich) &&
        message_scene_initialize_owned(&f.scene, &f.timeline, 1, 1,
                                       {f.widgets, kMessageSceneWidgetsPerRow + 1},
                                       {f.nodes, kMessageSceneWidgetsPerRow + 1});
    GatewayMessageAttachment attachment{};
    attachment.id = 70;
    attachment.filename = utf8_literal("clip.mp4");
    attachment.content_type = utf8_literal("video/mp4");
    attachment.url = utf8_literal("https://example.com/clip.mp4");
    attachment.width = 80;
    attachment.height = 60;
    attachment.has_width = attachment.has_height = true;
    GatewayMessageEmbed embeds[3]{};
    for (u32 i = 0; i < 3; ++i) {
        auto& embed = embeds[i];
        embed.type = i ? GatewayMessageEmbedType::gifv : GatewayMessageEmbedType::video;
        auto& media = i == 2 ? embed.image : embed.video;
        media.url = utf8_literal("https://example.com/embed.mp4");
        media.content_type = utf8_literal("video/mp4");
        media.width = 80;
        media.height = 60;
        media.has_width = media.has_height = true;
    }
    GatewayMessageEvent event{};
    event.id = 1;
    event.fields = gateway_message_field_attachments | gateway_message_field_embeds;
    event.attachments = {&attachment, 1};
    event.embeds = {embeds, 3};
    ready = ready && message_rich_store_apply(&f.rich, 1, f.record[0].revision, event).ok() &&
            message_scene_update(&f.scene, {0, 0, 740, 1800}).ok();
    PaintStream paint{};
    MessageMediaCommand commands[8]{};
    MessageMediaCommandStream media{};
    MessagePresentationResult result{};
    ready = ready && paint_stream_initialize(&paint) && paint_stream_reset(&paint, 740, 1800) &&
            message_media_command_stream_initialize(&media, {commands, 8}) &&
            message_presentation_build(f.scene, kDefaultTheme, &paint, &media, &result);
    u32 ordinary = 0, excluded = 0;
    for (u32 i = 0; ready && i < media.count; ++i) {
        ordinary += commands[i].ordinary_video;
        excluded += commands[i].kind == MessageMediaCommandKind::embed && commands[i].embed_index > 0 &&
                    !commands[i].ordinary_video;
    }
    check(ready && ordinary == 2 && excluded == 2,
          "video posters: actual message producer flags MP4 attachment/primary embed and excludes GIFV "
          "video and fallback image");
    paint_stream_release(&paint);
    return ready ? UiWidgetReference{&f.scene.widgets, f.scene.root} : UiWidgetReference{};
}
} // namespace
void test_video_scene(void (*check)(bool, const char*) noexcept) noexcept {
    test_video_pointer_tracks(check);
    test_video_idle(check);
    ContentTextRetentionOwner retained{};
    retained.valid = true;
    retained.key = {1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 96, 12, 13, true};
    auto changed = retained.key;
    changed.popup_signature = 99;
    check(content_text_retention_base_current(retained, changed),
          "video text retention: an overlay-only signature change can reuse the shell/message prefix");
    ContentTextRetentionKey changes[14]{};
    for (auto& key : changes)
        key = changed;
    ++changes[0].shell_signature;
    ++changes[1].message_signature;
    ++changes[2].theme_signature;
    ++changes[3].selection_message_id;
    ++changes[4].selection_command_identity;
    ++changes[5].selection_revision;
    ++changes[6].selection_command_index;
    ++changes[7].selection_anchor;
    ++changes[8].selection_caret;
    ++changes[9].dpi;
    ++changes[10].atlas_evicted_entries;
    ++changes[11].atlas_full_resets;
    changes[12].include_messages = false;
    retained.valid = false;
    bool refused = !content_text_retention_base_current(retained, changed);
    retained.valid = true;
    // The final unchanged copy verifies that the comparisons are selective.
    for (u32 i = 0; i < 13; ++i)
        refused = refused && !content_text_retention_base_current(retained, changes[i]);
    check(refused && content_text_retention_base_current(retained, changes[13]),
          "video text retention: selection, message/shell, font, theme, DPI, atlas changes and invalid "
          "owners require full rebuild");
    const auto text_owner = poster_metadata(check);
    test_video_visibility(check, text_owner);
    UiVideoPlaybackState state{7,     8,     9,     30000, 120000, 40,   true,
                               false, false, false, true,  true,   true, true};
    UiVideoControlsStyle controls{};
    UiVideoSceneStyle style{};
    UiVideoScene s{};
    // Existing layout/action tests use immediate host styling. The separate
    // visibility fixture above exercises real default-duration transitions.
    style.fade_ms = 0;
    const UiRect bounds{0, 0, 640, 120};
    const auto project = [&](UiRect clip) noexcept {
        return ui_video_scene_project(&s, state, bounds, clip, controls, style, kDefaultTheme);
    };
    const auto center = [](UiRect r) noexcept {
        return UiPoint{(r.left + r.right) * .5f, (r.top + r.bottom) * .5f};
    };
    check(project(bounds), "video scene: native scene accepts caller layout and metrics");
    const u64 initial_text = s.text_revision;
    const u64 initial_paint = ui_video_scene_paint_revision(s);
    const auto original_state = state;
    state.elapsed_ms = 30999;
    state.duration_ms = 120999;
    state.playing = false;
    state.muted = true;
    state.volume = 95;
    check(project(bounds) && s.text_revision == initial_text,
          "video scene: subsecond ticks and playback/range paint changes retain native glyph text");
    check(
        ui_video_scene_paint_revision(s) != initial_paint,
        "video scene: transport and range paint updates invalidate retained native geometry independently");
    const u64 before_hover = ui_video_scene_paint_revision(s);
    ui_video_scene_pointer(&s, UiVideoPointerEvent::move, center(s.projection.controls[0].bounds));
    ui_video_scene_key(&s, UiVideoKey::next);
    check(s.text_revision == initial_text, "video scene: hover and focus do not rebuild native glyph text");
    check(ui_video_scene_paint_revision(s) != before_hover,
          "video scene: pointer and keyboard feedback invalidate retained native geometry");
    state.elapsed_ms = 31000;
    check(project(bounds) && s.text_revision != initial_text,
          "video scene: displayed second changes invalidate native glyph text");
    state = original_state;
    check(project(bounds) && s.text_revision == initial_text,
          "video scene: same text projection restores its stable native text fingerprint");
    state.buffering = true;
    check(project(bounds) && s.text_revision == initial_text && ui_rect_has_area(s.buffering_indicator) &&
              !s.projection.controls[5].visible,
          "video scene: buffering uses native geometry while retaining existing timestamp glyphs");
    UiVideoScene tall{};
    check(ui_video_scene_project(&tall, state, {0, 0, 320, 500}, {0, 0, 320, 300}, controls, style,
                                 kDefaultTheme) &&
              tall.active && ui_rect_has_area(tall.buffering_indicator) &&
              ui_video_scene_next_wake(tall, 32, false) == 64,
          "video scene: visible center spinner survives an offscreen bottom control bar");
    const auto buffering_paint = ui_video_scene_paint_revision(s);
    check(ui_video_scene_tick(&s, 32, false) && s.text_revision == initial_text &&
              ui_video_scene_paint_revision(s) != buffering_paint &&
              ui_video_scene_next_wake(s, 32, false) == 64 && !ui_video_scene_next_wake(s, 32, true),
          "video scene: spinner advances only paint and schedules no reduced-motion wake");
    auto offscreen = s;
    offscreen.clip = {0, 0, 10, 10};
    check(!ui_video_scene_tick(&offscreen, 64, false) && !ui_video_scene_next_wake(offscreen, 64, false),
          "video scene: clipped-out buffering geometry schedules no idle frames");
    state.error = true;
    check(project(bounds) && !ui_rect_has_area(s.buffering_indicator) &&
              !ui_video_scene_next_wake(s, 64, false) && s.projection.controls[5].visible,
          "video scene: terminal error replaces loading geometry with readable error feedback");
    state = original_state;
    controls.text_style |= text_style_bold;
    check(project(bounds) && s.text_revision != initial_text,
          "video scene: host text style changes invalidate native glyph text");
    controls = {};
    Theme alternative = kDefaultTheme;
    alternative.text_primary.red = .7f;
    check(ui_video_scene_project(&s, state, bounds, bounds, controls, style, alternative) &&
              s.text_revision != initial_text,
          "video scene: host theme change invalidates native glyph text");
    project(bounds);
    ui_video_scene_blur(&s);
    const auto play = center(s.projection.controls[0].bounds);
    auto r = ui_video_scene_pointer(&s, UiVideoPointerEvent::down, play);
    check(r.request_capture && !r.action_requested && s.capture == UiVideoControl::play_pause,
          "video scene: button press captures without prematurely changing playback");
    r = ui_video_scene_pointer(&s, UiVideoPointerEvent::up, {700, 200});
    check(r.release_capture && !r.action_requested && state.playing,
          "video scene: releasing away cancels a button and preserves actual playing state");
    ui_video_scene_pointer(&s, UiVideoPointerEvent::down, play);
    r = ui_video_scene_pointer(&s, UiVideoPointerEvent::up, play);
    check(r.action_requested && r.action.control == UiVideoControl::play_pause && !r.action.value &&
              ui_video_action_current(state, r.action) && s.projection.playback.playing,
          "video scene: matching release emits a current pause request without optimistic playback");
    const auto prior = r.action;
    r = ui_video_scene_key(&s, UiVideoKey::next);
    check(r.handled && !r.action_requested && r.action.control == UiVideoControl::none &&
              s.focus == UiVideoControl::seek && !ui_video_action_current(state, r.action),
          "video scene: Tab carries no prior actionable request");
    auto seek = s.projection.controls[1].bounds;
    r = ui_video_scene_pointer(&s, UiVideoPointerEvent::down, center(seek));
    check(r.request_capture && r.action_requested && r.action.position_ms == 60000,
          "video scene: timeline press requests exact duration fraction");
    state.elapsed_ms = 45000;
    check(project(bounds) && s.capture == UiVideoControl::seek && s.projection.playback.elapsed_ms == 45000,
          "video scene: authoritative elapsed update preserves a current drag");
    r = ui_video_scene_pointer(&s, UiVideoPointerEvent::move, {-100, 200});
    check(r.action_requested && !r.action.position_ms && s.projection.playback.elapsed_ms == 45000,
          "video scene: captured seek clamps left outside target without faking seek completion");
    r = ui_video_scene_pointer(&s, UiVideoPointerEvent::up, {1000, -100});
    check(r.release_capture && r.action_requested && r.action.position_ms == 120000,
          "video scene: captured seek reaches bounded duration on rightward release");
    check(ui_video_scene_focus(&s, UiVideoControl::volume, true) && project(bounds) &&
              s.projection.controls[4].visible,
          "video scene: keyboard volume focus expands its visible slider before pointer capture");
    const auto volume = s.projection.controls[4].bounds;
    ui_video_scene_pointer(&s, UiVideoPointerEvent::down, center(volume));
    r = ui_video_scene_pointer(&s, UiVideoPointerEvent::move, {10000, 0});
    check(r.action_requested && r.action.volume == 100 && s.projection.playback.volume == 40,
          "video scene: captured volume clamps actual percentage requests");
    r = ui_video_scene_pointer(&s, UiVideoPointerEvent::cancel, {});
    check(r.release_capture && !r.action_requested && s.capture == UiVideoControl::none,
          "video scene: capture cancellation emits no transport request");
    ui_video_scene_blur(&s);
    check(project(bounds) && !s.projection.volume_expanded,
          "video scene: volume collapses after focus, hover and capture retire");
    const auto speaker = center(s.projection.controls[3].bounds);
    ui_video_scene_pointer(&s, UiVideoPointerEvent::move, speaker);
    check(project(bounds) && s.projection.volume_expanded && project(bounds) &&
              s.hover == UiVideoControl::mute,
          "video scene: speaker hover keeps expanded volume stable across playback reprojections");
    // A silent video disables the speaker; a press on it stays on the bar
    // instead of reaching the picture, whose click toggles playback.
    state.has_audio = false;
    project(bounds);
    ui_video_scene_pointer(&s, UiVideoPointerEvent::move, speaker);
    r = ui_video_scene_pointer(&s, UiVideoPointerEvent::down, speaker);
    check(!s.projection.controls[3].enabled && r.handled && !r.request_capture && !r.action_requested &&
              s.capture == UiVideoControl::none,
          "video scene: a press on the disabled speaker is consumed by the control bar");
    state.has_audio = true;
    ui_video_scene_blur(&s);
    project(bounds);
    seek = s.projection.controls[1].bounds;
    ui_video_scene_pointer(&s, UiVideoPointerEvent::down, center(seek));
    ui_video_scene_pointer(&s, UiVideoPointerEvent::move, speaker);
    check(project(bounds) && s.capture == UiVideoControl::seek && !s.projection.volume_expanded &&
              ui_rect_equal(seek, s.projection.controls[1].bounds),
          "video scene: speaker crossing cannot expand volume or steal an ongoing seek drag");
    ui_video_scene_pointer(&s, UiVideoPointerEvent::cancel, {});
    ui_video_scene_pointer(&s, UiVideoPointerEvent::move, speaker);
    project(bounds);
    const auto expanded_volume = center(s.projection.controls[4].bounds);
    ui_video_scene_pointer(&s, UiVideoPointerEvent::down, expanded_volume);
    r = ui_video_scene_pointer(&s, UiVideoPointerEvent::move, {10000, -100});
    check(r.action_requested && r.action.volume == 100 && project(bounds) &&
              s.capture == UiVideoControl::volume && s.projection.controls[4].visible,
          "video scene: captured volume stays expanded outside the preview and clamps requests without "
          "losing capture");
    ui_video_scene_pointer(&s, UiVideoPointerEvent::cancel, {});
    ui_video_scene_pointer(&s, UiVideoPointerEvent::down, play);
    ++state.generation;
    check(project(bounds) && s.capture == UiVideoControl::none && s.focus == UiVideoControl::none &&
              !ui_video_action_current(state, prior) &&
              !ui_video_scene_pointer(&s, UiVideoPointerEvent::up, play).action_requested,
          "video scene: replacement generation drops old capture, focus and request authority");
    const UiRect clip{(seek.left + seek.right) * .5f, 0, seek.right, bounds.bottom};
    check(project(clip) && ui_video_scene_hit(s, play) == UiVideoControl::none,
          "video scene: clipping excludes otherwise enabled pointer targets");
    ui_video_scene_pointer(&s, UiVideoPointerEvent::down, {clip.left + 1, (seek.top + seek.bottom) * .5f});
    r = ui_video_scene_pointer(&s, UiVideoPointerEvent::move, {-100, 0});
    check(r.action_requested && r.action.position_ms == 60000,
          "video scene: dragging toward clipped content stops at the visible timeline edge");
    check(project(bounds) && s.capture == UiVideoControl::none,
          "video scene: geometry change cancels captured drag before another dispatch");
    check(ui_video_scene_focus(&s, UiVideoControl::volume, true),
          "video scene: host may establish visible keyboard focus");
    r = ui_video_scene_key(&s, UiVideoKey::home);
    check(r.action_requested && r.action.volume == 0,
          "video scene: keyboard Home requests bounded minimum volume");
    state.can_volume = false;
    check(project(bounds) && s.focus == UiVideoControl::none &&
              !ui_video_scene_key(&s, UiVideoKey::end).action_requested,
          "video scene: revoked range capability prevents further key requests");
    char temporary[]{'P', 'l', 'a', 'y'};
    controls.pause = {temporary, 4};
    check(project(bounds), "video scene: accepts host semantic labels");
    const auto copied = s;
    temporary[0] = 'X';
    s.labels[0][0] = 'Y';
    check(utf8_equal(ui_video_scene_label(copied, UiVideoControl::play_pause), utf8_literal("Play")) &&
              ui_video_scene_label(copied, UiVideoControl::time).data == copied.labels[2],
          "video scene: copied scene owns both host labels and formatted time storage");
    controls = {};
    state.buffering = true;
    check(project(bounds), "video scene: buffering state remains available for native feedback");
    ui_video_scene_hover(&s, center(s.preview));
    PaintStream paint{};
    UiTextCommand commands[6]{};
    UiTextCommandStream stream{};
    UiTextOutput output{&stream, nullptr};
    check(paint_stream_initialize(&paint) && paint_stream_reset(&paint, 640, 200) &&
              ui_text_command_stream_initialize(&stream, {commands, 6}) &&
              ui_video_scene_paint(s, kDefaultTheme, &paint) &&
              ui_video_scene_text_output(s, kDefaultTheme, &output, text_owner) && paint.count > 24 &&
              stream.count == 1 && paint.clip_depth == 1,
          "video scene: loading arc and timestamp text use balanced native streams without a buffering "
          "label");
    auto stale_text_owner = text_owner;
    ++stale_text_owner.handle.generation;
    ui_text_command_stream_reset(&stream);
    check(!ui_video_scene_text_output(s, kDefaultTheme, &output, stale_text_owner) &&
              stream.rejected == 1 && stream.count == 0,
          "video scene: native text rejects a stale host widget generation before publishing commands");
    check(project(clip), "video scene: accepts partial native clip");
    ui_text_command_stream_reset(&stream);
    paint_stream_reset(&paint, 640, 200);
    bool confined = ui_video_scene_paint(s, kDefaultTheme, &paint) &&
                    ui_video_scene_text_output(s, kDefaultTheme, &output, text_owner);
    for (u32 i = 0; i < stream.count; ++i)
        confined = confined && commands[i].clip.left >= clip.left && commands[i].clip.right <= clip.right;
    for (u32 i = 0; i < paint.batch_count; ++i) {
        const auto& pc = paint.clips[paint.batches[i].clip_index];
        confined = confined && pc.left >= clip.left && pc.right <= clip.right && pc.top >= clip.top &&
                   pc.bottom <= clip.bottom;
    }
    check(confined, "video scene: every native text and paint batch carries the supplied clip");
    UiVideoPlayAffordanceStyle affordance{};
    const UiRect small{20, 25, 100, 85}, partial{60, 25, 100, 85};
    paint_stream_reset(&paint, 640, 200);
    check(ui_video_play_affordance_paint(small, partial, affordance, kDefaultTheme, &paint) &&
              paint.count > 1 && paint.clip_depth == 1,
          "video posters: clipped native backdrop and original play geometry share balanced host paint "
          "streams");
    bool poster_clipped = true;
    for (u32 i = 0; i < paint.batch_count; ++i) {
        const auto& pc = paint.clips[paint.batches[i].clip_index];
        poster_clipped = poster_clipped && pc.left == partial.left && pc.right == partial.right &&
                         pc.top == partial.top && pc.bottom == partial.bottom;
    }
    check(poster_clipped,
          "video posters: affordance clips exactly to preview and visible message intersection");
    state.error = false;
    state.can_play = true;
    check(ui_video_scene_project_play_only(&s, state, small, small, controls, style, affordance,
                                           kDefaultTheme) &&
              s.active && s.play_only && ui_video_scene_covers_preview(s, small) &&
              !ui_video_scene_covers_preview(s, {200, 25, 280, 85}),
          "video scene: tiny selected preview exposes only its own play/pause affordance and suppresses "
          "only its poster");
    const auto small_play = center(s.projection.controls[0].bounds);
    ui_video_scene_pointer(&s, UiVideoPointerEvent::down, small_play);
    r = ui_video_scene_pointer(&s, UiVideoPointerEvent::up, small_play);
    check(r.action_requested && r.action.control == UiVideoControl::play_pause &&
              r.action.value == !state.playing && ui_video_action_current(state, r.action) &&
              !ui_video_scene_focus(&s, UiVideoControl::seek, true) &&
              utf8_equal(ui_video_scene_label(s, UiVideoControl::play_pause),
                         state.playing ? controls.pause : controls.play),
          "video scene: play-only fallback retains actual fenced toggle semantics without promising seek "
          "or audio controls");
    ui_text_command_stream_reset(&stream);
    paint_stream_reset(&paint, 640, 200);
    check(ui_video_scene_paint(s, kDefaultTheme, &paint) &&
              ui_video_scene_text_output(s, kDefaultTheme, &output, text_owner) && stream.count == 0,
          "video scene: tiny fallback uses native symbols without cropped time/status glyphs");
    state.error = true;
    check(ui_video_scene_project_play_only(&s, state, small, small, controls, style, affordance,
                                           kDefaultTheme) &&
              !ui_rect_has_area(s.buffering_indicator) && !ui_video_scene_next_wake(s, 32, false) &&
              !s.projection.controls[0].enabled &&
              utf8_equal(ui_video_scene_label(s, UiVideoControl::play_pause), controls.error),
          "video scene: tiny failed player replaces spinner with explicit failure semantics and no timer");
    paint_stream_reset(&paint, 640, 200);
    bool warning_icon = ui_video_scene_paint(s, kDefaultTheme, &paint) && paint.clip_depth == 1;
    bool warning_seen = false;
    for (u32 i = 0; warning_icon && i < paint.count; ++i)
        warning_seen = warning_seen || (ui_icon_quad(paint.quads[i]) &&
                                        ui_icon_quad_data(paint.quads[i]).icon == static_cast<u32>(UiIcon::warning));
    check(warning_icon && warning_seen,
          "video scene: tiny failed player paints a warning icon inside its preview");
    state.error = false;
    const UiRect short_preview{10, 10, 650, 58};
    check(
        ui_video_scene_project_play_only(&s, state, short_preview, short_preview, controls, style,
                                         affordance, kDefaultTheme) &&
            s.play_only,
        "video scene: a wide short preview uses a bounded play target rather than a clipped control strip");
    check(!ui_video_scene_project_play_only(&s, state, {0, 0, 20, 20}, {0, 0, 20, 20}, controls, style,
                                            affordance, kDefaultTheme) &&
              !s.active,
          "video scene: previews smaller than host minimum target refuse interaction and clear stale "
          "fallback");
    paint_stream_release(&paint);
    const UiRect compact{0, 0, 240, 120};
    check(ui_video_scene_project(&s, state, compact, compact, controls, style, kDefaultTheme) &&
              s.projection.compact,
          "video scene: minimum compact geometry retains native control scene");
    state.can_volume = true;
    state.buffering = false;
    ui_video_scene_blur(&s);
    check(ui_video_scene_project(&s, state, compact, compact, controls, style, kDefaultTheme) &&
              !s.projection.controls[4].visible,
          "video scene: compact preview begins with time and a collapsed volume slider");
    bool navigated = true;
    for (u32 i = 0; i < 4; ++i)
        navigated = navigated && ui_video_scene_key(&s, UiVideoKey::next).handled;
    check(navigated && s.focus == UiVideoControl::volume &&
              ui_video_scene_project(&s, state, compact, compact, controls, style, kDefaultTheme) &&
              s.projection.controls[4].visible && !s.projection.controls[2].visible &&
              ui_video_scene_key(&s, UiVideoKey::increase).action.volume == 45,
          "video scene: Tab reaches hidden volume in a 240 DIP preview and expands it for bounded keyboard "
          "adjustment");
    state.playing = false;
    const UiRect poster{0, 0, 320, 240};
    check(ui_video_scene_project(&s, state, poster, poster, controls, style, kDefaultTheme) &&
              ui_rect_width(s.center_play) == kUiMediaPlayTarget,
          "video scene: paused poster keeps a centered 60 DIP play affordance inside the preview");
    const auto central_play = center(s.center_play);
    ui_video_scene_pointer(&s, UiVideoPointerEvent::down, central_play);
    r = ui_video_scene_pointer(&s, UiVideoPointerEvent::up, central_play);
    check(r.action_requested && r.action.value && ui_video_action_current(state, r.action),
          "video scene: center play shares the current descriptor action and session fence");
    state.playing = true;
    const UiRect short_bar{0, 0, 240, 60};
    check(
        ui_video_scene_project(&s, state, short_bar, short_bar, controls, style, kDefaultTheme) &&
            s.projection.bounds.top >= short_bar.top && s.projection.bounds.bottom <= short_bar.bottom &&
            ui_rect_height(s.projection.bounds) == kUiVideoControlBarHeight,
        "video scene: a 60 DIP tall preview retains a clipped single bottom bar without a detached panel");
    check(!ui_video_scene_project(&s, state, {0, 0, 190, 200}, {0, 0, 190, 200}, controls, style,
                                  kDefaultTheme) &&
              !s.active && !ui_video_scene_pointer(&s, UiVideoPointerEvent::down, play).action_requested,
          "video scene: rejected narrow layout cannot leave a stale actionable surface");
}
} // namespace maicord
