#include "../src/app/application_login_geometry.hpp"
#include "maicord/ui/text.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {
void test_application_login_geometry(void (*check)(bool, const char*) noexcept) noexcept {
    using app_detail::application_login_range_geometry;
    const auto valid_result = [](SecureEditResult result) noexcept {
        return result == SecureEditResult::changed || result == SecureEditResult::unchanged;
    };
    constexpr u32 dpis[]{96, 144, 192};
    {
        TextSystem text{}; SecureEdit edit{}; TextEditLayoutOwner layout{};
        TextEditMetric selections[kTextEditSelectionCapacity]{};
        layout.selections = selections; layout.selection_capacity = kTextEditSelectionCapacity;
        const bool ready = text_system_initialize(&text, L"Segoe UI", 16).ok() &&
            secure_edit_initialize(&edit, 512, false);
        check(ready, "login ACP geometry text provider initialization");
        if (ready) {
            const UiRect bounds{12.5f, 24.25f, 172.5f, 64.25f};
            const POINT origin{-320, 180};
            const Utf8View samples[]{utf8_literal("AV café"), utf8_literal("אבג العربية 123"),
                utf8_literal("a\u0301 👩🏽‍💻 🇸🇮"),
                utf8_literal("012345678901234567890123456789012345678901234567890123456789😀"),
                utf8_literal("")};
            for (const auto sample : samples) {
                check(valid_result(secure_edit_assign(&edit, sample)), "login ACP geometry fixture assign");
                char display[48 * 4]{}; u32 bytes = 0; SecureEditRenderWindow window{};
                const bool rendered = secure_edit_render_window(edit, 48, {display, sizeof(display)}, &bytes, &window) ==
                    SecureEditResult::unchanged;
                u32 anchor = 0, caret = 0, first = 0, last = 0;
                const bool mapped = rendered && utf8_utf16_position_for_codepoint({display, bytes}, window.anchor, &anchor) &&
                    utf8_utf16_position_for_codepoint({display, bytes}, window.caret, &caret) &&
                    utf8_utf16_position_for_codepoint(sample, window.first_codepoint, &first) &&
                    utf8_utf16_position_for_codepoint(sample, window.first_codepoint + window.codepoints, &last);
                const TextEditLayoutInput input{{display, bytes}, text_edit_signature({display, bytes}),
                    anchor, caret, bounds.left, bounds.top, ui_rect_width(bounds), ui_rect_height(bounds),
                    ui_rect_height(bounds), text_edit_layout_center_vertically, {}};
                const bool measured = mapped && text_edit_layout_measure(&layout, input, text);
                check(measured, "login ACP real retained layout measured");
                if (!measured) continue;
                for (u32 dpi : dpis) {
                    RECT rectangle{}; bool clipped = false;
                    const bool range = application_login_range_geometry(edit, layout, bounds, dpi, origin,
                        first, last, &rectangle, &clipped);
                    check(range && rectangle.right > rectangle.left && rectangle.bottom > rectangle.top,
                        "login ACP visible range and empty caret have pixel extent at each DPI");
                    check(rectangle.left >= origin.x + static_cast<LONG>(bounds.left * dpi / 96) - 1 &&
                        rectangle.right <= origin.x + static_cast<LONG>(bounds.right * dpi / 96) + 1,
                        "login ACP bidi and grapheme geometry stays within clipped field");
                    if (first) {
                        check(application_login_range_geometry(edit, layout, bounds, dpi, origin, 0, first,
                            &rectangle, &clipped) && clipped && !rectangle.left && !rectangle.top &&
                            !rectangle.right && !rectangle.bottom, "login ACP completely hidden prefix has zero geometry");
                        check(application_login_range_geometry(edit, layout, bounds, dpi, origin, 0, last,
                            &rectangle, &clipped) && clipped, "login ACP partial prefix rebases and flags clipping");
                    }
                }
                RECT sentinel{1,2,3,4}; bool untouched = false;
                check(!application_login_range_geometry(edit, layout, bounds, 96, origin, last + 1, last + 1,
                    &sentinel, &untouched) && sentinel.left == 1 && !untouched,
                    "login ACP invalid range fails atomically");
                const auto signature = layout.text_signature; layout.text_signature ^= 1;
                check(!application_login_range_geometry(edit, layout, bounds, 96, origin, first, last,
                    &sentinel, &untouched), "login ACP stale retained text is rejected");
                layout.text_signature = signature;
                auto moved_bounds = bounds; moved_bounds.top += 1; moved_bounds.bottom += 1;
                check(!application_login_range_geometry(edit, layout, moved_bounds, 96, origin, first, last,
                    &sentinel, &untouched), "login ACP stale field geometry is rejected");
            }
            check(valid_result(secure_edit_assign(&edit, utf8_literal("😀"))), "login ACP surrogate fixture");
            RECT rectangle{}; bool clipped = false;
            check(!application_login_range_geometry(edit, layout, {12.5f,24.25f,172.5f,64.25f}, 96,
                {0,0}, 1, 1, &rectangle, &clipped), "login ACP interior surrogate is rejected");
            edit.password = true;
            check(!application_login_range_geometry(edit, layout, {12.5f,24.25f,172.5f,64.25f}, 96,
                {0,0}, 0, 0, &rectangle, &clipped), "login ACP password geometry is unavailable");
        }
        text_edit_layout_release(&layout); secure_edit_release(&edit); text_system_release(&text);
    }
}
} // namespace maicord
