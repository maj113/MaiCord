#pragma once
#include "../shell/owner.hpp"
#include "layer.hpp"
#include "maicord/ui/shell/page_registry.hpp"

namespace maicord::app_detail {

// The scene, input layer and page registry every settings modal starts with. A modal's owner derives
// from it and the functions below carry the open/close/release sequence all of them share; the owner
// keeps its values, models and page projection.
struct SettingsModal {
    UiSettingsScene scene;
    SettingsLayer layer;
    UiPageRegistry registry;
};
// A dialog with one page (the page registry holds just `page`).
struct SettingsDialog : SettingsModal {
    UiPage page;
    UiSettingsPageContent content;
};

// Allocates a modal's owner with its layer bound to `hooks`; null when the system refuses.
template <typename Owner>
[[nodiscard]] inline Owner* settings_modal_allocate(const SettingsLayerHooks& hooks) noexcept {
    auto* owner = owner_allocate<Owner>();
    if (owner) {
        owner->layer.scene = &owner->scene;
        owner->layer.hooks = &hooks;
    }
    return owner;
}
// Releases the scene and frees the owner (the caller releases its own resources first).
template <typename Owner>
inline void settings_modal_free(Owner*& owner) noexcept {
    ui_settings_scene_release(&owner->scene);
    owner_release(owner);
}

// Layer hooks that forward to a modal's own functions (the layer's `close` hook, which closes with focus
// restored, and its predicate/repaint hooks) or answer for a modal without text editors.
template <void (*Close)(bool) noexcept>
void settings_hook_close(SettingsLayer*) noexcept {
    Close(true);
}
template <bool (*Call)() noexcept>
bool settings_hook(SettingsLayer*) noexcept {
    return Call();
}
inline void settings_hook_ignore_choice(SettingsLayer*, u32) noexcept {}
inline bool settings_hook_edited_none(SettingsLayer*, u64) noexcept {
    return false;
}
inline SettingsLayerEditor settings_hook_editor_none(SettingsLayer*, u64) noexcept {
    return {};
}
inline bool settings_hook_not_editable(SettingsLayer*) noexcept {
    return false;
}

// The text-editor plumbing of a modal whose owner (the pointer variable `*Slot`) has `layer`, `lifetime`
// and `models[]`, `bindings[]` and `editors[]` indexed by editor; `Editable` is the owner's own test of
// whether editors take input (it must itself refuse a null owner).
template <auto* Slot, bool (*Editable)() noexcept>
struct SettingsEditors {
    // Ready callback of a bound editor: the modal takes input, the binding is current and `model` has the focus.
    static bool ready(void* model, u64 identity) noexcept {
        auto& o = **Slot;
        MessageComposer* focused = nullptr;
        return Editable() && o.lifetime == identity &&
               settings_layer_editor(&o.layer, nullptr, &focused, nullptr) && focused == model;
    }
    // Shows the first `bytes` bytes of editor `index`'s borrowed storage with the caret at the end and rebinds it.
    static void load(u32 index, u32 bytes, u32 limit, bool multiline) noexcept {
        auto& o = **Slot;
        auto& model = o.models[index];
        message_composer_refresh_borrowed(&model, bytes);
        message_composer_set_selection(&model, bytes, bytes);
        bounded_edit_bind(&o.bindings[index], &model, limit, multiline, o.lifetime, ready, &model);
    }
    // The scene view of editor `index` for `control`.
    static const UiSettingsEditor* view(u32 index, u64 control, const Utf8View& placeholder, bool enabled) noexcept {
        auto& o = **Slot;
        o.editors[index] = settings_layer_editor_view(&o.layer, control, o.models[index], placeholder, enabled);
        return &o.editors[index];
    }
};

// Closes the surfaces a settings modal replaces; call before the owner's state changes.
void settings_modal_yield() noexcept;
// Takes input from the composer and popups: ends text input, dismisses the popup, cancels scrolling.
void settings_modal_take_input() noexcept;
// Opens a one-page dialog titled `title`: resets the layer, registers `page` and takes input from the
// composer. False when the registry refuses (the caller returns false).
[[nodiscard]] bool settings_dialog_begin(SettingsDialog& dialog, UiPageKey page, Utf8View title) noexcept;
// Shows the scene through the renderer, plays the layer entrance and gives the scene the focus.
void settings_modal_bind(SettingsModal& modal) noexcept;
// Makes the prepared modal visible: sets `focus` (a control id; zero none) on the scene and, with
// `focus_layer`, on the input layer; updates the scene, builds its page with `project`, binds it to the
// renderer and plays the entrance. False leaves the caller to close it; the caller then builds the text.
[[nodiscard]] bool settings_dialog_show(SettingsDialog& dialog, UiPageKey page, u64 focus, bool focus_layer,
                                        bool (*project)() noexcept) noexcept;
// The first half of closing: ends text input, the choice popup and pointer capture.
void settings_modal_suspend(SettingsModal& modal) noexcept;
// Unbinds the scene from the renderer and closes it.
void settings_modal_unbind(SettingsModal& modal) noexcept;
// Gives the shell the focus back (when `restore_focus`) and its attachment drop target.
void settings_modal_restore_focus(bool restore_focus) noexcept;
// Rebuilds the shell text and requests a frame after a close.
void settings_modal_repaint_shell() noexcept;
// The usual end of a close: unbind, restore focus, repaint the shell.
void settings_modal_leave(SettingsModal& modal, bool restore_focus) noexcept;
// The projection a repaint presents: false when `projected` failed; otherwise a frame and the page text.
[[nodiscard]] bool settings_modal_present(bool projected) noexcept;
// The end of a page projection: the focus ring follows the keyboard and the scene updates.
[[nodiscard]] bool settings_modal_update(SettingsModal& modal) noexcept;
// Rebuilds the page text unless the scene's own text is current.
[[nodiscard]] bool settings_modal_sync_text(const SettingsModal& modal) noexcept;
// Updates the scene and rebuilds the page text only when the scene changed it.
[[nodiscard]] bool settings_modal_refresh(SettingsModal& modal) noexcept;

} // namespace maicord::app_detail
