#include "maicord/app/input_layers.hpp"

namespace maicord {

AppInputLayers app_input_layers(AppInputState state, AppInputHandlers handlers) noexcept {
    AppInputLayers result{};
    const auto add = [&](AppInputLayerId id, UiInputHandler handler, UiInputMask barrier) noexcept {
        result.layers[result.count++] = {handler, handlers.context, barrier, static_cast<u32>(id)};
    };
    if (state.login) {
        add(AppInputLayerId::login, handlers.login,
            ui_input_mask(UiInputKind::key_down) | ui_input_mask(UiInputKind::key_up) | ui_input_mask(UiInputKind::character));
    } else {
        if (state.reaction_viewer) add(AppInputLayerId::reaction_viewer, handlers.reaction_viewer, kUiInputAll);
        if (state.popup != AppPopupInput::none) {
            const auto barrier = state.popup == AppPopupInput::autocomplete
                ? ui_input_mask(UiInputKind::system_character)
                : kUiInputAll;
            add(AppInputLayerId::popup, handlers.popup, barrier);
        }
        if (state.profile) add(AppInputLayerId::profile, handlers.profile, kUiInputAll);
    }
    add(AppInputLayerId::content, handlers.content, 0);
    return result;
}

} // namespace maicord
