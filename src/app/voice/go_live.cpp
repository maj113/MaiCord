#include "go_live.hpp"
#include "go_live_thumbnails.hpp"
#include "../account/account_presentation.hpp"
#include "../account/account_ui_runtime.hpp"
#include "../chat/composer/composer.hpp"
#include "../chat/messages/interaction.hpp"
#include "../shell/cursor.hpp"
#include "../shell/input.hpp"
#include "../shell/state.hpp"
#include "../shell/toast.hpp"
#include "../shell/window_events.hpp"
#include "../shell/window_state.hpp"
#include "../shell/overlay_input.hpp"
#include "voice.hpp"
#include "maicord/platform/audio/stream_audio.hpp"
#include "maicord/app/gateway/stream_session.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/os_text.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/core/time.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/os/thread.hpp"
#include "maicord/platform/desktop/shell_open.hpp"
#include "maicord/platform/window/key.hpp"
#include "maicord/protocol/voice/video_signaling.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/render/media/atlas.hpp"
#include "maicord/ui/core/cursor.hpp"
#include "maicord/ui/kit/scene_kit.hpp"
#include "maicord/ui/voice/go_live_scene.hpp"

namespace maicord::app_detail {
namespace {
// The official client lists sources at 447 x 251 and refreshes them every second while the dialog is open.
constexpr u32 kThumbnailWidth = 448, kThumbnailHeight = 252, kThumbnailBytes = kThumbnailWidth * kThumbnailHeight * 4;
constexpr u64 kThumbnailRefreshMs = 1'000, kWorkerPollMs = 20;
// Snapshot polling while a stream starts, runs or ends; nothing otherwise.
constexpr u64 kStartingPollMs = 100, kLivePollMs = 500;
constexpr u64 kMenuOwner = 0x474c'4956'4d45'4e55ull;
// Menu actions (the stream menu's popup owner routes them here).
enum : u32 {
    kActionBase = 0x7a11'0000,
    kPresetVideo = kActionBase + 1,
    kPresetText,
    kPresetCustom,
    kResolutionFirst = kActionBase + 0x10, // + index in kStreamResolutions
    kFpsFirst = kActionBase + 0x20,        // + index in kStreamFrameRates
    kMuteAudio = kActionBase + 0x30,
    kHidePreview,
    kUnlockQuality,
    kStop = kActionBase + 0x40,
    kChangeSource,
    kShareAudio,
    kLiveFpsFirst = kActionBase + 0x50,
    kLiveResolutionFirst = kActionBase + 0x60,
    kViewerFirst = kActionBase + 0x70, // + index in the snapshot's viewer ids
    kTitleFirst = kActionBase + 0xa0,  // Section titles, never enabled: a menu's actions must all differ.
    kActionLast = kActionBase + 0xff,
};
enum class Menu : u8 { none, options, manage, viewers };

struct ThumbnailJob {
    GpuDevice* device;
    u32 count;
    CaptureSource sources[kUiGoLiveMaximumSources];
    u32 width[kUiGoLiveMaximumSources], height[kUiGoLiveMaximumSources];
    bool ok[kUiGoLiveMaximumSources];
    u8 pixels[kUiGoLiveMaximumSources][kThumbnailBytes];
};

struct GoLiveUi {
    GoLiveSnapshot snapshot;
    u64 polled_ms;
    bool snapshot_valid, start_pending, reported;
    // The dialog.
    bool open, busy, change_source;
    u32 held_key, revision;
    UiGoLiveTab tab;
    UiGoLiveSystemKind kind;
    UiGoLiveScene scene;
    UiGoLiveContent content;
    CaptureSource capture[kUiGoLiveMaximumSources];
    u32 capture_count;
    UiGoLiveSource sources[kUiGoLiveMaximumSources];
    GoLiveThumbnails thumbnails;
    Thread* worker;
    ThumbnailJob* job;
    u64 next_refresh_ms;
    char summary[48], detail[96], notice[160];
    u32 summary_bytes, detail_bytes, notice_bytes;
    // Menus.
    Menu menu;
    UiRect menu_anchor;
    char labels[3][64];
};
GoLiveUi ui{};

u64 now_ms() noexcept {
    return clock_monotonic_milliseconds();
}
bool command(const GoLiveCommand& c) noexcept {
    auto& h = g_app.account;
    return h.instance && account_module_go_live(h.instance, &c);
}
bool streaming(const GoLiveSnapshot& s) noexcept {
    return s.state == GoLiveState::starting || s.state == GoLiveState::connecting || s.state == GoLiveState::live ||
           s.state == GoLiveState::reconnecting;
}
bool live() noexcept {
    return ui.snapshot_valid && streaming(ui.snapshot);
}
// With "Unlock all qualities" every quality counts as allowed, as with Nitro and a level 3 server: Discord checks the
// account's perks only in its client.
u8 premium_type() noexcept {
    if (application_voice_preferences().stream_unlocked)
        return 2;
    return g_app.account_ui ? g_app.account_ui->chat_state.self_premium_type : 0;
}
u8 guild_tier() noexcept {
    if (application_voice_preferences().stream_unlocked)
        return 3;
    const auto* c = application_voice_call();
    const auto* record = c && c->guild_id && g_app.account_ui
                             ? guild_detail_store_find(g_app.account_ui->guild_details, c->guild_id)
                             : nullptr;
    return record ? record->premium_tier : 0;
}
// No hardware H.264 encoder (known once the renderer's device reached the module): nothing can stream.
bool no_encoder() noexcept {
    return ui.snapshot_valid && ui.snapshot.encodable_known &&
           !(ui.snapshot.encodable & voice_video_codec_bit(VoiceVideoCodec::h264));
}
bool can_capture() noexcept {
    const auto support = capture_support();
    return support.screens || support.windows || support.system_picker;
}
bool allowed_in(u64 channel_id) noexcept {
    const auto* channel =
        g_app.account_ui ? chat_state_store_find_channel(g_app.account_ui->chat_state, channel_id) : nullptr;
    if (!channel)
        return false;
    // Calls have no permissions; a server channel needs Video (Stream).
    return channel_is_private(channel->type) ||
           (channel->permissions_resolved && (channel->actions & channel_action_stream));
}

// Settings.
VoicePreferences& prefs() noexcept {
    return application_voice_preferences();
}
StreamPreset preset() noexcept {
    return static_cast<StreamPreset>(prefs().stream_preset);
}
StreamQuality custom() noexcept {
    return {prefs().stream_resolution, prefs().stream_fps};
}
StreamQuality resolved() noexcept {
    return stream_preset_quality(preset(), custom(), premium_type(), guild_tier());
}
// Sound sharing never carries MaiCord's own output: where the system cannot leave it out there is none, and the
// toggles say why.
bool audio_available() noexcept {
    return stream_audio_available(stream_audio_method());
}
Utf8View audio_unavailable_reason() noexcept {
    return stream_audio_method() == StreamAudioMethod::unavailable_windows_version
               ? MAICORD_OS_TEXT("Sound sharing needs Windows 10 version 2004 or later.",
                                 "Sound sharing needs a newer system.")
               : utf8_literal("Sound sharing needs PipeWire.");
}
GoLiveSettings settings() noexcept {
    const auto q = resolved();
    return {q.resolution, q.fps, preset(), !prefs().stream_muted && audio_available(), {}};
}

// Labels.
template <u32 N> TextBank text(char (&buffer)[N]) noexcept {
    return {buffer, N, 0};
}
void resolution(TextBank& t, u16 value) noexcept {
    if (value) {
        t.number(value);
        t.add("p");
    } else {
        t.add("Source");
    }
}
Utf8View preset_name(StreamPreset p) noexcept {
    return p == StreamPreset::video ? utf8_literal("Gaming")
           : p == StreamPreset::text ? utf8_literal("Screenshare")
                                     : utf8_literal("Custom");
}

// The footer's summary: Stream Mode, then what it streams at.
void project_summary() noexcept {
    const auto q = resolved();
    auto s = text(ui.summary);
    ui.summary_bytes = s.add(preset_name(preset())).bytes;
    auto d = text(ui.detail);
    if (preset() == StreamPreset::video)
        d.add("Smoother video \xE2\x80\xA2 ");
    else if (preset() == StreamPreset::text)
        d.add("Clearer text \xE2\x80\xA2 ");
    resolution(d, q.resolution);
    d.add(" ");
    d.number(q.fps);
    d.add("fps");
    if (prefs().stream_muted && audio_available())
        d.add(" \xE2\x80\xA2 Stream audio muted");
    ui.detail_bytes = d.used;
}

// Thumbnails live where the renderer routes their keys (the large preview atlas) and are forgotten when the dialog
// closes or a source leaves the list (go_live_thumbnails.hpp).
bool upload_thumbnail(void*, const MediaAssetKey& key, u32 width, u32 height, const u8* rgba) noexcept {
    return renderer_upload_media_straight_rgba8(&g_app.renderer, key, static_cast<u16>(width), static_cast<u16>(height),
                                                width * 4, {rgba, width * height * 4})
        .ok();
}
void forget_thumbnail(void*, const MediaAssetKey& key) noexcept {
    if (MediaAtlas* atlas =
            g_app.renderer.preview_media_atlas ? g_app.renderer.preview_media_atlas : g_app.renderer.media_atlas)
        media_atlas_forget(atlas, key);
}
constexpr GoLiveThumbnailAtlas kThumbnailAtlas{nullptr, upload_thumbnail, forget_thumbnail};
void project_content() noexcept;
u32 worker_main(void* argument) noexcept {
    auto* job = static_cast<ThumbnailJob*>(argument);
    for (u32 i = 0; i < job->count; ++i)
        job->ok[i] = capture_thumbnail(job->device, job->sources[i], kThumbnailWidth, kThumbnailHeight, job->pixels[i],
                                       &job->width[i], &job->height[i]);
    return 0;
}
bool tab_has(const CaptureSource& s) noexcept {
    return (s.kind == CaptureSourceKind::screen) == (ui.tab == UiGoLiveTab::screens);
}
void project_sources() noexcept {
    u32 count = 0;
    for (u32 i = 0; i < ui.capture_count; ++i) {
        const auto& c = ui.capture[i];
        if (!tab_has(c))
            continue;
        const auto* t = go_live_thumbnail_find(ui.thumbnails, c.handle);
        ui.sources[count++] = {c.handle, {c.name, c.name_bytes}, t ? t->key : MediaAssetKey{},
                               t ? t->width : u16{}, t ? t->height : u16{}, c.kind == CaptureSourceKind::screen};
    }
    ui.content.sources = {ui.sources, count};
}
// Lists the sources and starts a thumbnail pass for the shown tab (one at a time, off the UI thread).
void refresh_sources(u64 now) noexcept {
    ui.next_refresh_ms = now + kThumbnailRefreshMs;
    ui.capture_count = capture_sources({ui.capture, kUiGoLiveMaximumSources});
    // Thumbnails of sources that left the list go.
    u64 listed[kUiGoLiveMaximumSources];
    for (u32 i = 0; i < ui.capture_count; ++i)
        listed[i] = ui.capture[i].handle;
    go_live_thumbnails_prune(&ui.thumbnails, kThumbnailAtlas, listed, ui.capture_count);
    project_sources();
    ++ui.revision;
    if (ui.worker || !g_app.renderer.graphics.device || !g_app.window || application_window_minimized())
        return;
    if (!ui.job)
        ui.job = commit_pages<ThumbnailJob>();
    if (!ui.job)
        return;
    ui.job->device = g_app.renderer.graphics.device;
    ui.job->count = 0;
    for (u32 i = 0; i < ui.capture_count; ++i)
        if (tab_has(ui.capture[i]))
            ui.job->sources[ui.job->count++] = ui.capture[i];
    if (ui.job->count)
        ui.worker = thread_start(worker_main, ui.job, 64 * 1024, nullptr);
}
// A finished pass: each picture replaces its source's previous one, which stays resident until the scene took the
// new key (application_go_live_modal_synchronize), so no tile is ever drawn without its picture.
void collect_thumbnails() noexcept {
    if (!ui.worker || !thread_wait(ui.worker, 0))
        return;
    thread_close(ui.worker);
    ui.worker = nullptr;
    if (!ui.open)
        return;
    for (u32 i = 0; i < ui.job->count; ++i)
        if (ui.job->ok[i])
            go_live_thumbnail_publish(&ui.thumbnails, kThumbnailAtlas, ui.job->sources[i].handle, ui.job->width[i],
                                      ui.job->height[i], ui.job->pixels[i]);
    project_sources();
    project_content();
    application_go_live_modal_synchronize();
    application_repaint();
}
void stop_worker() noexcept {
    if (ui.worker) {
        thread_wait(ui.worker, INFINITE);
        thread_close(ui.worker);
        ui.worker = nullptr;
    }
    if (ui.job) {
        pages_release(ui.job, sizeof(ThumbnailJob));
        ui.job = nullptr;
    }
    go_live_thumbnails_release(&ui.thumbnails, kThumbnailAtlas);
}

// Why this computer cannot stream, or {} when it can.
Utf8View unavailable_reason() noexcept {
    if (no_encoder())
        return utf8_literal("Your graphics card has no hardware H.264 video encoder, which streaming needs.");
    if (!can_capture())
        return MAICORD_OS_TEXT("Windows can't capture the screens and windows of this computer.",
                               "Screen sharing needs the desktop portal's screen cast and PipeWire, which this "
                               "computer doesn't have.");
    return {};
}
void project_content() noexcept {
    project_summary();
    auto& c = ui.content;
    const Utf8View reason = unavailable_reason();
    const auto support = capture_support();
    c.mode = reason.bytes            ? UiGoLiveMode::unavailable
             : support.system_picker ? UiGoLiveMode::system_picker
                                     : UiGoLiveMode::picker;
    c.title = c.mode == UiGoLiveMode::unavailable ? utf8_literal("Screen Sharing Unavailable")
                                                  : utf8_literal("Share a screen, window, or app");
    c.body = reason;
    c.summary = {ui.summary, ui.summary_bytes};
    c.detail = {ui.detail, ui.detail_bytes};
    c.notice = {ui.notice, ui.notice_bytes};
    c.tab = ui.tab;
    c.kind = ui.kind;
    c.busy = ui.busy;
    c.change_source = ui.change_source;
    // Without Nitro, the HD line (Nitro Classic streams 1080p and 1440p; Nitro streams everything).
    c.nitro = premium_type() != 2;
    c.revision = ++ui.revision;
}

void close_menu() noexcept {
    if (ui.menu != Menu::none && g_app.popup_scene.active && g_app.popup_scene.owner_id == kMenuOwner)
        dismiss_popup();
    ui.menu = Menu::none;
}
PopupItem row(const Utf8View& label, u32 action, u16 flags, const Utf8View& detail = {}) noexcept {
    PopupItem item{};
    popup_item_set(&item, label, action, flags);
    if (detail.bytes) {
        item.detail = detail;
        item.presentation.secondary_line = 1;
    }
    return item;
}
u16 checked(bool on) noexcept {
    return static_cast<u16>(popup_item_enabled | (on ? popup_item_checked : 0));
}
// A quality choice of a menu: a checked row, or a disabled one asking for Nitro where the account's tier lacks it.
PopupItem quality_row(const Utf8View& label, u32 action, StreamQuality quality, bool selected, u8 premium,
                      u8 tier) noexcept {
    return stream_quality_allowed(StreamPreset::custom, quality, premium, tier)
               ? row(label, action, checked(selected))
               : row(label, action, popup_item_none, utf8_literal("Nitro"));
}
// Without sound sharing the row stays, disabled, and says why.
PopupItem audio_row(const Utf8View& label, u32 action, bool on) noexcept {
    return audio_available() ? row(label, action, checked(on))
                             : row(label, action, popup_item_none, audio_unavailable_reason());
}
constexpr Utf8View kResolutions[]{utf8_literal("480p"), utf8_literal("720p"), utf8_literal("1080p"),
                                  utf8_literal("1440p"), utf8_literal("Source")};
// A preset's line in the Options menu: "Smoother video (720p, 30fps)".
Utf8View preset_detail(char (&buffer)[64], const Utf8View& lead, StreamQuality q) noexcept {
    auto t = text(buffer);
    t.add(lead);
    resolution(t, q.resolution);
    t.add(", ");
    t.number(q.fps);
    t.add("fps)");
    return t.view();
}
// The menus open at 248 DIPs and grow to their rows' text up to 320, the official client's menu width; a longer
// second line ("Hide stream preview") wraps below its label.
bool show_menu(Menu menu, const UiRect& anchor, ConstSlice<PopupItem> items, bool above) noexcept {
    const auto opened = popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, kMenuOwner, items, anchor,
                                         application_viewport_dips(), 248.0f, above);
    if (!opened.ok() || !popup_scene_fit_content(&g_app.popup_scene, g_app.renderer.message_measure_cache, 320.0f)) {
        log_u64(LogLevel::warning, "go_live", "menu_popup_error", static_cast<u64>(opened.error));
        return false;
    }
    ui.menu = menu;
    ui.menu_anchor = anchor;
    set_popup_owner(PopupOwnerKind::stream);
    return refresh_popup_text();
}
// The dialog's Options: Stream Mode, the custom quality, stream audio and the preview.
bool open_options(const UiRect& anchor) noexcept {
    const u8 premium = premium_type(), tier = guild_tier();
    const auto q = resolved();
    PopupItem items[24];
    u32 n = 0;
    items[n++] = row(utf8_literal("Stream Mode"), kTitleFirst, popup_item_none);
    const StreamQuality video = stream_preset_quality(StreamPreset::video, {}, premium, tier);
    const StreamQuality clear = stream_preset_quality(StreamPreset::text, {}, premium, tier);
    items[n++] = row(preset_name(StreamPreset::video), kPresetVideo, checked(preset() == StreamPreset::video),
                     preset_detail(ui.labels[0], utf8_literal("Smoother video ("), video));
    items[n++] = row(preset_name(StreamPreset::text), kPresetText, checked(preset() == StreamPreset::text),
                     preset_detail(ui.labels[1], utf8_literal("Clearer text ("), clear));
    items[n++] = row(preset_name(StreamPreset::custom), kPresetCustom, checked(preset() == StreamPreset::custom));
    if (preset() == StreamPreset::custom) {
        // The dialog offers 720p and up, as the official client's; 1080p and up need Nitro or the server's boosts.
        items[n++] = row(utf8_literal("Screen Resolution"), kTitleFirst + 1, popup_item_separator_before);
        for (u32 i = 1; i < 5; ++i)
            items[n++] = quality_row(kResolutions[i], kResolutionFirst + i, {kStreamResolutions[i], q.fps},
                                     q.resolution == kStreamResolutions[i], premium, tier);
        static constexpr Utf8View kRates[]{utf8_literal("15fps"), utf8_literal("30fps"), utf8_literal("60fps")};
        items[n++] = row(utf8_literal("Frame Rate"), kTitleFirst + 2, popup_item_separator_before);
        for (u32 i = 0; i < 3; ++i)
            items[n++] = quality_row(kRates[i], kFpsFirst + i, {q.resolution, kStreamFrameRates[i]},
                                     q.fps == kStreamFrameRates[i], premium, tier);
    }
    PopupItem mute = audio_row(utf8_literal("Mute stream audio"), kMuteAudio, prefs().stream_muted);
    mute.flags |= popup_item_separator_before;
    items[n++] = mute;
    items[n++] = row(utf8_literal("Hide stream preview"), kHidePreview, checked(prefs().stream_hide_preview),
                     utf8_literal("Users will not be able to see a preview of your stream before joining."));
    items[n++] = row(utf8_literal("Unlock all qualities"), kUnlockQuality, checked(prefs().stream_unlocked),
                     utf8_literal("Every resolution and frame rate, without Nitro or server boosts."));
    return show_menu(Menu::options, anchor, {items, n}, true);
}
// While live: stop, change the source, stream audio and the quality, as the official client's stream menu.
bool open_manage(const UiRect& anchor) noexcept {
    const u8 premium = premium_type(), tier = guild_tier();
    const auto& s = ui.snapshot.settings;
    PopupItem items[16];
    u32 n = 0;
    PopupItem stop = row(utf8_literal("Stop Streaming"), kStop, popup_item_enabled | popup_item_danger);
    stop.presentation = ui_menu_row_presentation(UiIcon::screen_share_off);
    items[n++] = stop;
    // The system's picker is asked again on Linux; the official client offers no change there.
    if (!capture_support().system_picker) {
        PopupItem change = row(utf8_literal("Change Stream"), kChangeSource, popup_item_enabled);
        change.presentation = ui_menu_row_presentation(UiIcon::screen_share);
        items[n++] = change;
    }
    items[n++] = audio_row(utf8_literal("Share Stream Audio"), kShareAudio, s.sound);
    static constexpr Utf8View kRates[]{utf8_literal("15 FPS"), utf8_literal("30 FPS"), utf8_literal("60 FPS")};
    items[n++] = row(utf8_literal("Frame Rate"), kTitleFirst, popup_item_separator_before);
    for (u32 i = 0; i < 3; ++i)
        items[n++] = quality_row(kRates[i], kLiveFpsFirst + i, {s.resolution, kStreamFrameRates[i]},
                                 s.fps == kStreamFrameRates[i], premium, tier);
    items[n++] = row(utf8_literal("Resolution"), kTitleFirst + 1, popup_item_separator_before);
    // The text preset's 5 fps keeps Source at 30 fps on the list, as the official client does.
    const u8 fps = s.preset == StreamPreset::text ? 30 : s.fps;
    for (u32 i = 0; i < 5; ++i)
        items[n++] = quality_row(kResolutions[i], kLiveResolutionFirst + i, {kStreamResolutions[i], fps},
                                 s.resolution == kStreamResolutions[i], premium, tier);
    return show_menu(Menu::manage, anchor, {items, n}, true);
}

void set_notice(const Utf8View& message) noexcept {
    auto t = text(ui.notice);
    ui.notice_bytes = t.add(message).bytes;
}
void open_modal(bool change_source) noexcept {
    if (g_app.popup_scene.active)
        dismiss_popup();
    ui.open = true;
    ui.busy = false;
    ui.change_source = change_source;
    ui.tab = UiGoLiveTab::applications;
    ui.kind = UiGoLiveSystemKind::screen;
    ui.notice_bytes = 0;
    ui.scene.scroll = 0;
    ui.next_refresh_ms = 0;
    project_content();
    application_go_live_modal_synchronize();
    application_repaint();
}
void update_live(GoLiveSettings next) noexcept {
    GoLiveCommand c{};
    c.kind = GoLiveCommandKind::update;
    c.settings = next;
    if (command(c))
        application_repaint();
}
// Starts the stream (or changes the source of the live one) from the dialog.
void start(const CaptureSource* source) noexcept {
    const auto* c = application_voice_call();
    if (!c)
        return;
    GoLiveCommand cmd{};
    const bool system = capture_support().system_picker;
    if (source)
        cmd.source = *source;
    cmd.system_picker = system;
    if (system)
        cmd.source.kind = ui.kind == UiGoLiveSystemKind::window ? CaptureSourceKind::window : CaptureSourceKind::screen;
    if (ui.change_source && live()) {
        cmd.kind = GoLiveCommandKind::change_source;
    } else {
        cmd.kind = GoLiveCommandKind::start;
        cmd.guild_id = c->guild_id;
        cmd.channel_id = c->channel_id;
        cmd.settings = settings();
    }
    if (!command(cmd)) {
        set_notice(utf8_literal("Stream failed to start :("));
        project_content();
        return;
    }
    ui.start_pending = cmd.kind == GoLiveCommandKind::start;
    ui.reported = false;
    if (system) {
        // The system's picker is up: the dialog waits for it, as the official client's does.
        ui.busy = true;
        ui.notice_bytes = 0;
        project_content();
        return;
    }
    application_go_live_modal_close();
}

// A stream that ended for a reason the user should hear about (not a stop, a leave or a picker cancelled).
Utf8View end_message(const GoLiveSnapshot& s, bool was_live) noexcept {
    switch (s.end) {
    case GoLiveEnd::no_hardware_encoder:
        return utf8_literal("Your graphics card has no hardware H.264 video encoder, which streaming needs.");
    case GoLiveEnd::capture_unavailable:
        return utf8_literal("Couldn't capture that screen or window.");
    case GoLiveEnd::not_allowed:
        return utf8_literal("You can\xE2\x80\x99t stream into this channel.");
    case GoLiveEnd::stream_full:
        return utf8_literal("Sorry, this stream has the max number of viewers.");
    case GoLiveEnd::connection_failed:
    case GoLiveEnd::timeout:
        return was_live ? utf8_literal("Stream ended due to a system error (connection)")
                        : utf8_literal("Stream failed to start :(");
    case GoLiveEnd::encoder_failed:
        return was_live ? utf8_literal("Stream ended due to a system error (encoder)")
                        : utf8_literal("Stream failed to start :(");
    default:
        return {};
    }
}
void observe(const GoLiveSnapshot& next) noexcept {
    const bool was_live = ui.snapshot_valid && ui.snapshot.state == GoLiveState::live;
    const bool was_streaming = live();
    ui.snapshot = next;
    ui.snapshot_valid = true;
    if (streaming(next) && next.state != GoLiveState::starting && ui.open && ui.busy)
        application_go_live_modal_close(); // The system's picker returned a source.
    if (next.state == GoLiveState::ended && (was_streaming || ui.start_pending) && !ui.reported) {
        ui.reported = true;
        ui.start_pending = false;
        if (next.end == GoLiveEnd::capture_denied) {
            // The picker was cancelled: back to the dialog's choices.
            ui.busy = false;
        } else if (const Utf8View message = end_message(next, was_live); message.bytes) {
            if (ui.open) {
                ui.busy = false;
                set_notice(message);
            } else
                application_toast_show(UiToastKind::failure, message);
        }
    }
    if (next.state == GoLiveState::live)
        ui.start_pending = false;
    if (ui.open)
        project_content();
    if (ui.menu == Menu::manage && !streaming(next))
        close_menu();
}
void poll(u64 now) noexcept {
    auto& h = g_app.account;
    GoLiveSnapshot next{};
    ui.polled_ms = now;
    if (!h.instance || !account_module_go_live_snapshot(h.instance, &next))
        return;
    if (ui.snapshot_valid && next.generation == ui.snapshot.generation)
        return;
    observe(next);
    application_repaint();
}
// A snapshot the account cannot give (no Gateway yet, or none any more) counts as idle: only a start from this dialog
// changes it, and every account pump reads it again until it can.
u64 poll_interval() noexcept {
    switch (ui.snapshot_valid ? ui.snapshot.state : GoLiveState::idle) {
    case GoLiveState::starting:
    case GoLiveState::connecting:
    case GoLiveState::reconnecting:
        return kStartingPollMs;
    case GoLiveState::live:
        return kLivePollMs;
    default:
        // Idle: only while something may change it (the dialog's capability line, a start in flight).
        return ui.open || ui.start_pending ? kLivePollMs : 0;
    }
}

void activate_target(const UiGoLiveTarget& target) noexcept {
    switch (target.action) {
    case UiGoLiveAction::close:
        application_go_live_modal_close();
        break;
    case UiGoLiveAction::tab:
        if (static_cast<u32>(ui.tab) != target.index) {
            ui.tab = static_cast<UiGoLiveTab>(target.index);
            ui.next_refresh_ms = 0;
            project_sources();
            project_content();
        }
        break;
    case UiGoLiveAction::source:
        for (u32 i = 0; i < ui.capture_count; ++i)
            if (ui.capture[i].handle == target.id) {
                start(&ui.capture[i]);
                break;
            }
        break;
    case UiGoLiveAction::kind:
        ui.kind = static_cast<UiGoLiveSystemKind>(target.index);
        project_content();
        break;
    case UiGoLiveAction::select:
        if (ui.kind != UiGoLiveSystemKind::none && !ui.busy)
            start(nullptr);
        break;
    case UiGoLiveAction::options:
        open_options(ui.scene.options_rectangle);
        break;
    case UiGoLiveAction::nitro:
        desktop_open_target(g_app.window, L"https://discord.com/nitro");
        break;
    default:
        break;
    }
    application_go_live_modal_synchronize();
    application_repaint();
}
} // namespace

