#pragma once

#include "maicord/ui/accessibility.hpp"

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
u32 ui_access_add_media_picker(UiAccessTree* tree, u32 parent, const UiMediaPickerScene& scene,
                               u8 surface) noexcept;

// Result count line ("12 results", "No matches", "Searching…") for status
// announcements; false when the scene has nothing to report.
bool ui_access_quick_switcher_summary(const UiQuickSwitcherScene& scene, UiAccessNameBuilder* output) noexcept;
bool ui_access_search_summary(const UiSearchScene& scene, UiAccessNameBuilder* output) noexcept;

} // namespace maicord
