#pragma once

#include "maicord/core/arena.hpp"
#include "maicord/ui/geometry.hpp"

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

// Selection dragging normally stays within one message draw command, so one
// native geometry snapshot is kept. Storage is allocated on first use and
// survives invalidation; it does not grow with message history or selection length.
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

// The emoji under a point in the body text of a message (its content, reply
// preview, embeds and text components; not author lines, captions or
// reactions). A Unicode emoji is a shaping cluster inside a run the text layer
// draws in the emoji font; custom emoji and regional flags are their
// semantics. Code, hidden spoilers and points beside the glyph miss. The
// rectangle is the emoji's cell on its line in DIPs, clipped to the command.
// Text without emoji is rejected before any layout; otherwise this shares the
// selection's single-command geometry cache.
enum class MessageEmojiHitKind : u8 {
    none,
    unicode,  // byte_offset/byte_count are the glyph in the command's text
    semantic, // semantic_index names a custom emoji or regional flag
};
struct MessageEmojiHit {
    u64 message_id;
    UiRect rectangle;
    u32 message_revision;
    u32 command_index;
    u32 byte_offset;
    u32 byte_count;
    u32 semantic_index;
    MessageEmojiHitKind kind;
};
MessageEmojiHit message_emoji_hit_test(MessageHitTestCacheOwner* owner, const MessageTextPresentation& presentation,
                                       const TextSystem& text_system, float x, float y) noexcept;

static_assert(sizeof(MessagePositionHit) == 40);
static_assert(sizeof(MessageHitTestCacheOwner) == 120);

} // namespace maicord
