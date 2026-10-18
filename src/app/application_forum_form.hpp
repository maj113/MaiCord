#pragma once
#include "application_forum_post.hpp"
#include "maicord/ui/input_router.hpp"
#include "maicord/ui/secure_edit.hpp"
#include "maicord/render/forum_post_surface.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord::app_detail {
inline constexpr u32 kApplicationForumTitleBytes = 400, kApplicationForumTitleScalars = 100;
inline constexpr u32 kApplicationForumBodyBytes = 8000, kApplicationForumBodyScalars = 2000;
enum class ApplicationForumField : u8 {
    title,
    body
};
bool application_forum_form_open(const ApplicationForumPostTicket&) noexcept;
// Owner dismissal detaches without cancelling a different or completed intent.
void application_forum_form_close(const ApplicationForumPostTicket&) noexcept;
void application_forum_form_close(bool restore_focus = true, bool redraw = true) noexcept;
void application_forum_form_release() noexcept;
bool application_forum_form_active() noexcept;
bool application_forum_form_suspend_child(const ApplicationForumPostTicket&) noexcept;
void application_forum_form_resume_child(const ApplicationForumPostTicket&) noexcept;
ForumPostScene* application_forum_form_scene() noexcept;
ForumPostSurface* application_forum_form_surface() noexcept;
SecureEdit* application_forum_form_edit(ApplicationForumField) noexcept;
bool application_forum_form_input(UiInputKind, u32 code, bool repeated) noexcept;
bool application_forum_form_filter_message(const MSG&) noexcept;
bool application_forum_form_window_message(UINT, WPARAM, LPARAM, LRESULT*) noexcept;
bool application_forum_form_synchronize() noexcept;
void application_forum_form_theme_changed() noexcept;
void application_forum_form_tick(u64 now_ms) noexcept;
u64 application_forum_form_next_wake() noexcept;
// Allocation metrics for the lazy form owner; child edit/history/TSF/layout owners report separately.
struct ApplicationForumFormMetrics {
    u32 owner_bytes, committed_bytes;
    u64 serial;
    u32 revision;
};
ApplicationForumFormMetrics application_forum_form_metrics() noexcept;
struct ApplicationForumForm;
ApplicationForumForm* application_forum_form_state() noexcept;
} // namespace maicord::app_detail
