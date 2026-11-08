// Client-side decorations: the app draws the frame, the compositor does the moving and sizing. On every pointer motion
// and press the window asks the sink where the point lies (hit_test, a synchronous query like WM_NCHITTEST) and acts
// on the answer: the caption starts xdg_toplevel.move (a double press maximizes or restores), an edge or corner starts
// xdg_toplevel.resize, the system-menu area and a right press on the caption open the compositor's window menu. The
// presses it takes never reach the app, and neither do their releases. Motion is always delivered, so the caption bar
// can track hover; the cursor over an edge is the resize cursor (pointer.cpp).
#include "backend.hpp"

namespace maicord::wayland {
namespace {

bool is_edge(FrameHit hit) noexcept {
    return hit >= FrameHit::left && hit <= FrameHit::bottom_right;
}

FrameHit hit_test(const WindowEvent& pointer_event) noexcept {
    WindowEvent query = make_event(EventType::hit_test, pointer_event.time_ms);
    query.pointer = pointer_event.pointer;
    WindowEventResult result{};
    sink_call(query, &result);
    if (!result.handled || result.value < 0 || result.value > static_cast<i32>(FrameHit::nowhere))
        return FrameHit::client;
    return static_cast<FrameHit>(result.value);
}

} // namespace

bool frame_filter(const WindowEvent& event) noexcept {
    FrameState& frame = g.frame;
    if (event.type != EventType::pointer_move && event.type != EventType::pointer_down &&
        event.type != EventType::pointer_up)
        return false;
    if (!g.caps.csd) {
        frame.hover = FrameHit::client;  // the compositor draws the frame
        return false;
    }
    const Window window = reinterpret_cast<Window>(&g.window);
    if (event.type == EventType::pointer_up) {
        const u8 bit = button_bit(static_cast<PointerButton>(event.button));
        if (!(frame.consumed_buttons & bit))
            return false;
        frame.consumed_buttons = static_cast<u8>(frame.consumed_buttons & ~bit);
        return true;
    }
    const FrameHit hit = hit_test(event);
    if (event.type == EventType::pointer_move) {
        frame.hover = hit;
        return false;
    }
    // A press.
    const PointerButton button = static_cast<PointerButton>(event.button);
    bool taken = false;
    if (button == PointerButton::left) {
        if (hit == FrameHit::caption) {
            taken = true;
            if (event.flags & ev_double_click)
                window_toggle_maximize(window);
            else
                begin_move();
        } else if (is_edge(hit)) {
            taken = true;
            begin_resize(hit);
        } else if (hit == FrameHit::system_menu) {
            taken = true;
            window_show_system_menu(window, event.pointer.x, event.pointer.y);
        }
    } else if (button == PointerButton::right && (hit == FrameHit::caption || hit == FrameHit::system_menu)) {
        taken = true;
        window_show_system_menu(window, event.pointer.x, event.pointer.y);
    }
    if (taken)
        frame.consumed_buttons |= button_bit(button);
    return taken;
}

} // namespace maicord::wayland
