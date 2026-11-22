#pragma once
namespace maicord { struct GatewayClientEvent; }
#include "maicord/model/notifications/message_notifications.hpp"
namespace maicord { enum class VoiceConnectionSound : u8; }
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
// Voice connection sounds go through the notification sound worker. While they are on, the worker stays on with
// message sounds off. The voice pump sets the switch on each turn.
void application_notifications_voice_sounds(Application*, bool enabled) noexcept;
// Plays a voice connection sound at volume_percent (1..100) of the message alerts' gain. Nothing plays while voice
// sounds are off, while Streamer Mode silences sounds or while the worker is full.
void application_notifications_voice_sound(Application*, VoiceConnectionSound, u32 volume_percent) noexcept;
bool application_notifications_setup(Application*) noexcept;
Utf8View application_notifications_status(const Application*) noexcept;
// Server-confirmed notification settings; null before the first READY.
const MessageNotificationStore* application_notifications_store(const Application*) noexcept;
// Replaces one guild's settings (channel 0) or one channel override with a
// local projection until the next USER_GUILD_SETTINGS_UPDATE. Other overrides
// of the guild are kept. Pending previews are dropped like any settings change.
bool application_notifications_project(Application*, u64 guild_id, u64 channel_id,
                                       const MessageNotificationSetting&) noexcept;
}
