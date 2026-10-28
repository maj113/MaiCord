#include "application_guild_settings_internal.hpp"
#include "application_member_moderation.hpp"
#include "application_settings.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/member_context_policy.hpp"

#include "maicord/core/memory.hpp"
#include "account_ui_runtime.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail::server_settings {
namespace {

constexpr MemberModerationMenuActions kModerationActions{11, 12, 13, 14, 15, 100};
// The search box applies 300 ms after typing stops.
constexpr u64 kSearchDebounceMs = 300;
constexpr u64 kHourMs = 3'600'000ull, kDayMs = 86'400'000ull;
// Member flags and user flags behind the Signals column.
constexpr u32 kMemberQuarantined = 1u << 7;
constexpr u64 kUserSpammer = 1ull << 20;

struct DatePreset {
    Utf8View label;
    u32 amount;
    // h hours, d days, w weeks, M calendar months.
    char unit;
};
constexpr DatePreset kDatePresets[]{
    {utf8_literal("All"), 0, 0},           {utf8_literal("Past 1 hr"), 1, 'h'},
    {utf8_literal("Past 24 hrs"), 24, 'h'}, {utf8_literal("Past 7 days"), 7, 'd'},
    {utf8_literal("Past 2 weeks"), 2, 'w'}, {utf8_literal("Past 4 weeks"), 4, 'w'},
    {utf8_literal("Past 3 months"), 3, 'M'},
};
constexpr u32 kDatePresetCount = sizeof(kDatePresets) / sizeof(kDatePresets[0]);
constexpr Utf8View kSignals[]{utf8_literal("Unusual DM Activity"), utf8_literal("Timed Out"),
                              utf8_literal("Unusual Account Activity"), utf8_literal("Quarantined")};
constexpr Utf8View kSorts[]{utf8_literal("Member Since (Newest first)"), utf8_literal("Member Since (Oldest first)"),
                            utf8_literal("Joined Discord (Newest first)"),
                            utf8_literal("Joined Discord (Oldest first)")};
struct JoinType {
    u8 type;
    Utf8View label;
};
// The "Other Join Methods" entries, in the official order.
constexpr JoinType kJoinTypes[]{
    {3, utf8_literal("Server Discovery")}, {6, utf8_literal("Vanity URL")}, {1, utf8_literal("Bot Invite")},
    {4, utf8_literal("Student Hub")},      {7, utf8_literal("Manual Verification")},
    {8, utf8_literal("Linked Channel")},
};
constexpr Utf8View kPageSizes[]{utf8_literal("12"), utf8_literal("25"), utf8_literal("50"), utf8_literal("100")};
constexpr Utf8View kBulkButtons[]{utf8_literal("Ban Members"), utf8_literal("Clear selection")};
constexpr Utf8View kPageButtons[]{utf8_literal("Previous"), utf8_literal("Next")};

// The checkbox column: member safety access plus Manage Server and Ban Members.
bool bulk_allowed() noexcept {
    return can_manage_guild() && permission_has(owner->access.permissions, kPermissionBanMembers);
}
// Whether the search box differs from the text the current results were asked for.
bool search_text_changed(const Utf8View& text) noexcept {
    return text.bytes != owner->search_applied_bytes || !bytes_equal(text.data, owner->search_applied, text.bytes);
}
bool default_filters() noexcept {
    const auto& p = *owner;
    return !p.search_applied_bytes && !p.filter_role_count && !p.filter_joined && !p.filter_created &&
           !p.filter_signals && !p.filter_sort && !p.filter_join_type_set && !p.filter_invite_bytes;
}

// A Unix millisecond time a preset's span before now; calendar months keep
// the day of the month (clamped), as moment().subtract does.
u64 preset_after(u64 now, const DatePreset& preset) noexcept {
    switch (preset.unit) {
    case 'h': return now - preset.amount * kHourMs;
    case 'd': return now - preset.amount * kDayMs;
    case 'w': return now - preset.amount * 7 * kDayMs;
    default: break;
    }
    const u64 days = now / kDayMs, rest = now % kDayMs;
    // Civil date of `days`, then back N months.
    const CivilDate civil = civil_date_from_days(static_cast<i64>(days));
    i64 month = civil.month - static_cast<i64>(preset.amount), year = civil.year;
    while (month < 1) {
        month += 12;
        --year;
    }
    const u32 length = civil_month_days(year, static_cast<u32>(month));
    const u32 day = civil.day > length ? length : civil.day;
    return static_cast<u64>(civil_days_from_date(year, static_cast<u32>(month), day)) * kDayMs + rest;
}

// "3 days ago" as the members table's columns word it.
void relative(Bank& bank, u64 then_ms, u64 now_ms, bool account) noexcept {
    const u64 seconds = now_ms > then_ms ? (now_ms - then_ms) / 1000 : 0;
    const auto count = [&](u64 value, const char* one, const char* other) noexcept {
        bank.number(value);
        bank.add(" ");
        const char* word = value == 1 ? one : other;
        bank.add({word, c_string_length(word)});
        bank.add(" ago");
    };
    if (account && seconds < 86'400)
        bank.add("<1 day ago");
    else if (seconds < 60)
        bank.add("just now");
    else if (seconds < 3'600)
        count(seconds / 60, "min", "mins");
    else if (seconds < 86'400)
        count(seconds / 3'600, "hr", "hrs");
    else if (seconds < 2'592'000)
        count(seconds / 86'400, "day", "days");
    else if (seconds < 31'104'000)
        count(seconds / 2'592'000, "month", "months");
    else
        count(seconds / 31'104'000, "year", "years");
}

Utf8View join_method(Bank& bank, const GuildSearchMember& m) noexcept {
    switch (m.join_source_type) {
    case 1: return utf8_literal("Bot Invite");
    case 2: return utf8_literal("Integration");
    case 3: return utf8_literal("Server Discovery");
    case 4: return utf8_literal("Student Hub");
    case 5:
        return m.invite_code_bytes ? bank.add({m.invite_code, m.invite_code_bytes}) : utf8_literal("Unknown");
    case 6: {
        const auto* record = guild_details();
        const auto vanity = record ? guild_detail_store_vanity_url_code(details(), *record) : Utf8View{};
        return vanity.bytes ? bank.add(vanity) : utf8_literal("Vanity URL");
    }
    case 7: {
        if (!m.invite_code_bytes)
            return utf8_literal("Manual Verification");
        const u32 start = bank.used;
        bank.add("Manual Verification (");
        bank.add({m.invite_code, m.invite_code_bytes});
        bank.add(")");
        return bank.since(start);
    }
    case 8: return utf8_literal("Linked Channel");
    default: return utf8_literal("Unknown");
    }
}

bool selected(u64 user) noexcept {
    for (u32 i = 0; i < owner->selected_count; ++i)
        if (owner->selected[i] == user)
            return true;
    return false;
}
void select(u64 user, bool on) noexcept {
    auto& p = *owner;
    for (u32 i = 0; i < p.selected_count; ++i)
        if (p.selected[i] == user) {
            if (!on) {
                for (u32 j = i + 1; j < p.selected_count; ++j)
                    p.selected[j - 1] = p.selected[j];
                --p.selected_count;
            }
            return;
        }
    if (on && p.selected_count < kGuildBulkBanMaximumUsers)
        p.selected[p.selected_count++] = user;
}
bool can_ban(u64 user) noexcept {
    return (moderation_capabilities(state(), g_app.account_ui->guild_member_store, owner->guild_id, user,
                                    clock_unix_milliseconds() / 1000)
                .allowed &
            moderation_action_ban) != 0;
}

// How many members of the page the account may ban, and how many of them are selected.
void page_selection(u32* actionable, u32* chosen) noexcept {
    *actionable = *chosen = 0;
    for (u32 i = 0; i < owner->page_row_count; ++i)
        if (can_ban(owner->page_rows[i]->user_id)) {
            ++*actionable;
            *chosen += selected(owner->page_rows[i]->user_id);
        }
}

bool ensure_bank() noexcept {
    auto& p = *owner;
    if (p.search_bank)
        return true;
    p.search_bank = static_cast<GuildSearchMember*>(VirtualAlloc(
        nullptr, sizeof(GuildSearchMember) * kGuildMemberSearchCapacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    return p.search_bank && guild_member_search_bind(&p.search, {p.search_bank, kGuildMemberSearchCapacity});
}

// Asks for the current filters and page. A search in flight is cancelled
// and asked again once its receipt arrives; one already queued builds its
// body from the latest state when it is sent.
void queue_search() noexcept {
    auto& p = *owner;
    if (!ensure_bank())
        return;
    if (p.request_id && p.request.op == Op::members_search) {
        if (g_app.account.api.guild_moderation)
            g_app.account.api.guild_moderation(g_app.account.instance, nullptr, p.request_id);
        p.search_dirty = true;
        return;
    }
    if (busy()) {
        if (!(p.request.queued && p.request.op == Op::members_search))
            p.search_dirty = true;
        return;
    }
    p.search_retry = false;
    queue(Op::members_search);
}

// A filter change: page 1 of a new result set.
void refilter() noexcept {
    auto& p = *owner;
    if (!ensure_bank())
        return;
    guild_member_search_reset(&p.search, false);
    queue_search();
    repaint();
}

void apply_text() noexcept {
    auto& p = *owner;
    const auto text = editor_text(editor_search);
    p.search_due_at = 0;
    if (!search_text_changed(text))
        return;
    p.search_applied_bytes = minimum(text.bytes, kSearchBytes);
    copy_bytes(p.search_applied, text.data, p.search_applied_bytes);
    refilter();
}

void load() noexcept {
    auto& p = *owner;
    if (!ensure_bank())
        return;
    guild_member_search_reset(&p.search, true);
    p.search_dirty = p.search_retry = false;
    p.search_due_at = 0;
    p.search_applied_bytes = 0;
    p.filter_role_count = 0;
    p.filter_joined = p.filter_created = p.filter_signals = p.filter_sort = p.filter_join_type = 0;
    p.filter_join_type_set = false;
    p.filter_invite_bytes = 0;
    p.filter_joined_after = p.filter_created_after = 0;
}

Utf8View filter_value(Bank& bank, Menu menu) noexcept {
    auto& p = *owner;
    switch (menu) {
    case Menu::member_since: return kDatePresets[p.filter_joined].label;
    case Menu::joined_discord: return kDatePresets[p.filter_created].label;
    case Menu::join_method:
        if (p.filter_invite_bytes)
            return bank.add({p.filter_invite, p.filter_invite_bytes});
        for (const auto& type : kJoinTypes)
            if (p.filter_join_type_set && type.type == p.filter_join_type)
                return type.label;
        return utf8_literal("All");
    case Menu::member_roles:
    case Menu::signals: {
        u32 count = p.filter_role_count;
        if (menu == Menu::signals) {
            count = 0;
            for (u32 i = 0; i < 4; ++i)
                count += (p.filter_signals >> i) & 1;
        }
        if (!count)
            return utf8_literal("All");
        const u32 start = bank.used;
        bank.number(count);
        bank.add(" selected");
        return bank.since(start);
    }
    default: return kSorts[p.filter_sort ? p.filter_sort - 1 : 0];
    }
}

void push_filter(Bank& bank, u32 slot, u64 id, const Utf8View& label, Menu menu, bool enabled) noexcept {
    auto& p = *owner;
    p.filter_choice[slot][0] = filter_value(bank, menu);
    push_choices(id, label, {}, UiSettingsControlKind::choice, enabled, {p.filter_choice[slot], 1});
}

void project_members(Bank& bank) noexcept {
    auto& p = *owner;
    if (!ensure_bank()) {
        push_error({}, utf8_literal("Couldn't open the member list."));
        return;
    }
    const bool edit = editable();
    const u64 now = clock_unix_milliseconds();
    push_editor(control_search, utf8_literal("Search members"), {}, UiSettingsControlKind::edit, edit, editor_search,
                utf8_literal("Search by username or id"));
    if (can_manage_guild()) {
        push_filter(bank, 0, control_filter_joined, utf8_literal("Member Since"), Menu::member_since, edit);
        push_filter(bank, 1, control_filter_created, utf8_literal("Joined Discord"), Menu::joined_discord, edit);
        push_filter(bank, 2, control_filter_method, utf8_literal("Join Method"), Menu::join_method, edit);
    }
    push_filter(bank, 3, control_filter_roles, utf8_literal("Roles"), Menu::member_roles, edit);
    push_filter(bank, 4, control_filter_signals, utf8_literal("Signals"), Menu::signals, edit);
    push_filter(bank, 5, control_filter_sort, utf8_literal("Sort"), Menu::sort, edit);

    auto& s = p.search;
    p.page_row_count = guild_member_search_page_rows(s, p.filter_sort, {p.member_order, kGuildMemberSearchCapacity},
                                                     {p.page_rows, 100});
    // Members of the page that the member cache lacks join it, so the row
    // menu can weigh roles and hierarchy.
    auto& members = g_app.account_ui->guild_member_store;
    for (u32 i = 0; i < p.page_row_count; ++i) {
        const auto& m = *p.page_rows[i];
        if (!guild_member_store_find(members, p.guild_id, m.user_id))
            guild_member_store_upsert(&members, state(), p.guild_id, m.user_id, {m.nick, m.nick_bytes},
                                      {m.role_ids, minimum<u32>(m.role_count, kGuildModerationMemberRoles)},
                                      m.role_count <= kGuildModerationMemberRoles, GuildMemberStatus::unknown);
    }
    const bool bulk = bulk_allowed();
    if (bulk && p.selected_count) {
        const u32 start = bank.used;
        bank.number(p.selected_count);
        bank.add(p.selected_count == 1 ? utf8_literal(" member selected") : utf8_literal(" members selected"));
        push_choices(control_bulk, bank.since(start), {}, UiSettingsControlKind::buttons, edit, {kBulkButtons, 2});
    }
    if (p.bulk_notice.bytes)
        push_row(control_notice, {}, p.bulk_notice, UiSettingsControlKind::notice, true, false, {},
                 {p.bulk_failed ? UiSettingsVariant::critical : UiSettingsVariant::positive, 0, kUiSettingsNoDefault});
    push_category(control_list_category,
                  default_filters() ? utf8_literal("Recent Members") : utf8_literal("Search Results"));
    if (bulk) {
        u32 actionable, chosen;
        page_selection(&actionable, &chosen);
        push_row(control_select_page, utf8_literal("Select all members on this page"),
                 actionable ? Utf8View{} : utf8_literal("There are no actionable members"),
                 UiSettingsControlKind::toggle, edit && actionable, actionable && chosen == actionable);
    }
    if (!p.page_row_count) {
        const auto phase = s.phase;
        if (phase == GuildMemberSearchPhase::indexing)
            push_loading(utf8_literal("Before searching, we need to index this server. Give us a bit."));
        else if (phase == GuildMemberSearchPhase::loading || phase == GuildMemberSearchPhase::idle)
            push_loading(utf8_literal("Loading members…"));
        else
            push_notice(utf8_literal("No members match these search results."));
    }
    auto& chips = p.chips;
    auto& chip_colors = p.chip_colors;
    p.row_count = 0;
    for (u32 i = 0; i < p.page_row_count && p.row_count < kRowCapacity; ++i) {
        const auto& m = *p.page_rows[i];
        const Utf8View name = m.nick_bytes          ? bank.add({m.nick, m.nick_bytes})
                              : m.global_name_bytes ? bank.add({m.global_name, m.global_name_bytes})
                                                    : bank.add({m.username, m.username_bytes});
        const u32 start = bank.used;
        if (m.username_bytes) {
            bank.add("@");
            bank.add({m.username, m.username_bytes});
            bank.add(" · ");
        }
        bank.add("Member since ");
        relative(bank, m.joined_at_ms, now, false);
        bank.add(" · Joined Discord ");
        relative(bank, (m.user_id >> 22) + kSnowflakeEpochMilliseconds, now, true);
        if (can_manage_guild()) {
            bank.add(" · ");
            bank.add(join_method(bank, m));
        }
        if (m.unusual_dm_activity_until_ms > now)
            bank.add(" · Unusual DM activity");
        if (m.communication_disabled_until_ms > now)
            bank.add(" · Timed out");
        if (m.user_flags & kUserSpammer)
            bank.add(" · Suspected spam");
        if (m.member_flags & kMemberQuarantined)
            bank.add(" · Quarantined");
        const auto description = bank.since(start);
        // The highest role, then "+N" for the others.
        const RoleRecord* top = nullptr;
        for (u32 j = 0; j < minimum<u32>(m.role_count, kGuildModerationMemberRoles); ++j)
            if (const auto* r = role(m.role_ids[j]); r && r->id != p.guild_id) {
                if (!top || r->position > top->position || (r->position == top->position && r->id < top->id))
                    top = r;
            }
        const u32 row = p.row_count;
        u32 chip_count = 0;
        if (top) {
            chips[row][chip_count] = bank.add(chat_state_store_role_name(state(), *top));
            chip_colors[row][chip_count++] = guild_role_color_rgba(top->color);
        }
        if (m.role_count > 1) {
            const u32 more = bank.used;
            bank.add("+");
            bank.number(m.role_count - 1);
            chips[row][chip_count] = bank.since(more);
            chip_colors[row][chip_count++] = 0;
        }
        p.row_ids[row] = m.user_id;
        u8 flags = ui_settings_presentation_avatar | ui_settings_presentation_compact | ui_settings_presentation_accent;
        if (bulk && selected(m.user_id))
            flags |= ui_settings_presentation_selected;
        const u32 index = push_choices(kRowBase + row, name, description, UiSettingsControlKind::action, true,
                                       {chips[row], chip_count}, 0, utf8_literal("More Options"),
                                       {UiSettingsVariant::standard, flags, kUiSettingsNoDefault});
        if (index < kControlCapacity)
            p.avatar_media[index] =
                application_listed_user_avatar_media(m.user_id, {m.avatar, m.avatar_bytes}, m.discriminator);
        p.row_accent[row] = top && top->color ? guild_role_color_rgba(top->color) : person_color(m.user_id);
        ++p.row_count;
    }
    // Footer: the page size and count, then the pages.
    const u32 count = guild_member_search_count(s);
    if (count > kGuildMemberSearchPageSizes[0]) {
        u32 size_index = 0;
        for (u32 i = 0; i < 4; ++i)
            if (kGuildMemberSearchPageSizes[i] == s.page_size)
                size_index = i;
        const u32 start = bank.used;
        bank.add("members of ");
        if (s.phase == GuildMemberSearchPhase::loading)
            bank.add("...");
        else
            bank.number(count);
        push_choices(control_page_size, utf8_literal("Showing"), bank.since(start), UiSettingsControlKind::segments,
                     edit, {kPageSizes, 4}, size_index);
    } else if (p.page_row_count) {
        const u32 start = bank.used;
        bank.add("Showing ");
        bank.number(count);
        bank.add(count == 1 ? utf8_literal(" member") : utf8_literal(" members"));
        push_notice(bank.since(start));
    }
    if (count > s.page_size || s.phase == GuildMemberSearchPhase::loading) {
        const u32 pages = guild_member_search_pages(s);
        const u32 start = bank.used;
        bank.add("Page ");
        bank.number(s.page);
        bank.add(" of ");
        bank.number(pages);
        push_choices(control_pages, bank.since(start), {}, UiSettingsControlKind::buttons,
                     edit && (s.page > 1 || s.page < pages), {kPageButtons, 2});
    }
}

void open_member_menu(u64 user, const UiRect& anchor) noexcept {
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

void open_menu(Menu menu, const UiRect& anchor) noexcept {
    auto& p = *owner;
    Bank bank{p.popup_text, sizeof(p.popup_text), 0};
    u32 count = 0;
    const auto item = [&](const Utf8View& label, bool checked, bool separator = false) noexcept {
        if (count >= kPopupCapacity)
            return;
        popup_row(count, label, count,
                  (checked ? popup_item_checked : 0) | (separator ? popup_item_separator_before : 0));
        ++count;
    };
    switch (menu) {
    case Menu::member_since:
    case Menu::joined_discord: {
        const u8 current = menu == Menu::member_since ? p.filter_joined : p.filter_created;
        for (u32 i = 0; i < kDatePresetCount; ++i)
            item(kDatePresets[i].label, current == i);
        break;
    }
    case Menu::join_method:
        item(utf8_literal("All"), !p.filter_invite_bytes && !p.filter_join_type_set);
        for (u32 i = 0; i < p.invite_count && i < 64; ++i) {
            const Utf8View code{p.invites[i].code, p.invites[i].code_bytes};
            item(bank.add(code), p.filter_invite_bytes == code.bytes && bytes_equal(p.filter_invite, code.data, code.bytes),
                 i == 0);
            p.menu_channel[count - 1] = i;
        }
        for (u32 i = 0; i < sizeof(kJoinTypes) / sizeof(kJoinTypes[0]); ++i) {
            Utf8View label = kJoinTypes[i].label;
            if (kJoinTypes[i].type == 6) {
                const auto* record = guild_details();
                const auto vanity = record ? guild_detail_store_vanity_url_code(details(), *record) : Utf8View{};
                if (vanity.bytes) {
                    const u32 start = bank.used;
                    bank.add(vanity);
                    bank.add(" (Vanity)");
                    label = bank.since(start);
                }
            }
            item(label, p.filter_join_type_set && p.filter_join_type == kJoinTypes[i].type, i == 0);
        }
        break;
    case Menu::member_roles: {
        const RoleRecord* sorted[kGuildRoleMaximum];
        const u32 roles = guild_settings_sorted_roles(state(), p.guild_id, {sorted, kGuildRoleMaximum});
        for (u32 i = 0; i < roles; ++i) {
            if (sorted[i]->id == p.guild_id)
                continue;
            bool on = false;
            for (u32 j = 0; j < p.filter_role_count; ++j)
                on = on || p.filter_roles[j] == sorted[i]->id;
            item(bank.add(chat_state_store_role_name(state(), *sorted[i])), on);
            p.menu_channel[count - 1] = sorted[i]->id;
        }
        break;
    }
    case Menu::signals:
        for (u32 i = 0; i < 4; ++i)
            item(kSignals[i], (p.filter_signals >> i) & 1);
        break;
    case Menu::sort:
        for (u32 i = 0; i < 4; ++i)
            item(kSorts[i], p.filter_sort ? p.filter_sort == i + 1 : i == 0);
        break;
    default: return;
    }
    if (!count)
        return;
    p.menu = menu;
    settings_layer_open_choice(&p.layer, p.guild_id, {p.popup_items, count}, anchor, 280);
}

void activate(const SettingsLayerActivation& activation) noexcept {
    auto& p = *owner;
    const u64 id = activation.value.target.control_id;
    auto& s = p.search;
    if (id >= kRowBase && id < kRowBase + p.row_count) {
        const u64 user = p.row_ids[id - kRowBase];
        // The row's button opens the menu; the row itself selects for a bulk
        // ban when that is allowed.
        if (bulk_allowed() && activation.pointer && !activation.button) {
            if (can_ban(user)) {
                select(user, !selected(user));
                repaint();
            }
            return;
        }
        open_member_menu(user, activation.anchor);
        return;
    }
    switch (id) {
    case control_filter_joined: open_menu(Menu::member_since, activation.anchor); return;
    case control_filter_created: open_menu(Menu::joined_discord, activation.anchor); return;
    case control_filter_method:
        // The menu lists the server's invite codes once they are read.
        if (!p.invites_requested && !busy()) {
            p.invites_requested = true;
            queue(Op::invite_list);
        }
        open_menu(Menu::join_method, activation.anchor);
        return;
    case control_filter_roles: open_menu(Menu::member_roles, activation.anchor); return;
    case control_filter_signals: open_menu(Menu::signals, activation.anchor); return;
    case control_filter_sort: open_menu(Menu::sort, activation.anchor); return;
    case control_page_size:
        if (activation.value.selected_choice < 4) {
            guild_member_search_set_page_size(&s, kGuildMemberSearchPageSizes[activation.value.selected_choice]);
            queue_search();
            repaint();
        }
        return;
    case control_pages: {
        const u32 pages = guild_member_search_pages(s);
        const u32 page = activation.value.selected_choice == 0 ? (s.page > 1 ? s.page - 1 : 1)
                                                               : (s.page < pages ? s.page + 1 : pages);
        if (page != s.page) {
            s.page = page;
            p.scene.content_offset = 0;
            queue_search();
            repaint();
        }
        return;
    }
    case control_select_page: {
        u32 actionable, chosen;
        page_selection(&actionable, &chosen);
        // All selected clears the whole selection; otherwise the page joins it.
        if (actionable && chosen == actionable)
            p.selected_count = 0;
        else
            for (u32 i = 0; i < p.page_row_count; ++i)
                if (can_ban(p.page_rows[i]->user_id))
                    select(p.page_rows[i]->user_id, true);
        repaint();
        return;
    }
    case control_bulk:
        if (activation.value.selected_choice == 1) {
            p.selected_count = 0;
            repaint();
        } else if (p.selected_count && bulk_allowed() &&
                   application_member_moderation_bulk_ban(p.guild_id, {p.selected, p.selected_count})) {
            reset_value(&p.bulk_notice);
            repaint();
        }
        return;
    default: return;
    }
}

void context(const SettingsLayerActivation& activation) noexcept {
    const u64 id = activation.value.target.control_id;
    if (id >= kRowBase && id < kRowBase + owner->row_count)
        open_member_menu(owner->row_ids[id - kRowBase], activation.anchor);
}

void choose(Menu menu, u32 action) noexcept {
    auto& p = *owner;
    if (menu == Menu::member) {
        const u64 user = p.menu_user;
        if (action == kModerationActions.ban) {
            application_member_moderation_start(p.guild_id, user, moderation_action_ban, 0);
            return;
        }
        if (action == kModerationActions.timeout) {
            const u32 count = member_context_timeout_choices(kModerationActions, {p.popup_items, 16});
            p.menu = Menu::timeout;
            settings_layer_open_choice(&p.layer, user, {p.popup_items, count}, g_app.popup_scene.anchor, 240);
            return;
        }
        const u8 bit = action == kModerationActions.nickname      ? moderation_action_nickname
                       : action == kModerationActions.end_timeout ? moderation_action_end_timeout
                       : action == kModerationActions.kick        ? moderation_action_kick
                                                                  : 0;
        if (bit)
            application_member_moderation_start(p.guild_id, user, bit, 0);
        return;
    }
    if (menu == Menu::timeout) {
        const u32 index = action - kModerationActions.choice_first;
        if (action >= kModerationActions.choice_first && index < sizeof(kModerationTimeoutChoices) / sizeof(u32))
            application_member_moderation_start(p.guild_id, p.menu_user, moderation_action_timeout,
                                                kModerationTimeoutChoices[index]);
        return;
    }
    if (action < kMenuChoiceBase)
        return;
    const u32 index = action - kMenuChoiceBase;
    const u64 now = clock_unix_milliseconds();
    switch (menu) {
    case Menu::member_since:
    case Menu::joined_discord:
        if (index < kDatePresetCount) {
            const u64 after = index ? preset_after(now, kDatePresets[index]) : 0;
            if (menu == Menu::member_since) {
                p.filter_joined = static_cast<u8>(index);
                p.filter_joined_after = after;
            } else {
                p.filter_created = static_cast<u8>(index);
                p.filter_created_after = after;
            }
        }
        break;
    case Menu::join_method: {
        // "All", the invite codes, then the other methods. Picking a code
        // clears the type and the reverse; the chosen type again clears it.
        const u32 codes = minimum<u32>(p.invite_count, 64);
        if (index == 0) {
            p.filter_invite_bytes = 0;
            p.filter_join_type_set = false;
        } else if (index <= codes) {
            const auto& invite = p.invites[p.menu_channel[index]];
            p.filter_invite_bytes = invite.code_bytes;
            copy_bytes(p.filter_invite, invite.code, invite.code_bytes);
            p.filter_join_type_set = false;
        } else if (index - codes - 1 < sizeof(kJoinTypes) / sizeof(kJoinTypes[0])) {
            const u8 type = kJoinTypes[index - codes - 1].type;
            const bool same = p.filter_join_type_set && p.filter_join_type == type;
            p.filter_join_type_set = !same;
            p.filter_join_type = same ? 0 : type;
            p.filter_invite_bytes = 0;
        }
        break;
    }
    case Menu::member_roles: {
        if (index >= kPopupCapacity)
            return;
        const u64 role_id = p.menu_channel[index];
        bool removed = false;
        for (u32 i = 0; i < p.filter_role_count; ++i)
            if (p.filter_roles[i] == role_id) {
                for (u32 j = i + 1; j < p.filter_role_count; ++j)
                    p.filter_roles[j - 1] = p.filter_roles[j];
                --p.filter_role_count;
                removed = true;
                break;
            }
        if (!removed && p.filter_role_count < kGuildMemberSearchRoleMaximum)
            p.filter_roles[p.filter_role_count++] = role_id;
        break;
    }
    case Menu::signals:
        if (index < 4)
            p.filter_signals = static_cast<u8>(p.filter_signals ^ (1u << index));
        break;
    case Menu::sort:
        if (index < 4)
            p.filter_sort = static_cast<u8>(index + 1);
        break;
    default: return;
    }
    refilter();
}

bool ensure_data() noexcept {
    auto& p = *owner;
    if (!ensure_bank() || p.search.phase != GuildMemberSearchPhase::idle || p.search_due_at)
        return false;
    p.search_retry = false;
    queue(Op::members_search);
    return true;
}

} // namespace

bool members_search_request(GuildModerationRequest* request, GuildMemberSearchQuery* query) noexcept {
    auto& p = *owner;
    if (!p.search_bank || p.page != Page::members)
        return false;
    GuildMemberSearchQuery q{};
    q.text = {p.search_applied, p.search_applied_bytes};
    q.role_ids = {p.filter_roles, p.filter_role_count};
    q.joined_after_ms = p.filter_joined_after;
    q.created_after_ms = p.filter_created_after;
    // A 202 retry repeats the request it answered, clock included.
    q.now_ms = p.search_retry ? p.search_sent_now : clock_unix_milliseconds();
    q.invite_code = {p.filter_invite, p.filter_invite_bytes};
    q.join_source_type = p.filter_join_type;
    q.join_source_type_set = p.filter_join_type_set;
    q.sort = p.filter_sort;
    q.signals = p.filter_signals;
    guild_member_search_plan(p.search, &q);
    *query = q;
    reset_value(request);
    request->operation = GuildModerationOperation::members_search;
    request->guild_id = p.guild_id;
    request->search = query;
    // An identical body was already asked; a 202 retry sends it again.
    u8 body[kGuildModerationMaximumBodyBytes];
    const auto written = guild_moderation_write_body(*request, {body, sizeof(body)});
    if (!written.ok())
        return false;
    const u64 hash = hash64_counted_bytes(kHash64Seed, body, written.bytes);
    secure_clear_bytes(body, written.bytes);
    if (!p.search_retry && hash == p.search.last_body_hash && written.bytes == p.search.last_body_bytes)
        return false;
    p.search_pending = q;
    p.search_pending_hash = hash;
    p.search_pending_bytes = written.bytes;
    return true;
}

void members_search_sent() noexcept {
    auto& p = *owner;
    if (p.search_retry) {
        p.search_retry = false;
        return;
    }
    if (guild_member_search_begin(&p.search, p.search_pending, p.search_pending_hash, p.search_pending_bytes))
        p.search_sent_now = p.search_pending.now_ms;
}

void members_search_response(const RestClientResponse& response, const Utf8View& body, u64 now) noexcept {
    auto& p = *owner;
    if (!p.search_bank)
        return;
    auto& s = p.search;
    // A superseded search's receipt only clears the lane.
    if (p.search_dirty) {
        p.search_dirty = false;
        p.search_retry = false;
        queue(Op::members_search);
        return;
    }
    if (response.error != RestClientError::none || response.status_code < 200 || response.status_code >= 300) {
        guild_member_search_failed(&s);
        return;
    }
    if (response.status_code == 202) {
        if (guild_member_search_indexing(&s, guild_member_search_retry_after_ms(body, {p.scratch, kScratchUnits}), now)) {
            queue(Op::members_search);
            p.search_retry = true;
            p.retry_at = s.retry_at_ms;
        }
        return;
    }
    // One reply of up to a 500-member chunk is decoded in a bank of its own.
    const u32 rows = guild_member_search_limit(s.page_size);
    const u32 units = body.bytes + 1;
    const SIZE_T rows_bytes = SIZE_T{rows} * sizeof(GuildSearchMember);
    auto* bank = static_cast<u8*>(
        VirtualAlloc(nullptr, rows_bytes + SIZE_T{units} * sizeof(u32), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    GuildMemberSearchPage page{{reinterpret_cast<GuildSearchMember*>(bank), bank ? rows : 0}, 0, 0, false};
    const bool decoded =
        bank && guild_member_search_decode(body, {reinterpret_cast<u32*>(bank + rows_bytes), units}, &page).ok();
    if (decoded)
        guild_member_search_apply(&s, page);
    else
        guild_member_search_failed(&s);
    if (bank)
        VirtualFree(bank, 0, MEM_RELEASE);
}

void members_tick(u64 now) noexcept {
    auto& p = *owner;
    if (p.page != Page::members)
        return;
    // A search held back by another request goes once the lane is free.
    if (p.search_dirty && !busy() && p.search_bank) {
        p.search_dirty = p.search_retry = false;
        queue(Op::members_search);
    }
    const auto text = editor_text(editor_search);
    if (!search_text_changed(text)) {
        p.search_due_at = 0;
        return;
    }
    // Clearing the box applies at once; typing waits for a pause.
    if (!text.bytes || (p.search_due_at && p.search_due_at <= now))
        apply_text();
    else if (!p.search_due_at)
        p.search_due_at = now + kSearchDebounceMs;
}

void members_bulk_ban_update(const GuildBulkBanUpdate& update) noexcept {
    // Either outcome clears the selection; failures alone read as failed.
    if (!owner || update.guild_id != owner->guild_id)
        return;
    owner->selected_count = 0;
    owner->bulk_failed = update.failed_count && !update.banned_count;
    owner->bulk_notice = owner->bulk_failed ? utf8_literal("Mass ban failed") : utf8_literal("Mass ban successful");
}

void members_release() noexcept {
    auto& p = *owner;
    if (p.search_bank) {
        secure_clear_bytes(p.search_bank, sizeof(GuildSearchMember) * kGuildMemberSearchCapacity);
        VirtualFree(p.search_bank, 0, MEM_RELEASE);
    }
    p.search_bank = nullptr;
    reset_value(&p.search);
    p.selected_count = 0;
}

const PageOps kMembersPage{load,    project_members, activate,    choose,  nullptr, nullptr,
                           nullptr, nullptr,         ensure_data, nullptr, nullptr, context};

} // namespace maicord::app_detail::server_settings
