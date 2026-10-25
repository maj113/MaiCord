#include "application_guild_settings_internal.hpp"
#include "maicord/core/memory.hpp"
#include "application_settings.hpp"

namespace maicord::app_detail::server_settings {
namespace {

constexpr u32 kPieces = 64;

// The search box split like the official parser: comma pieces, trimmed;
// 17-19 digit pieces are IDs and the first other piece is the text.
struct BanQuery {
    u64 ids[kPieces];
    u32 id_count;
    Utf8View text;
};
bool space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}
Utf8View trim(Utf8View text) noexcept {
    u32 start = 0, end = text.bytes;
    while (start < end && space(text.data[start]))
        ++start;
    while (end > start && space(text.data[end - 1]))
        --end;
    return {text.data + start, end - start};
}
BanQuery parse(Utf8View text) noexcept {
    BanQuery query{};
    bool text_seen = false;
    u32 start = 0;
    for (u32 i = 0; i <= text.bytes; ++i) {
        if (i < text.bytes && text.data[i] != ',')
            continue;
        const Utf8View piece = trim({text.data + start, i - start});
        start = i + 1;
        if (guild_member_search_snowflake(piece)) {
            u64 id = 0;
            for (u32 j = 0; j < piece.bytes; ++j)
                id = id * 10 + static_cast<u64>(piece.data[j] - '0');
            if (query.id_count < kPieces)
                query.ids[query.id_count++] = id;
        } else if (!text_seen) {
            text_seen = true;
            query.text = piece;
        }
    }
    return query;
}
bool matches(const BanQuery& query, u64 user_id, Utf8View name) noexcept {
    for (u32 i = 0; i < query.id_count; ++i)
        if (query.ids[i] == user_id)
            return true;
    return query.text.bytes && guild_settings_text_matches(query.text, name);
}

void push_row(Bank& bank, u64 user_id, Utf8View name, Utf8View reason, bool can_unban, Utf8View avatar,
              u16 discriminator) noexcept {
    auto& p = *owner;
    char id_text[24];
    Bank id{id_text, sizeof(id_text), 0};
    id.number(user_id);
    p.row_ids[p.row_count] = user_id;
    // Members who can lift bans open the ban's details; others read its reason here.
    const u32 index = push({kRowBase + p.row_count, name.bytes ? bank.add(name) : bank.add(id.since(0)),
                            can_unban           ? Utf8View{}
                            : reason.bytes      ? bank.add(reason)
                                                : utf8_literal("No reason provided"),
                            UiSettingsControlKind::action, can_unban, false, {}, 0, {}, nullptr},
                           {UiSettingsVariant::standard,
                            static_cast<u8>(ui_settings_presentation_avatar | ui_settings_presentation_compact |
                                            ui_settings_presentation_accent),
                            kUiSettingsNoDefault});
    if (index < kControlCapacity)
        p.avatar_media[index] = avatar.bytes || discriminator
                                    ? application_listed_user_avatar_media(user_id, avatar, discriminator)
                                    : application_user_avatar_media(user_id);
    p.row_accent[p.row_count] = person_color(user_id);
    ++p.row_count;
}

void project_bans(Bank& bank) noexcept {
    auto& p = *owner;
    push(category(control_list_category, utf8_literal("Server Ban List"),
                  utf8_literal("Bans by default are by account and IP. A user can circumvent an IP ban by using a "
                               "proxy. Ban circumvention can be made very hard by enabling phone verification in "
                               "Safety Setup.")));
    // The search field with its Search button beside it.
    push({control_search, {}, {}, UiSettingsControlKind::edit, editable(), false, {}, 0, utf8_literal("Search"),
          &editor_view(editor_search, control_search, utf8_literal("Search Bans by User Id or Username"),
                       editable())});
    const bool searching = busy() && p.request.op == Op::ban_search;
    const auto& store = p.bans;
    if (p.bans_failed)
        push(notice(utf8_literal("Couldn't load bans"), p.status, utf8_literal("Try again")),
             {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    else if (!store.complete)
        push(loading(utf8_literal("Loading bans…")));
    else if (searching)
        push(loading(utf8_literal("Searching bans…")));
    // Typing filters the loaded bans at once; Enter or Search also asks the server.
    const Utf8View typed = trim(editor_text(editor_search));
    const BanQuery query = parse(typed);
    const bool filtered = typed.bytes != 0;
    p.row_count = 0;
    const bool can_unban = editable() && guild_settings_can_unban(p.access);
    for (u32 i = 0; i < store.count && p.row_count < kRowCapacity; ++i) {
        const auto& ban = store.records[i];
        const Utf8View name{ban.name, ban.name_bytes};
        if (filtered && !matches(query, ban.user_id, name))
            continue;
        push_row(bank, ban.user_id, name, {ban.reason, ban.reason_bytes}, can_unban, {}, 0);
    }
    for (u32 i = 0; filtered && i < p.ban_search_count && p.row_count < kRowCapacity; ++i) {
        const auto& ban = p.ban_search[i];
        if (guild_ban_store_find(store, ban.user.id) || !matches(query, ban.user.id, user_name(ban.user)))
            continue;
        push_row(bank, ban.user.id, user_name(ban.user), {ban.reason, ban.reason_bytes}, can_unban,
                 {ban.user.avatar, ban.user.avatar_bytes}, ban.user.discriminator);
    }
    if (store.complete && !p.row_count)
        push(notice(utf8_literal("No Bans"),
                    utf8_literal("You haven't banned anybody... but if and when you must, do not hesitate!")));
}

// The ban's details, as the official dialog shows them: its reason, Revoke
// Ban and Done.
void open_ban(u64 user) noexcept {
    auto& p = *owner;
    Utf8View name{}, reason{};
    if (const auto* ban = guild_ban_store_find(p.bans, user)) {
        name = {ban->name, ban->name_bytes};
        reason = {ban->reason, ban->reason_bytes};
    } else {
        for (u32 i = 0; i < p.ban_search_count; ++i)
            if (p.ban_search[i].user.id == user) {
                name = user_name(p.ban_search[i].user);
                reason = {p.ban_search[i].reason, p.ban_search[i].reason_bytes};
            }
    }
    static char title[200], context[640];
    Bank title_bank{title, sizeof(title), 0};
    title_bank.add(name.bytes ? name : utf8_literal("Banned user"));
    Bank context_bank{context, sizeof(context), 0};
    context_bank.add(utf8_literal("Ban Reason\n"));
    context_bank.add(reason.bytes ? reason : utf8_literal("No reason provided"));
    // A plain confirmation: lifting a ban sends no reason.
    ApplicationTextFormRequest request{};
    request.title = title_bank.since(0);
    request.context = context_bank.since(0);
    request.accept = utf8_literal("Revoke Ban");
    request.cancel = utf8_literal("Done");
    request.owner = form_key(form_unban, user);
    request.valid = form_valid;
    request.save = form_save;
    request.confirmation_only = true;
    request.danger = true;
    application_text_form_open(request);
}

void search() noexcept {
    auto& p = *owner;
    const auto text = trim(editor_text(editor_search));
    if (!text.bytes || busy() || !connected())
        return;
    p.ban_search_bytes = minimum(text.bytes, kSearchBytes);
    copy_bytes(p.ban_search_text, text.data, p.ban_search_bytes);
    queue(Op::ban_search);
    repaint();
}

void activate(const SettingsLayerActivation& activation) noexcept {
    auto& p = *owner;
    const u64 id = activation.value.target.control_id;
    if (id == control_search) {
        search();
        return;
    }
    if (id >= kRowBase && id < kRowBase + p.row_count && !busy() && guild_settings_can_unban(p.access))
        open_ban(p.row_ids[id - kRowBase]);
}

bool ensure_data() noexcept {
    if (owner->bans_requested)
        return false;
    owner->bans_requested = true;
    owner->bans_failed = false;
    queue(Op::ban_list, 0);
    return true;
}

void notice_action() noexcept {
    auto& p = *owner;
    if (!p.bans_failed)
        return;
    p.bans_requested = false;
    guild_ban_store_reset(&p.bans, p.guild_id);
    if (!busy() && connected())
        ensure_data();
}

} // namespace

void bans_editor_enter() noexcept {
    if (owner->page == Page::bans)
        search();
}

bool bans_search_request(GuildModerationRequest* request) noexcept {
    auto& p = *owner;
    if (!p.ban_search_bytes)
        return false;
    reset_value(request);
    request->operation = GuildModerationOperation::ban_search;
    request->guild_id = p.guild_id;
    request->query = {p.ban_search_text, p.ban_search_bytes};
    return guild_moderation_request_valid(*request);
}

void bans_search_loaded(bool ok, Utf8View body) noexcept {
    auto& p = *owner;
    if (!ok)
        return;
    const auto decoded =
        guild_admin_decode_bans(body, {p.scratch, kScratchUnits}, {p.ban_search, kGuildBanSearchLimit});
    p.ban_search_count = decoded.ok() ? decoded.count : 0;
}

const PageOps kBansPage{nullptr, project_bans, activate,    nullptr,       nullptr, nullptr,
                        nullptr, nullptr,      ensure_data, notice_action, nullptr};

} // namespace maicord::app_detail::server_settings
