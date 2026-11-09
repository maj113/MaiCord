#include "maicord/app/settings/account_settings_rest.hpp"
#include "maicord/protocol/user/settings_proto.hpp"
#include "maicord/core/memory.hpp"

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
    if (!written.ok())
        return result;
    // A verified action carries the token /mfa/finish returned (Userdoccers: authentication#mfa-verification).
    u8 headers[kAccountSettingsMfaTokenBytes + 64];
    u32 header_bytes = 0;
    if (request.mfa.bytes) {
        header_bytes = rest_append_extra_header({headers, sizeof(headers)}, 0, utf8_literal("X-Discord-MFA-Authorization"),
                                                {reinterpret_cast<const u8*>(request.mfa.data), request.mfa.bytes});
        if (header_bytes == ~0u)
            return result;
    }
    RestClientRequest submit{{target, written.target_bytes}, {body.data, written.body_bytes},
                             {written.route_id, 0, written.method, RestMajorParameter::none},
                             request_id, kRestClientDefaultTimeoutMs, written.body_bytes != 0};
    submit.extra_headers = {headers, header_bytes};
    result = rest_client_submit(rest, submit);
    secure_clear_bytes(headers, header_bytes);
    return result;
}
} // namespace maicord
