// wl_pointer to WindowEvents: motion, buttons with the double-click synthesiser, wheel, capture bookkeeping and the
// cursor (wp_cursor_shape, with an XCursor wl_shm fallback).
#include "maicord/core/memory.hpp"
#include "maicord/platform/dbus/desktop_settings.hpp"
#include "maicord/platform/window/wayland/abi.hpp"
#include "maicord/sys/linux/process.hpp"
#include "maicord/ui/core/cursor.hpp"
#include "backend.hpp"

namespace maicord::wayland {
namespace {

// <linux/input-event-codes.h>
constexpr u32 kButtonLeft = 0x110;
constexpr u32 kButtonRight = 0x111;
constexpr u32 kButtonMiddle = 0x112;
constexpr u32 kButtonSide = 0x113;
constexpr u32 kButtonExtra = 0x114;
constexpr u32 kButtonForward = 0x115;
constexpr u32 kButtonBack = 0x116;

// libinput's convention: a wheel notch is 15 units of continuous scrolling.
constexpr float kUnitsPerNotch = 15.0f;
constexpr u32 kDefaultCursorSize = 24;

bool pointer_button_of(u32 code, PointerButton* button) noexcept {
    switch (code) {
    case kButtonLeft:
        *button = PointerButton::left;
        return true;
    case kButtonRight:
        *button = PointerButton::right;
        return true;
    case kButtonMiddle:
        *button = PointerButton::middle;
        return true;
    case kButtonSide:
    case kButtonBack:
        *button = PointerButton::back;
        return true;
    case kButtonExtra:
    case kButtonForward:
        *button = PointerButton::forward;
        return true;
    default:
        return false;
    }
}

WindowEvent pointer_event(EventType type, u32 time_ms) noexcept {
    WindowEvent event = make_event(type, time_ms);
    event.pointer.x = g.pointer.x;
    event.pointer.y = g.pointer.y;
    event.pointer.buttons = g.pointer.buttons;
    return event;
}

void flush_motion() noexcept {
    PointerState& pointer = g.pointer;
    if (!pointer.motion_pending)
        return;
    pointer.motion_pending = false;
    ring_push(pointer_event(EventType::pointer_move, pointer.motion_time));
}

i32 whole_notches(float units, float* remainder) noexcept {
    const float total = *remainder + units * (120.0f / kUnitsPerNotch);
    const i32 whole = static_cast<i32>(total);
    *remainder = total - static_cast<float>(whole);
    return whole;
}

void flush_axis() noexcept {
    PointerState& pointer = g.pointer;
    if (!pointer.axis_pending)
        return;
    pointer.axis_pending = false;
    // Windows reports a positive vertical delta for a scroll away from the user (up) and a positive horizontal one
    // for right; Wayland's axis is positive down and right.
    float dx_units = pointer.axis_px_x;
    float dy_units = -pointer.axis_px_y;
    i32 dx120 = pointer.axis_value120_x;
    i32 dy120 = -pointer.axis_value120_y;
    if (dx120 == 0 && dy120 == 0) {
        dx120 = pointer.axis_discrete_x * 120;
        dy120 = -pointer.axis_discrete_y * 120;
    }
    if (dx120 == 0 && dy120 == 0) {
        dx120 = whole_notches(dx_units, &pointer.remainder_x);
        dy120 = whole_notches(dy_units, &pointer.remainder_y);
    }
    const float scale = pixel_scale();
    WindowEvent event = make_event(EventType::wheel, pointer.axis_time);
    event.modifiers = g.keyboard.modifiers;
    event.wheel.x = pointer.x;
    event.wheel.y = pointer.y;
    event.wheel.dx120 = dx120;
    event.wheel.dy120 = dy120;
    event.wheel.dx_px = dx_units * scale;
    event.wheel.dy_px = dy_units * scale;
    event.wheel.source = pointer.axis_source;
    pointer.axis_px_x = pointer.axis_px_y = 0;
    pointer.axis_value120_x = pointer.axis_value120_y = 0;
    pointer.axis_discrete_x = pointer.axis_discrete_y = 0;
    pointer.axis_source = wl_pointer_axis_source_wheel;
    if (dx120 != 0 || dy120 != 0 || event.wheel.dx_px != 0 || event.wheel.dy_px != 0)
        ring_push(event);
}

ClickPolicy click_policy() noexcept {
    const PlatformSettings settings = platform_settings_read();
    const float scale = pixel_scale();
    return {settings.double_click_ms, static_cast<float>(settings.double_click_dx_px) * scale,
            static_cast<float>(settings.double_click_dy_px) * scale};
}

void on_enter(void*, wl_pointer*, u32 serial, wl_surface*, i32 x, i32 y) noexcept {
    PointerState& pointer = g.pointer;
    pointer.inside = true;
    pointer.enter_serial = serial;
    pointer.x = fixed_to_float(x) * pixel_scale();
    pointer.y = fixed_to_float(y) * pixel_scale();
    pointer.applied_valid = false;  // the compositor forgets the cursor when the pointer leaves
    pointer.motion_pending = true;
    pointer.motion_time = now_ms32();
    flush_motion();
}

void on_leave(void*, wl_pointer*, u32, wl_surface*) noexcept {
    PointerState& pointer = g.pointer;
    flush_motion();
    flush_axis();
    pointer.inside = false;
    pointer.buttons = 0;
    pointer.enter_serial = 0;
    click_tracker_reset(&pointer.click);
    ring_push(pointer_event(EventType::pointer_leave, now_ms32()));
}

void on_motion(void*, wl_pointer*, u32 time, i32 x, i32 y) noexcept {
    PointerState& pointer = g.pointer;
    const float scale = pixel_scale();
    pointer.x = fixed_to_float(x) * scale;
    pointer.y = fixed_to_float(y) * scale;
    pointer.motion_pending = true;
    pointer.motion_time = time;
    if (g.seat_version < wl_pointer_frame_since)
        flush_motion();  // no frame events: every motion is its own frame
}

void on_button(void*, wl_pointer*, u32 serial, u32 time, u32 code, u32 state) noexcept {
    PointerState& pointer = g.pointer;
    flush_motion();
    PointerButton button;
    if (!pointer_button_of(code, &button))
        return;
    const bool pressed = state == wl_pointer_button_state_pressed;
    if (pressed) {
        pointer.buttons |= button_bit(button);
        pointer.press_serial = serial;
        g.input_serial = serial;
    } else {
        pointer.buttons = static_cast<u8>(pointer.buttons & ~button_bit(button));
    }
    WindowEvent event = pointer_event(pressed ? EventType::pointer_down : EventType::pointer_up, time);
    event.button = static_cast<u8>(button);
    event.modifiers = g.keyboard.modifiers;
    if (pressed && click_tracker_press(&pointer.click, static_cast<u8>(button), time, pointer.x, pointer.y, click_policy()))
        event.flags |= ev_double_click;
    ring_push(event);
}

void on_axis(void*, wl_pointer*, u32 time, u32 axis, i32 value) noexcept {
    PointerState& pointer = g.pointer;
    const float units = fixed_to_float(value);
    if (axis == wl_pointer_axis_vertical_scroll)
        pointer.axis_px_y += units;
    else
        pointer.axis_px_x += units;
    pointer.axis_pending = true;
    pointer.axis_time = time;
    if (g.seat_version < wl_pointer_frame_since)
        flush_axis();
}

void on_frame(void*, wl_pointer*) noexcept {
    flush_motion();
    flush_axis();
}

void on_axis_source(void*, wl_pointer*, u32 source) noexcept {
    g.pointer.axis_source = static_cast<u8>(source);
}

void on_axis_stop(void*, wl_pointer*, u32, u32) noexcept {}

void on_axis_discrete(void*, wl_pointer*, u32 axis, i32 discrete) noexcept {
    PointerState& pointer = g.pointer;
    (axis == wl_pointer_axis_vertical_scroll ? pointer.axis_discrete_y : pointer.axis_discrete_x) += discrete;
    pointer.axis_pending = true;
}

void on_axis_value120(void*, wl_pointer*, u32 axis, i32 value120) noexcept {
    PointerState& pointer = g.pointer;
    (axis == wl_pointer_axis_vertical_scroll ? pointer.axis_value120_y : pointer.axis_value120_x) += value120;
    pointer.axis_pending = true;
}

void on_axis_relative_direction(void*, wl_pointer*, u32, u32) noexcept {}

const wl_pointer_listener kPointerListener = {
    on_enter,       on_leave,           on_motion,       on_button,   on_axis,
    on_frame,       on_axis_source,     on_axis_stop,    on_axis_discrete,
    on_axis_value120, on_axis_relative_direction};

// Cursor --------------------------------------------------------------------------------------------------------------

// A UiCursor value, or kFrameCursorBase + a FrameHit (the resize cursors of the window's edges): the wp_cursor_shape
// shape of each, and the XCursor names (the CSS-style one first, the classic X11 ones after) tried without it, three
// to a row in kCursorNames, in the order of the rows. Any other code is the first row.
struct CursorKind {
    u8 code, shape;
};
#define MAICORD_FRAME_CURSOR(hit) kFrameCursorBase + static_cast<u8>(FrameHit::hit)
constexpr CursorKind kCursors[] = {
    {0xFF, wp_cursor_shape_device_v1_shape_default},
    {static_cast<u8>(UiCursor::hand), wp_cursor_shape_device_v1_shape_pointer},
    {static_cast<u8>(UiCursor::ibeam), wp_cursor_shape_device_v1_shape_text},
    {static_cast<u8>(UiCursor::resize_we), wp_cursor_shape_device_v1_shape_ew_resize},
    {static_cast<u8>(UiCursor::move), wp_cursor_shape_device_v1_shape_move},
    {MAICORD_FRAME_CURSOR(left), wp_cursor_shape_device_v1_shape_w_resize},
    {MAICORD_FRAME_CURSOR(right), wp_cursor_shape_device_v1_shape_e_resize},
    {MAICORD_FRAME_CURSOR(top), wp_cursor_shape_device_v1_shape_n_resize},
    {MAICORD_FRAME_CURSOR(bottom), wp_cursor_shape_device_v1_shape_s_resize},
    {MAICORD_FRAME_CURSOR(top_left), wp_cursor_shape_device_v1_shape_nw_resize},
    {MAICORD_FRAME_CURSOR(top_right), wp_cursor_shape_device_v1_shape_ne_resize},
    {MAICORD_FRAME_CURSOR(bottom_left), wp_cursor_shape_device_v1_shape_sw_resize},
    {MAICORD_FRAME_CURSOR(bottom_right), wp_cursor_shape_device_v1_shape_se_resize},
};
#undef MAICORD_FRAME_CURSOR
constexpr char kCursorNames[] =
    "default\0left_ptr\0arrow\0"
    "pointer\0hand2\0hand1\0"
    "text\0xterm\0ibeam\0"
    "ew-resize\0sb_h_double_arrow\0h_double_arrow\0"
    "move\0fleur\0all-scroll\0"
    "w-resize\0left_side\0sb_h_double_arrow\0"
    "e-resize\0right_side\0sb_h_double_arrow\0"
    "n-resize\0top_side\0sb_v_double_arrow\0"
    "s-resize\0bottom_side\0sb_v_double_arrow\0"
    "nw-resize\0top_left_corner\0ul_angle\0"
    "ne-resize\0top_right_corner\0ur_angle\0"
    "sw-resize\0bottom_left_corner\0ll_angle\0"
    "se-resize\0bottom_right_corner\0lr_angle";
static_assert([]() constexpr {
    u32 names = 0;
    for (const char c : kCursorNames)
        names += c == 0;
    return names;
}() == 3 * sizeof(kCursors) / sizeof(kCursors[0]), "three names to a row");

// The row of `code` in kCursors.
u32 cursor_row(u8 code) noexcept {
    for (u32 row = 1; row < sizeof(kCursors) / sizeof(kCursors[0]); ++row)
        if (kCursors[row].code == code)
            return row;
    return 0;
}

u32 parse_unsigned(const char* text) noexcept {
    u32 value = 0;
    if (!text)
        return 0;
    for (; *text >= '0' && *text <= '9'; ++text)
        value = value * 10 + static_cast<u32>(*text - '0');
    return value;
}

u32 integer_scale() noexcept {
    const u32 scale = (g.window.dpi + 95) / 96;
    return scale ? scale : 1;
}

// The cursor theme and nominal size: XCURSOR_THEME and XCURSOR_SIZE when set (the user's explicit choice), else the
// desktop's settings (cursor-theme, cursor-size from the settings portal), else the library's default theme at 24.
void choose_cursor_theme(char (&name)[64], u32* size) noexcept {
    dbus::DesktopSettings desktop;
    dbus::desktop_settings_get(&desktop);
    const char* theme = environment_get("XCURSOR_THEME");
    if (!theme || !*theme)
        theme = desktop.cursor_theme;
    u32 length = 0;
    while (theme[length] && length < sizeof(name) - 1) {
        name[length] = theme[length];
        ++length;
    }
    name[length] = 0;
    *size = parse_unsigned(environment_get("XCURSOR_SIZE"));
    if (*size == 0)
        *size = desktop.cursor_size;
    if (*size == 0)
        *size = kDefaultCursorSize;
}

void apply_cursor_theme(u8 code) noexcept {
    PointerState& pointer = g.pointer;
    const CursorApi* api = cursor_api();
    if (!api || !g.shm || !g.compositor)
        return;
    const u32 scale = integer_scale();
    char name[64];
    u32 size = 0;
    choose_cursor_theme(name, &size);
    if (pointer.theme && (pointer.theme_scale != scale || pointer.theme_size != size || !c_string_equal(pointer.theme_name, name))) {
        api->theme_destroy(pointer.theme);
        pointer.theme = nullptr;
    }
    if (!pointer.theme) {
        pointer.cursor_surface_valid = false;  // its image was the old theme's
        pointer.theme = api->theme_load(name[0] ? name : nullptr, static_cast<int>(size * scale), g.shm);
        pointer.theme_scale = scale;
        pointer.theme_size = size;
        copy_bytes(pointer.theme_name, name, sizeof(name));
        if (!pointer.theme)
            return;
    }
    // The cursor surface keeps the image it was given: a pointer that enters again only names it with the new serial.
    if (pointer.cursor_surface_valid && pointer.cursor_surface_code == code) {
        wl_pointer_set_cursor(pointer.pointer, pointer.enter_serial, pointer.cursor_surface, pointer.cursor_hotspot_x,
                              pointer.cursor_hotspot_y);
        return;
    }
    Cursor* shape = nullptr;
    const char* candidate = kCursorNames;
    for (u32 skip = 3 * cursor_row(code); skip; --skip)
        candidate += c_string_length(candidate) + 1;
    for (u32 tried = 0; tried < 3; ++tried, candidate += c_string_length(candidate) + 1) {
        shape = api->theme_get_cursor(pointer.theme, candidate);
        if (shape && shape->image_count > 0)
            break;
        shape = nullptr;
    }
    if (!shape)
        return;
    CursorImage* image = shape->images[0];
    wl_buffer* buffer = api->image_get_buffer(image);
    if (!buffer)
        return;
    if (!pointer.cursor_surface)
        pointer.cursor_surface = wl_compositor_create_surface(g.compositor);
    wl_surface_set_buffer_scale(pointer.cursor_surface, static_cast<i32>(scale));
    wl_surface_attach(pointer.cursor_surface, buffer, 0, 0);
    wl_surface_damage_buffer(pointer.cursor_surface, 0, 0, static_cast<i32>(image->width),
                             static_cast<i32>(image->height));
    wl_surface_commit(pointer.cursor_surface);
    pointer.cursor_surface_valid = true;
    pointer.cursor_surface_code = code;
    pointer.cursor_hotspot_x = static_cast<i32>(image->hotspot_x / scale);
    pointer.cursor_hotspot_y = static_cast<i32>(image->hotspot_y / scale);
    wl_pointer_set_cursor(pointer.pointer, pointer.enter_serial, pointer.cursor_surface, pointer.cursor_hotspot_x,
                          pointer.cursor_hotspot_y);
}

void release_pointer(wl_pointer* pointer) noexcept {
    if (g.seat_version >= wl_pointer_release_since)
        wl_pointer_release(pointer);
    else
        g_client.proxy_destroy(reinterpret_cast<Proxy*>(pointer));
}

void send_capture_lost(u32 time_ms) noexcept {
    WindowEvent event = make_event(EventType::capture_lost, time_ms);
    event.focus.gained = false;
    WindowEventResult result{};
    sink_call(event, &result);
}

} // namespace

void pointer_attach(wl_seat* seat) noexcept {
    if (g.pointer.pointer)
        return;
    g.pointer.pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(g.pointer.pointer, &kPointerListener, nullptr);
    if (g.cursor_shape_manager)
        g.pointer.shape_device = wp_cursor_shape_manager_v1_get_pointer(g.cursor_shape_manager, g.pointer.pointer);
}

void pointer_detach() noexcept {
    PointerState& pointer = g.pointer;
    if (pointer.shape_device)
        wp_cursor_shape_device_v1_destroy(pointer.shape_device);
    if (pointer.pointer)
        release_pointer(pointer.pointer);
    if (pointer.cursor_surface)
        wl_surface_destroy(pointer.cursor_surface);
    if (pointer.theme) {
        if (const CursorApi* api = cursor_api())
            api->theme_destroy(pointer.theme);
    }
    clear_bytes(&pointer, static_cast<u32>(sizeof(pointer)));
}

void pointer_before_dispatch(const WindowEvent& event) noexcept {
    PointerState& pointer = g.pointer;
    if (event.type == EventType::pointer_down)
        pointer.app_buttons |= button_bit(static_cast<PointerButton>(event.button));
    else if (event.type == EventType::pointer_up)
        pointer.app_buttons = static_cast<u8>(pointer.app_buttons & ~button_bit(static_cast<PointerButton>(event.button)));
}

void pointer_after_dispatch(const WindowEvent& event) noexcept {
    PointerState& pointer = g.pointer;
    if (event.type == EventType::pointer_leave)
        pointer.app_buttons = 0;
    const bool grab_ended = (event.type == EventType::pointer_up && pointer.app_buttons == 0) ||
                            event.type == EventType::pointer_leave;
    if (grab_ended && pointer.captured) {
        pointer.captured = false;
        send_capture_lost(event.time_ms);
    }
}

void pointer_cursor_query(const WindowEvent& move) noexcept {
    PointerState& pointer = g.pointer;
    if (!pointer.inside)
        return;
    // Over the frame the cursor is the frame's: an edge resizes, the caption is an arrow. Only the client area asks the app.
    if (g.caps.csd && g.frame.hover != FrameHit::client) {
        pointer.desired_cursor = static_cast<u8>(kFrameCursorBase + static_cast<u8>(g.frame.hover));
        cursor_apply();
        return;
    }
    WindowEvent query = make_event(EventType::cursor_query, move.time_ms);
    query.flags = static_cast<u8>(move.flags & ev_synthetic);
    query.pointer = move.pointer;
    pointer.in_cursor_query = true;
    pointer.desired_cursor = static_cast<u8>(UiCursor::arrow);  // what Windows gives a query nobody answers
    WindowEventResult result{};
    sink_call(query, &result);
    pointer.in_cursor_query = false;
    cursor_apply();
}

void cursor_apply() noexcept {
    PointerState& pointer = g.pointer;
    if (!pointer.pointer || !pointer.inside || pointer.enter_serial == 0)
        return;
    if (pointer.applied_valid && pointer.applied_cursor == pointer.desired_cursor)
        return;
    const u8 code = pointer.desired_cursor;
    if (code == static_cast<u8>(UiCursor::hidden))
        wl_pointer_set_cursor(pointer.pointer, pointer.enter_serial, nullptr, 0, 0);
    else if (pointer.shape_device)
        wp_cursor_shape_device_v1_set_shape(pointer.shape_device, pointer.enter_serial, kCursors[cursor_row(code)].shape);
    else
        apply_cursor_theme(code);
    pointer.applied_cursor = pointer.desired_cursor;
    pointer.applied_valid = true;
}

// A cursor-shape cursor is drawn by the compositor at the output's scale; only the wl_cursor fallback's image is ours.
void pointer_scale_changed() noexcept {
    PointerState& pointer = g.pointer;
    if (pointer.shape_device)
        return;
    pointer.applied_valid = false;
    cursor_apply();
}

} // namespace maicord::wayland

