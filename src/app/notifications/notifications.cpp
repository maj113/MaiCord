#include "taskbar_attention.hpp"
#include "../shell/window_state.hpp"
#include "desktop.hpp"
#include "notifications.hpp"
#include "maicord/core/os_text.hpp"
#include "maicord/core/module.hpp"
#include "../shell/state.hpp"
#include "../settings/streamer_mode.hpp"
#include "../settings/user_settings.hpp"
#include "../account/user_actions.hpp"
#include "../voice/voice.hpp"
#include "../account/friends.hpp"
#include "../shell/content_input.hpp"
#include "maicord/platform/desktop/notifications.hpp"
#include "maicord/platform/desktop/notification_identity.hpp"
#include "maicord/platform/desktop/system_tray.hpp"
#include "maicord/platform/desktop/clipboard_image.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/os/file.hpp"
#include "maicord/core/stable_storage.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/model/notifications/desktop_notification_queue.hpp"
#include "maicord/model/notifications/retention.hpp"
#include "maicord/model/notifications/sound_admission.hpp"
#include "maicord/app/notifications/sound_assets.hpp"
#include "maicord/ui/chat/messages/markdown.hpp"
#include "maicord/model/chat/game_store.hpp"
#include "../account/account_ui_runtime.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/log_event.hpp"
#include "maicord/app/gateway/client.hpp"
#include "maicord/app/gateway/client_dispatch.hpp"

