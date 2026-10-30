#include "maicord/app/account_gif_favorites.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/protocol/emoji_preferences.hpp"
#include "maicord/protocol/application_commands.hpp"
#include "maicord/app/rest_client.hpp"
namespace maicord {
namespace {
using PatchValidator = bool (*)(ConstSlice<u8>) noexcept;
RestClientSubmitResult settings_proto(RestClient* rest, AccountGifFavoritesOperation operation,
                                      ConstSlice<u8> patch, u64 request, PatchValidator valid) noexcept {
    const RestClientSubmitResult invalid{RestClientSubmitError::invalid_input};
    if (!rest || !request || !byte_range_valid(patch))
        return invalid;
    if (operation == AccountGifFavoritesOperation::cancel) {
        if (patch.count)
            return invalid;
        return rest_client_cancel_result(rest, request);
    }
    if (operation > AccountGifFavoritesOperation::update ||
        (operation == AccountGifFavoritesOperation::fetch && patch.count) ||
        (operation == AccountGifFavoritesOperation::update && !valid(patch)))
        return invalid;
    static constexpr char target[] = "/api/v10/users/@me/settings-proto/2";
    const bool update = operation == AccountGifFavoritesOperation::update;
    return rest_client_submit_user_settings(
        rest,
        {{reinterpret_cast<const u8*>(target), sizeof(target) - 1},
         patch,
         {kRestUserSettingsRouteId, 0, update ? RestMethod::patch : RestMethod::get,
          RestMajorParameter::none},
         request,
         kRestClientDefaultTimeoutMs,
         update,
         false},
        kRestClientMaximumResponseBytes);
}
Utf8View text(ConstSlice<u8> patch) noexcept {
    return {reinterpret_cast<const char*>(patch.data), patch.count};
}
} // namespace
RestClientSubmitResult account_gif_favorites(RestClient* rest,
                                             AccountGifFavoritesOperation operation,
                                             ConstSlice<u8> patch, u64 request) noexcept {
    return settings_proto(rest, operation, patch, request, [](ConstSlice<u8> value) noexcept {
        return gif_favorites_validate_patch_json(text(value)) == GifFavoritesError::none;
    });
}
RestClientSubmitResult account_emoji_preferences(RestClient* rest,
                                                 AccountGifFavoritesOperation operation,
                                                 ConstSlice<u8> patch, u64 request) noexcept {
    return settings_proto(rest, operation, patch, request, [](ConstSlice<u8> value) noexcept {
        return emoji_preferences_validate_patch_json(text(value)) == EmojiPreferencesError::none;
    });
}
RestClientSubmitResult account_command_frecency(RestClient* rest,
                                                AccountGifFavoritesOperation operation,
                                                ConstSlice<u8> patch, u64 request) noexcept {
    return settings_proto(rest, operation, patch, request, [](ConstSlice<u8> value) noexcept {
        return command_frecency_patch_valid(text(value));
    });
}
} // namespace maicord
