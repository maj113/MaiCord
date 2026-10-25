#include "maicord/ui/button_keyboard.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
void ui_button_keyboard_cancel(UiButtonKeyboard* state) noexcept {
    if (state) reset_value(&state->pressed);
}

UiButtonKeyResult ui_button_keyboard_key(UiButtonKeyboard* state, const UiWidgetManager* owner,
    UiButtonKey key, bool down, bool repeated) noexcept {
    if (!state) return {};
    const auto pressed = state->pressed;
    if (!down && key != UiButtonKey::space) return {};
    if (!down || key != UiButtonKey::space) ui_button_keyboard_cancel(state);
    UiRect visible{};
    const auto* widget = owner ? ui_widget_manager_resolve(*owner, owner->focused) : nullptr;
    const bool button = widget && widget->kind == UiWidgetKind::button &&
        ui_widget_manager_context_anchor(*owner, &visible);
    if (!button) {
        ui_button_keyboard_cancel(state);
        return {ui_widget_invalid_handle(), (key == UiButtonKey::space || key == UiButtonKey::escape) &&
            pressed.manager != nullptr, false};
    }
    const auto target = owner->focused;
    if (key == UiButtonKey::space) {
        if (down) {
            if (!repeated) state->pressed = {owner, target};
            return {target, true, false};
        }
        return {target, true, pressed.manager == owner && pressed.handle.slot == target.slot &&
            pressed.handle.generation == target.generation};
    }
    if (down && key == UiButtonKey::enter) return {target, true, !repeated};
    return {ui_widget_invalid_handle(), key == UiButtonKey::escape && pressed.manager != nullptr, false};
}
} // namespace maicord
