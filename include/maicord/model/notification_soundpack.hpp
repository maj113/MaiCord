#pragma once
#include "maicord/model/notification_sound_policy.hpp"

namespace maicord {
// Persisted enum values; the order must not change so existing preferences stay
// readable. Every choice uses MaiCord's own sounds.
enum class NotificationSoundpack : u8 {
    classic, retro, bop, ducky, lofi, asmr, discodo, halloween, winter_holiday
};
constexpr bool notification_soundpack_valid(NotificationSoundpack pack) noexcept {
    return static_cast<u8>(pack)<=static_cast<u8>(NotificationSoundpack::winter_holiday);
}
// Bank IDs for the ordinary/focused alerts.
// Invalid persisted inputs fail closed.
constexpr u32 notification_soundpack_clip(NotificationSoundpack pack,NotificationSoundKind kind) noexcept {
    if(!notification_soundpack_valid(pack)) return 0;
    if(kind==NotificationSoundKind::message3) return 2;
    if(kind!=NotificationSoundKind::message1) return 0;
    return 1;
}
} // namespace maicord
