#pragma once
#include "maicord/protocol/gif_favorites.hpp"
namespace maicord {
struct RestClient;
struct RestClientSubmitResult;

enum class AccountGifFavoritesOperation : u8 {
    fetch,
    update,
    cancel
};
// Uses the existing primary REST owner. The application broker owns exact
// profile/self/snapshot revision tickets and drains/cancels their lifetimes.
RestClientSubmitResult account_gif_favorites(RestClient*, AccountGifFavoritesOperation,
                                             ConstSlice<u8> patch_json, u64 request_id) noexcept;
// Same settings-proto/2 route; updates may write only emoji favorites/frecency.
RestClientSubmitResult account_emoji_preferences(RestClient*, AccountGifFavoritesOperation,
                                                 ConstSlice<u8> patch_json, u64 request_id) noexcept;
// Same settings-proto/2 route; updates may write only application command frecency.
RestClientSubmitResult account_command_frecency(RestClient*, AccountGifFavoritesOperation,
                                                ConstSlice<u8> patch_json, u64 request_id) noexcept;
} // namespace maicord
