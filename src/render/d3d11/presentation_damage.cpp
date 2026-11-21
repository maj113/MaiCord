#include "maicord/render/d3d11/presentation_damage.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/render/media/atlas.hpp"
#include "maicord/ui/core/geometry.hpp"

namespace maicord {
namespace {

inline bool rect_valid(PresentationDamageRect rectangle) noexcept {
    return ui_coordinate_valid(rectangle.left) & ui_coordinate_valid(rectangle.top) &
           ui_coordinate_valid(rectangle.right) & ui_coordinate_valid(rectangle.bottom);
}

inline PresentationDamageRect intersect(PresentationDamageRect left,
                                 PresentationDamageRect right) noexcept {
    return PresentationDamageRect{
        maximum(left.left, right.left), maximum(left.top, right.top),
        minimum(left.right, right.right), minimum(left.bottom, right.bottom)};
}

inline bool has_area(PresentationDamageRect rectangle) noexcept {
    return rectangle.left < rectangle.right && rectangle.top < rectangle.bottom;
}

inline u32 media_bit(MediaAtlasHandle handle, u8 domain) noexcept {
    return static_cast<u32>(domain) * kPresentationDamageMediaCapacity +
           handle.slot;
}

} // namespace

void presentation_damage_reset(PresentationDamageTracker* tracker) noexcept {
    reset_value(tracker);
    tracker->first_dirty_word = static_cast<u16>(kPresentationDamageMediaWordCapacity);
}

bool presentation_damage_note_media(PresentationDamageTracker* tracker,
                                    MediaAtlasHandle handle,
                                    u8 domain) noexcept {
    if (handle.generation == 0 ||
        handle.slot >= kPresentationDamageMediaCapacity ||
        domain >= kPresentationDamageMediaDomainCount) {
        return false;
    }
    const u32 bit = media_bit(handle, domain);
    const u32 word = bit / 64u;
    const u64 mask = 1ull << (bit & 63u);
    if ((tracker->dirty_media[word] & mask) == 0) {
        tracker->dirty_media[word] |= mask;
        ++tracker->dirty_media_count;
        tracker->first_dirty_word = static_cast<u16>(
            minimum<u32>(tracker->first_dirty_word, word));
        tracker->last_dirty_word =
            static_cast<u16>(maximum<u32>(tracker->last_dirty_word, word));
    }
    return true;
}

bool presentation_damage_media_pending(const PresentationDamageTracker& tracker,
                                       MediaAtlasHandle handle,
                                       u8 domain) noexcept {
    if (handle.generation == 0 ||
        handle.slot >= kPresentationDamageMediaCapacity ||
        domain >= kPresentationDamageMediaDomainCount) {
        return false;
    }
    const u32 bit = media_bit(handle, domain);
    return (tracker.dirty_media[bit / 64u] & (1ull << (bit & 63u))) != 0;
}

bool presentation_damage_add(PresentationDamageTracker* tracker,
                             PresentationDamageRect rectangle,
                             PresentationDamageRect clip) noexcept {
    if (!rect_valid(rectangle) || !rect_valid(clip)) {
        return false;
    }
    rectangle = intersect(rectangle, clip);
    if (!has_area(rectangle)) {
        return true;
    }
    if (!tracker->pending_valid) {
        tracker->pending = rectangle;
        tracker->pending_valid = true;
        return true;
    }
    tracker->pending.left = minimum(tracker->pending.left, rectangle.left);
    tracker->pending.top = minimum(tracker->pending.top, rectangle.top);
    tracker->pending.right = maximum(tracker->pending.right, rectangle.right);
    tracker->pending.bottom = maximum(tracker->pending.bottom, rectangle.bottom);
    return true;
}

bool presentation_damage_add_media(PresentationDamageTracker* tracker,
                                   MediaAtlasHandle handle,
                                   PresentationDamageRect rectangle,
                                   PresentationDamageRect clip,
                                   u8 domain) noexcept {
    if (!presentation_damage_media_pending(*tracker, handle, domain)) {
        return true;
    }
    return presentation_damage_add(tracker, rectangle, clip);
}

void presentation_damage_invalidate_full(PresentationDamageTracker* tracker) noexcept {
    tracker->pending_full = true;
}

PresentationDamage presentation_damage_current(
    const PresentationDamageTracker& tracker, float viewport_width,
    float viewport_height) noexcept {
    if (!ui_size_valid({viewport_width, viewport_height})) {
        return PresentationDamage{};
    }
    const PresentationDamageRect viewport{0.0f, 0.0f, viewport_width,
                                          viewport_height};
    if (tracker.pending_full) {
        return PresentationDamage{viewport, true, true};
    }
    if (!tracker.pending_valid) {
        return PresentationDamage{};
    }
    const PresentationDamageRect clipped = intersect(tracker.pending, viewport);
    return PresentationDamage{clipped, has_area(clipped), false};
}

bool presentation_damage_empty(const PresentationDamageTracker& tracker) noexcept {
    return !tracker.pending_full && !tracker.pending_valid &&
           tracker.dirty_media_count == 0;
}

void presentation_damage_commit(PresentationDamageTracker* tracker) noexcept {
    if (tracker->dirty_media_count != 0) {
        for (u32 index = tracker->first_dirty_word;
             index <= tracker->last_dirty_word; ++index) {
            tracker->dirty_media[index] = 0;
        }
        tracker->dirty_media_count = 0;
        tracker->first_dirty_word =
            static_cast<u16>(kPresentationDamageMediaWordCapacity);
        tracker->last_dirty_word = 0;
    }
    tracker->pending_valid = false;
    tracker->pending_full = false;
}

} // namespace maicord
