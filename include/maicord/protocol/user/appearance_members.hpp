#pragma once

#include "maicord/core/json_reader.hpp"
#include "maicord/model/user/activity.hpp"
#include "maicord/model/user/profile.hpp"

// Decoders for a caller that visits a user, member or presence object for its own members. The caller gathers the
// members below in that visit, so no decoder visits the object a second time.
namespace maicord {

// The member names that an appearance reads from a user object or a guild member object, in the order of
// UserAppearanceMembers::value.
#define MAICORD_USER_APPEARANCE_MEMBERS \
    "bot", "public_flags", "display_name_styles", "avatar_decoration_data", "collectibles", "primary_guild", "avatar", \
        "system"
inline constexpr u32 kUserAppearanceMemberCount = 8;
inline constexpr u32 kUserAppearanceAvatarMember = 6;

struct UserAppearanceMembers {
    // A value without source text: the object does not hold the member.
    JsonValue value[kUserAppearanceMemberCount];
    // The object holds one of the names two times. The decode refuses it.
    bool repeated;

    void take(u32 index, const JsonValue& member) noexcept {
        if (value[index].source.data)
            repeated = true;
        else
            value[index] = member;
    }
};

// user_appearance_decode for gathered members.
bool user_appearance_decode(const JsonIndexedDocument& doc, const UserAppearanceMembers& members, u64 user_id,
                            u64 guild_id, UserAppearance* output) noexcept;
// activity_decode and user_activities_decode for the `activities` member of a presence. A value without source text:
// the presence does not hold the member. activity_decode and activity_decode_list take an appearance that holds no
// activity text or status emoji (a cleared or freshly decoded one). user_activities_decode_list leaves the items past
// the count as they were.
bool activity_decode_list(const JsonIndexedDocument& doc, const JsonValue& activities,
                          UserAppearance* output) noexcept;
bool user_activities_decode_list(const JsonIndexedDocument& doc, const JsonValue& activities, u64 user_id,
                                 UserActivitySet* output, bool* present) noexcept;

} // namespace maicord
