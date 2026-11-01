#pragma once

#include "../../../../src/app/chat/search/search_internal.hpp"
#include "application_search_measurement.hpp"

namespace maicord::search_runtime_test {
using namespace app_detail;

struct AccountFixture {
    MessageSearchQuery query;
    u64 request_id;
    u64 cancelled_id;
    u32 submissions;
    u32 cancellations;
    u32 acknowledgements;
    RestClientSubmitError admission;
};
extern AccountFixture account;

extern u32 repaints;

extern u32 forced_account_repaints;

extern u32 overlay_repaints;

extern u32 navigations;

extern u32 search_saves;

extern u32 content_confirmations;

extern MessageMediaPolicyResult media_policy;

extern u32 content_retirements;

extern u32 navigation_releases;

extern ApplicationSearchSourceTicket saved_search_ticket;

extern u64 saved_search_attachment;

extern bool saved_search_snapshot;

extern u32 hit_position;

extern u32 viewer_opens;

extern u32 viewer_index;

extern UiRect viewer_origin;

extern u64 navigated_channel;

extern u64 navigated_message;

extern bool settings;

extern bool downloads;

extern bool composing;

extern u32 member_queries;

extern u32 member_query_withdrawals;

extern bool member_query_active;

extern bool composer_query_owned;

extern u64 member_query_guild;

extern char member_query[32];

extern u32 member_query_bytes;

extern RelationshipRecord choice_relationship_records[1];

extern RelationshipStore choice_relationships;

extern GuildMemberRecord choice_member_records[4];

extern u32 choice_member_order[4];

extern PrivateChannelStore choice_private_store;

extern PrivateChannelRecord choice_private_records[1];

extern u64 choice_private_recipients[2];

extern MessageRecord records[2];

extern u32 poll_projection_calls;

extern u64 projected_poll_id;

extern u32 projected_poll_publication;

void check(bool passed, const char* label) noexcept;

usize committed(const void* pointer) noexcept;

bool modifiers(bool control = false, bool shift = false, bool alt = false) noexcept;

bool open(bool shift = false) noexcept;

bool key(u32 value) noexcept;

bool type(Utf8View text) noexcept;

bool query(Utf8View text) noexcept;

LPARAM point(UiRect bounds) noexcept;

UiRect action_bounds(UiSearchAction action) noexcept;

extern bool browser_open, browser_forum;

extern u64 browser_session;

extern u32 browser_searches, browser_closes;

extern char browser_query[128];

extern u32 browser_query_bytes;

bool click(UiSearchAction action, UINT press = WM_LBUTTONDOWN) noexcept;

bool response(u64 request_id, u16 status, Utf8View body = {}, u64 retry_at = 0,
              u64 retry_after_ms = 0) noexcept;

constexpr auto result_body = utf8_literal(
    R"({"total_results":41,"messages":[[{"id":"300","channel_id":"70","author":{"id":"3","username":"Original fixture author"},"content":"First original fixture hit","timestamp":"2026-01-01T00:00:00Z","hit":true},{"id":"299","channel_id":"70","author":{"id":"4","username":"Context fixture author"},"content":"Original fixture context","timestamp":"2026-01-01T00:00:00Z","hit":false}],[{"id":"280","channel_id":"70","author":{"id":"3","username":"Original fixture author"},"content":"Second original fixture hit","timestamp":"2026-01-01T00:00:00Z","hit":true}]]})");

usize bank_committed(const app_detail::SearchBank* b) noexcept;

void rich_bank_capacity() noexcept;

void private_privacy() noexcept;

void rich_publication() noexcept;

void poll_publication() noexcept;

void filter_choices_runtime() noexcept;

void tabs_and_recent() noexcept;

void thread_scope_runtime() noexcept;

void indexing_retry_runtime() noexcept;

void rich_scroll_runtime() noexcept;

} // namespace maicord::search_runtime_test
