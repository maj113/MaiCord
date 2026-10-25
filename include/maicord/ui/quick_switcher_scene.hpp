#pragma once
#include "maicord/ui/edit_field.hpp"
#include "maicord/ui/widget_storage.hpp"
#include "maicord/media/asset_key.hpp"

namespace maicord {
enum class UiPresence : u8;
struct ChatStateStore;
inline constexpr u32 kUiQuickSwitcherMaximumRows = 100;
inline constexpr u32 kUiQuickSwitcherVisibleRows = 12;
inline constexpr u32 kUiQuickSwitcherWidgetCapacity = 5 + kUiQuickSwitcherVisibleRows;
inline constexpr float kUiQuickSwitcherRowHeight = 56.f;
enum class UiQuickSwitcherKind : u8 { text_channel, voice_channel, private_channel, group_private, guild, user };
enum class UiQuickSwitcherPhase : u8 { empty, ready, no_matches, unavailable };
enum class UiQuickSwitcherAction : u8 { none, edit, activate, close, confirm };
struct UiQuickSwitcherRow {
    u64 key{}, guild_id{}, channel_id{}, user_id{};
    UiQuickSwitcherKind kind{};
    Utf8View label{}, detail{}, metadata{}, avatar_hash{};
    bool enabled{true};
    // People and DMs: the peer's status on the avatar, unknown for none;
    // mobile draws the phone status (online on mobile only).
    UiPresence presence{};
    bool mobile{};
};
struct UiQuickSwitcherSource {
    void* context{};
    u32 count{}, revision{};
    bool (*resolve)(void*, u32, UiQuickSwitcherRow*) noexcept{};
    bool (*current)(void*) noexcept{};
};
struct UiQuickSwitcherContent {
    UiEditView edit{};
    Utf8View heading{}, hint{}, status{};
    u32 revision{};
    UiQuickSwitcherPhase phase{};
    // People picker: rows toggle a checkbox for keys in checked, and the
    // footer carries the action button instead of keyboard hints.
    bool picker{}, can_confirm{};
    Utf8View action{};
    ConstSlice<u64> checked{};
};
struct UiQuickSwitcherTarget {
    UiQuickSwitcherAction action{};
    u64 key{}, guild_id{}, channel_id{}, user_id{};
    u32 index{}, source_revision{};
    UiQuickSwitcherKind kind{};
};
enum class UiQuickSwitcherMove : u8 { next, previous, first, last, page_next, page_previous };
struct UiQuickSwitcherScene {
    UiFixedWidgetStorageOwner storage;
    UiWidgetManager widgets;
    const UiQuickSwitcherSource* source{};
    const UiQuickSwitcherContent* content{};
    UiQuickSwitcherRow* accepted{};
    UiRect viewport{}, panel{}, field{}, text{}, body{}, footer{}, hint{}, confirm_rectangle{};
    UiWidgetHandle root{}, edit{}, close{}, body_owner{}, confirm{}, rows[kUiQuickSwitcherVisibleRows]{};
    UiRect row_rectangles[kUiQuickSwitcherVisibleRows]{};
    UiQuickSwitcherRow visible[kUiQuickSwitcherVisibleRows]{};
    u32 first{}, count{}, accepted_count{}, selected{~0u};
    u32 source_revision{}, content_revision{}, revision{};
    float scroll_dips{};
    bool active{};
};
// Borrowed descriptors/strings remain valid under revision + current fences.
// Native editor handles stay stable while query results are republished.
bool ui_quick_switcher_scene_update(UiQuickSwitcherScene*, const UiQuickSwitcherSource*,
                                    const UiQuickSwitcherContent*, UiRect viewport) noexcept;
void ui_quick_switcher_scene_release(UiQuickSwitcherScene*) noexcept;
bool ui_quick_switcher_scene_current(const UiQuickSwitcherScene&) noexcept;
bool ui_quick_switcher_scene_contains(const UiQuickSwitcherScene&, float x, float y) noexcept;
UiQuickSwitcherTarget ui_quick_switcher_scene_target(const UiQuickSwitcherScene&, UiWidgetHandle) noexcept;
UiQuickSwitcherTarget ui_quick_switcher_scene_hit_test(const UiQuickSwitcherScene&, float x, float y) noexcept;
UiQuickSwitcherTarget ui_quick_switcher_scene_selected(const UiQuickSwitcherScene&) noexcept;
bool ui_quick_switcher_scene_select(UiQuickSwitcherScene*, u32 index, bool reveal = true) noexcept;
bool ui_quick_switcher_scene_move(UiQuickSwitcherScene*, UiQuickSwitcherMove) noexcept;
bool ui_quick_switcher_scene_set_hover(UiQuickSwitcherScene*, float x, float y, bool* changed) noexcept;
float ui_quick_switcher_scene_scroll_maximum(const UiQuickSwitcherScene&) noexcept;
bool ui_quick_switcher_scene_scroll(UiQuickSwitcherScene*, float offset) noexcept;
bool ui_quick_switcher_scene_paint(const UiQuickSwitcherScene&, const Theme&, PaintStream*,
                                  const UiEditGeometry* = nullptr, u32 dpi = 96) noexcept;
bool ui_quick_switcher_scene_text_output(const UiQuickSwitcherScene&, const Theme&, UiTextOutput*,
                                        const UiEditGeometry* = nullptr) noexcept;
// Identity image of a visible row: the user's avatar for people and direct
// messages (the DM picker too), the server icon for servers. It covers the
// kind glyph, which stays as the placeholder while loading or without an image.
// hash borrows the row (avatar) or the state store (icon).
struct UiQuickSwitcherMedia {
    MediaAssetKey key;
    Utf8View hash;
    UiRect rectangle, clip;
};
bool ui_quick_switcher_scene_media(const UiQuickSwitcherScene&, u32 slot, const ChatStateStore* state,
                                   UiQuickSwitcherMedia*) noexcept;
// Above the row avatars (drawn after their media): status dots of people and
// direct messages at the avatar's lower right, as the DM list draws them.
UiRect ui_quick_switcher_scene_status_rectangle(const UiQuickSwitcherScene&, u32 slot) noexcept;
bool ui_quick_switcher_scene_presence_paint(const UiQuickSwitcherScene&, const Theme&, PaintStream*) noexcept;
} // namespace maicord
