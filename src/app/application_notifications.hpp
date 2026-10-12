#pragma once
#include "maicord/app/gateway_client.hpp"
#include "maicord/model/message_notifications.hpp"
#if defined(MAICORD_NOTIFICATION_FIXTURE)
#include "maicord/platform/desktop_notifications.hpp"
#include "maicord/platform/notification_audio.hpp"
#include "maicord/model/notification_soundpack.hpp"
#include "maicord/core/win32_types.hpp"
#endif
namespace maicord::app_detail {
struct Application;
inline constexpr UINT kApplicationNotificationMessage = kWindowMessageApp + 0x33;
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
struct ApplicationNotificationFixtureStorageMetrics {
    u64 committed_bytes, reserved_bytes;
    u32 stride_bytes, capacity_rows, reserved_rows;
};
struct ApplicationNotificationFixtureMetrics {
    u32 guilds,overrides,threads; bool ready,live; u32 seen_count;
    // This compiled fixture owner includes the nonemitting popup/audio hooks.
    // VM commit is queried, while stable storage totals are its Arena backing
    // counters; neither measurement is working set or live row demand.
    u32 owner_sizeof_bytes, owner_fixture_extension_bytes;
    u64 owner_vm_committed_bytes, storage_committed_bytes, storage_reserved_bytes;
    bool owner_vm_query_ok;
    u64 audio_epoch, audio_backend_epoch, audio_submitted, sound_deadline;
    u32 audio_output_percent;
    NotificationSoundpack audio_pack;
    bool audio_backpressured, audio_failed, audio_worker_started;
    u32 display_owner_sizeof_bytes, display_outstanding, display_bytes;
    u64 display_vm_committed_bytes, display_admitted, display_submitted;
    // Retention owns two independently page-rounded VirtualAlloc banks. These
    // are metadata/VM counts, not process working set or visible OS banners.
    u64 retention_rows_committed_bytes, retention_index_committed_bytes,
        retention_logical_bytes, retention_capacity_bytes, retention_allocation_failures, retention_tickets;
    u32 retention_channels, retention_capacity_channels,
        retention_index_slots, retention_row_bytes;
    ApplicationNotificationFixtureStorageMetrics guild_storage, override_storage,
        thread_storage, staging_storage, guild_scope_storage, channel_scope_storage;
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
