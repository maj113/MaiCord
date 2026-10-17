#pragma once

#include "maicord/core/arena.hpp"
#include "maicord/model/member_list.hpp"
#include "maicord/ui/emoji_catalog.hpp"
#include "maicord/model/chat_state.hpp"

namespace maicord {
struct MessageComposer;
struct MessageStore;
struct NavigationView;

inline constexpr u32 kComposerAutocompleteCapacity = 16;
// At most ten rows: users first, then roles fill the rows left, then
// @everyone/@here take any row still free (listed between users and roles).
inline constexpr u32 kMentionAutocompleteLimit = 10;
inline constexpr u32 kComposerAutocompleteInsertionBytes = maximum(kUnicodeEmojiMaximumGlyphBytes + 1, kCustomEmojiNameMaximumBytes + 25);
inline constexpr u32 kComposerAutocompleteLabelBytes =
    kUnicodeEmojiMaximumGlyphBytes + 2 + 1 + kUnicodeEmojiMaximumNameBytes + 1;

enum class ComposerAutocompleteKind : u8 {
    none,
    member,
    channel,
    emoji,
    // Per-suggestion kinds inside a member ('@') autocomplete.
    role,
    global_mention,
};

struct ComposerAutocompleteSuggestion {
    Utf8View label;
    Utf8View username;
    char username_storage[kUserDisplayNameMaximumBytes];
    u64 user_id;
    u32 avatar_variant;
    GuildMemberStatus status;
    bool animated_avatar;
    u64 emoji_id;
    bool animated_emoji;
    char label_storage[kComposerAutocompleteLabelBytes];
    char insertion[kComposerAutocompleteInsertionBytes];
    u16 insertion_bytes;
    u16 label_bytes;
    // member, channel, emoji, role or global_mention (@everyone / @here).
    ComposerAutocompleteKind item_kind;
    // 0xRRGGBB role colour; zero uses the theme.
    u32 role_color;
    u64 role_id;
    // Static description for roles and global mentions.
    Utf8View description;
};

struct ComposerAutocomplete {
    Arena storage;
    ComposerAutocompleteSuggestion* suggestions;
    u32 count;
    u32 token_begin;
    u32 token_end;
    ComposerAutocompleteKind kind;
    // Nonzero while an '@' query in this guild should also ask the gateway for
    // matching members (a REQUEST_GUILD_MEMBERS search); it stays set
    // when nothing matches locally yet.
    u64 member_query_guild_id;
};

// '@' lists users, then @everyone/@here, then roles, ten rows at most. In a
// guild channel (or thread) users are the cached guild members who can view the
// channel, or with an empty query the channel's recent message authors when it
// has any; private channels list their recipients and the current user. Users
// whose name (nickname, display name or username) starts with the query come
// first, then in-order letter matches. Roles are the mentionable ones, or all
// of them with MENTION_EVERYONE, ranked by match quality, ties by name. The
// member list view only supplies presence and, outside known channels,
// candidates.
bool composer_autocomplete_build(ComposerAutocomplete* autocomplete, const MessageComposer& composer,
                                 const MemberListView& members, const UserStore& users,
                                 const NavigationView& navigation,
                                 const ChatStateStore& state, const GuildMemberStore* member_store = nullptr,
                                 const MessageStore* messages = nullptr) noexcept;
void composer_autocomplete_release(ComposerAutocomplete* autocomplete) noexcept;

} // namespace maicord
