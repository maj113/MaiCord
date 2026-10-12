#pragma once

#include "maicord/core/arena.hpp"

namespace maicord {

struct MessageTextPresentation;
struct TextSystem;

inline constexpr u32 kMessageHitTestTextCapacity = 4 * 1'024;

struct MessagePositionHit {
    u64 message_id;
    u64 command_identity;
    u32 message_revision;
    u32 command_index;
    u32 byte_offset;
    u32 text_bytes;
    bool valid;
};

// Selection dragging normally stays within one message draw command. Keeps one
// native geometry snapshot; storage is allocated on first use and kept across
// invalidation. The size is fixed: it does not grow with message history or
// selection length.
struct MessageHitTestCacheOwner {
    u64 native_format;
    u64 message_id;
    u64 measurement_identity;
    Arena storage;
    u32 message_revision;
    u32 command_index;
    u32 utf16_units;
    u32 builds;
    u32 hits;
    u32 rejected;
    float width;
    float height;
    float em_size;
    u8 horizontal_alignment;
    u8 vertical_alignment;
    u8 wrapping;
    bool geometry; // storage holds the current command's hit geometry
};

MessagePositionHit message_position_hit_test(
    MessageHitTestCacheOwner* owner, const MessageTextPresentation& presentation,
    const TextSystem& text_system, u32 dpi, i32 client_x_pixels, i32 client_y_pixels) noexcept;
void message_hit_test_cache_invalidate(MessageHitTestCacheOwner* owner) noexcept;
void message_hit_test_cache_release(MessageHitTestCacheOwner* owner) noexcept;

static_assert(sizeof(MessagePositionHit) == 40);
static_assert(sizeof(MessageHitTestCacheOwner) == 120);

} // namespace maicord
