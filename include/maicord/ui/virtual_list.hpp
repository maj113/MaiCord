#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

inline constexpr u32 kUiExtentUnitsPerDip = 64;
inline constexpr u32 kMaximumUiItemExtentUnits = 1'048'576u * kUiExtentUnitsPerDip;
inline constexpr u32 kMaximumUiVariableItems = 1'048'576;

struct UiScrollAnchor {
    u32 index;
    u32 within_item_units;
    u16 generation;
    bool valid;
};

struct UiVariableRange {
    u32 first;
    u32 count;
    i64 first_offset_units;
};

struct UiVariableList {
    u32* item_units;
    u64* fenwick;
    u32 count;
    u32 capacity;
    u32 rejected;
    u32 height_updates;
    u32 structural_updates;
    u64 total_units;
    u16 generation;
};

u32 ui_extent_units(float dips) noexcept;
float ui_extent_dips(u64 units) noexcept;
float ui_signed_extent_dips(i64 units) noexcept;

bool ui_variable_list_initialize(UiVariableList* list, Slice<u32> item_units,
                                 Slice<u64> fenwick, u32 item_count,
                                 u32 default_item_units) noexcept;
u32 ui_variable_list_item_units(const UiVariableList& list, u32 index) noexcept;
u64 ui_variable_list_prefix_units(const UiVariableList& list,
                                  u32 exclusive_index) noexcept;
// Nearest-edge reveal in exact fixed-point units, including very long lists.
bool ui_scroll_reveal_units(u64 first, u64 extent, u64 total, u64 scroll_units,
                            u64 viewport_units, u64* result) noexcept;
bool ui_variable_list_reveal(const UiVariableList& list, u32 index, u64 scroll_units,
                             u64 viewport_units, u64* result) noexcept;
bool ui_variable_list_set_height(UiVariableList* list, u32 index,
                                 u32 item_units) noexcept;
bool ui_variable_list_capture_anchor(const UiVariableList& list, u64 scroll_units,
                                     UiScrollAnchor* anchor) noexcept;
bool ui_variable_list_restore_anchor(const UiVariableList& list,
                                     const UiScrollAnchor& anchor,
                                     u64* scroll_units) noexcept;
UiVariableRange ui_variable_list_range(const UiVariableList& list, u64 scroll_units,
                                       u64 viewport_units, u32 overscan) noexcept;
// Pixel-distance lookahead; returned offsets remain relative to the real viewport.
UiVariableRange ui_variable_list_buffered_range(const UiVariableList& list, u64 scroll_units,
                                                u64 viewport_units, u64 margin_units) noexcept;
bool ui_variable_list_prepend(UiVariableList* list, u32 item_count,
                              u32 default_item_units,
                              UiScrollAnchor* anchor = nullptr) noexcept;
bool ui_variable_list_remove_prefix(UiVariableList* list, u32 item_count,
                                    UiScrollAnchor* anchor = nullptr) noexcept;
bool ui_variable_list_insert(UiVariableList* list, u32 index, u32 item_count,
                             u32 default_item_units,
                             UiScrollAnchor* anchor = nullptr) noexcept;
bool ui_variable_list_remove(UiVariableList* list, u32 index, u32 item_count,
                             UiScrollAnchor* anchor = nullptr) noexcept;

} // namespace maicord
