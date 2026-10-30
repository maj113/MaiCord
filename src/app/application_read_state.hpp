#pragma once
#include "maicord/core/base.hpp"
#include "maicord/app/read_scope.hpp"
namespace maicord {
struct RestClientResponse;
struct GatewayClientEvent;
namespace app_detail {
struct Application;
struct ApplicationReadState;
bool application_read_state_eligible() noexcept;
void application_read_state_activity() noexcept;
bool application_read_state_mark(u64 channel_id, bool immediate) noexcept;
bool application_read_state_message_unread_available(u64 message_id) noexcept;
bool application_read_state_channel_unread_available(u64 channel_id) noexcept;
bool application_read_state_mark_message_unread(u64 message_id) noexcept;
bool application_read_state_mark_channel_unread(u64 channel_id) noexcept;
bool application_read_state_channel_unread_retry_available(u64 channel_id) noexcept;
bool application_read_state_retry_channel_unread(u64 channel_id) noexcept;
bool application_read_state_observe(u64 now_ms) noexcept;
bool application_read_state_submit(u64 request_id, u64 now_ms) noexcept;
bool application_read_state_response(const RestClientResponse& response, const u8* body, u32 bytes,
                                     u64 now_ms) noexcept;
u64 application_read_state_next_wake() noexcept;
void application_read_state_cancel(void*, u64 channel_id) noexcept;
void application_read_state_ack_observed(void*, u64 channel_id, u64 cursor, bool manual) noexcept;
void application_read_state_reset_token(void*) noexcept;
void application_read_state_ready() noexcept;
void application_read_state_release(Application& application) noexcept;
// Shared per-channel authority for scoped read plans. Preparation backs every
// ledger/queue/echo prefix before commit can change any logical/local state.
bool application_read_state_scoped_prepare(ConstSlice<ReadScopeTarget>, bool individual,
                                           u64* first_serial) noexcept;
bool application_read_state_scoped_commit(ConstSlice<ReadScopeTarget>, bool individual,
                                          u64 first_serial) noexcept;
bool application_read_state_action_current(u64 channel_id, u64 action_serial) noexcept;
void application_read_state_scoped_cancel(u64 channel_id, u64 action_serial) noexcept;
struct ApplicationReadEchoProof {
    u64 channel_id, cursor, action_serial;
};
bool application_read_state_echo_prepare(ConstSlice<ApplicationReadEchoProof>) noexcept;
void application_read_state_echo_admitted(ConstSlice<ApplicationReadEchoProof>, u64 request_id) noexcept;
void application_read_state_echo_rejected(u64 request_id) noexcept;
} // namespace app_detail
} // namespace maicord