UiCallShare application_go_live_share(u64 channel_id) noexcept {
    if (live())
        return UiCallShare::live;
    // A computer that cannot stream (no hardware H.264 encoder, nothing to capture) still opens the dialog, which says
    // why; the official client only disables the button.
    return allowed_in(channel_id) ? UiCallShare::available : UiCallShare::not_allowed;
}

bool application_go_live_button(const UiRect& anchor) noexcept {
    const auto* c = application_voice_call();
    if (!c || !g_app.account_ui)
        return false;
    if (live())
        return open_manage(anchor);
    if (application_go_live_share(c->channel_id) != UiCallShare::available)
        return false;
    open_modal(false);
    return true;
}

bool application_go_live_tile(u64 channel_id, UiCallParticipant* p) noexcept {
    if (!p || !live() || !g_app.account_ui)
        return false;
    const auto& s = ui.snapshot;
    if (s.key.channel_id != channel_id)
        return false;
    const u64 self = g_app.account_ui->chat_state.self_user_id;
    *p = {};
    p->user_id = gateway_stream_video_user(self);
    p->self = true;
    p->stream = s.state == GoLiveState::live           ? UiCallStream::live
                : s.state == GoLiveState::reconnecting ? UiCallStream::reconnecting
                                                       : UiCallStream::connecting;
    p->paused = s.paused;
    p->viewers = s.viewer_count;
    if (const auto* user = user_store_find(g_app.account_ui->user_store, self))
        p->name = user_store_name(g_app.account_ui->user_store, *user);
    return true;
}

