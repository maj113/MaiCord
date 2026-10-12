#pragma once

#include "application_state.hpp"
#include "maicord/ui/text_pointer_selection.hpp"

#include <Windows.h>

namespace maicord::app_detail {

// System double-click time and rectangle, in client DIPs.
inline UiClickPolicy application_click_policy() noexcept {
    const float scale = 96.0f / static_cast<float>(maximum(g_app.renderer.dpi, 1u));
    const UINT interval = GetDoubleClickTime();
    return {interval ? static_cast<u32>(interval) : 500u,
            static_cast<float>(maximum(GetSystemMetrics(SM_CXDOUBLECLK), 1)) * scale * 0.5f,
            static_cast<float>(maximum(GetSystemMetrics(SM_CYDOUBLECLK), 1)) * scale * 0.5f};
}

// Click count (1..3) for the current primary-button message on `owner`.
inline u8 application_click_count(const void* owner, float x, float y, bool double_click_message) noexcept {
    return ui_click_tracker_press(&g_app.click_tracker, owner, x, y, static_cast<u32>(GetMessageTime()),
                                  application_click_policy(), double_click_message);
}

} // namespace maicord::app_detail
