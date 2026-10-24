#include "maicord/app/account_settings_rest.hpp"
#include "maicord/protocol/user_settings_proto.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
RestClientSubmitResult account_settings_submit(RestClient* rest, const AccountSettingsRequest& request,
                                               u64 request_id) noexcept {
    if (!rest || !request_id || !account_settings_request_valid(request))
        return {RestClientSubmitError::invalid_input};
    const bool settings = request.operation == AccountSettingsOperation::settings_patch;
    const u32 body_bytes = settings ? kUserSettingsPatchBytes : kAccountSettingsMaximumBodyBytes;
    // Settings bodies exceed a comfortable stack frame; the bank is erased.
    u8* body = static_cast<u8*>(VirtualAlloc(nullptr, body_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!body)
        return {RestClientSubmitError::busy};
    u8 target[kAccountSettingsMaximumTargetBytes];
    const auto written = account_settings_write_request(request, {target, sizeof(target)}, {body, body_bytes});
    RestClientSubmitResult result{written.error == AccountSettingsWriteError::output_capacity
                                             ? RestClientSubmitError::target_capacity
                                             : RestClientSubmitError::invalid_input};
    if (written.ok()) {
        result = rest_client_submit(rest, {target, written.target_bytes},
                                    {body, written.body_bytes},
                                    {written.route_id, 0, written.method,
                                     RestMajorParameter::none},
                                    request_id,
                                    written.body_bytes != 0);
    }
    secure_clear_bytes(body, body_bytes);
    VirtualFree(body, 0, MEM_RELEASE);
    return result;
}
} // namespace maicord
