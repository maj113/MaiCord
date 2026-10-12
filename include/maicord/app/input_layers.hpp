#pragma once

#include "maicord/ui/input_router.hpp"

namespace maicord {

enum class AppPopupInput : u8 { none, modal, autocomplete };
enum class AppInputLayerId : u32 { login = 1, reaction_viewer, popup, content, profile };

struct AppInputState {
    AppPopupInput popup;
    bool reaction_viewer;
    bool login;
    bool profile;
};

struct AppInputHandlers {
    void* context;
    UiInputHandler login;
    UiInputHandler reaction_viewer;
    UiInputHandler popup;
    UiInputHandler content;
    UiInputHandler profile;
};

struct AppInputLayers {
    UiInputLayer layers[5];
    u32 count;
};

// Builds a per-event snapshot. Authentication handles ordinary keys and text;
// autocomplete lets unhandled typing continue to its editor.
AppInputLayers app_input_layers(AppInputState state, AppInputHandlers handlers) noexcept;

} // namespace maicord
