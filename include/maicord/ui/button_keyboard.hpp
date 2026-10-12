#pragma once
#include "maicord/ui/widget_manager.hpp"

namespace maicord {
enum class UiButtonKey : u8 { other, enter, space, escape };
struct UiButtonKeyboard {
    // A null manager is inactive. Keep all-zero initialization so embedding
    // this state does not move a large application owner out of zero-fill data.
    UiWidgetReference pressed;
};
static_assert(UiButtonKeyboard{}.pressed.manager == nullptr && UiButtonKeyboard{}.pressed.handle.slot == 0 &&
              UiButtonKeyboard{}.pressed.handle.generation == 0);
struct UiButtonKeyResult {
    UiWidgetHandle target = ui_widget_invalid_handle();
    bool handled = false;
    bool activate = false;
};

// Enter activates once on initial down; Space arms an exact focus generation
// and activates on matching release. Focus/visibility/eligibility changes cancel
// activation. The caller owns key translation, actions and OS focus lifetime.
UiButtonKeyResult ui_button_keyboard_key(UiButtonKeyboard* state, const UiWidgetManager* owner,
    UiButtonKey key, bool down, bool repeated = false) noexcept;
void ui_button_keyboard_cancel(UiButtonKeyboard* state) noexcept;
} // namespace maicord
