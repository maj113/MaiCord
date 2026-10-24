#include "maicord/app/account_media_catalog.hpp"

namespace maicord {
RestClientSubmitResult account_media_catalog(RestClient* rest,
                                             AccountMediaCatalogOperation operation,
                                             const MediaCatalogQuery* query,
                                             u64 request_id) noexcept {
    if (!rest || !request_id)
        return {RestClientSubmitError::invalid_input};
    if (operation == AccountMediaCatalogOperation::cancel) {
        if (query)
            return {RestClientSubmitError::invalid_input};
        return rest_client_cancel_result(rest, request_id);
    }
    if (operation != AccountMediaCatalogOperation::submit || !query)
        return {RestClientSubmitError::invalid_input};
    u8 target[kMediaCatalogMaximumTargetBytes];
    const auto written = media_catalog_write_target(*query, {target, sizeof(target)});
    if (!written.ok())
        return {written.error == MediaCatalogError::capacity ? RestClientSubmitError::target_capacity
                                                             : RestClientSubmitError::invalid_input};
    // Distinct endpoint buckets; search text never changes route identity.
    constexpr u64 routes[]{0x4749464341544547ull, 0x4749465452454E44ull, 0x4749465345415243ull,
                           0x4749465355474745ull, 0x535449434B504143ull, 0x535449434B475549ull};
    const bool guild = query->operation == MediaCatalogOperation::guild_stickers;
    return rest_client_submit(
        rest,
        {target, written.bytes},
        {},
        {routes[static_cast<u32>(query->operation)], guild ? query->guild_id : 0, RestMethod::get,
         guild ? RestMajorParameter::guild : RestMajorParameter::none},
        request_id,
        false);
}
} // namespace maicord
