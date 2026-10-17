#pragma once

#include "maicord/core/base.hpp"

namespace maicord {
struct MediaAtlasHandle;

inline constexpr u32 kPresentationDamageMediaCapacity = 4'096;
inline constexpr u32 kPresentationDamageMediaDomainCount = 2;
inline constexpr u32 kPresentationDamageMediaBitCount =
    kPresentationDamageMediaCapacity * kPresentationDamageMediaDomainCount;
inline constexpr u32 kPresentationDamageMediaWordCapacity =
    (kPresentationDamageMediaBitCount + 63u) / 64u;

struct PresentationDamageRect {
    float left;
    float top;
    float right;
    float bottom;
};

struct PresentationDamage {
    PresentationDamageRect rectangle;
    bool valid;
    bool full;
};

// Tracks mutations independently of scene construction. Media producers only
// publish a generation-safe atlas handle; retained presentation later maps
// changed visible handles to destination rectangles. Damage remains pending
// until a successful Present/Present1 commits it.
struct PresentationDamageTracker {
    u64 dirty_media[kPresentationDamageMediaWordCapacity];
    u32 dirty_media_count;
    u16 first_dirty_word;
    u16 last_dirty_word;
    PresentationDamageRect pending;
    bool pending_valid;
    bool pending_full;
};

void presentation_damage_reset(PresentationDamageTracker* tracker) noexcept;
bool presentation_damage_note_media(PresentationDamageTracker* tracker,
                                    MediaAtlasHandle handle,
                                    u8 domain = 0) noexcept;
bool presentation_damage_media_pending(const PresentationDamageTracker& tracker,
                                       MediaAtlasHandle handle,
                                       u8 domain = 0) noexcept;
bool presentation_damage_add(PresentationDamageTracker* tracker,
                             PresentationDamageRect rectangle,
                             PresentationDamageRect clip) noexcept;
bool presentation_damage_add_media(PresentationDamageTracker* tracker,
                                   MediaAtlasHandle handle,
                                   PresentationDamageRect rectangle,
                                   PresentationDamageRect clip,
                                   u8 domain = 0) noexcept;
void presentation_damage_invalidate_full(PresentationDamageTracker* tracker) noexcept;
PresentationDamage presentation_damage_current(
    const PresentationDamageTracker& tracker, float viewport_width,
    float viewport_height) noexcept;
bool presentation_damage_empty(const PresentationDamageTracker& tracker) noexcept;
void presentation_damage_commit(PresentationDamageTracker* tracker) noexcept;

static_assert(sizeof(PresentationDamageTracker) < 2 * 1024);

} // namespace maicord
