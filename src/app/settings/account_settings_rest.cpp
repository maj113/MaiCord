#include "maicord/app/settings/account_settings_rest.hpp"
#include "maicord/protocol/user/settings_proto.hpp"

#include "maicord/app/net/request_body_bank.hpp"

namespace maicord {
RestClientSubmitResult account_settings_submit(RestClient* rest, const AccountSettingsRequest& request,
                                               u64 request_id) noexcept {
    if (!rest || !request_id)
        return {RestClientSubmitError::invalid_input};
    const bool settings = request.operation == AccountSettingsOperation::settings_patch;
    const u32 body_bytes = settings ? kUserSettingsPatchBytes : kAccountSettingsMaximumBodyBytes;
    // Settings bodies exceed a comfortable stack frame.
    RequestBodyBank body{body_bytes};
    if (!body.data)
        return {RestClientSubmitError::busy};
    body.used = body_bytes;
    u8 target[kAccountSettingsMaximumTargetBytes];
    const auto written = account_settings_write_request(request, {target, sizeof(target)}, body.all());
    RestClientSubmitResult result{written.error == AccountSettingsWriteError::output_capacity
                                             ? RestClientSubmitError::target_capacity
                                             : RestClientSubmitError::invalid_input};
    if (written.ok()) {
        result = rest_client_submit(rest, {target, written.target_bytes},
                                    {body.data, written.body_bytes},
                                    {written.route_id, 0, written.method,
                                     RestMajorParameter::none},
                                    request_id,
                                    written.body_bytes != 0);
    }
    return result;
}
} // namespace maicord
