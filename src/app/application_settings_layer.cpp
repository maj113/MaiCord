#include "application_settings_layer.hpp"
#include "application_cursor.hpp"
#include "maicord/ui/scene_cursor.hpp"
#include "application_state.hpp"
#include "application_account_presentation.hpp"
#include "application_composer_text_input.hpp"
#include "application_message_interaction.hpp"
#include "application_modal_scroll.hpp"
#include "application_shell_input.hpp"
#include "maicord/render/text_edit_layout.hpp"
#include "maicord/render/layer_presentation.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/ui/transition.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/ui/clipboard.hpp"
#include "maicord/ui/scrollbar.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include <shellapi.h>

namespace maicord::app_detail {
namespace {
constexpr u32 kBlinkMs = 500, kShakeMs = 300, kAlertFadeMs = 240;
constexpr float kShakeDistance = 5;
constexpr u32 kEditBufferUnits = 4096;

bool editable(SettingsLayer* layer) noexcept {
    return layer->hooks->editable ? layer->hooks->editable(layer) : true;
}
bool repaint(SettingsLayer* layer) noexcept {
    return layer->hooks->repaint(layer);
}
// The page body and sidebar share the scroll host's wheel curve, keyboard
// paging and jump-vs-glide policy (smooth scrolling, reduced motion) with
// every other list. Owners keep the layer alive until shutdown, and a closed
// scene ends any running glide through read().
constexpr u64 kLayerBodyScroll = 0x534c4230u, kLayerSidebarScroll = 0x534c5331u;
UiScrollBinding scroll_binding(SettingsLayer* layer, bool sidebar) noexcept {
    return {layer, sidebar ? kLayerSidebarScroll : kLayerBodyScroll,
            [](void* context, u64 key, UiScrollRange* range) noexcept {
                const auto* layer = static_cast<SettingsLayer*>(context);
                const auto* scene = layer->scene;
                if (!scene || !scene->active || layer->choice || layer->scrollbar_dragging ||
                    layer->slider_dragging || g_app.text_form)
                    return false;
                const bool left = key == kLayerSidebarScroll;
                const auto* node = ui_widget_manager_layout_node(scene->widgets, left ? scene->sidebar : scene->body);
                if (!node)
                    return false;
                const auto geometry = ui_settings_scene_scrollbar(*scene, left);
                *range = {ui_rect_intersection(node->rectangle, node->clip_rectangle),
                          left ? scene->sidebar_offset : scene->content_offset,
                          geometry.maximum_scroll,
                          32,
                          1,
                          0,
                          scene->page_generation};
                return true;
            },
            [](void* context, u64 key, double offset, bool* changed) noexcept {
                auto* layer = static_cast<SettingsLayer*>(context);
                auto* scene = layer->scene;
                if (!scene || !scene->active)
                    return false;
                const bool left = key == kLayerSidebarScroll;
                const float before = left ? scene->sidebar_offset : scene->content_offset;
                if (!ui_settings_scene_scroll(scene, left, static_cast<float>(offset) - before))
                    return false;
                *changed = before != (left ? scene->sidebar_offset : scene->content_offset);
                if (*changed)
                    repaint(layer);
                return true;
            }};
}
u64 scroll_now_us() noexcept {
    return clock_microseconds(g_app.clock, g_app.process_start, clock_now());
}
void glide_to(SettingsLayer* layer, bool sidebar, float target) noexcept {
    ui_scroll_host_scroll_to(&g_app.scroll_host, scroll_binding(layer, sidebar), target, scroll_now_us(),
                             96.0 / maximum(g_app.renderer.dpi, 1u));
}
// Focus moves reveal their target inside the scene at once; replay the
// reveal from the previous offset on the shared curve.
void glide_reveal(SettingsLayer* layer, float before) noexcept {
    auto* scene = layer->scene;
    const float after = scene->content_offset;
    if (after == before)
        return;
    ui_settings_scene_scroll(scene, false, before - after);
    glide_to(layer, false, after);
}
bool focused_editor(SettingsLayer* layer, MessageComposer** model, BoundedEdit** binding, bool* multiline,
                    u64* control = nullptr) noexcept {
    const auto target = ui_settings_scene_focused(*layer->scene);
    if (target.kind != UiSettingsTargetKind::control || !editable(layer) || layer->choice || g_app.text_form)
        return false;
    const auto* c = ui_settings_scene_control(*layer->scene, target);
    if (!c || !c->enabled || (c->kind != UiSettingsControlKind::edit && c->kind != UiSettingsControlKind::multiline))
        return false;
    bool lines = false;
    if (!layer->hooks->editor(layer, target.control_id, model, binding, &lines))
        return false;
    if (multiline)
        *multiline = lines;
    if (control)
        *control = target.control_id;
    return true;
}
float keyframe_shift(float t) noexcept {
    constexpr float at[]{0, .1f, .3f, .5f, .7f, .9f, 1};
    constexpr float value[]{0, -1, 1, -1, 1, -.6f, 0};
    for (u32 i = 1; i < 7; ++i)
        if (t <= at[i]) {
            const float f = (t - at[i - 1]) / (at[i] - at[i - 1]);
            return (value[i - 1] + (value[i] - value[i - 1]) * f) * kShakeDistance;
        }
    return 0;
}
bool select_point(SettingsLayer* layer, float x, float y, bool extend) noexcept {
    MessageComposer* model = nullptr;
    BoundedEdit* binding = nullptr;
    u32 acp = 0, byte = 0;
    if (!focused_editor(layer, &model, &binding, nullptr) ||
        !text_edit_layout_hit_test(g_app.renderer.media_picker_layout, x, y, &acp) ||
        !utf8_byte_offset_for_utf16_position(message_composer_text(*model), acp, &byte))
        return false;
    message_composer_set_selection(model, extend ? model->anchor : byte, byte);
    return settings_layer_edit_changed(layer, false);
}
bool replace_selection(SettingsLayer* layer, Utf8View text, bool multiline_allowed) noexcept {
    MessageComposer* model = nullptr;
    BoundedEdit* binding = nullptr;
    bool multiline = false;
    if (!focused_editor(layer, &model, &binding, &multiline))
        return false;
    u16 input[kEditBufferUnits]{};
    u32 units = 0;
    for (u32 offset = 0; offset < text.bytes;) {
        const auto scalar = utf8_decode_one(text.data + offset, text.bytes - offset);
        if (!scalar.valid || units >= kEditBufferUnits)
            return false;
        offset += scalar.width;
        u32 codepoint = scalar.codepoint;
        // Clipboard text uses Windows line endings; the shared model stores LF.
        if (codepoint == '\r') {
            if (offset < text.bytes && text.data[offset] == '\n')
                ++offset;
            codepoint = '\n';
        }
        if (codepoint == '\n' && (!multiline || !multiline_allowed))
            codepoint = ' ';
        if (codepoint <= 0xffff)
            input[units++] = static_cast<u16>(codepoint);
        else {
            if (units + 2 > kEditBufferUnits)
                return false;
            codepoint -= 0x10000;
            input[units++] = static_cast<u16>(0xd800 + (codepoint >> 10));
            input[units++] = static_cast<u16>(0xdc00 + (codepoint & 1023));
        }
    }
    u32 first = 0, last = 0;
    const auto current = message_composer_text(*model);
    if (!utf8_utf16_position_for_byte_offset(current, minimum(model->anchor, model->caret), &first) ||
        !utf8_utf16_position_for_byte_offset(current, maximum(model->anchor, model->caret), &last))
        return false;
    TextInputChange change{};
    const auto target = bounded_edit_input_target(binding);
    const bool group = text_input_begin_group(target);
    const auto result = text_input_replace(target, first, last, {input, units}, &change);
    if (group)
        text_input_end_group(target);
    return result == TextInputResult::changed;
}
void dispatch(SettingsLayer* layer, const UiSettingsActivation& value, bool pointer, bool button) noexcept {
    SettingsLayerActivation activation{value, pointer, button, {}};
    if (value.target.kind == UiSettingsTargetKind::control) {
        UiSettingsControlGeometry geometry{};
        if (ui_settings_scene_control_geometry(*layer->scene, value.target, &geometry))
            activation.anchor = geometry.clip;
    }
    layer->hooks->activate(layer, activation);
}
bool choose_pointer(SettingsLayer* layer, UINT message, float x, float y) noexcept {
    const auto hit = popup_scene_hit_test(g_app.popup_scene, x, y);
    if (message == WM_MOUSEMOVE) {
        bool changed = false;
        popup_scene_set_hover(&g_app.popup_scene, x, y, &changed);
        if (changed)
            repaint(layer);
    } else if (message == WM_LBUTTONDOWN) {
        if (!hit.inside) {
            settings_layer_close_choice(layer);
            layer->hooks->choose(layer, 0);
            repaint(layer);
            return true;
        }
        layer->popup_pressed = hit.enabled ? hit.action : 0;
        SetCapture(g_app.window);
    } else if (message == WM_LBUTTONUP) {
        const u32 pressed = layer->popup_pressed;
        if (GetCapture() == g_app.window)
            ReleaseCapture();
        if (!hit.inside && !pressed)
            return true;
        settings_layer_close_choice(layer);
        layer->hooks->choose(layer, hit.enabled && hit.action == pressed ? pressed : 0);
        repaint(layer);
    }
    return true;
}
} // namespace

void settings_layer_reset(SettingsLayer* layer) noexcept {
    auto* scene = layer->scene;
    const auto* hooks = layer->hooks;
    void* owner = layer->owner;
    *layer = {};
    layer->scene = scene;
    layer->hooks = hooks;
    layer->owner = owner;
}

bool settings_layer_editor(SettingsLayer* layer, u64* control_id, MessageComposer** model, BoundedEdit** binding,
                           bool* multiline) noexcept {
    MessageComposer* m = nullptr;
    BoundedEdit* b = nullptr;
    u64 id = 0;
    if (!layer || !layer->scene || !layer->scene->active || !focused_editor(layer, &m, &b, multiline, &id))
        return false;
    if (control_id)
        *control_id = id;
    if (model)
        *model = m;
    if (binding)
        *binding = b;
    return true;
}

UiSettingsEditor settings_layer_editor_view(SettingsLayer* layer, u64 control_id, MessageComposer& model,
                                            Utf8View placeholder, bool can_edit) noexcept {
    const auto target = layer->scene->active ? ui_settings_scene_focused(*layer->scene) : UiSettingsTarget{};
    const bool focused = can_edit && !layer->choice && target.kind == UiSettingsTargetKind::control &&
                         target.control_id == control_id;
    model.focused = focused;
    return {{message_composer_text(model), model.anchor, model.caret, true, focused, focused && layer->caret_visible},
            focused ? &g_app.renderer.media_picker_edit_geometry : nullptr,
            placeholder};
}

bool settings_layer_focus(SettingsLayer* layer, UiSettingsTarget target) noexcept {
    application_composer_text_input_suspend();
    layer->pending_surrogate = 0;
    ui_settings_scene_focus(layer->scene, target);
    layer->caret_visible = true;
    MessageComposer* model = nullptr;
    BoundedEdit* binding = nullptr;
    layer->blink_at = focused_editor(layer, &model, &binding, nullptr)
                          ? deadline_after(clock_monotonic_milliseconds(), kBlinkMs)
                          : 0;
    g_app.context_focus = &layer->scene->widgets;
    return repaint(layer);
}

void settings_layer_activate_target(SettingsLayer* layer, UiSettingsTarget target) noexcept {
    UiSettingsActivation activation{};
    if (target.kind == UiSettingsTargetKind::save || target.kind == UiSettingsTargetKind::reset ||
        target.kind == UiSettingsTargetKind::close) {
        activation.target = target;
        activation.kind = UiSettingsControlKind::action;
    } else if (!ui_settings_scene_activation(*layer->scene, target, false, &activation))
        return;
    dispatch(layer, activation, false, true);
}

bool settings_layer_open_choice(SettingsLayer* layer, u64 owner_id, ConstSlice<PopupItem> items, UiRect anchor,
                                float width) noexcept {
    if (!items.count)
        return false;
    if (layer->choice)
        settings_layer_close_choice(layer);
    if (!popup_scene_open(&g_app.popup_scene, PopupKind::settings_choice, owner_id, items, anchor,
                          application_viewport_dips(), width)
             .ok())
        return false;
    layer->choice = true;
    layer->popup_opens = g_app.popup_scene.opens;
    layer->popup_pressed = 0;
    g_app.popup_owner_kind = PopupOwnerKind::settings_choice;
    g_app.context_focus = &g_app.popup_scene.widgets;
    application_composer_text_input_suspend();
    return repaint(layer);
}

void settings_layer_close_choice(SettingsLayer* layer) noexcept {
    if (!layer->choice)
        return;
    layer->choice = false;
    layer->popup_pressed = 0;
    dismiss_popup();
    g_app.context_focus = &layer->scene->widgets;
}

bool settings_layer_choice_current(const SettingsLayer& layer) noexcept {
    return layer.choice && g_app.popup_scene.active && layer.popup_opens == g_app.popup_scene.opens;
}

void settings_layer_begin_entrance(SettingsLayer* layer, u64 now_ms, bool reduced_motion) noexcept {
    const bool motion = g_app.window && !reduced_motion && !g_app.renderer.live_resize &&
                        (!g_app.surface_motion.system_preference_cached || g_app.surface_motion.client_area_animation);
    layer->entrance_started = motion ? (now_ms ? now_ms : 1) : 0;
    renderer_set_settings_modal_progress(&g_app.renderer, motion ? 0.f : 1.f);
    application_request_frame();
}

void settings_layer_shake(SettingsLayer* layer, u64 now_ms) noexcept {
    layer->shake_started = now_ms ? now_ms : 1;
    application_request_frame();
}

u64 settings_layer_tick(SettingsLayer* layer, u64 now_ms, bool reduced_motion) noexcept {
    if (!layer->scene || !layer->scene->active)
        return 0;
    u64 wake = 0;
    MessageComposer* model = nullptr;
    BoundedEdit* binding = nullptr;
    if (focused_editor(layer, &model, &binding, nullptr)) {
        if (!layer->blink_at)
            layer->blink_at = deadline_after(now_ms, kBlinkMs);
        else if (deadline_due(now_ms, layer->blink_at)) {
            layer->caret_visible = !layer->caret_visible;
            layer->blink_at = deadline_after(now_ms, kBlinkMs);
            repaint(layer);
        }
        wake = layer->blink_at;
    } else
        layer->blink_at = 0;
    // Switch thumbs animate on the scene clock.
    if (ui_settings_scene_tick(layer->scene, now_ms, reduced_motion)) {
        application_request_frame();
        wake = wake ? minimum(wake, now_ms + 16) : now_ms + 16;
    }
    if (layer->entrance_started) {
        const auto durations = ui_settings_tokens(application_theme()).duration;
        const u64 duration = maximum<u64>(1, durations.modal_enter_ms);
        const u64 elapsed = now_ms > layer->entrance_started ? now_ms - layer->entrance_started : 0;
        const bool running = !reduced_motion && elapsed < duration;
        // Dialogs (Events, polls, invites, reports...) open on the same spring as
        // Settings; full-window layers keep their CSS ease-out settle.
        const bool dialog = layer->scene->presentation != UiSettingsPresentation::layer;
        renderer_set_settings_modal_progress(
            &g_app.renderer,
            !running ? 1.f
            : dialog ? ui_settings_modal_entrance(durations, elapsed)
                     : ui_motion_curve_sample(UiMotionCurve::css_ease_out,
                                              static_cast<float>(elapsed) / static_cast<float>(duration)));
        if (running) {
            application_request_frame();
            wake = wake ? minimum(wake, now_ms + 16) : now_ms + 16;
        } else
            layer->entrance_started = 0;
    }
    if (layer->shake_started) {
        const u64 elapsed = now_ms > layer->shake_started ? now_ms - layer->shake_started : 0;
        float shift = 0, alert = 1;
        if (!reduced_motion && elapsed < kShakeMs)
            shift = keyframe_shift(static_cast<float>(elapsed) / kShakeMs);
        if (elapsed >= kShakeMs)
            alert = elapsed >= kShakeMs + kAlertFadeMs
                        ? 0.f
                        : 1.f - static_cast<float>(elapsed - kShakeMs) / static_cast<float>(kAlertFadeMs);
        if (elapsed >= kShakeMs + kAlertFadeMs)
            layer->shake_started = 0;
        if (ui_settings_scene_set_footer_motion(layer->scene, shift, alert))
            repaint(layer);
        if (layer->shake_started) {
            application_request_frame();
            wake = wake ? minimum(wake, now_ms + 16) : now_ms + 16;
        }
    }
    return wake;
}

u64 settings_layer_next_wake(const SettingsLayer& layer) noexcept {
    if (layer.shake_started || layer.entrance_started || (layer.scene && ui_settings_scene_animating(*layer.scene)))
        return 1;
    return layer.blink_at;
}

bool settings_layer_backdrop_hit(const UiSettingsScene& scene, float x, float y) noexcept {
    return scene.active && scene.presentation != UiSettingsPresentation::layer && !scene.layout.compact &&
           ui_rect_contains(scene.viewport, x, y) && !ui_rect_contains(scene.layout.modal, x, y);
}

void settings_layer_cancel_pointer(SettingsLayer* layer) noexcept {
    layer->backdrop_pressed = false;
    layer->selecting = layer->scrollbar_dragging = layer->slider_dragging = false;
    if (layer->dragging || layer->drag_armed) {
        layer->dragging = layer->drag_armed = false;
        ui_settings_scene_set_drag(layer->scene, 0, 0, false);
    }
    layer->pressed = {};
    if (layer->scene->active)
        ui_button_pointer(&layer->scene->widgets, UiButtonPointerEvent::cancel, {});
    if (GetCapture() == g_app.window)
        ReleaseCapture();
}

UiRect settings_layer_edit_bounds(SettingsLayer* layer) noexcept {
    UiSettingsControlGeometry geometry{};
    MessageComposer* model = nullptr;
    BoundedEdit* binding = nullptr;
    return layer && layer->scene->active && focused_editor(layer, &model, &binding, nullptr) &&
                   ui_settings_scene_control_geometry(*layer->scene, ui_settings_scene_focused(*layer->scene),
                                                      &geometry)
               ? geometry.text
               : UiRect{};
}

bool settings_layer_edit_changed(SettingsLayer* layer, bool content) noexcept {
    MessageComposer* model = nullptr;
    BoundedEdit* binding = nullptr;
    u64 control = 0;
    if (!layer || !focused_editor(layer, &model, &binding, nullptr, &control))
        return false;
    if (content && !layer->hooks->edited(layer, control))
        return false;
    layer->caret_visible = true;
    layer->blink_at = deadline_after(clock_monotonic_milliseconds(), kBlinkMs);
    return repaint(layer);
}

bool settings_layer_replace_text(SettingsLayer* layer, Utf8View text) noexcept {
    MessageComposer* model = nullptr;
    BoundedEdit* binding = nullptr;
    if (!layer || !focused_editor(layer, &model, &binding, nullptr))
        return false;
    const u32 revision = model->text_revision;
    message_composer_select_all(model);
    const bool replaced = replace_selection(layer, text, true);
    return settings_layer_edit_changed(layer, replaced || model->text_revision != revision);
}

void settings_layer_cancel_native_group(SettingsLayer* layer, TextInputTarget target) noexcept {
    MessageComposer* model = nullptr;
    BoundedEdit* binding = nullptr;
    layer->pending_surrogate = 0;
    if (!focused_editor(layer, &model, &binding, nullptr))
        return;
    if (target.context == binding && target.identity == binding->binding_identity &&
        binding->document_identity == model->document_identity) {
        const auto base = message_composer_input_target(model);
        text_input_end_group(base);
        text_input_cancel(base);
    }
}

bool settings_layer_input(SettingsLayer* layer, UiInputKind kind, u32 key, bool repeated) noexcept {
    if (ui_input_consume_key_tail(&layer->held_key, {kind, key, repeated}))
        return true;
    if (!layer->scene || !layer->scene->active)
        return false;
    if (kind == UiInputKind::system_key_down && key == VK_F4 && (GetKeyState(VK_MENU) & 0x8000))
        return false;
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000);
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    auto* scene = layer->scene;
    if (layer->choice) {
        if (kind != UiInputKind::key_down)
            return true;
        if (key == VK_ESCAPE) {
            layer->held_key = key;
            settings_layer_close_choice(layer);
            layer->hooks->choose(layer, 0);
            repaint(layer);
        } else if (key == VK_UP || key == VK_DOWN || key == VK_TAB) {
            bool changed = false;
            popup_scene_move_selection(&g_app.popup_scene, key == VK_UP || (key == VK_TAB && shift) ? -1 : 1,
                                       &changed);
            if (changed)
                repaint(layer);
        } else if ((key == VK_RETURN || key == VK_SPACE) && !repeated) {
            const u32 action = popup_scene_selected_action(g_app.popup_scene);
            layer->held_key = key;
            settings_layer_close_choice(layer);
            layer->hooks->choose(layer, action);
            repaint(layer);
        }
        return true;
    }
    MessageComposer* model = nullptr;
    BoundedEdit* binding = nullptr;
    bool multiline = false;
    const bool editing = focused_editor(layer, &model, &binding, &multiline);
    if (kind == UiInputKind::character) {
        if (!editing || control || key > 0xffff || key < 32 || (key >= 127 && key <= 159))
            return true;
        u32 scalar = key;
        if (key >= 0xd800 && key <= 0xdbff) {
            layer->pending_surrogate = static_cast<u16>(key);
            return true;
        }
        if (key >= 0xdc00 && key <= 0xdfff) {
            if (!layer->pending_surrogate)
                return true;
            scalar = 0x10000 + ((layer->pending_surrogate - 0xd800) << 10) + (key - 0xdc00);
        }
        layer->pending_surrogate = 0;
        char encoded[4]{};
        const u32 bytes = utf8_encode(scalar, encoded);
        if (bytes && replace_selection(layer, {encoded, bytes}, false))
            settings_layer_edit_changed(layer, true);
        return true;
    }
    if (kind != UiInputKind::key_down)
        return true;
    if (key == VK_ESCAPE) {
        layer->held_key = key;
        layer->hooks->close(layer);
        return true;
    }
    if (key == VK_TAB) {
        layer->held_key = key;
        application_composer_text_input_suspend();
        const float before = scene->content_offset;
        ui_settings_scene_move_focus(scene, shift);
        settings_layer_focus(layer, ui_settings_scene_focused(*scene));
        glide_reveal(layer, before);
        return true;
    }
    if (key == VK_RETURN && control && !repeated) {
        layer->held_key = key;
        settings_layer_activate_target(layer, {UiSettingsTargetKind::save, scene->selected_page, 0});
        return true;
    }
    if (!editing) {
        const auto focused = ui_settings_scene_focused(*scene);
        if ((key == VK_RETURN || key == VK_SPACE) && !repeated) {
            layer->held_key = key;
            settings_layer_activate_target(layer, focused);
        } else if (key == VK_UP || key == VK_DOWN) {
            const float before = scene->content_offset;
            if (!ui_settings_scene_move_within(scene, key == VK_UP))
                ui_settings_scene_move_focus(scene, key == VK_UP);
            settings_layer_focus(layer, ui_settings_scene_focused(*scene));
            glide_reveal(layer, before);
        } else if (key == VK_LEFT || key == VK_RIGHT || key == VK_HOME || key == VK_END) {
            UiSettingsActivation activation{};
            const auto step = key == VK_LEFT    ? UiSettingsSliderKey::decrease
                              : key == VK_RIGHT ? UiSettingsSliderKey::increase
                              : key == VK_HOME  ? UiSettingsSliderKey::home
                                                : UiSettingsSliderKey::end;
            if (ui_settings_scene_slider_key(*scene, focused, step, &activation))
                dispatch(layer, activation, false, false);
        } else if (key == VK_PRIOR || key == VK_NEXT) {
            glide_to(layer, false,
                     scene->content_offset + (key == VK_PRIOR ? -1.f : 1.f) * ui_rect_height(scene->viewport) * .8f);
        }
        return true;
    }
    const u32 revision = model->text_revision;
    const auto native_target = bounded_edit_input_target(binding);
    const bool destructive = key == VK_BACK || key == VK_DELETE || (control && key == 'X');
    const bool group = destructive && text_input_begin_group(native_target);
    auto result = MessageComposerResult::unchanged;
    if (key == VK_RETURN) {
        layer->held_key = key;
        if (group)
            text_input_end_group(native_target);
        if (multiline && replace_selection(layer, utf8_literal("\n"), true))
            settings_layer_edit_changed(layer, true);
        return true;
    }
    if (control && key == 'A')
        result = message_composer_select_all(model);
    else if (control && (key == 'C' || key == 'X')) {
        const u32 first = minimum(model->anchor, model->caret), last = maximum(model->anchor, model->caret);
        if (last > first && clipboard_write_utf8(g_app.window, {model->text + first, last - first}) && key == 'X')
            result = message_composer_replace_range(model, first, last, {});
    } else if (control && key == 'V') {
        char buffer[kEditBufferUnits];
        const auto read = clipboard_read_utf8(g_app.window, {buffer, sizeof(buffer)});
        if (read.ok() && replace_selection(layer, {buffer, read.bytes}, true))
            result = MessageComposerResult::changed;
    } else if (control && key == 'Z')
        result = shift ? message_composer_redo(model) : message_composer_undo(model);
    else if (control && key == 'Y')
        result = message_composer_redo(model);
    else if (key == VK_BACK)
        result = control ? message_composer_delete_word(model, false) : message_composer_backspace(model);
    else if (key == VK_DELETE)
        result = control ? message_composer_delete_word(model, true) : message_composer_delete_forward(model);
    else if (key == VK_LEFT || key == VK_RIGHT)
        result = message_composer_move(
            model,
            control ? (key == VK_LEFT ? MessageComposerMove::previous_word : MessageComposerMove::next_word)
                    : (key == VK_LEFT ? MessageComposerMove::previous : MessageComposerMove::next),
            shift);
    else if (key == VK_HOME || key == VK_END)
        result = message_composer_move(
            model, key == VK_HOME ? MessageComposerMove::document_home : MessageComposerMove::document_end, shift);
    else if (key == VK_UP || key == VK_DOWN) {
        if (group)
            text_input_end_group(native_target);
        if (!multiline) {
            ui_settings_scene_move_focus(scene, key == VK_UP);
            settings_layer_focus(layer, ui_settings_scene_focused(*scene));
            return true;
        }
        const auto& edit_layout = g_app.renderer.media_picker_layout;
        select_point(layer, edit_layout.caret.left,
                     edit_layout.caret.top + (key == VK_UP ? -.5f : 1.5f) * edit_layout.caret.height, shift);
        return true;
    }
    if (group)
        text_input_end_group(native_target);
    if (result == MessageComposerResult::changed)
        settings_layer_edit_changed(layer, revision != model->text_revision);
    return true;
}

