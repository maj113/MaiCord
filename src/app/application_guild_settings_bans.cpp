#include "application_guild_settings_internal.hpp"
#include "application_account_presentation.hpp"

namespace maicord::app_detail::server_settings {
namespace {

void project_bans(Bank& bank) noexcept {
    push({control_search, utf8_literal("Search bans"), {}, UiSettingsControlKind::edit, editable(), false, {}, 0, {},
          &editor_view(editor_search, control_search, utf8_literal("Search by name or user ID"), editable())});
    const auto& store = owner->bans;
    if (owner->bans_failed)
        push(notice(utf8_literal("Couldn't load bans"), owner->status, utf8_literal("Try again")),
             {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    else if (!store.complete)
        push(notice({}, utf8_literal("Loading bans…")));
    const auto query = editor_text(editor_search);
    const u32 heading = bank.mark();
    bank.add(utf8_literal("Bans — "));
    bank.number(store.count);
    push(category(control_list_category, bank.since(heading)));
    owner->row_count = 0;
    // View Audit Log alone lists bans without lifting them.
    const bool can_unban = editable() && guild_settings_can_unban(owner->access);
    for (u32 i = 0; i < store.count && owner->row_count < kRowCapacity; ++i) {
        const auto& ban = store.records[i];
        const Utf8View name{ban.name, ban.name_bytes};
        char id_text[24];
        u32 id_bytes = 0;
        for (u64 v = ban.user_id; v && id_bytes < 20; v /= 10)
            id_text[id_bytes++] = static_cast<char>('0' + v % 10);
        for (u32 a = 0, b = id_bytes; a + 1 < b; ++a, --b) {
            const char t = id_text[a];
            id_text[a] = id_text[b - 1];
            id_text[b - 1] = t;
        }
        if (!guild_settings_text_matches(query, name) && !guild_settings_text_matches(query, {id_text, id_bytes}))
            continue;
        owner->row_ids[owner->row_count] = ban.user_id;
        push_avatar(push({kRowBase + owner->row_count, name.bytes ? bank.add(name) : bank.add({id_text, id_bytes}),
                          ban.reason_bytes ? bank.add({ban.reason, ban.reason_bytes}) : utf8_literal("No reason given"),
                          UiSettingsControlKind::action, can_unban, false, {}, 0,
                          can_unban ? utf8_literal("Revoke ban") : Utf8View{}, nullptr},
                         {UiSettingsVariant::standard,
                          static_cast<u8>(ui_settings_presentation_avatar | ui_settings_presentation_compact |
                                          ui_settings_presentation_accent),
                          kUiSettingsNoDefault}),
                    ban.user_id);
        owner->row_accent[owner->row_count] = person_color(ban.user_id);
        ++owner->row_count;
    }
}

void confirm_unban(u64 user) noexcept {
    const auto* ban = guild_ban_store_find(owner->bans, user);
    if (!ban)
        return;
    static char title[200];
    Bank bank{title, sizeof(title), 0};
    bank.add(utf8_literal("Revoke ban for "));
    bank.add(ban->name_bytes ? Utf8View{ban->name, ban->name_bytes} : utf8_literal("this user"));
    // A plain confirmation: lifting a ban sends no reason.
    ApplicationTextFormRequest request{};
    request.title = bank.since(0);
    request.context = utf8_literal("They can join again with an invite.");
    request.accept = utf8_literal("Revoke ban");
    request.owner = form_key(form_unban, user);
    request.valid = form_valid;
    request.save = form_save;
    request.confirmation_only = true;
    application_text_form_open(request);
}

void activate(const SettingsLayerActivation& activation) noexcept {
    auto& p = *owner;
    const u64 id = activation.value.target.control_id;
    if (id >= kRowBase && id < kRowBase + p.row_count && !busy() && guild_settings_can_unban(p.access))
        confirm_unban(p.row_ids[id - kRowBase]);
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

const PageOps kBansPage{nullptr, project_bans, activate,    nullptr,       nullptr, nullptr,
                        nullptr, nullptr,      ensure_data, notice_action, nullptr};

} // namespace maicord::app_detail::server_settings
