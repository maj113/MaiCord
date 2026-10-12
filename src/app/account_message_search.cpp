#include "maicord/app/account_message_search.hpp"

namespace maicord {

RestClientSubmitResult account_message_search(RestClient* rest,
                                              AccountMessageSearchOperation operation,
                                              const MessageSearchQuery* query,
                                              u64 request_id) noexcept {
    if (!rest || !request_id) {
        return {RestClientSubmitError::invalid_input};
    }
    if (operation == AccountMessageSearchOperation::cancel) {
        if (query) {
            return {RestClientSubmitError::invalid_input};
        }
        return rest_client_cancel_result(rest, request_id);
    }
    if (operation != AccountMessageSearchOperation::submit || !query) {
        return {RestClientSubmitError::invalid_input};
    }
    u8 target[kMessageSearchMaximumTargetBytes]{};
    if (query->endpoint == MessageSearchEndpoint::tabs) {
        u8 body[kMessageSearchMaximumBodyBytes]{};
        const auto request = write_message_search_tabs_request(*query, {target, sizeof(target)},
                                                               {body, sizeof(body)});
        if (!request.ok())
            return {request.error == MessageSearchWriteError::output_capacity
                        ? RestClientSubmitError::target_capacity
                        : RestClientSubmitError::invalid_input};
        const auto scope = query->scope;
        return rest_client_submit(
            rest, {target, request.target_bytes},
            {body, request.body_bytes},
            {scope == MessageSearchScope::guild     ? kGuildMessageSearchTabsRouteId
             : scope == MessageSearchScope::channel ? kChannelMessageSearchTabsRouteId
                                                           : kPrivateMessageSearchTabsRouteId,
             query->scope_id, RestMethod::post,
             scope == MessageSearchScope::guild     ? RestMajorParameter::guild
             : scope == MessageSearchScope::channel ? RestMajorParameter::channel
                                                           : RestMajorParameter::none},
            request_id,
            true);
    }
    const auto written = write_message_search_target(*query, {target, sizeof(target)});
    if (!written.ok()) {
        return {written.error == MessageSearchWriteError::output_capacity
                    ? RestClientSubmitError::target_capacity
                    : RestClientSubmitError::invalid_input};
    }
    const bool guild = query->scope == MessageSearchScope::guild;
    return rest_client_submit(
        rest, {target, written.bytes},
        {},
        {guild ? kGuildMessageSearchRouteId : kChannelMessageSearchRouteId,
         query->scope_id, RestMethod::get,
         guild ? RestMajorParameter::guild : RestMajorParameter::channel},
        request_id,
        false);
}

} // namespace maicord
