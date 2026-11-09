#pragma once
#include "maicord/ui/settings/scene.hpp"
#include "maicord/ui/shell/popup_scene.hpp"
#include "maicord/ui/core/input_router.hpp"
#include "maicord/model/chat/message_composer.hpp"
#include "maicord/model/chat/bounded_edit.hpp"
#include "maicord/platform/window/event.hpp"
#include "../shell/state.hpp"

namespace maicord::app_detail {

// Shared input engine for full-window settings layers (Server and Channel
// Settings): inline text editing, choice popups, pointer routing, scrolling,
// list drag and the unsaved-changes shake. The owning page host keeps the
// scene, registry, models and all values; this engine never mutates them
// except through its hooks.
struct SettingsLayer;
struct SettingsLayerActivation {
    UiSettingsActivation value;
    // Pointer activations know whether the row's own button was hit.
    bool pointer, button;
    // Popup anchor for the activated control or button.
    UiRect anchor;
};
// An editable text control on the current page: its model, binding and whether Enter breaks lines. A null
// model means the control is not an editor.
struct SettingsLayerEditor {
    MessageComposer* model;
    BoundedEdit* binding;
    bool multiline;
};
struct SettingsLayerHooks {
    SettingsLayerEditor (*editor)(SettingsLayer*, u64 control_id) noexcept;
    // The focused editor's text changed.
    bool (*edited)(SettingsLayer*, u64 control_id) noexcept;
    void (*activate)(SettingsLayer*, const SettingsLayerActivation&) noexcept;
    // A choice popup item was picked; zero when it closed without a pick.
    void (*choose)(SettingsLayer*, u32 action) noexcept;
    // ESC or the close button.
    void (*close)(SettingsLayer*) noexcept;
    // Rebuild projections, update the scene and request text.
    bool (*repaint)(SettingsLayer*) noexcept;
    // A grip row was dropped next to another row.
    void (*drop)(SettingsLayer*, u64 dragged, u64 target, bool after) noexcept;
    // Whether editors accept input right now.
    bool (*editable)(SettingsLayer*) noexcept;
    // Right-click or the menu key on a control; null leaves both ignored.
    void (*context)(SettingsLayer*, const SettingsLayerActivation&) noexcept;
    // Enter in a single-line editor; null ignores it.
    void (*editor_enter)(SettingsLayer*, u64 control_id) noexcept;
};
struct SettingsLayer {
    UiSettingsScene* scene;
    const SettingsLayerHooks* hooks;
    UiSettingsTarget pressed;
    u64 blink_at, shake_started, drag_id, entrance_started, drop_target;
    u32 pressed_revision, popup_opens, popup_pressed, held_key;
    float scrollbar_grab, press_y;
    u16 pending_surrogate;
    bool caret_visible, selecting, scrollbar_dragging, slider_dragging, choice, dragging, drag_armed,
        scrollbar_sidebar, drop_below;
    // A press on a dialog's scrim; releasing it there closes the dialog.
    bool backdrop_pressed;
    // A press in an editor placed its caret: later moves extend from it. A press into a field whose text layout is
    // not built yet places the caret at the first point that maps instead.
    bool select_anchored;
};
// Rows of side-by-side options that a click picks directly.
inline bool settings_option_row(UiSettingsControlKind kind) noexcept {
    return kind == UiSettingsControlKind::segments || kind == UiSettingsControlKind::swatches ||
           kind == UiSettingsControlKind::tabs || kind == UiSettingsControlKind::buttons ||
           kind == UiSettingsControlKind::colors;
}

void settings_layer_reset(SettingsLayer*) noexcept;
// Focused text control, if editable.
bool settings_layer_editor(SettingsLayer*, u64* control_id, MessageComposer** model, BoundedEdit** binding) noexcept;
UiSettingsEditor settings_layer_editor_view(SettingsLayer*, u64 control_id, MessageComposer& model,
                                            const Utf8View& placeholder, bool editable) noexcept;
bool settings_layer_focus(SettingsLayer*, UiSettingsTarget) noexcept;
// Builds the scene activation for a keyboard target and dispatches it.
void settings_layer_activate_target(SettingsLayer*, UiSettingsTarget) noexcept;
bool settings_layer_open_choice(SettingsLayer*, u64 owner_id, const ConstSlice<PopupItem>&, const UiRect& anchor,
                                float width) noexcept;
void settings_layer_close_choice(SettingsLayer*) noexcept;
// Clears a choice popup that another surface replaced behind the layer's back; true when it did.
bool settings_layer_choice_lapsed(SettingsLayer&) noexcept;
// Starts the layer entrance: the layer fades in from 110% over the receding shell.
void settings_layer_begin_entrance(SettingsLayer*) noexcept;
// Shakes the unsaved-changes bar (300 ms, 5 DIP) and tints it briefly.
void settings_layer_shake(SettingsLayer*) noexcept;
// Caret blink and bar motion on the application clock.
void settings_layer_tick(SettingsLayer*, u64 now_ms) noexcept;
u64 settings_layer_next_wake(const SettingsLayer&) noexcept;
bool settings_layer_input(SettingsLayer*, UiInputKind, u32 key, bool repeated) noexcept;
bool settings_layer_window_event(SettingsLayer*, const WindowEvent&) noexcept;
// Releases capture and pointer state; keeps focus.
void settings_layer_cancel_pointer(SettingsLayer*) noexcept;
// Native text input glue for the focused editor.
UiRect settings_layer_edit_bounds(SettingsLayer*) noexcept;
bool settings_layer_edit_changed(SettingsLayer*, bool content) noexcept;
// Replaces the focused editor's whole text through the paste path (UI
// Automation Value.SetValue). False when no editor is focused and editable.
bool settings_layer_replace_text(SettingsLayer*, Utf8View text) noexcept;
void settings_layer_cancel_native_group(SettingsLayer*, const TextInputTarget&) noexcept;
// Scrolls a settings scene's view to `target` on the wheel's curve (a jump when smooth scrolling is off or motion is
// reduced): keyboard paging and track clicks. Inline: the settings modal and the layers link it without each other.
__declspec(noinline) inline void settings_glide_to(const UiScrollBinding& binding, float target) noexcept {
    ui_scroll_host_scroll_to(&g_app.scroll_host, binding, target, application_now_us(),
                             96.0 / maximum(g_app.renderer.dpi, 1u));
}
// Focus moves reveal their target inside the scene at once; replays the page body's reveal from `before` on the same
// curve. `body` is the scene's body binding.
__declspec(noinline) inline void settings_glide_reveal(UiSettingsScene* scene, const UiScrollBinding& body,
                                                       float before) noexcept {
    const float after = scene->content_offset;
    if (after == before)
        return;
    ui_settings_scene_scroll(scene, false, before - after);
    settings_glide_to(body, after);
}

} // namespace maicord::app_detail
