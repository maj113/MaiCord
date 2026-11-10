#include "page_common.hpp"
#include "../../shell/toast.hpp"
#include "maicord/core/time.hpp"
#include "../../settings/streamer_mode.hpp"

namespace maicord::app_detail::server_settings {
namespace {
// Each row: Copy Link for everyone, Revoke for managers.
constexpr char kInviteActions[] = "Copy Link\0Revoke";
constexpr u32 kInviteCopy = 0, kInviteRevoke = 1;
// The code, or its mask while Streamer Mode hides invite links (Copy Link still copies it).
Utf8View invite_code(const GuildAdminInvite& invite) noexcept {
    return application_streamer_hide_invites() ? utf8_literal("\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2")
                                               : Utf8View{invite.code, invite.code_bytes};
}

void project_invites(Bank& bank) noexcept {
    auto& p = *owner;
    if (p.invites_failed)
        push_error("Couldn't load invites", p.status, "Try again");
    else if (p.failed && p.status.bytes)
        push_error({}, p.status);
    else if (!p.invites_loaded)
        push_loading("Loading invites…");
    push_count_heading(bank, "Active invites — ", p.invite_count);
    if (p.invites_loaded && !p.invite_count)
        push_notice("No active invites.");
    const u64 now = clock_unix_milliseconds();
    for (u32 i = 0; i < p.invite_count; ++i) {
        const auto& invite = p.invites[i];
        const auto code = bank.add(invite_code(invite));
        const u32 start = bank.used;
        if (const auto* channel = chat_state_store_find_channel(state(), invite.channel_id)) {
            bank.add(channel->type == 2 ? utf8_literal("") : utf8_literal("#"));
            bank.add(chat_state_store_channel_name(state(), *channel));
            bank.add(" · ");
        }
        const auto inviter = user_label(invite.inviter_id);
        if (inviter.bytes) {
            bank.add("by ");
            bank.add(inviter);
            bank.add(" · ");
        }
        bank.number(invite.uses);
        if (invite.max_uses) {
            bank.add("/");
            bank.number(invite.max_uses);
        }
        bank.add(text_view(invite.uses == 1 && !invite.max_uses ? " use · " : " uses · "));
        if (invite.expires_at_ms) {
            bank.add("expires ");
            relative_time(bank, invite.expires_at_ms, now, true);
        } else
            bank.add("never expires");
        const auto description = bank.since(start);
        push_buttons(kRowBase + i, code, description, true, kInviteActions, editable() ? 2u : 1u,
                     {UiSettingsVariant::standard, ui_settings_presentation_compact, kUiSettingsNoDefault});
    }
}

void confirm_revoke_invite(u32 index) noexcept {
    if (index >= owner->invite_count)
        return;
    static char title[96];
    Bank bank{title, sizeof(title), 0};
    bank.add("Revoke invite ");
    bank.add(invite_code(owner->invites[index]));
    bank.add("?");
    // A plain confirmation: revoking sends no reason.
    open_confirmation(form_invite_revoke, index, bank.since(0), "The link stops working for everyone.", "Revoke",
                      form_save);
}

// The invite's full link, as the Invite People modal copies it.
bool copy_invite(u32 index) noexcept {
    const auto& invite = owner->invites[index];
    char link[96];
    Bank bank{link, sizeof(link), 0};
    bank.add("https://discord.gg/");
    bank.add({invite.code, invite.code_bytes});
    return application_copy_text(bank.since(0), kToastInviteCopied, kToastLinkCopyFailed);
}

void activate(const SettingsLayerActivation& activation) noexcept {
    auto& p = *owner;
    const u64 id = activation.value.target.control_id;
    if (id < kRowBase || id >= kRowBase + p.invite_count)
        return;
    const u32 index = static_cast<u32>(id - kRowBase), choice = activation.value.selected_choice;
    if (choice == kInviteCopy)
        copy_invite(index);
    else if (choice == kInviteRevoke && editable() && !busy())
        confirm_revoke_invite(index);
}

bool ensure_data() noexcept {
    return request_read(owner->invites_requested, owner->invites_failed, Op::invite_list);
}

void notice_action() noexcept {
    retry_read(owner->invites_requested, owner->invites_failed, Op::invite_list);
}

} // namespace

const PageOps kInvitesPage{nullptr, project_invites, activate,    nullptr,       nullptr, nullptr,
                           nullptr, nullptr,         ensure_data, notice_action, nullptr};

} // namespace maicord::app_detail::server_settings
