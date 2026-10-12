#include "maicord/app/account_notification_settings.hpp"

namespace maicord {
RestClientSubmitResult account_guild_settings_submit(RestClient* rest, const UserGuildSettingsPatch* patch,
                                                     u64 request_id) noexcept {
    if (!rest || !patch || !request_id)
        return {RestClientSubmitError::invalid_input};
    u8 target[kUserGuildSettingsMaximumTargetBytes]{};
    u8 body[kUserGuildSettingsMaximumBodyBytes]{};
    const auto written = user_guild_settings_write(*patch, {target, sizeof(target)}, {body, sizeof(body)});
    if (!written.ok)
        return {RestClientSubmitError::invalid_input};
    return rest_client_submit(rest, {target, written.target_bytes},
                              {body, written.body_bytes},
                              {kUserGuildSettingsRouteId, 0, RestMethod::patch,
                               RestMajorParameter::none},
                              request_id,
                              true);
}
RestClientSubmitResult account_mentions_submit(RestClient* rest, const MentionsQuery* query,
                                               u64 request_id) noexcept {
    if (!rest || !query || !request_id)
        return {RestClientSubmitError::invalid_input};
    u8 target[kMentionsMaximumTargetBytes]{};
    const u32 bytes = write_mentions_target(*query, {target, sizeof(target)});
    if (!bytes)
        return {RestClientSubmitError::invalid_input};
    return rest_client_submit(rest, {target, bytes},
                              {},
                              {kMentionsRouteId, 0, RestMethod::get,
                               RestMajorParameter::none},
                              request_id,
                              false);
}
RestClientSubmitResult account_notification_request_cancel(RestClient* rest, u64 request_id) noexcept {
    return rest_client_cancel_result(rest, request_id);
}
} // namespace maicord
