#pragma once
#include "forum_post.hpp"
#include "maicord/ui/core/input_router.hpp"
#include "maicord/render/text/forum_post_surface.hpp"
#include "maicord/platform/window/event.hpp"

namespace maicord::app_detail {
inline constexpr u32 kApplicationForumTitleBytes = 400, kApplicationForumTitleScalars = 100;
inline constexpr u32 kApplicationForumBodyBytes = 8000, kApplicationForumBodyScalars = 2000;
void application_forum_form_close(bool restore_focus = true, bool redraw = true) noexcept;
void application_forum_form_release() noexcept;
bool application_forum_form_active() noexcept;
bool application_forum_form_suspend_child(const ApplicationForumPostTicket&) noexcept;
void application_forum_form_resume_child(const ApplicationForumPostTicket&) noexcept;
ForumPostScene* application_forum_form_scene() noexcept;
bool application_forum_form_input(UiInputKind, u32 code, bool repeated) noexcept;
bool application_forum_form_filter(const WindowEvent&) noexcept;
bool application_forum_form_window_event(const WindowEvent&) noexcept;
bool application_forum_form_synchronize() noexcept;
void application_forum_form_theme_changed() noexcept;
void application_forum_form_tick(u64 now_ms) noexcept;
u64 application_forum_form_next_wake() noexcept;
struct ApplicationForumForm;
} // namespace maicord::app_detail