bool application_go_live_preview(VideoStreamFrame* frame) noexcept {
    auto& h = g_app.account;
    return live() && h.instance && account_module_go_live_preview(h.instance, frame);
}

bool application_go_live_viewers_open(const UiRect& anchor) noexcept {
    if (!live() || !g_app.account_ui || !ui.snapshot.viewer_count)
        return false;
    const auto& s = ui.snapshot;
    auto& users = g_app.account_ui->user_store;
    PopupItem items[1 + kGoLiveViewerIds];
    u32 n = 0;
    auto header = text(ui.labels[2]);
    header.add("Spectators - ");
    header.number(s.viewer_count);
    items[n++] = row(header.view(), kTitleFirst, popup_item_none);
    for (u32 i = 0; i < s.viewer_count && i < kGoLiveViewerIds; ++i) {
        const auto* user = user_store_find(users, s.viewer_ids[i]);
        PopupItem item = row(user ? user_store_name(users, *user) : utf8_literal("Unknown user"), kViewerFirst + i,
                             popup_item_enabled);
        item.flags |= popup_item_avatar;
        item.media_id = s.viewer_ids[i];
        items[n++] = item;
    }
    return show_menu(Menu::viewers, anchor, {items, n}, true);
}

void application_go_live_stop() noexcept {
    GoLiveCommand c{};
    c.kind = GoLiveCommandKind::stop;
    ui.start_pending = false;
    if (command(c))
        application_repaint();
}

