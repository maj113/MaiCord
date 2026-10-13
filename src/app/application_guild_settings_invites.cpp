#include "application_guild_settings_internal.hpp"
#include "maicord/core/time.hpp"

namespace maicord::app_detail::server_settings {
namespace {

void project_invites(Bank& bank) noexcept {
    auto& p = *owner;
    if (p.invites_failed)
        push(notice(utf8_literal("Couldn't load invites"), p.status, utf8_literal("Try again")),
             {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    else if (p.failed && p.status.bytes)
        push(notice({}, p.status), {UiSettingsVariant::critical, 0, kUiSettingsNoDefault});
    else if (!p.invites_loaded)
        push(loading(utf8_literal("Loading invites…")));
    const u32 heading = bank.mark();
    bank.add(utf8_literal("Active invites — "));
    bank.number(p.invite_count);
    push(category(control_list_category, bank.since(heading)));
    if (p.invites_loaded && !p.invite_count)
        push(notice({}, utf8_literal("No active invites.")));
    const u64 now = clock_unix_milliseconds();
    p.row_count = 0;
    for (u32 i = 0; i < p.invite_count; ++i) {
        const auto& invite = p.invites[i];
        const auto code = bank.add({invite.code, invite.code_bytes});
        const u32 start = bank.mark();
        if (const auto* channel = chat_state_store_find_channel(state(), invite.channel_id)) {
            bank.add(channel->type == 2 ? utf8_literal("") : utf8_literal("#"));
            bank.add(chat_state_store_channel_name(state(), *channel));
            bank.add(utf8_literal(" · "));
        }
        const auto inviter = user_label(invite.inviter_id);
        if (inviter.bytes) {
            bank.add(utf8_literal("by "));
            bank.add(inviter);
            bank.add(utf8_literal(" · "));
        }
        bank.number(invite.uses);
        if (invite.max_uses) {
            bank.add(utf8_literal("/"));
            bank.number(invite.max_uses);
        }
        bank.add(invite.uses == 1 && !invite.max_uses ? utf8_literal(" use · ") : utf8_literal(" uses · "));
        if (invite.expires_at_ms) {
            bank.add(utf8_literal("expires "));
            relative_time(bank, invite.expires_at_ms, now, true);
        } else
            bank.add(utf8_literal("never expires"));
        const auto description = bank.since(start);
        push({kRowBase + i, code, description, UiSettingsControlKind::action, editable(), false, {}, 0,
              utf8_literal("Revoke"), nullptr},
             {UiSettingsVariant::standard, ui_settings_presentation_compact, kUiSettingsNoDefault});
        ++p.row_count;
    }
}

void confirm_revoke_invite(u32 index) noexcept {
    if (index >= owner->invite_count)
        return;
    static char title[96];
    Bank bank{title, sizeof(title), 0};
    bank.add(utf8_literal("Revoke invite "));
    bank.add({owner->invites[index].code, owner->invites[index].code_bytes});
    bank.add(utf8_literal("?"));
    // A plain confirmation: revoking sends no reason.
    ApplicationTextFormRequest request{};
    request.title = bank.since(0);
    request.context = utf8_literal("The link stops working for everyone.");
    request.accept = utf8_literal("Revoke");
    request.danger = true;
    request.owner = form_key(form_invite_revoke, index);
    request.valid = form_valid;
    request.save = form_save;
    request.confirmation_only = true;
    application_text_form_open(request);
}

void activate(const SettingsLayerActivation& activation) noexcept {
    auto& p = *owner;
    const u64 id = activation.value.target.control_id;
    if (id >= kRowBase && id < kRowBase + p.invite_count && activation.button && !busy())
        confirm_revoke_invite(static_cast<u32>(id - kRowBase));
}

bool ensure_data() noexcept {
    if (owner->invites_requested)
        return false;
    owner->invites_requested = true;
    owner->invites_failed = false;
    queue(Op::invite_list);
    return true;
}

void notice_action() noexcept {
    if (!owner->invites_failed || busy() || !connected())
        return;
    owner->invites_requested = false;
    ensure_data();
}

} // namespace

const PageOps kInvitesPage{nullptr, project_invites, activate,    nullptr,       nullptr, nullptr,
                           nullptr, nullptr,         ensure_data, notice_action, nullptr};

} // namespace maicord::app_detail::server_settings
