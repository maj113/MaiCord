#include "maicord/app/account_channel_overview.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
RestClientSubmitResult account_channel_overview_submit(RestClient* rest,
                                                       ChannelOverviewOperation operation,
                                                       u64 channel_id,
                                                       const ChannelOverviewPatch* patch,
                                                       u64 request_id) noexcept {
    if (!rest || !request_id ||
        (operation != ChannelOverviewOperation::fetch && operation != ChannelOverviewOperation::update) ||
        (operation == ChannelOverviewOperation::fetch && patch) ||
        (operation == ChannelOverviewOperation::update && !patch))
        return {RestClientSubmitError::invalid_input};
    u8 target[64]{}, body[kChannelOverviewMaximumBodyBytes]{};
    const auto path = channel_overview_write_target(channel_id, {target, sizeof(target)});
    if (!path.ok())
        return {RestClientSubmitError::invalid_input};
    ChannelOverviewResult written{};
    if (patch)
        written = channel_overview_write_patch(*patch, {body, sizeof(body)});
    if (!written.ok())
        return {RestClientSubmitError::invalid_input};
    const auto submitted = rest_client_submit(
        rest,
        {target, path.bytes},
        {body, written.bytes},
        {kChannelOverviewRouteId, channel_id,
         operation == ChannelOverviewOperation::fetch ? RestMethod::get : RestMethod::patch,
         RestMajorParameter::channel},
        request_id,
        patch != nullptr);
    secure_clear_bytes(body, sizeof(body));
    return submitted;
}
} // namespace maicord
