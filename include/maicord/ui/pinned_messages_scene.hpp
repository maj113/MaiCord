#pragma once
#include "maicord/ui/widget_storage.hpp"
#include "maicord/ui/scrollbar.hpp"
#include "maicord/ui/text_presentation.hpp"

namespace maicord {
inline constexpr u32 kUiPinnedMessagesPageCapacity = 50;
inline constexpr u32 kUiPinnedMessagesVisibleRows = 16;
inline constexpr u32 kUiPinnedMessagesWidgetCapacity = 6 + 2 * kUiPinnedMessagesVisibleRows;
inline constexpr float kUiPinnedMessagesPreferredWidth = 420.f;
inline constexpr float kUiPinnedMessagesHeaderHeight = 64.f;
inline constexpr float kUiPinnedMessagesFooterHeight = 52.f;
enum class UiPinnedMessagesPhase : u8 { empty, loading, ready, failed };
enum class UiPinnedMessagesAction : u8 { none, close, refresh, jump, unpin, previous, next, retry };
struct UiPinnedMessagesTarget {
    UiPinnedMessagesAction action{};
    u64 channel_id{}, message_id{};
};
constexpr bool ui_pinned_messages_target_equal(UiPinnedMessagesTarget a, UiPinnedMessagesTarget b) noexcept {
    return a.action == b.action && a.channel_id == b.channel_id && a.message_id == b.message_id;
}
struct UiPinnedMessagesRow {
    u64 channel_id{}, message_id{}, author_id{};
    Utf8View author{}, timestamp{}, body{}, detail{}, avatar_hash{};
    // Owner-prepared excerpt height: zero uses 48 DIP, otherwise [24,144].
    // Measure once when publishing a page, not on pointer or paint events.
    float body_height{};
    bool can_unpin{}, unpin_pending{};
    // Retained stale previews may remain readable while their replacement loads.
    // The host disables navigation until that page is authoritative again.
    bool jump_enabled{true};
};
struct UiPinnedMessagesSource {
    void* context{};
    u32 count{}, revision{};
    bool (*resolve)(void*, u32, UiPinnedMessagesRow*) noexcept{};
    bool (*current)(void*) noexcept{};
};
struct UiPinnedMessagesContent {
    Utf8View channel{}, status{};
    u64 channel_id{};
    u32 revision{};
    UiPinnedMessagesPhase phase{};
    bool has_previous{}, has_next{}, loading_more{}, mutation_failed{};
    u32 page_index{};
};
enum class UiPinnedMessagesFocusMove : u8 { next, previous, first, last, page_previous, page_next };
struct UiPinnedMessagesScene {
    UiFixedWidgetStorageOwner storage;
    UiWidgetManager widgets;
    const UiPinnedMessagesSource* source{};
    const UiPinnedMessagesContent* content{};
    UiRect viewport{}, panel{}, header{}, body{}, footer{}, status_rectangle{};
    UiWidgetHandle root{}, body_owner{}, close{}, refresh{}, previous{}, next{}, retry{};
    UiWidgetHandle rows[kUiPinnedMessagesVisibleRows]{}, unpin[kUiPinnedMessagesVisibleRows]{};
    UiPinnedMessagesRow visible[kUiPinnedMessagesVisibleRows]{};
    UiRect cards[kUiPinnedMessagesVisibleRows]{};
    UiPinnedMessagesTarget targets[kUiPinnedMessagesWidgetCapacity]{}, focused{};
    float offsets[kUiPinnedMessagesPageCapacity + 1]{};
    float scroll_dips{};
    u64 channel_id{}, page_first_message_id{};
    u32 page_index{};
    u32 first{}, count{}, source_revision{}, content_revision{}, measured_count{}, revision{};
    bool active{}, overlay{};
    char page_text[24]{};
    u8 page_bytes{};
    // Host-sampled UiSpinnerClock phase; part of the panel's paint stamp.
    u8 spinner_phase{};
};
// Viewport is the available message/content area below the shell header.
// Source and content descriptors remain borrowed under revision/current fences.
bool ui_pinned_messages_scene_update(UiPinnedMessagesScene*, const UiPinnedMessagesSource*,
                                    const UiPinnedMessagesContent*, UiRect viewport) noexcept;
void ui_pinned_messages_scene_release(UiPinnedMessagesScene*) noexcept;
bool ui_pinned_messages_scene_current(const UiPinnedMessagesScene&) noexcept;
bool ui_pinned_messages_scene_contains(const UiPinnedMessagesScene&, float x, float y) noexcept;
UiPinnedMessagesTarget ui_pinned_messages_scene_hit_test(const UiPinnedMessagesScene&, float x, float y) noexcept;
UiPinnedMessagesTarget ui_pinned_messages_scene_target(const UiPinnedMessagesScene&, UiWidgetHandle) noexcept;
UiPinnedMessagesTarget ui_pinned_messages_scene_focused_target(const UiPinnedMessagesScene&) noexcept;
bool ui_pinned_messages_scene_set_hover(UiPinnedMessagesScene*, float x, float y, bool* changed) noexcept;
float ui_pinned_messages_scene_scroll_maximum(const UiPinnedMessagesScene&) noexcept;
bool ui_pinned_messages_scene_scroll(UiPinnedMessagesScene*, float offset) noexcept;
bool ui_pinned_messages_scene_focus(UiPinnedMessagesScene*, UiPinnedMessagesTarget) noexcept;
// Traverses every accepted page row, revealing offscreen actions with stable IDs.
bool ui_pinned_messages_scene_move_focus(UiPinnedMessagesScene*, UiPinnedMessagesFocusMove) noexcept;
bool ui_pinned_messages_scene_avatar(const UiPinnedMessagesScene&, u32 visible_slot,
                                    UiRect* rectangle, UiRect* clip) noexcept;
// True while the panel paints a loading spinner (initial page or a pending unpin).
bool ui_pinned_messages_scene_spinner_visible(const UiPinnedMessagesScene&) noexcept;
bool ui_pinned_messages_scene_paint(const UiPinnedMessagesScene&, const Theme&, PaintStream*, u32 dpi = 96) noexcept;
bool ui_pinned_messages_scene_text_output(const UiPinnedMessagesScene&, const Theme&, UiTextOutput*) noexcept;
} // namespace maicord
