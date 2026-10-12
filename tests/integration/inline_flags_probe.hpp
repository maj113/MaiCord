#pragma once
namespace maicord {
struct D3D11Renderer;
struct UiScene;
struct ChatStateStore;
struct NavigationView;
struct InlineFlagsProbeContext {
    D3D11Renderer& renderer;
    UiScene& shell;
    ChatStateStore& state;
    NavigationView& navigation;
    bool (*rebuild)() noexcept;
};
// Original in-memory RGBA tests typed Slovenia identity and native pixels. It
// uses no reference files, network/account owner or screenshot output.
bool inline_flags_ui_probe(InlineFlagsProbeContext) noexcept;
} // namespace maicord
