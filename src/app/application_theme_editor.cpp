#include "application_theme_editor.hpp"
#include "application_cursor.hpp"
#include "application_settings.hpp"
#include "application_custom_theme.hpp"
#include "application_account_presentation.hpp"
#include "application_state.hpp"
#include "application_text_form.hpp"
#include "application_surface_motion.hpp"
#include "application_tooltip.hpp"
#include "application_modal_scroll.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/widget_scroll.hpp"
#include "maicord/ui/theme_style_projection.hpp"

namespace maicord::app_detail {
struct ApplicationThemeEditor {
    UiThemeEditorScene scene;
    ClientPreferences baseline;
    ClientPreferences gesture_preferences;
    ClientThemeStorageSnapshot theme_baseline, gesture_theme;
    bool theme_captured, gesture_theme_captured;
    BackgroundTheme gesture_draft;
    BackgroundEffectSettings gesture_profiles[kBackgroundEffectProfileCount];
    float gesture_hue, gesture_saturation, gesture_value, scrollbar_grab;
    u64 gesture_lifetime;
    u8 gesture_selected_stop;
    bool gesture_light_mode, gesture_active, scrollbar_dragging;
    UiWidgetManager* previous_focus;
    u64 settings_lifetime, account_scope, profile, lifetime, foreground_published;
    UiThemeEditorElement pressed;
    u16 captured_generation;
    u32 edit_revision, held_key, range_key;
    bool dragging, dirty;
};
static_assert(sizeof(ApplicationThemeEditor) +
                  kUiThemeEditorElements * (sizeof(UiWidget) + sizeof(UiLayoutNode)) <
              32 * 1024);
namespace {
bool current(const ApplicationThemeEditor& e) noexcept {
    if (!g_app.settings)
        return false;
    return e.scene.active && g_app.account_ui && !g_app.login.visible &&
           application_settings_lifetime() == e.settings_lifetime && application_settings_appearance_selected() &&
           application_settings_active() &&
           plugin_ui_scope_current(g_app.plugin_account_scope_serial, e.account_scope) &&
           g_app.account.status.profile_id == e.profile;
}
bool publish_foreground(ApplicationThemeEditor& e) noexcept {
    const u64 signature = content_text_theme_signature(application_theme());
    if (signature == e.foreground_published)
        return true;
    if (!refresh_account_text(true))
        return false;
    e.foreground_published = signature;
    return true;
}
void begin_gesture(ApplicationThemeEditor& e) noexcept {
    if (e.gesture_active)
        return;
    e.gesture_preferences = g_app.preferences;
    e.gesture_theme_captured = application_custom_theme_snapshot(&e.gesture_theme);
    e.gesture_draft = e.scene.draft;
    copy_bytes(e.gesture_profiles, e.scene.effect_profiles, sizeof(e.gesture_profiles));
    e.gesture_hue = e.scene.hue;
    e.gesture_saturation = e.scene.saturation;
    e.gesture_value = e.scene.value;
    e.gesture_lifetime = e.lifetime;
    e.gesture_selected_stop = e.scene.selected_stop;
    e.gesture_light_mode = e.scene.light_mode;
    e.gesture_active = true;
}
void cancel_capture(ApplicationThemeEditor& e, bool rollback = true) noexcept {
    const bool restore = rollback && e.gesture_active && e.gesture_lifetime == e.lifetime &&
                         g_app.theme_editor == &e && current(e);
    e.gesture_active = false;
    e.range_key = 0;
    e.dragging = false;
    e.scrollbar_dragging = false;
    e.pressed = UiThemeEditorElement::count;
    e.captured_generation = 0;
    if (restore) {
        g_app.preferences = e.gesture_preferences;
        if (e.gesture_theme_captured)
            application_custom_theme_restore(e.gesture_theme, false);
        const auto style = ui_theme_editor_style(application_theme());
        ui_theme_editor_update(&e.scene, e.gesture_draft, e.gesture_selected_stop,
                               application_viewport_dips(), &style, e.gesture_profiles);
        e.scene.light_mode = e.gesture_light_mode;
        e.scene.hue = e.gesture_hue;
        e.scene.saturation = e.gesture_saturation;
        e.scene.value = e.gesture_value;
        e.dirty = !client_preferences_equal(g_app.preferences, e.baseline);
        ++e.edit_revision;
        ++e.scene.revision;
        application_preferences_apply();
        application_request_frame();
    }
    ui_button_pointer(&e.scene.widgets, UiButtonPointerEvent::cancel, {});
    if (g_app.window && GetCapture() == g_app.window)
        ReleaseCapture();
}
bool commit(ApplicationThemeEditor& e) noexcept {
    if (!current(e))
        return false;
    if (!e.dirty && !e.scene.save_failed)
        return true;
    if (!application_settings_commit_preferences()) {
        e.scene.save_failed = true;
        ++e.scene.revision;
        application_request_frame();
        return false;
    }
    e.baseline = g_app.preferences;
    e.theme_captured = application_custom_theme_snapshot(&e.theme_baseline);
    e.dirty = false;
    e.scene.save_failed = false;
    return true;
}
bool effect_control(UiThemeEditorElement element) noexcept {
    return element == UiThemeEditorElement::speed || element == UiThemeEditorElement::strength ||
           element == UiThemeEditorElement::density ||
           (element >= UiThemeEditorElement::effect_none && element <= UiThemeEditorElement::effect_both);
}
bool flat_theme(ClientTheme theme) noexcept {
    return theme == ClientTheme::dark || theme == ClientTheme::amoled || theme == ClientTheme::light;
}
void preview(ApplicationThemeEditor& e, const BackgroundTheme& draft, bool color_edit = true) noexcept {
    if (!current(e))
        return;
    if (color_edit)
        g_app.preferences.gradient = draft;
    else {
        // The displayed preset palette is only an editor view. Effects must
        // not replace the user's custom colors beneath that preset.
        g_app.preferences.gradient.effect = draft.effect;
        g_app.preferences.gradient.speed = draft.speed;
        g_app.preferences.gradient.strength = draft.strength;
        g_app.preferences.gradient.density = draft.density;
    }
    copy_bytes(g_app.preferences.effect_profiles, e.scene.effect_profiles, sizeof(e.scene.effect_profiles));
    if (color_edit)
        g_app.preferences.theme =
            e.scene.light_mode ? ClientTheme::custom_light_gradient : ClientTheme::custom_gradient;
    if (!application_custom_theme_preferences_changed(false, false))
        return;
    e.dirty = !client_preferences_equal(g_app.preferences, e.baseline);
    ++e.edit_revision;
    application_preferences_apply();
    application_request_frame();
}
bool hex_valid(TextFormOwnerKey key) noexcept {
    const auto* e = g_app.theme_editor;
    return e && current(*e) && key.scope == e->lifetime &&
           key.collection == (static_cast<u64>(e->scene.widgets.generation) << 32 | e->edit_revision) &&
           key.item == e->scene.selected_stop;
}
bool hex_save(TextFormOwnerKey key, Utf8View text) noexcept {
    if (!hex_valid(key))
        return false;
    BackgroundTheme parsed = g_app.theme_editor->scene.draft;
    // The native text form edits one selected color. A pasted color
    // list must not silently change only its first entry.
    if (!text.data || text.bytes > 14 || !background_theme_parse_colors(text, &parsed) ||
        parsed.color_count != 1)
        return false;
    auto& e = *g_app.theme_editor;
    auto draft = e.scene.draft;
    draft.colors[e.scene.selected_stop] = parsed.colors[0];
    const auto style = ui_theme_editor_style(application_theme());
    if (!ui_theme_editor_update(&e.scene, draft, e.scene.selected_stop, application_viewport_dips(), &style,
                                e.scene.effect_profiles))
        return false;
    preview(e, draft);
    return commit(e);
}
bool action(ApplicationThemeEditor& e, UiThemeEditorElement element) noexcept {
    if (!current(e))
        return false;
    e.gesture_active = false;
    e.range_key = 0;
    if (element == UiThemeEditorElement::close || element == UiThemeEditorElement::apply) {
        application_theme_editor_close(true);
        return true;
    }
    if (element == UiThemeEditorElement::hex)
        return application_text_form_open(
            {utf8_literal("Edit color"),
             utf8_literal("Enter a color in #RRGGBB notation."),
             utf8_literal("Color"),
             utf8_literal("Apply"),
             {e.scene.hex, 7},
             {e.lifetime, static_cast<u64>(e.scene.widgets.generation) << 32 | e.edit_revision,
              e.scene.selected_stop},
             hex_valid,
             hex_save,
             14});
    if (element == UiThemeEditorElement::dark || element == UiThemeEditorElement::light) {
        const bool light = element == UiThemeEditorElement::light;
        if (light != e.scene.light_mode) {
            const bool was_flat = flat_theme(g_app.preferences.theme);
            e.scene.light_mode = light;
            ++e.scene.revision;
            if (was_flat)
                g_app.preferences.theme = light ? ClientTheme::light : ClientTheme::dark;
            else
                g_app.preferences.theme =
                    light ? ClientTheme::custom_light_gradient : ClientTheme::custom_gradient;
            preview(e, e.scene.draft, !was_flat);
            return commit(e);
        }
        return true;
    }
    BackgroundTheme next{};
    if (ui_theme_editor_action(&e.scene, element, static_cast<u32>(clock_now()), &next)) {
        if (element >= UiThemeEditorElement::stop0 && element <= UiThemeEditorElement::stop4) {
            ++e.edit_revision;
            application_request_frame();
            return true;
        }
        preview(e, next, !effect_control(element));
        if (element == UiThemeEditorElement::surprise || element == UiThemeEditorElement::reset ||
            element == UiThemeEditorElement::add || element == UiThemeEditorElement::remove ||
            (element >= UiThemeEditorElement::effect_none && element <= UiThemeEditorElement::effect_both))
            return commit(e);
    }
    return true;
}
bool range(UiThemeEditorElement e) noexcept {
    return e == UiThemeEditorElement::sv || e == UiThemeEditorElement::hue ||
           e == UiThemeEditorElement::direction || e == UiThemeEditorElement::intensity ||
           e == UiThemeEditorElement::speed || e == UiThemeEditorElement::strength ||
           e == UiThemeEditorElement::density;
}
UiThemeEditorElement focused(const UiThemeEditorScene& scene) noexcept {
    for (u32 i = 0; i < kUiThemeEditorElements; ++i)
        if (scene.widgets.focused.slot == scene.elements[i].slot &&
            scene.widgets.focused.generation == scene.elements[i].generation)
            return static_cast<UiThemeEditorElement>(i);
    return UiThemeEditorElement::count;
}
} // namespace
bool application_theme_editor_active() noexcept {
    return g_app.theme_editor && current(*g_app.theme_editor);
}
const UiThemeEditorScene* application_theme_editor_scene() noexcept {
    return application_theme_editor_active() ? &g_app.theme_editor->scene : nullptr;
}
u32 application_theme_editor_owner_bytes() noexcept {
    return sizeof(ApplicationThemeEditor);
}
bool application_theme_editor_open() noexcept {
    if (g_app.theme_editor)
        return application_theme_editor_active();
    if (!g_app.renderer_ready || !g_app.account_ui || g_app.login.visible ||
        !application_settings_active() || g_app.text_form || g_app.theme_editor_lifetime_serial == ~u64{0})
        return false;
    application_settings_cancel_pointer();
    application_tooltip_clear();
    auto* e = static_cast<ApplicationThemeEditor*>(
        VirtualAlloc(nullptr, sizeof(ApplicationThemeEditor), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!e)
        return false;
    ui_scroll_host_cancel(&g_app.scroll_host);
    e->baseline = g_app.preferences;
    e->theme_captured = application_custom_theme_snapshot(&e->theme_baseline);
    e->foreground_published = content_text_theme_signature(application_theme());
    e->previous_focus = g_app.context_focus;
    e->settings_lifetime = application_settings_lifetime();
    e->account_scope = g_app.plugin_account_scope_serial;
    e->profile = g_app.account.status.profile_id;
    e->pressed = UiThemeEditorElement::count;
    e->edit_revision = 1;
    e->lifetime = g_app.theme_editor_lifetime_serial + 1;
    auto draft = application_theme().background;
    if (!background_theme_valid(draft))
        draft = g_app.preferences.gradient;
    const auto style = ui_theme_editor_style(application_theme());
    if (!ui_theme_editor_update(&e->scene, draft, 0, application_viewport_dips(), &style,
                                g_app.preferences.effect_profiles)) {
        ui_theme_editor_release(&e->scene);
        VirtualFree(e, 0, MEM_RELEASE);
        return false;
    }
    e->scene.light_mode = g_app.preferences.theme == ClientTheme::light ||
                          g_app.preferences.theme == ClientTheme::custom_light_gradient;
    g_app.theme_editor = e;
    if (!current(*e)) {
        application_theme_editor_close(false);
        return false;
    }
    renderer_bind_theme_editor_scene(&g_app.renderer, &e->scene);
    ++g_app.theme_editor_lifetime_serial;
    g_app.context_focus = &e->scene.widgets;
    ui_theme_editor_focus(&e->scene, UiThemeEditorElement::hex);
    application_request_frame();
    return true;
}
void application_theme_editor_close(bool save) noexcept {
    auto* e = g_app.theme_editor;
    if (!e)
        return;
    cancel_capture(*e, !save);
    if (g_app.theme_editor != e)
        return;
    if (save && current(*e) && !commit(*e))
        return;
    application_text_form_cancel_owner(hex_valid);
    if (!save && current(*e)) {
        g_app.preferences = e->baseline;
        if (e->theme_captured)
            application_custom_theme_restore(e->theme_baseline, false);
        application_preferences_apply();
    }
    // Closing releases the lazily created editor, so carry its final foreground
    // state into the chat view before the last committed signature is lost.
    if (current(*e) && !publish_foreground(*e)) {
        application_request_frame();
        return;
    }
    auto* focus = e->previous_focus;
    const auto* editor_focus = &e->scene.widgets;
    const bool restore_owner = current(*e);
    if (e->held_key)
        g_app.settings_held_key = e->held_key;
    application_tooltip_clear();
    application_surface_motion_retire(RendererLayerSurface::theme_editor);
    ui_scroll_host_cancel(&g_app.scroll_host);
    renderer_bind_theme_editor_scene(&g_app.renderer, nullptr);
    g_app.theme_editor = nullptr;
    ui_theme_editor_release(&e->scene);
    VirtualFree(e, 0, MEM_RELEASE);
    if (restore_owner && g_app.settings && application_settings_active())
        g_app.context_focus = focus;
    else if (g_app.context_focus == editor_focus)
        g_app.context_focus = nullptr;
    application_request_frame();
}
void application_theme_editor_release() noexcept {
    application_theme_editor_close(false);
}
UiScrollBinding application_theme_editor_scroll_binding() noexcept {
    return {nullptr, 0,
            [](void*, u64, UiScrollRange* range) noexcept {
                auto* e = g_app.theme_editor;
                if (!e || !current(*e) || g_app.text_form || e->dragging || e->scrollbar_dragging)
                    return false;
                const auto body = e->scene.elements[static_cast<u32>(UiThemeEditorElement::body)];
                const auto* node = ui_widget_manager_layout_node(e->scene.widgets, body);
                UiWidgetScrollGeometry geometry{};
                if (!node || !ui_widget_scroll_geometry(e->scene.widgets, body, &geometry))
                    return false;
                *range = {ui_rect_intersection(node->rectangle, node->clip_rectangle),
                          geometry.offset.y,
                          geometry.maximum.y,
                          32,
                          1,
                          0,
                          g_app.theme_editor_lifetime_serial};
                return true;
            },
            [](void*, u64, double offset, bool* changed) noexcept {
                const auto binding = application_theme_editor_scroll_binding();
                UiScrollRange before{}, after{};
                if (!binding.read(nullptr, 0, &before))
                    return false;
                const float target = static_cast<float>(minimum(maximum(offset, 0.0), before.maximum));
                if (target == before.offset) {
                    *changed = false;
                    return true;
                }
                if (!ui_theme_editor_scroll(&g_app.theme_editor->scene,
                                            target - static_cast<float>(before.offset)) ||
                    !binding.read(nullptr, 0, &after))
                    return false;
                *changed = before.offset != after.offset;
                return true;
            }};
}
bool application_theme_editor_synchronize() noexcept {
    auto* e = g_app.theme_editor;
    if (!e)
        return true;
    if (!current(*e)) {
        application_theme_editor_close(false);
        return true;
    }
    const auto generation = e->scene.widgets.generation;
    const auto style = ui_theme_editor_style(application_theme());
    if (!ui_theme_editor_update(&e->scene, e->scene.draft, e->scene.selected_stop,
                                application_viewport_dips(), &style, e->scene.effect_profiles)) {
        application_theme_editor_close(false);
        return false;
    }
    if (generation != e->scene.widgets.generation)
        cancel_capture(*e);
    const bool save_failed = g_app.preferences_save_failed || application_custom_theme_view().save_failed;
    if (e->scene.save_failed != save_failed) {
        e->scene.save_failed = save_failed;
        ++e->scene.revision;
    }
    if (!g_app.text_form && !g_app.context_focus)
        g_app.context_focus = &e->scene.widgets;
    return true;
}
bool application_theme_editor_publish_foreground() noexcept {
    auto* e = g_app.theme_editor;
    return !e || !current(*e) || publish_foreground(*e);
}
bool application_theme_editor_input(UiInputKind kind, u32 key, bool repeated) noexcept {
    auto* e = g_app.theme_editor;
    if (!e)
        return false;
    if (!current(*e)) {
        application_theme_editor_close(false);
        return true;
    }
    if (kind == UiInputKind::system_key_down && key == VK_F4 && (GetKeyState(VK_MENU) & 0x8000))
        return false;
    if (ui_input_consume_key_tail(&e->held_key, {kind, key, repeated}))
        return true;
    if (kind == UiInputKind::key_up && e->range_key == key) {
        e->range_key = 0;
        e->gesture_active = false;
        commit(*e);
        return true;
    }
    if (kind != UiInputKind::key_down)
        return kind == UiInputKind::character || kind == UiInputKind::system_key_down ||
               kind == UiInputKind::key_up;
    if (key == VK_ESCAPE) {
        e->held_key = key;
        application_theme_editor_close(false);
        return true;
    }
    if (key == VK_TAB) {
        ui_theme_editor_move_focus(&e->scene, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        application_request_frame();
        return true;
    }
    const auto element = focused(e->scene);
    if (key == VK_RETURN || key == VK_SPACE) {
        if (!repeated) {
            e->held_key = key;
            action(*e, element);
        }
        return true;
    }
    BackgroundTheme next{};
    if (range(element) && (key == VK_HOME || key == VK_END || key == VK_LEFT || key == VK_RIGHT ||
                           key == VK_UP || key == VK_DOWN))
        begin_gesture(*e);
    if (ui_theme_editor_key(&e->scene, element, key, &next)) {
        e->range_key = key;
        preview(*e, next, !effect_control(element));
    }
    return true;
}
bool application_theme_editor_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    auto* e = g_app.theme_editor;
    if (!e)
        return false;
    if (!current(*e)) {
        application_theme_editor_close(false);
        return true;
    }
    // The optional hex editor is a separate text form. Drawer controls use
    // ordinary press/release semantics for both clicks of a rapid pair.
    if (message == WM_LBUTTONDBLCLK)
        message = WM_LBUTTONDOWN;
    if (message == WM_SETCURSOR) {
        // The drawer is modal: its controls declare hands; the scrollbar stays an arrow.
        UiPoint point{};
        if (!application_cursor_point(g_app.window, g_app.renderer.dpi, lparam, &point))
            return false;
        application_set_cursor(e->dragging ? UiCursor::hand
                               : ui_theme_editor_hit_test(e->scene, point) == UiThemeEditorElement::scrollbar
                                   ? UiCursor::arrow
                                   : ui_widget_manager_cursor(e->scene.widgets, point.x, point.y));
        return true;
    }
    if (message == WM_CANCELMODE || message == WM_CAPTURECHANGED || message == WM_KILLFOCUS ||
        message == WM_SIZE || message == WM_DPICHANGED) {
        cancel_capture(*e);
        return false;
    }
    if (message == WM_MOUSELEAVE) {
        ui_button_pointer(&e->scene.widgets, UiButtonPointerEvent::move, {-1, -1});
        application_request_frame();
        return true;
    }
    if (message == WM_MOUSEWHEEL) {
        application_modal_scroll_wheel(&g_app.scroll_host, application_theme_editor_scroll_binding(),
                                       g_app.window, g_app.renderer.dpi, wparam, lparam,
                                       clock_microseconds(g_app.clock, g_app.process_start, clock_now()));
        application_request_frame();
        return true;
    }
    if (message != WM_MOUSEMOVE && message != WM_LBUTTONDOWN && message != WM_LBUTTONUP)
        return message == WM_RBUTTONDOWN || message == WM_RBUTTONUP || message == WM_CONTEXTMENU ||
               message == WM_MOUSEHWHEEL || message == WM_LBUTTONDBLCLK;
    const float scale = 96.f / maximum(g_app.renderer.dpi, 1u);
    const UiPoint p{static_cast<short>(LOWORD(lparam)) * scale, static_cast<short>(HIWORD(lparam)) * scale};
    const auto hit = ui_theme_editor_hit_test(e->scene, p);
    if (message == WM_MOUSEMOVE) {
        if (e->scrollbar_dragging && e->captured_generation == e->scene.widgets.generation &&
            GetCapture() == g_app.window) {
            const auto geometry = ui_theme_editor_scrollbar(e->scene);
            UiWidgetScrollGeometry scroll{};
            if (ui_widget_scroll_geometry(e->scene.widgets,
                                          e->scene.elements[static_cast<u32>(UiThemeEditorElement::body)],
                                          &scroll) &&
                ui_theme_editor_scroll(&e->scene,
                                       ui_scrollbar_scroll_from_pointer(geometry, p.y, e->scrollbar_grab) -
                                           scroll.offset.y))
                application_request_frame();
            return true;
        }
        if (e->dragging && e->captured_generation == e->scene.widgets.generation &&
            GetCapture() == g_app.window) {
            BackgroundTheme next{};
            if (ui_theme_editor_pointer(&e->scene, e->pressed, p, &next))
                preview(*e, next, !effect_control(e->pressed));
        } else if (ui_button_pointer(&e->scene.widgets, UiButtonPointerEvent::move, p).changed)
            application_request_frame();
        return true;
    }
    if (message == WM_LBUTTONDOWN) {
        SetFocus(g_app.window);
        if (g_app.theme_editor != e || !current(*e))
            return true;
        e->gesture_active = false;
        e->range_key = 0;
        const auto pointer = ui_button_pointer(&e->scene.widgets, UiButtonPointerEvent::press, p);
        if (!pointer.handled || hit == UiThemeEditorElement::count)
            return true;
        e->pressed = hit;
        e->captured_generation = e->scene.widgets.generation;
        e->dragging = range(hit);
        if (e->dragging)
            begin_gesture(*e);
        ui_theme_editor_focus(&e->scene, hit);
        SetCapture(g_app.window);
        if (g_app.theme_editor != e || !current(*e) || GetCapture() != g_app.window) {
            if (g_app.theme_editor == e)
                cancel_capture(*e);
            return true;
        }
        if (e->dragging || hit == UiThemeEditorElement::gradient) {
            BackgroundTheme next{};
            if (ui_theme_editor_pointer(&e->scene, hit, p, &next)) {
                if (e->dragging)
                    preview(*e, next, !effect_control(hit));
                else {
                    ++e->edit_revision;
                    application_request_frame();
                }
            }
        }
        if (hit == UiThemeEditorElement::scrollbar) {
            ui_scroll_host_cancel(&g_app.scroll_host);
            const auto geometry = ui_theme_editor_scrollbar(e->scene);
            e->scrollbar_dragging = ui_scrollbar_thumb_hit(geometry, p.x, p.y);
            e->scrollbar_grab = p.y - geometry.thumb.top;
            if (!e->scrollbar_dragging && ui_scrollbar_track_hit(geometry, p.x, p.y))
                ui_theme_editor_scroll(&e->scene, (p.y < geometry.thumb.top ? -.8f : .8f) *
                                                      ui_rect_height(geometry.track));
        }
        application_request_frame();
        return true;
    }
    const auto pressed = e->pressed;
    const bool dragging = e->dragging;
    const bool accepted =
        e->captured_generation == e->scene.widgets.generation && GetCapture() == g_app.window;
    const auto pointer = ui_button_pointer(&e->scene.widgets, UiButtonPointerEvent::release, p);
    cancel_capture(*e, false);
    if (g_app.theme_editor != e || !current(*e) || !accepted)
        return true;
    if (dragging)
        commit(*e);
    else if (pointer.activate && hit == pressed)
        action(*e, pressed);
    return true;
}
} // namespace maicord::app_detail
