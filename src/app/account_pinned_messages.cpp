#include "maicord/app/account_pinned_messages.hpp"

namespace maicord {

RestClientSubmitResult account_pinned_messages_submit(
    RestClient* rest, const MessagePinsRequest* request, u64 request_id) noexcept {
    if (!rest || !request || !request_id)
        return {RestClientSubmitError::invalid_input};
    const bool fetch = request->operation == MessagePinsOperation::fetch;
    const bool pin = request->operation == MessagePinsOperation::pin;
    const bool unpin = request->operation == MessagePinsOperation::unpin;
    if ((!fetch && !pin && !unpin) || (fetch && request->message_id) ||
        (!fetch && (!request->message_id || request->query.before.bytes ||
                    (request->query.limit && request->query.limit != kMessagePinsMaximumItems))))
        return {RestClientSubmitError::invalid_input};
    u8 target[kMessagePinsMaximumTargetBytes]{};
    const auto written = fetch
        ? write_message_pins_target(request->query, {target, sizeof(target)})
        : write_message_pin_target(request->query.channel_id, request->message_id,
                                   {target, sizeof(target)});
    if (!written.ok())
        return {written.error == MessagePinsWriteError::output_capacity
                    ? RestClientSubmitError::target_capacity
                    : RestClientSubmitError::invalid_input};
    return rest_client_submit(
        rest, {target, written.bytes}, {},
        {fetch ? kMessagePinsRouteId : kMessagePinRouteId,
         request->query.channel_id,
         fetch ? RestMethod::get : pin ? RestMethod::put : RestMethod::delete_method,
         RestMajorParameter::channel},
        request_id, false);
}

RestClientSubmitResult account_pinned_messages_cancel(
    RestClient* rest, u64 request_id) noexcept {
    return rest_client_cancel_result(rest, request_id);
}

} // namespace maicord
