#include "maicord/app/account_stage.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
static_assert(kStageMaximumTargetBytes <= kRestClientMaximumTargetBytes);
static_assert(kStageMaximumBodyBytes <= kRestClientMaximumRequestBodyBytes);

RestClientSubmitResult account_stage_submit(RestClient* rest, const StageRequest* request,
                                            u64 request_id) noexcept {
    if (!rest || !request || !request_id || !stage_request_valid(*request))
        return {RestClientSubmitError::invalid_input};
    u8 target[kStageMaximumTargetBytes];
    const auto path = stage_write_target(*request, {target, sizeof(target)});
    if (!path.ok())
        return {RestClientSubmitError::target_capacity};
    u8 body[kStageMaximumBodyBytes];
    StageWriteResult written{};
    if (stage_has_body(request->operation)) {
        written = stage_write_body(*request, {body, sizeof(body)});
        if (!written.ok())
            return {RestClientSubmitError::body_capacity};
    }
    const auto result = rest_client_submit(rest, {target, path.bytes},
                                           written.bytes ? ConstSlice<u8>{body, written.bytes}
                                                                 : ConstSlice<u8>{},
                                           stage_route(*request),
                                           request_id,
                                           written.bytes != 0);
    secure_clear_bytes(body, written.bytes);
    return result;
}

RestClientSubmitResult account_stage_cancel(RestClient* rest, u64 request_id) noexcept {
    return rest_client_cancel_result(rest, request_id);
}
} // namespace maicord
