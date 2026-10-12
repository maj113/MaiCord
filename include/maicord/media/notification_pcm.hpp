#pragma once
#include "maicord/media/deflate.hpp"

namespace maicord {
inline constexpr u32 kNotificationPcmBlockFrames=256;
inline constexpr u32 kNotificationPcmMaximumFrames=10*48000;
struct NotificationPcmSpec {u32 frames{};u8 left_order{},side_order{};};
enum class NotificationPcmError : u8 {none,invalid_input,inflate};
struct NotificationPcmResult {
    NotificationPcmError error{};ZlibInflateError inflate_error{};
    bool ok() const noexcept {return error==NotificationPcmError::none;}
};
// Sealed native asset filter, not FLAC. A zlib stream expands to exactly
// frames*4 bytes: blocks<=256 frames, four byte planes [left low/high,side
// low/high], with zigzag16 residuals and predictor order0..2 modulo65536.
// Side=(right-left) modulo65536. Predictor history carries across blocks.
// Inflates once into the caller bank, then restores interleaved PCM16 stereo
// in place using only 1KiB transient scratch. No allocation, decoder or device.
// Disjoint input/output/workspace required. On inflate failure private output
// may be partially modified; caller must not use it or treat it as PCM.
NotificationPcmResult notification_pcm_unpack(ConstSlice<u8> packed,NotificationPcmSpec,
    Slice<u8> pcm,DeflateWorkspace*) noexcept;
}