bool application_go_live_menu_action(u32 action) noexcept {
    return action > kActionBase && action <= kActionLast && ui.menu != Menu::none &&
           g_app.popup_owner_kind == PopupOwnerKind::stream && g_app.popup_scene.owner_id == kMenuOwner;
}

bool application_go_live_menu_activate(u32 action) noexcept {
    const Menu menu = ui.menu;
    const UiRect anchor = ui.menu_anchor;
    close_menu();
    auto& p = prefs();
    bool persist = false;
    if (action >= kPresetVideo && action <= kPresetCustom) {
        p.stream_preset = static_cast<u8>(action - kActionBase);
        persist = true;
    } else if (action >= kResolutionFirst && action < kResolutionFirst + 5) {
        p.stream_resolution = kStreamResolutions[action - kResolutionFirst];
        persist = true;
    } else if (action >= kFpsFirst && action < kFpsFirst + 3) {
        p.stream_fps = kStreamFrameRates[action - kFpsFirst];
        persist = true;
    } else if (action == kMuteAudio) {
        p.stream_muted = !p.stream_muted;
        persist = true;
    } else if (action == kHidePreview) {
        p.stream_hide_preview = !p.stream_hide_preview;
        persist = true;
    } else if (action == kUnlockQuality) {
        p.stream_unlocked = !p.stream_unlocked;
        persist = true;
    } else if (action == kStop) {
        application_go_live_stop();
    } else if (action == kChangeSource) {
        open_modal(true);
    } else if (action == kShareAudio && live()) {
        // As the official client's, the choice is also remembered for the next stream.
        auto next = ui.snapshot.settings;
        next.sound = !next.sound;
        p.stream_muted = !next.sound;
        persist = true;
        update_live(next);
    } else if (action >= kLiveFpsFirst && action < kLiveFpsFirst + 3 && live()) {
        // A live quality change is custom quality, as the official client's.
        auto next = ui.snapshot.settings;
        next.preset = StreamPreset::custom;
        next.fps = kStreamFrameRates[action - kLiveFpsFirst];
        update_live(next);
    } else if (action >= kLiveResolutionFirst && action < kLiveResolutionFirst + 5 && live()) {
        auto next = ui.snapshot.settings;
        next.preset = StreamPreset::custom;
        if (next.fps == 5)
            next.fps = 30;
        next.resolution = kStreamResolutions[action - kLiveResolutionFirst];
        update_live(next);
    }
    if (persist) {
        application_voice_settings_changed(true);
        if (ui.open)
            project_content();
    }
    // The dialog's options stay open while choosing, as the official client's popout does.
    if (menu == Menu::options && ui.open && persist)
        open_options(anchor);
    application_repaint();
    return true;
}

