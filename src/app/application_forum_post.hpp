#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/core/utf.hpp"

namespace maicord { struct GatewayMessageEvent; }

namespace maicord::app_detail {
enum class ApplicationForumPostPhase : u8 {
    metadata_queued,
    metadata_loading,
    editing,
    queued,
    creating,
    settled,
    failed,
    cancelled
};
struct ApplicationForumPostTicket {
    u64 serial, parent_id;
    u32 publication_revision;
    friend bool operator==(const ApplicationForumPostTicket&, const ApplicationForumPostTicket&) = default;
};
struct ApplicationForumPostTag {
    u64 id;
    Utf8View name;
    bool selected, enabled, moderated;
};
struct ApplicationForumPostView {
    ApplicationForumPostTicket ticket;
    Utf8View parent_name, title, body, status;
    ConstSlice<ApplicationForumPostTag> tags;
    ConstSlice<u64> selected_tags;
    u32 revision, metadata_revision;
    ApplicationForumPostPhase phase;
    bool available, error, can_edit, can_submit, can_retry, can_cancel, uncertain, require_tag;
};
bool application_forum_post_available(u64 parent_id = 0) noexcept;
bool application_forum_post_begin(u64 parent_id, ApplicationForumPostTicket* = nullptr) noexcept;
bool application_forum_post_open(u64 parent_id = 0) noexcept;
ApplicationForumPostView application_forum_post_view() noexcept;
bool application_forum_post_current(const ApplicationForumPostTicket&) noexcept;
bool application_forum_post_editable(const ApplicationForumPostTicket&) noexcept;
bool application_forum_post_cancel_upload(const ApplicationForumPostTicket&, u64 draft_id) noexcept;
void application_forum_post_attachment_changed(const ApplicationForumPostTicket&) noexcept;
// Empty drafts are valid while editing; submit checks title/body and tag policy.
bool application_forum_post_draft_update(const ApplicationForumPostTicket&, Utf8View title,
                                         Utf8View body) noexcept;
bool application_forum_post_tag_toggle(const ApplicationForumPostTicket&, u64 tag_id) noexcept;
bool application_forum_post_draft_submit(const ApplicationForumPostTicket&) noexcept;
// An ambiguous POST has no deterministic ID. Only an explicit duplicate-risk
// acknowledgment allows a fresh intent after the user inspects existing posts.
bool application_forum_post_retry(const ApplicationForumPostTicket&,
                                  bool acknowledge_duplicate_risk = false) noexcept;
bool application_forum_post_inspect(const ApplicationForumPostTicket&) noexcept;
bool application_forum_post_cancel(const ApplicationForumPostTicket&) noexcept;
bool application_forum_post_pending(u64 now_ms) noexcept;
bool application_forum_post_submit(u64 request_id, u64 now_ms) noexcept;
bool application_forum_post_response(const RestClientResponse&, Utf8View) noexcept;
bool application_forum_post_synchronize() noexcept;
u64 application_forum_post_next_wake() noexcept;
void application_forum_post_gateway(u64 guild_id = 0, u64 parent_id = 0, u64 thread_id = 0) noexcept;
bool application_forum_post_observation_needed() noexcept;
void application_forum_post_message_observe(const GatewayMessageEvent&) noexcept;
void application_forum_post_message_deleted(u64 channel_id, u64 message_id) noexcept;
void application_forum_post_release() noexcept;
struct ApplicationForumPostMetrics {
    u32 owner_bytes, owner_committed_bytes, publication_revision;
    bool flight_active;
};
ApplicationForumPostMetrics application_forum_post_metrics() noexcept;
// Native two-field composer boundary, implemented by application_forum_form.
bool application_forum_form_open(const ApplicationForumPostTicket&) noexcept;
void application_forum_form_close(const ApplicationForumPostTicket&) noexcept;
} // namespace maicord::app_detail
