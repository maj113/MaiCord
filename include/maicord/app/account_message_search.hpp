#pragma once

#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/message_search.hpp"

namespace maicord {

enum class AccountMessageSearchOperation : u8 {
    submit,
    cancel
};
inline constexpr u64 kGuildMessageSearchRouteId = 0x534541524348474Cull;
inline constexpr u64 kChannelMessageSearchRouteId = 0x5345415243484348ull;
inline constexpr u64 kGuildMessageSearchTabsRouteId = 0x5354414253474C44ull;
inline constexpr u64 kChannelMessageSearchTabsRouteId = 0x5354414253434843ull;
inline constexpr u64 kPrivateMessageSearchTabsRouteId = 0x5354414253444D53ull;
// The query's endpoint selects GET .../messages/search or POST .../messages/search/tabs.
RestClientSubmitResult account_message_search(RestClient*, AccountMessageSearchOperation,
                                              const MessageSearchQuery*, u64 request_id) noexcept;

} // namespace maicord
