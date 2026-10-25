#include "maicord/ui/member_context_policy.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/popup_scene.hpp"

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
                                    const MemberModerationMenuActions& actions, Slice<PopupItem> output) noexcept {
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

u32 member_context_timeout_choices(const MemberModerationMenuActions& actions, Slice<PopupItem> output) noexcept {
    static constexpr Utf8View labels[] = {utf8_literal("60 seconds"), utf8_literal("5 minutes"), utf8_literal("10 minutes"),
                                   utf8_literal("1 hour"),     utf8_literal("1 day"),     utf8_literal("1 week")};
    static_assert(sizeof(labels) / sizeof(labels[0]) == sizeof(kModerationTimeoutChoices) / sizeof(u32));
    constexpr u32 count = sizeof(labels) / sizeof(labels[0]);
    if (!output.data || output.count < count)
        return 0;
    for (u32 i = 0; i < count; ++i) {
        reset_value(&output[i]);
        output[i].label = labels[i];
        output[i].action = actions.choice_first + i;
        output[i].flags = popup_item_enabled;
    }
    return count;
}

u32 member_context_ban_choices(const MemberModerationMenuActions& actions, Slice<PopupItem> output) noexcept {
    static constexpr Utf8View labels[] = {utf8_literal("Keep their messages"),   utf8_literal("Delete the last hour"),
                                   utf8_literal("Delete the last 6 hours"), utf8_literal("Delete the last 12 hours"),
                                   utf8_literal("Delete the last day"),   utf8_literal("Delete the last 3 days"),
                                   utf8_literal("Delete the last week")};
    static_assert(sizeof(labels) / sizeof(labels[0]) == sizeof(kModerationBanDeleteChoices) / sizeof(u32));
    constexpr u32 count = sizeof(labels) / sizeof(labels[0]);
    if (!output.data || output.count < count)
        return 0;
    for (u32 i = 0; i < count; ++i) {
        reset_value(&output[i]);
        output[i].label = labels[i];
        output[i].action = actions.choice_first + i;
        output[i].flags = static_cast<u16>(popup_item_enabled | (i == 1 ? popup_item_separator_before : 0u));
    }
    return count;
}

} // namespace maicord
