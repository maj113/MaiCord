#pragma once
namespace maicord { struct GatewayClientEvent; }
#include "maicord/model/notifications/message_notifications.hpp"
#if defined(MAICORD_NOTIFICATION_FIXTURE)
#include "maicord/platform/desktop/notifications.hpp"
#include "maicord/platform/audio/notification_audio.hpp"
#include "maicord/model/notifications/soundpack.hpp"
#include "maicord/core/win32_types.hpp"
#endif
namespace maicord::app_detail {
struct Application;
bool application_notifications_initialize(Application*) noexcept;
// Optional author avatar for desktop toasts: writes the author's resident
// avatar bitmap as a local PNG and returns its file:/// URI in storage, or an
// empty view when it is not cached. Installed by the client, which owns the
// renderer; never downloads anything.
using ApplicationNotificationAvatarFunction = Utf8View (*)(u64 user_id, Utf8View avatar_hash,
                                                           Slice<char> storage) noexcept;
void application_notifications_set_avatar_source(ApplicationNotificationAvatarFunction) noexcept;
void application_notifications_attach_window(Application*) noexcept;
void application_notifications_gateway(Application*, const GatewayClientEvent&, ConstSlice<u8>) noexcept;
void application_notifications_message(void*, const GatewayClientEvent&, ConstSlice<u8>) noexcept;
MessageReadBadge application_notifications_read_badge(void*, const GatewayClientEvent&,
                                                      ConstSlice<u8>) noexcept;
void application_notifications_drain(Application*) noexcept;
bool application_notifications_can_pop(Application*) noexcept;
bool application_notifications_gateway_pending(const Application*) noexcept;
// Absolute GetTickCount64 deadline; zero when idle, disconnected or audio-full.
u64 application_notifications_next_wake(const Application*) noexcept;
void application_notifications_wake(Application*) noexcept;
void application_notifications_selection(Application*) noexcept;
void application_notifications_ack(Application*, u64 channel_id) noexcept;
void application_notifications_account_stop(Application*) noexcept;
void application_notifications_release(Application*) noexcept;
void application_notifications_prepare_shutdown(Application*) noexcept;
void application_notifications_settings_refresh(Application*) noexcept;
bool application_notifications_setup(Application*) noexcept;
Utf8View application_notifications_status(const Application*) noexcept;
// Server-confirmed notification settings; null before the first READY.
const MessageNotificationStore* application_notifications_store(const Application*) noexcept;
// Replaces one guild's settings (channel 0) or one channel override with a
// local projection until the next USER_GUILD_SETTINGS_UPDATE. Other overrides
// of the guild are kept. Pending previews are dropped like any settings change.
bool application_notifications_project(Application*, u64 guild_id, u64 channel_id,
                                       const MessageNotificationSetting&) noexcept;
#if defined(MAICORD_NOTIFICATION_FIXTURE)
struct ApplicationNotificationFixtureMetrics {
    u32 guilds,overrides,threads; bool ready,live; u32 seen_count;
    u64 audio_epoch, audio_backend_epoch, sound_deadline;
    u32 audio_output_percent;
    NotificationSoundpack audio_pack;
    bool audio_failed, audio_worker_started;
    u32 display_outstanding;
    u64 retention_tickets;
    u32 retention_channels, retention_capacity_channels;
};
bool application_notifications_fixture_sink(Application*,void(*)(void*,const ::maicord::DesktopNotificationPayload&) noexcept,void*) noexcept;
// Explicit fake sink only: fixture builds never submit to a real audio worker.
bool application_notifications_fixture_audio(Application*,
    NotificationAudioResult(*)(void*,const NotificationAudioRequest&) noexcept,
    NotificationAudioStatus(*)(void*) noexcept,u64(*)(void*) noexcept,void*) noexcept;
void application_notifications_fixture_focus(Application*,bool) noexcept;
void application_notifications_fixture_show_state(Application*,DesktopNotificationShowState) noexcept;
bool application_notifications_fixture_retention_remove(Application*,
    DesktopNotificationResult(*)(void*,DesktopNotificationActivation) noexcept,void*) noexcept;
// 0 normal; 1 fail row allocation; 2 fail index allocation after rows exist.
// Existing capacity works in every mode; production contains no failure hook.
void application_notifications_fixture_retention_oom(Application*,u32 bank) noexcept;
void application_notifications_fixture_suppress_popup(Application*,bool) noexcept;
u64 application_notifications_fixture_retained(Application*,u64 channel,u32 ordinal) noexcept;
ApplicationNotificationFixtureMetrics application_notifications_fixture_metrics(Application*) noexcept;
#endif
}
