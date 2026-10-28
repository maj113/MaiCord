#include "application_downloads.hpp"
#include "application_content_point.hpp"
#include "application_composer_text_input_state.hpp"
#include "application_profile.hpp"
#include "application_composer.hpp"
#include "application_state.hpp"
#include "application_friends.hpp"
#include "application_search.hpp"
#include "application_media_picker.hpp"
#include "application_quick_switcher.hpp"
#include "application_message_forward.hpp"
#include "application_channel_overview.hpp"
#include "application_thread_browser.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/message_forward_scene.hpp"
#include "maicord/ui/call_scene.hpp"
#include "maicord/render/forum_post_surface.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/ui/friends_scene.hpp"
#include "maicord/ui/media_picker.hpp"
#include "maicord/ui/forum_post_scene.hpp"

namespace maicord::app_detail {
namespace {
bool forward_edit() noexcept {
    return application_message_forward_edit_focused();
}
bool quick_edit() noexcept {
    return application_quick_switcher_edit_focused();
}
bool overview_edit() noexcept {
    return application_channel_overview_edit_focused();
}

void cancel_native_groups(TextInputTarget target) noexcept {
    application_message_forward_edit_cancel_native_group(target);
    application_quick_switcher_edit_cancel_native_group(target);
    application_friends_search_cancel_native_group(target);
    application_search_edit_cancel_native_group(target);
    application_media_picker_edit_cancel_native_group(target);
    application_channel_overview_edit_cancel_native_group(target);
}

bool profile_edit() noexcept {
    return g_app.profile && g_app.profile->scene.active && g_app.profile->scene.edit.active;
}
bool friends_edit() noexcept {
    return !(g_app.profile && g_app.profile->scene.active) && application_friends_active() &&
           application_friends_search_model();
}
bool gallery_edit() noexcept {
    return !profile_edit() && !friends_edit() && application_media_picker_edit_focused() &&
           application_media_picker_edit_model();
}
bool search_edit() noexcept {
    return !gallery_edit() && !profile_edit() && !friends_edit() && application_search_edit_focused() &&
           application_search_edit_model();
}
bool plain_edit() noexcept {
    return forward_edit() || quick_edit() || overview_edit() || profile_edit() || friends_edit() ||
           gallery_edit() || search_edit();
}
const TextEditLayoutOwner& edit_layout() noexcept {
    return forward_edit()
               ? (application_message_forward_caption_focused() ? g_app.renderer.popup_edit_layout
                                                                : g_app.renderer.media_picker_layout)
           : quick_edit()    ? g_app.renderer.media_picker_layout
           : overview_edit() ? g_app.renderer.media_picker_layout
           : profile_edit()  ? g_app.renderer.profile_edit_layout
           : gallery_edit()  ? g_app.renderer.media_picker_layout
           : search_edit()   ? g_app.renderer.search_layout
                             : g_app.renderer.composer_layout;
}
MessageComposer& edit_model() noexcept {
    return forward_edit()    ? *application_message_forward_edit_model()
           : quick_edit()    ? *application_quick_switcher_edit_model()
           : overview_edit() ? *application_channel_overview_edit_model()
           : profile_edit()  ? *application_profile_edit_model()
           : friends_edit()  ? *application_friends_search_model()
           : gallery_edit()  ? *application_media_picker_edit_model()
           : search_edit()   ? *application_search_edit_model()
                             : g_app.message_composer;
}
TextInputTarget edit_target() noexcept {
    return forward_edit()    ? application_message_forward_edit_target()
           : quick_edit()    ? application_quick_switcher_edit_target()
           : overview_edit() ? application_channel_overview_edit_target()
           : friends_edit()  ? application_friends_search_target()
           : gallery_edit()  ? application_media_picker_edit_target()
           : search_edit()   ? application_search_edit_target()
                             : message_composer_input_target(&edit_model());
}
UiRect edit_bounds() noexcept {
    if (forward_edit())
        return application_message_forward_edit_bounds();
    if (quick_edit())
        return application_quick_switcher_edit_bounds();
    if (overview_edit())
        return application_channel_overview_edit_bounds();
    if (profile_edit())
        return g_app.profile->scene.edit_content;
    if (friends_edit() && g_app.renderer.friends_scene)
        return ui_friends_scene_search_geometry(*g_app.renderer.friends_scene).text;
    if (gallery_edit())
        return application_media_picker_edit_bounds();
    if (search_edit())
        return application_search_edit_bounds();
    const auto* node =
        message_composer_scene_layout_node(g_app.message_composer_scene, MessageComposerControl::edit);
    return node ? node->content_rectangle : UiRect{};
}
bool eligible() noexcept {
    if (g_app.renderer.forum_form && g_app.renderer.forum_form->scene &&
        g_app.renderer.forum_form->scene->active)
        return false;
    if (application_downloads_active())
        return false;
    if (application_message_forward_active())
        return g_app.window && forward_edit() && GetFocus() == g_app.window;
    if (application_quick_switcher_active())
        return g_app.window && quick_edit() && GetFocus() == g_app.window;
    if (g_app.renderer.settings_scene && g_app.renderer.settings_scene->active)
        return overview_edit() && !g_app.text_form && !g_app.popup_scene.active &&
               GetFocus() == g_app.window;
    if (!g_app.window || !g_app.renderer_ready || !g_app.account_ui || g_app.login.visible ||
        g_app.text_form || g_app.image_viewer.scene.active || g_app.reaction_viewer.active)
        return false;
    if (g_app.profile && g_app.profile->scene.active)
        return profile_edit() && application_profile_edit_focused() &&
               !g_app.popup_scene.active && GetFocus() == g_app.window;
    if (application_friends_active())
        return friends_edit() && application_friends_search_focused() && !g_app.popup_scene.active &&
               GetFocus() == g_app.window;
    if (g_app.renderer.friends_scene && g_app.renderer.friends_scene->active)
        return false;
    // The call view covers the composer: keys must not edit or send through
    // it. Beside its text chat (split) the composer is visible and editable.
    if (g_app.renderer.call_scene && g_app.renderer.call_scene->active && !g_app.renderer.call_scene->chat_open)
        return false;
    if (application_media_picker_active())
        return gallery_edit() && GetFocus() == g_app.window;
    // The thread browser covers the composer; the header search bar is the
    // only text field while it is open.
    if (application_thread_browser_active())
        return search_edit() && !g_app.popup_scene.active && GetFocus() == g_app.window;
    if (search_edit())
        return !g_app.popup_scene.active && GetFocus() == g_app.window;
    return g_app.message_composer.focused && g_app.message_composer_scene.active &&
           g_app.message_composer.channel_id &&
           message_composer_scene_focused_control(g_app.message_composer_scene) ==
               MessageComposerControl::edit &&
           (!g_app.popup_scene.active || (g_app.popup_owner_kind == PopupOwnerKind::autocomplete ||
                                          g_app.popup_owner_kind == PopupOwnerKind::commands)) &&
           GetFocus() == g_app.window;
}
bool current_owner(const ApplicationComposerTextInput* input) noexcept {
    return input && input == g_app.composer_text_input && eligible() && input->target == edit_target() &&
           text_input_ready(input->target);
}
bool current_layout() noexcept {
    const auto& layout = edit_layout();
    if (!text_edit_layout_current_font(layout, g_app.renderer.glyph_text.system))
        return false;
    if (plain_edit()) {
        const auto text = message_composer_text(edit_model());
        if (layout.text_bytes != text.bytes || layout.text_signature != text_edit_signature(text))
            return false;
        if (forward_edit() || quick_edit() || overview_edit() || friends_edit() || gallery_edit() ||
            search_edit()) {
            const auto bounds = edit_bounds();
            u32 anchor = 0, caret = 0;
            if (!ui_rect_has_area(bounds) ||
                !utf8_utf16_position_for_byte_offset(text, edit_model().anchor, &anchor) ||
                !utf8_utf16_position_for_byte_offset(text, edit_model().caret, &caret) ||
                layout.anchor != anchor || layout.caret_offset != caret ||
                layout.viewport_left != bounds.left || layout.viewport_top != bounds.top ||
                layout.width != ui_rect_width(bounds) || layout.height != ui_rect_height(bounds))
                return false;
        }
        return true;
    }
    return g_app.message_composer_scene.model == &edit_model() &&
           text_composer_layout_current(layout, g_app.message_composer_scene,
                                          g_app.renderer.glyph_text.system);
}
void project(ApplicationComposerTextInput* input) noexcept {
    secure_text_store_composition(input->store, &input->composition);
    if (plain_edit())
        return;
    auto& scene = g_app.message_composer_scene;
    scene.composition = {input->composition.spans,
                         input->composition.active ? input->composition.span_count : 0};
    ++scene.composition_revision;
}
bool geometry(void* opaque, SecureTextStoreGeometry* output) noexcept {
    if (!output || !current_owner(static_cast<ApplicationComposerTextInput*>(opaque)) || !current_layout())
        return false;
    const auto bounds = edit_bounds();
    if (!ui_rect_has_area(bounds) || !g_app.renderer.dpi)
        return false;
    POINT origin{};
    if (!application_content_to_screen(&origin) ||
        !GetWindowRect(g_app.window, output->window_screen.native()))
        return false;
    const float scale = static_cast<float>(g_app.renderer.dpi) / 96.0f;
    output->text_screen = {origin.x + static_cast<LONG>(bounds.left * scale),
                           origin.y + static_cast<LONG>(bounds.top * scale),
                           origin.x + static_cast<LONG>(bounds.right * scale),
                           origin.y + static_cast<LONG>(bounds.bottom * scale)};
    return true;
}
void changed(void* context) noexcept {
    auto* input = static_cast<ApplicationComposerTextInput*>(context);
    if (input != g_app.composer_text_input)
        return;
    input->dirty = true;
    if (input->busy)
        return;
    input->dirty = false;
    if (!current_owner(input))
        return;
    const bool content = input->observed_text_revision != edit_model().text_revision;
    input->observed_text_revision = edit_model().text_revision;
    project(input);
    input->busy = true;
    const bool refreshed = forward_edit()    ? application_message_forward_edit_changed(content)
                           : quick_edit()    ? application_quick_switcher_edit_changed(content)
                           : overview_edit() ? application_channel_overview_edit_changed(content)
                           : profile_edit()  ? application_profile_refresh_message()
                           : friends_edit()  ? application_friends_search_changed(content)
                           : gallery_edit()  ? application_media_picker_edit_changed(content)
                           : search_edit()   ? application_search_edit_changed(content)
                                             : composer_apply_result(MessageComposerResult::changed, content);
    if (g_app.composer_text_input == input)
        input->busy = false;
    if (!refreshed)
        log_write(LogLevel::warning, "ui", "composer_native_text_refresh_failed");
    application_request_frame();
}
bool point_geometry(void* opaque, POINT point, bool round, u32* position, bool* inside) noexcept {
    if (!position || !inside || !current_owner(static_cast<ApplicationComposerTextInput*>(opaque)) ||
        !current_layout())
        return false;
    const auto& scene = g_app.message_composer_scene;
    const auto& layout = edit_layout();
    const auto model = message_composer_text(edit_model());
    const bool plain = plain_edit();
    if (!g_app.renderer.dpi || !application_screen_to_content(&point))
        return false;
    const float scale = 96.0f / g_app.renderer.dpi;
    u32 hit = 0, byte = 0;
    const auto display = plain ? model : message_composer_scene_display_text(scene);
    if (!text_edit_layout_point_geometry(layout, point.x * scale, point.y * scale, round, &hit, inside) ||
        !utf8_byte_offset_for_utf16_position(display, hit, &byte))
        return false;
    if (!plain)
        byte = message_composer_scene_model_offset_from_display(scene, byte);
    return utf8_utf16_position_for_byte_offset(model, byte, position);
}
bool range_geometry(void* opaque, u32 first, u32 last, RECT* output, bool* clipped) noexcept {
    if (!current_owner(static_cast<ApplicationComposerTextInput*>(opaque)) || !current_layout())
        return false;
    const auto& scene = g_app.message_composer_scene;
    const auto& layout = edit_layout();
    const auto bounds = edit_bounds();
    if (!output || !clipped || !layout.active || !ui_rect_has_area(bounds))
        return false;
    const bool plain = plain_edit();
    u32 begin = 0, end = 0;
    const auto model = message_composer_text(edit_model());
    if (!utf8_byte_offset_for_utf16_position(model, first, &begin) ||
        !utf8_byte_offset_for_utf16_position(model, last, &end))
        return false;
    *clipped = !plain && (begin < scene.display_first || end > scene.display_source_last);
    const auto display = plain ? model : message_composer_scene_display_text(scene);
    u32 a = 0, b = 0;
    if (!utf8_utf16_position_for_byte_offset(
            display, plain ? begin : message_composer_scene_display_offset_from_model(scene, begin), &a) ||
        !utf8_utf16_position_for_byte_offset(
            display, plain ? end : message_composer_scene_display_offset_from_model(scene, end), &b))
        return false;
    UiRect rectangle{};
    if (a == b) {
        TextEditMetric metric{};
        if (!text_edit_layout_position_geometry(layout, a, &metric))
            return false;
        const float left = minimum(maximum(metric.left, bounds.left), bounds.right - 1);
        *clipped = *clipped || left != metric.left;
        rectangle = {left, metric.top, left + 1, metric.top + metric.height};
    } else {
        TextEditMetric metrics[kTextEditSelectionCapacity]{};
        u32 count = 0;
        if (!text_edit_layout_range_geometry(layout, a, b, {metrics, kTextEditSelectionCapacity},
                                               &count) ||
            !count)
            return false;
        rectangle = {metrics[0].left, metrics[0].top, metrics[0].left + metrics[0].width,
                     metrics[0].top + metrics[0].height};
        for (u32 i = 1; i < count; ++i) {
            rectangle.left = minimum(rectangle.left, metrics[i].left);
            rectangle.top = minimum(rectangle.top, metrics[i].top);
            rectangle.right = maximum(rectangle.right, metrics[i].left + metrics[i].width);
            rectangle.bottom = maximum(rectangle.bottom, metrics[i].top + metrics[i].height);
        }
    }
    const auto visible = ui_rect_intersection(rectangle, bounds);
    if (!ui_rect_has_area(visible))
        return false;
    *clipped = *clipped || !ui_rect_equal(rectangle, visible);
    POINT origin{};
    if (!application_content_to_screen(&origin))
        return false;
    const float scale = static_cast<float>(g_app.renderer.dpi) / 96.0f;
    output->left = origin.x + static_cast<LONG>(visible.left * scale);
    output->top = origin.y + static_cast<LONG>(visible.top * scale);
    output->right = origin.x + static_cast<LONG>(visible.right * scale + 0.999f);
    output->bottom = origin.y + static_cast<LONG>(visible.bottom * scale + 0.999f);
    return true;
}
} // namespace
bool application_composer_text_input_range_bounds(Utf8View text, u32 begin, u32 end, UiRect* output) noexcept {
    auto* input = g_app.composer_text_input;
    if (!output || !input || !current_owner(input) || !g_app.renderer.dpi)
        return false;
    const auto model = message_composer_text(edit_model());
    u32 first = 0, last = 0;
    if (model.bytes != text.bytes || !bytes_equal(model.data, text.data, text.bytes) ||
        !utf8_utf16_position_for_byte_offset(model, minimum(begin, model.bytes), &first) ||
        !utf8_utf16_position_for_byte_offset(model, minimum(end, model.bytes), &last))
        return false;
    RECT screen{};
    bool clipped = false;
    if (!range_geometry(input, first, last, &screen, &clipped))
        return false;
    POINT origin{};
    if (!application_content_to_screen(&origin))
        return false;
    const float scale = 96.0f / static_cast<float>(g_app.renderer.dpi);
    *output = {(screen.left - origin.x) * scale, (screen.top - origin.y) * scale, (screen.right - origin.x) * scale,
               (screen.bottom - origin.y) * scale};
    return true;
}

void application_composer_text_input_suspend() noexcept {
    auto* input = g_app.composer_text_input;
    if (!input || !input->target)
        return;
    const bool busy = input->busy;
    input->busy = true;
    cancel_native_groups(input->target);
    secure_text_store_unbind(&input->store);
    secure_text_store_detach(&input->store);
    reset_value(&input->target);
    reset_value(&input->composition);
    input->dirty = false;
    input->busy = busy;
    input->geometry_valid = false;
    reset_value(&g_app.message_composer_scene.composition);
    ++g_app.message_composer_scene.composition_revision;
}
void application_composer_text_input_release() noexcept {
    auto* input = g_app.composer_text_input;
    if (!input)
        return;
    application_composer_text_input_suspend();
    g_app.composer_text_input = nullptr;
    secure_text_store_release(&input->store);
    SecureZeroMemory(input, sizeof(*input));
    VirtualFree(input, 0, MEM_RELEASE);
}
bool application_composer_text_input_synchronize() noexcept {
    if (!eligible()) {
        application_composer_text_input_suspend();
        return true;
    }
    auto* input = g_app.composer_text_input;
    if (!input) {
        input = static_cast<ApplicationComposerTextInput*>(VirtualAlloc(
            nullptr, sizeof(ApplicationComposerTextInput), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!input)
            return false;
        g_app.composer_text_input = input;
        input->failed = !secure_text_store_initialize(
            &input->store, {g_app.window, geometry, changed, input, range_geometry, point_geometry});
        if (input->failed)
            log_write(LogLevel::warning, "ui", "composer_native_text_unavailable");
    }
    if (input->busy || input->failed)
        return true;
    input->busy = true;
    const auto target = edit_target();
    if (target != input->target) {
        cancel_native_groups(input->target);
        input->target = target;
        input->geometry_valid = false;
        input->observed_text_revision = edit_model().text_revision;
        if (!secure_text_store_bind_target(&input->store, target, SecureTextInputScope::text) ||
            !secure_text_store_attach(&input->store)) {
            input->failed = true;
            application_composer_text_input_suspend();
            log_write(LogLevel::warning, "ui", "composer_native_text_unavailable");
        } else {
            secure_text_store_set_window_focus(&input->store, true);
            project(input);
        }
    }
    // Application edits have already committed their draft/typing state.
    if (!input->dirty)
        input->observed_text_revision = edit_model().text_revision;
    secure_text_store_synchronize(&input->store);
    input->busy = false;
    if (input->dirty)
        changed(input);
    return true;
}
void application_composer_text_input_notify_layout() noexcept {
    auto* input = g_app.composer_text_input;
    if (!input || !input->target || input->busy || input->failed)
        return;
    SecureTextStoreGeometry current{};
    if (!geometry(input, &current))
        return;
    const auto& scene = g_app.message_composer_scene;
    const auto& layout = edit_layout();
    const u32 revision = forward_edit()    ? g_app.renderer.message_forward_scene->revision
                         : quick_edit()    ? g_app.renderer.quick_switcher_scene->revision
                         : overview_edit() ? g_app.renderer.settings_scene->revision
                         : profile_edit()  ? g_app.profile->scene.revision
                         : friends_edit()  ? g_app.renderer.friends_scene->revision
                         : gallery_edit()  ? g_app.renderer.media_picker_scene->revision
                         : search_edit()   ? g_app.renderer.search_scene->revision
                                           : scene.observed_revision;
    if (input->geometry_valid && input->observed_layout_revision == revision &&
        input->observed_layout_builds == layout.builds &&
        bytes_equal(&current, &input->observed_geometry, sizeof(current)))
        return;
    input->observed_geometry = current;
    input->observed_layout_revision = revision;
    input->observed_layout_builds = layout.builds;
    input->geometry_valid = true;
    secure_text_store_notify_layout(&input->store);
}
bool application_composer_text_input_composing() noexcept {
    auto* input = g_app.composer_text_input;
    if (!current_owner(input))
        return false;
    SecureTextComposition composition{};
    return secure_text_store_composition(input->store, &composition) && composition.active;
}
bool application_composer_text_input_filter(const MSG& message) noexcept {
    if (application_downloads_filter(message))
        return true;
    if (application_message_forward_filter(message))
        return true;
    if (application_quick_switcher_filter(message))
        return true;
    if (message.hwnd != g_app.window || (message.message != WM_KEYDOWN && message.message != WM_KEYUP))
        return false;
    application_composer_text_input_synchronize();
    auto* input = g_app.composer_text_input;
    if (!input)
        return false;
    const bool down = message.message == WM_KEYDOWN;
    if (down && !(message.lParam & (1ll << 30)))
        input->held_key = 0;
    const bool held = input->held_key && input->held_key == message.wParam;
    if (!down && held)
        input->held_key = 0;
    if (!input->target || input->failed)
        return held;
    const bool composing = application_composer_text_input_composing();
    const bool eaten = secure_text_store_filter_message(&input->store, message);
    // Enter may finish composition during TestKeyDown without being marked
    // eaten. Do not translate that same key into the application's send action.
    const bool command =
        message.wParam == VK_RETURN || message.wParam == VK_ESCAPE || message.wParam == VK_TAB ||
        ((message.wParam == 'Z' || message.wParam == 'Y' || message.wParam == VK_BACK ||
          message.wParam == VK_DELETE || message.wParam == VK_LEFT || message.wParam == VK_RIGHT) &&
         (GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000));
    if (down && command && (composing || eaten) && input == g_app.composer_text_input)
        input->held_key = static_cast<u32>(message.wParam);
    return eaten || held || (composing && command);
}
} // namespace maicord::app_detail
