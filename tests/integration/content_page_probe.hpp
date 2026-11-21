#pragma once
namespace maicord {
struct D3D11Renderer;
struct ContentPageProbeContext {
    D3D11Renderer& renderer;
    bool (*rebuild)() noexcept;
};
// A content page with a Server Hub banner picture through the real layout and draw path: the banner picture draws
// before the page's top paint layer (the banner's scrim), and the foreground picture (the server icon) after it.
// The drawn target shows a white banner picture dark at its foot and light at its top, and the icon unshaded.
bool content_page_ui_probe(ContentPageProbeContext) noexcept;
} // namespace maicord