namespace maicord::app_detail {
namespace {
constexpr wchar_t app_id[] = L"MaiCord.Desktop";
constexpr wchar_t identity_nonce[] = L"MaiCordDesktopIdentityV1";
constexpr GUID activator_id{0x92c1a487,0x6a5e,0x4a7c,{0x9c,0xcb,0x72,0xcf,0xee,0xa8,0x26,0x38}};
constexpr u32 guild_capacity = kChatStateGuildCapacity + 1;
constexpr u32 channel_capacity = kChatStateChannelCapacity;
struct Scope { u64 id, guild; u32 flags, known_gates, gates; };
struct Seen { u64 channel, message; };
struct Identity {
    wchar_t executable[1024], icon[1024];
    u32 icon_units;
    NotificationIdentityConfig config;
};
struct Display {
    DesktopNotificationQueue queue;
    DesktopNotificationQueueRow rows[kDesktopNotificationQueueRows];
    char bytes[kDesktopNotificationQueueBytes], scratch[kDesktopNotificationQueueItemBytes];
};
constexpr u32 projected_maximum_bytes=256+1024;
bool identity_paths(Identity& i) noexcept {
    const u32 n = application_desktop_registration_executable(i.executable,1024).count;
    if(!n) return false;
    // Toast app icon: MaiCord's mark as a PNG beside the profile data. It is
    // named in the registry only while the file exists; setup writes it.
    const DataPathResult icon_path = data_path(i.icon,1024,L"notification-icon.png");
    i.icon_units = icon_path.length;
    if(!icon_path.ok()) i.icon[0] = 0;
    FileStat icon{};
    const bool icon_present = i.icon_units && file_path_stat(i.icon, &icon) && icon.kind != FileKind::directory;
    i.config={{app_id,sizeof(app_id)/sizeof(wchar_t)-1},activator_id,{i.executable,n},{L"MaiCord",7},
        {i.icon,icon_present?i.icon_units:0},{identity_nonce,sizeof(identity_nonce)/sizeof(wchar_t)-1},{}};
    return true;
}
// Writes the tray/taskbar mark in the current theme's colours and names it in
// the identity. On failure the identity keeps Windows' generic icon.
void prepare_icon(Identity& i) noexcept {
    reset_value(&i.config.icon_path);
    if(!i.icon_units) return;
    constexpr u32 extent = kTrayIconMaximumExtent, pixels_bytes = extent*extent*4;
    const u64 png_bytes = clipboard_image_png_bytes(extent,extent);
    auto* memory = commit_pages<u8>(pixels_bytes*2+static_cast<SIZE_T>(png_bytes));
    if(!memory) return;
    const TrayIconPalette palette = application_tray_palette();
    auto* argb = reinterpret_cast<u32*>(memory);
    u8* rgba = memory+pixels_bytes;
    bool written = tray_icon_render(TrayIconState::normal,0,palette,extent,{argb,extent*extent});
    for(u32 p=0;written && p<extent*extent;++p) {
        rgba[p*4]=static_cast<u8>(argb[p]>>16); rgba[p*4+1]=static_cast<u8>(argb[p]>>8);
        rgba[p*4+2]=static_cast<u8>(argb[p]); rgba[p*4+3]=static_cast<u8>(argb[p]>>24);
    }
    written = written && clipboard_image_encode_png(rgba,extent,extent,extent*4,{rgba+pixels_bytes,static_cast<u32>(png_bytes)})==png_bytes;
    if(written) {
        data_path(i.icon,1024,L"notification-icon.png",true); // creates the MaiCord directory
        const HANDLE file = file_open(i.icon, {FileAccess::write, FileCreate::create_always, 0, 0});
        u32 count = 0;
        written = file!=INVALID_HANDLE_VALUE && file_write(file,rgba+pixels_bytes,static_cast<u32>(png_bytes),&count) && count==png_bytes;
        if(file!=INVALID_HANDLE_VALUE) file_close(file);
        if(!written) file_remove(i.icon);
    }
    pages_release(memory);
    if(written) i.config.icon_path={i.icon,i.icon_units};
}
// Enrolls or repairs the registry identity. COM is not needed: the identity
// is registry-only.
NotificationIdentityResult enroll(Identity& i) noexcept {
    prepare_icon(i);
    return notification_identity_create(i.config);
}
}
struct ApplicationNotifications {
    MessageNotificationStore settings;
    MessageNotificationGlobal global;
    StableStorage guilds, overrides, threads, staging, guild_scopes, channel_scopes;
    DesktopNotifications native;
    NotificationAudio audio;
    NotificationSoundAssetBank sound_assets;
    NotificationSoundThrottle sound_throttle;
    DesktopNotificationActivation pending;
    Seen seen[256];
    Display* display;
    NotificationRetention retention;
    u64 generation;
    u64 profile, audio_epoch, audio_failures_logged, selected, create_sequence;
    // Toast withdrawals deferred out of sent window messages (WM_ACTIVATE):
    // COM cannot call out during an input-synchronous call.
    u64 deferred_removals[4];
    u32 deferred_removal_count;
    u32 seen_next, guild_scope_count, channel_scope_count, os_setting, global_known_gates, global_gates;
    bool live, ready, snapshot, snapshot_failed, enrolled, focused, has_create_sequence, snapshot_partial, work_pending,
        backpressured, history_turn, audio_backpressured, audio_failed, audio_preference, mention_on_all;
    NotificationSoundpack current_pack;
    Utf8View status;
#if defined(MAICORD_NOTIFICATION_FIXTURE)
    void(*fixture_sink)(void*,const DesktopNotificationPayload&) noexcept;
    void* fixture_context;
    NotificationAudioResult(*fixture_audio_sink)(void*,const NotificationAudioRequest&) noexcept;
    NotificationAudioStatus(*fixture_audio_status)(void*) noexcept;
    u64(*fixture_clock)(void*) noexcept;
    void* fixture_audio_context;
    DesktopNotificationShowState fixture_show_state;
    DesktopNotificationResult(*fixture_retention_remove)(void*,DesktopNotificationActivation) noexcept;
    void* fixture_retention_context;
    u32 fixture_retention_failure_bank;
    bool fixture_suppress_popup;
    bool fixture_focus_known, fixture_focused;
#endif
};
namespace {
// A channel the user sees: listed, with View Channel.
bool viewable(const ChatStateStore& state,const ChannelRecord* channel) noexcept {
    return channel && chat_state_store_channel_visible(state,*channel) && (channel->actions&channel_action_view);
}
bool local_dnd(const Application& a) noexcept {
    if(a.self_status.queued) return a.self_status.desired==GatewayUserStatus::dnd;
    if(!a.account.instance) return false;
    const auto snapshot=account_module_self_status_snapshot(a.account.instance);
    return snapshot.authoritative && snapshot.status==GatewayUserStatus::dnd;
}
bool popup_sink(const Application& a,const ApplicationNotifications& n) noexcept {
#if defined(MAICORD_NOTIFICATION_FIXTURE)
    return a.preferences.desktop_enabled && (n.enrolled || n.fixture_sink);
#else
    return a.preferences.desktop_enabled && n.enrolled;
#endif
}
bool focused(const ApplicationNotifications& n) noexcept {
    (void)n;
#if defined(MAICORD_NOTIFICATION_FIXTURE)
    if(n.fixture_focus_known) return n.fixture_focused;
#endif
    return application_window_focused();
}
u64 sound_clock(const ApplicationNotifications& n) noexcept {
    (void)n;
#if defined(MAICORD_NOTIFICATION_FIXTURE)
    if(n.fixture_clock) return n.fixture_clock(n.fixture_audio_context);
#endif
    return clock_monotonic_milliseconds();
}
bool audio_sink(const ApplicationNotifications& n) noexcept {
#if defined(MAICORD_NOTIFICATION_FIXTURE)
    return n.fixture_audio_sink && n.fixture_audio_status && !n.audio_failed;
#else
    return n.audio.initialized && !n.audio_failed;
#endif
}
bool audio_capacity(const ApplicationNotifications& n,u32 needed) noexcept {
    if(!audio_sink(n)) return false;
#if defined(MAICORD_NOTIFICATION_FIXTURE)
    const auto state=n.fixture_audio_status(n.fixture_audio_context);
    return state.enabled && !state.stopping && !state.worker_exited &&
        state.queued<=kNotificationAudioQueueCapacity && state.outstanding<=kNotificationAudioVoiceCapacity &&
        needed<=kNotificationAudioQueueCapacity-state.queued && needed<=kNotificationAudioVoiceCapacity-state.outstanding;
#else
    return notification_audio_can_accept(&n.audio,needed);
#endif
}
void audio_error(ApplicationNotifications& n,NotificationAudioResult result) noexcept {
    if(result.ok()) return;
    LogEvent("audio_failed").field("error",static_cast<u64>(result.error)).error("native_error",static_cast<u32>(result.native_error)).write(LogLevel::warning,"notifications");
    // Terminal backend/launch failures disable only audio, never the Gateway.
    n.audio_failed=true;
    notification_sound_throttle_cancel(&n.sound_throttle);
}
void audio_observe(ApplicationNotifications& n) noexcept {
    if(!audio_sink(n)) return;
#if defined(MAICORD_NOTIFICATION_FIXTURE)
    const auto state=n.fixture_audio_status(n.fixture_audio_context);
#else
    const auto state=notification_audio_status(&n.audio);
#endif
    // A request can fail on the worker after submit accepted it.
    if(state.failed>n.audio_failures_logged) {
        n.audio_failures_logged=state.failed;
        LogEvent("audio_worker_failed").field("error",static_cast<u64>(state.error)).error("native_error",static_cast<u32>(state.native_error)).write(LogLevel::warning,"notifications");
    }
    if(state.worker_exited || state.stopping) audio_error(n,{NotificationAudioError::stopped,state.native_error});
}
void audio_reset(ApplicationNotifications& n,bool enabled) noexcept {
    if(n.audio_epoch==~u64{0}) { audio_error(n,{NotificationAudioError::timeline_exhausted,0}); return; }
    ++n.audio_epoch;
    notification_sound_throttle_generation(&n.sound_throttle,n.audio_epoch);
    // A failed enqueue must not bypass cancellation of earlier voices.
    // Worker exit/stopping may reject this reset, but PCM stays attached until join.
    if(n.audio.initialized) audio_error(n,notification_audio_reset(&n.audio,n.audio_epoch,enabled && !n.audio_failed));
}
void audio_preferences(Application& a,ApplicationNotifications& n) noexcept {
    notification_sound_assets_volume(&n.sound_assets,a.preferences.output_volume);
    // Local pack override changes future invocation mapping, never H's logical
    // pending args, an accepted clip ID, active voice, or account audio epoch.
    n.current_pack=a.preferences.soundpack;
    if(n.audio_preference!=a.preferences.message_sounds_enabled) {
        n.audio_preference=a.preferences.message_sounds_enabled;
        audio_reset(n,n.audio_preference && n.profile!=0);
    }
}
void sound_submit(ApplicationNotifications& n,u32 id,u64 epoch) noexcept {
    // Streamer Mode's "Disable all sound effects".
    if(!id || epoch!=n.audio_epoch || !n.audio_preference || application_streamer_disable_sounds() || !audio_sink(n)) return;
    if(id!=1 && id!=2) return;
    const u32 clip=notification_soundpack_clip(n.current_pack,id==2?NotificationSoundKind::message3:NotificationSoundKind::message1);
    if(!clip) return;
    const NotificationAudioRequest request{epoch,clip,.4f,{}};
#if defined(MAICORD_NOTIFICATION_FIXTURE)
    const auto result=n.fixture_audio_sink(n.fixture_audio_context,request);
#else
    const auto result=notification_audio_submit(&n.audio,request);
#endif
    if(!result.ok()) audio_error(n,result);
}
void sound_throttled(ApplicationNotifications& n,u32 id,u64 epoch) noexcept {
    if(!id || epoch!=n.audio_epoch || !n.audio_preference || !audio_sink(n)) return;
    const auto invocation=notification_sound_throttle_request(&n.sound_throttle,sound_clock(n),
        {epoch,0,.4f,id==2?NotificationSoundKind::message3:NotificationSoundKind::message1});
    if(invocation.invoke) sound_submit(n,invocation.request.kind==NotificationSoundKind::message3?2:1,invocation.request.generation);
}
Display* ensure_display(ApplicationNotifications& n) noexcept {
    if(n.display) return n.display;
    auto* display=commit_pages<Display>();
    if(!display) return nullptr;
    if(!desktop_notification_queue_initialize(&display->queue,{display->rows,kDesktopNotificationQueueRows},
        {display->bytes,kDesktopNotificationQueueBytes},{display->scratch,kDesktopNotificationQueueItemBytes})) {
        pages_release(display); return nullptr;
    }
    desktop_notification_queue_reset(&display->queue,n.profile,n.generation);
    n.display=display; return display;
}
void purge_display(ApplicationNotifications& n) noexcept {
    if(n.display) desktop_notification_queue_purge_all(&n.display->queue);
}
// Queue backpressure, admission scopes and create dedupe start over with a new account session.
void reset_ingress(ApplicationNotifications& n) noexcept {
    n.backpressured=n.audio_backpressured=false;
    n.guild_scope_count=n.channel_scope_count=n.seen_next=0;
    n.create_sequence=0; n.has_create_sequence=false;
    clear_bytes(n.seen,sizeof(n.seen));
}
void release_retention(ApplicationNotifications& n) noexcept {
    auto* rows=n.retention.rows.data; auto* index=n.retention.index.data;
    reset_value(&n.retention);
    if(rows) pages_release(rows);
    if(index) pages_release(index);
}
bool retention_unavailable(ApplicationNotifications& n) noexcept {
    n.status=utf8_literal("Notifications are unavailable right now.");
    log_write(LogLevel::warning,"notifications","retention_storage_unavailable");
    return false;
}
bool retention_room(ApplicationNotifications& n,u64 channel) noexcept {
    const auto room=notification_retention_room(&n.retention,channel);
    if(room.ok()) return true;
    if(room.error!=NotificationRetentionError::full_channels) return retention_unavailable(n);
    // Two replacement banks grow only when a new channel needs a slot. The
    // old banks remain valid until both allocations and rebind succeed.
    const u32 old_capacity=n.retention.rows.count;
    // Slice counts/index slots must remain representable; this is not a
    // policy channel cap. Ordinary allocation failure occurs independently.
    if(old_capacity>=(u32{1}<<30)) return retention_unavailable(n);
    const u32 capacity=old_capacity?old_capacity*2:16;
    const u32 slots=capacity*2;
    // VirtualAlloc commits whole pages, so the byte counts need no rounding here.
    const u64 row_bytes=u64{capacity}*sizeof(NotificationRetentionRow);
    const u64 index_bytes=u64{slots}*sizeof(u32);
#if defined(MAICORD_NOTIFICATION_FIXTURE)
    const bool allocate_rows=n.fixture_retention_failure_bank!=1;
    const bool allocate_index=n.fixture_retention_failure_bank!=2;
#else
    constexpr bool allocate_rows=true, allocate_index=true;
#endif
    auto* rows=allocate_rows?commit_pages<NotificationRetentionRow>(static_cast<SIZE_T>(row_bytes)):nullptr;
    auto* index=rows && allocate_index?commit_pages<u32>(static_cast<SIZE_T>(index_bytes)):nullptr;
    auto* old_rows=n.retention.rows.data; auto* old_index=n.retention.index.data;
    if(!rows || !index || notification_retention_rebind(&n.retention,{rows,capacity},{index,slots})!=NotificationRetentionError::none) {
        if(rows) pages_release(rows);
        if(index) pages_release(index);
        return retention_unavailable(n);
    }
    if(old_rows) pages_release(old_rows);
    if(old_index) pages_release(old_index);
    return true;
}
// Where the notification workers and the continuation post their wakes.
Waker wake_target(const Application& a) noexcept {
    return a.window?Waker{window_surface_native(a.window),wake_notifications}:Waker{};
}
void post_continuation(Application& a,ApplicationNotifications& n) noexcept {
    n.work_pending=true;
    if(a.window) window_post(wake_target(a),0,0);
}
void release_storage(ApplicationNotifications& n) noexcept {
    stable_storage_release(&n.guilds); stable_storage_release(&n.overrides);
    stable_storage_release(&n.threads); stable_storage_release(&n.staging);
    stable_storage_release(&n.guild_scopes); stable_storage_release(&n.channel_scopes);
}
ApplicationNotifications* ensure(Application* a) noexcept {
    if(!a) return nullptr;
    if(a->notifications) return a->notifications;
    auto* n = commit_pages<ApplicationNotifications>();
    if(!n) return nullptr;
    if(!stable_storage_initialize(&n->guilds,guild_capacity,0,sizeof(MessageNotificationGuildSetting),alignof(MessageNotificationGuildSetting)) ||
       !stable_storage_initialize(&n->overrides,channel_capacity,0,sizeof(MessageNotificationOverride),alignof(MessageNotificationOverride)) ||
       !stable_storage_initialize(&n->threads,channel_capacity,0,sizeof(MessageNotificationThreadSetting),alignof(MessageNotificationThreadSetting)) ||
       !stable_storage_initialize(&n->staging,channel_capacity,0,sizeof(MessageNotificationOverride),alignof(MessageNotificationOverride)) ||
       !stable_storage_initialize(&n->guild_scopes,guild_capacity,0,sizeof(Scope),alignof(Scope)) ||
       !stable_storage_initialize(&n->channel_scopes,channel_capacity,0,sizeof(Scope),alignof(Scope)) ||
       !message_notifications_initialize(&n->settings,
          {stable_storage_data<MessageNotificationGuildSetting>(n->guilds),guild_capacity},
          {stable_storage_data<MessageNotificationOverride>(n->overrides),channel_capacity},
          {stable_storage_data<MessageNotificationThreadSetting>(n->threads),channel_capacity},
          [](void* context,u32 g,u32 o,u32 t) noexcept {
              auto& owner=*static_cast<ApplicationNotifications*>(context);
              return (!g || stable_storage_reserve(&owner.guilds,g)) &&
                  (!o || stable_storage_reserve(&owner.overrides,o)) &&
                  (!t || stable_storage_reserve(&owner.threads,t));
          },n)) {
        release_storage(*n); pages_release(n); return nullptr;
    }
    if(!notification_retention_initialize(&n->retention,{},{})) {
        release_storage(*n); pages_release(n); return nullptr;
    }
    n->audio_epoch=1; n->audio_preference=a->preferences.message_sounds_enabled;
    n->current_pack=a->preferences.soundpack;
    n->sound_assets.module=module_image();
    // VirtualAlloc zeroes storage; it does not run default member initializers.
    notification_sound_assets_volume(&n->sound_assets,a->preferences.output_volume);
    const NotificationAudioConfig audio_config{notification_sound_asset_resolve,&n->sound_assets,
        a->window?Waker{window_surface_native(a->window),wake_notifications}:Waker{},1,n->audio_preference,notification_sound_asset_gain};
    if(!notification_audio_initialize(&n->audio,audio_config).ok()) {
        release_storage(*n); pages_release(n); return nullptr;
    }
    notification_sound_throttle_generation(&n->sound_throttle,n->audio_epoch);
    n->status=utf8_literal("Desktop notifications aren't set up yet. Select Set up to turn them on.");
    a->notifications=n;
    return n;
}
Scope* find_scope(StableStorage& storage,u32 count,u64 id) noexcept {
    auto* rows=stable_storage_data<Scope>(storage);
    for(u32 i=0;i<count;++i) if(rows[i].id==id) return rows+i;
    return nullptr;
}
bool scope_apply(StableStorage& storage,u32& count,u64 id,u64 guild,u32 flags,u32 known,u32 gates) noexcept {
    auto* row=find_scope(storage,count,id);
    if(!row) {
        if(!stable_storage_reserve(&storage,count+1)) return false;
        row=stable_storage_data<Scope>(storage)+count++; *row={id,guild,0,0,0};
    }
    if(flags&gateway_notification_guild_default_known)
        row->flags=(row->flags&~(gateway_notification_guild_default_known|gateway_notification_guild_default_mentions))|flags;
    else row->flags|=flags;
    row->gates=(row->gates&~known)|gates;
    row->known_gates|=known;
    return true;
}
void scope_remove(StableStorage& storage,u32& count,u64 id) noexcept {
    auto* rows=stable_storage_data<Scope>(storage);
    for(u32 i=0;i<count;++i) if(rows[i].id==id) { rows[i]=rows[--count]; return; }
}
void scope_remove_guild(StableStorage& storage,u32& count,u64 guild) noexcept {
    auto* rows=stable_storage_data<Scope>(storage);
    u32 kept{};
    for(u32 i=0;i<count;++i) if(rows[i].guild!=guild) rows[kept++]=rows[i];
    count=kept;
}
void forget_age_gate(Scope& scope) noexcept {
    scope.known_gates&=~notification_gate_age_access;
    scope.flags&=~gateway_notification_channel_age_baseline_complete;
}
void scope_admission_failed(ApplicationNotifications& n,u64 guild) noexcept {
    // Direct message metadata lives in its channel scope; the user's DM
    // settings stay known.
    if(!guild) return;
    message_notifications_mark_incomplete(&n.settings,guild);
    if(auto* scope=find_scope(n.guild_scopes,n.guild_scope_count,guild)) forget_age_gate(*scope);
}
void transport_error(ApplicationNotifications& n,DesktopNotificationResult result) noexcept {
    if(result.ok()) return;
    n.status=MAICORD_OS_TEXT("Windows could not deliver desktop notifications. Check notification settings.",
                             "Your desktop could not deliver notifications. Check notification settings.");
    log_u64(LogLevel::warning,"notifications","native_error",result.native_error);
}
void retention_close(ApplicationNotifications& n,DesktopNotificationActivation target) noexcept {
    // Windows closes a stable profile/message tag. Source permits duplicate
    // opaque request IDs; its private IPC does not prove a unique close nonce.
#if defined(MAICORD_NOTIFICATION_FIXTURE)
    if(n.fixture_retention_remove) {
        transport_error(n,n.fixture_retention_remove(n.fixture_retention_context,target)); return;
    }
#endif
    if(n.enrolled) transport_error(n,desktop_notifications_remove_message(&n.native,target));
}
Utf8View entity(void* context,MessageMarkdownEntityKind kind,u64 id) noexcept {
    auto& a=*static_cast<Application*>(context);
    if(!a.account_ui) return {};
    auto& ui=*a.account_ui;
    if(kind==MessageMarkdownEntityKind::user) {
        const auto* user=user_store_find(ui.user_store,id);
        return user?user_store_name(ui.user_store,*user):Utf8View{};
    }
    if(kind==MessageMarkdownEntityKind::channel) {
        const auto* channel=chat_state_store_find_channel(ui.chat_state,id);
        return channel?chat_state_store_channel_name(ui.chat_state,*channel):Utf8View{};
    }
    if(kind==MessageMarkdownEntityKind::game&&a.renderer.game_store) return game_store_name(*a.renderer.game_store,id);
    return {};
}
bool contains_nul(Utf8View text) noexcept { for(u32 i=0;i<text.bytes;++i) if(!text.data[i]) return true; return false; }
Utf8View preview(Application& a,Utf8View source,char* output,u32 capacity) noexcept {
    if(contains_nul(source)) return {};
    if(!source.bytes) return utf8_literal("New message");
    // The initial text is limited to a short message. Oversized or malformed
    // previews use a placeholder label, never a raw or truncated spoiler body.
    if(source.bytes>768) return utf8_literal("New message");
    char text[1024]; u32 styles[1024]; TextStyleSpan spans[64];
    MessageMarkdownOptions options{}; options.context=&a; options.resolve_entity=entity;
    const auto parsed=message_markdown_parse(source,{text,1024},{styles,1024},{spans,64},options,{});
    if(!parsed.ok()) return utf8_literal("New message");
    u32 used{}; bool hidden=false;
    static constexpr char redacted[]="[Spoiler]";
    for(u32 i=0;i<parsed.text.bytes;++i) {
        if(styles[i]&text_style_spoiler) {
            if(!hidden) { if(used+sizeof(redacted)-1>capacity) return utf8_literal("New message"); copy_bytes(output+used,redacted,sizeof(redacted)-1); used+=sizeof(redacted)-1; }
            hidden=true; continue;
        }
        hidden=false;
        if(used==capacity) return utf8_literal("New message");
        output[used++]=text[i];
    }
    return {output,used};
}
// Discord's text-to-speech messages: a /tts message in the channel being read while "Allow playback and usage of
// /tts command" is on, and with "Speak all messages out loud" others' messages there or everywhere; never while
// deafened, from a blocked or ignored user, or in a muted channel. Reads "{name} said {text}" (a reply:
// "{name} replied to {name} {text}") as the official client does: mentions, channels and emoji as their names, each
// spoiler as the word "Spoiler", a link as its domain, and the line cut to 200 characters at the last space.
constexpr u32 kSpeechCharacters = 200;
// A link's spoken form: its host without "www.", empty when it has none.
Utf8View link_domain(Utf8View text,u32 at,u32* end) noexcept {
    u32 i=at;
    while(i<text.bytes&&text.data[i]!=' '&&text.data[i]!='\n'&&text.data[i]!='\t') ++i;
    *end=i;
    u32 host=at+(text.data[at+4]=='s'?8u:7u);
    if(host+4<=i&&bytes_equal(text.data+host,"www.",4)) host+=4;
    u32 stop=host;
    while(stop<i&&text.data[stop]!='/'&&text.data[stop]!='?'&&text.data[stop]!='#') ++stop;
    return {text.data+host,stop-host};
}
bool link_at(Utf8View text,u32 at) noexcept {
    const u32 left=text.bytes-at;
    return (at==0||text.data[at-1]==' '||text.data[at-1]=='\n')&&
           ((left>7&&bytes_equal(text.data+at,"http://",7))||(left>8&&bytes_equal(text.data+at,"https://",8)));
}
void speak(Application& a,const ApplicationNotifications& n,const GatewayMessageEvent& message,const ChannelRecord& channel,
           Utf8View author,u32 relationship_gates) noexcept {
    const auto& state=a.account_ui->chat_state;
    const bool selected=channel.id==state.selected_channel_id;
    const u8 everything=a.preferences.speak_messages;
    const bool command=message.tts&&selected&&application_user_settings_tts_command();
    const bool spoken=message.author_id!=state.self_user_id&&(everything==client_speak_messages_all||
                                                               (everything==client_speak_messages_selected&&selected));
    if((!command&&!spoken)||relationship_gates||!message.content.bytes||contains_nul(message.content)||
       application_voice_view().self_deaf||
       message_notifications_override_muted(n.settings,channel.guild_id,channel.id,clock_unix_milliseconds()))
        return;
    // Markdown reduced to its plain text, entities named; the line is cut to 200 characters anyway, so the source is
    // bounded to its first 768 bytes.
    Utf8View source=message.content;
    if(source.bytes>768) source.bytes=utf8_truncated_length(source,768);
    char text[1024]; u32 styles[1024]; TextStyleSpan spans[64];
    MessageMarkdownOptions options{}; options.context=&a; options.resolve_entity=entity;
    const auto parsed=message_markdown_parse(source,{text,1024},{styles,1024},{spans,64},options,{});
    const Utf8View body=parsed.ok()?parsed.text:source;
    const Utf8View replied=message.type==19?message.reference.author_name:Utf8View{};
    char line[1536];
    TextWriter writer{{line,sizeof(line)}};
    writer.add(author.bytes?author:utf8_literal("Someone"));
    if(replied.bytes) { writer.add(utf8_literal(" replied to ")); writer.add(replied); writer.add(utf8_literal(" ")); }
    else writer.add(utf8_literal(" said "));
    for(u32 i=0;i<body.bytes;) {
        if(parsed.ok()&&(styles[i]&text_style_spoiler)) {
            writer.add(utf8_literal("Spoiler"));
            while(i<body.bytes&&(styles[i]&text_style_spoiler)) ++i;
            continue;
        }
        if(link_at(body,i)) {
            u32 end=i;
            writer.add(link_domain(body,i,&end));
            i=end;
            continue;
        }
        writer.add({body.data+i,1});
        ++i;
    }
    if(!writer.ok) return;
    // Over 200 characters: the first 200, then back to the last space among them.
    u32 bytes=writer.used,characters=0,cut=0;
    for(u32 i=0;i<bytes;++i) {
        if((static_cast<u8>(line[i])&0xC0)==0x80) continue;
        if(characters++==kSpeechCharacters) { cut=i; break; }
    }
    if(cut) {
        u32 space=cut;
        while(space>0&&line[space-1]!=' ') --space;
        bytes=space>1?space-1:cut;
    }
    if(!application_user_actions_speak({line,bytes}))
        log_write(LogLevel::warning,"notifications","speech_unavailable");
}
// Mentioned role ids of a message and those of them the user holds; false when the payload lacks one.
bool collect_mention_roles(const ChatStateStore& state,u64 guild_id,const GatewayClientEvent& event,const ConstSlice<u8>& payload,u64* mentioned,u64* held,u32* held_count) noexcept {
    for(u32 i=0;i<event.message.mention_role_count;++i) {
        if(!gateway_client_event_message_mention_role_at(event,payload,i,mentioned+i)) return false;
        if(chat_state_store_self_has_role(state,guild_id,mentioned[i])) held[(*held_count)++]=mentioned[i];
    }
    return true;
}
u64 category_of(const ChannelRecord* parent) noexcept { return parent?(parent->type==4?parent->id:parent->parent_id):0; }
void mention_flags(MessageNotificationPolicyInput& input,const GatewayClientEvent& event) noexcept {
    input.direct_mention=(event.message.mention_flags&gateway_client_message_mention_current_user)!=0;
    input.everyone_mention=(event.message.mention_flags&gateway_client_message_mention_everyone)!=0;
}
ApplicationNotificationAvatarFunction g_notification_avatar{};
} // namespace
void application_notifications_set_avatar_source(ApplicationNotificationAvatarFunction source) noexcept {
    g_notification_avatar = source;
}
bool application_notifications_initialize(Application* a) noexcept {
    if(!a) return false;
    if(!notification_identity_set_process({app_id,sizeof(app_id)/sizeof(wchar_t)-1}).ok()) return false;
    // A path-sized identity is transient and does not enlarge Application.
    auto* identity=commit_pages<Identity>();
    if(!identity) return false;
    const bool paths=identity_paths(*identity);
    auto checked=paths?notification_identity_check(identity->config):NotificationIdentityResult{NotificationIdentityError::invalid_input};
    // The user already enrolled: an owned identity whose executable path or
    // icon drifted is repaired here. Absent or foreign identities wait for setup.
    if(checked.error==NotificationIdentityError::stale) {
        checked=enroll(*identity);
        if(!checked.ok()) log_u64(LogLevel::warning,"notifications","identity_repair_error",checked.native_error);
    }
    pages_release(identity);
    if(!checked.ok()) return true;
    auto* n=ensure(a); if(!n) return false;
    const auto result=desktop_notifications_initialize(&n->native,{{app_id,sizeof(app_id)/sizeof(wchar_t)-1},activator_id,wake_target(*a),true});
    n->enrolled=result.ok(); transport_error(*n,result);
    return result.ok();
}
void application_notifications_attach_window(Application* a) noexcept {
    if(!a || !a->notifications) return;
    auto& n=*a->notifications;
    if(n.native.runtime) desktop_notifications_set_wake(&n.native,wake_target(*a));
    // Startup attaches HWND before the first account request launches audio.
    if(!n.audio.runtime) {
        n.audio.config.wake=wake_target(*a);
    }
}
void application_notifications_settings_refresh(Application* a) noexcept {
    auto* n=ensure(a); if(!n) return;
    audio_preferences(*a,*n); audio_observe(*n);
    if(!n->enrolled) return;
    const auto result=desktop_notifications_setting(&n->native,&n->os_setting);
    transport_error(*n,result);
    if(!result.ok()) return;
    // Windows.UI.Notifications.NotificationSetting.
    switch(n->os_setting) {
    case 0: n->status=utf8_literal("Notifications are on."); break;
    case 1: n->status=MAICORD_OS_TEXT("MaiCord's notifications are turned off in Windows. Turn them on in Windows notification settings.",
                                      "No notification service is running on your desktop. Start one to get notifications."); break;
    case 2: n->status=MAICORD_OS_TEXT("All notifications are turned off in Windows. Turn them on in Windows notification settings.",
                                      "Notifications are turned off on your desktop."); break;
    case 3: n->status=MAICORD_OS_TEXT("Your organization has turned off notifications on this PC.",
                                      "Your organization has turned off notifications on this computer."); break;
    default: n->status=MAICORD_OS_TEXT("Windows is blocking notifications from MaiCord.",
                                       "Your desktop is blocking notifications from MaiCord."); break;
    }
}
bool application_notifications_setup(Application* a) noexcept {
    auto* n=ensure(a); if(!n) return false;
    // Enrollment runs only from the explicit Notifications setup action. Startup only repairs an identity the
    // user already enrolled. The Set up button doubles as the retry.
    if(n->enrolled) { application_notifications_settings_refresh(a); return true; }
    auto* identity=commit_pages<Identity>();
    if(!identity) { n->status=utf8_literal("Couldn't set up notifications. Try again."); return false; }
    auto checked=identity_paths(*identity)?notification_identity_check(identity->config):
        NotificationIdentityResult{NotificationIdentityError::invalid_input,ERROR_INVALID_PARAMETER};
    // Creates an absent identity and repairs an owned one; never claims a foreign key.
    if(checked.error==NotificationIdentityError::absent || checked.error==NotificationIdentityError::stale ||
       (checked.ok() && !identity->config.icon_path.count)) checked=enroll(*identity);
    pages_release(identity);
    if(!checked.ok()) {
        switch(checked.error) {
        case NotificationIdentityError::invalid_input:
            n->status=utf8_literal("Couldn't set up notifications: MaiCord's install location isn't supported. Move MaiCord to a local folder and try again."); break;
        case NotificationIdentityError::foreign_resource:
            n->status=utf8_literal("Couldn't set up notifications: another program owns MaiCord's notification registration."); break;
        case NotificationIdentityError::rollback:
            n->status=utf8_literal("Notification setup didn't finish. Try again."); break;
        default:
            n->status=MAICORD_OS_TEXT("Windows didn't accept MaiCord's notification registration. Try again.",
                                      "Your desktop didn't accept MaiCord's notification registration. Try again."); break;
        }
        LogEvent("identity_failed").field("error",static_cast<u64>(checked.error)).error("native_error",static_cast<u32>(checked.native_error))
            .field("registry_created",static_cast<u64>(checked.registry_created)).write(LogLevel::warning,"notifications");
        return false;
    }
    log_u64(LogLevel::info,"notifications","identity_ready",checked.registry_created);
    const auto result=desktop_notifications_initialize(&n->native,{{app_id,sizeof(app_id)/sizeof(wchar_t)-1},activator_id,wake_target(*a),true});
    n->enrolled=result.ok(); transport_error(*n,result);
    if(result.ok()) application_notifications_settings_refresh(a);
    return result.ok();
}
Utf8View application_notifications_status(const Application* a) noexcept {
    return a && a->notifications?a->notifications->status:utf8_literal("Desktop notifications aren't set up yet. Select Set up to turn them on.");
}
const MessageNotificationStore* application_notifications_store(const Application* a) noexcept {
    return a && a->notifications?&a->notifications->settings:nullptr;
}
bool application_notifications_project(Application* a,u64 guild,u64 channel,const MessageNotificationSetting& value) noexcept {
    auto* n=ensure(a); if(!n || (!guild && !channel)) return false;
    auto& store=n->settings;
    u32 count=0;
    for(u32 i=0;i<store.override_count;++i) count+=store.overrides[i].guild_id==guild;
    if(!stable_storage_reserve(&n->staging,count+1)) return false;
    auto* rows=stable_storage_data<MessageNotificationOverride>(n->staging);
    u32 used=0; bool replaced=false;
    for(u32 i=0;i<store.override_count;++i) {
        if(store.overrides[i].guild_id!=guild) continue;
        rows[used]=store.overrides[i];
        if(channel && rows[used].channel_id==channel) { rows[used].setting=value; replaced=true; }
        ++used;
    }
    if(channel && !replaced) rows[used++]={guild,channel,value};
    const auto* current=message_notifications_guild(store,guild);
    MessageNotificationGuildSetting entry{guild,current?current->setting:MessageNotificationSetting{}};
    if(!channel) entry.setting=value;
    if(message_notifications_apply_settings(&store,{&entry,1},{rows,used},true,store.version)!=MessageNotificationApplyError::none)
        return false;
    purge_display(*n);
    return true;
}
namespace {
void acknowledge(Application* a,u64 channel,bool defer_removal) noexcept {
    if(!a || !a->notifications || !channel) return;
    auto& n=*a->notifications;
    notification_retention_discard_channel(&n.retention,channel);
    if(n.display) {
        desktop_notification_queue_purge_channel(&n.display->queue,n.profile,n.generation,channel);
        if(n.backpressured) post_continuation(*a,n);
    }
    if(!n.enrolled || !n.profile) return;
    if(defer_removal && n.deferred_removal_count<4) {
        for(u32 i=0;i<n.deferred_removal_count;++i) if(n.deferred_removals[i]==channel) return;
        n.deferred_removals[n.deferred_removal_count++]=channel;
        post_continuation(*a,n);
        return;
    }
    transport_error(n,desktop_notifications_remove_channel(&n.native,n.profile,channel));
}
}
void application_notifications_ack(Application* a,u64 channel) noexcept {
    acknowledge(a,channel,false);
}
bool application_notifications_can_pop(Application* a) noexcept {
    if(!a || !a->notifications) return true;
    auto& n=*a->notifications;
    audio_preferences(*a,n); audio_observe(n);
    if(!n.ready || !n.live || n.snapshot || n.snapshot_failed || n.profile!=a->account.status.profile_id) {
        n.backpressured=n.audio_backpressured=false; return true;
    }
    // Reserve the worst case selected message3 plus ordinary message1 BEFORE
    // consuming Gateway input. Worker completion posts a wake; never poll-full.
    n.audio_backpressured=n.audio_preference && audio_sink(n) && !audio_capacity(n,2);
    if(!popup_sink(*a,n)) {
        purge_display(n); n.backpressured=false; return !n.audio_backpressured;
    }
    auto* display=ensure_display(n);
    if(!display) {
        n.backpressured=false;
        n.status=utf8_literal("Notifications are unavailable right now.");
        log_write(LogLevel::warning,"notifications","display_storage_unavailable");
        return !n.audio_backpressured;
    }
    const bool available=desktop_notification_queue_can_admit(&display->queue,projected_maximum_bytes);
    n.backpressured=!available;
    if(!available && !n.audio_backpressured) post_continuation(*a,n);
    return available && !n.audio_backpressured;
}
bool application_notifications_gateway_pending(const Application* a) noexcept {
    if(!a || !a->notifications) return false;
    const auto& n=*a->notifications;
    if(n.audio_backpressured)
        return !n.audio_preference || !audio_sink(n) || audio_capacity(n,2);
    return n.backpressured;
}
u64 application_notifications_next_wake(const Application* a) noexcept {
    if(!a || !a->notifications) return 0;
    const auto& n=*a->notifications;
    if(!n.ready || !n.live || n.snapshot || n.snapshot_failed || n.profile!=a->account.status.profile_id ||
        !n.audio_preference || !audio_capacity(n,1)) return 0;
    u64 deadline{};
    return notification_sound_throttle_deadline(n.sound_throttle,&deadline)?deadline:0;
}
void application_notifications_selection(Application* a) noexcept {
    if(!a || !a->notifications || !a->account_ui) return;
    auto& n=*a->notifications;
    const u64 selected=a->account_ui->chat_state.selected_channel_id;
    const bool window_focused=focused(n);
    if(selected!=n.selected || (window_focused && !n.focused)) {
        // Runs inside focus messages; the toast withdrawal waits for the drain.
        if(selected) acknowledge(a,selected,true);
        n.selected=selected;
    }
    n.focused=window_focused;
}
static bool row_audio_ready(const ApplicationNotifications& n,const DesktopNotificationQueueRow& row) noexcept {
    return !row.sound_id || row.sound_epoch!=n.audio_epoch || !n.audio_preference || !audio_sink(n) || audio_capacity(n,1);
}
void application_notifications_drain(Application* a) noexcept {
    if(!a || !a->notifications) return;
    auto& n=*a->notifications;
    audio_preferences(*a,n); audio_observe(n);
    const bool audio_can_resume=n.audio_backpressured &&
        (!n.audio_preference || !audio_sink(n) || audio_capacity(n,2));
    if(audio_can_resume) n.work_pending=true;
    const u64 now=sound_clock(n);
    u64 deadline{};
    if(n.ready && n.live && !n.snapshot && !n.snapshot_failed && n.audio_preference && audio_capacity(n,1) &&
        notification_sound_throttle_deadline(n.sound_throttle,&deadline) && now>=deadline) {
        const auto invocation=notification_sound_throttle_timer(&n.sound_throttle,now,n.audio_epoch);
        if(invocation.invoke) sound_submit(n,invocation.request.kind==NotificationSoundKind::message3?2:1,invocation.request.generation);
    }
    const bool display_pending=n.display && desktop_notification_queue_pending(&n.display->queue);
    if(!n.work_pending && !n.pending.message_id && !display_pending && !desktop_notifications_history_pending(&n.native)) return;
    // Only now the queue's front row: an idle turn never copies it.
    DesktopNotificationQueueRow front{};
    const bool has_front=n.display && desktop_notification_queue_front(&n.display->queue,&front);
    const bool display_audio_ready=!has_front || row_audio_ready(n,front);
    n.work_pending=false;
    for(u32 i=0;i<n.deferred_removal_count;++i)
        if(n.enrolled && n.profile) transport_error(n,desktop_notifications_remove_channel(&n.native,n.profile,n.deferred_removals[i]));
    n.deferred_removal_count=0;
    DesktopNotificationActivation request{};
    u32 activations=0;
    while(activations<kDesktopNotificationActivationCapacity && desktop_notifications_take_activation(&n.native,&request)) {
        ++activations;
        if(a->account.status.profile_id && request.profile_id!=a->account.status.profile_id) continue;
        if(n.ready && a->account_ui &&
            !viewable(a->account_ui->chat_state,chat_state_store_find_channel(a->account_ui->chat_state,request.channel_id)))
            continue;
        n.pending=request;
    }
    if(activations==kDesktopNotificationActivationCapacity) post_continuation(*a,n);
    const bool history_pending=desktop_notifications_history_pending(&n.native);
    // Alternate display and history continuations while both have work. Each
    // dispatch borrows stable copied text and makes at most one actual Show.
    if(display_pending && display_audio_ready && (!history_pending || !n.history_turn)) {
        DesktopNotificationQueueDispatch dispatch{};
        if(desktop_notification_queue_begin(&n.display->queue,&dispatch).ok()) {
            auto outcome=DesktopNotificationQueueOutcome::failed;
            const auto& id=dispatch.identity;
            const auto* channel=a->account_ui?chat_state_store_find_channel(a->account_ui->chat_state,id.channel_id):nullptr;
            const bool window_focused=focused(n);
            const auto* policy_channel=a->account_ui?chat_state_store_find_channel(a->account_ui->chat_state,
                id.policy_channel_id?id.policy_channel_id:id.channel_id):nullptr;
            const bool selected=policy_channel && policy_channel->id==a->account_ui->chat_state.selected_channel_id;
            const bool valid=id.profile_id==n.profile && id.profile_id==a->account.status.profile_id && id.generation==n.generation &&
                popup_sink(*a,n) && n.ready && n.live && !n.snapshot && !n.snapshot_failed && !a->login.visible &&
                a->account.status.required_action==AccountRequirement::none &&
                channel && chat_state_store_channel_visible(a->account_ui->chat_state,*channel) &&
                (channel->actions&channel_action_view) && policy_channel &&
                chat_state_store_channel_visible(a->account_ui->chat_state,*policy_channel) &&
                (policy_channel->actions&channel_action_view) && !(window_focused && selected);
            // Failure to reserve metadata cancels this queued intent and
            // follows the normal failed-Show sound fallback, before any Show.
            if(valid && retention_room(n,id.channel_id)) {
                // The official client flashes as it shows the popup; focus stops it.
                if(a->preferences.taskbar_flash) application_taskbar_attention_notify();
                const DesktopNotificationPayload projected{{id.profile_id,id.channel_id,id.message_id},
                    dispatch.title,dispatch.body,dispatch.avatar_file_uri,id.silent,id.suppress_popup};
#if defined(MAICORD_NOTIFICATION_FIXTURE)
                if(n.fixture_sink) {
                    n.fixture_sink(n.fixture_context,projected);
                    if(n.fixture_show_state==DesktopNotificationShowState::submitted) outcome=DesktopNotificationQueueOutcome::submitted;
                    else if(n.fixture_show_state==DesktopNotificationShowState::retry_required && dispatch.attempt<2)
                        outcome=DesktopNotificationQueueOutcome::retry;
                }
                else
#endif
                {
                    const auto shown=desktop_notifications_show_once(&n.native,projected);
                    if(shown.state==DesktopNotificationShowState::submitted) outcome=DesktopNotificationQueueOutcome::submitted;
                    else if(shown.state==DesktopNotificationShowState::retry_required && dispatch.attempt<2)
                        outcome=DesktopNotificationQueueOutcome::retry;
                    else transport_error(n,shown.result);
                }
            }
            const auto completed=desktop_notification_queue_complete(&n.display->queue,id.token,dispatch.attempt,outcome);
            if(completed.error==DesktopNotificationQueueError::none && outcome==DesktopNotificationQueueOutcome::submitted &&
                !completed.canceled) {
                // Source's async callback has no epoch guard. Keep native
                // privacy/cancellation barriers if Show pumps READY/account
                // events; the activation channel decides retention.
                if(id.profile_id==n.profile && id.profile_id==a->account.status.profile_id && id.generation==n.generation) {
                    const auto retained=notification_retention_append(&n.retention,id.channel_id,id.message_id);
                    if(retained.ok()) {
                        if(retained.evicted) retention_close(n,{id.profile_id,id.channel_id,retained.evicted_ticket});
                    } else {
                        // Preflight guarantees a slot without a borrowed view
                        // across Show. An invariant failure still cleans up an
                        // untracked native tag rather than keeping it hidden.
                        log_write(LogLevel::warning,"notifications","retention_append_failed");
                        retention_close(n,{id.profile_id,id.channel_id,id.message_id});
                    }
                } else retention_close(n,{id.profile_id,id.channel_id,id.message_id});
            }
            if(completed.error==DesktopNotificationQueueError::none && valid &&
                outcome==DesktopNotificationQueueOutcome::failed && !completed.canceled && id.sound_id)
                sound_throttled(n,id.sound_id,id.sound_epoch);
            if(completed.removal_required) {
                // Existing canceled-Show cleanup removes the whole Windows
                // channel, including earlier retained successes. Cancel its
                // metadata too, but never erase a new profile's channel.
                if(id.profile_id==n.profile) notification_retention_discard_channel(&n.retention,id.channel_id);
                if(n.enrolled) transport_error(n,desktop_notifications_remove_channel(&n.native,id.profile_id,id.channel_id));
            }
        }
        n.history_turn=true;
    } else if(history_pending && n.enrolled) {
        bool pending_history=false;
        transport_error(n,desktop_notifications_history_step(&n.native,&pending_history));
        n.history_turn=false;
    }
    DesktopNotificationQueueRow next{};
    const bool next_pending=n.display && desktop_notification_queue_pending(&n.display->queue);
    const bool next_front=n.display && desktop_notification_queue_front(&n.display->queue,&next);
    const bool display_can_progress=next_pending && (!next_front || row_audio_ready(n,next));
    if(display_can_progress || desktop_notifications_history_pending(&n.native) || audio_can_resume ||
        (n.backpressured && !n.audio_backpressured)) post_continuation(*a,n);
    if(!n.pending.message_id || !n.ready || !a->account_ui || a->login.visible) return;
    request=n.pending; reset_value(&n.pending);
    if(request.profile_id!=a->account.status.profile_id) return;
    const auto& state=a->account_ui->chat_state;
    if(!viewable(state,chat_state_store_find_channel(state,request.channel_id))) {
        log_write(LogLevel::info,"notifications","activation_channel_unavailable"); return;
    }
    // Reveal restores only a minimized or hidden window, so a maximized one stays maximized.
    if(a->window) desktop_window_reveal(a->window);
    if(!application_navigate_known_message(request.channel_id,request.message_id))
        log_write(LogLevel::warning,"notifications","activation_navigation_failed");
}
void application_notifications_wake(Application* a) noexcept {
    if(a && a->notifications) a->notifications->work_pending=true;
}
void application_notifications_account_stop(Application* a) noexcept {
    if(!a || !a->notifications) return;
    auto& n=*a->notifications;
    audio_reset(n,false);
    if(n.enrolled && n.profile) transport_error(n,desktop_notifications_remove_profile(&n.native,n.profile));
    n.ready=n.live=false; n.snapshot=false; n.profile=0; reset_value(&n.pending); reset_value(&n.global); n.mention_on_all=false;
    release_retention(n);
    if(n.display) desktop_notification_queue_reset(&n.display->queue,0,0);
    reset_ingress(n);
    n.selected=0; n.focused=false; n.deferred_removal_count=0;
    message_notifications_clear(&n.settings);
    desktop_notifications_discard_activations(&n.native);
}
void application_notifications_release(Application* a) noexcept {
    if(!a || !a->notifications) return;
    auto& n=*a->notifications;
    const auto joined=notification_audio_release(&n.audio,2000);
    if(!joined.ok()) {
        // Keep the worker's borrowed bank/context alive on a timed-out join.
        // A later release can retry; process termination remains the last bound.
        audio_error(n,joined); log_write(LogLevel::warning,"notifications","audio_join_retains_owner"); return;
    }
    notification_sound_assets_release(&n.sound_assets);
    desktop_notifications_release(&n.native); release_storage(n); release_retention(n);
    if(n.display) pages_release(n.display);
    pages_release(a->notifications); a->notifications=nullptr;
}
void application_notifications_prepare_shutdown(Application* a) noexcept {
    // Normal process shutdown retains OS history for later cold activation.
    // Explicit account invalidation still removes that profile's history.
    if(a && a->notifications) {
        audio_reset(*a->notifications,false);
        a->notifications->backpressured=a->notifications->audio_backpressured=false;
        a->notifications->profile=0;
    }
}
#if defined(MAICORD_NOTIFICATION_FIXTURE)
bool application_notifications_fixture_sink(Application* a,void(*sink)(void*,const DesktopNotificationPayload&) noexcept,void* context) noexcept {
    auto* n=ensure(a); if(!n) return false;
    n->fixture_sink=sink; n->fixture_context=context; return true;
}
bool application_notifications_fixture_audio(Application* a,
    NotificationAudioResult(*sink)(void*,const NotificationAudioRequest&) noexcept,
    NotificationAudioStatus(*status)(void*) noexcept,u64(*clock)(void*) noexcept,void* context) noexcept {
    if((sink&&!status)||(!sink&&status)) return false;
    auto* n=ensure(a); if(!n || n->audio.runtime) return false;
    n->fixture_audio_sink=sink; n->fixture_audio_status=status;
    n->fixture_clock=clock; n->fixture_audio_context=context; return true;
}
void application_notifications_fixture_focus(Application* a,bool value) noexcept {
    if(auto* n=ensure(a)) {n->fixture_focus_known=true;n->fixture_focused=value;}
}
void application_notifications_fixture_show_state(Application* a,DesktopNotificationShowState state) noexcept {
    if(auto* n=ensure(a)) n->fixture_show_state=state;
}
bool application_notifications_fixture_retention_remove(Application* a,
    DesktopNotificationResult(*remove)(void*,DesktopNotificationActivation) noexcept,void* context) noexcept {
    auto* n=ensure(a); if(!n) return false;
    n->fixture_retention_remove=remove; n->fixture_retention_context=context; return true;
}
void application_notifications_fixture_retention_oom(Application* a,u32 bank) noexcept {
    if(auto* n=ensure(a)) n->fixture_retention_failure_bank=bank<=2?bank:1;
}
void application_notifications_fixture_suppress_popup(Application* a,bool value) noexcept {
    if(auto* n=ensure(a)) n->fixture_suppress_popup=value;
}
u64 application_notifications_fixture_retained(Application* a,u64 channel,u32 ordinal) noexcept {
    u64 ticket{};
    if(a && a->notifications) notification_retention_ticket(&a->notifications->retention,channel,ordinal,&ticket);
    return ticket;
}
ApplicationNotificationFixtureMetrics application_notifications_fixture_metrics(Application* a) noexcept {
    if(!a || !a->notifications) return {};
    const auto& n=*a->notifications;
    ApplicationNotificationFixtureMetrics metrics{};
    metrics.guilds=n.settings.guild_count; metrics.overrides=n.settings.override_count;
    metrics.threads=n.settings.thread_count; metrics.ready=n.ready; metrics.live=n.live;
    metrics.seen_count=n.seen_next;
    metrics.audio_epoch=n.audio_epoch; metrics.audio_backend_epoch=n.audio.epoch;
    notification_sound_throttle_deadline(n.sound_throttle,&metrics.sound_deadline);
    metrics.audio_failed=n.audio_failed;
    metrics.audio_worker_started=n.audio.runtime!=nullptr;
    metrics.audio_output_percent=static_cast<u32>(ReadAcquire(&n.sound_assets.output_percent));
    metrics.audio_pack=n.current_pack;
    metrics.retention_channels=n.retention.count;
    metrics.retention_capacity_channels=n.retention.rows.count;
    for(u32 i=0;i<n.retention.count;++i) saturating_accumulate(metrics.retention_tickets,u64{n.retention.rows[i].count});
    if(n.display) metrics.display_outstanding=desktop_notification_queue_metrics(&n.display->queue).outstanding;
    return metrics;
}
#endif
static bool invalidates_previews(GatewayClientDispatchKind kind) noexcept {
    switch(kind) {
    case GatewayClientDispatchKind::notification_snapshot_begin:
    case GatewayClientDispatchKind::notification_settings_entry:
    case GatewayClientDispatchKind::notification_snapshot_complete:
    case GatewayClientDispatchKind::notification_global:
    case GatewayClientDispatchKind::notification_incomplete:
    case GatewayClientDispatchKind::relationship_add:
    case GatewayClientDispatchKind::relationship_update:
    case GatewayClientDispatchKind::relationship_remove:
    case GatewayClientDispatchKind::relationship_snapshot_begin:
    case GatewayClientDispatchKind::relationship_state_incomplete:
    case GatewayClientDispatchKind::account_required_action:
    case GatewayClientDispatchKind::guild_role_create:
    case GatewayClientDispatchKind::guild_role_update:
    case GatewayClientDispatchKind::guild_role_delete:
    case GatewayClientDispatchKind::guild_member_update:
    case GatewayClientDispatchKind::private_state_incomplete:
    case GatewayClientDispatchKind::channel_recipient_remove:
    case GatewayClientDispatchKind::guild_update:
    case GatewayClientDispatchKind::guild_delete:
    case GatewayClientDispatchKind::channel_update:
    case GatewayClientDispatchKind::thread_self_update:
    case GatewayClientDispatchKind::thread_list_sync:
    case GatewayClientDispatchKind::thread_members_update:
        return true;
    default:
        return false;
    }
}
// The trailing sidecar of a settings event (scopes, age gates, thread rows): rare, so out of line, which keeps the
// per-event function above it free of this frame.
__declspec(noinline) static void apply_sidecar(ApplicationNotifications* n,const GatewayClientEvent& event,ConstSlice<u8> payload) noexcept {
    const auto kind=event.dispatch_kind;
    GatewayClientEvent normal{}; ConstSlice<u8> normal_payload{};
    if(!gateway_client_notification_trim(event,payload,&normal,&normal_payload)) {
        message_notifications_mark_incomplete(&n->settings,0); return;
    }
    if(normal_payload.count==payload.count) return;
    GatewayClientNotificationSidecar side{}; ConstSlice<u8> rows{};
    if(!gateway_client_notification_sidecar({payload.data+normal_payload.count,payload.count-normal_payload.count},&side,&rows)) return;
    if(side.flags&gateway_notification_incomplete) {
        scope_admission_failed(*n,side.guild_id);
        if(auto* scope=find_scope(n->channel_scopes,n->channel_scope_count,side.channel_id))
            scope->known_gates&=~notification_gate_age_access;
    }
    const bool guild=kind==GatewayClientDispatchKind::guild_create || kind==GatewayClientDispatchKind::guild_update;
    if(kind==GatewayClientDispatchKind::user_snapshot && (side.flags&gateway_notification_incomplete))
        n->global_known_gates&=~notification_gate_restricted_hours;
    if(guild) {
        if((side.flags&gateway_notification_channel_age_baseline_complete) && !(side.flags&gateway_notification_incomplete))
            for(u32 i=0;i<n->channel_scope_count;++i) {
                auto& scope=stable_storage_data<Scope>(n->channel_scopes)[i];
                if(scope.guild==side.guild_id) { scope.known_gates&=~notification_gate_age_access; scope.gates&=~notification_gate_age_access; }
            }
        if(!scope_apply(n->guild_scopes,n->guild_scope_count,side.guild_id,side.guild_id,side.flags,side.known_gates,side.gates))
            message_notifications_mark_incomplete(&n->settings,side.guild_id);
    }
    else if(side.channel_id) {
        if(!scope_apply(n->channel_scopes,n->channel_scope_count,side.channel_id,side.guild_id,side.flags,side.known_gates,side.gates))
            scope_admission_failed(*n,side.guild_id);
    }
    else {
        n->global_gates=(n->global_gates&~side.known_gates)|side.gates;
        n->global_known_gates|=side.known_gates;
    }
    for(u32 i=0;i<side.thread_count;++i) {
        GatewayClientNotificationThreadRecord row{};
        if(!gateway_client_notification_thread_at(rows,i,&row)) { message_notifications_mark_incomplete(&n->settings,side.guild_id); return; }
        if(row.flags&gateway_notification_channel_metadata) {
            if(!scope_apply(n->channel_scopes,n->channel_scope_count,row.thread.thread_id,row.thread.guild_id,0,row.known_gates,row.gates))
                scope_admission_failed(*n,row.thread.guild_id);
        } else if(row.flags&gateway_notification_thread_remove) message_notifications_remove_thread(&n->settings,row.thread.thread_id);
        else if(message_notifications_upsert_thread(&n->settings,row.thread)!=MessageNotificationApplyError::none)
            message_notifications_mark_incomplete(&n->settings,side.guild_id);
    }
}
void application_notifications_gateway(Application* a,const GatewayClientEvent& event,ConstSlice<u8> payload) noexcept {
    if(!a || event.dispatch_payload_bytes!=payload.count) return;
    auto* n=a->notifications;
    if(event.kind==GatewayClientEventKind::ready) {
        n=ensure(a); if(!n) return;
        if(n->profile!=a->account.status.profile_id) {
            message_notifications_clear(&n->settings); release_retention(*n);
        }
        // Same-profile READY changes queue/ingress epochs, not successful
        // notification history. The source ledger has no READY reset hook.
        // Same-account partial READY settings retain omitted guild/override
        // rows. A full snapshot clears them at its typed begin boundary.
        n->settings.thread_count=0; ++n->settings.revision;
        reset_value(&n->global); n->global_known_gates=n->global_gates=0; n->mention_on_all=false;
        n->profile=a->account.status.profile_id; n->live=true; n->ready=n->snapshot=false;
        audio_preferences(*a,*n); audio_reset(*n,n->audio_preference);
        if(++n->generation==0) ++n->generation;
        if(n->display) desktop_notification_queue_reset(&n->display->queue,n->profile,n->generation);
        reset_ingress(*n); return;
    }
    if(!n && event.kind==GatewayClientEventKind::dispatch &&
        event.dispatch_kind>=GatewayClientDispatchKind::notification_snapshot_begin &&
        event.dispatch_kind<=GatewayClientDispatchKind::notification_incomplete) n=ensure(a);
    if(!n) return;
    if(event.kind==GatewayClientEventKind::resumed) { n->live=true; return; }
    if(event.kind==GatewayClientEventKind::reconnecting || event.kind==GatewayClientEventKind::closed ||
        event.kind==GatewayClientEventKind::failed || event.kind==GatewayClientEventKind::stopped) { n->live=false; purge_display(*n); return; }
    if(event.kind!=GatewayClientEventKind::dispatch) return;
    const auto kind=event.dispatch_kind;
    if(n->display && kind==GatewayClientDispatchKind::message_delete) {
        GatewayMessageDeleteEvent removed{};
        if(gateway_client_event_as_message_delete(event,payload,&removed))
            desktop_notification_queue_purge_message(&n->display->queue,n->profile,n->generation,removed.channel_id,removed.id);
    }
    if(n->display && kind==GatewayClientDispatchKind::message_bulk_delete) {
        const auto ids=a->message_pump.bulk_id_scratch; GatewayMessageBulkDeleteEvent removed{};
        if(gateway_client_event_as_message_bulk_delete(event,payload,ids,&removed))
            for(u32 i=0;i<removed.id_count;++i)
                desktop_notification_queue_purge_message(&n->display->queue,n->profile,n->generation,removed.channel_id,ids[i]);
    }
    // Pending previews have already passed the policy at their event boundary.
    // Replacing the server-confirmed state invalidates those intents; never display stale text
    // under a new mute/privacy/relationship state without its original facts.
    if(event.notification_payload_bytes || invalidates_previews(kind)) purge_display(*n);
    if(event.notification_payload_bytes==~u32{0} &&
        (kind==GatewayClientDispatchKind::guild_create || kind==GatewayClientDispatchKind::guild_update ||
         kind==GatewayClientDispatchKind::channel_create || kind==GatewayClientDispatchKind::channel_update ||
         kind==GatewayClientDispatchKind::thread_members_update || kind==GatewayClientDispatchKind::user_snapshot || kind==GatewayClientDispatchKind::unknown)) {
        const u64 guild=(kind==GatewayClientDispatchKind::guild_create || kind==GatewayClientDispatchKind::guild_update)?event.state.id:
            kind==GatewayClientDispatchKind::thread_members_update?event.thread_members.guild_id:event.state.guild_id;
        scope_admission_failed(*n,guild);
        if(auto* scope=find_scope(n->channel_scopes,n->channel_scope_count,event.state.id))
            scope->known_gates&=~notification_gate_age_access;
        if(kind==GatewayClientDispatchKind::user_snapshot) n->global_known_gates&=~notification_gate_restricted_hours;
        if(kind==GatewayClientDispatchKind::unknown) {
            n->settings.complete=false; ++n->settings.revision;
            for(u32 i=0;i<n->settings.guild_count;++i) n->settings.guilds[i].incomplete=true;
            for(u32 i=0;i<n->guild_scope_count;++i) forget_age_gate(stable_storage_data<Scope>(n->guild_scopes)[i]);
        }
        return;
    }
    if(kind==GatewayClientDispatchKind::notification_snapshot_begin) {
        n->snapshot=true; n->snapshot_partial=(event.notification.flags&gateway_notification_partial)!=0;
        n->snapshot_failed=(event.notification.flags&gateway_notification_incomplete)!=0;
        if(!n->snapshot_partial) message_notifications_clear(&n->settings);
    } else if(kind==GatewayClientDispatchKind::notification_settings_entry) {
        MessageNotificationGuildSetting guild{};
        bool valid=gateway_client_notification_entry(event,payload,&guild) &&
            (!event.notification.override_count || stable_storage_reserve(&n->staging,event.notification.override_count));
        auto* rows=stable_storage_data<MessageNotificationOverride>(n->staging);
        for(u32 i=0;valid && i<event.notification.override_count;++i)
            valid=gateway_client_notification_override_at(event,payload,i,rows+i);
        const u64 version=(event.notification.flags&gateway_notification_version_known)?event.notification.version:n->settings.version;
        // Snapshot entries come from one server-confirmed packet, whose per-guild
        // versions may differ. Accept every entry before advancing the stored
        // maximum; ordinary live updates still obey the stale-version guard.
        const auto applied=valid?message_notifications_apply_settings(&n->settings,{&guild,1},{rows,event.notification.override_count},true,
            n->snapshot?n->settings.version:version):MessageNotificationApplyError::input;
        if(n->snapshot && applied==MessageNotificationApplyError::none)
            n->settings.version=maximum(n->settings.version,version);
        if(applied!=MessageNotificationApplyError::none && applied!=MessageNotificationApplyError::stale) {
            message_notifications_mark_incomplete(&n->settings,event.notification.guild_id); n->snapshot_failed|=n->snapshot;
        }
    } else if(kind==GatewayClientDispatchKind::notification_snapshot_complete) {
        if(!n->snapshot) return;
        n->snapshot_failed|=(event.notification.flags&gateway_notification_incomplete)!=0;
        if(!n->snapshot_partial) n->settings.complete=!n->snapshot_failed;
        if(!n->snapshot_failed && (event.notification.flags&gateway_notification_version_known))
            n->settings.version=maximum(n->settings.version,event.notification.version);
        n->snapshot=false;
    } else if(kind==GatewayClientDispatchKind::notification_global) {
        if(event.notification.flags&gateway_notification_account_flags_known)
            n->mention_on_all=(event.notification.account_flags&32u)!=0;
        MessageNotificationGlobal global{};
        const bool valid=payload.count==sizeof(global) && !(event.notification.flags&gateway_notification_incomplete);
        if(event.notification.flags&gateway_notification_restricted_known) {
            n->global_known_gates|=notification_gate_restricted_hours;
            n->global_gates=(n->global_gates&~notification_gate_restricted_hours)|
                ((event.notification.flags&gateway_notification_restricted)?notification_gate_restricted_hours:0u);
        }
        if(valid) {
            copy_bytes(&global,payload.data,sizeof(global));
            if(global.fields&~15u || global.status>MessageNotificationStatus::offline) { reset_value(&n->global); return; }
            const bool partial=(event.notification.flags&gateway_notification_partial)!=0;
            // Valid full preloaded settings use the connected client's
            // ONLINE/quiet=false defaults. Missing/malformed proto stays unknown.
            if(!partial) {
                if(!(global.fields&notification_status)) global.status=MessageNotificationStatus::online;
                if(!(global.fields&notification_quiet)) global.quiet=false;
                global.fields|=notification_status|notification_quiet;
            }
            message_notifications_apply_global(&n->global,global,partial);
        } else reset_value(&n->global);
    } else if(kind==GatewayClientDispatchKind::notification_incomplete) {
        message_notifications_mark_incomplete(&n->settings,event.notification.guild_id);
        n->snapshot_failed|=n->snapshot;
    } else if(kind==GatewayClientDispatchKind::ready_state_complete) {
        n->ready=!n->snapshot && !n->snapshot_failed; application_notifications_wake(a);
    } else if(kind==GatewayClientDispatchKind::message_ack) {
        if(!payload.count && event.read_state.entry.channel_id && event.read_state.entry.message_id)
            application_notifications_ack(a,event.read_state.entry.channel_id);
    } else if(kind==GatewayClientDispatchKind::channel_delete) {
        message_notifications_remove_thread(&n->settings,event.state.id);
        scope_remove(n->channel_scopes,n->channel_scope_count,event.state.id);
        application_notifications_ack(a,event.state.id);
    } else if(kind==GatewayClientDispatchKind::guild_delete) {
        if(!(event.state.fields&gateway_client_state_field_unavailable) || !event.state.unavailable) {
            message_notifications_remove_guild(&n->settings,event.state.id);
            scope_remove(n->guild_scopes,n->guild_scope_count,event.state.id);
            scope_remove_guild(n->channel_scopes,n->channel_scope_count,event.state.id);
        } else {
            // Joined-thread membership is rebuilt on availability, while the
            // user's guild notification choices stay as the user set them.
            u32 kept{};
            for(u32 i=0;i<n->settings.thread_count;++i)
                if(n->settings.threads[i].guild_id!=event.state.id)
                    n->settings.threads[kept++]=n->settings.threads[i];
            n->settings.thread_count=kept; ++n->settings.revision;
        }
    }
    // Only a trailing sidecar changes the scopes, gates and threads; without one the event is fully applied.
    if(event.notification_payload_bytes) apply_sidecar(n,event,payload);
}
MessageReadBadge application_notifications_read_badge(void* context,const GatewayClientEvent& event,
                                                      ConstSlice<u8> payload) noexcept {
    const auto* a=static_cast<Application*>(context);
    if(!a||!a->notifications||!a->account_ui||
        event.message.mention_role_count>kGatewayMessageMentionRoleMaximumIds)return MessageReadBadge::none;
    const auto& n=*a->notifications;const auto& state=a->account_ui->chat_state;
    const auto* channel=chat_state_store_find_channel(state,event.message.channel_id);
    if(!channel)return MessageReadBadge::none;
    // Message requests and spam never hold a mention count.
    if(channel_is_private(channel->type)&&state.private_channels)
        if(const auto* record=private_channel_store_find(*state.private_channels,channel->id);
            record&&private_channel_is_request(*record))return MessageReadBadge::none;
    const auto* parent=channel->parent_id?chat_state_store_find_channel(state,channel->parent_id):nullptr;
    const auto* guild=channel->guild_id?chat_state_store_find_guild(state,channel->guild_id):nullptr;
    u64 mentioned_roles[kGatewayMessageMentionRoleMaximumIds],self_roles[kGatewayMessageMentionRoleMaximumIds];
    u32 role_count=0;
    if(!collect_mention_roles(state,channel->guild_id,event,payload,mentioned_roles,self_roles,&role_count))return MessageReadBadge::none;
    MessageNotificationPolicyInput input{};
    input.settings=&n.settings;input.self_id=state.self_user_id;input.author_id=event.message.author_id;
    input.guild_id=channel->guild_id;input.channel_id=channel->id;input.parent_id=channel->parent_id;
    input.category_id=category_of(parent);
    input.channel_type=channel->type;input.message_type=event.message.type;input.now_ms=clock_unix_milliseconds();
    mention_flags(input,event);
    input.roles_known=guild&&guild->member_roles_known;
    input.mention_roles={mentioned_roles,event.message.mention_role_count};input.self_roles={self_roles,role_count};
    input.guild_default_known=!guild||(guild->settings.known&guild_settings_default_notifications);
    input.guild_default=guild&&guild->settings.default_message_notifications?MessageNotificationLevel::mentions:MessageNotificationLevel::all;
    if(application_friends_notification_gates(input.author_id,&input.gates))
        input.known_gates=notification_gate_blocked|notification_gate_ignored;
    return message_read_badge(input,n.mention_on_all);
}
void application_notifications_message(void* context,const GatewayClientEvent& event,ConstSlice<u8> payload) noexcept {
    auto* a=static_cast<Application*>(context);
    if(!a || !a->notifications || !a->account_ui || event.dispatch_kind!=GatewayClientDispatchKind::message_create) return;
    auto& n=*a->notifications; auto& ui=*a->account_ui; const auto& state=ui.chat_state;
    if(n.profile!=a->account.status.profile_id || !event.has_sequence) return;
    if(n.has_create_sequence && event.sequence<=n.create_sequence) return;
    for(const auto& seen:n.seen)
        if(seen.channel==event.message.channel_id && seen.message==event.message.id) return;
    n.seen[n.seen_next++%256]={event.message.channel_id,event.message.id};
    n.create_sequence=maximum(n.create_sequence,event.sequence); n.has_create_sequence=true;
    audio_preferences(*a,n); audio_observe(n);
    const bool has_popup_sink=popup_sink(*a,n);
    const auto* channel=chat_state_store_find_channel(state,event.message.channel_id);
    if(!viewable(state,channel)) return;
    const auto* guild=channel->guild_id?chat_state_store_find_guild(state,channel->guild_id):nullptr;
    const auto* parent=channel->parent_id?chat_state_store_find_channel(state,channel->parent_id):nullptr;
    const auto* policy_channel=event.message.type==21?parent:channel;
    if(!viewable(state,policy_channel)) return;
    const auto* policy_parent=policy_channel->parent_id?chat_state_store_find_channel(state,policy_channel->parent_id):nullptr;
    const auto* gs=find_scope(n.guild_scopes,n.guild_scope_count,channel->guild_id);
    const auto* cs=find_scope(n.channel_scopes,n.channel_scope_count,policy_channel->id);
    GatewayMessageEvent message{};
    if(!gateway_client_event_as_message(event,payload,&message)) return;
    u64 mention_roles[kGatewayMessageMentionRoleMaximumIds], self_roles[kGatewayMessageMentionRoleMaximumIds];
    u32 roles{};
    if(!collect_mention_roles(state,channel->guild_id,event,payload,mention_roles,self_roles,&roles)) return;
    MessageNotificationPolicyInput input{};
    input.settings=&n.settings; input.global=n.global;
    // The account's own Do Not Disturb choice silences this client at once,
    // before the settings echo arrives.
    if(local_dnd(*a)) {
        input.global.status=MessageNotificationStatus::dnd;
        input.global.fields|=notification_status;
        input.global.status_expires_ms=0;
    }
    input.self_id=state.self_user_id; input.author_id=message.author_id; input.guild_id=channel->guild_id;
    input.channel_id=policy_channel->id; input.parent_id=policy_channel->parent_id;
    input.category_id=category_of(policy_parent);
    input.now_ms=clock_unix_milliseconds();
    input.mention_roles={mention_roles,event.message.mention_role_count}; input.self_roles={self_roles,roles};
    input.message_flags=message.flags; input.message_type=event.message.type; input.channel_type=policy_channel->type;
    input.ready=n.ready && !n.snapshot && !n.snapshot_failed;
    input.live=n.live; input.type_known=(event.message.fields&gateway_message_field_type)!=0;
    input.identities_known=state.self_user_id && message.author_id && (!channel->guild_id || guild);
    input.roles_known=guild && guild->member_roles_known;
    input.self_mentionable_system=input.message_type==46;
    input.guild_default_known=!channel->guild_id || (gs && (gs->flags&gateway_notification_guild_default_known));
    input.guild_default=gs && (gs->flags&gateway_notification_guild_default_mentions)?MessageNotificationLevel::mentions:MessageNotificationLevel::all;
    mention_flags(input,event);
    input.focused=focused(n);
    input.selected=policy_channel->id==state.selected_channel_id;
    // This process's own call; a voice state from another client never counts.
    input.current_voice_channel=ui.voice_session.channel_id==policy_channel->id &&
        ui.voice_session.phase!=VoiceSetupPhase::idle;
    input.known_gates=notification_gate_active_application|notification_gate_rpc_suppressed|notification_gate_streamer|notification_gate_overlay|notification_gate_required_action;
    input.gates=(a->account.status.required_action==AccountRequirement::none?0u:notification_gate_required_action)|
        (application_streamer_disable_notifications()?notification_gate_streamer:0u);
    input.known_gates|=n.global_known_gates; input.gates|=n.global_gates;
    if(gs) { input.known_gates|=gs->known_gates; input.gates|=gs->gates; }
    if(cs) { input.known_gates|=cs->known_gates; input.gates|=cs->gates; }
    if(policy_channel->type!=3 && policy_channel->type!=18) input.known_gates|=notification_gate_managed;
    if(channel->type!=1 && channel->type!=3) input.known_gates|=notification_gate_request;
    if(!channel->guild_id) input.known_gates|=notification_gate_lurking;
    u32 relationship_gates{};
    if(application_friends_notification_gates(message.author_id,&relationship_gates)) {
        input.known_gates|=notification_gate_blocked|notification_gate_ignored; input.gates|=relationship_gates;
    }
    {
        // Read aloud by the server nickname, else the name the message carries.
        Utf8View speaker=message.member_nick;
        if(!speaker.bytes&&!gateway_client_event_message_author_name(event,payload,&speaker)) reset_value(&speaker);
        if(contains_nul(speaker)||speaker.bytes>256) reset_value(&speaker);
        speak(*a,n,message,*channel,speaker,relationship_gates&(notification_gate_blocked|notification_gate_ignored));
    }
    if(event.message.fields&gateway_message_field_author_policy) {
        input.known_gates|=notification_gate_spammer;
        if(event.message.author_public_flags&1048576) input.gates|=notification_gate_spammer;
    }
    input.known_gates|=notification_gate_own_activity;
    // JS tests !==undefined, so a present null instance counts.
    if(event.message.fields&gateway_message_field_activity_instance) {
        u64 interaction_user{};
        if(!gateway_client_event_message_interaction_user(event,payload,&interaction_user)) return;
        if(interaction_user==state.self_user_id) input.gates|=notification_gate_own_activity;
    }
    if(input.message_type!=47) input.known_gates|=notification_gate_changelog;
    else if(!(event.message.mention_flags&gateway_client_message_changelog_present)) {
        input.known_gates|=notification_gate_changelog; input.gates|=notification_gate_changelog;
    }
    // A blocked or ignored user who ran the interaction suppresses its reply.
    // The interaction user stands for interaction_metadata.user.
    if(event.message.fields&gateway_message_field_interaction_metadata) {
        u64 interaction_user{}; u32 interaction_gates{};
        if(!(event.message.mention_flags&gateway_client_message_interaction_metadata_present))
            input.known_gates|=notification_gate_interaction_relationship;
        else if(gateway_client_event_message_interaction_user(event,payload,&interaction_user)) {
            if(!interaction_user) input.known_gates|=notification_gate_interaction_relationship;
            else if(application_friends_notification_gates(interaction_user,&interaction_gates)) {
                input.known_gates|=notification_gate_interaction_relationship;
                if(interaction_gates) input.gates|=notification_gate_interaction_relationship;
            }
        }
    }
    input.required_gates=(1u<<16)-1;
    const MessageNotificationDevice device{a->preferences.desktop_enabled,a->preferences.message_sounds_enabled,
        a->preferences.selected_message_sounds_enabled,a->preferences.taskbar_flash,n.mention_on_all};
    const auto outputs=message_notification_outputs({&input,true,channel->id==state.selected_channel_id,true,
        a->preferences.selected_message_sounds_enabled},device);
    // Selected message3 is direct and precedes preview/ordinary transport. It
    // shares MESSAGE1's sound enablement, but never shares fallback H throttle.
    if(outputs.selected_sound) sound_submit(n,2,n.audio_epoch);
    if(!outputs.ordinary) return;
    Utf8View author{}; if(!gateway_client_event_message_author_name(event,payload,&author)) return;
    if(!author.bytes) author=entity(a,MessageMarkdownEntityKind::user,message.author_id);
    if(contains_nul(author)) reset_value(&author);
    if(author.bytes>256) author=utf8_literal("New message");
    char body[1024];
    const Utf8View text=preview(*a,message.content,body,sizeof(body));
    // Source projects title/body (and dispatches RPC) before the ordinary
    // desktop-NEVER or popup sound branch. Invalid author projection must not
    // produce that later sound; selected message3 has already been handled.
    if(outputs.sound==NotificationMessageSoundRoute::direct) sound_submit(n,1,n.audio_epoch);
    else if(outputs.sound==NotificationMessageSoundRoute::popup) sound_throttled(n,1,n.audio_epoch);
    // Popup transport contributes only streamer here; sound remains independent.
    if(!has_popup_sink || !outputs.popup) return;
    auto* display=ensure_display(n);
    if(!display) { log_write(LogLevel::warning,"notifications","display_storage_unavailable"); return; }
#if defined(MAICORD_NOTIFICATION_FIXTURE)
    const bool suppress_popup=n.fixture_suppress_popup;
#else
    constexpr bool suppress_popup=false;
#endif
    // The author's cached avatar bitmap, when resident; the queue copies the URI.
    char avatar_uri[512];
    Utf8View avatar{};
    if(g_notification_avatar && message.author_id) {
        Utf8View hash{};
        if(!gateway_client_event_message_author_avatar_hash(event,payload,&hash)) reset_value(&hash);
        if(const auto* user=user_store_find(a->account_ui->user_store,message.author_id))
            if(user->avatar_hash_bytes) hash=user_store_avatar_hash(*user);
        avatar=g_notification_avatar(message.author_id,hash,{avatar_uri,sizeof(avatar_uri)});
    }
    const auto admitted=desktop_notification_queue_enqueue(&display->queue,
        {n.profile,n.generation,channel->id,message.id,event.sequence,author,text,avatar,true,suppress_popup,policy_channel->id,
            outputs.sound==NotificationMessageSoundRoute::popup?n.audio_epoch:0,
            outputs.sound==NotificationMessageSoundRoute::popup?1u:0u});
    if(!admitted.ok()) log_u64(LogLevel::warning,"notifications","display_admission_error",static_cast<u64>(admitted.error));
    else post_continuation(*a,n);
}
} // namespace maicord::app_detail
