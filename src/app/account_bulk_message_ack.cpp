#include "maicord/app/account_bulk_message_ack.hpp"

namespace maicord {
static_assert(kBulkMessageAckMaximumBodyBytes <= kRestClientMaximumRequestBodyBytes);
namespace {
__declspec(noinline) RestClientSubmitResult submit_batch(
    RestClient* primary, ConstSlice<BulkMessageAckEntry> entries, u64 request_id) noexcept {
    u8 body[kBulkMessageAckMaximumBodyBytes];
    const auto written = write_bulk_message_ack_body(entries, {body, sizeof(body)});
    if (!written.ok())
        return {RestClientSubmitError::invalid_input};
    constexpr char target[] = "/api/v10/read-states/ack-bulk";
    return rest_client_submit(
        primary, {reinterpret_cast<const u8*>(target), sizeof(target) - 1},
        {body, written.bytes},
        {kBulkMessageAckRouteId, 0, RestMethod::post, RestMajorParameter::none},
        request_id,
        true);
}
} // namespace
RestClientSubmitResult account_bulk_message_ack_submit(RestClient* primary,
                                                       const BulkMessageAckEntry* entries,
                                                       u32 count, u64 request_id) noexcept {
    if (!primary || !request_id || (!entries && count) || (entries && !count) ||
        count > kBulkMessageAckMaximumEntries)
        return {RestClientSubmitError::invalid_input};
    if (!entries)
        return rest_client_cancel_result(primary, request_id);
    return submit_batch(primary, {entries, count}, request_id);
}
} // namespace maicord
