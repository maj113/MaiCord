#pragma once
#include "application_self_status.hpp"
#include "application_gif_favorites.hpp"
#include "application_surface_motion.hpp"
#include "maicord/app/quick_switcher_controller.hpp"

#include "maicord/ui/button_keyboard.hpp"

#include "application_message_delete.hpp"
#include "application_image_viewer.hpp"
#include "application_message_reaction.hpp"
#include "application_message_mutation_notice.hpp"

#include "maicord/app/account_host.hpp"
#include "maicord/app/client_preferences.hpp"
#include "maicord/app/client_preferences_writer.hpp"
#include "maicord/app/animation_visual_runtime.hpp"
#include "maicord/app/frame_rate.hpp"
#include "maicord/app/member_list_channel_binding.hpp"
#include "maicord/app/message_edit_runtime.hpp"
#include "maicord/app/message_event_pump.hpp"
#include "maicord/app/message_history_sync.hpp"
#include "maicord/app/message_media_controller.hpp"
#include "maicord/app/media_url_resolver.hpp"
#include "maicord/app/message_view_runtime.hpp"
#include "maicord/app/reaction_viewer_storage.hpp"
#include "maicord/app/remote_auth_client.hpp"
#include "maicord/core/time.hpp"
#include "maicord/protocol/user_auth.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/ui/composer_autocomplete.hpp"
#include "maicord/ui/composer_spellcheck.hpp"
#include "maicord/ui/login_scene.hpp"
#include "maicord/ui/message_action_bar.hpp"
#include "maicord/ui/message_composer_scene.hpp"
#include "maicord/ui/message_context_controller.hpp"
#include "maicord/ui/qr_code.hpp"
#include "maicord/ui/popup_scene.hpp"
#include "maicord/ui/toast.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"
#include "maicord/ui/scene.hpp"
#include "maicord/ui/member_list_scene.hpp"
#include "maicord/ui/guild_rail_scene.hpp"
#include "maicord/ui/scrollbar.hpp"
#include "maicord/ui/secure_edit.hpp"
#include "maicord/ui/secure_text_store.hpp"
#include "maicord/ui/smooth_scroll.hpp"
#include "maicord/ui/text_pointer_selection.hpp"
#include "maicord/ui/scroll_host.hpp"
#include "maicord/ui/widget_storage.hpp"
#include "maicord/ui/thread_header.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/app/reaction_viewer_controller.hpp"

