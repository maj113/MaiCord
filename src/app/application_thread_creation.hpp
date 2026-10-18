#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/protocol/thread_creation.hpp"

namespace maicord::app_detail {
// The channel record a created thread's receipt describes; false when it conflicts with the stored channel
// `old` (null when unknown).
inline bool application_thread_creation_receipt_input(const ThreadCreationSnapshot& receipt,
                                                      const ChannelRecord* old, ChannelInput* output) noexcept {
    if (old && (old->parent_id != receipt.parent_id || old->guild_id != receipt.guild_id ||
                old->type != receipt.type))
        return false;
    *output = {receipt.id,
               receipt.guild_id,
               receipt.parent_id,
               thread_creation_name_view(receipt),
               old ? old->position : 0,
               old ? old->flags : 0,
               receipt.type};
    output->name_known = true;
    output->last_message_known = (receipt.fields & thread_creation_last_message_known) != 0;
    output->last_message_id = receipt.last_message_id;
    output->thread_archived_known = output->thread_locked_known = true;
    output->thread_archived = (receipt.fields & thread_creation_archived) != 0;
    output->thread_locked = (receipt.fields & thread_creation_locked) != 0;
    output->thread_active_known = true;
    output->thread_listed_active = !output->thread_archived;
    output->thread_self_joined_known = (receipt.fields & thread_creation_self_joined) != 0;
    output->thread_self_joined = output->thread_self_joined_known;
    if (output->thread_self_joined)
        output->thread_join_seconds = thread_join_seconds_from_ms(receipt.self_join_timestamp_ms);
    return true;
}
enum class ApplicationThreadCreationPhase : u8 {
    editing, queued, creating, reconcile_queued, reconciling, settled, failed, cancelled
};
// Stable intent identity. Phase/status revisions do not retire the name form.
struct ApplicationThreadCreationTicket {
    u64 serial, parent_id, source_message_id;
    u32 publication_revision;
};
struct ApplicationThreadCreationView {
    ApplicationThreadCreationTicket ticket;
    Utf8View parent_name, name, status;
    u32 revision;
    ApplicationThreadCreationPhase phase;
    bool available, error, can_submit, can_retry, can_cancel, uncertain;
};
bool application_thread_creation_available(u64 message_id) noexcept;
// begin prepares a source-owned intent; open additionally opens the native name form.
bool application_thread_creation_begin(u64 message_id, ApplicationThreadCreationTicket* = nullptr) noexcept;
bool application_thread_creation_open(u64 message_id) noexcept;
ApplicationThreadCreationView application_thread_creation_view() noexcept;
bool application_thread_creation_current(const ApplicationThreadCreationTicket&) noexcept;
bool application_thread_creation_name_submit(const ApplicationThreadCreationTicket&, Utf8View) noexcept;
bool application_thread_creation_retry(const ApplicationThreadCreationTicket&) noexcept;
bool application_thread_creation_cancel(const ApplicationThreadCreationTicket&) noexcept;
bool application_thread_creation_pending(u64 now_ms) noexcept;
bool application_thread_creation_submit(u64 request_id, u64 now_ms) noexcept;
bool application_thread_creation_response(const RestClientResponse&, Utf8View) noexcept;
bool application_thread_creation_synchronize() noexcept;
u64 application_thread_creation_next_wake() noexcept;
void application_thread_creation_gateway(u64 guild_id = 0, u64 parent_id = 0, u64 thread_id = 0) noexcept;
void application_thread_creation_release() noexcept;
struct ApplicationThreadCreationMetrics {
    u32 owner_bytes, owner_committed_bytes, publication_revision;
    bool flight_active;
};
ApplicationThreadCreationMetrics application_thread_creation_metrics() noexcept;
} // namespace maicord::app_detail
