#include "maicord/model/message_search.hpp"

namespace maicord {
namespace {

bool token_equal(const MessageSearchToken& left, const MessageSearchToken& right) noexcept {
    return left.account_id == right.account_id && left.scope_id == right.scope_id &&
           left.generation == right.generation && left.request_id == right.request_id;
}

void fail(MessageSearchController* controller, MessageSearchFailure failure) noexcept {
    controller->state = MessageSearchState::failed;
    controller->failure = failure;
    controller->retry_at_ms = 0;
}

} // namespace

bool message_search_begin(MessageSearchController* controller, u64 account_id,
                          const MessageSearchQuery& query) noexcept {
    if (!controller || !account_id || controller->token.generation == ~u64{0} ||
        !message_search_query_valid(query)) {
        return false;
    }
    const u64 generation = controller->token.generation + 1;
    *controller = {};
    controller->query = query;
    controller->token = {account_id, query.scope_id, generation, 0};
    controller->state = MessageSearchState::ready_to_submit;
    return true;
}

bool message_search_submit(MessageSearchController* controller, u64 request_id, u64 now_ms,
                           MessageSearchToken* token) noexcept {
    if (!controller || !token || !request_id || request_id == controller->token.request_id ||
        !controller->token.account_id ||
        (controller->state != MessageSearchState::ready_to_submit &&
         controller->state != MessageSearchState::indexing &&
         controller->state != MessageSearchState::rate_limited) ||
        (controller->retry_at_ms && now_ms < controller->retry_at_ms)) {
        return false;
    }
    controller->token.request_id = request_id;
    controller->state = MessageSearchState::fetching;
    controller->retry_at_ms = 0;
    controller->failure = MessageSearchFailure::none;
    *token = controller->token;
    return true;
}

bool message_search_accepts(const MessageSearchController* controller,
                            const MessageSearchToken& token) noexcept {
    return controller && controller->state == MessageSearchState::fetching && token.request_id &&
           token_equal(controller->token, token);
}

bool message_search_submission_failed(MessageSearchController* controller, const MessageSearchToken& token,
                                      bool busy) noexcept {
    if (!message_search_accepts(controller, token)) {
        return false;
    }
    if (busy) {
        controller->state = MessageSearchState::ready_to_submit;
        controller->retry_at_ms = 0;
    } else {
        fail(controller, MessageSearchFailure::admission);
    }
    return true;
}

bool message_search_complete(MessageSearchController* controller, const MessageSearchToken& token,
                             u16 http_status, bool transport_ok, bool decoded,
                             const MessageSearchPage* page, u64 now_ms, u64 retry_after_ms) noexcept {
    if (!message_search_accepts(controller, token)) {
        return false;
    }
    controller->http_status = http_status;
    // A missing or rejected tabs route retries once through the legacy GET.
    if (transport_ok && (http_status == 400 || http_status == 404 || http_status == 405) &&
        message_search_fallback(controller, token))
        return true;
    if (!transport_ok) {
        fail(controller, MessageSearchFailure::transport);
    } else if (http_status == 202 || http_status == 429) {
        if (controller->indexing_retries >= 5) {
            fail(controller, MessageSearchFailure::retry_limit);
            return true;
        }
        const u64 delay = retry_after_ms;
        // Neither index readiness nor rate limits may invent a server deadline.
        if (!delay || delay > ~u64{0} - now_ms) {
            fail(controller, MessageSearchFailure::deadline);
            return true;
        }
        ++controller->indexing_retries;
        controller->retry_at_ms = now_ms + delay;
        controller->state =
            http_status == 202 ? MessageSearchState::indexing : MessageSearchState::rate_limited;
    } else if (http_status != 200) {
        fail(controller, MessageSearchFailure::http);
    } else if (!decoded || !page || page->group_count > controller->query.limit ||
               page->message_count > kMessageSearchMaximumMessages) {
        fail(controller, MessageSearchFailure::decode);
    } else {
        controller->total_results = page->total_results;
        controller->loaded_groups = page->group_count;
        controller->has_results = true;
        controller->state = MessageSearchState::ready;
        controller->retry_at_ms = 0;
        controller->indexing_retries = 0;
    }
    return true;
}

bool message_search_page(MessageSearchController* controller, u32 offset) noexcept {
    if (!controller || controller->state != MessageSearchState::ready ||
        offset > kMessageSearchMaximumOffset || (offset && offset >= controller->total_results) ||
        controller->token.generation == ~u64{0}) {
        return false;
    }
    ++controller->token.generation;
    controller->token.request_id = 0;
    controller->query.offset = offset;
    controller->state = MessageSearchState::ready_to_submit;
    controller->failure = MessageSearchFailure::none;
    controller->http_status = 0;
    controller->indexing_retries = 0;
    return true;
}

bool message_search_fallback(MessageSearchController* controller,
                             const MessageSearchToken& token) noexcept {
    if (!message_search_accepts(controller, token) ||
        controller->query.endpoint != MessageSearchEndpoint::tabs ||
        controller->query.scope == MessageSearchScope::private_channels)
        return false;
    controller->query.endpoint = MessageSearchEndpoint::legacy;
    controller->query.tab_counts = false;
    controller->endpoint_fallback = true;
    controller->token.request_id = 0;
    controller->state = MessageSearchState::ready_to_submit;
    controller->retry_at_ms = 0;
    return true;
}

u64 message_search_cancel(MessageSearchController* controller) noexcept {
    if (!controller) {
        return 0;
    }
    const u64 request_id =
        controller->state == MessageSearchState::fetching ? controller->token.request_id : 0;
    controller->token.request_id = 0;
    controller->state = MessageSearchState::cancelled;
    controller->retry_at_ms = 0;
    return request_id;
}

u64 message_search_next_wake(const MessageSearchController* controller) noexcept {
    return controller && (controller->state == MessageSearchState::indexing ||
                          controller->state == MessageSearchState::rate_limited)
               ? controller->retry_at_ms
               : 0;
}

} // namespace maicord
