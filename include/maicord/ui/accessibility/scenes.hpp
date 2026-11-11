#pragma once

#include "maicord/ui/accessibility/accessibility.hpp"

// Accessibility projections of the overlay scenes. Each function adds one
// active scene beneath parent (usually the window root) and returns the scene's
// node, or kUiAccessNone when the scene is inactive or the snapshot is full.
// Rows and controls are named from the scene's own data; drawn text still
// reaches them through ui_access_attribute_text.
//
// UiAccessNode::item of rows, for host routing (scroll into view, actions):
//   quick switcher rows   source index + 1
//   search results        source index + 1 (rows of the current page)
//   pinned messages       message id
//   thread browser rows   thread id
//   inbox rows            source index + 1
//   media picker cells    source index + 1
//   call tiles            participant user id (stream tiles: stream tile id)
//   screen share sources  source index + 1
//   media page queue rows track number (one-based)
//   media page player     kUiAccessPlayerItem | the control id of the video or audio player

namespace maicord {

struct UiQuickSwitcherScene;
struct UiSearchScene;
struct UiPinnedMessagesScene;
struct UiThreadBrowserScene;
struct UiInboxScene;
struct UiCallScene;
struct ForumPostScene;
struct TextFormScene;
struct UiImageViewer;
struct UiMediaPickerScene;
struct UiGoLiveScene;
struct UiVideoScene;
struct UiAudioScene;

// UiAccessNode::item of a media page's player nodes: this bit, and the id of the UiVideoControl or UiAudioControl.
inline constexpr u64 kUiAccessPlayerItem = 1ull << 16;

u32 ui_access_add_quick_switcher(UiAccessTree* tree, u32 parent, const UiQuickSwitcherScene& scene,
                                 u8 surface) noexcept;
u32 ui_access_add_search(UiAccessTree* tree, u32 parent, const UiSearchScene& scene, u8 surface) noexcept;
u32 ui_access_add_pinned_messages(UiAccessTree* tree, u32 parent, const UiPinnedMessagesScene& scene,
                                  u8 surface) noexcept;
u32 ui_access_add_thread_browser(UiAccessTree* tree, u32 parent, const UiThreadBrowserScene& scene,
                                 u8 surface) noexcept;
u32 ui_access_add_inbox(UiAccessTree* tree, u32 parent, const UiInboxScene& scene, u8 surface) noexcept;
u32 ui_access_add_call(UiAccessTree* tree, u32 parent, const UiCallScene& scene, u8 surface) noexcept;
u32 ui_access_add_forum_post(UiAccessTree* tree, u32 parent, const ForumPostScene& scene, u8 surface) noexcept;
u32 ui_access_add_text_form(UiAccessTree* tree, u32 parent, const TextFormScene& scene, u8 surface) noexcept;
u32 ui_access_add_image_viewer(UiAccessTree* tree, u32 parent, const UiImageViewer& viewer, u8 surface) noexcept;
// A media page's hosted player (the viewer's page above carries its queue): the scene's controls as buttons, the
// seek and volume sliders, the time and the player's state, added beneath the page's node.
void ui_access_add_video_controls(UiAccessTree* tree, u32 parent, const UiVideoScene& scene, u8 surface) noexcept;
void ui_access_add_audio_controls(UiAccessTree* tree, u32 parent, const UiAudioScene& scene, u8 surface) noexcept;
u32 ui_access_add_media_picker(UiAccessTree* tree, u32 parent, const UiMediaPickerScene& scene,
                               u8 surface) noexcept;
u32 ui_access_add_go_live(UiAccessTree* tree, u32 parent, const UiGoLiveScene& scene, u8 surface) noexcept;

// Result count line ("12 results", "No matches", "Searching…") for status
// announcements; false when the scene has nothing to report.
bool ui_access_quick_switcher_summary(const UiQuickSwitcherScene& scene, UiAccessNameBuilder* output) noexcept;
bool ui_access_search_summary(const UiSearchScene& scene, UiAccessNameBuilder* output) noexcept;

} // namespace maicord
