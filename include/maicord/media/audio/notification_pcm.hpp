#pragma once
#include "maicord/media/compression/deflate.hpp"

namespace maicord {
inline constexpr u32 kNotificationPcmMaximumFrames=10*48000;
struct NotificationPcmSpec {u32 frames{};u8 left_order{},side_order{};};
// Sealed native asset filter, not FLAC. A zlib stream expands to exactly
// frames*4 bytes: blocks<=256 frames, four byte planes [left low/high,side
// low/high], with zigzag16 residuals and predictor order0..2 modulo65536.
// Side=(right-left) modulo65536. Predictor history carries across blocks.
// Inflates once into the caller bank, then restores interleaved PCM16 stereo
// in place using only 1KiB transient scratch. No allocation, decoder or device.
// The caller (sound_assets.cpp) admits the asset: 1..kNotificationPcmMaximumFrames
// frames, orders 0..2, pcm.count frames*4, and disjoint input, output and
// workspace. False when the stream does not inflate to exactly that; the
// output may then be partially modified and is not PCM.
bool notification_pcm_unpack(ConstSlice<u8> packed,NotificationPcmSpec,
    Slice<u8> pcm,DeflateWorkspace*) noexcept;
}
