#pragma once
// Wraps PCM16 stereo frames in a clip with one reference. The shipping client only decodes clips from files;
// the block layout matches soundboard_clip_decode, so soundboard_clip_release frees it.
#include "maicord/app/soundboard_audio.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
inline SoundboardClip* soundboard_clip_from_pcm(u64 sound_id, ConstSlice<i16> stereo) noexcept {
    const u32 frames = stereo.count / 2;
    if (!stereo.data || !frames || frames > kSoundboardClipMaximumFrames)
        return nullptr;
    const usize header = (sizeof(SoundboardClip) + 63) & ~usize{63};
    const usize bytes = header + usize{frames} * 2 * sizeof(i16);
    auto* block = static_cast<u8*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!block)
        return nullptr;
    auto* clip = reinterpret_cast<SoundboardClip*>(block);
    clip->references = 1;
    clip->frames = frames;
    clip->allocation_bytes = static_cast<u32>(bytes);
    clip->sound_id = sound_id;
    clip->pcm = reinterpret_cast<i16*>(block + header);
    copy_bytes(clip->pcm, stereo.data, frames * 2 * static_cast<u32>(sizeof(i16)));
    return clip;
}
} // namespace maicord
