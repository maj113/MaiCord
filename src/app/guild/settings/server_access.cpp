#include "server_access.hpp"
#include "maicord/core/scratch_pages.hpp"

namespace maicord::app_detail::server_settings {
namespace {

bool community_lane(AccessStep step) noexcept {
    switch (step) {
    case AccessStep::verification_get:
    case AccessStep::requirements_get:
    case AccessStep::categories_get:
    case AccessStep::metadata_get:
    case AccessStep::join_requests_get:
    case AccessStep::metadata_update:
    case AccessStep::profile_visibility:
    case AccessStep::subcategory_add:
    case AccessStep::subcategory_remove:
    case AccessStep::verification_update:
    case AccessStep::templates_get:
    case AccessStep::template_create:
    case AccessStep::template_sync:
    case AccessStep::template_update:
    case AccessStep::template_delete: return true;
    default: return false;
    }
}

} // namespace

ServerAccessState* server_access_state() noexcept {
    if (!owner)
        return nullptr;
    auto*& s = owner->server_access;
    if (!s)
        s = static_cast<ServerAccessState*>(commit_pages(sizeof(ServerAccessState)));
    if (s && s->lifetime != owner->lifetime) {
        reset_value(s);
        s->lifetime = owner->lifetime;
    }
    return s;
}

void server_access_queue(AccessStep step) noexcept {
    if (auto* s = server_access_state()) {
        s->step = step;
        queue(Op::server_access);
    }
}

bool server_access_request(GuildAdminRequest* admin, GuildSettingsPatch* patch, RolePatch* role,
                           GuildCommunityRequest* community, bool* lane) noexcept {
    auto* s = owner->server_access;
    if (!s || s->lifetime != owner->lifetime || s->step == AccessStep::none)
        return false;
    const AccessStep step = s->step;
    *lane = community_lane(step);
    community->guild_id = owner->guild_id;
    admin->operation = GuildAdminOperation::guild_update;
    admin->guild = patch;
    switch (step) {
    case AccessStep::everyone_role:
    case AccessStep::community_enable:
    case AccessStep::overview_update:
        if (step == AccessStep::everyone_role) {
            // @everyone's id is the server's.
            admin->operation = GuildAdminOperation::role_update;
            admin->guild = nullptr;
            admin->target_id = owner->guild_id;
            admin->role = role;
        }
        return community_build(step, patch, role);
    case AccessStep::templates_get:
    case AccessStep::template_create:
    case AccessStep::template_sync:
    case AccessStep::template_update:
    case AccessStep::template_delete: return template_build(step, community);
    default: return access_build(step, patch, community);
    }
}

bool server_access_reading() noexcept {
    const auto* s = owner ? owner->server_access : nullptr;
    if (!s)
        return false;
    const AccessStep step = s->step;
    return step == AccessStep::verification_get || step == AccessStep::requirements_get ||
           step == AccessStep::categories_get || step == AccessStep::metadata_get ||
           step == AccessStep::join_requests_get || step == AccessStep::templates_get;
}

void server_access_response(const RestClientResponse& response, const Utf8View& body) noexcept {
    auto* s = owner ? owner->server_access : nullptr;
    if (!s || s->lifetime != owner->lifetime)
        return;
    const AccessStep step = s->step;
    s->step = AccessStep::none;
    if (step >= AccessStep::templates_get)
        template_response(step, response, body);
    else if (step >= AccessStep::everyone_role)
        community_response(step, response, body);
    else if (step != AccessStep::none)
        access_response(step, response, body);
}

void server_access_release() noexcept {
    if (owner && owner->server_access) {
        pages_release(owner->server_access);
        owner->server_access = nullptr;
    }
}

void access_open_channels(AccessMenu menu, u64 current, bool none_row, bool create_row, const UiRect& anchor) noexcept {
    auto& p = *owner;
    auto* s = server_access_state();
    if (!s)
        return;
    Bank bank{p.popup_text, sizeof(p.popup_text), 0};
    u32 count = 0;
    if (none_row) {
        popup_row(count, utf8_literal("None"), count, !current ? popup_item_checked : 0);
        p.menu_channel[count++] = 0;
    }
    if (create_row) {
        popup_row(count, utf8_literal("Create one for me"), count,
                  current == kGuildCommunityNewChannel ? popup_item_checked : 0);
        p.menu_channel[count++] = kGuildCommunityNewChannel;
    }
    const auto& st = state();
    const u32 first = count;
    const u32 listed = sorted_channels(1u << 0, kPopupCapacity - first);
    for (u32 i = 0; i < listed; ++i) {
        const auto* channel = p.popup_rows[i].channel;
        const u32 start = bank.used;
        bank.add("#");
        bank.add(chat_state_store_channel_name(st, *channel));
        popup_row(first + i, bank.since(start), first + i,
                  (channel->id == current ? popup_item_checked : 0) | (i == 0 && first ? popup_item_separator_before : 0));
        p.menu_channel[first + i] = channel->id;
    }
    p.menu = Menu::server_access;
    s->menu = menu;
    settings_layer_open_choice(&p.layer, p.guild_id, {p.popup_items, first + listed}, anchor, 280);
}

void access_open_locales(Utf8View current, const UiRect& anchor) noexcept {
    auto& p = *owner;
    auto* s = server_access_state();
    if (!s)
        return;
    for (u32 i = 0; i < kGuildLocaleCount; ++i) {
        const auto locale = guild_locale(i);
        popup_row(i, locale.name, i, utf8_equal(current, locale.code) ? popup_item_checked : 0);
    }
    p.menu = Menu::server_access;
    s->menu = AccessMenu::locale;
    settings_layer_open_choice(&p.layer, p.guild_id, {p.popup_items, kGuildLocaleCount}, anchor, 240);
}

Utf8View access_channel_label(Bank& bank, u64 channel_id) noexcept {
    if (channel_id == kGuildCommunityNewChannel)
        return utf8_literal("Create one for me");
    return channel_label(bank, channel_id, utf8_literal("None"));
}

Utf8View access_locale_label(Utf8View code) noexcept {
    const u32 index = guild_locale_index(code);
    return index < kGuildLocaleCount ? guild_locale(index).name : utf8_literal("Choose a language");
}

Utf8View access_guild_description() noexcept {
    const auto* record = guild_details();
    return record && (record->known & guild_detail_description)
               ? Utf8View{details().text + record->text_offset, record->description_bytes}
               : Utf8View{};
}

bool access_feature_names(ConstSlice<u8>* names, u16* count) noexcept {
    const auto* record = guild_details();
    if (!record || !record->feature_names_kept || !(record->known & guild_detail_features))
        return false;
    *names = guild_detail_store_feature_names(details(), *record);
    *count = record->feature_count;
    return true;
}

bool access_features_known() noexcept {
    ConstSlice<u8> names{};
    u16 count = 0;
    return access_feature_names(&names, &count);
}

void access_sync_description(GuildCommunityOverview* overview) noexcept {
    overview->description_bytes = static_cast<u16>(
        utf8_copy_truncated(overview->description, sizeof(overview->description), editor_text(editor_description)));
}

void access_choose_locale(GuildCommunityOverview* overview, u32 index) noexcept {
    if (index >= kGuildLocaleCount)
        return;
    const auto code = guild_locale(index).code;
    copy_bytes(overview->locale, code.data, code.bytes);
    overview->locale_bytes = static_cast<u8>(code.bytes);
}

} // namespace maicord::app_detail::server_settings