void application_go_live_modal_close() noexcept {
    if (!ui.open)
        return;
    if (ui.menu == Menu::options)
        close_menu();
    ui.open = false;
    ui.busy = false;
    ui.scene.active = false;
    stop_worker();
    if (g_app.context_focus == &ui.scene.widgets)
        g_app.context_focus = nullptr;
    g_app.renderer.go_live_scene = nullptr;
    application_repaint();
}

bool application_go_live_modal_synchronize() noexcept {
    if (!ui.open) {
        g_app.renderer.go_live_scene = nullptr;
        return true;
    }
    // The call left or the account went away: the dialog has nothing to stream into.
    if (!g_app.account_ui || g_app.login.visible || !application_voice_call()) {
        application_go_live_modal_close();
        return true;
    }
    if (!ui_go_live_scene_update(&ui.scene, ui.content, application_viewport_dips())) {
        application_go_live_modal_close();
        return true;
    }
    // The scene holds the latest keys now: the pictures they replaced can go.
    go_live_thumbnails_synchronized(&ui.thumbnails, kThumbnailAtlas);
    g_app.renderer.go_live_scene = &ui.scene;
    g_app.context_focus = &ui.scene.widgets;
    return true;
}

bool application_go_live_modal_input(UiInputKind kind, u32 code, bool repeated) noexcept {
    // The key that closed the dialog keeps its character and release.
    if (ui.held_key) {
        if (kind == UiInputKind::key_up && code == ui.held_key) {
            ui.held_key = 0;
            return true;
        }
        if (kind == UiInputKind::character || ((kind == UiInputKind::key_down) && code == ui.held_key && repeated))
            return true;
        ui.held_key = 0;
    }
    // Its options menu takes the keys while open.
    if (!ui.open || (g_app.popup_scene.active && g_app.popup_scene.owner_id == kMenuOwner))
        return false;
    if (kind != UiInputKind::key_down && kind != UiInputKind::system_key_down)
        return kind != UiInputKind::key_up;
    if (kind == UiInputKind::system_key_down && code == key::f4)
        return false;
    bool moved = false;
    switch (code) {
    case key::escape:
        if (!repeated) {
            ui.held_key = code;
            application_go_live_modal_close();
        }
        return true;
    case key::tab: moved = ui_go_live_scene_focus_next(&ui.scene, application_shift_down()); break;
    case key::left: moved = ui_go_live_scene_focus_arrow(&ui.scene, -1, 0); break;
    case key::right: moved = ui_go_live_scene_focus_arrow(&ui.scene, 1, 0); break;
    case key::up: moved = ui_go_live_scene_focus_arrow(&ui.scene, 0, -1); break;
    case key::down: moved = ui_go_live_scene_focus_arrow(&ui.scene, 0, 1); break;
    case key::enter:
    case key::space:
        if (!repeated) {
            ui.held_key = code;
            activate_target(ui_go_live_scene_target(ui.scene, ui.scene.widgets.focused));
        }
        return true;
    default: return true;
    }
    if (moved) {
        g_app.keyboard_focus_visible = true;
        application_repaint();
    }
    return true;
}

