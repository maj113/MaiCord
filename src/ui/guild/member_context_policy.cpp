#include "maicord/ui/guild/member_context_policy.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/ui/shell/popup_scene.hpp"

namespace maicord {

Utf8View member_context_block_text(ModerationBlock block) noexcept {
    switch (block) {
    case ModerationBlock::unresolved: return utf8_literal("Member details are still loading");
    case ModerationBlock::target_owner: return utf8_literal("Server owner");
    case ModerationBlock::hierarchy: return utf8_literal("Their role is not below yours");
    case ModerationBlock::target_admin: return utf8_literal("Administrators can't be timed out");
    case ModerationBlock::two_factor: return utf8_literal("Requires two-factor authentication");
    default: return {};
    }
}

u32 member_context_moderation_build(const ModerationCapabilities& capabilities,
                                    const MemberModerationMenuActions& actions, const Slice<PopupItem>& output) noexcept {
    struct Entry {
        u8 bit;
        Utf8View label;
        u32 action;
        bool danger;
    };
    const Entry entries[] = {
        {moderation_action_nickname, utf8_literal("Edit nickname"), actions.nickname, false},
        {moderation_action_timeout, utf8_literal("Time out"), actions.timeout, false},
        {moderation_action_end_timeout, utf8_literal("End timeout"), actions.end_timeout, false},
        {moderation_action_kick, utf8_literal("Remove from server"), actions.kick, true},
        {moderation_action_ban, utf8_literal("Ban from server"), actions.ban, true},
    };
    u32 needed = 0;
    for (const auto& entry : entries)
        needed += (capabilities.visible & entry.bit) ? 1u : 0u;
    if (!needed || !output.data || output.count < needed)
        return 0;
    const Utf8View reason = member_context_block_text(capabilities.block);
    u32 used = 0;
    for (const auto& entry : entries) {
        if (!(capabilities.visible & entry.bit))
            continue;
        const bool allowed = (capabilities.allowed & entry.bit) != 0;
        PopupItem item{};
        item.label = entry.label;
        item.action = entry.action;
        item.flags = static_cast<u16>((allowed ? popup_item_enabled : 0u) |
                                      (entry.danger && allowed ? popup_item_danger : 0u) |
                                      (used == 0 ? popup_item_separator_before : 0u));
        if (!allowed)
            item.detail = reason;
        output[used++] = item;
    }
    return used;
}

namespace {
// One enabled row for each of the `count` names (each followed by "\0"), actions from first_action; `separator` gets
// the rule above it.
u32 choices(const char* names, u32 count, u32 first_action, u32 separator, const Slice<PopupItem>& output) noexcept {
    if (!output.data || output.count < count)
        return 0;
    u32 i = 0;
    for (NameList name{names}; name; name.next(), ++i)
        popup_item_set(&output[i], name.name(), first_action + i,
                       static_cast<u16>(popup_item_enabled | (i == separator ? popup_item_separator_before : 0u)));
    return count;
}
} // namespace

u32 member_context_timeout_choices(const MemberModerationMenuActions& actions, const Slice<PopupItem>& output) noexcept {
    static constexpr char kLabels[] = "60 seconds\0" "5 minutes\0" "10 minutes\0" "1 hour\0" "1 day\0" "1 week\0";
    constexpr u32 kCount = sizeof(kModerationTimeoutChoices) / sizeof(u32);
    static_assert(name_list_count(kLabels) == kCount);
    return choices(kLabels, kCount, actions.choice_first, ~0u, output);
}

u32 member_context_ban_choices(const MemberModerationMenuActions& actions, Slice<PopupItem> output) noexcept {
    static constexpr char kLabels[] = "Keep their messages\0Delete the last hour\0Delete the last 6 hours\0"
                                      "Delete the last 12 hours\0Delete the last day\0Delete the last 3 days\0"
                                      "Delete the last week\0";
    constexpr u32 kCount = sizeof(kModerationBanDeleteChoices) / sizeof(u32);
    static_assert(name_list_count(kLabels) == kCount);
    return choices(kLabels, kCount, actions.choice_first, 1, output);
}

} // namespace maicord
