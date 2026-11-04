#pragma once
#include "maicord/core/color_hex.hpp"
#include "maicord/ui/text/secure_edit.hpp"
#include "maicord/ui/core/input_router.hpp"
#include "../threads/forum_post.hpp"
#include "maicord/core/win32_types.hpp"
#include "maicord/platform/window/event.hpp"

namespace maicord::app_detail {
struct TextFormOwnerKey {
    u64 scope, collection, item;
};
using TextFormOwnerValid = bool (*)(TextFormOwnerKey key) noexcept;
using TextFormSave = bool (*)(TextFormOwnerKey key, Utf8View text) noexcept;
// Confirmations with an option row receive its state instead of text.
using TextFormSaveOption = bool (*)(TextFormOwnerKey key, bool option_checked) noexcept;
// Forms with a radio group or dropdown: the text, whether it was typed since
// the last choice, the chosen option (~0u for none) and the dropdown's.
using TextFormSaveChoices = bool (*)(TextFormOwnerKey key, Utf8View text, bool typed, u32 choice,
                                     u32 select) noexcept;
struct ApplicationTextFormProgress {
    Utf8View status, accept;
    u64 revision;
    bool editable = true, can_accept = true, complete = false;
    bool can_cancel = true, dismissible = true;
};
using TextFormProgress = ApplicationTextFormProgress (*)(TextFormOwnerKey key) noexcept;
// Reads an editor's text after every edit: whether it is acceptable, and the
// colour it names (0xRRGGBBAA) for forms with a swatch.
using TextFormCheck = bool (*)(TextFormOwnerKey key, Utf8View text, u32* swatch_rgba) noexcept;
using TextFormClosed = void (*)(TextFormOwnerKey key) noexcept;
// An aggregate whose default is all zero bytes: `ApplicationTextFormRequest request{}` is one fill, not a
// store per field at every one of its dozens of call sites.
struct ApplicationTextFormRequest {
    Utf8View title, context, label, accept, initial;
    TextFormOwnerKey owner;
    TextFormOwnerValid valid;
    TextFormSave save;
    u32 maximum_codepoints;
    bool confirmation_only;
    // Optional asynchronous controller. Polls are borrowed views only; closing
    // cancels this form's controller while accepted transport responses finish.
    u32 maximum_bytes;
    bool single_line;
    TextFormProgress progress;
    TextFormClosed closed;
    // A child may cover only this live post intent. All other forms keep
    // the ordinary modal rule.
    ApplicationForumPostTicket forum_parent;
    // Startup recovery may cover login without allocating or loading an
    // account. This exception is limited to a current confirmation dialog.
    bool login_owner;
    // The accept action is destructive and uses the danger fill.
    bool danger;
    // Confirmations only: Enter accepts (initial focus on accept, not Cancel).
    bool accept_focused;
    // The dismiss button's label; empty keeps "Cancel".
    Utf8View cancel;
    // A checkbox row above the buttons (confirmations only), and the accept
    // handler that receives its state; it replaces `save`.
    Utf8View option;
    bool option_checked;
    TextFormSaveOption save_option;
    // Editors only: a radio group above the field and a dropdown below it;
    // save_choices replaces `save`. Choosing an option clears the text, and
    // the field shows only while the choice numbered choice_editor - 1 is chosen (0: always).
    Utf8View choices_label;
    ConstSlice<Utf8View> choices;
    u32 choice_editor;
    Utf8View select_label;
    ConstSlice<Utf8View> select_options;
    u32 select_selected;
    TextFormSaveChoices save_choices;
    // Editors only: a hint while the field is empty.
    Utf8View placeholder;
    // Editors only: a live check of the text. Accept with text the check
    // rejects restores the last accepted text and shows `rejected` as an
    // error instead of saving; with `swatch` the field previews the colour of
    // the last accepted text.
    TextFormCheck check;
    Utf8View rejected;
    bool swatch;
    // Confirmations only: read-only text under `label` (static and wrapping,
    // never editable or focused), up to kApplicationTextFormDetailBytes.
    Utf8View detail;
    // Confirmations only: Accept is the only button; Escape and a click
    // outside still dismiss the form.
    bool single_action;
};
// Check for colour editors: #RGB or #RRGGBB, with or without the '#'.
inline bool application_text_form_color_check(TextFormOwnerKey, Utf8View text, u32* swatch_rgba) noexcept {
    return color_hex_parse(text, false, swatch_rgba);
}
bool application_text_form_forum_child(const ApplicationForumPostTicket&) noexcept;
bool application_text_form_open(const ApplicationTextFormRequest& request) noexcept;
void application_text_form_close(bool restore_focus = true, bool redraw = true) noexcept;
void application_text_form_cancel_owner(TextFormOwnerValid validator) noexcept;
bool application_text_form_input(UiInputKind kind, u32 code, bool repeated) noexcept;
bool application_text_form_filter(const WindowEvent& event) noexcept;
bool application_text_form_window_event(const WindowEvent& event) noexcept;
bool application_text_form_synchronize() noexcept;
// Every committed font-bank change, including hidden owners and token wrap.
void application_text_form_theme_changed() noexcept;
void application_text_form_tick(u64 now_ms) noexcept;
u64 application_text_form_next_wake() noexcept;
} // namespace maicord::app_detail
