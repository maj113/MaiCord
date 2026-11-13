#pragma once
#include "chat/threads/forum_form_state.hpp"

namespace maicord::app_detail {
// The open forum form (null when none). The product reaches its scene through application_forum_form_scene(); the
// scene is the form's first member, so the scene's address is the form's.
static_assert(__builtin_offsetof(ApplicationForumForm, scene) == 0);
inline ApplicationForumForm* application_forum_form_state() noexcept {
    return reinterpret_cast<ApplicationForumForm*>(application_forum_form_scene());
}
} // namespace maicord::app_detail
