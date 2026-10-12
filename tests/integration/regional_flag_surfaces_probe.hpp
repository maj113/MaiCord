#pragma once

#include "maicord/core/utf.hpp"

namespace maicord {
struct D3D11Renderer;
struct UiScene;
struct MessageViewRuntime;
struct MessageRichStore;
struct MessageTextPresentation;
struct MessageScene;
struct MessageComposer;
struct MessageComposerScene;
struct PopupScene;
struct ChatStateStore;
struct UserStore;
struct NavigationView;

// UIProbe supplies its initialized, focused fixture. This probe creates no
// application, worker, atlas or polling owner of its own.
struct RegionalFlagSurfacesProbeContext {
    D3D11Renderer& renderer;
    UiScene& shell_scene;
    MessageViewRuntime& message_view;
    MessageRichStore& rich_store;
    MessageTextPresentation& message_text;
    MessageScene& message_scene;
    MessageComposer& composer;
    MessageComposerScene& composer_scene;
    PopupScene& popup;
    const ChatStateStore& state;
    const UserStore& users;
    const NavigationView& navigation;
    bool (*add_message)(u64, Utf8View) noexcept;
    bool (*rebuild)() noexcept;
    bool (*capture)(const wchar_t*) noexcept;
};

bool regional_flag_surfaces_ui_probe(RegionalFlagSurfacesProbeContext context) noexcept;
} // namespace maicord