bool settings_layer_window_message(SettingsLayer* layer, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (!layer->scene || !layer->scene->active)
        return false;
    auto* scene = layer->scene;
    if (message == WM_KILLFOCUS || message == WM_CANCELMODE || message == WM_CAPTURECHANGED) {
        layer->backdrop_pressed = false;
        layer->selecting = layer->scrollbar_dragging = layer->slider_dragging = false;
        if (layer->dragging || layer->drag_armed) {
            layer->dragging = layer->drag_armed = false;
            ui_settings_scene_set_drag(scene, 0, 0, false);
            application_request_frame();
        }
        layer->pressed = {};
        ui_button_pointer(&scene->widgets, UiButtonPointerEvent::cancel, {});
        if (message != WM_CAPTURECHANGED && GetCapture() == g_app.window)
            ReleaseCapture();
        if (message == WM_KILLFOCUS) {
            if (layer->choice) {
                settings_layer_close_choice(layer);
                layer->hooks->choose(layer, 0);
            }
            application_composer_text_input_suspend();
        }
        return false;
    }
    if (message == WM_MOUSELEAVE) {
        bool changed = ui_button_pointer(&scene->widgets, UiButtonPointerEvent::move, {-1, -1}).changed;
        changed = ui_settings_scene_palette_hover(scene, {}, -1, -1) || changed;
        if (changed)
            repaint(layer);
        return true;
    }
    if (message == WM_MOUSEWHEEL) {
        if (layer->choice) {
            // The choice menu glides like every other list (and jumps when
            // smooth scrolling is off); each animated step repaints the layer.
            const UiScrollBinding choice{layer, 0x53434849u /* 'SCHI' */,
                [](void* context, u64, UiScrollRange* range) noexcept {
                    const auto& popup = g_app.popup_scene;
                    if (!static_cast<SettingsLayer*>(context)->choice || !popup.active) return false;
                    *range = {popup.rectangle, popup.scroll_offset, popup.scroll_max, kPopupRowHeight, 1};
                    return true;
                },
                [](void* context, u64, double offset, bool* changed) noexcept {
                    if (!popup_scene_scroll(&g_app.popup_scene,
                                            static_cast<float>(offset - g_app.popup_scene.scroll_offset), changed))
                        return false;
                    if (*changed) repaint(static_cast<SettingsLayer*>(context));
                    return true;
                }};
            application_modal_scroll_wheel(&g_app.scroll_host, choice, g_app.window, g_app.renderer.dpi, wparam,
                                           lparam, clock_microseconds(g_app.clock, g_app.process_start, clock_now()));
            return true;
        }
        POINT point{static_cast<i16>(LOWORD(lparam)), static_cast<i16>(HIWORD(lparam))};
        ScreenToClient(g_app.window, &point);
        const float scale = 96.f / maximum(g_app.renderer.dpi, 1u);
        const bool sidebar = ui_rect_contains(scene->layout.sidebar, point.x * scale, point.y * scale);
        application_modal_scroll_wheel(&g_app.scroll_host, scroll_binding(layer, sidebar), g_app.window,
                                       g_app.renderer.dpi, wparam, lparam, scroll_now_us());
        return true;
    }
    if (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP ||
        message == WM_LBUTTONDBLCLK) {
        if (message == WM_LBUTTONDBLCLK)
            message = WM_LBUTTONDOWN;
        const float x = client_dip_x(lparam), y = client_dip_y(lparam);
        if (layer->choice)
            return choose_pointer(layer, message, x, y);
        if (layer->scrollbar_dragging) {
            if (message == WM_MOUSEMOVE) {
                const bool side = layer->scrollbar_sidebar;
                const auto bar = ui_settings_scene_scrollbar(*scene, side);
                ui_settings_scene_scroll(scene, side,
                                         ui_scrollbar_scroll_from_pointer(bar, y, layer->scrollbar_grab) -
                                             (side ? scene->sidebar_offset : scene->content_offset));
                repaint(layer);
            } else if (message == WM_LBUTTONUP) {
                layer->scrollbar_dragging = false;
                if (GetCapture() == g_app.window)
                    ReleaseCapture();
            }
            return true;
        }
        if (layer->slider_dragging) {
            UiSettingsActivation activation{};
            if (message != WM_LBUTTONDOWN &&
                ui_settings_scene_slider_pointer(*scene, layer->pressed, x, &activation))
                dispatch(layer, activation, true, false);
            if (message == WM_LBUTTONUP) {
                layer->slider_dragging = false;
                layer->pressed = {};
                if (GetCapture() == g_app.window)
                    ReleaseCapture();
            }
            return true;
        }
        if (layer->drag_armed) {
            if (message == WM_MOUSEMOVE) {
                if (!layer->dragging && (y - layer->press_y > 4 || layer->press_y - y > 4))
                    layer->dragging = true;
                if (layer->dragging) {
                    u64 target = 0;
                    bool after = false;
                    // Scroll while holding near the list edges.
                    const auto* body = ui_widget_manager_layout_node(scene->widgets, scene->body);
                    if (body && y < body->rectangle.top + 24)
                        ui_settings_scene_scroll(scene, false, -12);
                    else if (body && y > body->rectangle.bottom - 24)
                        ui_settings_scene_scroll(scene, false, 12);
                    if (ui_settings_scene_drop_target(*scene, y, &target, &after)) {
                        layer->drop_target = target;
                        layer->drop_below = after;
                        ui_settings_scene_set_drag(scene, layer->drag_id, target, after);
                    }
                    repaint(layer);
                }
            } else if (message == WM_LBUTTONUP) {
                const u64 dragged = layer->drag_id, target = layer->drop_target;
                const bool after = layer->drop_below, moved = layer->dragging;
                layer->drop_target = 0;
                layer->drag_armed = layer->dragging = false;
                ui_settings_scene_set_drag(scene, 0, 0, false);
                if (GetCapture() == g_app.window)
                    ReleaseCapture();
                if (moved && target && target != dragged && layer->hooks->drop)
                    layer->hooks->drop(layer, dragged, target, after);
                repaint(layer);
            }
            return true;
        }
        if (message == WM_LBUTTONDOWN) {
            for (u32 side = 0; side < 2; ++side) {
                const auto bar = ui_settings_scene_scrollbar(*scene, side != 0);
                if (!ui_scrollbar_track_hit(bar, x, y))
                    continue;
                // The thumb tracks the pointer directly; paging glides.
                ui_scroll_host_cancel(&g_app.scroll_host);
                if (ui_scrollbar_thumb_hit(bar, x, y)) {
                    layer->scrollbar_dragging = true;
                    layer->scrollbar_sidebar = side != 0;
                    layer->scrollbar_grab = y - bar.thumb.top;
                    SetCapture(g_app.window);
                } else {
                    glide_to(layer, side != 0,
                             (side ? scene->sidebar_offset : scene->content_offset) +
                                 (y < bar.thumb.top ? -1.f : 1.f) * ui_rect_height(bar.track));
                }
                return true;
            }
        }
        // The official client closes a dialog when a click lands on its scrim; the owner's
        // close path keeps its own unsaved-changes guard.
        if (layer->backdrop_pressed) {
            if (message == WM_LBUTTONUP) {
                layer->backdrop_pressed = false;
                if (GetCapture() == g_app.window)
                    ReleaseCapture();
                if (settings_layer_backdrop_hit(*scene, x, y))
                    settings_layer_activate_target(layer, {UiSettingsTargetKind::close, {}, 0});
            }
            return true;
        }
        if (message == WM_LBUTTONDOWN && settings_layer_backdrop_hit(*scene, x, y)) {
            layer->backdrop_pressed = true;
            SetCapture(g_app.window);
            return true;
        }
        if (layer->selecting) {
            if (message == WM_MOUSEMOVE)
                select_point(layer, x, y, true);
            else if (message == WM_LBUTTONUP) {
                layer->selecting = false;
                if (GetCapture() == g_app.window)
                    ReleaseCapture();
            }
            return true;
        }
        UiSettingsControlGeometry field{};
        const bool editor = editable(layer) && ui_settings_scene_editor_hit(*scene, x, y, &field);
        const auto target = ui_settings_scene_hit_test(*scene, x, y);
        const auto pointer = ui_button_pointer(&scene->widgets,
                                               message == WM_LBUTTONDOWN ? UiButtonPointerEvent::press
                                               : message == WM_LBUTTONUP ? UiButtonPointerEvent::release
                                                                         : UiButtonPointerEvent::move,
                                               {x, y});
        bool changed = pointer.changed;
        if (message == WM_MOUSEMOVE) {
            changed = ui_settings_scene_palette_hover(scene, target, x, y) || changed;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, g_app.window, 0};
            TrackMouseEvent(&track);
        }
        if (changed)
            repaint(layer);
        if (message == WM_LBUTTONDOWN) {
            SetFocus(g_app.window);
            if (!editor && ui_settings_scene_grip_hit(*scene, target, x, y)) {
                layer->drop_target = 0;
                layer->drag_armed = true;
                layer->dragging = false;
                layer->drag_id = target.control_id;
                layer->press_y = y;
                SetCapture(g_app.window);
                return true;
            }
            settings_layer_focus(layer, editor ? field.target : target);
            if (editor) {
                select_point(layer, x, y, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
                layer->selecting = true;
            } else if (ui_settings_scene_slider_hit(*scene, target, x, y)) {
                UiSettingsActivation activation{};
                layer->slider_dragging = true;
                layer->pressed = target;
                if (ui_settings_scene_slider_pointer(*scene, target, x, &activation))
                    dispatch(layer, activation, true, false);
            } else {
                layer->pressed = target;
                layer->pressed_revision = scene->revision;
            }
            SetCapture(g_app.window);
        } else if (message == WM_LBUTTONUP) {
            const auto pressed = layer->pressed;
            layer->pressed = {};
            if (GetCapture() == g_app.window)
                ReleaseCapture();
            if (!pointer.activate || !ui_settings_target_equal(pressed, target))
                return true;
            UiSettingsActivation activation{};
            if (ui_settings_scene_option_pointer(*scene, target, x, y, &activation) ||
                ui_settings_scene_radio_pointer(*scene, target, x, y, &activation) ||
                ui_settings_scene_palette_pointer(*scene, target, x, y, &activation)) {
                dispatch(layer, activation, true, true);
                return true;
            }
            if (target.kind == UiSettingsTargetKind::control) {
                const auto* c = ui_settings_scene_control(*scene, target);
                if (c && (c->kind == UiSettingsControlKind::segments || c->kind == UiSettingsControlKind::swatches ||
                          c->kind == UiSettingsControlKind::tabs || c->kind == UiSettingsControlKind::buttons ||
                          c->kind == UiSettingsControlKind::colors ||
                          c->kind == UiSettingsControlKind::radio || c->kind == UiSettingsControlKind::palette))
                    return true;
            }
            if (!ui_settings_scene_activation(*scene, target, false, &activation)) {
                if (target.kind != UiSettingsTargetKind::save && target.kind != UiSettingsTargetKind::reset &&
                    target.kind != UiSettingsTargetKind::close)
                    return true;
                activation = {target, UiSettingsControlKind::action, false, 0};
            }
            const bool button = target.kind != UiSettingsTargetKind::control ||
                                ui_settings_scene_button_hit(*scene, target, x, y);
            SettingsLayerActivation full{activation, true, button, {}};
            if (target.kind == UiSettingsTargetKind::control) {
                UiSettingsControlGeometry geometry{};
                if (ui_settings_scene_control_geometry(*scene, target, &geometry))
                    full.anchor = button ? geometry.field : UiRect{x, y, x + 1, y + 1};
            }
            layer->hooks->activate(layer, full);
        }
        return true;
    }
    if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT) {
        UiPoint point{};
        const bool popup = layer->choice && g_app.popup_scene.active;
        application_set_cursor(!application_cursor_point(g_app.window, g_app.renderer.dpi, lparam, &point)
                                   ? UiCursor::arrow
                               : popup ? ui_popup_scene_cursor(g_app.popup_scene, point.x, point.y)
                                       : ui_settings_scene_cursor(*scene, point.x, point.y, layer->dragging));
        return true;
    }
    if (message == WM_DROPFILES) {
        DragFinish(reinterpret_cast<HDROP>(wparam));
        return true;
    }
    return message == WM_CONTEXTMENU || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP ||
           message == WM_MBUTTONDOWN || message == WM_MBUTTONUP || message == WM_MOUSEHWHEEL;
}

} // namespace maicord::app_detail
