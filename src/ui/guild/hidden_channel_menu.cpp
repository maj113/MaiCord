#include "maicord/ui/guild/hidden_channel_menu.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/guild/permission_compute.hpp"
#include "maicord/ui/text/timestamp_format.hpp"

namespace maicord {
namespace {
constexpr u32 kRoleRows = 5, kMemberRows = 3;

PopupItem info_row(Utf8View label, UiIcon icon, Utf8View detail = {}) noexcept {
    PopupItem item{};
    item.label = label;
    item.detail = detail;
    item.presentation = ui_menu_row_presentation(icon, detail.bytes != 0);
    return item;
}

// Whether a member holding only this role (and @everyone) would see the channel. The member is
// hypothetical: an id no account can have (snowflakes stay below 2^63), so no owner or member
// overwrite applies to it.
bool role_sees(const ChatStateStore& state, const ChannelRecord& channel, u64 role_id) noexcept {
    const PermissionMember member{~0ull, {&role_id, 1}, 0, false};
    return permission_has(
        chat_state_store_member_permissions(state, channel.guild_id, channel.id, member, PermissionOptions{}),
        kPermissionViewChannel);
}

u64 unix_seconds_of(u64 snowflake) noexcept {
    return ((snowflake >> 22) + kSnowflakeEpochMilliseconds) / 1000;
}
} // namespace

u32 hidden_channel_menu_build(const ChatStateStore& state, const UserStore& users, const ChannelRecord& channel,
                              u64 now_unix_seconds, TextBank* bank, Slice<PopupItem> output) noexcept {
    if (!chat_state_store_channel_hidden(state, channel) || output.count < kHiddenChannelMenuRows)
        return 0;
    u32 count = 0;
    PopupItem* items = output.data;
    items[count++] = info_row(chat_state_store_channel_name(state, channel), UiIcon::lock,
                              utf8_literal("You can't view this channel"));
    char date[kMessageMarkdownTimestampMaximumBytes];
    const u32 created = bank->used;
    bank->add("Created ");
    if (const auto stamp = format_profile_date(unix_seconds_of(channel.id), Slice<char>{date, sizeof(date)}); stamp.ok)
        bank->append(date, stamp.bytes);
    items[count++] = info_row(bank->since(created), UiIcon::calendar);
    if (channel.last_message_id) {
        const u32 last = bank->used;
        bank->add("Last message ");
        if (const auto stamp = format_timestamp_calendar(unix_seconds_of(channel.last_message_id), now_unix_seconds,
                                                         Slice<char>{date, sizeof(date)});
            stamp.ok)
            bank->append(date, stamp.bytes);
        items[count++] = info_row(bank->since(last), UiIcon::recent);
    }

    // Who sees it: roles (highest first), then members an overwrite names.
    const RoleRecord* roles[kRoleRows]{};
    u32 role_count = 0, role_total = 0;
    for (u32 index = 0; index < state.role_count; ++index) {
        const RoleRecord& role = state.roles[index];
        if (role.guild_id != channel.guild_id || role.id == channel.guild_id || role.managed ||
            !role_sees(state, channel, role.id))
            continue;
        ++role_total;
        u32 at = role_count;
        while (at && roles[at - 1]->position < role.position) {
            if (at < kRoleRows)
                roles[at] = roles[at - 1];
            --at;
        }
        if (at < kRoleRows) {
            roles[at] = &role;
            role_count = minimum(role_count + 1, kRoleRows);
        }
    }
    u64 members[kMemberRows]{};
    u32 member_count = 0, member_total = 0;
    const PermissionOverwriteRecord* overwrites = state.overwrites ? state.overwrites + channel.overwrite_offset : nullptr;
    for (u32 index = 0; overwrites && index < channel.overwrite_count; ++index) {
        if (overwrites[index].type != 1 || !permission_has(overwrites[index].allow, kPermissionViewChannel))
            continue;
        ++member_total;
        if (member_count < kMemberRows)
            members[member_count++] = overwrites[index].id;
    }
    const u32 heading = count;
    items[count++] = info_row(role_total || member_total ? utf8_literal("Who can view it")
                                                          : utf8_literal("Owner and admins only"),
                              UiIcon::members);
    items[heading].flags |= popup_item_separator_before;
    for (u32 index = 0; index < role_count; ++index) {
        PopupItem item = info_row(chat_state_store_role_name(state, *roles[index]), UiIcon::shield);
        if (roles[index]->color) {
            item.media_flags = kPopupItemMediaIconTint;
            item.media_variant = roles[index]->color & 0xFFFFFFu;
        }
        items[count++] = item;
    }
    for (u32 index = 0; index < member_count; ++index) {
        const UserRecord* user = user_store_find(users, members[index]);
        items[count++] =
            info_row(user ? user_store_name(users, *user) : utf8_literal("Unknown user"), UiIcon::user);
    }
    const u32 more = (role_total - role_count) + (member_total - member_count);
    if (more) {
        const u32 start = bank->used;
        bank->add("and ");
        bank->number(more);
        bank->add(" more");
        items[count++] = info_row(bank->since(start), UiIcon::members);
    }
    // The popup requires a distinct action per row; none is enabled, so none can fire.
    for (u32 index = 0; index < count; ++index)
        items[index].action = 0x48430000u + index + 1;
    return count;
}

} // namespace maicord
