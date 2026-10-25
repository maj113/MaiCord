#include "application_guild_settings_internal.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"

namespace maicord::app_detail::server_settings {
namespace {

static constexpr Utf8View kVerificationChoices[]{utf8_literal("None"), utf8_literal("Low"), utf8_literal("Medium"),
                                          utf8_literal("High"), utf8_literal("Highest")};
static_assert(sizeof(kVerificationChoices) / sizeof(kVerificationChoices[0]) == kGuildVerificationLevels);
constexpr Utf8View kVerificationDetails[]{
    utf8_literal("Unrestricted."),
    utf8_literal("Members need a verified email."),
    utf8_literal("Members need a verified email and an account older than 5 minutes."),
    utf8_literal("Members also need to have been in the server for 10 minutes."),
    utf8_literal("Members need a verified phone number.")};
constexpr Utf8View kFilterChoices[]{utf8_literal("Don't scan any media"),
                                    utf8_literal("Scan media from members without roles"),
                                    utf8_literal("Scan media from all members")};
static_assert(sizeof(kFilterChoices) / sizeof(kFilterChoices[0]) == kGuildContentFilterLevels);

void project_safety(Bank&) noexcept {
    auto& p = *owner;
    const bool edit = editable() && page_allowed(Page::safety);
    auto& d = p.safety_draft;
    push(category(control_list_category, utf8_literal("Verification level"),
                  utf8_literal("Members must meet this before they can send messages or start conversations.")));
    push({control_verification, utf8_literal("Verification level"), kVerificationDetails[d.verification_level],
          UiSettingsControlKind::radio, edit, false, {kVerificationChoices, kGuildVerificationLevels},
          d.verification_level, {}, nullptr});
    push(category(control_display_category, utf8_literal("Explicit image filter"),
                  utf8_literal("Automatically scan and delete explicit images and embeds.")));
    push({control_content_filter, utf8_literal("Explicit image filter"), {}, UiSettingsControlKind::radio, edit,
          false, {kFilterChoices, kGuildContentFilterLevels}, d.explicit_content_filter, {}, nullptr});
    push(category(control_delete_category, utf8_literal("Moderation")));
    const bool can_mfa = guild_settings_can_set_mfa(p.access);
    const bool self_mfa = state().self_mfa != SelfMfaState::disabled;
    push({control_mfa, utf8_literal("Require 2FA for moderator actions"),
          !can_mfa    ? utf8_literal("Only the server owner can change this.")
          : !self_mfa ? utf8_literal("Turn on two-factor authentication for your account first.")
                      : utf8_literal("Moderators must have two-factor authentication to ban, kick or delete messages."),
          UiSettingsControlKind::toggle, edit && can_mfa && (self_mfa || d.mfa_level), d.mfa_level != 0, {}, 0, {},
          nullptr});
}

bool patch_pending() noexcept {
    GuildSettingsPatch patch{};
    return guild_safety_patch(owner->safety_base, owner->safety_draft, &patch);
}

void load() noexcept {
    owner->safety_base = owner->safety_draft = guild_safety_from_state(state(), owner->guild_id);
}

void activate(const SettingsLayerActivation& activation) noexcept {
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
        ui_settings_scene_animate_toggle(&p.scene, target, activation.value.checked,
                                         clock_monotonic_milliseconds());
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
