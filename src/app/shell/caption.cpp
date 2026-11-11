#include "caption.hpp"
#include "cursor.hpp"
#include "window_events.hpp"
#include "window_state.hpp"
#include "maicord/core/scratch_pages.hpp"

#include "app_icon.hpp"
#include "frame_presentation.hpp"
#include "../account/friends.hpp"
#include "../settings/settings.hpp"
#include "state.hpp"
#include "tooltip.hpp"
#include "../update/updates.hpp"
#include "../settings/catalog.hpp"
#include "maicord/ui/accessibility/accessibility.hpp"
#include "../chat/messages/interaction.hpp"
#include "tabs.hpp"
#include "window_runtime.hpp"
#include "maicord/ui/shell/navigation_drag.hpp"

#include "maicord/core/time.hpp"
#include "maicord/platform/window/frame.hpp"
#include "maicord/platform/window/key.hpp"
#include "maicord/platform/window/window.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/ui/widgets/tooltip.hpp"
#include "../account/account_ui_runtime.hpp"

namespace maicord::app_detail {
namespace {

// A press on a tab that may become its drag: its index, where the pointer and the tab's left edge were.
struct TabDrag {
    bool pressed, active;
    u8 index, origin;
    float press_x, grab;
};

struct CaptionState {
    UiCaptionPart hovered;
    UiCaptionPart pressed;
    // Which tab, "+" or "..." the pointer is over and pressed (part tab).
    UiTabHit hovered_tab;
    UiTabHit pressed_tab;
    TabDrag drag;
    bool enabled;
    bool hidden;
    bool active;
    bool maximized;
    // The app icon's pixel size on the renderer, zero until decoded.
    u32 icon_extent;
    // Visible frame pixels beside the client beyond those above it; stale
    // after a size change until the next frame measures it.
    u32 side_frame;
    bool side_frame_stale;
    // When the hovered part's tooltip is due; zero while none is pending or
    // after a press, until the pointer moves to another part.
    u64 tooltip_at_ms;
    // When a shown tooltip last hid.
    u64 tooltip_hidden_ms;
    bool tooltip_shown;
    // The shown tooltip's entrance (kUiMotionTooltip), restarted for each part it shows for.
    UiMotionTransition tooltip_entrance;
    UiCaptionPart entrance_part;
    u64 entrance_generation;
} g_caption;
constexpr u64 kCaptionTooltipMotionOwner = 0x4341'5054'5449'5031ull; // "CAPTTIP1"

inline constexpr u64 kCaptionTooltipSweepMs = 400;

// The DWM border of a restored window lies outside the client at the sides but over the client's top row; none while
// maximized.
void measure_side_frame() noexcept {
    u32 pixels = 0;
    if (!window_frame_inset(g_app.window, &pixels))
        return;
    g_caption.side_frame_stale = false;
    if (pixels != g_caption.side_frame) {
        g_caption.side_frame = pixels;
        application_request_frame();
    }
}

u32 window_dpi() noexcept {
    const u32 dpi = application_window_dpi(g_app.window);
    return dpi ? dpi : 96;
}

float to_dips(i32 pixels, u32 dpi) noexcept {
    return static_cast<float>(pixels) * 96.0f / static_cast<float>(dpi);
}

UiCaptionBarLayout layout_for(i32 client_width, u32 dpi) noexcept {
    return ui_caption_bar_layout(application_theme(), to_dips(client_width, dpi),
                                 application_caption_state(),
                                 to_dips(static_cast<i32>(g_app.renderer.caption_height), dpi),
                                 static_cast<float>(dpi) / 96.0f);
}

// What the platform does with a press on a part: only the caption, the window menu and (where the system's snap
// layouts want it) the maximize button are frame; the rest of the bar is client.
FrameHit frame_part(UiCaptionPart part) noexcept {
    switch (ui_caption_bar_native(part)) {
    case UiCaptionNative::caption:
        return FrameHit::caption;
    case UiCaptionNative::system_menu:
        return FrameHit::system_menu;
    case UiCaptionNative::maximize:
        return window_caps().native_caption_buttons ? FrameHit::maximize : FrameHit::client;
    default:
        return FrameHit::client;
    }
}

UiCaptionPart button_part(FrameHit hit) noexcept {
    switch (hit) {
    case FrameHit::minimize:
        return UiCaptionPart::minimize;
    case FrameHit::maximize:
        return UiCaptionPart::maximize;
    case FrameHit::close:
        return UiCaptionPart::close;
    default:
        return UiCaptionPart::none;
    }
}

void set_hover(UiCaptionPart hovered, UiCaptionPart pressed, UiTabHit tab = {}, UiTabHit down = {}) noexcept {
    if (g_caption.hovered == hovered && g_caption.pressed == pressed && g_caption.hovered_tab == tab &&
        g_caption.pressed_tab == down)
        return;
    if (g_caption.hovered != hovered || g_caption.hovered_tab != tab) {
        // A content tooltip gives way to the bar's.
        if (g_caption.hovered == UiCaptionPart::none && ui_caption_bar_part_tooltip(hovered))
            application_tooltip_clear();
        // Sweeping from one shown tooltip to the next part skips the delay.
        const u64 now = clock_monotonic_milliseconds();
        if (g_caption.tooltip_shown)
            g_caption.tooltip_hidden_ms = now;
        const bool sweep = g_caption.tooltip_hidden_ms && now - g_caption.tooltip_hidden_ms < kCaptionTooltipSweepMs;
        g_caption.tooltip_shown = false;
        g_caption.tooltip_at_ms = !ui_caption_bar_part_tooltip(hovered) ? 0
                                  : sweep                                ? maximum<u64>(1, now)
                                                                         : now + kUiTooltipPointerDelayMs;
    }
    if (pressed != UiCaptionPart::none) {
        g_caption.tooltip_shown = false;
        g_caption.tooltip_at_ms = 0;
    }
    g_caption.hovered = hovered;
    g_caption.pressed = pressed;
    g_caption.hovered_tab = tab;
    g_caption.pressed_tab = down;
    application_request_frame();
}

// The icon at the bar's DPI, decoded once per size.
void synchronize_icon(u32 dpi) noexcept {
    // A new device (after a reset) holds no icon texture.
    static u32 device_generation = 0;
    if (device_generation != g_app.renderer.device_generation) {
        device_generation = g_app.renderer.device_generation;
        g_caption.icon_extent = 0;
    }
    const u32 extent =
        static_cast<u32>(application_theme().scale.caption_icon * static_cast<float>(dpi) / 96.0f + .5f);
    if (extent == g_caption.icon_extent || !g_app.renderer_ready)
        return;
    ScratchPages<u32[kAppIconLargestExtent * kAppIconLargestExtent]> pixels;
    if (pixels.data && app_icon_decode(extent, *pixels.data) &&
        renderer_set_caption_icon(&g_app.renderer, *pixels.data, extent))
        g_caption.icon_extent = extent;
}

// The bar's own items: client area, but the bar consumes their input before content coordinates apply. The window
// buttons are among them wherever the platform does not draw them as frame.
bool bar_item(UiCaptionPart part) noexcept {
    return part == UiCaptionPart::update || part == UiCaptionPart::settings || part == UiCaptionPart::tab ||
           part == UiCaptionPart::minimize || part == UiCaptionPart::close ||
           (part == UiCaptionPart::maximize && !window_caps().native_caption_buttons);
}

// What lies under a client point: the bar part, and for a tab, "+" or "..." its hit and the tab's left edge.
struct BarHit {
    UiCaptionPart part;
    UiTabHit tab;
    float tab_left;
};
BarHit client_hit(const WindowEvent& event) noexcept {
    const i32 x = static_cast<i32>(event.pointer.x), y = static_cast<i32>(event.pointer.y);
    if (y < 0 || y >= static_cast<i32>(g_app.renderer.caption_height))
        return {};
    const u32 dpi = window_dpi();
    const UiCaptionBarLayout layout = layout_for(g_window_state.client_width, dpi);
    const float dx = to_dips(x, dpi), dy = to_dips(y, dpi);
    const UiCaptionPart part = ui_caption_bar_part(layout, dx, dy);
    if (part != UiCaptionPart::tab)
        return {part, {}, 0};
    const UiTabHit tab = ui_tab_strip_hit(layout.tabs, dx, dy);
    return {part, tab, tab.kind == UiTabHitKind::tab ? layout.tabs.left[tab.index] : 0};
}

// A tab strip item's rectangle in content DIPs (the strip sits above the content origin), for the popups.
UiRect tab_anchor(UiTabHit hit) noexcept {
    const UiCaptionBarLayout layout = layout_for(g_window_state.client_width, window_dpi());
    const UiRect r = ui_tab_strip_hit_rect(layout.tabs, hit);
    return {r.left, r.top - layout.height, r.right, r.bottom - layout.height};
}

// What a click (or an accessibility invoke) on a tab strip item does.
bool activate_tab(UiTabHit hit) noexcept {
    if (application_input_blocked())
        return false;
    switch (hit.kind) {
    case UiTabHitKind::tab:
        return application_tabs_activate(hit.index);
    case UiTabHitKind::close:
        return application_tabs_close(hit.index);
    case UiTabHitKind::icon:
        return application_tabs_toggle_playback(hit.index);
    case UiTabHitKind::plus:
        return application_tabs_pin_current();
    case UiTabHitKind::overflow:
        return dismiss_popup() && application_tab_list_open(tab_anchor(hit));
    default:
        return false;
    }
}

// The pointer at `x` DIPs drags the pressed tab: past the drag threshold it follows the pointer and the others make
// way as its centre crosses theirs.
void drag_tab(float x) noexcept {
    TabDrag& drag = g_caption.drag;
    if (!drag.active && x - drag.press_x < kNavigationDragThreshold && drag.press_x - x < kNavigationDragThreshold)
        return;
    if (!drag.active) {
        const u32 at = application_tabs_drag_begin(drag.index);
        if (at >= kUiTabCapacity) {
            drag = {};
            return;
        }
        drag.active = true;
        drag.index = drag.origin = static_cast<u8>(at);
    }
    // The strip keeps the dragged tab inside itself; the slot follows from its centre.
    const UiTabStripLayout tabs = layout_for(g_window_state.client_width, window_dpi()).tabs;
    const float left = x - drag.grab;
    application_tabs_drag_to(left);
    const u32 to = ui_tab_strip_drag_slot(tabs, drag.index, left + tabs.slot_width[drag.index] / 2);
    if (to != drag.index && application_tabs_move(drag.index, to))
        drag.index = static_cast<u8>(to);
}

// Esc or a lost capture: the dragged tab goes back where it was.
void cancel_drag() noexcept {
    if (g_caption.drag.active) {
        application_tabs_move(g_caption.drag.index, g_caption.drag.origin);
        application_tabs_drag_end();
    }
    g_caption.drag = {};
}

// Pointer input over the bar's own items (update badge, Settings, the tabs, the window buttons the platform leaves to
// the bar).
bool item_pointer(const WindowEvent& event) noexcept {
    switch (event.type) {
    case EventType::pointer_move: {
        // Another owner's captured drag keeps its moves, even over the bar.
        const bool foreign = window_pointer_captured(g_app.window) && !bar_item(g_caption.pressed);
        const BarHit hit = foreign ? BarHit{} : client_hit(event);
        if (g_caption.drag.pressed)
            drag_tab(to_dips(static_cast<i32>(event.pointer.x), window_dpi()));
        if (bar_item(hit.part))
            set_hover(hit.part, g_caption.pressed, hit.tab, g_caption.pressed_tab);
        else if (bar_item(g_caption.hovered))
            set_hover(UiCaptionPart::none, g_caption.pressed, {}, g_caption.pressed_tab);
        // A press dragged off its item keeps the pointer until release.
        return bar_item(hit.part) || bar_item(g_caption.pressed);
    }
    case EventType::pointer_down: {
        const BarHit hit = client_hit(event);
        if (!bar_item(hit.part))
            return false;
        const bool tab = hit.part == UiCaptionPart::tab;
        if (tab && g_app.popup_scene.active)
            dismiss_popup();
        if (event.button == static_cast<u8>(PointerButton::left)) {
            window_capture_pointer(g_app.window);
            set_hover(hit.part, hit.part, hit.tab, hit.tab);
            // A press selects the tab at once, and may turn into its drag.
            if (hit.tab.kind == UiTabHitKind::tab && activate_tab(hit.tab)) {
                const float x = to_dips(static_cast<i32>(event.pointer.x), window_dpi());
                g_caption.drag = {true, false, hit.tab.index, hit.tab.index, x, x - hit.tab_left};
            }
            return true;
        }
        // The bar's items take no other buttons; content never sees them. A tab's right click opens its menu and a
        // middle click closes it.
        if (tab && !application_input_blocked() &&
            hit.tab.kind != UiTabHitKind::none && hit.tab.kind != UiTabHitKind::plus &&
            hit.tab.kind != UiTabHitKind::overflow) {
            if (event.button == static_cast<u8>(PointerButton::right))
                application_tab_menu_open(hit.tab.index, tab_anchor(hit.tab));
            else if (event.button == static_cast<u8>(PointerButton::middle))
                application_tabs_close(hit.tab.index);
        }
        return event.button <= static_cast<u8>(PointerButton::middle);
    }
    case EventType::pointer_up: {
        if (event.button != static_cast<u8>(PointerButton::left))
            return event.button <= static_cast<u8>(PointerButton::middle) && bar_item(client_hit(event).part);
        const UiCaptionPart pressed = g_caption.pressed;
        if (!bar_item(pressed))
            return false;
        const BarHit hit = client_hit(event);
        const UiTabHit down = g_caption.pressed_tab;
        const bool dragged = g_caption.drag.active;
        if (dragged)
            application_tabs_drag_end();
        g_caption.drag = {};
        set_hover(bar_item(hit.part) ? hit.part : UiCaptionPart::none, UiCaptionPart::none, hit.tab, {});
        window_release_capture(g_app.window);
        if (hit.part != pressed)
            return true;
        if (pressed != UiCaptionPart::tab)
            application_caption_command(pressed);
        else if (!dragged && hit.tab == down && down.kind != UiTabHitKind::tab)
            activate_tab(down);
        return true;
    }
    case EventType::pointer_leave:
        if (bar_item(g_caption.hovered))
            set_hover(UiCaptionPart::none, g_caption.pressed, {}, g_caption.pressed_tab);
        return false;
    case EventType::capture_lost:
        cancel_drag();
        if (bar_item(g_caption.pressed))
            set_hover(g_caption.hovered, UiCaptionPart::none, g_caption.hovered_tab, {});
        return false;
    case EventType::key_down:
        if (!g_caption.drag.active || !input_plain_key_down(event) || event.key.key != key::escape)
            return false;
        cancel_drag();
        set_hover(g_caption.hovered, UiCaptionPart::none, g_caption.hovered_tab, {});
        window_release_capture(g_app.window);
        return true;
    case EventType::cursor_query:
        if (input_has_position(event) && event.pointer.y < static_cast<float>(g_app.renderer.caption_height)) {
            application_set_cursor(UiCursor::arrow);
            return true;
        }
        return false;
    default:
        return false;
    }
}

// The current place, as the official client's title bar names it.
UiCaptionTitle caption_title() noexcept {
    UiCaptionTitleInput in{};
    const auto* ui = g_app.account_ui;
    in.signed_in = ui && !g_app.login.visible;
    if (in.signed_in) {
        const auto& state = ui->chat_state;
        in.friends = application_friends_active();
        in.home = state.home_selected;
        const ChannelRecord* channel = chat_state_store_find_channel(state, state.selected_channel_id);
        in.private_type = channel && channel_is_private(channel->type) ? channel->type : 0;
        if (const GuildRecord* guild = state.home_selected ? nullptr
                                                           : chat_state_store_find_guild(state, state.selected_guild_id)) {
            in.guild_id = guild->id;
            in.guild_icon_variant = guild->icon_hash_bytes ? guild->icon_variant : 0;
            in.guild_name = chat_state_store_guild_name(state, *guild);
        }
    }
    return ui_caption_title(in);
}

} // namespace

void application_caption_enable() noexcept {
    g_caption.enabled = true;
    g_caption.active = true;
    g_caption.side_frame_stale = true;
}

u32 application_caption_pixels(u32 dpi) noexcept {
    if (!g_caption.enabled || g_caption.hidden)
        return 0;
    return static_cast<u32>(ui_caption_bar_height(application_theme()) * static_cast<float>(dpi) / 96.0f + .5f);
}

bool application_caption_hidden() noexcept {
    return g_caption.hidden;
}

u64 application_caption_tooltip_wake() noexcept {
    return g_caption.tooltip_shown ? 0 : g_caption.tooltip_at_ms;
}

UiCaptionBarState application_caption_state() noexcept {
    if (!g_caption.tooltip_shown && g_caption.tooltip_at_ms &&
        clock_monotonic_milliseconds() >= g_caption.tooltip_at_ms)
        g_caption.tooltip_shown = true;
    const bool tooltip = g_caption.tooltip_shown && g_caption.pressed == UiCaptionPart::none &&
                         ui_caption_bar_part_tooltip(g_caption.hovered);
    const ApplicationUpdatesView updates = application_updates_view();
    // A newly shown tooltip pops in (opacity) on the tooltip spring.
    const UiCaptionPart shown = tooltip ? g_caption.hovered : UiCaptionPart::none;
    float entrance = 0;
    if (shown != UiCaptionPart::none) {
        if (shown != g_caption.entrance_part) {
            g_caption.entrance_part = shown;
            ui_motion_transition_reset(&g_caption.tooltip_entrance,
                                       {kCaptionTooltipMotionOwner, ++g_caption.entrance_generation}, 0);
        }
        entrance = 1 - application_motion_tick(&g_caption.tooltip_entrance, g_caption.tooltip_entrance.scope, 1,
                                               clock_monotonic_milliseconds(), kUiMotionTooltip);
    } else {
        g_caption.entrance_part = UiCaptionPart::none;
    }
    return {g_caption.hovered,
            g_caption.pressed,
            g_caption.active,
            g_caption.maximized,
            application_update_badge(updates),
            g_app.login.visible,
            application_update_progress(updates),
            to_dips(static_cast<i32>(g_caption.side_frame), window_dpi()),
            shown,
            entrance,
            application_tabs_view(g_caption.hovered_tab, g_caption.pressed_tab, clock_monotonic_milliseconds())};
}

UiCaptionBarLayout application_caption_layout() noexcept {
    const u32 dpi = maximum(g_app.renderer.dpi, 1u);
    UiCaptionBarLayout layout = layout_for(static_cast<i32>(g_app.renderer.width), dpi);
    // Content coordinates: the caption sits above the content origin.
    const float top = -layout.height;
    const auto lift = [&](UiRect* r) noexcept { *r = {r->left, r->top + top, r->right, r->bottom + top}; };
    for (auto& button : layout.buttons)
        lift(&button);
    lift(&layout.icon);
    lift(&layout.icon_target);
    lift(&layout.update);
    lift(&layout.settings);
    lift(&layout.title);
    lift(&layout.tabs.area);
    lift(&layout.tabs.plus);
    lift(&layout.tabs.overflow);
    lift(&layout.tab_hover);
    return layout;
}

bool application_caption_command(UiCaptionPart part) noexcept {
    if (!g_app.window)
        return false;
    switch (part) {
    case UiCaptionPart::icon:
        // As Alt+Space: the window menu at the caption.
        window_show_system_menu(g_app.window, 0, static_cast<float>(g_app.renderer.caption_height));
        return true;
    case UiCaptionPart::update:
        if (application_updates_restart())
            return true;
        return application_settings_open_page(
            {kSettingsNativeOwner, static_cast<u64>(SettingsPanel::updates) + 1});
    case UiCaptionPart::settings:
        return application_settings_open();
    case UiCaptionPart::minimize:
        window_minimize(g_app.window);
        return true;
    case UiCaptionPart::maximize:
        window_toggle_maximize(g_app.window);
        return true;
    case UiCaptionPart::close:
        window_request_close(g_app.window);
        return true;
    default:
        return false;
    }
}

bool application_caption_access(u64 item) noexcept {
    const auto part = static_cast<UiCaptionPart>(item & 0xff);
    return part == UiCaptionPart::tab
               ? activate_tab({static_cast<UiTabHitKind>(item >> 8 & 0xff), static_cast<u8>(item >> 16 & 0xff)})
               : application_caption_command(part);
}

void application_caption_describe_tabs(UiAccessTree* tree, u32 parent, u8 surface) noexcept {
    const UiCaptionBarLayout layout = application_caption_layout();
    const UiTabStripView* tabs = application_caption_state().tabs;
    if (!tabs)
        return;
    UiAccessNodeDesc list{};
    list.key = ui_access_key(surface, static_cast<u64>(UiCaptionPart::tab));
    list.rectangle = layout.tabs.area;
    list.name = utf8_literal("Open tabs");
    list.role = UiAccessRole::tab_list;
    list.surface = surface;
    const u32 tab_list = ui_access_add(tree, parent, list);
    if (tab_list == kUiAccessNone)
        return;
    const auto add = [&](u32 under, UiAccessRole role, UiTabHit hit, const UiRect& rectangle, Utf8View name,
                         u16 flags, u16 actions) noexcept {
        UiAccessNodeDesc node{};
        node.item = application_caption_tab_item(hit);
        node.key = ui_access_key(surface, node.item);
        node.rectangle = rectangle;
        node.name = name;
        node.role = role;
        node.state = flags;
        node.actions = actions;
        node.surface = surface;
        return ui_rect_has_area(rectangle) ? ui_access_add(tree, under, node) : kUiAccessNone;
    };
    for (u32 i = 0; i < tabs->count; ++i) {
        const UiTabItem& item = tabs->items[i];
        if (item.flags & kUiTabClosing)
            continue;
        const u32 tab = add(tab_list, UiAccessRole::tab, {UiTabHitKind::tab, static_cast<u8>(i)},
                            ui_tab_strip_tab(layout.tabs, i), item.label,
                            static_cast<u16>(ui_access_selectable | (item.flags & kUiTabActive ? ui_access_selected : 0)),
                            ui_access_action_select | ui_access_action_invoke);
        if (tab == kUiAccessNone)
            continue;
        // A media tab says what its page holds (its icon), and when it cannot load again; a playing one has a pause
        // (or play) button.
        const Utf8View media = item.icon == UiIcon::video   ? utf8_literal("Video")
                               : item.icon == UiIcon::music ? utf8_literal("Audio")
                               : item.icon == UiIcon::audio_lines ? utf8_literal("Playing")
                               : item.icon == UiIcon::image || item.icon == UiIcon::image_off ? utf8_literal("Image")
                                                                                               : Utf8View{};
        if (media.bytes)
            ui_access_set_help(tree, tab, item.flags & kUiTabMissing ? utf8_literal("Media unavailable") : media);
        if (item.flags & kUiTabPlaying) {
            const UiTabHit icon{UiTabHitKind::icon, static_cast<u8>(i)};
            add(tab, UiAccessRole::button, icon, ui_tab_strip_icon(layout.tabs, i), ui_tab_strip_hit_name(*tabs, icon), 0,
                ui_access_action_invoke);
        }
        add(tab, UiAccessRole::button, {UiTabHitKind::close, static_cast<u8>(i)}, ui_tab_strip_close(layout.tabs, i),
            utf8_literal("Close tab"), 0, ui_access_action_invoke);
    }
    static constexpr UiTabHitKind kButtons[]{UiTabHitKind::plus, UiTabHitKind::overflow};
    for (const UiTabHitKind kind : kButtons) {
        const UiTabHit hit{kind, 0};
        add(parent, UiAccessRole::button, hit, ui_tab_strip_hit_rect(layout.tabs, hit), ui_tab_strip_hit_name(*tabs, hit),
            0, ui_access_action_invoke);
    }
}

void application_caption_set_hidden(bool hidden) noexcept {
    if (!g_caption.enabled || g_caption.hidden == hidden)
        return;
    g_caption.hidden = hidden;
    renderer_set_caption_height(&g_app.renderer, application_caption_pixels(window_dpi()));
}

void application_caption_synchronize() noexcept {
    if (!g_caption.enabled || !g_app.window)
        return;
    const u32 dpi = window_dpi();
    const u32 height = application_caption_pixels(dpi);
    if (height != g_app.renderer.caption_height) {
        // A theme changed the band: the content below it resizes.
        renderer_set_caption_height(&g_app.renderer, height);
        application_resize_client();
    }
    if (g_caption.side_frame_stale)
        measure_side_frame();
    const UiCaptionBarState state = application_caption_state();
    if (height && renderer_set_caption(&g_app.renderer, state, caption_title()))
        synchronize_icon(dpi);
    // The tabs slide from where this frame draws them.
    if (height && state.tabs)
        application_tabs_note_layout(layout_for(static_cast<i32>(g_app.renderer.width), dpi).tabs);
}

bool application_caption_event(const WindowEvent& event) noexcept {
    if (!g_caption.enabled)
        return false;
    // The platform publishes the new frame bounds after the move completes.
    if (event.type == EventType::resize && !(event.flags & ev_provisional))
        g_caption.side_frame_stale = true;
    // The bar's height follows the scale of the monitor the window is on.
    else if (event.type == EventType::scale_changed)
        renderer_set_caption_height(&g_app.renderer, application_caption_pixels(input_dpi(event)));
    return g_app.renderer.caption_height && item_pointer(event);
}

FrameHit application_caption_hit(float x, float y) noexcept {
    const i32 px = static_cast<i32>(x), py = static_cast<i32>(y);
    const u32 dpi = window_dpi();
    const i32 width = g_window_state.client_width;
    const auto caption = static_cast<i32>(g_app.renderer.caption_height);
    UiCaptionPart part = UiCaptionPart::none;
    if (py >= 0 && py < caption && px >= 0 && px < width)
        part = ui_caption_bar_part(layout_for(width, dpi), to_dips(px, dpi), to_dips(py, dpi));
    return window_frame_hit(px, py, width, g_window_state.client_height,
                            static_cast<i32>(window_resize_border(g_app.window, dpi)), g_caption.maximized,
                            frame_part(part));
}

void application_caption_maximized(bool maximized) noexcept {
    if (g_caption.maximized != maximized) {
        g_caption.maximized = maximized;
        application_request_frame();
    }
}

void application_caption_active(bool active) noexcept {
    if (g_caption.active != active) {
        g_caption.active = active;
        application_request_frame();
    }
}

void application_caption_hover(FrameHit part) noexcept {
    set_hover(button_part(part), g_caption.pressed, {}, g_caption.pressed_tab);
}

void application_caption_leave() noexcept {
    set_hover(UiCaptionPart::none, UiCaptionPart::none);
}

void application_caption_press(FrameHit part) noexcept {
    const UiCaptionPart pressed = button_part(part);
    set_hover(pressed, pressed);
}

bool application_caption_release(FrameHit hit) noexcept {
    const UiCaptionPart part = button_part(hit);
    const UiCaptionPart pressed = g_caption.pressed;
    if (pressed == UiCaptionPart::none && part == UiCaptionPart::none)
        return false;
    set_hover(part, UiCaptionPart::none);
    if (part != UiCaptionPart::none && part == pressed)
        application_caption_command(part);
    return true;
}

bool application_caption_bar_rects(FrameBarRects* rectangles) noexcept {
    if (!g_app.renderer.caption_height)
        return false;
    const u32 dpi = window_dpi();
    const UiCaptionBarLayout layout = layout_for(g_window_state.client_width, dpi);
    const float scale = static_cast<float>(dpi) / 96.0f;
    const auto pixel = [scale](float value) noexcept { return static_cast<i32>(value * scale + .5f); };
    rectangles->bar_right = pixel(layout.width);
    rectangles->bar_bottom = pixel(layout.height);
    for (u32 i = 0; i < kUiCaptionButtonCount; ++i)
        rectangles->button[i][0] = pixel(layout.buttons[i].left), rectangles->button[i][1] = pixel(layout.buttons[i].top),
        rectangles->button[i][2] = pixel(layout.buttons[i].right), rectangles->button[i][3] = pixel(layout.buttons[i].bottom);
    return true;
}

} // namespace maicord::app_detail