namespace maicord {

using namespace wayland;

void window_set_cursor(Window, UiCursor cursor) noexcept {
    g.pointer.desired_cursor = static_cast<u8>(cursor);
    if (!g.pointer.in_cursor_query)
        cursor_apply();
}

void window_request_cursor_refresh(Window) noexcept {
    PointerState& pointer = g.pointer;
    if (!pointer.inside)
        return;
    WindowEvent move = make_event(EventType::pointer_move, now_ms32());
    move.flags = ev_synthetic;
    move.pointer.x = pointer.x;
    move.pointer.y = pointer.y;
    move.pointer.buttons = pointer.app_buttons;
    pointer_cursor_query(move);
}

bool window_pointer_position(Window, float* x_px, float* y_px) noexcept {
    if (x_px)
        *x_px = g.pointer.x;
    if (y_px)
        *y_px = g.pointer.y;
    return g.pointer.inside;
}

void window_capture_pointer(Window) noexcept {
    // The implicit grab already delivers motion and the release while a button is held; the call records that the
    // app wants it. With no button down there is nothing to hold, which is the "system may refuse" case.
    if (g.pointer.app_buttons != 0)
        g.pointer.captured = true;
}

void window_release_capture(Window) noexcept {
    PointerState& pointer = g.pointer;
    if (!pointer.captured)
        return;
    pointer.captured = false;
    send_capture_lost(now_ms32());
}

bool window_pointer_captured(Window) noexcept {
    return g.pointer.captured;
}

} // namespace maicord
