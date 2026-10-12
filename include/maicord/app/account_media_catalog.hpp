#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/media_catalog.hpp"

namespace maicord {
enum class AccountMediaCatalogOperation : u8 {
    submit,
    cancel
};
// One authenticated existing REST worker, no provider key or parallel network
// owner. The application broker supplies a unique scoped request ID and admits
// a completed response only while its account/query generation still matches.
RestClientSubmitResult account_media_catalog(RestClient* rest, AccountMediaCatalogOperation operation,
                                             const MediaCatalogQuery* query, u64 request_id) noexcept;
} // namespace maicord
