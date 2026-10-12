#include "probe_present.hpp"
#include "maicord/ui/login_text_presentation.hpp"
#include "maicord/core/log.hpp"

namespace maicord {
// Synthetic native scene only. No listener, credential, browser launch or
// profile mutation occurs. Root owns rendering/capture invocation.
bool login_browser_ui_probe(D3D11Renderer& renderer, bool (*capture)(const wchar_t*) noexcept,
                            bool (*text_pixels)(ConstSlice<UiRect>) noexcept) noexcept {
    const auto failed = [](const char* stage, u32 iteration = 0) noexcept {
        log_write(LogLevel::error, "probe", stage);
        log_u64(LogLevel::error, "probe", "login_browser_failed_iteration", iteration);
        return false;
    };
    if (!capture || !text_pixels)
        return failed("login_browser_invalid_callbacks");
    if (renderer.login_visible)
        return failed("login_browser_existing_login_view");
    if (renderer.content_text)
        return failed("login_browser_existing_content_view");
    if (!renderer.dpi)
        return failed("login_browser_missing_dpi");
    LoginScene scene{};
    const auto width = renderer.width, height = renderer.height;
    struct Restore {
        D3D11Renderer& renderer;
        LoginScene& scene;
        u32 width, height;
        ~Restore() noexcept {
            if (renderer.login_scene == &scene)
                renderer_set_login_view(&renderer, nullptr, {}, kDefaultTheme);
            login_scene_release(&scene);
            renderer_resize(&renderer, width, height);
        }
    } restore{renderer, scene, width, height};
    constexpr LoginStatus statuses[]{LoginStatus::idle, LoginStatus::browser_waiting,
                                     LoginStatus::browser_exchange_pending, LoginStatus::browser_expired,
                                     LoginStatus::browser_failed};
    constexpr const wchar_t* names[]{L"login-browser-action.bmp", L"login-browser-waiting.bmp",
                                     L"login-browser-exchange.bmp", L"login-browser-expired.bmp",
                                     L"login-browser-failed.bmp"};
    const float scale = renderer.dpi / 96.f;
    for (u32 i = 0; i < 5; ++i) {
        if (!renderer_resize(&renderer, static_cast<u32>(360 * scale), static_cast<u32>(640 * scale)))
            return failed("login_browser_resize_failed", i);
        LoginView view{};
        view.visible = true;
        view.status = statuses[i];
        view.focus = i ? LoginControl::back : LoginControl::browser_sign_in;
        scene.focus = view.focus;
        scene.focus_visible = true;
        scene.browser_available = login_browser_available(view.status, {}, false);
        scene.browser_mode = login_browser_mode(view.status);
        scene.back_only = login_requires_back(view.status, {});
        scene.credential_fields_hidden = login_credential_fields_hidden(view.status, {}, false);
        scene.action_disabled = login_view_action_disabled(view);
        scene.submit_disabled = login_view_submit_disabled(view);
        scene.recovery_disabled = login_view_recovery_disabled(view);
        if (!login_scene_layout(&scene, 360, 640))
            return failed("login_browser_layout_failed", i);
        if (!renderer_set_login_view(&renderer, &scene, view, kDefaultTheme))
            return failed("login_browser_view_failed", i);
        if (!probe_present(&renderer, kDefaultTheme))
            return failed("login_browser_present_failed", i);
        // The shared native login pixel callback has a four-rectangle contract:
        // heading and submit use neutral ink; Back and guidance use accent ink.
        const LoginElement elements[]{LoginElement::heading, LoginElement::forgot_password,
                                      i ? LoginElement::status_message : LoginElement::browser_sign_in,
                                      LoginElement::submit};
        UiRect rectangles[4]{};
        for (u32 n = 0; n < 4; ++n) {
            const auto* node = login_scene_element(scene, elements[n]);
            if (!node)
                return failed("login_browser_element_missing", i);
            rectangles[n] = ui_rect_intersection(node->content_rectangle, node->clip_rectangle);
            if (!ui_rect_has_area(rectangles[n]))
                return failed("login_browser_element_clipped", i);
        }
        if (!text_pixels({rectangles, 4}))
            return failed("login_browser_native_text_pixels_failed", i);
        if (!capture(names[i]))
            return failed("login_browser_capture_failed", i);
        const auto layouts = scene.layout_passes, cache = renderer.glyph_text.cache_builds,
                   edit = renderer.login_edit.builds;
        if (!probe_present(&renderer, kDefaultTheme))
            return failed("login_browser_repeat_present_failed", i);
        if (scene.layout_passes != layouts || renderer.glyph_text.cache_builds != cache ||
            renderer.login_edit.builds != edit)
            return failed("login_browser_repeat_cache_changed", i);
    }
    return renderer_set_login_view(&renderer, nullptr, {}, kDefaultTheme) ||
           failed("login_browser_view_release_failed");
}
} // namespace maicord
