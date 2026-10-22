#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/core/stable_storage.hpp"
#include "maicord/text/paragraph_hit_metric.hpp"
#include "maicord/ui/text_style.hpp"

namespace maicord {

struct MessageComposerScene;
struct TextSystem;
struct UiRect;

inline constexpr u32 kTextEditTextCapacity = 16 * 1'024;
inline constexpr u32 kTextEditSelectionCapacity = 48;

// Native queries publish directly into the retained editor rectangle buffer.
using TextEditMetric = NativeParagraphHitMetric;

enum TextEditLayoutOption : u8 {
    text_edit_layout_none = 0,
    text_edit_layout_wrap = 1u << 0,
    text_edit_layout_center_vertically = 1u << 1,
    text_edit_layout_scroll_to_caret = 1u << 2,
};

struct TextEditLayoutInput {
    Utf8View text;
    u64 text_signature;
    u32 anchor_utf16;
    u32 caret_utf16;
    float origin_x;
    float viewport_top;
    float width;
    float height;
    float maximum_layout_height;
    u8 options;
    ConstSlice<TextStyleSpan> styles;
};

// Owns retained native edit geometry for one text field: font-independent hit
// clusters/lines across the full edit-text capacity, kept in `storage`.
// Callers retain their own model/storage and only provide an immutable display
// projection, so secure login edits and rich message edits can share this path.
struct TextEditLayoutOwner {
    u64 format;
    u64 text_signature;
    u64 style_signature;
    // Presentation rectangles. Large selections may be clipped and merged;
    // model anchor/caret remain the authority for the logical text range.
    TextEditMetric* selections;
    TextEditMetric caret;
    StableStorage storage;
    u32 selection_capacity;
    u32 text_bytes;
    u32 anchor;
    u32 caret_offset;
    u32 utf16_units;
    u32 selection_count;
    u32 builds;
    u32 hits;
    u32 rejected;
    float origin_x;
    float origin_y;
    float width;
    float height;
    float viewport_top;
    float layout_height;
    float scroll_y;
    float scroll_x;
    float viewport_left;
    float em_size;
    float maximum_layout_height;
    bool active;
    u8 options;
    bool manual_scroll;
    bool geometry; // storage holds the current text's hit geometry
};

u64 text_edit_signature(Utf8View text) noexcept;
bool text_edit_layout_measure(TextEditLayoutOwner* owner, const TextEditLayoutInput& input,
                              const TextSystem& text_system) noexcept;
bool text_edit_layout_hit_test(const TextEditLayoutOwner& owner, float x, float y,
                               u32* utf16_position) noexcept;
// Also supplies the containing cluster for TSF queries without rounding.
bool text_edit_layout_point_geometry(const TextEditLayoutOwner&, float x, float y, bool round_nearest,
                                     u32* utf16_position, bool* inside) noexcept;
bool text_edit_layout_move_vertical(const TextEditLayoutOwner& owner, bool down,
                                    u32* utf16_position) noexcept;
bool text_edit_layout_range_geometry(const TextEditLayoutOwner& owner, u32 first, u32 last,
                                     Slice<TextEditMetric> output, u32* count) noexcept;
bool text_edit_layout_position_geometry(const TextEditLayoutOwner&, u32 position, TextEditMetric*) noexcept;
bool text_edit_layout_raw_range_geometry(const TextEditLayoutOwner&, u32 first, u32 last,
                                         Slice<TextEditMetric>, u32* count) noexcept;
u32 text_edit_layout_line_count(const TextEditLayoutOwner&) noexcept;
bool text_edit_layout_scroll(TextEditLayoutOwner* owner, float delta) noexcept;
void text_edit_layout_invalidate(TextEditLayoutOwner* owner) noexcept;
void text_edit_layout_release(TextEditLayoutOwner* owner) noexcept;

// Checks retained geometry against the current scene/model projection without
// measuring or touching the native text cache. Suitable for reentrant TSF queries.
bool text_composer_layout_current(const TextEditLayoutOwner& owner,
                                  const MessageComposerScene& scene) noexcept;
bool text_composer_layout_current(const TextEditLayoutOwner& owner, const MessageComposerScene& scene,
                                  const TextSystem& system) noexcept;
// Read-only font fence for TSF/plain editors; preserves selection/manual scroll.
bool text_edit_layout_current_font(const TextEditLayoutOwner&, const TextSystem&) noexcept;
bool text_composer_layout_measure(TextEditLayoutOwner* owner, const MessageComposerScene* scene,
                                  const TextSystem& text_system) noexcept;
bool text_composer_emoji_rectangle(const TextEditLayoutOwner& owner, const MessageComposerScene& scene,
                                   u32 index, UiRect* rectangle, UiRect* clip) noexcept;

static_assert(sizeof(TextEditLayoutOwner) <= 224);

} // namespace maicord
