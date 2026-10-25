#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/ui/scene.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"
#include "maicord/ui/guild_rail_scene.hpp"

namespace maicord {
namespace {

const UiNode* navigation_scene_node(const D3D11Renderer& renderer, UiSemantic semantic) noexcept {
    return renderer.ui_scene
               ? ui_scene_resolve(*renderer.ui_scene, ui_scene_find(*renderer.ui_scene, semantic))
               : nullptr;
}

bool point_in_node(const D3D11Renderer& renderer, const UiNode* node, i32 client_x_pixels,
                   i32 client_y_pixels, float* x, float* y) noexcept {
    if (!node || !x || !y || renderer.dpi == 0 || client_x_pixels < 0 || client_y_pixels < 0) {
        return false;
    }
    *x = static_cast<float>(client_x_pixels) * 96.0f / static_cast<float>(renderer.dpi);
    *y = static_cast<float>(client_y_pixels) * 96.0f / static_cast<float>(renderer.dpi);
    return ui_rect_contains(node->rectangle, *x, *y);
}

} // namespace

bool renderer_guild_rail_hit_test(const D3D11Renderer& renderer, i32 client_x_pixels,
                                  i32 client_y_pixels) noexcept {
    float x = 0.0f;
    float y = 0.0f;
    return point_in_node(renderer, navigation_scene_node(renderer, UiSemantic::guild_navigation), client_x_pixels,
                         client_y_pixels, &x, &y);
}

RendererNavigationHit renderer_navigation_hit_test(const D3D11Renderer& renderer, i32 client_x_pixels,
                                                   i32 client_y_pixels) noexcept {
    if (!renderer.navigation_view || renderer.dpi == 0 || client_x_pixels < 0 || client_y_pixels < 0) {
        return RendererNavigationHit{};
    }
    const float x = static_cast<float>(client_x_pixels) * 96.0f / static_cast<float>(renderer.dpi);
    const float y = static_cast<float>(client_y_pixels) * 96.0f / static_cast<float>(renderer.dpi);
    const UiNode* home = navigation_scene_node(renderer, UiSemantic::home_button);
    if (home && ui_rect_contains(home->rectangle, x, y) &&
        ui_widget_manager_input_enabled(renderer.ui_scene->widgets, home->widget)) {
        return {0, RendererNavigationHitKind::home, true};
    }
    const UiNode* guild = navigation_scene_node(renderer, UiSemantic::guild_navigation);
    if (guild && renderer.guild_rail_scene && ui_rect_contains(guild->rectangle, x, y)) {
        const GuildRailHit hit = guild_rail_scene_hit_test(*renderer.guild_rail_scene, x, y);
        if (hit.conversation)
            return {hit.id, RendererNavigationHitKind::channel, true};
        return hit.id != 0 ? RendererNavigationHit{hit.id, RendererNavigationHitKind::guild, hit.selectable}
                           : RendererNavigationHit{};
    }
    const UiNode* navigation = navigation_scene_node(renderer, UiSemantic::channel_navigation);
    if (!navigation || !renderer.shell_navigation_scene || !ui_rect_contains(navigation->rectangle, x, y)) {
        return RendererNavigationHit{};
    }
    const ShellNavigationHit hit = shell_navigation_scene_hit_test(*renderer.shell_navigation_scene, x, y);
    return hit.id != 0 ? RendererNavigationHit{hit.id, RendererNavigationHitKind::channel, hit.selectable}
                       : RendererNavigationHit{};
}

void renderer_bind_navigation_view(D3D11Renderer* renderer, const ChatStateStore* state,
                                const UserStore* users,
                                const NavigationView* view) noexcept {
    if (!renderer) {
        return;
    }
    if (renderer->chat_state != state) arena_release(&renderer->shell_text_storage);
    renderer->chat_state = state;
    renderer->user_store = users;
    renderer->navigation_view = view;
}

void renderer_unbind_navigation_view(D3D11Renderer* renderer) noexcept {
    if (!renderer) {
        return;
    }
    renderer->chat_state = nullptr;
    arena_release(&renderer->shell_text_storage);
    renderer->user_store = nullptr;
    renderer->navigation_view = nullptr;
}

} // namespace maicord
