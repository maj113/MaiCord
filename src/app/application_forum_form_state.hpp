#pragma once
#include "application_forum_form.hpp"
#include "maicord/ui/secure_text_store.hpp"
#include "maicord/ui/button_keyboard.hpp"
#include "maicord/ui/forum_post_scene.hpp"
namespace maicord::app_detail {
struct ApplicationForumFormField {
    SecureEdit edit;
    SecureTextStore store;
    SecureTextComposition composition;
    ApplicationForumForm* owner;
    u32 index;
};
struct ApplicationForumForm {
    ForumPostScene scene;
    ForumPostSurface surface;
    ApplicationForumPostTicket ticket;
    u64 surface_serial;
    ApplicationForumFormField fields[2];
    ForumPostTagView tags[kForumPostTagCapacity];
    UiAttachmentDraftView attachments[kUiAttachmentDraftMaximum];
    char attachment_count[48];
    char attachment_labels[kUiAttachmentDraftMaximum][192];
    ForumPostTarget child_previous_focus;
    UiWidgetReference previous_focus;
    UiWidgetHandle pointer_button;
    UiButtonKeyboard keyboard;
    Utf8View local_status;
    u64 blink_at_ms;
    u32 blink_ms, owner_revision, metadata_revision, attachment_revision, preview_revision;
    bool previous_composer_focus, dragging, refreshing, caret_visible;
    bool editable, risk_acknowledged, uncertain;
    bool child_suspended, closing;
};
} // namespace maicord::app_detail
