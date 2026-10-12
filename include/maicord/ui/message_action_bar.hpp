#pragma once

#include "maicord/ui/message_context_menu.hpp"
#include "maicord/ui/message_scene.hpp"

namespace maicord {

inline constexpr u32 kMessageActionBarQuickReactionCount = 3;
// Collapsed bar: three quick reactions, Add reaction, Reply or Edit, More,
// plus Forward when available. Shift expansion and failed sends differ.
inline constexpr u32 kMessageActionBarItemCount = 6;
inline constexpr u32 kMessageActionBarMaximumItems = kMessageActionBarItemCount + 1;
// Nine host items (Shift expansion) plus up to two plugin buttons.
inline constexpr u32 kMessageActionBarPluginItems = 2;
inline constexpr u32 kMessageActionBarCapacity = 9 + kMessageActionBarPluginItems;
inline constexpr float kMessageActionBarCellExtent = 28.0f;
inline constexpr float kMessageActionBarSectionGap = 8.0f;
inline constexpr float kMessageActionBarPanelPadding = 2.0f;
inline constexpr float kMessageActionBarRowOverhang = 14.0f;
inline constexpr float kMessageActionBarViewportMargin = 8.0f;
inline constexpr float kMessageActionBarRowRightInset = 16.0f;
inline constexpr u32 kMessageActionBarNoItem = ~0u;
// Timeline overlays drawn above the toolbar (the unread banner at the top,
// the jump-to-present pill at the bottom). The toolbar lives inside the
// timeline viewport: it is clipped there and slides under them.
inline constexpr u32 kMessageActionBarOccluderCount = 2;

enum class MessageActionBarAction : u8 {
    none,
    quick_reaction_0,
    quick_reaction_1,
    quick_reaction_2,
    add_reaction,
    reply,
    more,
    forward,
    edit,
    copy_link,
    mark_unread,
    pin,
    unpin,
    delete_message,
    copy_id,
    retry,
    // Plugin buttons (mc.ui.slots MESSAGE_HOVER); catalog_index is the slot index.
    plugin_0,
    plugin_1,
};

enum class MessageActionBarItemKind : u8 {
    quick_reaction,
    add_reaction,
    reply,
    more,
    forward,
    edit,
    copy_link,
    mark_unread,
    pin,
    unpin,
    delete_message,
    copy_id,
    retry,
    plugin,
};

// Host authority for one message, revalidated on every hover transition.
enum MessageActionBarCapability : u16 {
    message_action_bar_forward = 1u << 0,
    message_action_bar_edit = 1u << 1,
    message_action_bar_expanded = 1u << 2,
    message_action_bar_pin = 1u << 3,
    message_action_bar_delete = 1u << 4,
    message_action_bar_copy_id = 1u << 5,
    message_action_bar_mark_unread = 1u << 6,
};

struct MessageActionBarOptions {
    u16 capabilities;
};

enum class MessageActionBarError : u8 {
    none,
    invalid_input,
    unsupported_message,
    quick_reactions,
    layout,
    scene,
};

struct MessageActionBarItem {
    UiRect rectangle;
    Utf8View glyph;
    u32 catalog_index;
    MessageActionBarAction action;
    MessageActionBarItemKind kind;
};

struct MessageActionBarResult {
    MessageActionBarError error;
    bool changed;

    constexpr bool ok() const noexcept {
        return error == MessageActionBarError::none;
    }
};

struct MessageActionBarHit {
    u64 message_id;
    Utf8View glyph;
    UiRect rectangle;
    u32 catalog_index;
    u32 item_index;
    MessageActionBarAction action;
    bool inside;
};

struct MessageActionBarPointerResult {
    MessageActionBarHit action_bar;
    u64 hovered_message_id;
    MessageActionBarError error;
    bool changed;

