#pragma once
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/popup_scene.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/model/message_composer.hpp"
#include "maicord/text/bounded_edit.hpp"
#include "maicord/core/win32_types.hpp"

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
struct SettingsLayerHooks {
    // Model and binding of an editable text control on the current page.
    bool (*editor)(SettingsLayer*, u64 control_id, MessageComposer** model, BoundedEdit** binding,
                   bool* multiline) noexcept;
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
};
struct SettingsLayer {
    UiSettingsScene* scene;
    const SettingsLayerHooks* hooks;
    void* owner;
    UiSettingsTarget pressed;
    u64 blink_at, shake_started, drag_id, entrance_started, drop_target;
    u32 pressed_revision, popup_opens, popup_pressed, held_key;
    float scrollbar_grab, press_y;
    u16 pending_surrogate;
    bool caret_visible, selecting, scrollbar_dragging, slider_dragging, choice, dragging, drag_armed,
        scrollbar_sidebar, drop_below;
    // A press on a dialog's scrim; releasing it there closes the dialog.
    bool backdrop_pressed;
};

void settings_layer_reset(SettingsLayer*) noexcept;
// Focused text control, if editable.
bool settings_layer_editor(SettingsLayer*, u64* control_id, MessageComposer** model, BoundedEdit** binding,
                           bool* multiline) noexcept;
UiSettingsEditor settings_layer_editor_view(SettingsLayer*, u64 control_id, MessageComposer& model,
                                            Utf8View placeholder, bool editable) noexcept;
bool settings_layer_focus(SettingsLayer*, UiSettingsTarget) noexcept;
// Builds the scene activation for a keyboard target and dispatches it.
void settings_layer_activate_target(SettingsLayer*, UiSettingsTarget) noexcept;
bool settings_layer_open_choice(SettingsLayer*, u64 owner_id, ConstSlice<PopupItem>, UiRect anchor,
                                float width) noexcept;
void settings_layer_close_choice(SettingsLayer*) noexcept;
bool settings_layer_choice_current(const SettingsLayer&) noexcept;
// Starts the layer entrance: the layer fades in from 110% over the receding shell.
void settings_layer_begin_entrance(SettingsLayer*, u64 now_ms, bool reduced_motion) noexcept;
// Shakes the unsaved-changes bar (300 ms, 5 DIP) and tints it briefly.
void settings_layer_shake(SettingsLayer*, u64 now_ms) noexcept;
// Caret blink and bar motion; returns the next wake or zero.
u64 settings_layer_tick(SettingsLayer*, u64 now_ms, bool reduced_motion) noexcept;
u64 settings_layer_next_wake(const SettingsLayer&) noexcept;
bool settings_layer_input(SettingsLayer*, UiInputKind, u32 key, bool repeated) noexcept;
bool settings_layer_window_message(SettingsLayer*, UINT, WPARAM, LPARAM) noexcept;
// Releases capture and pointer state; keeps focus.
void settings_layer_cancel_pointer(SettingsLayer*) noexcept;
// A point on the scrim around a dialog or modal (never a full-window layer).
bool settings_layer_backdrop_hit(const UiSettingsScene&, float x, float y) noexcept;
// Native text input glue for the focused editor.
UiRect settings_layer_edit_bounds(SettingsLayer*) noexcept;
bool settings_layer_edit_changed(SettingsLayer*, bool content) noexcept;
// Replaces the focused editor's whole text through the paste path (UI
// Automation Value.SetValue). False when no editor is focused and editable.
bool settings_layer_replace_text(SettingsLayer*, Utf8View text) noexcept;
void settings_layer_cancel_native_group(SettingsLayer*, TextInputTarget) noexcept;

} // namespace maicord::app_detail
