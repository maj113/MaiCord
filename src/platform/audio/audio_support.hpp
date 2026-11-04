#pragma once

#include "maicord/core/byte_range.hpp"
#include "maicord/platform/audio/media_audio.hpp"
#include "maicord/platform/audio/notification_audio.hpp"

// Small helpers the audio sources share across the transports (Windows, Linux) and the pure parts.
namespace maicord::audio_detail {

// True when both byte ranges are valid and do not overlap.
inline bool disjoint(const void* a, u32 an, const void* b, u32 bn) noexcept {
    const ConstSlice<u8> x{static_cast<const u8*>(a), an}, y{static_cast<const u8*>(b), bn};
    return byte_range_valid(x) && byte_range_valid(y) && !byte_ranges_overlap(x, y);
}
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
