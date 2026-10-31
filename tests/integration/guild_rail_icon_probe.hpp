#pragma once
namespace maicord {
struct D3D11Renderer;
struct GuildRailScene;
struct ChatStateStore;
struct NavigationView;
struct Theme;
struct GuildRailIconProbeContext {
    D3D11Renderer& renderer;
    GuildRailScene& rail;
    ChatStateStore& state;
    NavigationView& navigation;
    bool (*rebuild)() noexcept;
    bool (*capture)(const wchar_t*, const Theme&) noexcept = nullptr;
};
// Server icons through the real layout and present path: a static resident
// icon, animated icons at rest and hovered, and one still loading. Every frame
// must build its layout and present, and the rail queues exactly the resident
// rendition each row should show. In-memory pixels only; the optional capture
// owner records the finished rail (selected, unread, mention and hovered text
// tile) at 100% and 125% in the flat and gradient themes.
bool guild_rail_icon_ui_probe(GuildRailIconProbeContext) noexcept;
} // namespace maicord
