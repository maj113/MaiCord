#pragma once
#include "maicord/model/member_moderation.hpp"
#include "maicord/ui/popup_scene.hpp"

namespace maicord {

// Popup action identities owned by member moderation; choices are indexed.
struct MemberModerationMenuActions {
    u32 nickname, timeout, end_timeout, kick, ban, choice_first;
};

// Appends the moderation section (nothing when no action is visible).
// Visible-but-gated actions are listed disabled with the blocking reason.
u32 member_context_moderation_build(const ModerationCapabilities&, const MemberModerationMenuActions&,
                                    Slice<PopupItem>) noexcept;
// Timeout length choices, in kModerationTimeoutChoices order.
u32 member_context_timeout_choices(const MemberModerationMenuActions&, Slice<PopupItem>) noexcept;
// Message history deletion windows, in kModerationBanDeleteChoices order.
u32 member_context_ban_choices(const MemberModerationMenuActions&, Slice<PopupItem>) noexcept;
Utf8View member_context_block_text(ModerationBlock) noexcept;

} // namespace maicord
