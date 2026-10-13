#pragma once
#include "maicord/app/account_gif_favorites.hpp"

namespace maicord::app_detail {
struct ApplicationGifFavorites;
struct ApplicationGifFavoritesView {
    ConstSlice<GifFavoriteRecord> records;
    u64 revision, data_revision;
    bool authoritative, loading, failed;
    Utf8View status;
};
struct ApplicationGifFavoriteState {
    bool confirmed, pending, failed, available;
};
bool application_gif_favorites_open() noexcept;
void application_gif_favorites_set_visible(bool) noexcept;
// Call after pending responses have been handled or the account worker has joined.
void application_gif_favorites_release() noexcept;
ApplicationGifFavoritesView application_gif_favorites_view() noexcept;
ApplicationGifFavoriteState application_gif_favorites_state(Utf8View url) noexcept;
bool application_gif_favorites_toggle(Utf8View url, Utf8View src, u32 width, u32 height,
                                      GifFavoriteFormat format) noexcept;
// FavoriteGIF.format as the official client stores it: VIDEO for an MP4/WebM
// source (its picker plays a <video>), IMAGE for everything else.
inline GifFavoriteFormat application_gif_favorites_format(Utf8View src) noexcept {
    u32 end = 0;
    while (end < src.bytes && src.data[end] != '?' && src.data[end] != '#')
        ++end;
    const auto ends = [&](const char* suffix, u32 bytes) noexcept {
        if (end < bytes)
            return false;
        for (u32 i = 0; i < bytes; ++i) {
            char c = src.data[end - bytes + i];
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c + ('a' - 'A'));
            if (c != suffix[i])
                return false;
        }
        return true;
    };
    return ends(".mp4", 4) || ends(".webm", 5) ? GifFavoriteFormat::video : GifFavoriteFormat::image;
}
void application_gif_favorites_synchronize() noexcept;
bool application_gif_favorites_pending(u64 now_ms) noexcept;
bool application_gif_favorites_submit(u64 request_id, u64 now_ms) noexcept;
bool application_gif_favorites_response(const RestClientResponse&, Utf8View body) noexcept;
// The primary response guard calls this after erasing its output slice. Details
// responses cannot release a buffer while the Favorites request remains pending.
void application_gif_favorites_response_drained() noexcept;
Slice<u8> application_gif_favorites_response_buffer() noexcept;
u64 application_gif_favorites_next_wake() noexcept;
u32 application_gif_favorites_owner_bytes() noexcept;
} // namespace maicord::app_detail