namespace maicord::app_detail {

struct AccountUiRuntime;
struct ApplicationVideo;
struct ApplicationTooltip;
struct ApplicationAccountMenu;
struct ApplicationAccountLogout;
struct ApplicationSearch;
struct ApplicationPinnedMessages;
struct ApplicationMediaPicker;
struct ApplicationQuickSwitcher;
struct ApplicationMessageForward;
struct ApplicationDownloads;
struct ApplicationAttachments;
struct ApplicationProfile;
struct ApplicationTextForm;
struct ApplicationComposerTextInput;
struct ApplicationSettings;
struct ApplicationThemeEditor;
struct ApplicationPlugins;
struct ApplicationFriends;
struct ApplicationReadState;
struct ApplicationScopedRead;
struct ApplicationNotifications;

inline constexpr u32 kAccountStopTimeoutMs = 5'000;
inline constexpr u32 kDefaultCaretBlinkMilliseconds = 530;
inline constexpr float kMinimumAccountMessageWidthDips = kUiShellMinimumMessageWidthDips;
inline constexpr float kMinimumLoginClientWidthDips = 560.0f;
inline constexpr float kMinimumClientHeightDips = 480.0f;
inline constexpr u32 kUiRebuildRetryMinimumMilliseconds = 100;
inline constexpr u32 kUiRebuildRetryMaximumMilliseconds = 1'600;
inline constexpr u32 kRemoteAuthRetryMinimumMs = 1'500;
inline constexpr u32 kRemoteAuthRetryMaximumMs = 30'000;
inline constexpr UINT kBeginWebAuthnMessage = kWindowMessageApp + 0x31;
inline constexpr UINT kApplicationPhoneVerifyMessage = kWindowMessageApp + 0x32;
inline constexpr u32 kWebAuthnStructuralCapacity = 4 * 1024;
inline constexpr u32 kGuildRailRowCapacity = 192;
inline constexpr u32 kInitialVirtualRowCapacity = 32;
// Root, top spacer, rows and the trailing "Add a server" button.
inline constexpr u32 kGuildRailWidgetCapacity = kGuildRailRowCapacity + 3;
inline constexpr u32 kGuildRailInitialWidgetCapacity = 32;
inline constexpr u32 kShellNavigationRowCapacity = 192;
inline constexpr u32 kShellNavigationWidgetCapacity = kShellNavigationRowCapacity * 4 + 2;
inline constexpr u32 kShellNavigationInitialWidgetCapacity = 128;
inline constexpr u32 kMessageSceneRowCapacity = 192;
inline constexpr u32 kMessageSceneWidgetCapacity =
    kMessageSceneRowCapacity * kMessageSceneWidgetsPerRow + 1;
// A tall window's worth of rows; the storage grows to kMessageSceneWidgetCapacity on demand.
inline constexpr u32 kMessageSceneInitialWidgetCapacity = 48 * kMessageSceneBaseWidgetsPerRow + 1;
inline constexpr u32 kMemberListRowCapacity = 128;
inline constexpr u32 kMemberListWidgetCapacity = 1 + kMemberListRowCapacity * kMemberListWidgetsPerRow;
inline constexpr u32 kMemberListInitialWidgetCapacity = 128;
// Menus, autocomplete and pickers share one bounded popup bank.
inline constexpr u32 kPopupItemCapacity = 72;
inline constexpr u32 kPopupWidgetCapacity = 2 + kPopupItemCapacity * kPopupWidgetsPerItem;
inline constexpr u32 kPopupInitialWidgetCapacity = 32;

static_assert(kMessageSceneWidgetCapacity <= kInvalidUiWidgetSlot);
static_assert(static_cast<u8>(user_auth_mfa_capability_totp) == static_cast<u8>(login_mfa_capability_totp));
static_assert(static_cast<u8>(user_auth_mfa_capability_sms) == static_cast<u8>(login_mfa_capability_sms));
static_assert(static_cast<u8>(user_auth_mfa_capability_backup) ==
              static_cast<u8>(login_mfa_capability_backup));
static_assert(static_cast<u8>(user_auth_mfa_capability_webauthn) ==
              static_cast<u8>(login_mfa_capability_webauthn));

enum class MessageHistoryRequestOwner : u8 {
    none,
    private_conversation,
    attachment_upload,
    typing,
    message_send,
    message_delete,
    message_edit,
    message_reaction,
    reaction_users,
    user_profile,
    commands,
    catchup,
    message_jump,
    attachment_refresh,
    media_url_refresh,
    older,
    friends,
    read_state_ack,
    message_search,
    self_status,
    media_catalog,
    gif_favorites,
    channel_overview,
    reply_refresh,
    pinned_messages,
    search_navigation,
    thread_actions,
    thread_browser,
    thread_creation,
    forum_post,
    scoped_read,
    poll,
    member_moderation,
    guild_invite,
    emoji_preferences,
    channel_prefetch,
    user_settings,
    account_settings,
    private_channel,
    guild_notification_settings,
    custom_status,
    inbox_mentions,
    stream_preview,
    stage,
    message_components,
    invite_cards,
    activity_sharing,
    soundboard,
    reaction_tooltip,
    dm_profile,
    count,
};

enum class PopupOwnerKind : u8 {
    none,
    private_channel,
    private_notice,
    message,
    member,
    autocomplete,
    commands,
    attachments,
    composer_edit,
    friends,
    channel,
    guild,
    settings_choice,
    account_menu,
    notification_settings,
    stream, // Watched stream tile: volume slider, full screen, stop watching.
    guild_folder, // Guild rail folder header (owner_id is the folder's own id).
    message_components, // An open message select menu (owner_id is the message).
    soundboard,         // The call bar's soundboard picker (owner_id is the channel).
    plugin_popover,     // A plugin slot popover (owner_id is the plugin).
    profile,            // A profile card menu: Edit Profile, Friend Actions, role picker.
};

enum class PaneResizeKind : u8 {
    none,
    channels,
    members,
};

enum class ScrollbarDragKind : u8 {
    none,
    messages,
    channels,
    members,
    popup,
};

enum PopupAction : u32 {
    popup_action_copy_member_name = 101,
    popup_action_copy_member_id,
    popup_action_message_user,
    popup_action_close_dm,
    popup_action_private_notice_close,
    popup_action_private_notice_retry,
    popup_action_mark_channel_read,
    popup_action_copy_guild_id,
    popup_action_edit_channel,
    popup_action_mark_channel_unread,
    popup_action_retry_channel_unread,
    popup_action_browse_threads,
    popup_action_mark_scope_read,
    popup_action_retry_scope_read,
    popup_action_create_text_channel,
    popup_action_create_voice_channel,
    popup_action_private_add_people,
    popup_action_private_rename,
    popup_action_private_leave,
    popup_action_private_remove_member,
    popup_action_private_notice_dismiss,
    popup_action_private_call_join,
    popup_action_private_call_decline,
    popup_action_invite_people,
    popup_action_leave_guild,
    popup_action_folder_settings,
    popup_action_open_voice_chat,
    popup_action_guild_onboarding,
    popup_action_create_event,
    popup_action_private_accept_request,
    popup_action_private_ignore_request,
    popup_action_autocomplete_first = 1'000,
};

struct LoginRuntime {
    SecureEdit login;
    SecureEdit password;
    SecureEdit mfa;
    SecureTextStore text_store;
    QrCode qr_code;
    char remote_user[kRemoteAuthMaximumUserPayloadBytes];
    LoginControl focus;
    LoginControl pointer_selection;
    RendererLoginStatus status;
    LoginRemoteStatus remote_status;
    UserAuthMfaType mfa_type;
    u8 mfa_capabilities;
    bool initialized;
    bool visible;
    bool sms_requested;
    bool sms_sending;
    bool sms_failed;
    bool webauthn_attempted;
    bool backup_acknowledged;
    bool remote_active;
    bool qr_ready;
    u32 remote_user_bytes;
    u32 caret_blink_ms;
    u8 backup_acknowledgement_seconds;
    u8 remote_retry_attempt;
    u64 backup_acknowledgement_at_ms;
    u64 remote_restart_at_ms;
    // Server admission belongs to the login actor, not its replaceable worker.
    u64 auth_retry_at_ms;
    bool auth_retry_time_unknown;
    u64 caret_blink_at_ms;
    bool caret_visible;
    bool reset_mode;
    bool reset_retry;
    u64 application_auth_epoch;
    u64 browser_launch_epoch;
    bool browser_return_to_qr;
    u64 webauthn_post_epoch;
    // Browser CAPTCHA bridge: generation is the explicit-click epoch.
    u64 captcha_generation;
    u64 captcha_launch_epoch;
    u64 captcha_deadline_ms;
    LoginCaptchaStatus captcha_status;
    bool captcha_submitted;
    bool password_revealed;
};

struct Application {
    struct LoadTrace {
        i64 channel_started;
        u64 channel_epoch;
        i64 pending_since[static_cast<u32>(MessageHistoryRequestOwner::count)];
        u64 pending_mask;
        MessageHistoryRequestOwner primary, details;
        u64 next_wait_report_ms;
        bool shell_presented, history_ready, history_presented, members_presented, media_uploaded;
    } load_trace;
    ApplicationProfile* profile;
    ApplicationSettings* settings;
    ApplicationThemeEditor* theme_editor;
    u64 theme_editor_lifetime_serial;
    // Process-lifetime owner identity; a freed Settings address can be reused.
    u64 settings_lifetime_serial;
    // Process-lifetime menu scope; settings/model allocations may be recycled.
    u64 plugin_account_scope_serial;
    struct LocalPosterPresentation {
        void* instance;
        u64 profile, account_scope;
        bool pending;
    } local_poster_presentation;
    ApplicationPlugins* plugins;
    ApplicationFriends* friends;
    ApplicationReadState* read_state;
    ApplicationScopedRead* scoped_read;
    ApplicationNotifications* notifications;
    ApplicationVideo* video;
    ApplicationTooltip* tooltip;
    // The toast queue (application_toast.hpp); zero is empty.
    UiToastScene toasts;
    ApplicationAccountMenu* account_menu;
    ApplicationAccountLogout* account_logout;
    u64 account_logout_lifetime_serial;
    ApplicationSearch* search;
    ApplicationPinnedMessages* pinned_messages;
    ApplicationMediaPicker* media_picker;
    ApplicationQuickSwitcher* quick_switcher;
    ApplicationMessageForward* message_forward;
    ApplicationDownloads* downloads;
    u64 downloads_lifetime_serial;
    u32 downloads_held_key;
    bool downloads_pointer_tail;
    u64 message_forward_lifetime_serial;
    u32 message_forward_held_key;
    bool message_forward_pointer_tail;
    QuickSwitcherScope quick_switcher_recent_scope;
    u64 quick_switcher_recent_channels[kQuickSwitcherRecentMaximum];
    u64 quick_switcher_lifetime_serial;
    u32 quick_switcher_held_key;
    u8 quick_switcher_recent_count;
    ApplicationGifFavorites* gif_favorites;
    ApplicationSurfaceMotionOwner surface_motion;
    u64 media_picker_scope_serial, media_picker_item_serial;
    ApplicationSelfStatusRequest self_status;
    u64 video_lifetime_serial;
    u64 read_last_activity_ms;
    ClientPreferences preferences;
    ClientPreferencesWriter* preferences_writer;
    u64 preferences_request_revision;
    bool preferences_save_failed, preferences_save_pending;
    u32 settings_held_key;
    HINSTANCE instance;
    HWND window;
    MessageViewRuntime message_view;
    MessageMediaController message_media;
    D3D11Renderer renderer;
    MessageEventPump message_pump;
    MessageHistoryCatchup message_catchup;
    MessageHistoryOlder message_older;
    Clock clock;
    FrameRateCounter frame_rate;
    i64 process_start;
    u64 frame_clock_watchdog_at_us;
    UiScrollHost scroll_host;
    // Active content surface; its logical row focus survives virtual rebuilds.
    // Managers live in Application. Popups temporarily retain this owner.
    UiWidgetManager* context_focus;
    UiButtonKeyboard button_keyboard;
    UiWidgetManager* button_pointer_owner;
    MemberListIdentityCache member_list_identity;
    MemberListSubscriptionRetry member_list_subscription;
    MemberListChannelMemory member_list_channels;
    ThreadMemberListSubscriptionRetry thread_member_list_subscription;
    u64 message_composer_caret_blink_at_ms;
    AccountHost account;
    LoginRuntime login;
    LoginScene login_scene;
    UiScene shell_scene;
    UiThreadHeader thread_header;
    GuildRailScene guild_rail_scene;
    UiVirtualRowStorageOwner guild_rail_rows;
    UiWidgetStorageOwner guild_rail_storage;
    ShellNavigationScene shell_navigation_scene;
    UiVirtualRowStorageOwner shell_navigation_rows;
    UiWidgetStorageOwner shell_navigation_storage;
    MessageScene message_scene;
    MessageActionBar message_action_bar;
    ReactionViewer reaction_viewer;
    ApplicationImageViewer image_viewer;
    ReactionViewerController reaction_viewer_controller;
    ReactionViewerStorage reaction_viewer_storage;
    UiWidgetStorageOwner message_scene_storage;
    MessageComposer message_composer;
    ApiRejection message_rejection;
    u64 message_rejection_channel_id;
    ApplicationMessageMutationNotice message_mutation_notice;
    MessageComposerScene message_composer_scene;
    MemberListScene member_list_scene;
    UiVirtualRowStorageOwner member_list_rows;
    UiWidgetStorageOwner member_list_storage;
    PopupScene popup_scene;
    MessageContextController message_context;
    ComposerAutocomplete composer_autocomplete;
    UiWidgetStorageOwner popup_storage;
    PopupOwnerKind popup_owner_kind;
    u32 composer_menu_revision;
    ComposerSpellcheck composer_spellcheck;
    u32 composer_menu_held_key;
    PaneResizeKind pane_resize;
    ScrollbarDragKind scrollbar_drag;
    float scrollbar_thumb_grab_offset;
    AccountUiRuntime* account_ui;
    ApplicationAttachments* attachments;
    ApplicationTextForm* text_form;
    ApplicationComposerTextInput* composer_text_input;
    u32 text_form_held_key;
    AnimationVisualRuntime animations;
    MessageHistoryRequestOwner message_history_request_owner;
    MessageHistoryRequestOwner details_request_owner;
    u64 primary_request_id;
    u64 details_request_id;
    // When the DM profile panel's fetch may next be admitted, and its shown
    // activity timers next tick (a visual deadline); zero: none.
    u64 dm_profile_wake_ms, dm_profile_clock_ms;
    AttachmentRefresh attachment_refresh;
    ReplyReferenceRefresh reply_refresh;
    MediaUrlResolver media_urls;
    MessageDeleteRequest message_delete;
    MessageEditRuntime message_edit;
    MessageReactionRequest message_reaction;
    u64 pending_message_jump_channel_id;
    u64 pending_message_jump_message_id;
    RendererMessageTextPositionHit message_text_selection;
    u32 message_text_selection_anchor;
    u32 message_text_selection_caret;
    bool renderer_ready;
    bool frame_loop_active;
    bool frame_pending;
    bool pane_layout_pending;
    bool keyboard_focus_visible;
    bool first_present_recorded;
    bool frame_clock_timer_fallback;
    bool message_composer_caret_visible;
    bool message_composer_pointer_selection;
    bool message_text_pointer_selection;
    bool member_pane_visible;
    float channel_pane_width;
    float member_pane_width;
    // Width when the active splitter drag began (Escape restores it).
    float pane_resize_origin_width;
    // A finished splitter drag changed a width; Settings persists it once.
    bool pane_widths_persist_pending;
    // Multi-click state shared by every text surface; the per-surface unit
    // keeps word/line drags anchored to the originally selected unit.
    UiClickTracker click_tracker;
    UiTextPointerSelection composer_pointer_unit;
    UiTextPointerSelection message_text_pointer_unit;
    u32 frame_clock_timeouts;
    u32 presentation_failures;
};

extern Application g_app;

// Microseconds since process start on the application clock.
inline u64 application_now_us() noexcept {
    return clock_microseconds(g_app.clock, g_app.process_start, clock_now());
}

// Resident timeline record of a message, or null.
inline const MessageRecord* application_message_record(u64 id) noexcept {
    const auto found = message_store_find(g_app.message_view.store, id);
    return found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
}

// Scope key of the active account: the plugin scope serial, else the profile id.
inline u64 application_account_scope() noexcept {
    return g_app.plugin_account_scope_serial ? g_app.plugin_account_scope_serial
                                             : g_app.account.status.profile_id;
}

// The signed-in account UI can issue requests.
inline bool application_account_available() noexcept {
    return g_app.account_ui && !g_app.login.visible && g_app.account.status.running;
}

const Theme& application_theme() noexcept;
bool application_refresh_theme_consumers(bool typography_changed, bool geometry_changed) noexcept;
bool application_synchronize_theme_consumers() noexcept;
void application_theme_release() noexcept;

// UI-thread only. Worker completions reach this through the main event pump.
bool application_request_frame() noexcept;
// Drops the retained presentation and schedules a frame.
inline void application_repaint() noexcept {
    g_app.renderer.retained_presentation.valid = false;
    application_request_frame();
}

} // namespace maicord::app_detail
