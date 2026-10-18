#pragma once
#include "maicord/core/json_reader.hpp"
#include "maicord/model/message_notifications.hpp"
namespace maicord {
inline constexpr u32 kNotificationSettingsMaximumBytes = 128 * 1024;
enum class NotificationSettingsDecodeError : u8 { none, input, json, duplicate, field, capacity, protobuf, base64 };
struct NotificationSettingsPacket { u32 guild_count, override_count; u64 version; bool partial, version_known; };
// snapshot=true expects READY user_guild_settings {entries,partial,version};
// false expects one USER_GUILD_SETTINGS_UPDATE entry. Output slices are caller
// staging storage and may be dirty after failure; packet stays unpublished.
NotificationSettingsDecodeError decode_notification_settings(const JsonIndexedDocument&, JsonValue,
    bool snapshot, Slice<MessageNotificationGuildSetting>, Slice<MessageNotificationOverride>,
    NotificationSettingsPacket*) noexcept;
NotificationSettingsDecodeError decode_notification_thread(const JsonIndexedDocument&, JsonValue,
    u64 fallback_guild_id, MessageNotificationThreadSetting*, u64 fallback_thread_id) noexcept;
// Whole canonical base64 is validated first. Protobuf reads virtual decoded
// bytes with a single cached quartet: no decoded scratch bank or copy.
NotificationSettingsDecodeError decode_notification_global_base64(Utf8View, MessageNotificationGlobal*) noexcept;
} // namespace maicord
