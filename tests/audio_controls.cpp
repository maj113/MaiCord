#include "maicord/ui/audio_controls.hpp"
#include "maicord/ui/message_rich_layout.hpp"
#include "maicord/ui/message_rich_widgets.hpp"
#include "maicord/ui/message_scene.hpp"
#include "maicord/ui/search_scene.hpp"
#include "maicord/ui/foreground_color.hpp"
#include "maicord/ui/theme_presets.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
UiPoint center(UiRect rectangle) noexcept {
    return {(rectangle.left + rectangle.right) * .5f, (rectangle.top + rectangle.bottom) * .5f};
}
bool contained(UiRect outer, UiRect inner) noexcept {
    return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right &&
           inner.bottom <= outer.bottom && inner.right > inner.left && inner.bottom > inner.top;
}
bool separate(UiRect a, UiRect b) noexcept {
    return a.right <= b.left || b.right <= a.left || a.bottom <= b.top || b.bottom <= a.top;
}
UiAudioPlaybackState playback() noexcept {
    UiAudioPlaybackState state{};
    state.owner = 7;
    state.generation = 8;
    state.revision = 9;
    state.elapsed_ms = 30000;
    state.duration_ms = 120000;
    state.volume = 40;
    state.playing = state.can_play = state.can_seek = state.can_volume = state.can_download =
        state.has_duration = true;
    state.title = utf8_literal("Original recording.ogg");
    return state;
}
struct WidgetFixture {
    UiWidget widgets[12]{};
    UiLayoutNode nodes[12]{};
    UiWidgetManager manager{};
    UiWidgetHandle root{};
    bool initialize() noexcept {
        if (!ui_widget_manager_initialize(&manager, {widgets, 12}, {nodes, 12}))
            return false;
        const auto added = ui_widget_manager_add(
            &manager, ui_widget_invalid_handle(),
            {1,
             UiWidgetKind::button,
             static_cast<u16>(ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable),
             {utf8_literal("Audio owner"), ui_layout_style(), {}, 0, 1}});
        root = added.handle;
        return added.ok();
    }
};
struct SearchAudioFixture {
    MessageRecord records[1]{};
    u32 order[1]{};
    u8 text[256]{};
    MessageRichRecord rich_records[2]{};
    u16 rich_order[2]{};
    u8 rich_text[1024]{};
    MessageHandle displayed[1]{};
    u32 heights[1]{};
    u64 fenwick[1]{};
    MessageStore store{};
    MessageRichStore rich{};
    UiVariableList list{};
    MessageTimeline timeline{};
    bool current{true};
};
void search_geometry(void (*check)(bool, const char*) noexcept,
                     Utf8View filename = utf8_literal("voice.ogg"),
                     Utf8View mime = utf8_literal("audio/ogg"), bool voice = true,
                     MediaAttachmentFormat format = MediaAttachmentFormat::ogg_opus) noexcept {
    SearchAudioFixture fixture{};
    bool ready =
        message_store_initialize(&fixture.store, {fixture.records, 1}, {fixture.order, 1},
                                 {fixture.text, sizeof(fixture.text)}) &&
        message_rich_store_initialize(&fixture.rich, {fixture.rich_records, 2}, {fixture.rich_order, 2},
                                      {fixture.rich_text, sizeof(fixture.rich_text)}) &&
        message_store_upsert(&fixture.store, {17, 71, 9, 1700000000000ull, 0, utf8_literal("Audio message"),
                                              voice ? 8192u : 0u})
            .ok();
    fixture.displayed[0] = message_store_find(fixture.store, 17).handle;
    const auto* record = message_store_resolve(fixture.store, fixture.displayed[0]);
    GatewayMessageAttachment attachment{};
    attachment.id = 19;
    attachment.filename = filename;
    attachment.content_type = mime;
    attachment.url = utf8_literal("https://cdn.discordapp.com/attachments/71/19/original");
    attachment.has_duration = true;
    attachment.duration_ms = 120000;
    GatewayMessageEvent event{};
    event.id = 17;
    event.flags = voice ? 8192u : 0u;
    event.fields = gateway_message_field_attachments | gateway_message_field_flags;
    event.attachments = {&attachment, 1};
    ready = ready && record && message_rich_store_apply(&fixture.rich, 17, record->revision, event).ok() &&
            ui_variable_list_initialize(&fixture.list, {fixture.heights, 1}, {fixture.fenwick, 1}, 1,
                                        ui_extent_units(200)) &&
            message_timeline_initialize(&fixture.timeline, &fixture.store, &fixture.list,
                                        ui_extent_units(200), ui_extent_units(320), false) &&
            message_timeline_bind_rich_store(&fixture.timeline, &fixture.rich);
    UiSearchSource source{
        &fixture, 1, 1,
        +[](void* opaque, u32 index, UiSearchRow* output) noexcept {
            auto& f = *static_cast<SearchAudioFixture*>(opaque);
            const auto* value = index == 0 ? message_store_resolve(f.store, f.displayed[0]) : nullptr;
            if (!value || !output)
                return false;
            *output = {value->channel_id,
                       value->id,
                       utf8_literal("Original author"),
                       utf8_literal("# audio"),
                       {},
                       {},
                       true,
                       true};
            output->author_id = value->author_id;
            return true;
        },
        +[](void* opaque) noexcept { return static_cast<SearchAudioFixture*>(opaque)->current; }};
    source.rich_timeline = &fixture.timeline;
    source.rich_order = {fixture.displayed, 1};
    UiSearchContent content{};
    content.edit.active = content.panel = true;
    content.phase = UiSearchPhase::ready;
    content.total = content.revision = 1;
    UiSearchScene scene{};
    ready = ready && ui_search_scene_update(&scene, &source, &content, {0, 0, 480, 400}, {0, 0, 480, 48});
    UiSearchTarget target{UiSearchAction::rich_item, 71, 17};
    target.source_revision = 1;
    target.source_index = 0;
    UiRect bounds{}, clip{};
    const auto* native = ui_search_scene_message_scene(scene);
    const auto* node = native && native->count
                           ? message_scene_rich_item_layout_node(*native, native->rows[0],
                                                                 MessageSceneRichItemKind::attachment, 0)
                           : nullptr;
    UiAudioCardGeometry expected{};
    check(ready && node &&
              ui_audio_card_geometry(node->content_rectangle, kDefaultUiAudioControlsStyle, &expected,
                                     voice) &&
              ui_search_scene_rich_preview_geometry(scene, target, &bounds, &clip) &&
              ui_rect_equal(bounds, expected.bounds) && ui_rect_has_area(clip) &&
              ui_rect_height(bounds) == (voice ? 86 : 112) &&
              message_rich_attachment_format(fixture.rich, fixture.rich_records[0].attachments[0]) ==
                  format &&
              fixture.rich_records[0].attachments[0].duration_ms == 120000 &&
              ui_search_scene_hit_test(scene, center(expected.play_pause).x, center(expected.play_pause).y)
                      .action == UiSearchAction::rich_item,
          "audio Search: actual accepted audio format/duration and rich projection expose the same native "
          "card "
          "bounds and independent attachment hit target");
    const auto save_hit =
        ui_search_scene_hit_test(scene, center(expected.download).x, center(expected.download).y);
    bool keyboard_save = false;
    for (u32 i = 0; ready && i < scene.widgets.count; ++i) {
        if (!scene.targets[i].save)
            continue;
        const UiWidgetHandle handle{static_cast<u16>(i), scene.widgets.generation};
        const auto* save_node = ui_widget_manager_layout_node(scene.widgets, handle);
        keyboard_save =
            save_node && ui_widget_manager_focus(&scene.widgets, handle) &&
            ui_rect_equal(save_node->rectangle, ui_rect_intersection(expected.download, clip)) &&
            ui_search_scene_target_current(scene, scene.targets[i]);
    }
    check(ready && save_hit.action == UiSearchAction::rich_item && save_hit.save && keyboard_save &&
              ui_search_scene_hit_test(scene, center(expected.time).x, center(expected.time).y).action ==
                  UiSearchAction::none &&
              ui_search_scene_hit_test(scene, center(expected.seek).x, center(expected.seek).y).action ==
                  UiSearchAction::none,
          "audio Search: pointer and keyboard Save own the exact download footer while inert time and seek "
          "never open a generic attachment");
    fixture.current = false;
    check(!ui_search_scene_rich_preview_geometry(scene, target, &bounds, &clip),
          "audio Search: retired accepted source rejects stale card geometry without image fallback");
    ui_search_scene_release(&scene);
}
void rich_layout(void (*check)(bool, const char*) noexcept) noexcept {
    constexpr auto filename = utf8_literal("voice.ogg");
    constexpr auto mime = utf8_literal("audio/ogg");
    u8 text[64]{};
    copy_bytes(text, filename.data, filename.bytes);
    copy_bytes(text + filename.bytes, mime.data, mime.bytes);
    MessageRichStore store{};
    store.text = text;
    store.text_capacity = sizeof(text);
    store.text_used = filename.bytes + mime.bytes;
    MessageRichRecord rich{};
    MessageRichAttachment fixture_attachments[1]{};
    rich.attachments = fixture_attachments;
    rich.attachment_count = 1;
    auto& attachment = rich.attachments[0];
    attachment.id = 17;
    attachment.filename = {0, static_cast<u16>(filename.bytes), 0};
    attachment.content_type = {filename.bytes, static_cast<u16>(mime.bytes), 0};
    attachment.url = attachment.filename;
    check(message_rich_attachment_audio_supported(store, attachment) &&
              message_rich_attachment_format(store, attachment) == MediaAttachmentFormat::ogg_opus,
          "audio rich layout: actual shared Ogg eligibility selects audio rather than an image preview");
    float width = 71, height = 72;
    check(!message_rich_attachment_preview_extent(store, attachment, 320, &width, &height) && width == 71 &&
              height == 72,
          "audio rich layout: Ogg never reserves a fabricated image aspect ratio");
    const auto ordinary = message_rich_estimate_item_layout(&store, &rich, 320);
    rich.voice_message = true;
    const auto voice = message_rich_estimate_item_layout(&store, &rich, 160);
    check(ordinary.heights[0] == 112 && voice.heights[0] == 86 &&
              message_rich_aggregate_layout(rich, voice, 160).attachments == 86,
          "audio rich layout: enclosing voice flag omits autogenerated filename row at compact width");
    check(message_rich_estimate_item_layout(&store, &rich, 159).heights[0] == 42,
          "audio rich layout: insufficient control width keeps generic accessible attachment fallback");
    attachment.content_type = {};
    const Utf8View names[]{utf8_literal("Original song.mp3"), utf8_literal("Original recording.wav")};
    const Utf8View types[]{utf8_literal("audio/mpeg"), utf8_literal("audio/wav")};
    const MediaAttachmentFormat formats[]{MediaAttachmentFormat::mp3, MediaAttachmentFormat::wav};
    rich.voice_message = false;
    for (u32 i = 0; i < 2; ++i) {
        copy_bytes(text, names[i].data, names[i].bytes);
        copy_bytes(text + names[i].bytes, types[i].data, types[i].bytes);
        store.text_used = names[i].bytes + types[i].bytes;
        attachment.filename = {0, static_cast<u16>(names[i].bytes), 0};
        attachment.content_type = {names[i].bytes, static_cast<u16>(types[i].bytes), 0};
        attachment.state = 8;
        attachment.duration_ms = 120000;
        const auto layout = message_rich_estimate_item_layout(&store, &rich, 160);
        width = 71;
        height = 72;
        check(message_rich_attachment_audio_supported(store, attachment) &&
                  message_rich_attachment_format(store, attachment) == formats[i] &&
                  layout.heights[0] == 112 &&
                  !message_rich_attachment_preview_extent(store, attachment, 320, &width, &height) &&
                  width == 71 && height == 72,
              "audio rich layout: admitted MP3/WAV uses the compact ordinary filename card without "
              "fabricated image geometry");
    }
    attachment.content_type = {};
    constexpr auto unsupported = utf8_literal("song.aac");
    copy_bytes(text, unsupported.data, unsupported.bytes);
    store.text_used = unsupported.bytes;
    attachment.filename.bytes = static_cast<u16>(unsupported.bytes);
    check(
        !message_rich_attachment_audio_supported(store, attachment) &&
            message_rich_estimate_item_layout(&store, &rich, 320).heights[0] == 42,
        "audio rich layout: unsupported AAC remains generic Open and Save without false playback controls");
    copy_bytes(text, filename.data, filename.bytes);
    store.text_used = filename.bytes;
    attachment.filename.bytes = static_cast<u16>(filename.bytes);
    rich.voice_message = true;
    WidgetFixture fixture{};
    MessageSceneRowWidget row{};
    const bool initialized = fixture.initialize();
    row.rich_item_layout = message_rich_estimate_item_layout(&store, &rich, 160);
    row.rich_layout = message_rich_aggregate_layout(rich, row.rich_item_layout, 160);
    const bool built =
        initialized && message_rich_widgets_build(&fixture.manager, fixture.root, 100, &rich, &row);
    const auto* widget = built ? ui_widget_manager_resolve(fixture.manager, row.rich_items[0]) : nullptr;
    check(built && fixture.manager.count == 4 && widget && (widget->flags & ui_widget_focusable) &&
              message_rich_widgets_structure_matches(row, &rich),
          "audio rich widgets: one existing attachment root remains focusable with no per-control retained "
          "children");
}
} // namespace