    constexpr bool ok() const noexcept {
        return error == MessageActionBarError::none;
    }
};

struct MessageActionBar {
    MessageActionBarItem items[kMessageActionBarCapacity];
    UiWidgetReference owner;
    UiRect rectangle;
    // Visible region: the timeline viewport minus the overlays above the bar.
    // Paint, glyphs, hit testing and tooltips all use rectangle & clip.
    UiRect clip;
    UiRect viewport;
    UiRect occluders[kMessageActionBarOccluderCount];
    u64 message_id;
    u32 revision;
    u32 preference_revision;
    u32 opens;
    u32 closes;
    u32 rejected;
    u32 hovered_index;
    u16 capabilities;
    // Items before this index are quick reactions followed by a section gap.
    u8 section_end;
    u8 item_count;
    bool active;
};

static_assert(sizeof(MessageActionBarItem) == 40);
static_assert(sizeof(MessageActionBar) == 576);
using MessageActionBarForwardPolicy = bool (*)(void*, u64 message_id) noexcept;
using MessageActionBarPolicy = u16 (*)(void*, u64 message_id) noexcept;

// Pending sends never get a bar; failed sends show only Retry and Delete.
MessageActionBarResult message_action_bar_open(
    MessageActionBar* bar, const MessageScene& scene, MessageSceneHit hit, const MessageRecord& message,
    const ReactionPreferenceStore* preferences, MessageActionBarOptions options) noexcept;
MessageActionBarPointerResult message_action_bar_track_pointer(
    MessageActionBar* bar, MessageScene* scene, float x, float y, const ReactionPreferenceStore* preferences,
    MessageActionBarPolicy policy, void* policy_context = nullptr) noexcept;
// Reopens the current bar with new capabilities (for example a Shift change).
bool message_action_bar_reconfigure(MessageActionBar* bar, const MessageScene& scene,
                                    const ReactionPreferenceStore* preferences,
                                    MessageActionBarOptions options, bool* changed = nullptr) noexcept;

MessageActionBarResult message_action_bar_open(
    MessageActionBar* bar, const MessageScene& scene,
    MessageSceneHit hit, const MessageRecord& message,
    const ReactionPreferenceStore* preferences = nullptr, bool can_forward = false) noexcept;
bool message_action_bar_synchronize(MessageActionBar* bar,
                                    const MessageScene& scene,
                                    bool* changed = nullptr) noexcept;
bool message_action_bar_close(MessageActionBar* bar,
                              bool* changed = nullptr) noexcept;
MessageActionBarHit message_action_bar_hit_test(const MessageActionBar& bar,
                                                float x, float y) noexcept;
// rectangle & clip; empty when the bar is closed or fully covered.
UiRect message_action_bar_visible_rectangle(const MessageActionBar& bar) noexcept;
// Publishes the overlays covering the timeline this frame (empty rectangles
// for hidden ones). Recomputes the clip of an open bar and closes it when an
// overlay covers it entirely. Retained across open/close transitions.
bool message_action_bar_set_occluders(MessageActionBar* bar, const UiRect* occluders, u32 count,
                                      bool* changed = nullptr) noexcept;

// Hides the hover toolbar and the row's hover highlight as soon as the timeline
// scrolls (wheel, keys, scrollbar drag or a programmatic jump); only fresh
// pointer movement brings them back, never content moving under a stationary
// pointer.
struct MessageHoverScrollGate {
    double scroll;  // deliberate timeline offset (scroll units minus anchoring origin)
    float pointer_x, pointer_y;
    u32 generation;  // timeline reset generation
    bool scroll_known, pointer_known, suppressed;
};
// Records the timeline position; true only when this call newly suppressed
// hover (the host then closes the bar and clears the hovered row).
bool message_hover_scroll_gate_observe(MessageHoverScrollGate* gate, u32 generation, double scroll) noexcept;
// Records a pointer sample; true when hover may be evaluated for it.
bool message_hover_scroll_gate_pointer(MessageHoverScrollGate* gate, float x, float y) noexcept;
bool message_action_bar_set_hover(MessageActionBar* bar, float x, float y,
                                  bool* changed) noexcept;
MessageActionBarPointerResult message_action_bar_track_pointer(
    MessageActionBar* bar, MessageScene* scene, float x, float y,
    const ReactionPreferenceStore* preferences = nullptr,
    MessageActionBarForwardPolicy forward_policy = nullptr, void* policy_context = nullptr) noexcept;

} // namespace maicord
