#include "application_guild_settings_internal.hpp"
#include "application_account_presentation.hpp"
#include "application_member_moderation.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/member_context_policy.hpp"

namespace maicord::app_detail::server_settings {
namespace {

constexpr MemberModerationMenuActions kModerationActions{11, 12, 13, 14, 15, 100};

void project_members(Bank& bank) noexcept {
    push({control_search, utf8_literal("Search members"), {}, UiSettingsControlKind::edit, editable(), false, {}, 0,
          {}, &editor_view(editor_search, control_search, utf8_literal("Search by name"), editable())});
    const auto& members = g_app.account_ui->guild_member_store;
    const auto query = editor_text(editor_search);
    // Sort loaded members by display name.
    auto& order = owner->member_order;
    u32 count = 0;
    for (u32 i = 0; i < members.capacity && count < kRowCapacity; ++i) {
        const auto& m = members.records[i];
        if (!m.occupied || m.guild_id != owner->guild_id)
            continue;
        Utf8View username{};
        const auto name = member_name(m, &username);
        if (guild_settings_text_matches(query, name) || guild_settings_text_matches(query, username))
            order[count++] = i;
    }
    const auto fold = [](char c) noexcept { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
    heap_sort(order, count, [&](u32 a, u32 b) noexcept {
        Utf8View ua{}, ub{};
        const auto na = member_name(members.records[a], &ua), nb = member_name(members.records[b], &ub);
        const u32 n = minimum(na.bytes, nb.bytes);
        for (u32 i = 0; i < n; ++i)
            if (fold(na.data[i]) != fold(nb.data[i]))
                return fold(na.data[i]) < fold(nb.data[i]);
        return na.bytes < nb.bytes;
    });
    const u32 heading = bank.mark();
    bank.add(utf8_literal("Members — "));
    bank.number(count);
    push(category(control_list_category, bank.since(heading),
                  utf8_literal("Members loaded on this device. Open a channel's member list to load more.")));
    if (!count)
        push(notice({}, query.bytes ? utf8_literal("No loaded members match this search.")
                                    : utf8_literal("No members are loaded for this server yet.")));
    auto& chips = owner->chips;
    auto& chip_colors = owner->chip_colors;
    owner->row_count = 0;
    const u64 now = clock_unix_milliseconds() / 1000;
    for (u32 i = 0; i < count && owner->row_count < kRowCapacity; ++i) {
        const auto& m = members.records[order[i]];
        Utf8View username{};
        const auto name = bank.add(member_name(m, &username));
        const u32 start = bank.mark();
        if (username.bytes && !utf8_equal(username, name)) {
            bank.add(utf8_literal("@"));
            bank.add(username);
        }
        if (m.user_id == owner->access.owner_id)
            bank.add(bank.since(start).bytes ? utf8_literal(" · Owner") : utf8_literal("Server owner"));
        if (m.communication_disabled_known && m.communication_disabled_until > now)
            bank.add(bank.since(start).bytes ? utf8_literal(" · Timed out") : utf8_literal("Timed out"));
        const auto description = bank.since(start);
        // Highest roles first, up to three chips and an overflow count.
        const auto ids = guild_member_store_role_ids(m);
        const RoleRecord* held[kGuildMemberRoleIdCapacity];
        u32 held_count = 0;
        for (u32 j = 0; j < ids.count && held_count < kGuildMemberRoleIdCapacity; ++j)
            if (const auto* r = role(ids[j]); r && r->id != owner->guild_id)
                held[held_count++] = r;
        heap_sort(held, held_count, [](const RoleRecord* a, const RoleRecord* b) noexcept {
            return a->position > b->position || (a->position == b->position && a->id < b->id);
        });
        const u32 row = owner->row_count;
        u32 chip_count = 0;
        for (u32 j = 0; j < held_count && chip_count < 3; ++j) {
            chips[row][chip_count] = bank.add(chat_state_store_role_name(state(), *held[j]));
            chip_colors[row][chip_count++] = guild_role_color_rgba(held[j]->color);
        }
        if (held_count > 3) {
            const u32 more = bank.mark();
            bank.add(utf8_literal("+"));
            bank.number(held_count - 3);
            chips[row][chip_count] = bank.since(more);
            chip_colors[row][chip_count++] = 0;
        }
        const auto capabilities =
            moderation_capabilities(state(), members, owner->guild_id, m.user_id, now);
        owner->row_ids[row] = m.user_id;
        const u32 top_color = held_count && held[0]->color ? guild_role_color_rgba(held[0]->color) : 0;
        push_avatar(push({kRowBase + row, name, description, UiSettingsControlKind::action, true, false,
                          {chips[row], chip_count}, 0, capabilities.visible ? utf8_literal("Manage") : Utf8View{},
                          nullptr},
                         {UiSettingsVariant::standard,
                          static_cast<u8>(ui_settings_presentation_avatar | ui_settings_presentation_compact |
                                          ui_settings_presentation_accent),
                          kUiSettingsNoDefault}),
                    m.user_id);
        owner->row_accent[row] = top_color ? top_color : person_color(m.user_id);
        ++owner->row_count;
    }
}

void open_member_menu(u64 user, UiRect anchor) noexcept {
    auto& p = *owner;
    const auto capabilities = moderation_capabilities(state(), g_app.account_ui->guild_member_store, p.guild_id, user,
                                                      clock_unix_milliseconds() / 1000);
    const u32 count = member_context_moderation_build(capabilities, kModerationActions, {p.popup_items, 8});
    if (!count)
        return;
    p.popup_items[0].flags = static_cast<u16>(p.popup_items[0].flags & ~popup_item_separator_before);
    p.menu = Menu::member;
    p.menu_user = user;
    settings_layer_open_choice(&p.layer, user, {p.popup_items, count}, anchor, 240);
}

void activate(const SettingsLayerActivation& activation) noexcept {
    auto& p = *owner;
    const u64 id = activation.value.target.control_id;
    // The whole row opens the menu, by pointer or keyboard.
    if (id >= kRowBase && id < kRowBase + p.row_count)
        open_member_menu(p.row_ids[id - kRowBase], activation.anchor);
}

void choose(Menu menu, u32 action) noexcept {
    auto& p = *owner;
    if (menu == Menu::member) {
        const u64 user = p.menu_user;
        if (action == kModerationActions.timeout || action == kModerationActions.ban) {
            const bool timeout = action == kModerationActions.timeout;
            const u32 count = timeout ? member_context_timeout_choices(kModerationActions, {p.popup_items, 16})
                                      : member_context_ban_choices(kModerationActions, {p.popup_items, 16});
            p.menu = timeout ? Menu::timeout : Menu::ban;
            settings_layer_open_choice(&p.layer, user, {p.popup_items, count}, g_app.popup_scene.anchor, 240);
            return;
        }
        const u8 bit = action == kModerationActions.nickname      ? moderation_action_nickname
                       : action == kModerationActions.end_timeout ? moderation_action_end_timeout
                       : action == kModerationActions.kick        ? moderation_action_kick
                                                                  : 0;
        if (bit)
            application_member_moderation_start(p.guild_id, user, bit, 0);
    } else if (menu == Menu::timeout || menu == Menu::ban) {
        const u32 index = action - kModerationActions.choice_first;
        const bool timeout = menu == Menu::timeout;
        const u32 limit = timeout ? sizeof(kModerationTimeoutChoices) / sizeof(u32)
                                  : sizeof(kModerationBanDeleteChoices) / sizeof(u32);
        if (action >= kModerationActions.choice_first && index < limit)
            application_member_moderation_start(p.guild_id, p.menu_user,
                                                timeout ? moderation_action_timeout : moderation_action_ban,
                                                timeout ? kModerationTimeoutChoices[index]
                                                        : kModerationBanDeleteChoices[index]);
    }
}

} // namespace

const PageOps kMembersPage{nullptr, project_members, activate, choose,  nullptr, nullptr,
                           nullptr, nullptr,         nullptr,  nullptr, nullptr};

} // namespace maicord::app_detail::server_settings
