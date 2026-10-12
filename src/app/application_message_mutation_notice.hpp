#pragma once
#include "maicord/app/rest_client.hpp"
#include "application_message_navigation.hpp"

namespace maicord::app_detail {
enum class ApplicationMessageMutationKind : u8 {
    edit,
    erase,
    reaction,
    attachment_open,
    mark_unread,
    navigate_message
};
inline constexpr u32 kApplicationMessageMutationKinds = 6;
enum class ApplicationMessageMutationNoticePhase : u8 {
    empty,
    pending,
    failed
};
struct ApplicationMessageMutationScope {
    u64 account_serial, profile_id, self_user_id, channel_id, channel_generation, message_id;
};
struct ApplicationMessageMutationNotice {
    ApplicationMessageMutationScope requests[kApplicationMessageMutationKinds];
    ApplicationMessageMutationScope shown;
    ApiRejection rejection;
    ApplicationMessageMutationKind kind;
    ApplicationMessageMutationNoticePhase phase;
    u16 status_code;
    bool transport_failed;
    ApplicationMessageNavigationStage navigation_stage;
};
static_assert(sizeof(ApplicationMessageMutationNotice) == 352);
// UI-thread only. Capture the owning request when it is accepted, before any worker response.
void application_message_mutation_notice_begin(ApplicationMessageMutationKind kind, u64 channel_id,
                                               u64 message_id) noexcept;
void application_message_mutation_notice_pending(ApplicationMessageMutationKind kind, u64 channel_id,
                                                 u64 message_id) noexcept;
void application_message_mutation_notice_pending(ApplicationMessageMutationKind kind, u64 channel_id,
                                                 u64 message_id, u16 status_code) noexcept;
void application_message_mutation_notice_navigation_pending(u64 channel_id, u64 message_id,
                                                            ApplicationMessageNavigationStage stage,
                                                            u16 status_code = 0) noexcept;
void application_message_mutation_notice_navigation_failed(
    u64 channel_id, u64 message_id, ApplicationMessageNavigationStage stage,
    const RestClientResponse& response = {}) noexcept;
void application_message_mutation_notice_failed(ApplicationMessageMutationKind kind, u64 channel_id,
                                                u64 message_id,
                                                const RestClientResponse& response) noexcept;
void application_message_mutation_notice_succeeded(ApplicationMessageMutationKind kind, u64 channel_id,
                                                   u64 message_id) noexcept;
Utf8View application_message_mutation_notice_text() noexcept;
bool application_message_mutation_notice_is_error() noexcept;
void application_message_mutation_notice_clear(ApplicationMessageMutationKind kind, u64 channel_id = 0,
                                               u64 message_id = 0) noexcept;
} // namespace maicord::app_detail