bool application_go_live_modal_window_event(const WindowEvent& event) noexcept {
    if (!ui.open || !ui.scene.active)
        return false;
    // Its options menu sits over it and takes the pointer while open; a press outside the menu closes it.
    if (g_app.popup_scene.active && g_app.popup_scene.owner_id == kMenuOwner) {
        if (event.type == EventType::pointer_move)
            handle_popup_hover(event);
        else if (input_down(event))
            handle_popup_click(event);
        else if (event.type == EventType::cursor_query)
            application_set_cursor(UiCursor::arrow);
        return input_pointer(event) || event.type == EventType::cursor_query || event.type == EventType::context_menu;
    }
    if (input_vertical_wheel(event)) {
        if (ui_go_live_scene_scroll(&ui.scene, -input_wheel_notches(event) * 80))
            application_repaint();
        return true;
    }
    if (event.type == EventType::pointer_move) {
        bool changed = false;
        ui_go_live_scene_set_hover(&ui.scene, client_dip_x(event), client_dip_y(event), &changed);
        if (changed)
            application_repaint();
        return true;
    }
    if (event.type == EventType::cursor_query) {
        UiPoint point{};
        if (!client_cursor_dip(event, &point))
            return false;
        const auto target = ui_go_live_scene_hit_test(ui.scene, point.x, point.y);
        application_set_cursor(ui_action_cursor(target.action != UiGoLiveAction::none &&
                                                ui_rect_contains(ui.scene.panel, point.x, point.y)));
        return true;
    }
    if (input_down(event)) {
        const auto target = ui_go_live_scene_hit_test(ui.scene, client_dip_x(event), client_dip_y(event));
        if (target.action != UiGoLiveAction::none && target.action != UiGoLiveAction::close)
            ui_widget_manager_focus(&ui.scene.widgets, ui_widget_manager_hit_test(ui.scene.widgets, client_dip_x(event),
                                                                                  client_dip_y(event)));
        activate_target(target);
        return true;
    }
    // The other releases, the other buttons, tilts and the context menu are swallowed.
    return input_release(event) || input_other_button(event) || input_horizontal_wheel(event) ||
           event.type == EventType::context_menu;
}

