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

#include "maicord/core/time.hpp"
#include "maicord/platform/window/frame.hpp"
#include "maicord/platform/window/key.hpp"
#include "maicord/platform/window/window.hpp"
#include "maicord/render/d3d11/renderer.hpp"
#include "maicord/ui/widgets/tooltip.hpp"
#include "../account/account_ui_runtime.hpp"

namespace maicord::app_detail {
namespace {

struct CaptionState {
    UiCaptionPart hovered;
    UiCaptionPart pressed;
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
} g_caption;

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

void set_hover(UiCaptionPart hovered, UiCaptionPart pressed) noexcept {
    if (g_caption.hovered == hovered && g_caption.pressed == pressed)
        return;
    if (g_caption.hovered != hovered) {
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
    return part == UiCaptionPart::update || part == UiCaptionPart::settings || part == UiCaptionPart::minimize ||
           part == UiCaptionPart::close ||
           (part == UiCaptionPart::maximize && !window_caps().native_caption_buttons);
}

// The bar part under a client point, none below the bar.
UiCaptionPart client_part(const WindowEvent& event) noexcept {
    const i32 x = static_cast<i32>(event.pointer.x), y = static_cast<i32>(event.pointer.y);
    if (y < 0 || y >= static_cast<i32>(g_app.renderer.caption_height))
        return UiCaptionPart::none;
    const u32 dpi = window_dpi();
    return ui_caption_bar_part(layout_for(g_window_state.client_width, dpi), to_dips(x, dpi), to_dips(y, dpi));
}

// Pointer input over the bar's own items (update badge, Settings, the window buttons the platform leaves to the bar).
bool item_pointer(const WindowEvent& event) noexcept {
    switch (event.type) {
    case EventType::pointer_move: {
        // Another owner's captured drag keeps its moves, even over the bar.
        const bool foreign = window_pointer_captured(g_app.window) && !bar_item(g_caption.pressed);
        const UiCaptionPart part = foreign ? UiCaptionPart::none : client_part(event);
        if (bar_item(part))
            set_hover(part, g_caption.pressed);
        else if (bar_item(g_caption.hovered))
            set_hover(UiCaptionPart::none, g_caption.pressed);
        // A press dragged off its item keeps the pointer until release.
        return bar_item(part) || bar_item(g_caption.pressed);
    }
    case EventType::pointer_down: {
        if (event.button == static_cast<u8>(PointerButton::left)) {
            const UiCaptionPart part = client_part(event);
            if (!bar_item(part))
                return false;
            window_capture_pointer(g_app.window);
            set_hover(part, part);
            return true;
        }
        // The bar's items take no other buttons; content never sees them.
        return event.button <= static_cast<u8>(PointerButton::middle) && bar_item(client_part(event));
    }
    case EventType::pointer_up: {
        if (event.button != static_cast<u8>(PointerButton::left))
            return event.button <= static_cast<u8>(PointerButton::middle) && bar_item(client_part(event));
        const UiCaptionPart pressed = g_caption.pressed;
        if (!bar_item(pressed))
            return false;
        const UiCaptionPart part = client_part(event);
        set_hover(bar_item(part) ? part : UiCaptionPart::none, UiCaptionPart::none);
        window_release_capture(g_app.window);
        if (part == pressed)
            application_caption_command(part);
        return true;
    }
    case EventType::pointer_leave:
        if (bar_item(g_caption.hovered))
            set_hover(UiCaptionPart::none, g_caption.pressed);
        return false;
    case EventType::capture_lost:
        if (bar_item(g_caption.pressed))
            set_hover(g_caption.hovered, UiCaptionPart::none);
        return false;
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
    return {g_caption.hovered,
            g_caption.pressed,
            g_caption.active,
            g_caption.maximized,
            application_update_badge(updates),
            g_app.login.visible,
            application_update_progress(updates),
            to_dips(static_cast<i32>(g_caption.side_frame), window_dpi()),
            tooltip ? g_caption.hovered : UiCaptionPart::none};
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
    if (height && renderer_set_caption(&g_app.renderer, application_caption_state(), caption_title()))
        synchronize_icon(dpi);
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
    set_hover(button_part(part), g_caption.pressed);
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
