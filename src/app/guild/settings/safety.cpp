#include "page_common.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/model/chat/chat_state.hpp"

namespace maicord::app_detail::server_settings {
namespace {

constexpr char kVerificationChoices[] = "None\0Low\0Medium\0High\0Highest\0";
static_assert(name_list_count(kVerificationChoices) == kGuildVerificationLevels);
constexpr char kVerificationDetails[] =
    "Unrestricted.\0"
    "Members need a verified email.\0"
    "Members need a verified email and an account older than 5 minutes.\0"
    "Members also need to have been in the server for 10 minutes.\0"
    "Members need a verified phone number.";
constexpr char kFilterChoices[] =
    "Don't scan any media\0Scan media from members without roles\0Scan media from all members\0";
static_assert(name_list_count(kFilterChoices) == kGuildContentFilterLevels);

void project_safety(Bank& bank) noexcept {
    auto& p = *owner;
    const bool edit = editable() && page_allowed(Page::safety);
    auto& d = p.safety_draft;
    push_category(control_list_category, "Verification level",
                  "Members must meet this before they can send messages or start conversations.");
    push_names(control_verification, "Verification level",
               name_list_at(kVerificationDetails, d.verification_level), UiSettingsControlKind::radio, edit,
               kVerificationChoices, kGuildVerificationLevels, d.verification_level);
    push_category(control_display_category, "Explicit image filter",
                  "Automatically scan and delete explicit images and embeds.");
    push_names(control_content_filter, "Explicit image filter", {}, UiSettingsControlKind::radio, edit,
               kFilterChoices, kGuildContentFilterLevels, d.explicit_content_filter);
    // Community servers set up AutoMod here (the official client routes it into Safety Setup).
    automod_safety_entry(bank);
    push_category(control_delete_category, "Moderation");
    const bool can_mfa = guild_settings_can_set_mfa(p.access);
    const bool self_mfa = state().self_mfa != SelfMfaState::disabled;
    push_toggle(control_mfa, "Require 2FA for moderator actions",
                !can_mfa    ? "Only the server owner can change this."
                : !self_mfa ? "Turn on two-factor authentication for your account first."
                            : "Moderators must have two-factor authentication to ban, kick or delete messages.",
                edit && can_mfa && (self_mfa || d.mfa_level), d.mfa_level != 0);
}

bool patch_pending() noexcept {
    GuildSettingsPatch patch{};
    return guild_safety_patch(owner->safety_base, owner->safety_draft, &patch);
}

void load() noexcept {
    owner->safety_base = owner->safety_draft = guild_safety_from_state(state(), owner->guild_id);
}

void activate(const SettingsLayerActivation& activation) noexcept {
    if (automod_safety_activate(activation))
        return;
    auto& p = *owner;
    const auto target = activation.value.target;
    const u64 id = target.control_id;
    auto& d = p.safety_draft;
    if (id == control_verification && activation.value.selected_choice < kGuildVerificationLevels)
        d.verification_level = static_cast<u8>(activation.value.selected_choice);
    else if (id == control_content_filter && activation.value.selected_choice < kGuildContentFilterLevels)
        d.explicit_content_filter = static_cast<u8>(activation.value.selected_choice);
    else if (id == control_mfa && guild_settings_can_set_mfa(p.access)) {
        d.mfa_level = activation.value.checked ? 1 : 0;
        animate_toggle(activation);
    }
    repaint();
}

bool dirty() noexcept {
    return !guild_safety_equal(owner->safety_base, owner->safety_draft);
}

bool can_save() noexcept {
    return patch_pending() ||
           (owner->safety_draft.mfa_level != owner->safety_base.mfa_level && guild_settings_can_set_mfa(owner->access));
}

Op next_save() noexcept {
    return patch_pending()                                                   ? Op::safety_update
           : owner->safety_draft.mfa_level != owner->safety_base.mfa_level ? Op::mfa
                                                                             : Op::none;
}

bool reconcile(u64 now) noexcept {
    auto& p = *owner;
    bool changed = false;
    const auto current = guild_safety_from_state(state(), p.guild_id);
    if (p.safety_echo) {
        if (guild_safety_equal(current, p.safety_base) || deadline_due(now, p.safety_echo_until))
            p.safety_echo = false;
    } else if (!guild_safety_equal(current, p.safety_base) && !mutation_pending()) {
        // A clean draft follows the store; a dirty one keeps its edits.
        if (!dirty())
            p.safety_draft = current;
        p.safety_base = current;
        changed = true;
    }
    return changed;
}

} // namespace

const PageOps kSafetyPage{load,      project_safety, activate, nullptr, dirty,  can_save,
                          next_save, reconcile,      nullptr,  nullptr, nullptr};

} // namespace maicord::app_detail::server_settings