void application_go_live_pump() noexcept {
    if (!g_app.account_ui) {
        application_go_live_modal_close();
        reset_value(&ui.snapshot);
        ui.snapshot_valid = false;
        return;
    }
    const u64 now = now_ms();
    const u64 interval = poll_interval();
    if (!ui.snapshot_valid || (interval && now - ui.polled_ms >= interval))
        poll(now);
    if (!ui.open)
        return;
    collect_thumbnails();
    if (ui.content.mode == UiGoLiveMode::picker && now >= ui.next_refresh_ms) {
        refresh_sources(now);
        project_content();
        application_repaint();
    }
}

u64 application_go_live_next_wake() noexcept {
    u64 wake = 0;
    const auto earliest = [&](u64 at) noexcept { wake = earliest_deadline(wake, at); };
    if (const u64 interval = poll_interval())
        earliest(ui.polled_ms + interval);
    if (ui.open && ui.worker)
        earliest(now_ms() + kWorkerPollMs);
    else if (ui.open && ui.content.mode == UiGoLiveMode::picker)
        earliest(ui.next_refresh_ms ? ui.next_refresh_ms : 1);
    return wake;
}

void application_go_live_release() noexcept {
    application_go_live_modal_close();
    stop_worker();
    ui_go_live_scene_release(&ui.scene);
    ui.snapshot_valid = false;
    ui.menu = Menu::none;
}
} // namespace maicord::app_detail
