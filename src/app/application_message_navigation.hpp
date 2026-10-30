#pragma once
#include "maicord/model/message_store.hpp"
#include "maicord/ui/message_markdown.hpp"
namespace maicord::app_detail {
struct AccountUiRuntime;
enum class ApplicationMessageNavigationStage : u8 {
    idle, waiting_channel, loading_channel, loading_parent, waiting_membership,
    opening_history, rate_limited, error, busy, cancelled, failure_parent,
    failure_membership, permission_denied, source_changed, message_missing
};
struct ApplicationMessageNavigationSource {
    u64 channel_id, message_id;
    MessageHandle handle;
    u32 revision;
    u64 account_scope, profile_id, self_user_id;
    const void* account_instance;
    const AccountUiRuntime* account_ui;
    u64 channel_epoch;
    u16 channel_generation;
};
static_assert(sizeof(ApplicationMessageNavigationSource) == 88);
// Capture only a currently readable loaded timeline message. The captured
// handle must be revalidated before the request is accepted, including after account changes.
bool application_message_navigation_source(u64 channel_id, u64 message_id,
    MessageHandle, u32 revision, ApplicationMessageNavigationSource*) noexcept;
// Internal message links are consumed even when denied, stale or already busy.
bool application_message_navigation_link(const ApplicationMessageNavigationSource&,
    const MessageLink&) noexcept;
bool application_message_navigation_active() noexcept;
bool application_message_navigation_observation_needed() noexcept;
bool application_message_navigation_cancel() noexcept;
void application_message_navigation_release() noexcept;
// Called for decoded Gateway CHANNEL_DELETE/THREAD_DELETE before/after Core
// deletion. A late metadata response cannot recreate a deleted destination.
void application_message_navigation_gateway_deleted(u64 guild_id, u64 channel_id) noexcept;
void application_message_navigation_message_deleted(u64 channel_id, u64 message_id) noexcept;
void application_message_navigation_gateway_reset() noexcept;
// The scheduler checks this before an around-history response may change
// the timeline; unrelated legacy jumps return true and keep their own guards.
bool application_message_navigation_history_current(u64 request_id, u64 channel_id,
    u64 message_id) noexcept;
} // namespace maicord::app_detail
