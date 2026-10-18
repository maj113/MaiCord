#include "application_downloads_inactive.hpp"
#include "application_state.hpp"
#include "application_video.hpp"
#include "application_settings.hpp"
#include "application_friends.hpp"
#include "application_composer_text_input.hpp"
#include "application_search.hpp"
#include "maicord/media/gpu_video_frame.hpp"
#include "maicord/ui/audio_presentation.hpp"
#include "test_output.hpp"

#include "maicord/core/win32_user.hpp"
#include "maicord/sys/winsock.hpp"

namespace maicord::video_focus_runtime_test {
u32 confirmations, media_refreshes;
UiSearchScene* mutable_search_scene;
MessageExplicitFilter media_filter = MessageExplicitFilter::show;
bool media_reveal_allowed = true;
MessageMediaPolicyResult query_media(const void*, const MessageMediaOwner&) noexcept {
    return {media_filter, media_reveal_allowed};
}

using namespace app_detail;

struct AccountFixture {
    AccountVideoPlaybackStatus status{};
    AccountVideoFrame queued_frame{};
    u32 selections{}, commands{}, network_requests{}, gpu_publications{};
    u32 frame_leases{}, frame_acknowledgements{};
    bool frame_pending{};
    AccountMediaAttachmentRequest last_request{};
} account;
u32 failures{}, text_publications{};
bool settings;
bool selection_busy;
bool search_current;
bool deferred_stop, stop_pending;
u32 viewer_opens;
bool viewer_allowed = true;
UiRect viewer_preview{40, 60, 600, 375};
u32 downloads{};
u64 download_message{}, download_attachment{};
bool download_snapshot{};
ApplicationSearchSourceView search_view{};
AccountPlaybackKind audio_kind(MediaAttachmentFormat format) noexcept {
    if (format == MediaAttachmentFormat::mp3)
        return AccountPlaybackKind::mp3;
    if (format == MediaAttachmentFormat::wav)
        return AccountPlaybackKind::wave_pcm;
    return AccountPlaybackKind::ogg_opus;
}
CdnImageType audio_type(MediaAttachmentFormat format) noexcept {
    if (format == MediaAttachmentFormat::mp3)
        return CdnImageType::mp3;
    if (format == MediaAttachmentFormat::wav)
        return CdnImageType::wav;
    return CdnImageType::ogg_opus;
}

void check(bool passed, const char* label) noexcept {
    test_check(&failures, passed, label);
}

AccountVideoResult WINAPI select(void*, const AccountVideoSelection* selection) noexcept {
    ++account.selections;
    if (selection_busy)
        return {AccountVideoError::busy};
    account.status = {};
    account.status.selected = true;
    account.status.key = selection->key;
    auto& playback = account.status.playback;
    playback.identity = selection->identity;
    playback.identity.content_identity = 0x51;
    playback.identity.cache_generation = 1;
    playback.duration_100ns = 120'000 * 10'000ll;
    playback.position_100ns = 3'000 * 10'000ll;
    playback.applied_volume = .4f;
    playback.has_audio = playback.volume_valid = true;
    const auto format = media_asset_attachment_format(selection->key);
    playback.kind =
        selection->key.kind == MediaAssetKind::attachment && media_attachment_format_audio_supported(format)
            ? audio_kind(format)
            : AccountPlaybackKind::mp4;
    return {AccountVideoError::none, 0, true, true};
}
AccountVideoResult WINAPI control(void*, const AccountVideoControl* command) noexcept {
    if (!account_video_identity_equal(account.status.playback.identity, command->identity) ||
        command->command_serial <= account.status.playback.desired_command)
        return {AccountVideoError::stale};
    ++account.commands;
    auto& playback = account.status.playback;
    const u64 observed_before = playback.observed_command;
    playback.desired_command = playback.observed_command = command->command_serial;
    switch (command->command) {
    case AccountVideoCommand::play:
        playback.playing = true;
        playback.paused = false;
        break;
    case AccountVideoCommand::pause:
        playback.playing = false;
        playback.paused = true;
        break;
    case AccountVideoCommand::seek:
        playback.position_100ns = command->seek_time_100ns;
        break;
    case AccountVideoCommand::volume:
        playback.desired_volume = playback.applied_volume = command->volume;
        break;
    case AccountVideoCommand::mute:
        playback.muted = command->muted;
        break;
    case AccountVideoCommand::stop:
        if (deferred_stop) {
            playback.observed_command = observed_before;
            stop_pending = true;
            return {AccountVideoError::none, 0, true, false};
        }
        account.status.selected = false;
        playback.playing = false;
        break;
    default:
        break;
    }
    return {AccountVideoError::none, 0, true, true};
}
AccountVideoPlaybackStatus WINAPI status(void*) noexcept {
    if (stop_pending && !account.frame_pending) {
        account.status.selected = false;
        account.status.playback.playing = false;
        account.status.playback.paused = true;
        account.status.playback.observed_command = account.status.playback.desired_command;
        stop_pending = false;
    }
    return account.status;
}
bool WINAPI take(void*, AccountVideoFrame* frame) noexcept {
    if (!account.frame_pending || !frame)
        return false;
    *frame = account.queued_frame;
    account.frame_pending = false;
    ++account.frame_leases;
    return true;
}
bool WINAPI acknowledge(void*, const AccountVideoFrame* frame) noexcept {
    if (!frame || !account_video_identity_equal(frame->identity, account.queued_frame.identity) ||
        !media_asset_key_equal(frame->key, account.queued_frame.key))
        return false;
    ++account.frame_acknowledgements;
    return true;
}
AccountMediaRequestResult WINAPI request(void*, const AccountMediaAttachmentRequest* requested, u64,
                                         u64) noexcept {
    ++account.network_requests;
    if (requested)
        account.last_request = *requested;
    return {};
}

struct SceneFixture {
    MessageRecord record[1]{};
    u32 order[1]{};
    u8 text[32]{};
    MessageRichRecord rich_record[2]{};
    u16 rich_order[1]{};
    u8 rich_text[512]{};
    u32 heights[1]{};
    u64 fenwick[1]{};
    MessageSceneRowWidget row[1]{};
    MessageSceneRowWidget snapshot_row[1]{};
    UiWidget widgets[kMessageSceneWidgetsPerRow + 1]{};
    UiLayoutNode nodes[kMessageSceneWidgetsPerRow + 1]{};
};

bool initialize_scene(SceneFixture& fixture,
                      Utf8View source = utf8_literal("https://example.com/focus-fixture.mp4"),
                      bool forwarded = false, bool audio = false, bool voice = false,
                      MediaAttachmentFormat audio_format = MediaAttachmentFormat::ogg_opus) noexcept {
    auto& view = g_app.message_view;
    auto& rich = g_app.account_ui->message_rich_store;
    if (!message_store_initialize(&view.store, {fixture.record, 1}, {fixture.order, 1},
                                  {fixture.text, 32}) ||
        !message_store_upsert(&view.store, {1, 7, 9, 0, 0, utf8_literal(""), 0}).ok() ||
        !message_rich_store_initialize(&rich, {fixture.rich_record, 2}, {fixture.rich_order, 1},
                                       {fixture.rich_text, 512}) ||
        !ui_variable_list_initialize(&view.list, {fixture.heights, 1}, {fixture.fenwick, 1}, 1,
                                     ui_extent_units(57)) ||
        !message_timeline_initialize(&view.timeline, &view.store, &view.list, ui_extent_units(57),
                                     ui_extent_units(480), false) ||
        !message_timeline_bind_rich_store(&view.timeline, &rich) ||
        !message_scene_initialize(&g_app.message_scene, &view.timeline, {fixture.row, 1},
                                  {fixture.widgets, kMessageSceneWidgetsPerRow + 1},
                                  {fixture.nodes, kMessageSceneWidgetsPerRow + 1}) ||
        !message_scene_bind_snapshot_rows(&g_app.message_scene, {fixture.snapshot_row, 1}))
        return false;
    GatewayMessageAttachment attachment{};
    attachment.id = 70;
    attachment.filename = utf8_literal("focus-fixture.mp4");
    attachment.content_type = utf8_literal("video/mp4");
    attachment.url = source;
    attachment.width = 320;
    attachment.height = 240;
    attachment.has_width = attachment.has_height = true;
    if (audio) {
        attachment.filename =
            audio_format == MediaAttachmentFormat::mp3   ? utf8_literal("original audio title.mp3")
            : audio_format == MediaAttachmentFormat::wav ? utf8_literal("original audio title.wav")
                                                         : utf8_literal("original audio title.opus");
        attachment.content_type = audio_format == MediaAttachmentFormat::mp3   ? utf8_literal("audio/mpeg")
                                  : audio_format == MediaAttachmentFormat::wav ? utf8_literal("audio/wav")
                                                                               : utf8_literal("audio/ogg");
        attachment.width = attachment.height = 0;
        attachment.has_width = attachment.has_height = false;
        if (audio_format == MediaAttachmentFormat::ogg_opus) {
            attachment.waveform = utf8_literal("AAECAw==");
            attachment.has_duration = true;
            attachment.duration_ms = 15000;
        }
    }
    GatewayMessageEvent event{};
    event.id = 1;
    event.fields = gateway_message_field_attachments;
    event.attachments = {&attachment, 1};
    if (voice) {
        event.fields |= gateway_message_field_flags;
        event.flags = kGatewayMessageVoiceFlag;
    }
    GatewayMessageAttachment snapshot_attachment = attachment;
    snapshot_attachment.id = 71;
    if (forwarded) {
        event.fields |= gateway_message_field_message_snapshots | gateway_message_field_message_reference;
        event.reference.type = 1;
        event.reference.channel_id = 8;
        event.reference.message_id = 99;
        event.snapshot.present = true;
        event.snapshot.content = utf8_literal("Forwarded fixture");
        event.snapshot.fields = gateway_message_field_content | gateway_message_field_attachments;
        event.snapshot.attachments = {&snapshot_attachment, 1};
        if (voice) {
            event.snapshot.fields |= gateway_message_field_flags;
            event.snapshot.flags = kGatewayMessageVoiceFlag;
        }
    }
    bool changed = false;
    return message_rich_store_apply(&rich, 1, fixture.record[0].revision, event).ok() &&
           message_scene_update(&g_app.message_scene, {0, 0, 640, 480}).ok() &&
           message_scene_set_text_metrics(&g_app.message_scene, 0, 0, 0, false, nullptr, nullptr, {},
                                          &changed) &&
           message_scene_update(&g_app.message_scene, {0, 0, 640, 480}).ok();
}

LPARAM point(UiRect bounds) noexcept {
    return MAKELPARAM(static_cast<short>((bounds.left + bounds.right) * .5f),
                      static_cast<short>((bounds.top + bounds.bottom) * .5f));
}
bool pump() noexcept {
    bool repaint = false;
    return application_video_pump(&repaint);
}
bool dormant() noexcept {
    const auto* scene = g_app.renderer.video_scene;
    return scene && scene->active && !scene->keyboard_focus && scene->focus == UiVideoControl::none &&
           scene->controls_transition.value == 0 && !application_video_next_wake();
}
void release() noexcept {
    application_video_close();
    message_scene_release(&g_app.message_scene);
    g_app.account.instance = nullptr;
    if (g_app.account_ui)
        VirtualFree(g_app.account_ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
    if (g_app.window)
        DestroyWindow(g_app.window);
    g_app.window = nullptr;
}

void run_search_video() noexcept {
    const MessageViewRuntime prior_view = g_app.message_view;
    const MessageRichStore prior_rich = g_app.account_ui->message_rich_store;
    application_video_close();
    message_scene_release(&g_app.message_scene);
    SceneFixture fixture{};
    const bool initialized =
        initialize_scene(fixture, utf8_literal("https://example.com/search-forward.mp4"), true);
    MessageViewRuntime bank = g_app.message_view;
    bank.timeline.store = &bank.store;
    bank.timeline.list = &bank.list;
    message_scene_release(&g_app.message_scene);
    g_app.message_view = {}; // Accepted Search playback must not use the live message store.
    const u64 prior_channel = g_app.account_ui->active_channel_id;
    g_app.account_ui->active_channel_id = 99;
    search_current = true;
    search_view = {};
    search_view.ticket = {1, fixture.record[0].revision, 0};
    search_view.message = &fixture.record[0];
    search_view.store = &bank.store;
    search_view.handle = message_store_ordered_handle(bank.store, 0);
    search_view.rich_store = &g_app.account_ui->message_rich_store;
    UiSearchSource source{};
    source.count = source.revision = 1;
    source.rich_timeline = &bank.timeline;
    source.current = +[](void*) noexcept { return search_current; };
    source.resolve = +[](void*, u32 i, UiSearchRow* row) noexcept {
        if (i || !row)
            return false;
        *row = {7, 1, utf8_literal("Original author"), utf8_literal("#search"), {}, {}, true, true};
        return true;
    };
    UiSearchContent content{};
    content.edit.active = content.panel = true;
    content.phase = UiSearchPhase::ready;
    content.revision = 1;
    UiSearchScene scene{};
    const bool ready = initialized && ui_search_scene_update(&scene, &source, &content, {0, 0, 1400, 1200},
                                                             {0, 0, 1400, 48});
    mutable_search_scene = &scene;
    g_app.renderer.search_scene = &scene;
    UiSearchTarget target{};
    target.action = UiSearchAction::rich_item;
    target.channel_id = 7;
    target.message_id = 1;
    target.source_revision = source.revision;
    target.source_index = 0;
    target.rich_kind = MessageSceneRichItemKind::attachment;
    target.snapshot = true;
    const u32 selections = account.selections;
    bool opened = ready && application_video_open_search(target) && pump();
    check(opened && !g_app.message_view.store.count && application_video_search_active() &&
              account.selections == selections + 1 && media_asset_search_snapshot(account.status.key) &&
              account.status.key.id == 71 && account.status.playback.has_audio &&
              account.status.playback.playing && g_app.renderer.video_scene &&
              g_app.renderer.video_scene_search_result,
          "search video runtime: independent accepted child source selects shared native player with audio "
          "and Search paint ownership");
    if (opened && g_app.renderer.video_scene) {
        const auto& native = *g_app.renderer.video_scene;
        check(!native.play_only && native.projection.controls[1].enabled &&
                  native.projection.controls[4].enabled &&
                  ui_rect_width(native.preview) <
                      UiVideoControlsStyle{}.normal_width + 2 * native.style.inset &&
                  ui_rect_has_area(ui_rect_intersection(native.projection.controls[1].bounds, native.clip)),
              "search video runtime: actual forwarded card width retains full-sized native seek and volume "
              "controls");
        g_app.keyboard_focus_visible = true;
        // The host initially assigns Play/Pause when the newly selected
        // full-control player acquires focus; Tab advances from that control.
        const bool focused = pump() && g_app.renderer.video_scene &&
                             g_app.renderer.video_scene->focus == UiVideoControl::play_pause &&
                             g_app.renderer.video_scene->keyboard_focus;
        const bool paused = focused && application_video_key(VK_SPACE, false, false) && pump();
        check(paused && account.status.playback.paused,
              "search video runtime: native keyboard Play Pause routes to actual Search audio owner");
        const bool seek = application_video_key(VK_TAB, false, false) &&
                          application_video_key(VK_END, false, false) && pump();
        check(seek && account.status.playback.position_100ns == account.status.playback.duration_100ns,
              "search video runtime: native seek control updates exact accepted playback source");
        const bool volume = application_video_key(VK_TAB, false, false) &&
                            application_video_key(VK_TAB, false, false) &&
                            application_video_key(VK_HOME, false, false) && pump();
        check(volume && account.status.playback.applied_volume == 0,
              "search video runtime: native volume control reaches existing audio session");
        ++source.revision;
        target.source_revision = source.revision;
        const bool layout =
            ui_search_scene_update(&scene, &source, &content, {0, 0, 1400, 1200}, {0, 0, 1400, 48}) &&
            pump();
        check(layout && application_video_search_active() && account.selections == selections + 1,
              "search video runtime: presentation revision preserves immutable publication without "
              "reselection");
        check(application_video_open_search(target) && pump() && account.status.playback.playing,
              "search video runtime: current rich target resumes exact source after control use");
        UiThreadBrowserScene browser{};
        UiThreadBrowserView browser_view{};
        browser_view.title = utf8_literal("Search video browser coverage");
        browser_view.source_publication = browser_view.revision = 1;
        browser_view.parent_id = 7;
        check(ui_thread_browser_scene_update(&browser, &browser_view, {0, 0, 1400, 1200}),
              "search video browser runtime: actual current content panel projection initializes");
        const auto* searched_owner = g_app.video;
        const u32 before_browser = account.commands;
        g_app.renderer.thread_browser_scene = &browser;
        check(!application_video_key(VK_SPACE, false, false) && g_app.video == searched_owner && pump() &&
                  account.status.playback.paused && account.commands == before_browser + 1 &&
                  application_video_search_active() && !g_app.renderer.video_scene &&
                  !g_app.renderer.video_scene_search_result && !application_video_next_wake() &&
                  !application_video_open_search(target),
              "search video browser runtime: covered key preserves source while current browser pauses "
              "audio and blocks controls reopen and wakes");
        browser.active = false;
        g_app.renderer.thread_browser_scene = nullptr;
        check(pump() && account.status.playback.paused && !application_video_key(VK_SPACE, false, false) &&
                  application_video_open_search(target) && pump() && account.status.playback.playing &&
                  account.selections == selections + 1,
              "search video browser runtime: dismissal restores current geometry with explicit same-source "
              "resume");
        UiQuickSwitcherScene modal{};
        modal.active = true;
        g_app.renderer.quick_switcher_scene = &modal;
        check(
            pump() && account.status.playback.paused && !g_app.renderer.video_scene &&
                !g_app.renderer.video_scene_search_result && !application_video_next_wake() &&
                !application_video_open_search(target),
            "search video runtime: active modal pauses Search audio and removes covered controls and wake");
        g_app.renderer.quick_switcher_scene = nullptr;
        check(pump() && account.status.playback.paused && application_video_open_search(target) && pump() &&
                  account.status.playback.playing,
              "search video runtime: modal dismissal requires explicit current-source resume");
        search_current = false;
        browser.active = true;
        g_app.renderer.thread_browser_scene = &browser;
        const u32 before_stale = account.commands;
        check(!application_video_key(VK_SPACE, false, false) && pump() &&
                  !application_video_search_active() && !g_app.renderer.video_scene &&
                  !g_app.renderer.video_scene_search_result && !account.status.playback.playing &&
                  account.commands == before_stale + 1 && !application_video_next_wake() &&
                  !application_video_open_search(target),
              "search video runtime: retired publication stops audio controls capture and wake and rejects "
              "stale reopen");
        browser.active = false;
        g_app.renderer.thread_browser_scene = nullptr;
        check(pump() && !g_app.video && !application_video_open_search(target) &&
                  !account.status.playback.playing && account.commands == before_stale + 1,
              "search video browser runtime: closing browser cannot restore a retired Search playback "
              "source");
        const u32 commands = account.commands;
        bool idle = true;
        for (u32 frame = 0; frame < 8; ++frame)
            idle = pump() && idle;
        check(idle && account.commands == commands && account.selections == selections + 1 &&
                  !application_video_next_wake(),
              "search video runtime: 8 retired frames perform no decoder commands or deadlines");
        ui_thread_browser_scene_release(&browser);
    }
    application_video_close();
    mutable_search_scene = nullptr;
    g_app.renderer.search_scene = nullptr;
    ui_search_scene_release(&scene);
    search_current = false;
    search_view = {};
    g_app.account_ui->message_rich_store = prior_rich;
    g_app.message_view = prior_view;
    g_app.message_view.timeline.store = &g_app.message_view.store;
    g_app.message_view.timeline.list = &g_app.message_view.list;
    g_app.account_ui->active_channel_id = prior_channel;
}

void run_search_audio(MediaAttachmentFormat format = MediaAttachmentFormat::ogg_opus) noexcept {
    const auto prior_view = g_app.message_view;
    const auto prior_rich = g_app.account_ui->message_rich_store;
    application_video_close();
    message_scene_release(&g_app.message_scene);
    SceneFixture fixture{};
    const bool initialized =
        initialize_scene(fixture, utf8_literal("https://example.com/forwarded-search.opus"), true, true,
                         format == MediaAttachmentFormat::ogg_opus, format);
    auto bank = g_app.message_view;
    bank.timeline.store = &bank.store;
    bank.timeline.list = &bank.list;
    message_scene_release(&g_app.message_scene);
    g_app.message_view = {};
    const auto prior_channel = g_app.account_ui->active_channel_id;
    g_app.account_ui->active_channel_id = 99;
    search_current = true;
    search_view = {};
    search_view.ticket = {1, fixture.record[0].revision, 0};
    search_view.message = &fixture.record[0];
    search_view.store = &bank.store;
    search_view.handle = message_store_ordered_handle(bank.store, 0);
    search_view.rich_store = &g_app.account_ui->message_rich_store;
    UiSearchSource source{};
    source.count = source.revision = 1;
    source.rich_timeline = &bank.timeline;
    source.current = +[](void*) noexcept { return search_current; };
    source.resolve = +[](void*, u32 i, UiSearchRow* row) noexcept {
        if (i || !row)
            return false;
        *row = {7, 1, utf8_literal("Audio author"), utf8_literal("#source"), {}, {}, true, true};
        return true;
    };
    UiSearchContent content{};
    content.edit.active = content.panel = true;
    content.phase = UiSearchPhase::ready;
    content.revision = 1;
    UiSearchScene scene{};
    const bool ready = initialized && ui_search_scene_update(&scene, &source, &content, {0, 0, 1400, 1200},
                                                             {0, 0, 1400, 48});
    mutable_search_scene = &scene;
    g_app.renderer.search_scene = &scene;
    UiSearchTarget target{};
    target.action = UiSearchAction::rich_item;
    target.channel_id = 7;
    target.message_id = 1;
    target.source_revision = 1;
    target.source_index = 0;
    target.rich_kind = MessageSceneRichItemKind::attachment;
    target.snapshot = true;
    const auto selections = account.selections;
    const bool opened = ready && application_video_open_search(target) && pump() && pump();
    const auto* rich_scene = ui_search_scene_message_scene(scene);
    check(opened && g_app.renderer.audio_scene && rich_scene &&
              g_app.renderer.audio_scene->owner.manager == &rich_scene->widgets &&
              g_app.renderer.audio_scene->surface == UiAudioSourceSurface::search &&
              g_app.renderer.audio_scene->projection.playback.voice ==
                  (format == MediaAttachmentFormat::ogg_opus) &&
              application_video_search_active() && account.status.key.id == 71 &&
              media_asset_attachment_format(account.status.key) == format &&
              account.status.playback.kind == audio_kind(format) && account.selections == selections + 1,
          "audio Search runtime: actual forwarded rich card owns playback independently of absent live "
          "timeline and selected channel");
    if (opened && g_app.renderer.audio_scene) {
        const auto* download =
            ui_audio_controls_find(g_app.renderer.audio_scene->projection, UiAudioControl::download);
        const auto before = downloads;
        if (download) {
            const auto bounds = download->bounds;
            check(application_video_pointer(WM_LBUTTONDOWN, point(bounds)) &&
                      application_video_pointer(WM_LBUTTONUP, point(bounds)) && downloads == before + 1 &&
                      download_message == 1 && download_attachment == 71 && download_snapshot,
                  "audio Search runtime: player Download preserves publication ticket and forwarded "
                  "attachment identity");
        }
        const auto commands = account.commands;
        ++source.revision;
        target.source_revision = source.revision;
        check(ui_search_scene_update(&scene, &source, &content, {0, 0, 1400, 1200}, {0, 0, 1400, 48}) &&
                  pump() && account.selections == selections + 1 && account.commands == commands &&
                  g_app.renderer.audio_scene,
              "audio Search runtime: presentation revision updates exact row generation without decoder "
              "reselection");
        search_current = false;
        check(pump() && !g_app.video && !g_app.renderer.audio_scene && !application_video_search_active() &&
                  !application_video_open_search(target),
              "audio Search runtime: retired publication closes owner before borrowed rich widgets are "
              "released");
    }
    application_video_close();
    mutable_search_scene = nullptr;
    g_app.renderer.search_scene = nullptr;
    ui_search_scene_release(&scene);
    search_view = {};
    search_current = false;
    g_app.message_view = prior_view;
    g_app.message_view.timeline.store = &g_app.message_view.store;
    g_app.message_view.timeline.list = &g_app.message_view.list;
    g_app.account_ui->message_rich_store = prior_rich;
    g_app.account_ui->active_channel_id = prior_channel;
    g_app.context_focus = nullptr;
}
void run_audio_format(MediaAttachmentFormat format) noexcept {
    const auto prior_view = g_app.message_view;
    const auto prior_rich = g_app.account_ui->message_rich_store;
    for (u32 scope = 0; scope < 2; ++scope) {
        application_video_close();
        message_scene_release(&g_app.message_scene);
        SceneFixture fixture{};
        const bool initialized =
            initialize_scene(fixture, utf8_literal("https://example.com/native-original.opus"), scope != 0,
                             true, scope != 0 && format == MediaAttachmentFormat::ogg_opus, format);
        check(initialized,
              "audio runtime: authoritative audio metadata creates actual attachment-root geometry");
        if (!initialized) {
            application_video_close();
            message_scene_release(&g_app.message_scene);
            continue;
        }
        const auto* row = scope
                              ? message_scene_snapshot_row(g_app.message_scene, g_app.message_scene.rows[0])
                              : &g_app.message_scene.rows[0];
        const auto widget =
            row ? message_scene_rich_item_widget(*row, MessageSceneRichItemKind::attachment, 0)
                : ui_widget_invalid_handle();
        SetFocus(g_app.window);
        g_app.context_focus = &g_app.message_scene.widgets;
        const bool focused = ui_widget_manager_focus(&g_app.message_scene.widgets, widget);
        const auto selections = account.selections;
        check(focused && application_video_open_focused() && pump() && g_app.renderer.audio_scene &&
                  !g_app.renderer.video_scene && account.status.playback.kind == audio_kind(format) &&
                  media_asset_attachment_format(account.status.key) == format &&
                  account.selections == selections + 1,
              "audio runtime: focused native activation selects exact audio codec through the existing "
              "single playback "
              "owner");
        if (!g_app.renderer.audio_scene) {
            application_video_close();
            message_scene_release(&g_app.message_scene);
            continue;
        }
        check(pump(), "audio runtime: command publication settles before unchanged-frame receipt");
        const auto& scene = *g_app.renderer.audio_scene;
        check(
            scene.owner.manager == &g_app.message_scene.widgets && scene.owner.handle.slot == widget.slot &&
                scene.owner.handle.generation == widget.generation &&
                scene.projection.playback.voice ==
                    (scope != 0 && format == MediaAttachmentFormat::ogg_opus) &&
                scene.projection.playback.duration_ms == 120000 &&
                scene.projection.playback.elapsed_ms == 3000 &&
                scene.projection.playback.waveform.count ==
                    (format == MediaAttachmentFormat::ogg_opus ? 4u : 0u) &&
                (format != MediaAttachmentFormat::ogg_opus || scene.projection.playback.waveform[3] == 3) &&
                scene.projection.playback.title.bytes,
            "audio runtime: exact generation-checked rich root, decoded waveform and observed codec time "
            "supersede declared metadata");
        const auto paint = ui_audio_scene_paint_revision(scene), text = ui_audio_scene_text_revision(scene);
        const auto commands = account.commands, network = account.network_requests,
                   published = text_publications, gpu = account.gpu_publications;
        bool stable = true;
        for (u32 frame = 0; frame < 8; ++frame)
            stable = pump() && stable;
        check(stable && g_app.renderer.audio_scene &&
                  ui_audio_scene_paint_revision(*g_app.renderer.audio_scene) == paint &&
                  ui_audio_scene_text_revision(*g_app.renderer.audio_scene) == text &&
                  account.commands == commands && account.selections == selections + 1 &&
                  account.network_requests == network && text_publications == published &&
                  !application_video_next_wake() && account.gpu_publications == gpu,
              "audio runtime: 8 unchanged pumps retain paint/text and emit no request, command, "
              "selection, GPU publication or animation deadline");
        if (format != MediaAttachmentFormat::ogg_opus) {
            account.status.playback.kind = AccountPlaybackKind::ogg_opus;
            check(pump() && g_app.renderer.audio_scene->projection.playback.error &&
                      !g_app.renderer.audio_scene->projection.playback.can_seek,
                  "audio runtime: a different codec status cannot masquerade as selected MP3/WAV playback");
            account.status.playback.kind = audio_kind(format);
            check(pump() && !g_app.renderer.audio_scene->projection.playback.error &&
                      account.selections == selections + 1,
                  "audio runtime: exact codec status restores existing control owner without source "
                  "reselection");
        }
        g_app.keyboard_focus_visible = true;
        SetFocus(g_app.window);
        check(pump() && application_video_key(VK_SPACE, false, false) && pump() &&
                  account.status.playback.paused,
              "audio runtime: native keyboard Play Pause controls the exact selected session");
        check(application_video_key(VK_TAB, false, false) && application_video_key(VK_END, false, false) &&
                  pump() &&
                  account.status.playback.position_100ns == account.status.playback.duration_100ns,
              "audio runtime: retained keyboard seek reaches observed duration");
        const auto* seek =
            ui_audio_controls_find(g_app.renderer.audio_scene->projection, UiAudioControl::seek);
        if (seek) {
            const auto bounds = seek->bounds;
            const auto identity = account.status.playback.identity;
            const LPARAM later = MAKELPARAM(static_cast<short>(bounds.left + ui_rect_width(bounds) * .75f),
                                            static_cast<short>((bounds.top + bounds.bottom) * .5f));
            const bool down = application_video_pointer(WM_LBUTTONDOWN, point(bounds));
            account.status.playback.seeking = account.status.playback.buffering = true;
            const auto commands_before_drag = account.commands;
            check(down && GetCapture() == g_app.window && pump() && g_app.renderer.audio_scene &&
                      g_app.renderer.audio_scene->capture == UiAudioControl::seek &&
                      !g_app.renderer.audio_scene->projection.playback.loading &&
                      application_video_pointer(WM_MOUSEMOVE, later) &&
                      application_video_pointer(WM_LBUTTONUP, later) && pump() &&
                      GetCapture() != g_app.window && account.commands == commands_before_drag + 2 &&
                      account_video_identity_equal(identity, account.status.playback.identity) &&
                      account.status.playback.position_100ns > account.status.playback.duration_100ns / 2 &&
                      account.status.playback.position_100ns < account.status.playback.duration_100ns,
                  "audio runtime: real seek drag retains capture through observed seeking and buffering, "
                  "then move/up command the same epoch");
            account.status.playback.seeking = account.status.playback.buffering = false;
            check(pump(), "audio runtime: native seek completion keeps the same observed controls");
        }
        const auto* volume =
            ui_audio_controls_find(g_app.renderer.audio_scene->projection, UiAudioControl::volume);
        if (volume) {
            const auto bounds = volume->bounds;
            check(application_video_pointer(WM_LBUTTONDOWN, point(bounds)) &&
                      GetCapture() == g_app.window &&
                      application_video_pointer(WM_LBUTTONUP, point(bounds)) &&
                      GetCapture() != g_app.window && pump() && account.status.playback.applied_volume > 0,
                  "audio runtime: actual native volume range capture releases and submits bounded volume");
            const bool captured = application_video_pointer(WM_LBUTTONDOWN, point(bounds));
            account.status.playback.volume_valid = false;
            check(captured && pump() && GetCapture() != g_app.window &&
                      g_app.renderer.audio_scene->capture == UiAudioControl::none,
                  "audio runtime: observed volume capability withdrawal cancels its actual native range "
                  "capture");
            account.status.playback.volume_valid = true;
            check(pump(), "audio runtime: observed volume capability restores without source replacement");
        }
        const auto* download =
            ui_audio_controls_find(g_app.renderer.audio_scene->projection, UiAudioControl::download);
        const auto before_download = downloads;
        if (download) {
            const auto bounds = download->bounds;
            check(application_video_pointer(WM_LBUTTONDOWN, point(bounds)) &&
                      application_video_pointer(WM_LBUTTONUP, point(bounds)) &&
                      downloads == before_download + 1 && download_message == 1 &&
                      download_attachment == (scope ? 71u : 70u) && download_snapshot == (scope != 0),
                  "audio runtime: Download keeps exact outer message, attachment and forwarded scope");
        }
        seek = ui_audio_controls_find(g_app.renderer.audio_scene->projection, UiAudioControl::seek);
        if (seek)
            check(application_video_pointer(WM_LBUTTONDOWN, point(seek->bounds)) &&
                      GetCapture() == g_app.window,
                  "audio runtime: real seek capture exists before terminal error");
        account.status.playback.error = AccountVideoError::decoder;
        account.status.playback.seeking = true;
        check(pump() && g_app.renderer.audio_scene->projection.playback.error &&
                  GetCapture() != g_app.window &&
                  g_app.renderer.audio_scene->capture == UiAudioControl::none,
              "audio runtime: observed decoder failure publishes error instead of fabricated playback");
        const auto* retry =
            ui_audio_controls_find(g_app.renderer.audio_scene->projection, UiAudioControl::play_pause);
        const auto retry_selections = account.selections;
        if (retry) {
            const auto bounds = retry->bounds;
            const auto retired = account.status;
            const auto before_stop = account.commands;
            deferred_stop = true;
            const bool requested = application_video_pointer(WM_LBUTTONDOWN, point(bounds)) &&
                                   application_video_pointer(WM_LBUTTONUP, point(bounds));
            GpuVideoFrame late{};
            account.queued_frame = {retired.key, retired.playback.identity, &late};
            account.frame_pending = true;
            const auto acknowledgements = account.frame_acknowledgements;
            check(requested && stop_pending && pump() && account.selections == retry_selections &&
                      account.commands == before_stop + 1 &&
                      account.frame_acknowledgements == acknowledgements + 1 && !account.frame_pending,
                  "audio runtime: replacement drains exact late prior lease without duplicate stop, early "
                  "selection or GPU publication");
            deferred_stop = false;
            check(pump() && account.selections == retry_selections + 1 && g_app.renderer.audio_scene &&
                      !g_app.renderer.audio_scene->projection.playback.error,
                  "audio runtime: Retry captures a fresh playback epoch through the prior-stop barrier");
        }
        UiThreadBrowserScene modal{};
        modal.active = true;
        g_app.renderer.thread_browser_scene = &modal;
        const auto before_modal = account.commands;
        check(pump() && !g_app.renderer.audio_scene && account.status.playback.paused &&
                  account.commands == before_modal + 1 && !application_video_next_wake(),
              "audio runtime: front panel pauses audio, detaches controls and removes presentation "
              "deadlines");
        stable = true;
        for (u32 frame = 0; frame < 8; ++frame)
            stable = pump() && stable;
        check(stable && account.commands == before_modal + 1,
              "audio runtime: covered pumps never repeat the accepted pause");
        g_app.renderer.thread_browser_scene = nullptr;
        check(pump() && g_app.renderer.audio_scene && account.status.playback.paused,
              "audio runtime: modal dismissal restores the card without unsolicited resume");
        check(message_store_delete(&g_app.message_view.store, 1) && pump() && !g_app.video &&
                  !g_app.renderer.audio_scene,
              "audio runtime: authoritative deletion retires source, controls and transport owner");
        message_scene_release(&g_app.message_scene);
    }
    application_video_close();
    message_scene_release(&g_app.message_scene);
    {
        SceneFixture fixture{};
        selection_busy = true;
        g_app.media_urls = {};
        const bool ready = initialize_scene(
            fixture, utf8_literal("https://example.com/request-contract.opus"), false, true, false, format);
        const auto widget = ready ? message_scene_rich_item_widget(g_app.message_scene.rows[0],
                                                                   MessageSceneRichItemKind::attachment, 0)
                                  : ui_widget_invalid_handle();
        const auto requests = account.network_requests;
        check(ready && ui_widget_manager_focus(&g_app.message_scene.widgets, widget) &&
                  application_video_open_focused() && pump() && account.network_requests == requests + 1 &&
                  account.last_request.image_type == audio_type(format) &&
                  !account.last_request.video_device && !account.last_request.proxy_width &&
                  !account.last_request.proxy_height &&
                  media_asset_attachment_format(account.last_request.key) == format,
              "audio runtime: encoded-source request has exact codec type/key, zero image dimensions "
              "and no GPU device");
        selection_busy = false;
        application_video_close();
        message_scene_release(&g_app.message_scene);
    }
    g_app.context_focus = nullptr;
    g_app.message_view = prior_view;
    g_app.message_view.timeline.store = &g_app.message_view.store;
    g_app.message_view.timeline.list = &g_app.message_view.list;
    g_app.account_ui->message_rich_store = prior_rich;
}
void run_audio() noexcept {
    const MediaAttachmentFormat formats[]{MediaAttachmentFormat::ogg_opus, MediaAttachmentFormat::mp3,
                                          MediaAttachmentFormat::wav};
    for (auto format : formats)
        run_audio_format(format);
}
void run() noexcept {
    // Hidden HWND and fake callbacks exercise shipping AppVideo input/pump.
    // No account DLL, socket, media request, decoder or GPU resource is opened.
    g_app.window = CreateWindowExW(0, L"STATIC", L"MaiCord video focus fixture", WS_POPUP, -16000, -16000,
                                   640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    g_app.account_ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(g_app.window && g_app.account_ui, "video focus runtime: bounded hidden native host initializes");
    if (!g_app.window || !g_app.account_ui) {
        release();
        return;
    }
    g_app.renderer_ready = true;
    g_app.renderer.dpi = 96;
    g_app.preferences.reduced_motion = true; // Focus invariants need no wall-clock wait.
    g_app.account_ui->active_channel_id = 7;
    GuildRecord source_guilds[1]{};
    ChannelRecord source_channels[1]{};
    u16 source_guild_order[1]{}, source_channel_order[1]{};
    u8 source_names[128]{};
    check(chat_state_store_initialize(&g_app.account_ui->chat_state, {source_guilds, 1},
                                         {source_guild_order, 1}, {source_channels, 1},
                                         {source_channel_order, 1}, {source_names, 128}) &&
              chat_state_store_upsert_channel(&g_app.account_ui->chat_state,
                                                 {7, 0, 0, utf8_literal("native media source"), 0, 0, 1})
                  .ok(),
          "video focus runtime: actual containing channel owns current VIEW and READ authority");
    g_app.account.status.profile_id = 4;
    g_app.plugin_account_scope_serial = 9;
    g_app.account.instance = &account;
    g_app.account.api.video_select = select;
    g_app.account.api.video_control = control;
    g_app.account.api.video_status = status;
    g_app.account.api.video_take = take;
    g_app.account.api.video_acknowledge = acknowledge;
    g_app.account.api.media_attachment_request = request;
    SceneFixture fixture{};
    const bool initialized = initialize_scene(fixture);
    check(initialized, "video focus runtime: actual borrowed message/rich attachment geometry initializes");
    if (!initialized) {
        release();
        return;
    }
    const auto* node = message_scene_rich_item_layout_node(g_app.message_scene, g_app.message_scene.rows[0],
                                                           MessageSceneRichItemKind::attachment, 0);
    if (!node) {
        check(false, "video focus runtime: real attachment layout is present");
        release();
        return;
    }
    const LPARAM preview_point = MAKELPARAM(static_cast<short>(node->rectangle.left + 160),
                                            static_cast<short>(node->rectangle.top + 40));
    fixture.rich_record[0].attachments[0].flags |= kGatewayMediaExplicitFlag;
    check(message_scene_set_media_policy(&g_app.message_scene, {nullptr, query_media, 1}),
          "video policy fixture binds the actual native scene query");
    media_filter = MessageExplicitFilter::block;
    UiCursor guarded_cursor{};
    check(application_video_pointer(WM_LBUTTONDOWN, preview_point) && !g_app.video && !account.selections &&
              !account.network_requests &&
              !application_video_cursor({static_cast<float>(static_cast<i16>(LOWORD(preview_point))),
                                         static_cast<float>(static_cast<i16>(HIWORD(preview_point)))},
                                        &guarded_cursor),
          "blocked inline video consumes activation without selection, network request or hand cursor");
    media_filter = MessageExplicitFilter::blur;
    media_reveal_allowed = false;
    check(application_video_pointer(WM_LBUTTONDOWN, preview_point) && !g_app.video && confirmations == 1 &&
              !g_app.message_scene.media_reveals.count,
          "age gated video activation requests confirmation without selecting or revealing");
    media_reveal_allowed = true;
    check(application_video_pointer(WM_LBUTTONDOWN, preview_point) && !g_app.video &&
              media_refreshes == 1 &&
              message_media_revealed(g_app.message_scene.media_reveals,
                                     message_media_reveal_key(1, MessageMediaSource::attachment, 0)),
          "inline video first activation reveals only and leaves decoder selection for the next gesture");
    check(application_video_pointer(WM_LBUTTONDOWN, preview_point) && pump() && account.selections == 1 &&
              GetFocus() == g_app.window && g_app.renderer.video_scene &&
              !g_app.renderer.video_scene->keyboard_focus &&
              g_app.renderer.video_scene->controls_transition.value == 0 && !application_video_next_wake(),
          "video focus runtime: real mouse selection off-hover does not pin controls or an idle wake");
    fixture.rich_record[0].attachments[0].flags &= ~kGatewayMediaExplicitFlag;
    media_filter = MessageExplicitFilter::show;
    if (!g_app.renderer.video_scene) {
        release();
        return;
    }
    application_video_pointer(WM_MOUSEMOVE, preview_point);
    check(g_app.renderer.video_scene->preview_hovered && !application_video_pointer(WM_MOUSELEAVE, 0) &&
              !g_app.renderer.video_scene->preview_hovered,
          "video focus runtime: production mouse leave observes and clears preview hover without consuming "
          "input");
    {
        // Expansion moves the one session into the viewer and back again.
        const UiRect inline_preview = g_app.renderer.video_scene->preview;
        const u32 selections = account.selections;
        const bool was_playing = account.status.playback.playing;
        const i64 position = account.status.playback.position_100ns;
        check(application_video_key('F', false, false) && viewer_opens == 1 && application_video_expanded() &&
                  pump() && g_app.renderer.video_scene && g_app.renderer.video_scene_viewer &&
                  ui_rect_equal(g_app.renderer.video_scene->preview, viewer_preview) &&
                  account.selections == selections && account.status.playback.playing == was_playing &&
                  account.status.playback.position_100ns == position,
              "video expand runtime: F moves the same session into the viewer without reselection, pause or seek");
        check(g_app.renderer.video_scene->style.idle_ms == kUiVideoControlsExpandedIdleMs &&
                  g_app.renderer.video_scene->projection.controls[6].selected,
              "video expand runtime: the hosted player uses the expanded idle delay and collapse control");
        check(application_video_key(VK_ESCAPE, false, false) && !application_video_expanded() &&
                  !g_app.image_viewer.scene.active && pump() && g_app.renderer.video_scene &&
                  !g_app.renderer.video_scene_viewer &&
                  ui_rect_equal(g_app.renderer.video_scene->preview, inline_preview) &&
                  account.selections == selections && account.status.playback.playing == was_playing,
              "video expand runtime: Esc returns the session to its inline preview with the same play state");
        viewer_allowed = false;
        check(application_video_key('F', false, false) && !application_video_expanded() && pump() &&
                  !g_app.renderer.video_scene_viewer && !g_app.image_viewer.scene.active,
              "video expand runtime: a refused viewer leaves the inline player in place");
        viewer_allowed = true;
    }
    g_app.keyboard_focus_visible = true;
    check(application_video_key(VK_TAB, false, false) && pump() && g_app.renderer.video_scene &&
              g_app.renderer.video_scene->keyboard_focus &&
              g_app.renderer.video_scene->controls_transition.value == 1 && !application_video_next_wake(),
          "video focus runtime: actual native keyboard focus reveals controls without a settled wake");
    const u32 before_competing = account.commands;
    g_app.message_composer.focused = true;
    check(!application_video_key(VK_SPACE, false, false) && pump() && dormant() &&
              account.commands == before_competing,
          "video focus runtime: competing composer blocks commands and retires old video focus");
    g_app.context_focus = &g_app.message_scene.widgets;
    g_app.keyboard_focus_visible = false;
    check(
        application_video_pointer(WM_LBUTTONDOWN, preview_point) && pump() && !g_app.context_focus &&
            !g_app.message_composer.focused,
        "video focus runtime: existing preview click reacquires native focus and clears competing owners");
    g_app.context_focus = &g_app.message_scene.widgets;
    const u32 before_context = account.commands;
    check(!application_video_key(VK_SPACE, false, false) && pump() && dormant() &&
              account.commands == before_context,
          "video focus runtime: competing widget context blocks a stale keyboard command");
    application_video_pointer(WM_LBUTTONDOWN, preview_point);
    pump();
    g_app.keyboard_focus_visible = true;
    application_video_key(VK_TAB, false, false);
    pump();
    SetFocus(nullptr);
    application_video_blur(); // Exact API called by the real premodal WM_KILLFOCUS hook.
    SetFocus(g_app.window);
    check(
        !application_video_key(VK_SPACE, false, false) && pump() && dormant(),
        "video focus runtime: blur survives native focus restoration without reviving keyboard ownership");
    g_app.keyboard_focus_visible = false;
    application_video_pointer(WM_LBUTTONDOWN, preview_point);
    pump();
    if (!g_app.renderer.video_scene) {
        check(false, "video focus runtime: existing preview remains admitted before native capture");
        release();
        return;
    }
    const auto seek_point = point(g_app.renderer.video_scene->projection.controls[1].bounds);
    check(application_video_pointer(WM_LBUTTONDOWN, seek_point) && GetCapture() == g_app.window,
          "video focus runtime: actual range press owns native capture");
    application_video_pointer(WM_MOUSELEAVE, 0);
    check(pump() && g_app.renderer.video_scene &&
              g_app.renderer.video_scene->capture == UiVideoControl::seek &&
              g_app.renderer.video_scene->controls_transition.value == 1 && GetCapture() == g_app.window,
          "video focus runtime: mouse leave preserves captured seek and its visible controls");
    settings = true;
    check(pump() && !g_app.renderer.video_scene && GetCapture() != g_app.window &&
              !application_video_next_wake(),
          "video focus runtime: opaque Settings retires capture, focus and the video deadline");
    settings = false;
    g_app.keyboard_focus_visible = true;
    check(pump() && dormant() && !application_video_key(VK_SPACE, false, false),
          "video focus runtime: modal dismissal cannot restore a stale input-focus flag");
    const auto original_clip = g_app.renderer.video_scene ? g_app.renderer.video_scene->clip : UiRect{};
    UiSearchContent search_content{};
    search_content.panel = true;
    UiSearchScene search_scene{};
    search_scene.active = true;
    search_scene.content = &search_content;
    search_scene.panel = {(original_clip.left + original_clip.right) * .5f, original_clip.top,
                          original_clip.right + 40, original_clip.bottom};
    mutable_search_scene = &search_scene;
    g_app.renderer.search_scene = &search_scene;
    check(pump() && g_app.renderer.video_scene &&
              g_app.renderer.video_scene->clip.right == search_scene.panel.left &&
              ui_rect_has_area(g_app.renderer.video_scene->clip),
          "video focus runtime: search clips video controls to their actual visible area");
    const LPARAM covered_point = MAKELPARAM(static_cast<short>(original_clip.right - 2),
                                            static_cast<short>(original_clip.bottom - 2));
    check(!application_video_pointer(WM_LBUTTONDOWN, covered_point) && account.selections == 1,
          "video focus runtime: occluded preview cannot acquire input or reopen its source");
    search_scene.panel.left = original_clip.left;
    check(pump() && !g_app.renderer.video_scene && !application_video_next_wake(),
          "video focus runtime: a fully occluded preview retires its controls and visual deadline");
    mutable_search_scene = nullptr;
    g_app.renderer.search_scene = nullptr;
    check(pump() && dormant(),
          "video focus runtime: search dismissal restores geometry without stale keyboard focus");
    UiPinnedMessagesScene pins{};
    UiPinnedMessagesSource pins_source{};
    UiPinnedMessagesContent pins_content{};
    pins_source.count = 1;
    pins_source.resolve = +[](void*, u32 index, UiPinnedMessagesRow* row) noexcept {
        if (index || !row)
            return false;
        *row = {};
        row->channel_id = 7;
        row->message_id = 99;
        row->author = utf8_literal("Pinned author");
        row->body = utf8_literal("Pinned fixture message");
        return true;
    };
    pins_content.channel_id = 7;
    pins_content.phase = UiPinnedMessagesPhase::ready;
    pins_content.revision = pins_source.revision = 1;
    // Position an actual 800-DIP dock viewport so its shipping right-edge
    // panel covers half the unchanged source preview.
    const float dock_right =
        (original_clip.left + original_clip.right) * .5f + kUiPinnedMessagesPreferredWidth;
    check(ui_pinned_messages_scene_update(&pins, &pins_source, &pins_content,
                                          {dock_right - 800, 0, dock_right, 480}) &&
              !pins.overlay,
          "video pins runtime: real authoritative docked panel geometry initializes");
    application_video_pointer(WM_LBUTTONDOWN, preview_point);
    pump();
    const auto full_seek = g_app.renderer.video_scene->projection.controls[1].bounds;
    application_video_pointer(WM_LBUTTONDOWN, point(full_seek));
    check(GetCapture() == g_app.window, "video pins runtime: source range capture precedes dock occlusion");
    g_app.renderer.pinned_messages_scene = &pins;
    const u32 before_pins = account.commands;
    check(pump() && g_app.renderer.video_scene &&
              g_app.renderer.video_scene->clip.right == pins.panel.left &&
              g_app.renderer.video_scene->capture == UiVideoControl::none && GetCapture() != g_app.window &&
              account.status.playback.playing && account.commands == before_pins,
          "video pins runtime: partial dock clips the playing source and retires captured geometry without "
          "pausing its visible portion");
    check(!application_video_pointer(WM_LBUTTONDOWN, covered_point) && account.selections == 1,
          "video pins runtime: pinned-covered preview cannot reopen source or acquire controls");
    ++pins_source.revision;
    check(pump() && g_app.renderer.video_scene &&
              ui_rect_equal(g_app.renderer.video_scene->clip, original_clip),
          "video pins runtime: replaced borrowed panel authority cannot occlude the current source");
    --pins_source.revision;
    check(ui_pinned_messages_scene_update(&pins, &pins_source, &pins_content, {0, 0, 640, 480}) &&
              pins.overlay,
          "video pins runtime: actual compact viewport publishes modal overlay ownership");
    const u32 before_overlay = account.commands;
    check(
        pump() && !g_app.renderer.video_scene && GetCapture() != g_app.window &&
            account.status.playback.paused && !account.status.playback.playing &&
            account.commands == before_overlay + 1 && !application_video_next_wake(),
        "video pins runtime: modal pins pause fully hidden audio and retire controls capture and deadline");
    check(pump() && account.commands == before_overlay + 1 &&
              !application_video_pointer(WM_LBUTTONDOWN, preview_point),
          "video pins runtime: stable full occlusion emits one pause and rejects hidden source input");
    g_app.renderer.pinned_messages_scene = nullptr;
    check(pump() && dormant() && account.status.playback.paused,
          "video pins runtime: dismissal restores source geometry without resuming audio or stale focus");
    ui_pinned_messages_scene_release(&pins);
    UiQuickSwitcherScene quick{};
    bool quick_current = true;
    UiQuickSwitcherSource quick_source{};
    quick_source.context = &quick_current;
    quick_source.count = 1;
    quick_source.revision = 1;
    quick_source.current = +[](void* context) noexcept { return *static_cast<bool*>(context); };
    quick_source.resolve = +[](void*, u32 index, UiQuickSwitcherRow* row) noexcept {
        if (index || !row)
            return false;
        *row = {};
        row->key = 1;
        row->channel_id = 7;
        row->label = utf8_literal("Original quick fixture");
        return true;
    };
    UiQuickSwitcherContent quick_content{};
    quick_content.phase = UiQuickSwitcherPhase::ready;
    quick_content.revision = 1;
    quick_content.edit = {utf8_literal("fixture"), 7, 7, true, true, true};
    const bool quick_ready =
        ui_quick_switcher_scene_update(&quick, &quick_source, &quick_content, {0, 0, 640, 480});
    check(quick_ready, "video quick runtime: real current modal source initializes");
    if (quick_ready) {
        application_video_pointer(WM_LBUTTONDOWN, preview_point);
        pump();
        const auto quick_seek = g_app.renderer.video_scene->projection.controls[1].bounds;
        application_video_pointer(WM_LBUTTONDOWN, point(quick_seek));
        check(GetCapture() == g_app.window && account.status.playback.playing,
              "video quick runtime: captured playing source precedes modal opening");
        g_app.renderer.quick_switcher_scene = &quick;
        const u32 before_quick = account.commands, selected_before_quick = account.selections;
        check(pump() && !g_app.renderer.video_scene && GetCapture() != g_app.window &&
                  account.status.playback.paused && account.commands == before_quick + 1 &&
                  !application_video_next_wake() &&
                  !application_video_pointer(WM_LBUTTONDOWN, preview_point) &&
                  !application_video_key(VK_SPACE, false, false),
              "video quick runtime: authoritative modal pauses audio retires capture and blocks controls");
        bool idle = true;
        for (u32 frame = 0; frame < 8; ++frame)
            idle = pump() && idle;
        check(idle && account.commands == before_quick + 1 && account.selections == selected_before_quick &&
                  !application_video_next_wake(),
              "video quick runtime: 8 unchanged modal frames issue no command reselection or deadline");
        quick_current = false;
        check(pump() && !g_app.renderer.video_scene && account.status.playback.paused &&
                  account.commands == before_quick + 1 && !application_video_next_wake() &&
                  !application_video_pointer(WM_LBUTTONDOWN, preview_point),
              "video quick runtime: stale query authority retains active modal audio and input occlusion");
        quick.active = false;
        check(pump() && dormant() && account.status.playback.paused,
              "video quick runtime: modal close restores geometry without playback or old focus");
        g_app.renderer.quick_switcher_scene = nullptr;
    }
    ui_quick_switcher_scene_release(&quick);
    {
        UiThreadBrowserScene browser{};
        // The app must respect active panel ownership even before its next
        // current query projection; no browser renderer is mocked here.
        browser.active = true;
        check(application_video_pointer(WM_LBUTTONDOWN, preview_point) && pump() &&
                  g_app.renderer.video_scene && account.status.playback.playing,
              "video browser runtime: selected source resumes explicitly before panel coverage");
        if (g_app.renderer.video_scene) {
            application_video_pointer(WM_LBUTTONDOWN,
                                      point(g_app.renderer.video_scene->projection.controls[1].bounds));
            check(GetCapture() == g_app.window,
                  "video browser runtime: source owns seek capture before panel opens");
        }
        const u32 commands = account.commands, selections = account.selections;
        const u32 uploads = account.gpu_publications, acknowledged = account.frame_acknowledgements;
        GpuVideoFrame queued_gpu{};
        account.queued_frame = {account.status.key, account.status.playback.identity, &queued_gpu};
        account.frame_pending = true;
        g_app.preferences.reduced_motion = false;
        g_app.renderer.thread_browser_scene = &browser;
        check(!application_video_pointer(WM_LBUTTONDOWN, preview_point) &&
                  !application_video_key(VK_SPACE, false, false) && !application_video_next_wake() &&
                  pump() && !g_app.renderer.video_scene && GetCapture() != g_app.window &&
                  account.status.playback.paused && !account.status.playback.playing &&
                  account.commands == commands + 1 && account.gpu_publications == uploads &&
                  account.frame_acknowledgements == acknowledged + 1 && !account.frame_pending,
              "video browser runtime: active panel pauses audio retires controls and acknowledges queued "
              "frame without GPU upload");
        const u32 text = text_publications, requests = account.network_requests;
        bool idle = true;
        for (u32 frame = 0; frame < 8; ++frame)
            idle = pump() && idle;
        ++browser.source_publication;
        check(idle && pump() && account.commands == commands + 1 && account.selections == selections &&
                  account.network_requests == requests && account.gpu_publications == uploads &&
                  text_publications == text && !application_video_next_wake() &&
                  !application_video_pointer(WM_LBUTTONDOWN, preview_point),
              "video browser runtime: 8 unchanged and retired-publication frames retain occlusion without "
              "commands selection uploads text or wakes");
        browser.active = false;
        g_app.preferences.reduced_motion = true;
        check(pump() && dormant() && account.status.playback.paused && account.commands == commands + 1 &&
                  !application_video_key(VK_SPACE, false, false) &&
                  application_video_pointer(WM_LBUTTONDOWN, preview_point) && pump() &&
                  account.status.playback.playing && account.selections == selections,
              "video browser runtime: dismissal restores geometry with paused playback and explicit "
              "same-session resume");
        g_app.renderer.thread_browser_scene = nullptr;

        application_video_close();
        check(application_video_pointer(WM_LBUTTONDOWN, preview_point) && g_app.video,
              "video browser runtime: explicit preview request exists before first decoder selection");
        const u32 pending_selections = account.selections, pending_commands = account.commands;
        const u32 pending_requests = account.network_requests;
        browser.active = true;
        g_app.renderer.thread_browser_scene = &browser;
        idle = true;
        for (u32 frame = 0; frame < 8; ++frame)
            idle = pump() && idle;
        check(idle && g_app.video && !g_app.renderer.video_scene &&
                  account.selections == pending_selections && account.commands == pending_commands &&
                  account.network_requests == pending_requests && !application_video_next_wake(),
              "video browser runtime: covered pending request admits no decoder selection network request "
              "or playback command");
        browser.active = false;
        g_app.renderer.thread_browser_scene = nullptr;
        check(pump() && account.selections == pending_selections + 1 && account.status.playback.playing,
              "video browser runtime: uncovered pending user request selects its original current source "
              "once");

        const auto retiring_decoder = account.status;
        deferred_stop = true;
        GatewayMessageAttachment replacement{};
        replacement.id = 72;
        replacement.filename = utf8_literal("replacement-fixture.mp4");
        replacement.content_type = utf8_literal("video/mp4");
        replacement.url = utf8_literal("https://example.com/replacement-fixture.mp4");
        replacement.width = 320;
        replacement.height = 240;
        replacement.has_width = replacement.has_height = true;
        GatewayMessageEvent replacement_event{};
        replacement_event.id = 1;
        replacement_event.fields = gateway_message_field_attachments;
        replacement_event.attachments = {&replacement, 1};
        check(
            message_rich_store_apply(&g_app.account_ui->message_rich_store, 1, fixture.record[0].revision,
                                     replacement_event)
                    .ok() &&
                message_scene_update(&g_app.message_scene, {0, 0, 640, 480}).ok() &&
                application_video_pointer(WM_LBUTTONDOWN, preview_point) && stop_pending && g_app.video,
            "video browser runtime: replacement request owns an accepted asynchronous prior decoder stop");
        const u32 replacement_selections = account.selections, replacement_commands = account.commands;
        const u32 replacement_acks = account.frame_acknowledgements;
        account.queued_frame = {retiring_decoder.key, retiring_decoder.playback.identity, &queued_gpu};
        account.frame_pending = true; // Arrives after the old close's single drain.
        browser.active = true;
        g_app.renderer.thread_browser_scene = &browser;
        idle = true;
        for (u32 frame = 0; frame < 8; ++frame)
            idle = pump() && idle;
        check(idle && account.frame_acknowledgements == replacement_acks + 1 && !account.frame_pending &&
                  !stop_pending && !account.status.playback.playing &&
                  account.selections == replacement_selections &&
                  account.commands == replacement_commands && account.gpu_publications == uploads &&
                  account.network_requests == pending_requests && !g_app.renderer.video_scene &&
                  !application_video_next_wake(),
              "video browser runtime: late prior frame exact acknowledgement completes accepted stop with "
              "no replacement select upload or repeated stop across 8 covered pumps");
        // A source can retire before the next pump drains another late slot.
        account.queued_frame = {retiring_decoder.key, retiring_decoder.playback.identity, &queued_gpu};
        account.frame_pending = true;
        g_app.account_ui->active_channel_id = 8;
        check(pump() && !g_app.video && !account.frame_pending &&
                  account.frame_acknowledgements == replacement_acks + 2 &&
                  account.commands == replacement_commands && account.selections == replacement_selections,
              "video browser runtime: covered selecting owner retirement drains same-account frame without "
              "stopping an unowned session");
        g_app.account_ui->active_channel_id = 7;
        browser.active = false;
        g_app.renderer.thread_browser_scene = nullptr;
        deferred_stop = false;
        check(pump() && !g_app.video && !account.status.playback.playing &&
                  account.selections == replacement_selections && !application_video_next_wake(),
              "video browser runtime: browser dismissal never resumes a retired pending source");
        check(application_video_pointer(WM_LBUTTONDOWN, preview_point) && g_app.video,
              "video browser runtime: pending owner exists before account authority switches");
        account.queued_frame = {retiring_decoder.key, retiring_decoder.playback.identity, &queued_gpu};
        account.frame_pending = true;
        const u32 account_switch_acks = account.frame_acknowledgements;
        ++g_app.plugin_account_scope_serial;
        check(pump() && !g_app.video && account.frame_pending &&
                  account.frame_acknowledgements == account_switch_acks &&
                  account.commands == replacement_commands && account.selections == replacement_selections,
              "video browser runtime: account-switch retirement never takes or acknowledges another "
              "account frame");
        --g_app.plugin_account_scope_serial;
        account.frame_pending = false; // The fixture retires the old account's own slot.
    }
    check(account.network_requests == 0 && account.gpu_publications == 0 && text_publications != 0,
          "video focus runtime: production input fixture publishes metadata without network or GPU work");
    application_video_close();
    message_scene_release(&g_app.message_scene);
    const bool forward_scene =
        initialize_scene(fixture, utf8_literal("https://example.com/forward.mp4"), true);
    check(forward_scene,
          "video snapshot runtime: outer and independently owned child attachment geometry initializes");
    if (forward_scene) {
        const auto* child = message_scene_snapshot_row(g_app.message_scene, g_app.message_scene.rows[0]);
        const auto* child_node =
            child ? message_scene_rich_item_layout_node(g_app.message_scene, *child,
                                                        MessageSceneRichItemKind::attachment, 0)
                  : nullptr;
        const auto outer_key = media_asset_embed(1, fixture.record[0].revision, 0,
                                                 MediaEmbedResource::video, MediaAttachmentFormat::mp4);
        const auto inner_key =
            media_asset_embed(1, fixture.record[0].revision, 0, MediaEmbedResource::video,
                              MediaAttachmentFormat::mp4, false, MediaResolutionTier::original, true);
        check(child_node && media_asset_key_valid(inner_key) &&
                  !media_asset_key_equal(outer_key, inner_key) && inner_key.id == outer_key.id &&
                  inner_key.variant == outer_key.variant,
              "video snapshot runtime: same-index embed namespaces preserve outer message revision without "
              "collision");
        if (child_node) {
            const auto click = MAKELPARAM(static_cast<short>(child_node->rectangle.left + 160),
                                          static_cast<short>(child_node->rectangle.top + 40));
            const u32 before_select = account.selections;
            check(application_video_pointer(WM_LBUTTONDOWN, click) && pump() &&
                      account.selections == before_select + 1 && account.status.key.id == 71 &&
                      account.status.playback.identity.message_id == 1 && g_app.renderer.video_scene &&
                      account.status.playback.playing,
                  "video snapshot runtime: shipping pointer and pump play the child source under outer "
                  "action ownership");
            const auto hit = message_scene_hit_test(g_app.message_scene, child_node->rectangle.left + 160,
                                                    child_node->rectangle.top + 40);
            check(
                hit.message_id == 1 &&
                    message_scene_resolve(g_app.message_scene, hit.widget) == &g_app.message_scene.rows[0],
                "video snapshot runtime: child native hit keeps outer reply reaction and delete authority");
            GatewayMessageEvent clear{};
            clear.id = 1;
            clear.fields = gateway_message_field_message_snapshots;
            check(message_rich_store_apply(&g_app.account_ui->message_rich_store, 1,
                                           fixture.record[0].revision, clear)
                          .ok() &&
                      message_scene_update(&g_app.message_scene, {0, 0, 640, 480}).ok() &&
                      !message_scene_snapshot_row(g_app.message_scene, g_app.message_scene.rows[0]) &&
                      pump() && !g_app.renderer.video_scene && account.status.playback.paused,
                  "video snapshot runtime: explicit child removal retires geometry and pauses its active "
                  "audio");
        }
    }
    run_search_video();
    run_audio();
    run_search_audio();
    run_search_audio(MediaAttachmentFormat::mp3);
    application_video_close();
    message_scene_release(&g_app.message_scene);
    constexpr auto expired_source =
        utf8_literal("https://cdn.discordapp.com/attachments/7/70/focus-fixture.mp4");
    selection_busy = true;
    g_app.media_urls = {};
    const bool refresh_scene = initialize_scene(fixture, expired_source);
    check(refresh_scene, "video refresh runtime: actual unsigned source fixture initializes");
    if (refresh_scene) {
        check(
            application_video_pointer(WM_LBUTTONDOWN, preview_point) && pump() &&
                g_app.renderer.video_scene && g_app.renderer.video_scene->projection.playback.buffering &&
                !g_app.renderer.video_scene->projection.playback.error &&
                media_url_resolution_state(g_app.media_urls, expired_source) == MediaUrlState::queued,
            "video refresh runtime: unresolved refresh retains real loading without sending expired bytes");
        auto& refresh = g_app.media_urls.entries[0];
        refresh.state = MediaUrlState::failed;
        const u32 requests_before_failure = account.network_requests;
        check(
            pump() && g_app.renderer.video_scene && g_app.renderer.video_scene->projection.playback.error &&
                !g_app.renderer.video_scene->projection.playback.buffering,
            "video refresh runtime: terminal refresh publishes unavailable and retires indefinite loading");
        const u32 selections_after_failure = account.selections;
        bool stable = true;
        for (u32 frame = 0; frame < 8; ++frame)
            stable = pump() && stable;
        check(stable && account.selections == selections_after_failure &&
                  account.network_requests == requests_before_failure && !application_video_next_wake(),
              "video refresh runtime: terminal refresh stays idle across 8 frames without reselection or "
              "expired requests");
    }
    fixture.rich_record[0].attachments[0].flags |= kGatewayMediaExplicitFlag;
    media_filter = MessageExplicitFilter::block;
    const u32 selections_before_policy = account.selections;
    check(message_scene_set_media_policy(&g_app.message_scene, {nullptr, query_media, 2}) && pump() &&
              !g_app.video && !g_app.renderer.video_scene && account.selections == selections_before_policy,
          "newly blocked video policy retires an existing decoder owner without reselection or reveal");
    media_filter = MessageExplicitFilter::show;
    release();
    check(!g_app.video && !g_app.renderer.video_scene && !application_video_next_wake(),
          "video focus runtime: release removes the selected owner and every deadline");
}
} // namespace maicord::video_focus_runtime_test

namespace maicord {
bool renderer_bind_audio_scene(D3D11Renderer* renderer, const UiAudioScene* scene) noexcept {
    if (!renderer)
        return false;
    if (renderer->audio_scene != scene) {
        renderer->audio_scene = scene;
        renderer->retained_presentation.valid = false;
    }
    if (scene) {
        renderer->video_scene = nullptr;
        renderer->video_scene_search_result = false;
    }
    return true;
}
bool renderer_rebuild_content_text(D3D11Renderer* renderer, const Theme&) noexcept {
    ++video_focus_runtime_test::text_publications;
    renderer->interaction_text_video_revision =
        renderer->video_scene ? ui_video_scene_text_paint_revision(*renderer->video_scene) : 0;
    renderer->interaction_text_audio_revision =
        renderer->audio_scene ? ui_audio_scene_text_revision(*renderer->audio_scene) : 0;
    return true;
}
MediaAtlasGpuResult renderer_present_video_frame(D3D11Renderer*, MediaAssetKey,
                                                 const GpuVideoFrame&, u32) noexcept {
    ++video_focus_runtime_test::account.gpu_publications;
    return {{}, E_FAIL, MediaAtlasGpuError::invalid_input};
}
namespace app_detail {
UiSearchScene* application_search_scene() noexcept { return video_focus_runtime_test::mutable_search_scene; }
bool refresh_account_frame(bool) noexcept {
    ++video_focus_runtime_test::media_refreshes;
    return true;
}
void application_user_settings_request_content_confirmation() noexcept {
    ++video_focus_runtime_test::confirmations;
}
bool application_image_viewer_open_video(u64, u32, bool, u8, bool, const ApplicationSearchSourceTicket*,
                                         MediaAssetKey, UiRect) noexcept {
    if (!video_focus_runtime_test::viewer_allowed)
        return false;
    ++video_focus_runtime_test::viewer_opens;
    g_app.image_viewer.scene.active = true;
    return true;
}
bool application_image_viewer_video_geometry(MediaAssetKey, UiRect* preview, UiRect* clip) noexcept {
    if (!g_app.image_viewer.scene.active || !preview || !clip)
        return false;
    *preview = video_focus_runtime_test::viewer_preview;
    *clip = {0, 0, 640, 480};
    return true;
}
void application_image_viewer_close() noexcept {
    g_app.image_viewer.scene.active = false;
    application_video_viewer_detach();
}
void application_image_viewer_dismiss() noexcept {
    application_image_viewer_close();
}
bool application_downloads_begin(u64 message, u64 attachment, bool snapshot) noexcept {
    ++video_focus_runtime_test::downloads;
    video_focus_runtime_test::download_message = message;
    video_focus_runtime_test::download_attachment = attachment;
    video_focus_runtime_test::download_snapshot = snapshot;
    return true;
}
bool application_downloads_begin_search(const ApplicationSearchSourceTicket& source, u64 attachment,
                                        bool snapshot) noexcept {
    return application_downloads_begin(source.message_id, attachment, snapshot);
}
bool application_search_source_ticket(u32 index, ApplicationSearchSourceTicket* ticket) noexcept {
    if (!video_focus_runtime_test::search_current || !ticket ||
        index != video_focus_runtime_test::search_view.ticket.index)
        return false;
    *ticket = video_focus_runtime_test::search_view.ticket;
    return true;
}
bool application_search_source_resolve(const ApplicationSearchSourceTicket& ticket, bool snapshot,
                                       ApplicationSearchSourceView* source) noexcept {
    const auto& view = video_focus_runtime_test::search_view;
    if (!video_focus_runtime_test::search_current || !source ||
        ticket.message_id != view.ticket.message_id ||
        ticket.publication_revision != view.ticket.publication_revision ||
        ticket.index != view.ticket.index)
        return false;
    *source = view;
    source->snapshot = snapshot;
    source->rich = snapshot ? message_rich_store_snapshot(*view.rich_store, ticket.message_id)
                            : message_rich_store_find(*view.rich_store, ticket.message_id);
    return source->rich != nullptr;
}
bool application_search_source_resolve(u64 id, u32 publication, bool snapshot,
                                       ApplicationSearchSourceView* source) noexcept {
    auto ticket = video_focus_runtime_test::search_view.ticket;
    ticket.message_id = id;
    ticket.publication_revision = publication;
    return application_search_source_resolve(ticket, snapshot, source);
}
bool application_settings_active() noexcept {
    return video_focus_runtime_test::settings;
}
bool application_friends_active() noexcept {
    return false;
}
void application_composer_text_input_suspend() noexcept {
}
bool application_call_pump(bool*) noexcept {
    return true;
}
u64 application_call_next_wake() noexcept {
    return 0;
}
} // namespace app_detail
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_video_focus_runtime_entry() noexcept {
    maicord::video_focus_runtime_test::run();
    const bool failed = maicord::video_focus_runtime_test::failures != 0;
    constexpr char passed_text[] = "PASS video focus runtime\n";
    constexpr char failed_text[] = "FAIL video focus runtime\n";
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), failed ? failed_text : passed_text,
              failed ? sizeof(failed_text) - 1 : sizeof(passed_text) - 1, &written, nullptr);
    ExitProcess(failed ? 1 : 0);
}
