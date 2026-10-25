#pragma once
#include "maicord/ui/widget_storage.hpp"
#include "maicord/ui/scrollbar.hpp"

namespace maicord {
struct UiTextOutput;

// Inbox popout: Mentions (recent mentions with filters and Load more) and
// Unreads (unread channels grouped by server). Rows are borrowed from the host
// under revision and current fences, as in the pinned messages popout.
inline constexpr u32 kUiInboxMaximumRows = 400;
inline constexpr u32 kUiInboxVisibleRows = 24;
inline constexpr u32 kUiInboxWidgetCapacity = 12 + 2 * kUiInboxVisibleRows;
inline constexpr float kUiInboxPreferredWidth = 480.f;
inline constexpr float kUiInboxPreferredHeight = 600.f;
inline constexpr float kUiInboxGroupHeight = 40.f;
inline constexpr float kUiInboxChannelHeight = 36.f;
enum class UiInboxTab : u8 { mentions, unreads };
enum class UiInboxPhase : u8 { loading, ready, failed };
enum class UiInboxRowKind : u8 { mention, group, channel };
enum class UiInboxAction : u8 {
    none, close, tab_mentions, tab_unreads, filter_server, filter_everyone, filter_roles,
    jump, open_channel, mark_channel, mark_guild, load_more, retry
};
struct UiInboxTarget {
    UiInboxAction action{};
    u64 id{}, channel_id{};
};
constexpr bool ui_inbox_target_equal(UiInboxTarget a, UiInboxTarget b) noexcept {
    return a.action == b.action && a.id == b.id && a.channel_id == b.channel_id;
}
struct UiInboxRow {
    UiInboxRowKind kind{};
    // Mention: message and channel; group: guild; channel: channel and guild.
    u64 id{}, channel_id{}, guild_id{};
    Utf8View title{}, detail{}, body{}, timestamp{};
    u32 mention_count{};
    bool muted{};
    // Mention author identity for the card avatar; the hash is borrowed with the row.
    u64 author_id{};
    Utf8View avatar_hash{};
};
struct UiInboxSource {
    void* context{};
    u32 count{}, revision{};
    bool (*resolve)(void*, u32, UiInboxRow*) noexcept{};
    bool (*current)(void*) noexcept{};
};
struct UiInboxContent {
    Utf8View status{};
    u32 revision{};
    UiInboxTab tab{};
    UiInboxPhase phase{};
    // Mentions filters; this_server is offered only inside a server.
    bool this_server{}, this_server_available{}, everyone{true}, roles{true};
    bool has_more{}, loading_more{};
};
enum class UiInboxFocusMove : u8 { next, previous, first, last };
struct UiInboxScene {
    UiFixedWidgetStorageOwner storage;
    UiWidgetManager widgets;
    const UiInboxSource* source{};
    const UiInboxContent* content{};
    UiRect viewport{}, panel{}, header{}, filters{}, body{}, footer{};
    UiWidgetHandle root{}, body_owner{}, close{}, tabs[2]{}, chips[3]{}, more{};
    UiWidgetHandle rows[kUiInboxVisibleRows]{}, actions[kUiInboxVisibleRows]{};
    UiInboxRow visible[kUiInboxVisibleRows]{};
    UiRect row_rectangles[kUiInboxVisibleRows]{};
    UiInboxTarget targets[kUiInboxWidgetCapacity]{}, focused{};
    float offsets[kUiInboxMaximumRows + 1]{};
    float scroll_dips{};
    u32 first{}, count{}, measured_count{}, source_revision{}, content_revision{}, revision{};
    UiInboxTab tab{};
    bool active{};
    // Host-sampled UiSpinnerClock phase; part of the popout's paint stamp.
    u8 spinner_phase{};
    UiScrollbarVisualState scrollbar_state{};
};
// Viewport is the content area below the header; the panel hangs from its
// top right edge.
bool ui_inbox_scene_update(UiInboxScene*, const UiInboxSource*, const UiInboxContent*, UiRect viewport) noexcept;
void ui_inbox_scene_release(UiInboxScene*) noexcept;
bool ui_inbox_scene_current(const UiInboxScene&) noexcept;
bool ui_inbox_scene_contains(const UiInboxScene&, float x, float y) noexcept;
UiInboxTarget ui_inbox_scene_hit_test(const UiInboxScene&, float x, float y) noexcept;
UiInboxTarget ui_inbox_scene_focused_target(const UiInboxScene&) noexcept;
bool ui_inbox_scene_set_hover(UiInboxScene*, float x, float y, bool* changed) noexcept;
float ui_inbox_scene_scroll_maximum(const UiInboxScene&) noexcept;
bool ui_inbox_scene_scroll(UiInboxScene*, float offset) noexcept;
// The body's overlay scrollbar (always painted while the list overflows, so a
// long mention history shows its position). Invisible when not current.
UiScrollbarGeometry ui_inbox_scene_scrollbar(const UiInboxScene&) noexcept;
// Hover/pressed thumb presentation; returns true when the paint changed.
bool ui_inbox_scene_set_scrollbar_state(UiInboxScene*, UiScrollbarVisualState) noexcept;
bool ui_inbox_scene_focus(UiInboxScene*, UiInboxTarget) noexcept;
bool ui_inbox_scene_move_focus(UiInboxScene*, UiInboxFocusMove) noexcept;
// True while the popout paints its loading spinner (first page still loading).
bool ui_inbox_scene_spinner_visible(const UiInboxScene&) noexcept;
bool ui_inbox_scene_paint(const UiInboxScene&, const Theme&, PaintStream*) noexcept;
// Visible mention card avatar (slot < count) and its body clip; false when the
// slot is not a mention or the avatar is scrolled out.
bool ui_inbox_scene_avatar(const UiInboxScene&, u32 slot, UiRect* rectangle, UiRect* clip) noexcept;
inline constexpr float kUiInboxAvatarExtent = 32.f;
bool ui_inbox_scene_text_output(const UiInboxScene&, const Theme&, UiTextOutput*) noexcept;
// Unreads: a channel row's check button paints only while the row or the
// button is hovered or focused; it stays hit-testable and in the focus order.
bool ui_inbox_scene_channel_action_visible(const UiInboxScene&, u32 slot) noexcept;
} // namespace maicord
