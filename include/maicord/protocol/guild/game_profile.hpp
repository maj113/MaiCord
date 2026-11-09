#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {

// The game profile's details of one game object from GET /games?game_ids={id}
// (Userdoccers: Games > List Games; the game object and its
// supplemental_game_data), read as the official client reads its game record:
// the top-level summary_localized (else description), genres, platforms,
// companies (role 1 publisher, 2 developer), first_release_date and websites,
// with the documented supplemental fields (summary, publisher_names,
// developer_names, ...) filling what the top level leaves out.
inline constexpr u32 kGameProfileSummaryBytes = 4096;
inline constexpr u32 kGameProfileNamesBytes = 512;
inline constexpr u32 kGameProfileGenreMaximum = 16;
inline constexpr u32 kGameProfileLinkMaximum = 16;
inline constexpr u32 kGameProfileUrlBytes = 384;

struct GameProfileLink {
    u8 category; // Userdoccers: Game Website Category
    u16 url_bytes;
    char url[kGameProfileUrlBytes];
};

struct GameProfile {
    u64 id;
    u64 release_ms; // zero: unknown
    u16 summary_bytes, publishers_bytes, developers_bytes;
    u8 genre_count, link_count, publisher_count, developer_count;
    u16 platforms; // bit per Userdoccers Game Platform Type
    u8 genres[kGameProfileGenreMaximum]; // Userdoccers: SKU Genre
    char summary[kGameProfileSummaryBytes];
    char publishers[kGameProfileNamesBytes]; // ", "-joined
    char developers[kGameProfileNamesBytes];
    GameProfileLink links[kGameProfileLinkMaximum];
};

// The game with id in the response; false when it is missing or the body is not a list of games.
bool game_profile_decode(Utf8View body, Slice<u32> scratch, u64 id, GameProfile* output) noexcept;

} // namespace maicord
