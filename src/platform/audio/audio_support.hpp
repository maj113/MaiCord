#pragma once

#include "maicord/platform/audio/media_audio.hpp"
#include "maicord/platform/audio/notification_audio.hpp"

// Small helpers the audio sources share across the transports (Windows, Linux) and the pure parts.
namespace maicord::audio_detail {

inline MediaAudioResult fail(MediaAudioError error, u32 native = 0) noexcept {
    return {error, native};
}
inline NotificationAudioResult fail(NotificationAudioError error, u32 native = 0) noexcept {
    return {error, native};
}
// Saturating counter increment.
inline void add(u64& counter, u32 n = 1) noexcept {
    counter = counter > ~u64{0} - n ? ~u64{0} : counter + n;
}

} // namespace maicord::audio_detail
