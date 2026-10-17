#include "maicord/render/presentation_damage.hpp"
#include "maicord/render/media_atlas.hpp"

namespace maicord {
namespace {

bool coordinate_valid(float value) noexcept {
    return value == value && value >= -1'048'576.0f &&
           value <= 1'048'576.0f;
}

bool rect_valid(PresentationDamageRect rectangle) noexcept {
    return coordinate_valid(rectangle.left) &&
           coordinate_valid(rectangle.top) &&
           coordinate_valid(rectangle.right) &&
           coordinate_valid(rectangle.bottom);
}

PresentationDamageRect intersect(PresentationDamageRect left,
                                 PresentationDamageRect right) noexcept {
    return PresentationDamageRect{
        maximum(left.left, right.left), maximum(left.top, right.top),
        minimum(left.right, right.right), minimum(left.bottom, right.bottom)};
}

bool has_area(PresentationDamageRect rectangle) noexcept {
    return rectangle.left < rectangle.right && rectangle.top < rectangle.bottom;
}

u32 media_bit(MediaAtlasHandle handle, u8 domain) noexcept {
    return static_cast<u32>(domain) * kPresentationDamageMediaCapacity +
           handle.slot;
}

} // namespace

void presentation_damage_reset(PresentationDamageTracker* tracker) noexcept {
    if (!tracker) {
        return;
    }
    for (u32 index = 0; index < kPresentationDamageMediaWordCapacity;
         ++index) {
        tracker->dirty_media[index] = 0;
    }
    tracker->dirty_media_count = 0;
    tracker->first_dirty_word =
        static_cast<u16>(kPresentationDamageMediaWordCapacity);
    tracker->last_dirty_word = 0;
    tracker->pending = PresentationDamageRect{};
    tracker->pending_valid = false;
    tracker->pending_full = false;
}

bool presentation_damage_note_media(PresentationDamageTracker* tracker,
                                    MediaAtlasHandle handle,
                                    u8 domain) noexcept {
    if (!tracker || handle.generation == 0 ||
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
    if (!tracker || !rect_valid(rectangle) || !rect_valid(clip)) {
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
    if (!tracker ||
        !presentation_damage_media_pending(*tracker, handle, domain)) {
        return tracker != nullptr;
    }
    return presentation_damage_add(tracker, rectangle, clip);
}

void presentation_damage_invalidate_full(PresentationDamageTracker* tracker) noexcept {
    if (tracker) {
        tracker->pending_full = true;
    }
}

PresentationDamage presentation_damage_current(
    const PresentationDamageTracker& tracker, float viewport_width,
    float viewport_height) noexcept {
    if (!coordinate_valid(viewport_width) || !coordinate_valid(viewport_height) ||
        viewport_width <= 0.0f || viewport_height <= 0.0f) {
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
    if (!tracker) {
        return;
    }
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
    tracker->pending = PresentationDamageRect{};
    tracker->pending_valid = false;
    tracker->pending_full = false;
}

} // namespace maicord
