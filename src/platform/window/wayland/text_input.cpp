// zwp_text_input_v3: the compositor's input method (IME) talks to the window through it. The app says when a text field
// has focus (enable, with the text around the caret and where the caret is on screen) and the IME answers with
// transactions: delete_surrounding_text, commit_string and preedit_string, applied together at `done`. Each becomes an
// ime_* WindowEvent (UTF-16, as everything the app reads); a compositor without text-input leaves plain xkb text.
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/platform/window/wayland.hpp"
#include "backend.hpp"

namespace maicord::wayland {
namespace {

constexpr u32 kSurroundingMaximum = 4000;  // bytes the protocol allows

u32 copy_text(char* destination, u32 capacity, const char* text) noexcept {
    u32 length = 0;
    while (text && text[length] && length + 1 < capacity) {
        destination[length] = text[length];
        ++length;
    }
    destination[length] = 0;
    return length;
}

// The UTF-16 units of `bytes` of UTF-8 text, in the ring the ime events point into; null when it does not fit.
const u16* store_units(const char* text, u32 bytes, u32* count) noexcept {
    TextState& state = g.text;
    *count = 0;
    if (bytes == 0)
        return state.units;
    const u32 need = utf8_utf16_units(Utf8View{text, bytes});
    if (need > kImeUnits)
        return nullptr;
    if (state.unit_cursor + need > kImeUnits)
        state.unit_cursor = 0;
    u32 written = 0;
    u32 next = 0;
    u16* destination = state.units + state.unit_cursor;
    // The range is in UTF-16 positions (the text model's), not bytes: all of it.
    if (!utf8_copy_utf16_range(Utf8View{text, bytes}, 0, need, Slice<u16>{destination, need}, &written, &next))
        return nullptr;
    state.unit_cursor += written;
    *count = written;
    return destination;
}

// A byte offset into the preedit as a UTF-16 position; -1 where the IME sent none.
i32 preedit_position(i32 byte_offset) noexcept {
    TextState& state = g.text;
    if (byte_offset < 0 || static_cast<u32>(byte_offset) > state.preedit_bytes)
        return -1;
    u32 position = 0;
    if (!utf8_utf16_position_for_byte_offset(Utf8View{state.preedit, state.preedit_bytes}, static_cast<u32>(byte_offset),
                                             &position))
        return -1;
    return static_cast<i32>(position);
}

// UTF-16 units in `bytes` bytes of the surrounding text starting at `from`.
u32 surrounding_units(u32 from, u32 bytes) noexcept {
    TextState& state = g.text;
    if (from > state.surrounding_bytes || bytes > state.surrounding_bytes - from)
        return bytes;  // the IME counted past what we sent: take its count
    return utf8_utf16_units(Utf8View{state.surrounding + from, bytes});
}

void send_request(const TextInputRequest& request) noexcept;

TextInputRequest g_wanted;
char g_wanted_text[kSurroundingMaximum + 4];

void on_enter(void*, zwp_text_input_v3*, wl_surface*) noexcept {
    g.text.entered = true;
    // What the app asked for before the text input had the focus is sent now.
    if (g.text.wanted) {
        g.text.enabled = false;
        send_request(g_wanted);
    }
}

void on_leave(void*, zwp_text_input_v3*, wl_surface*) noexcept {
    TextState& state = g.text;
    state.entered = false;
    state.enabled = false;
    state.preedit_present = false;
    state.commit_present = false;
    state.delete_before = state.delete_after = 0;
    if (state.preedit_bytes != 0) {
        state.preedit_bytes = 0;
        WindowEvent event = make_event(EventType::ime_preedit, now_ms32());
        event.ime.text = state.units;
        event.ime.cursor_begin = event.ime.cursor_end = -1;
        ring_push(event);
    }
}

void on_preedit_string(void*, zwp_text_input_v3*, const char* text, i32 begin, i32 end) noexcept {
    TextState& state = g.text;
    state.preedit_present = text != nullptr;
    state.preedit_bytes = copy_text(state.preedit, kImeBytes, text);
    state.preedit_begin = begin;
    state.preedit_end = end;
}

void on_commit_string(void*, zwp_text_input_v3*, const char* text) noexcept {
    TextState& state = g.text;
    state.commit_present = text != nullptr;
    state.commit_bytes = copy_text(state.commit, kImeBytes, text);
}

void on_delete_surrounding(void*, zwp_text_input_v3*, u32 before, u32 after) noexcept {
    g.text.delete_before = before;
    g.text.delete_after = after;
}

void on_done(void*, zwp_text_input_v3*, u32) noexcept {
    TextState& state = g.text;
    const u32 time = now_ms32();
    // The IME's changes in the order the protocol gives them: delete around the caret, insert, then the new preedit.
    if (state.delete_before != 0 || state.delete_after != 0) {
        WindowEvent event = make_event(EventType::ime_delete_surrounding, time);
        const u32 cursor = state.cursor_byte;
        event.ime.delete_before = surrounding_units(cursor >= state.delete_before ? cursor - state.delete_before : 0,
                                                    minimum(cursor, state.delete_before));
        event.ime.delete_after = surrounding_units(cursor, state.delete_after);
        ring_push(event);
    }
    if (state.commit_present && state.commit_bytes != 0) {
        WindowEvent event = make_event(EventType::ime_commit, time);
        u32 count = 0;
        event.ime.text = store_units(state.commit, state.commit_bytes, &count);
        event.ime.count = count;
        event.ime.cursor_begin = event.ime.cursor_end = -1;
        if (event.ime.text)
            ring_push(event);
    }
    if ((state.preedit_present && state.preedit_bytes != 0) || state.had_preedit) {
        WindowEvent event = make_event(EventType::ime_preedit, time);
        u32 count = 0;
        event.ime.text = state.preedit_present ? store_units(state.preedit, state.preedit_bytes, &count) : state.units;
        event.ime.count = count;
        event.ime.cursor_begin = preedit_position(state.preedit_begin);
        event.ime.cursor_end = preedit_position(state.preedit_end);
        if (event.ime.text)
            ring_push(event);
        state.had_preedit = count != 0;
    }
    state.commit_present = false;
    state.commit_bytes = 0;
    state.delete_before = state.delete_after = 0;
    state.preedit_present = false;
    state.preedit_bytes = 0;
    state.preedit_begin = state.preedit_end = -1;
}

const zwp_text_input_v3_listener kListener = {on_enter,           on_leave,       on_preedit_string,
                                              on_commit_string,   on_delete_surrounding, on_done};

u32 purpose_of(TextInputPurpose purpose) noexcept {
    switch (purpose) {
    case TextInputPurpose::digits:
        return zwp_text_input_v3_content_purpose_digits;
    case TextInputPurpose::number:
        return zwp_text_input_v3_content_purpose_number;
    case TextInputPurpose::phone:
        return zwp_text_input_v3_content_purpose_phone;
    case TextInputPurpose::url:
        return zwp_text_input_v3_content_purpose_url;
    case TextInputPurpose::email:
        return zwp_text_input_v3_content_purpose_email;
    case TextInputPurpose::password:
        return zwp_text_input_v3_content_purpose_password;
    case TextInputPurpose::pin:
        return zwp_text_input_v3_content_purpose_pin;
    default:
        return zwp_text_input_v3_content_purpose_normal;
    }
}

// Sends the whole input state and commits it.
void send_request(const TextInputRequest& request) noexcept {
    TextState& state = g.text;
    zwp_text_input_v3* input = state.input;
    if (!state.enabled) {
        zwp_text_input_v3_enable(input);
        state.enabled = true;
    }
    // The text around the caret, at most what the protocol carries; a field holding secrets sends none.
    u32 start = 0;
    u32 length = 0;
    u32 cursor = 0;
    u32 anchor = 0;
    if (!request.sensitive && request.surrounding.data) {
        const Utf8View whole = request.surrounding;
        length = minimum(whole.bytes, kSurroundingMaximum);
        if (whole.bytes > kSurroundingMaximum) {
            // Centre the window on the caret and keep it on character boundaries.
            const u32 want = request.cursor_byte > kSurroundingMaximum / 2 ? request.cursor_byte - kSurroundingMaximum / 2 : 0;
            start = want;
            while (start < whole.bytes && !utf8_is_boundary(whole, start))
                ++start;
            length = minimum(whole.bytes - start, kSurroundingMaximum);
            length = utf8_truncated_length(Utf8View{whole.data + start, whole.bytes - start}, length);
        }
        cursor = request.cursor_byte >= start ? minimum(request.cursor_byte - start, length) : 0;
        anchor = request.anchor_byte >= start ? minimum(request.anchor_byte - start, length) : 0;
        copy_bytes(state.surrounding, whole.data + start, length);
    }
    state.surrounding[length] = 0;
    state.surrounding_bytes = length;
    state.cursor_byte = cursor;
    zwp_text_input_v3_set_surrounding_text(input, state.surrounding, static_cast<i32>(cursor), static_cast<i32>(anchor));
    zwp_text_input_v3_set_text_change_cause(input, zwp_text_input_v3_change_cause_other);
    u32 hint = zwp_text_input_v3_content_hint_none;
    if (request.sensitive)
        hint |= zwp_text_input_v3_content_hint_hidden_text | zwp_text_input_v3_content_hint_sensitive_data;
    zwp_text_input_v3_set_content_type(input, hint, purpose_of(request.purpose));
    // Surface-local logical coordinates: the client pixels the app works in, divided by the scale.
    const u32 dpi = g.window.dpi ? g.window.dpi : 96;
    zwp_text_input_v3_set_cursor_rectangle(input, request.x * 96 / static_cast<i32>(dpi),
                                           request.y * 96 / static_cast<i32>(dpi),
                                           maximum<i32>(request.width * 96 / static_cast<i32>(dpi), 1),
                                           maximum<i32>(request.height * 96 / static_cast<i32>(dpi), 1));
    zwp_text_input_v3_commit(input);
    ++state.commits;
    g_client.display_flush(g.display);
}


} // namespace

void text_attach() noexcept {
    if (!g.seat || !g.text_input_manager)
        return;
    g.text.input = zwp_text_input_manager_v3_get_text_input(g.text_input_manager, g.seat);
    zwp_text_input_v3_add_listener(g.text.input, &kListener, nullptr);
}

void text_shutdown() noexcept {
    if (g.text.input)
        zwp_text_input_v3_destroy(g.text.input);
    clear_bytes(&g.text, static_cast<u32>(sizeof(g.text)));
}

bool text_input_enable(const TextInputRequest& request) noexcept {
    TextState& state = g.text;
    if (!state.input)
        return false;
    // Kept (with its own copy of the text) so it can be sent when the compositor gives the text input focus later.
    g_wanted = request;
    g_wanted.surrounding = Utf8View{};
    if (request.surrounding.data && !request.sensitive) {
        const u32 length = utf8_truncated_length(request.surrounding, kSurroundingMaximum);
        copy_bytes(g_wanted_text, request.surrounding.data, length);
        g_wanted.surrounding = Utf8View{g_wanted_text, length};
    }
    state.wanted = true;
    if (state.entered)
        send_request(request);
    return true;
}

void text_input_disable() noexcept {
    TextState& state = g.text;
    state.wanted = false;
    if (state.input && state.enabled) {
        zwp_text_input_v3_disable(state.input);
        zwp_text_input_v3_commit(state.input);
        ++state.commits;
        state.enabled = false;
        g_client.display_flush(g.display);
    }
}

} // namespace maicord::wayland

namespace maicord {

using namespace wayland;

bool window_wayland_text_input_available(NativeWindow window) noexcept {
    return window && window == g.window.surface && g.window_exists && g.text.input != nullptr;
}

bool window_wayland_text_input_enable(NativeWindow window, const TextInputRequest& request) noexcept {
    return window_wayland_text_input_available(window) && text_input_enable(request);
}

void window_wayland_text_input_disable(NativeWindow window) noexcept {
    if (window_wayland_text_input_available(window))
        text_input_disable();
}

} // namespace maicord
