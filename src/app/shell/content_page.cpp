#include "content_page.hpp"
#include "input.hpp"
#include "overlay_input.hpp"
#include "owner.hpp"
#include "panel_cover.hpp"
#include "retained_projection.hpp"
#include "state.hpp"
#include "../chat/threads/browser.hpp"
#include "../chat/composer/input.hpp"
#include "../chat/composer/text_input.hpp"
#include "../chat/messages/interaction.hpp"
#include "../chat/messages/pinned_messages.hpp"
#include "../chat/search/search.hpp"
#include "window_events.hpp"
#include "../settings/settings.hpp"
#include "maicord/platform/window/key.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/ui/layout/scrollbar.hpp"
#include "maicord/ui/settings/scene.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/ui/theme/style_projection.hpp"
#include "maicord/ui/theme/styles.hpp"

namespace maicord::app_detail {
namespace {
struct ContentPage {
    UiStoreScene scene;
    UiStoreMeasure measure;
    UiRect viewport;
    const ContentPageOwner* owner;
    float scrollbar_grab;
    bool dirty, pressing, dragging_scrollbar;
};
ContentPage* g_page;

bool media_key(const void*, u32 slot, const UiRect&, MediaAssetKey* key, u8* domain) noexcept {
    const auto* media = g_page && g_page->owner ? g_page->owner->media(slot) : nullptr;
    if (!media || !media->key.id)
        return false;
    *key = media->key;
    *domain = 0;
    return true;
}

UiScrollbarGeometry scrollbar() noexcept {
    const auto& s = g_page->scene;
    return ui_scrollbar_geometry({s.panel.left, s.column.top, s.panel.right, s.panel.bottom}, s.scroll,
                                 s.content_height);
}
} // namespace

bool application_content_page_open(const ContentPageOwner* owner) noexcept {
    if (!g_page && !(g_page = owner_allocate<ContentPage>()))
        return false;
    auto& scene = g_page->scene;
    // A closed page released its scene (to zero): it opens again here.
    if (!scene.nodes) {
        if (!ui_store_scene_open(&scene))
            return false;
        g_page->measure = ui_store_cache_measure(g_app.renderer.message_measure_cache);
        scene.measure = &g_page->measure;
        scene.media_key = media_key;
        scene.active = true;
    }
    // The page covers what the thread browser covers: the same panels close and the composer lets go.
    application_thread_browser_close();
    if (!application_search_close_panel())
        return false;
    application_pinned_messages_close();
    end_message_composer_pointer_selection();
    end_message_text_selection();
    ui_scroll_host_cancel(&g_app.scroll_host);
    application_release_capture();
    application_composer_text_input_suspend();
    g_app.message_composer.focused = false;
    reset_value(&g_app.message_action_bar);
    if (g_page->owner != owner)
        g_page->scene.scroll = 0;
    g_page->owner = owner;
    g_page->dirty = true;
    g_app.context_focus = &g_page->scene.widgets;
    // A forced text refresh lays the header out again: the page shows no channel's pins or threads.
    return application_content_page_synchronize() && refresh_retained_projection(true);
}

void application_content_page_close() noexcept {
    renderer_bind_content_page(&g_app.renderer, nullptr, 0);
    if (!g_page || !g_page->owner)
        return;
    g_page->owner = nullptr;
    if (g_page->pressing)
        application_release_capture();
    g_page->pressing = g_page->dragging_scrollbar = false;
    if (g_app.context_focus == &g_page->scene.widgets)
        g_app.context_focus = nullptr;
    // A closed page keeps no tree: the next page builds its own.
    ui_store_scene_release(&g_page->scene);
    application_request_frame();
}

bool application_content_page_active() noexcept {
    return g_page && g_page->owner;
}

const ContentPageOwner* application_content_page() noexcept {
    return g_page ? g_page->owner : nullptr;
}

void application_content_page_invalidate() noexcept {
    if (application_content_page_active()) {
        g_page->dirty = true;
        application_request_frame();
    }
}

bool application_content_page_synchronize() noexcept {
    if (!application_content_page_active())
        return true;
    // The owner's check may close the page.
    const bool stale = g_page->owner->stale();
    if (!application_content_page_active())
        return true;
    auto& p = *g_page;
    p.dirty = p.dirty || stale;
    const UiRect viewport = application_thread_browser_viewport();
    if (p.dirty || !ui_rect_equal(viewport, p.viewport)) {
        p.dirty = false;
        p.viewport = viewport;
        // A column of at most the owner's maximum, centred, its padding from the edges.
        const float pad = p.owner->padding ? p.owner->padding : 16;
        const float room = maximum(1.f, ui_rect_width(viewport) - pad * 2);
        const float width =
            minimum(room, p.owner->column_maximum ? p.owner->column_maximum : kContentPageColumnMaximum);
        const float left = viewport.left + pad + (room - width) * .5f;
        const UiRect column{left, viewport.top + pad, left + width, viewport.bottom - pad};
        ui_store_scene_begin(&p.scene);
        UiStoreBuilder builder(&p.scene);
        p.owner->build(builder, width);
        if (!ui_store_scene_commit(&p.scene, viewport, column, viewport))
            return false;
        // The media pump ran before this frame built the tree: it asks for the new tree's pictures next turn.
        g_app.message_media.scan_pending = true;
        application_request_frame();
    }
    // A switch slides and skeleton rows pulse (the Channels & Roles follow switches, the hub's loading rows): frames
    // only then.
    const bool reduced = application_reduced_motion();
    const auto& timing = ui_theme_settings(application_theme().styles);
    if (ui_store_scene_tick(&p.scene, clock_monotonic_milliseconds(), reduced,
                            reduced ? timing.switch_reduced_ms : timing.switch_ms))
        application_request_frame();
    return renderer_bind_content_page(&g_app.renderer, &p.scene, p.owner->row);
}

bool application_content_page_refresh() noexcept {
    return application_content_page_synchronize() && refresh_retained_projection(false);
}

bool application_content_page_scroll_range(UiScrollRange* range) noexcept {
    float offset = 0, most = 0;
    if (!application_content_page_active() || g_page->dragging_scrollbar ||
        !ui_store_scene_scroll_range(g_page->scene, false, &range->viewport, &offset, &most))
        return false;
    range->offset = offset;
    range->maximum = most;
    range->line = kUiScrollLineDips;
    range->units_per_dip = 1;
    range->origin = 0;
    range->generation = g_page->owner->row;
    return true;
}

bool application_content_page_scroll_to(double offset) noexcept {
    if (!application_content_page_active())
        return false;
    if (ui_store_scene_scroll_to(&g_page->scene, static_cast<float>(offset)))
        application_request_frame();
    return true;
}

float application_content_page_pane_offset() noexcept {
    return application_content_page_active() ? g_page->scene.pane_scroll : 0;
}

bool application_content_page_pane_range(UiScrollRange* range) noexcept {
    float offset = 0, most = 0;
    if (!application_content_page_active() ||
        !ui_store_scene_pane_range(g_page->scene, &range->viewport, &offset, &most))
        return false;
    range->offset = offset;
    range->maximum = most;
    range->line = kUiScrollLineDips;
    range->units_per_dip = 1;
    range->origin = 0;
    range->generation = g_page->owner->row ^ 1;
    return true;
}

bool application_content_page_pane_scroll_to(double offset) noexcept {
    if (!application_content_page_active())
        return false;
    // The owner places the account's floating row by the list's offset: the page is built again.
    if (ui_store_scene_pane_scroll_to(&g_page->scene, static_cast<float>(offset))) {
        g_page->dirty = true;
        application_request_frame();
    }
    return true;
}

bool application_content_page_cursor(float x, float y, UiCursor* cursor) noexcept {
    if (!application_content_page_active() || !ui_rect_contains(g_page->viewport, x, y))
        return false;
    *cursor = ui_store_scene_action_at(g_page->scene, x, y) ? UiCursor::hand : UiCursor::arrow;
    return true;
}

bool application_content_page_tooltip(UiPoint point, u64 scope, UiTooltipTarget* target) noexcept {
    Utf8View text{};
    UiRect anchor{};
    u32 index = 0;
    if (!application_content_page_active() || !ui_rect_contains(g_page->viewport, point.x, point.y) ||
        !ui_store_scene_tooltip_at(g_page->scene, point.x, point.y, &text, &anchor, &index))
        return false;
    const auto& s = g_page->scene;
    *target = {{{&s.widgets, s.root}, scope, index, s.revision, 0xc00u}, anchor, text};
    return true;
}

bool application_content_page_window_event(const WindowEvent& input) noexcept {
    if (!application_content_page_active())
        return false;
    auto& p = *g_page;
    if (input.type == EventType::capture_lost || input.type == EventType::cancel_input || input_focus_lost(input)) {
        p.pressing = p.dragging_scrollbar = false;
        ui_store_scene_release_pointer(&p.scene, -1, -1, nullptr);
        return false;
    }
    if (input.type == EventType::pointer_leave) {
        if (ui_store_scene_hover(&p.scene, -1, -1))
            application_request_frame();
        return false;
    }
    const PointerStep step = input_pointer_step(input);
    if (step == PointerStep::none)
        return false;
    // The window procedure asks the page before the profile, the menus and the reaction viewer. When one of these, a
    // dialog or a viewer covers the page, the page gives the event to it. The page also ends its press and its hover.
    if (!application_panel_uncovered()) {
        if (p.pressing) {
            p.pressing = p.dragging_scrollbar = false;
            ui_store_scene_release_pointer(&p.scene, -1, -1, nullptr);
            application_release_capture();
        }
        if (step == PointerStep::move && ui_store_scene_hover(&p.scene, -1, -1))
            application_request_frame();
        return false;
    }
    UiPoint point{};
    client_event_dip(input, &point);
    const bool inside = ui_rect_contains(p.viewport, point.x, point.y);
    if (step == PointerStep::move) {
        if (p.dragging_scrollbar) {
            ui_store_scene_scroll_to(&p.scene, ui_scrollbar_scroll_from_pointer(scrollbar(), point.y, p.scrollbar_grab));
            return application_request_frame();
        }
        if (ui_store_scene_hover(&p.scene, inside ? point.x : -1, inside ? point.y : -1))
            application_request_frame();
        return p.pressing;
    }
    if (step == PointerStep::press) {
        if (!inside)
            return false;
        p.pressing = true;
        g_app.context_focus = &p.scene.widgets;
        application_focus_window();
        application_capture_pointer();
        const auto bar = scrollbar();
        if (ui_scrollbar_thumb_hit(bar, point.x, point.y)) {
            p.dragging_scrollbar = true;
            p.scrollbar_grab = point.y - bar.thumb.top;
            return true;
        }
        UiStoreEvent event{};
        ui_store_scene_press(&p.scene, point.x, point.y, &event);
        return application_request_frame();
    }
    // The release of a press the page took.
    if (!p.pressing)
        return false;
    const bool dragged = p.dragging_scrollbar;
    p.pressing = p.dragging_scrollbar = false;
    UiStoreEvent event{};
    if (!dragged)
        ui_store_scene_release_pointer(&p.scene, point.x, point.y, &event);
    application_release_capture();
    if (event.kind == UiStoreEventKind::activate && p.owner)
        p.owner->activate(event.action, event.anchor);
    return application_request_frame();
}

bool application_content_page_input(UiInputKind kind, u32 code, bool repeated) noexcept {
    if (!application_content_page_active() || g_app.context_focus != &g_page->scene.widgets ||
        (kind != UiInputKind::key_down && kind != UiInputKind::system_key_down))
        return false;
    static constexpr u32 kCodes[]{key::tab, key::up, key::down, key::page_up, key::page_down, key::home, key::end,
                                  key::enter, key::space};
    static constexpr UiStoreKey kKeys[]{UiStoreKey::tab,       UiStoreKey::up,   UiStoreKey::down,
                                        UiStoreKey::page_up,   UiStoreKey::page_down, UiStoreKey::home,
                                        UiStoreKey::end,       UiStoreKey::enter, UiStoreKey::space};
    u32 i = 0;
    while (i < sizeof(kCodes) / sizeof(kCodes[0]) && kCodes[i] != code)
        ++i;
    if (i == sizeof(kCodes) / sizeof(kCodes[0]))
        return false;
    if (i >= 7 && repeated)
        return true;
    const UiStoreKey key = i == 0 && application_shift_down() ? UiStoreKey::back_tab : kKeys[i];
    UiStoreEvent event{};
    const float pane = g_page->scene.pane_scroll;
    const bool used = ui_store_scene_key(&g_page->scene, key, &event);
    // Focus that scrolls the inner list moves what the owner places by it.
    g_page->dirty = g_page->dirty || g_page->scene.pane_scroll != pane;
    if (event.kind == UiStoreEventKind::activate)
        g_page->owner->activate(event.action, event.anchor);
    if (used)
        application_request_frame();
    return used;
}

void application_content_page_release() noexcept {
    application_content_page_close();
    if (g_page)
        owner_release(g_page);
}
} // namespace maicord::app_detail