void test_audio_controls(void (*check)(bool, const char*) noexcept) noexcept {
    auto state = playback();
    UiAudioControlsStyle style{};
    UiAudioControlsProjection projection{};
    const auto project = [&](float width, bool expanded = false) noexcept {
        return ui_audio_controls_project(
            state, {10, 20, 10 + width, 20 + ui_audio_card_height(style, width, state.voice)}, style,
            kDefaultTheme, &projection, expanded);
    };
    check(project(480) && !projection.geometry.compact && projection.controls[1].value == 2500 &&
              utf8_equal(ui_audio_controls_label(projection, UiAudioControl::time),
                         utf8_literal("0:30 / 2:00")),
          "audio controls: actual duration and elapsed facts produce a compact filename card with real "
          "progress");
    const auto copied = projection;
    projection.time_text[0] = '9';
    check(
        ui_audio_controls_label(copied, UiAudioControl::time).data == copied.time_text &&
            utf8_equal(ui_audio_controls_label(copied, UiAudioControl::time), utf8_literal("0:30 / 2:00")),
        "audio controls: copied projection time accessor never points into its prior owner");
    state.elapsed_ms = 3600000;
    state.duration_ms = 7200000;
    check(project(480) && ui_rect_width(projection.geometry.time) == 112 &&
              utf8_equal(ui_audio_controls_label(projection, UiAudioControl::time),
                         utf8_literal("1:00:00 / 2:00:00")) &&
              project(160) && ui_rect_height(projection.geometry.time) == 32 &&
              utf8_equal(ui_audio_controls_label(projection, UiAudioControl::time),
                         utf8_literal("1:00:00\n2:00:00")),
          "audio controls: hour-scale clocks use a natural wide slot and explicit compact two-line "
          "elapsed/duration without changing card height");
    state = playback();
    bool geometry_valid = true;
    constexpr float widths[]{160, 192, 232, 320, 480, 640};
    constexpr bool voices[]{false, true};
    for (bool voice : voices) {
        state.voice = voice;
        for (float width : widths) {
            geometry_valid = geometry_valid && project(width) && projection.geometry.footer_rows == 1 &&
                             ui_rect_height(projection.geometry.bounds) == (voice ? 86 : 112) &&
                             ui_rect_width(projection.geometry.bounds) <= style.maximum_width;
            for (const auto& descriptor : projection.controls)
                if (descriptor.visible)
                    geometry_valid =
                        geometry_valid && contained(projection.geometry.bounds, descriptor.bounds);
            for (u32 i = 0; i < kUiAudioControlCount; ++i)
                for (u32 j = i + 1; j < kUiAudioControlCount; ++j)
                    if (projection.controls[i].visible && projection.controls[j].visible)
                        geometry_valid = geometry_valid && separate(projection.controls[i].bounds,
                                                                    projection.controls[j].bounds);
        }
    }
    check(geometry_valid, "audio controls: 160 through 640 DIP ordinary/voice cards keep all visible "
                          "controls inside separate bounds and cap width");
    auto custom_style = style;
    custom_style.padding = 10;
    custom_style.target = 28;
    custom_style.gap = 4;
    custom_style.title_height = 18;
    custom_style.volume_thumb_extent = 8;
    auto custom_theme = kLightTheme;
    custom_theme.accent = {.2f, .4f, .6f, 1};
    // Semantic tokens (emphasis.outline drives the played bar) follow the core accent.
    custom_theme = ui_theme_resolve_defaults(custom_theme);
    UiAudioControlsProjection themed{};
    check(ui_audio_controls_project(state,
                                    {0, 0, 232, ui_audio_card_height(custom_style, 232, state.voice)},
                                    custom_style, custom_theme, &themed) &&
              ui_rect_height(themed.geometry.bounds) == 80 && themed.volume_thumb_inset == 4 &&
              themed.played.red == custom_theme.accent.red &&
              themed.waveform.red == custom_theme.text_secondary.red,
          "audio controls: host metrics and semantic theme tokens change real geometry and colors "
          "independently");
    state.voice = true;
    check(project(160) && projection.controls[2].visible && !projection.controls[4].visible &&
              projection.controls[4].enabled && project(160, true) && !projection.controls[2].visible &&
              projection.controls[4].visible,
          "audio controls: narrow volume replaces time without hiding seek or download and keeps keyboard "
          "volume available");
    UiAudioAction request{};
    check(ui_audio_controls_pointer_action(projection, center(projection.geometry.play_pause), &request) &&
              request.control == UiAudioControl::play_pause && !request.value && state.playing &&
              ui_audio_action_current(state, request),
          "audio controls: pointer pause is a fenced request and never changes authoritative playback");
    check(ui_audio_controls_pointer_action(projection, center(projection.geometry.seek), &request) &&
              request.position_ms == 60000 &&
              ui_audio_controls_range_action(projection, UiAudioControl::seek, {-100, 0}, &request) &&
              !request.position_ms &&
              ui_audio_controls_range_action(projection, UiAudioControl::seek, {10000, 0}, &request) &&
              request.position_ms == 120000,
          "audio controls: captured seek maps real duration and clamps both endpoints without overflow");
    const auto volume_track = ui_audio_controls_range_track(projection, UiAudioControl::volume);
    check(volume_track.left == projection.geometry.volume.left + 3 &&
              volume_track.right == projection.geometry.volume.right - 3 &&
              ui_audio_controls_range_action(projection, UiAudioControl::volume,
                                             {volume_track.left, volume_track.top}, &request) &&
              request.volume == 0 &&
              ui_audio_controls_range_action(projection, UiAudioControl::volume,
                                             {volume_track.right, volume_track.top}, &request) &&
              request.volume == 100,
          "audio controls: volume actions and painted thumb centers share exact themed endpoints");
    UiAudioControl focus = UiAudioControl::none;
    request.control = UiAudioControl::download;
    check(ui_audio_controls_key(projection, UiAudioKey::next, &focus, &request) &&
              focus == UiAudioControl::play_pause && request.control == UiAudioControl::none &&
              !ui_audio_action_current(state, request),
          "audio controls: focus-only navigation clears a prior actionable request");
    focus = UiAudioControl::volume;
    check(ui_audio_controls_key(projection, UiAudioKey::decrease, &focus, &request) &&
              request.volume == 35 &&
              ui_audio_controls_key(projection, UiAudioKey::end, &focus, &request) && request.volume == 100,
          "audio controls: keyboard volume uses bounded actual percentage requests");
    auto changed = state;
    ++changed.revision;
    check(!ui_audio_action_current(changed, request),
          "audio controls: stale interaction revision refuses a retained action");
    state.loading = true;
    check(project(160) && !projection.controls[0].enabled && !projection.controls[1].visible &&
              projection.controls[6].visible && projection.controls[5].enabled &&
              ui_rect_height(projection.geometry.bounds) == 86,
          "audio controls: loading occupies waveform space, gates transport and preserves download without "
          "timeline reflow");
    state.loading = false;
    state.error = true;
    check(
        project(160) && projection.controls[0].enabled &&
            utf8_equal(ui_audio_controls_label(projection, UiAudioControl::play_pause), style.retry) &&
            ui_audio_controls_pointer_action(projection, center(projection.geometry.play_pause),
                                             &request) &&
            request.value && ui_audio_action_current(state, request),
        "audio controls: explicit error Retry is admitted only by the host's current can_play capability");
    state.can_play = false;
    check(project(160) && !projection.controls[0].enabled && !ui_audio_action_current(state, request),
          "audio controls: withdrawn retry capability rejects both new input and a captured prior request");
    state.can_play = true;
    check(
        project(160) &&
            projection.controls[6].foreground.red ==
                ui_foreground_color(kDefaultTheme, kDefaultTheme.notification).red &&
            ui_audio_controls_pointer_action(projection, center(projection.geometry.download), &request) &&
            request.control == UiAudioControl::download,
        "audio controls: unavailable playback retains a separate working Download action and semantic "
        "error text");
    state.error = false;
    state.has_duration = false;
    state.duration_ms = 0;
    check(project(160) && !projection.controls[1].enabled &&
              utf8_equal(ui_audio_controls_label(projection, UiAudioControl::time),
                         utf8_literal("0:30\n--:--")) &&
              !projection.playback.waveform.count,
          "audio controls: unknown duration and missing waveform remain explicit without invented seek "
          "capability or amplitudes");
    state.has_duration = true;
    state.elapsed_ms = 0;
    check(project(160) &&
              utf8_equal(ui_audio_controls_label(projection, UiAudioControl::time),
                         utf8_literal("0:00\n0:00")) &&
              !projection.controls[1].enabled,
          "audio controls: known zero duration differs from unknown and cannot seek");
    const auto previous = projection;
    u8 too_many[257]{};
    state.waveform = {too_many, 257};
    check(!project(160) && bytes_equal(&previous, &projection, sizeof(previous)),
          "audio controls: oversized waveform refuses projection transactionally");
    state = playback();
    state.voice = true;
    style.minimum_width = 1;
    check(!project(160) && bytes_equal(&previous, &projection, sizeof(previous)),
          "audio controls: invalid themed geometry refuses before changing the previous projection");
    style = {};
    WidgetFixture fixture{};
    UiAudioScene scene{};
    const bool initialized = fixture.initialize();
    const auto scene_project = [&](float width = 160) noexcept {
        return initialized && ui_audio_scene_project(&scene, state, {0, 0, width, 86}, {0, 0, width, 86},
                                                     {&fixture.manager, fixture.root},
                                                     UiAudioSourceSurface::timeline, style, kLightTheme);
    };
    check(scene_project() && ui_audio_scene_focus(&scene, UiAudioControl::volume, true) &&
              scene.projection.controls[4].visible && !scene.projection.controls[2].visible &&
              scene_project() && scene.focus == UiAudioControl::volume &&
              scene.projection.controls[4].visible,
          "audio scene: real widget ownership preserves keyboard volume and expanded geometry across "
          "current reprojection");
    const auto warm_geometry = scene.projection.geometry;
    bool unchanged = true;
    for (u32 i = 0; i < 64; ++i)
        unchanged = unchanged && scene_project() && scene.focus == UiAudioControl::volume &&
                    scene.keyboard_focus && scene.capture == UiAudioControl::none &&
                    ui_rect_equal(scene.projection.geometry.bounds, warm_geometry.bounds) &&
                    ui_rect_equal(scene.projection.geometry.volume, warm_geometry.volume) &&
                    scene.projection.controls[4].visible && !scene.projection.controls[2].visible &&
                    utf8_equal(ui_audio_controls_label(scene.projection, UiAudioControl::time),
                               utf8_literal("0:30\n2:00"));
    check(unchanged, "audio scene: sixty-four unchanged pure projections retain geometry, labels and "
                     "semantic focus without allocations or clocks");
    auto result =
        ui_audio_scene_pointer(&scene, UiAudioPointerEvent::down, center(scene.projection.geometry.seek));
    const bool captured =
        result.request_capture && result.action_requested && result.action.position_ms == 60000;
    ++state.elapsed_ms;
    check(captured && scene_project() && scene.capture == UiAudioControl::seek &&
              ui_audio_scene_pointer(&scene, UiAudioPointerEvent::up, {1000, 0}).release_capture,
          "audio scene: elapsed ticks preserve current range capture and release clamps safely outside "
          "card bounds");
    ui_audio_scene_pointer(&scene, UiAudioPointerEvent::down, center(scene.projection.geometry.play_pause));
    ++state.revision;
    check(
        scene_project() && scene.capture == UiAudioControl::none &&
            !ui_audio_scene_pointer(&scene, UiAudioPointerEvent::up,
                                    center(scene.projection.geometry.play_pause))
                 .action_requested,
        "audio scene: replacement authority retires held button release before it can affect new playback");
    ui_audio_scene_pointer(&scene, UiAudioPointerEvent::down, center(scene.projection.geometry.seek));
    check(scene_project(232) && scene.capture == UiAudioControl::none,
          "audio scene: resize cancels a captured seek whose geometry changed");
    const auto seek_bounds = scene.projection.geometry.seek;
    const UiRect clipped{seek_bounds.left + ui_rect_width(seek_bounds) * .25f, 0, 232, seek_bounds.bottom};
    const bool clipped_ready =
        ui_audio_scene_project(&scene, state, {0, 0, 232, 86}, clipped, {&fixture.manager, fixture.root},
                               UiAudioSourceSurface::timeline, style, kLightTheme);
    ui_audio_scene_pointer(&scene, UiAudioPointerEvent::down, center(seek_bounds));
    const auto clipped_up = ui_audio_scene_pointer(&scene, UiAudioPointerEvent::up, {-100, -100});
    UiAudioControl clipped_focus = UiAudioControl::none;
    bool clipped_keys = clipped_ready;
    for (u32 i = 0; i < 5; ++i) {
        const auto key = ui_audio_scene_key(&scene, UiAudioKey::next);
        clipped_focus = scene.focus;
        clipped_keys = clipped_keys && key.handled && clipped_focus == UiAudioControl::seek;
    }
    check(clipped_ready && clipped_up.release_capture && clipped_up.action_requested &&
              clipped_up.action.position_ms == 30000 && clipped_keys,
          "audio scene: clipped range clamps to visible quarter and keyboard skips footer controls outside "
          "the current clip");
    ui_widget_manager_reset(&fixture.manager);
    check(ui_audio_scene_hit(scene, center(scene.projection.geometry.download)) == UiAudioControl::none &&
              !ui_audio_scene_key(&scene, UiAudioKey::activate).action_requested && !scene_project(),
          "audio scene: reset widget generation rejects pointer, keyboard and reprojection without "
          "touching stale owners");
    rich_layout(check);
    search_geometry(check);
    search_geometry(check, utf8_literal("Original song.mp3"), utf8_literal("audio/mpeg"), false,
                    MediaAttachmentFormat::mp3);
}
} // namespace maicord
