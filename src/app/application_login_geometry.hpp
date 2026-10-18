#pragma once

#include "maicord/ui/secure_edit.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/render/text_edit_layout.hpp"
#include "maicord/ui/login_view.hpp"

#include <Windows.h>

namespace maicord::app_detail {

inline u64& application_phone_tsf_mutation_stamp() noexcept {
    static u64 stamp{}; return stamp;
}

inline bool application_phone_edit_ready(const SecureEdit* edit) noexcept {
    return edit && edit->storage.capacity >= kLoginPhoneCodeDigits && !edit->password &&
        edit->storage.data && edit->storage.locked &&
        edit->anchor <= kLoginPhoneCodeDigits && edit->caret <= kLoginPhoneCodeDigits &&
        login_phone_cells_valid({reinterpret_cast<const char*>(edit->storage.data), edit->storage.bytes});
}
inline bool application_phone_materialize(SecureEdit* edit) noexcept {
    if (!application_phone_edit_ready(edit)) return false;
    // Materializing projected empty ACP cells changes neither text nor its
    // revision; subsequent generic rendering sees the same six-cell document.
    for (u32 i = edit->storage.bytes; i < kLoginPhoneCodeDigits; ++i) edit->storage.data[i] = ' ';
    edit->storage.bytes = kLoginPhoneCodeDigits;
    return true;
}
inline char application_phone_cell(const SecureEdit& edit, u32 cell) noexcept {
    return cell < edit.storage.bytes ? static_cast<char>(edit.storage.data[cell]) : ' ';
}
inline SecureEditResult application_phone_select(SecureEdit* edit, TextInputSelection selection) noexcept {
    if (!application_phone_edit_ready(edit) || selection.anchor > kLoginPhoneCodeDigits ||
        selection.caret > kLoginPhoneCodeDigits) return SecureEditResult::invalid_input;
    if (!application_phone_materialize(edit)) return SecureEditResult::invalid_input;
    const bool changed = edit->anchor != selection.anchor || edit->caret != selection.caret;
    edit->anchor = selection.anchor; edit->caret = selection.caret;
    edit->pending_high_surrogate = 0;
    return changed ? SecureEditResult::changed : SecureEditResult::unchanged;
}
// A fixed six-ACP document maps directly to six independent cells. Mutations
// overwrite in place; deleting a range clears its cells without shifting later
// digits. No secondary code bank or model allocation is needed.
inline SecureEditResult application_phone_replace(SecureEdit* edit, u32 first, u32 last,
    ConstSlice<u16> input, TextInputChange* change) noexcept {
    if (!application_phone_edit_ready(edit) || !change || first > last || last > kLoginPhoneCodeDigits ||
        (!input.data && input.count) || input.count > kLoginPhoneCodeDigits - first)
        return SecureEditResult::invalid_input;
    for (u32 i = 0; i < input.count; ++i)
        if (input[i] < '0' || input[i] > '9') return SecureEditResult::invalid_input;
    char cells[kLoginPhoneCodeDigits]{};
    for (u32 i = 0; i < kLoginPhoneCodeDigits; ++i) cells[i] = application_phone_cell(*edit, i);
    const u32 finish = maximum(last, first + input.count);
    for (u32 i = first; i < last; ++i) cells[i] = ' ';
    for (u32 i = 0; i < input.count; ++i) cells[first + i] = static_cast<char>(input[i]);
    bool changed = false;
    for (u32 i = 0; i < kLoginPhoneCodeDigits; ++i)
        changed |= cells[i] != application_phone_cell(*edit, i);
    SecureEditResult result = SecureEditResult::unchanged;
    if (!application_phone_materialize(edit)) { SecureZeroMemory(cells, sizeof(cells)); return SecureEditResult::invalid_input; }
    if (changed) result = secure_edit_assign(edit, {cells, kLoginPhoneCodeDigits});
    SecureZeroMemory(cells, sizeof(cells));
    if (result != SecureEditResult::changed && result != SecureEditResult::unchanged) return result;
    edit->anchor = edit->caret = first + input.count;
    edit->pending_high_surrogate = 0;
    *change = {first, finish, finish};
    if (result == SecureEditResult::changed)
        application_phone_tsf_mutation_stamp() = static_cast<u64>(edit->revision) + 1;
    return result;
}

// Ordinary MFA continues to bind the original SecureEdit operations.
inline TextInputTarget application_phone_input_target(SecureEdit* edit) noexcept {
    static constexpr TextInputOperations operations{
        [](const TextInputTarget& t) noexcept {
            auto* e = static_cast<SecureEdit*>(t.context);
            return application_phone_edit_ready(e);
        },
        [](const TextInputTarget& t) noexcept { return static_cast<SecureEdit*>(t.context)->revision; },
        [](const TextInputTarget&, u32* n) noexcept {
            if (!n) return TextInputResult::invalid_input; *n = kLoginPhoneCodeDigits; return TextInputResult::unchanged;
        },
        [](const TextInputTarget& t, TextInputSelection* s) noexcept {
            if (!s) return TextInputResult::invalid_input;
            const auto& e = *static_cast<SecureEdit*>(t.context); *s = {e.anchor, e.caret}; return TextInputResult::unchanged;
        },
        [](const TextInputTarget& t, TextInputSelection s) noexcept { return application_phone_select(static_cast<SecureEdit*>(t.context), s); },
        [](const TextInputTarget& t, u32 a, u32 b, Slice<u16> out, u32* n, u32* next) noexcept {
            if (!n || !next || a > b || b > kLoginPhoneCodeDigits || (!out.data && out.count)) return TextInputResult::invalid_input;
            const auto& e = *static_cast<SecureEdit*>(t.context);
            *n = minimum(b - a, out.count); *next = a + *n;
            for (u32 i = 0; i < *n; ++i) out[i] = static_cast<u16>(application_phone_cell(e, a + i));
            return TextInputResult::unchanged;
        },
        [](const TextInputTarget& t, u32 a, u32 b, ConstSlice<u16> in, TextInputChange* change) noexcept {
            return application_phone_replace(static_cast<SecureEdit*>(t.context), a, b, in, change);
        },
        [](const TextInputTarget& t) noexcept { static_cast<SecureEdit*>(t.context)->pending_high_surrogate = 0; },
        [](const TextInputTarget&) noexcept { return true; },
        [](const TextInputTarget&) noexcept {},
    };
    return {edit, &operations, 0};
}

inline SecureEditResult application_phone_insert_utf8(SecureEdit* edit, Utf8View text) noexcept {
    if (!edit || !login_phone_code_valid(text, false)) return SecureEditResult::invalid_input;
    u16 digits[kLoginPhoneCodeDigits]{};
    for (u32 i = 0; i < text.bytes; ++i) digits[i] = static_cast<u16>(text.data[i]);
    TextInputChange change{};
    const auto result = text_input_replace(application_phone_input_target(edit), minimum(edit->anchor, edit->caret),
        maximum(edit->anchor, edit->caret), {digits, text.bytes}, &change);
    SecureZeroMemory(digits, sizeof(digits));
    if (result == SecureEditResult::changed) {
        u32 first_empty = 0;
        while (first_empty < kLoginPhoneCodeDigits && application_phone_cell(*edit, first_empty) != ' ') ++first_empty;
        application_phone_select(edit, {first_empty, first_empty});
    }
    return result;
}

inline SecureEditResult application_phone_move(SecureEdit* edit, SecureEditMove movement, bool extend) noexcept {
    if (!application_phone_edit_ready(edit)) return SecureEditResult::invalid_input;
    u32 position = edit->caret;
    const bool selected = edit->anchor != edit->caret;
    if (movement == SecureEditMove::previous) position = selected && !extend ? minimum(edit->anchor, edit->caret) : position ? position - 1 : 0;
    else if (movement == SecureEditMove::next) position = selected && !extend ? maximum(edit->anchor, edit->caret) : minimum(position + 1, kLoginPhoneCodeDigits);
    else if (movement == SecureEditMove::home || movement == SecureEditMove::line_home) position = 0;
    else if (movement == SecureEditMove::end || movement == SecureEditMove::line_end) position = kLoginPhoneCodeDigits;
    return application_phone_select(edit, {extend ? edit->anchor : position, position});
}
inline SecureEditResult application_phone_select_all(SecureEdit* edit) noexcept {
    return application_phone_select(edit, {0, kLoginPhoneCodeDigits});
}
inline SecureEditResult application_phone_erase(SecureEdit* edit, bool backward) noexcept {
    if (!application_phone_edit_ready(edit)) return SecureEditResult::invalid_input;
    const TextInputSelection original{edit->anchor, edit->caret};
    u32 first = minimum(edit->anchor, edit->caret), last = maximum(edit->anchor, edit->caret);
    if (first == last) {
        if (backward && first && (first == kLoginPhoneCodeDigits || application_phone_cell(*edit, first) == ' ')) --first;
        if (first == kLoginPhoneCodeDigits) return SecureEditResult::unchanged;
        last = first + 1;
    }
    TextInputChange change{};
    const auto result = application_phone_replace(edit, first, last, {}, &change);
    if (result != SecureEditResult::unchanged) return result;
    // Empty Backspace still moves left even when that preceding slot is empty.
    return original.anchor != first || original.caret != first ? SecureEditResult::changed : SecureEditResult::unchanged;
}
inline SecureEditResult application_phone_backspace(SecureEdit* edit) noexcept { return application_phone_erase(edit, true); }
inline SecureEditResult application_phone_delete_forward(SecureEdit* edit) noexcept { return application_phone_erase(edit, false); }

// Editing a code field withdraws the error shown for its previous value.
inline void application_login_code_edited(LoginStatus* status) noexcept {
    if (*status == LoginStatus::mfa_input_invalid)
        *status = LoginStatus::mfa_required;
    else if (*status == LoginStatus::recovery_phone_input_invalid ||
             *status == LoginStatus::recovery_phone_code_rejected)
        *status = LoginStatus::recovery_phone_required;
}

// Device-pixel rectangle covering a DIP rectangle at `dpi`, offset by `origin`.
inline RECT application_login_pixel_rect(const UiRect& rectangle, u32 dpi, POINT origin) noexcept {
    const float scale = static_cast<float>(dpi) / 96.0f;
    const auto floor_pixel = [](float value) noexcept {
        const LONG pixel = static_cast<LONG>(value);
        return pixel - (static_cast<float>(pixel) > value ? 1 : 0);
    };
    const auto ceil_pixel = [](float value) noexcept {
        const LONG pixel = static_cast<LONG>(value);
        return pixel + (static_cast<float>(pixel) < value ? 1 : 0);
    };
    return {origin.x + floor_pixel(rectangle.left * scale), origin.y + floor_pixel(rectangle.top * scale),
            origin.x + ceil_pixel(rectangle.right * scale), origin.y + ceil_pixel(rectangle.bottom * scale)};
}

inline bool application_phone_range_geometry(const SecureEdit& edit, UiRect bounds, u32 dpi,
    POINT origin, u32 first, u32 last, RECT* output, bool* clipped, const UiRect* visible_bounds = nullptr) noexcept {
    if (!output || !clipped || !dpi || !application_phone_edit_ready(&edit) || first > last || last > kLoginPhoneCodeDigits) return false;
    const auto original = login_phone_range_rectangle(bounds, first, last);
    if (!ui_rect_has_area(original)) return false;
    const auto rectangle = ui_rect_intersection(original, visible_bounds ? *visible_bounds : bounds);
    *clipped = !ui_rect_equal(original, rectangle);
    if (!ui_rect_has_area(rectangle)) { *output = {}; *clipped = true; return true; }
    *output = application_login_pixel_rect(rectangle, dpi, origin);
    return true;
}

// Model ACP is relative to the complete locked field; retained geometry is
// relative to its scalar-safe 48-codepoint display window. This helper owns no
// state and is shared by the production TSF callback and its geometry fixture.
inline bool application_login_range_geometry(const SecureEdit& edit,
    const TextEditLayoutOwner& layout, UiRect bounds, u32 dpi, POINT screen_origin,
    u32 first, u32 last, RECT* output, bool* clipped) noexcept {
    if (!output || !clipped || edit.password || !dpi || !layout.active ||
        !ui_rect_has_area(bounds) || first > last ||
        layout.viewport_left != bounds.left || layout.viewport_top != bounds.top ||
        layout.width != ui_rect_width(bounds) || layout.height != ui_rect_height(bounds)) return false;
    const auto model = secure_edit_view(edit);
    const Utf8View source{reinterpret_cast<const char*>(model.data), model.count};
    u32 ignored = 0;
    if (!utf8_byte_offset_for_utf16_position(source, first, &ignored) ||
        !utf8_byte_offset_for_utf16_position(source, last, &ignored)) return false;
    char display[48 * 4]{};
    u32 bytes = 0;
    SecureEditRenderWindow window{};
    const auto rendered = secure_edit_render_window(edit, 48, {display, sizeof(display)}, &bytes, &window);
    const bool fresh = rendered == SecureEditResult::unchanged &&
        layout.text_bytes == bytes && layout.text_signature == text_edit_signature({display, bytes});
    SecureZeroMemory(display, sizeof(display));
    u32 visible_first = 0, visible_last = 0;
    if (!fresh || !utf8_utf16_position_for_codepoint(source, window.first_codepoint, &visible_first) ||
        !utf8_utf16_position_for_codepoint(source, window.first_codepoint + window.codepoints, &visible_last) ||
        visible_last - visible_first != layout.utf16_units) return false;
    bool range_clipped = first < visible_first || last > visible_last;
    // TSF requires zero geometry when a requested range is completely hidden.
    if (last < visible_first || first > visible_last ||
        (first < last && (last == visible_first || first == visible_last))) {
        *output = {}; *clipped = true; return true;
    }
    const u32 a = maximum(first, visible_first) - visible_first;
    const u32 b = minimum(last, visible_last) - visible_first;
    UiRect rectangle{};
    if (a == b) {
        TextEditMetric metric{};
        if (!text_edit_layout_position_geometry(layout, a, &metric)) return false;
        rectangle = {metric.left, metric.top, metric.left + 1, metric.top + metric.height};
        const float left = minimum(maximum(rectangle.left, bounds.left), bounds.right - 1);
        range_clipped = range_clipped || left != rectangle.left;
        rectangle.left = left; rectangle.right = left + 1;
    } else {
        TextEditMetric metrics[kTextEditSelectionCapacity]{};
        u32 count = 0;
        if (!text_edit_layout_range_geometry(layout, a, b, {metrics, kTextEditSelectionCapacity}, &count) ||
            !count) return false;
        rectangle = {metrics[0].left, metrics[0].top, metrics[0].left + metrics[0].width,
                     metrics[0].top + metrics[0].height};
        for (u32 index = 1; index < count; ++index) {
            rectangle.left = minimum(rectangle.left, metrics[index].left);
            rectangle.top = minimum(rectangle.top, metrics[index].top);
            rectangle.right = maximum(rectangle.right, metrics[index].left + metrics[index].width);
            rectangle.bottom = maximum(rectangle.bottom, metrics[index].top + metrics[index].height);
        }
    }
    const auto visible = ui_rect_intersection(rectangle, bounds);
    range_clipped = range_clipped || !ui_rect_equal(rectangle, visible);
    if (!ui_rect_has_area(visible)) { *output = {}; *clipped = true; return true; }
    *output = application_login_pixel_rect(visible, dpi, screen_origin);
    *clipped = range_clipped;
    return true;
}

} // namespace maicord::app_detail
