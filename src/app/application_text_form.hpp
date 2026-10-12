#pragma once
#include "maicord/ui/secure_edit.hpp"
#include "maicord/ui/input_router.hpp"
#include "application_forum_post.hpp"

namespace maicord::app_detail {
struct TextFormOwnerKey {
    u64 scope, collection, item;
};
using TextFormOwnerValid = bool (*)(TextFormOwnerKey key) noexcept;
using TextFormSave = bool (*)(TextFormOwnerKey key, Utf8View text) noexcept;
// Confirmations with an option row receive its state instead of text.
using TextFormSaveOption = bool (*)(TextFormOwnerKey key, bool option_checked) noexcept;
struct ApplicationTextFormProgress {
    Utf8View status, accept;
    u64 revision;
    bool editable = true, can_accept = true, complete = false;
    bool can_cancel = true, dismissible = true;
};
using TextFormProgress = ApplicationTextFormProgress (*)(TextFormOwnerKey key) noexcept;
using TextFormClosed = void (*)(TextFormOwnerKey key) noexcept;
struct ApplicationTextFormRequest {
    Utf8View title, context, label, accept, initial;
    TextFormOwnerKey owner;
    TextFormOwnerValid valid;
    TextFormSave save;
    u32 maximum_codepoints;
    bool confirmation_only = false;
    // Optional asynchronous controller. Polls are borrowed views only; closing
    // cancels this form's controller while accepted transport responses finish.
    u32 maximum_bytes = 0;
    bool single_line = false;
    TextFormProgress progress = nullptr;
    TextFormClosed closed = nullptr;
    // A child may cover only this live post intent. All other forms keep
    // the ordinary modal rule.
    ApplicationForumPostTicket forum_parent{};
    // Startup recovery may cover login without allocating or loading an
    // account. This exception is limited to a current confirmation dialog.
    bool login_owner = false;
    // The accept action is destructive and uses the danger fill.
    bool danger = false;
    // Confirmations only: Enter accepts (initial focus on accept, not Cancel).
    bool accept_focused = false;
    // The dismiss button's label; empty keeps "Cancel".
    Utf8View cancel{};
    // A checkbox row above the buttons (confirmations only), and the accept
    // handler that receives its state; it replaces `save`.
    Utf8View option{};
    bool option_checked = false;
    TextFormSaveOption save_option = nullptr;
};
bool application_text_form_forum_child(const ApplicationForumPostTicket&) noexcept;
bool application_text_form_open(const ApplicationTextFormRequest& request) noexcept;
void application_text_form_close(bool restore_focus = true, bool redraw = true) noexcept;
void application_text_form_cancel_owner(TextFormOwnerValid validator) noexcept;
bool application_text_form_input(UiInputKind kind, u32 code, bool repeated) noexcept;
bool application_text_form_filter_message(const MSG& message) noexcept;
bool application_text_form_window_message(UINT message, WPARAM wparam, LPARAM lparam,
                                          LRESULT* result) noexcept;
bool application_text_form_synchronize() noexcept;
// Every committed font-bank change, including hidden owners and token wrap.
void application_text_form_theme_changed() noexcept;
void application_text_form_tick(u64 now_ms) noexcept;
u64 application_text_form_next_wake() noexcept;
} // namespace maicord::app_detail
