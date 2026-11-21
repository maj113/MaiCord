#pragma once
#include "maicord/ui/core/input_router.hpp"
#include "maicord/ui/layout/scroll_host.hpp"
#include "maicord/ui/store/store_scene.hpp"
#include "maicord/platform/window/event.hpp"
#include "maicord/ui/core/cursor.hpp"
#include "maicord/ui/widgets/tooltip.hpp"

namespace maicord {
struct UiSettingsAvatarMedia;
}

namespace maicord::app_detail {
// A page in place of the chat column and the member list, as the official client shows a server's own routes (the
// Server Hub). Its owner builds a UiStoreScene tree. The host lays the tree out in a centred column of the content
// area, draws it through the renderer's content page slot, scrolls it and routes pointer and keys to it. Its row in
// the channel list shows as selected and names the header. One page shows at a time. Choosing a channel, another
// server or Home closes it.
struct ContentPageOwner {
    u64 row;
    // Builds the page tree for a column `width` DIPs wide.
    void (*build)(UiStoreBuilder& builder, float width) noexcept;
    // A node with this action was activated; `anchor` is its rectangle.
    void (*activate)(u32 action, const UiRect& anchor) noexcept;
    // The picture of a preview's slot (a user avatar, a server icon, a picture by URL), or null.
    const UiSettingsAvatarMedia* (*media)(u32 slot) noexcept;
    // Frame time: true when the page must be built again (its data changed). May close the page.
    bool (*stale)() noexcept;
    // The column's widest (0: kContentPageColumnMaximum) and its padding from the page's edges (0: 16 DIPs).
    float column_maximum, padding;
};
inline constexpr float kContentPageColumnMaximum = 1072;

bool application_content_page_open(const ContentPageOwner* owner) noexcept;
void application_content_page_close() noexcept;
bool application_content_page_active() noexcept;
// The open page's owner, or null.
const ContentPageOwner* application_content_page() noexcept;
// The owner's data changed: the page is built again at the next synchronization.
void application_content_page_invalidate() noexcept;
// Frame time: builds and lays the page out when needed and binds it to the renderer.
bool application_content_page_synchronize() noexcept;
// After a scroll step: lay out and refresh the retained projection.
bool application_content_page_refresh() noexcept;
bool application_content_page_window_event(const WindowEvent&) noexcept;
bool application_content_page_input(UiInputKind kind, u32 code, bool repeated) noexcept;
bool application_content_page_scroll_range(UiScrollRange* range) noexcept;
bool application_content_page_scroll_to(double offset) noexcept;
// The page's inner list (a store pane): its scroll range and offset, as a scroll host surface.
bool application_content_page_pane_range(UiScrollRange* range) noexcept;
bool application_content_page_pane_scroll_to(double offset) noexcept;
// How far the inner list is scrolled (0 without one): an owner places what follows the list by it.
float application_content_page_pane_offset() noexcept;
// Over the page: the hand over what clicks, else the arrow. False when the point is not on the page.
bool application_content_page_cursor(float x, float y, UiCursor* cursor) noexcept;
// The tooltip a node of the page names under (x, y) (a game's name, what a game icon cluster played).
bool application_content_page_tooltip(UiPoint point, u64 scope, UiTooltipTarget* target) noexcept;
void application_content_page_release() noexcept;
} // namespace maicord::app_detail
