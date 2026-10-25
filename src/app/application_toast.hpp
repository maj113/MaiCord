#pragma once

#include "application_state.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/ui/clipboard.hpp"
#include "maicord/ui/toast.hpp"

namespace maicord::app_detail {

// Confirmations for user-initiated copies and saves. Text edits copying a
// selection (Ctrl+C) stay silent.
inline constexpr Utf8View kToastLinkCopied = utf8_literal("Link Copied!");
inline constexpr Utf8View kToastLinkCopyFailed = utf8_literal("Unable to copy link");
inline constexpr Utf8View kToastInviteCopied = utf8_literal("Invite link copied");
inline constexpr Utf8View kToastImageCopied = utf8_literal("Image Copied!");
inline constexpr Utf8View kToastImageCopyFailed = utf8_literal("Unable to copy image");
inline constexpr Utf8View kToastIdCopied = utf8_literal("Copied ID");
inline constexpr Utf8View kToastMessageIdCopied = utf8_literal("Copied Message ID");
inline constexpr Utf8View kToastIdCopyFailed = utf8_literal("Unable to copy ID");
inline constexpr Utf8View kToastTextCopied = utf8_literal("Copied Message Text");
inline constexpr Utf8View kToastUsernameCopied = utf8_literal("Username Copied");
inline constexpr Utf8View kToastCopyFailed = utf8_literal("Unable to copy to clipboard");
inline constexpr Utf8View kToastImageSaved = utf8_literal("Image Saved");
inline constexpr Utf8View kToastImageSaveFailed = utf8_literal("Unable to save image");
inline constexpr Utf8View kToastAttachmentSaved = utf8_literal("Attachment Saved");
inline constexpr Utf8View kToastAttachmentSaveFailed = utf8_literal("Unable to save attachment");

// Queues a toast; the next frame shows it (application_toast_synchronize).
inline void application_toast_show(UiToastKind kind, Utf8View text) noexcept {
    if (ui_toast_push(&g_app.toasts, kind, text))
        application_request_frame();
}
inline void application_toast_result(bool succeeded, Utf8View success, Utf8View failure) noexcept {
    application_toast_show(succeeded ? UiToastKind::success : UiToastKind::failure,
                           succeeded ? success : failure);
}
// Writes text to the clipboard for a user action and confirms the result.
// Empty text is a failure. Returns whether the clipboard took it.
inline bool application_copy_text(Utf8View text, Utf8View copied, Utf8View failed) noexcept {
    const bool ok = text.bytes && clipboard_write_utf8(g_app.window, text);
    application_toast_result(ok, copied, failed);
    return ok;
}

// Frame step: advances the queue, publishes the scene and its opacity to the
// renderer and requests frames only while a toast animates.
bool application_toast_synchronize(u64 now_ms) noexcept;
// The next due update; zero when no toast is queued or showing.
u64 application_toast_next_wake() noexcept;
// Loop tick: the hold and the handover end without a frame of their own.
inline void application_toast_tick(u64 now_ms) noexcept {
    if (deadline_due(now_ms, application_toast_next_wake()))
        application_request_frame();
}
void application_toast_release() noexcept;

} // namespace maicord::app_detail
