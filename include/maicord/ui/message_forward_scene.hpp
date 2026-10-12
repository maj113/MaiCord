#pragma once
#include "maicord/ui/quick_switcher_scene.hpp"

namespace maicord {
inline constexpr u32 kUiMessageForwardMaximumRows = 100;
inline constexpr u32 kUiMessageForwardMaximumTargets = 5;
inline constexpr u32 kUiMessageForwardVisibleRows = 10;
inline constexpr u32 kUiMessageForwardWidgetCapacity = 7 + kUiMessageForwardVisibleRows + kUiMessageForwardMaximumTargets;
inline constexpr float kUiMessageForwardRowHeight = 52.f;
using UiMessageForwardRow = UiQuickSwitcherRow;
using UiMessageForwardSource = UiQuickSwitcherSource;
enum class UiMessageForwardState : u8 { ready, sending, sent, failed, cancelled };
enum class UiMessageForwardPhase : u8 { ready, no_matches, unavailable };
enum class UiMessageForwardAction : u8 { none, edit, caption, toggle, remove, retry, send, close, cancel_pending };
struct UiMessageForwardSelection {
    u64 channel_id{};
    Utf8View label{}, detail{};
    UiMessageForwardState state{};
};
struct UiMessageForwardContent {
    UiEditView edit{}, caption{};
    Utf8View author{}, preview{}, media_summary{}, status{};
    UiMessageForwardSelection selected[kUiMessageForwardMaximumTargets]{};
    u32 selected_count{}, revision{};
    UiMessageForwardPhase phase{};
    bool can_send{}, sending{}, committed{};
};
struct UiMessageForwardTarget {
    UiMessageForwardAction action{};
    u64 channel_id{};
    u32 index{}, source_revision{};
};
struct UiMessageForwardScene {
    UiFixedWidgetStorageOwner storage;
    UiWidgetManager widgets;
    const UiMessageForwardSource* source{};
    const UiMessageForwardContent* content{};
    UiMessageForwardRow* accepted{};
    UiRect viewport{}, panel{}, preview{}, field{}, text{}, body{}, hint{}, receipts{},
        caption_field{}, caption_text{}, footer{}, send_rectangle{}, cancel_rectangle{};
    UiWidgetHandle root{}, edit{}, caption{}, close{}, send{}, cancel{}, body_owner{},
        rows[kUiMessageForwardVisibleRows]{}, targets[kUiMessageForwardMaximumTargets]{};
    UiRect row_rectangles[kUiMessageForwardVisibleRows]{}, target_rectangles[kUiMessageForwardMaximumTargets]{};
    UiMessageForwardRow visible[kUiMessageForwardVisibleRows]{};
    u32 first{}, count{}, accepted_count{}, selected{~0u}, source_revision{}, content_revision{}, revision{};
    float scroll_dips{};
    char send_label[32]{}, selection_label[48]{};
    u8 send_label_bytes{}, selection_label_bytes{};
    bool active{}, compact{}, receipts_visible{};
};
// Strings and descriptors are borrowed under revision + current authority fences.
// Selection, sending and retry eligibility belong to the host workflow, not this scene.
bool ui_message_forward_scene_update(UiMessageForwardScene*, const UiMessageForwardSource*,
    const UiMessageForwardContent*, UiRect viewport) noexcept;
void ui_message_forward_scene_release(UiMessageForwardScene*) noexcept;
bool ui_message_forward_scene_current(const UiMessageForwardScene&) noexcept;
bool ui_message_forward_scene_contains(const UiMessageForwardScene&, float x, float y) noexcept;
UiMessageForwardTarget ui_message_forward_scene_target(const UiMessageForwardScene&, UiWidgetHandle) noexcept;
UiMessageForwardTarget ui_message_forward_scene_hit_test(const UiMessageForwardScene&, float x, float y) noexcept;
UiMessageForwardTarget ui_message_forward_scene_selected(const UiMessageForwardScene&) noexcept;
bool ui_message_forward_scene_select(UiMessageForwardScene*, u32 index, bool reveal = true) noexcept;
bool ui_message_forward_scene_move(UiMessageForwardScene*, UiQuickSwitcherMove) noexcept;
bool ui_message_forward_scene_set_hover(UiMessageForwardScene*, float x, float y, bool* changed) noexcept;
float ui_message_forward_scene_scroll_maximum(const UiMessageForwardScene&) noexcept;
bool ui_message_forward_scene_scroll(UiMessageForwardScene*, float offset) noexcept;
bool ui_message_forward_scene_paint(const UiMessageForwardScene&, const Theme&, PaintStream*,
    const UiEditGeometry* query = nullptr, const UiEditGeometry* caption = nullptr, u32 dpi = 96) noexcept;
bool ui_message_forward_scene_text_output(const UiMessageForwardScene&, const Theme&, UiTextOutput*,
    const UiEditGeometry* query = nullptr, const UiEditGeometry* caption = nullptr) noexcept;
// Identity image of a visible destination tile: the server's icon for server
// channels (the rail's key, so it is shared and resident with the rail) and
// the person's avatar (a circle) for direct messages. It covers the tile's
// destination glyph, which stays while loading or without an image. hash
// borrows the row (avatar) or the state store (icon).
struct UiMessageForwardMedia {
    MediaAssetKey key;
    Utf8View hash;
    UiRect rectangle, clip;
    float radius;
    bool circle;
};
bool ui_message_forward_scene_media(const UiMessageForwardScene&, u32 slot, const ChatStateStore* state,
                                    UiMessageForwardMedia*) noexcept;
} // namespace maicord
