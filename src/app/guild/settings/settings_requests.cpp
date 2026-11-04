#include "internal.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "../../net/rest_rate_retry.hpp"
#include "../../account/account_access.hpp"
#include "maicord/app/gateway/client_dispatch.hpp"
#include "maicord/model/chat/sorted_search.hpp"

namespace maicord::app_detail {

bool application_guild_settings_submit(u64 now) noexcept {
    if (creation.queued && !creation.request_id && (!creation.retry_at || creation.retry_at <= now) &&
        g_app.details_request_owner == MessageHistoryRequestOwner::none && connected()) {
        ChannelPatch patch{};
        patch.fields = channel_patch_name | channel_patch_type | (creation.parent_id ? channel_patch_parent : 0);
        patch.name = {creation.name, creation.name_bytes};
        patch.type = creation.type;
        patch.parent_id = creation.parent_id;
        GuildAdminRequest request{};
        request.operation = GuildAdminOperation::channel_create;
        request.guild_id = creation.guild_id;
        request.channel = &patch;
        const u64 id = application_next_request_id();
        const auto submitted = g_app.account.api.guild_admin(g_app.account.instance, &request, id);
        if (submit_busy(submitted, creation.retry_at, now, 50))
            return false;
        creation.queued = false;
        ++creation.revision;
        if (!submitted.ok()) {
            creation.status = utf8_literal("This channel couldn't be created.");
            application_text_form_synchronize();
            return false;
        }
        creation.request_id = id;
        g_app.details_request_owner = MessageHistoryRequestOwner::channel_overview;
        g_app.details_request_id = id;
        return true;
    }
    if (!owner || !owner->request.queued || owner->request_id || (owner->retry_at && owner->retry_at > now) ||
        g_app.details_request_owner != MessageHistoryRequestOwner::none || !connected() || !scope_current())
        return false;
    auto& p = *owner;
    GuildAdminRequest request{};
    request.guild_id = p.guild_id;
    GuildSettingsPatch guild_patch{};
    GuildServerProfileWrite profile_write{};
    GuildServerProfileTraitWrite profile_traits[kGuildAdminProfileTraits];
    RolePatch role_patch{};
    GuildAdminPageQuery query{};
    GuildAssetRequest asset{};
    asset.guild_id = p.guild_id;
    asset.target_id = p.request.target;
    GuildModerationRequest moderation{};
    GuildMemberSearchQuery search_query{};
    bool moderation_op = false;
    bool assets = true;
    const auto image = [](const Picked& picked) noexcept {
        return picked.set ? ConstSlice<u8>{picked.data, picked.bytes} : ConstSlice<u8>{};
    };
    bool valid = true;
    switch (p.request.op) {
    case Op::role_icon:
        asset.operation = GuildAssetOperation::role_icon;
        asset.image = image(p.role_icon);
        valid = pending(p.role_icon) && asset.target_id;
        break;
    case Op::emoji_create:
        asset.operation = GuildAssetOperation::emoji_create;
        asset.name = {p.asset_name, p.asset_name_bytes};
        asset.image = image(p.upload);
        valid = p.upload.set;
        break;
    case Op::emoji_rename:
        asset.operation = GuildAssetOperation::emoji_update;
        asset.name = {p.asset_name, p.asset_name_bytes};
        break;
    case Op::emoji_delete: asset.operation = GuildAssetOperation::emoji_delete; break;
    case Op::sticker_list: asset.operation = GuildAssetOperation::sticker_list; break;
    case Op::sticker_create:
        asset.operation = GuildAssetOperation::sticker_create;
        asset.name = {p.asset_name, p.asset_name_bytes};
        asset.tags = {p.asset_tags, p.asset_tags_bytes};
        asset.description = {p.asset_description, p.asset_description_bytes};
        asset.image = image(p.upload);
        valid = p.upload.set;
        break;
    case Op::sticker_update:
        asset.operation = GuildAssetOperation::sticker_update;
        asset.fields = p.asset_fields;
        if (p.asset_fields & guild_asset_field_name)
            asset.name = {p.asset_name, p.asset_name_bytes};
        if (p.asset_fields & guild_asset_field_tags)
            asset.tags = {p.asset_tags, p.asset_tags_bytes};
        if (p.asset_fields & guild_asset_field_description)
            asset.description = {p.asset_description, p.asset_description_bytes};
        break;
    case Op::sticker_delete: asset.operation = GuildAssetOperation::sticker_delete; break;
    case Op::mfa:
        asset.operation = GuildAssetOperation::guild_mfa;
        asset.target_id = 0;
        asset.mfa_level = p.safety_draft.mfa_level;
        valid = guild_settings_can_set_mfa(p.access);
        break;
    case Op::guild_delete:
        asset.operation = GuildAssetOperation::guild_delete;
        asset.target_id = 0;
        valid = guild_settings_can_delete(p.access);
        break;
    default: assets = false; break;
    }
    if (assets)
        valid = valid && g_app.account.api.guild_assets && guild_asset_request_valid(asset);
    else if (p.request.op == Op::members_search) {
        // Built from the current filters and page; an unchanged body is not sent.
        moderation_op = true;
        valid = g_app.account.api.guild_moderation && members_search_request(&moderation, &search_query);
    } else if (p.request.op == Op::ban_search) {
        moderation_op = true;
        valid = g_app.account.api.guild_moderation && bans_search_request(&moderation);
    } else switch (p.request.op) {
    case Op::widget_get:
        request.operation = GuildAdminOperation::widget_get;
        break;
    case Op::profile_get:
        request.operation = GuildAdminOperation::profile_get;
        break;
    case Op::engagement_update:
        request.operation = GuildAdminOperation::guild_update;
        valid = can_manage_guild() && engagement_guild_patch(&guild_patch);
        request.guild = &guild_patch;
        break;
    case Op::widget_update:
        request.operation = GuildAdminOperation::widget_update;
        valid = can_manage_guild();
        engagement_widget_patch(&guild_patch);
        request.guild = &guild_patch;
        break;
    case Op::safety_update:
        request.operation = GuildAdminOperation::guild_update;
        valid = guild_safety_patch(p.safety_base, p.safety_draft, &guild_patch);
        request.guild = &guild_patch;
        break;
    case Op::profile_update:
        request.operation = GuildAdminOperation::profile_update;
        valid = can_manage_guild() && profile_patch(&guild_patch, &profile_write, {profile_traits, kGuildAdminProfileTraits});
        request.guild = &guild_patch;
        break;
    case Op::vanity_get:
        request.operation = GuildAdminOperation::vanity_get;
        break;
    case Op::vanity_update:
        request.operation = GuildAdminOperation::vanity_update;
        valid = boost_vanity_patch(&guild_patch);
        request.guild = &guild_patch;
        break;
    case Op::boost_update:
        request.operation = GuildAdminOperation::guild_update;
        valid = can_manage_guild() && boost_guild_patch(&guild_patch);
        request.guild = &guild_patch;
        break;
    case Op::role_counts:
        request.operation = GuildAdminOperation::role_member_counts;
        break;
    case Op::role_create:
        // No patch: the default "new role" body.
        request.operation = GuildAdminOperation::role_create;
        break;
    case Op::role_update:
        request.operation = GuildAdminOperation::role_update;
        request.target_id = p.editing_role;
        valid = guild_role_patch(p.role_base, p.role_draft, &role_patch);
        request.role = &role_patch;
        break;
    case Op::role_delete:
        request.operation = GuildAdminOperation::role_delete;
        request.target_id = p.request.target;
        break;
    case Op::role_reorder: {
        request.operation = GuildAdminOperation::role_reorder;
        const u32 count = resolve_order(p.order_rows);
        const u32 written = guild_settings_role_positions({p.order_rows, count}, p.guild_id,
                                                          {p.positions, kGuildRoleMaximum});
        valid = written != ~0u && written;
        request.positions = {p.positions, written == ~0u ? 0 : written};
        break;
    }
    case Op::ban_list:
        request.operation = GuildAdminOperation::ban_list;
        query.limit = kBanPage;
        query.after = guild_ban_store_next_after(p.bans);
        request.page = &query;
        break;
    case Op::ban_remove:
        request.operation = GuildAdminOperation::ban_remove;
        request.target_id = p.request.target;
        break;
    case Op::invite_list:
        request.operation = GuildAdminOperation::invite_list_guild;
        break;
    case Op::invite_delete:
        // Invite routes are neither guild- nor channel-major.
        request.operation = GuildAdminOperation::invite_delete;
        request.guild_id = 0;
        valid = p.request.target < p.invite_count;
        if (valid)
            request.invite_code = {p.invites[p.request.target].code, p.invites[p.request.target].code_bytes};
        break;
    case Op::audit_page:
        request.operation = GuildAdminOperation::audit_log;
        query.limit = kAuditPage;
        query.before = p.audit_count ? p.audit[p.audit_count - 1].id : 0;
        query.user_id = p.audit_filter_user;
        query.action_type = p.audit_filter_action;
        query.action_type_set = p.audit_filter_action_set;
        request.page = &query;
        break;
    default:
        valid = false;
        break;
    }
    if (!valid) {
        p.request.queued = false;
        if (p.request.op == Op::role_reorder)
            p.order_dirty = false;
        // An image that could not be prepared fails the save visibly.
        if (p.request.op == Op::profile_update || p.request.op == Op::boost_update) {
            p.failed = true;
            p.status = utf8_literal("This image couldn't be prepared.");
        }
        repaint();
        return false;
    }
    const u64 id = application_next_request_id();
    const auto submitted = assets         ? g_app.account.api.guild_assets(g_app.account.instance, &asset, id)
                           : moderation_op ? g_app.account.api.guild_moderation(g_app.account.instance, &moderation, id)
                                           : g_app.account.api.guild_admin(g_app.account.instance, &request, id);
    if (submit_busy(submitted, p.retry_at, now, 50))
        return false;
    if (!submitted.ok()) {
        p.request.queued = false;
        p.failed = true;
        p.status = utf8_literal("This change couldn't be sent.");
        if (p.request.op == Op::ban_list)
            p.bans_failed = true;
        if (p.request.op == Op::members_search) {
            guild_member_search_failed(&p.search);
            p.search_retry = false;
        }
        repaint();
        return false;
    }
    p.request.queued = false;
    p.request_id = id;
    if (p.request.op == Op::members_search)
        members_search_sent();
    g_app.details_request_owner = MessageHistoryRequestOwner::channel_overview;
    g_app.details_request_id = id;
    repaint();
    return true;
}

bool application_guild_settings_owns_response(u64 request_id) noexcept {
    return request_id && ((owner && owner->request_id == request_id) || creation.request_id == request_id);
}

bool application_guild_settings_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!application_guild_settings_owns_response(response.request_id))
        return false;
    if (creation.request_id == response.request_id) {
        creation.request_id = 0;
        ++creation.revision;
        if (response.succeeded())
            creation.done = true;
        else
            creation.status = failure_text(response);
        application_text_form_synchronize();
        return true;
    }
    auto& p = *owner;
    p.request_id = 0;
    const Op op = p.request.op;
    const bool ok = response.succeeded();
    if (!p.scene.active || !scope_current())
        return true;
    if (op == Op::members_search) {
        members_search_response(response, body, clock_monotonic_milliseconds());
        repaint();
        return true;
    }
    // A failed ban search leaves the local filter, as the official page does.
    if (op == Op::ban_search) {
        bans_search_loaded(ok, body);
        repaint();
        return true;
    }
    if (!ok) {
        const u64 retry_at = rest_rate_retry_deadline(response);
        if (retry_at && read_op(op)) {
            p.request.queued = true;
            p.retry_at = retry_at;
            return true;
        }
        p.failed = op != Op::role_counts && op != Op::widget_get && op != Op::profile_get && op != Op::vanity_get;
        p.status = failure_text(response, body);
        if (op == Op::ban_list)
            p.bans_failed = true;
        p.invites_failed = p.invites_failed || op == Op::invite_list;
        p.audit_failed = p.audit_failed || (op == Op::audit_page && !p.audit_count);
        p.stickers_failed = p.stickers_failed || op == Op::sticker_list;
        if (op == Op::widget_get)
            engagement_widget_loaded(nullptr);
        if (op == Op::profile_get)
            server_profile_loaded(nullptr);
        if (op == Op::vanity_get)
            boost_vanity_loaded(nullptr);
        if (op == Op::vanity_update || op == Op::boost_update)
            boost_saved(op, false, body, clock_monotonic_milliseconds());
        if (op == Op::engagement_update || op == Op::widget_update) {
            // The widget PATCH still follows a failed guild PATCH.
            engagement_saved(op, false, clock_monotonic_milliseconds());
            const Op next = next_save_op();
            const Utf8View status = p.status;
            if (op == Op::engagement_update && next == Op::widget_update) {
                queue(next);
                p.failed = true;
                p.status = status;
            }
        }
        if (op == Op::sticker_list)
            p.stickers_loaded = true;
        if (op == Op::emoji_create || op == Op::sticker_create)
            release_picked(&p.upload);
        repaint();
        return true;
    }
    const u64 now = clock_monotonic_milliseconds();
    // A multi-part save continues with its next pending part.
    const auto continue_save = [&]() noexcept {
        const Op next = next_save_op();
        if (next != Op::none && next != op)
            queue(next, next == Op::role_icon ? p.editing_role : 0);
    };
    switch (op) {
    case Op::profile_update:
        profile_saved(true, body);
        break;
    case Op::vanity_update:
    case Op::boost_update:
        boost_saved(op, true, body, now);
        continue_save();
        break;
    case Op::vanity_get: {
        GuildAdminVanity vanity{};
        const bool decoded = guild_admin_decode_vanity(body, {p.scratch, kScratchUnits}, &vanity).ok();
        boost_vanity_loaded(decoded ? &vanity : nullptr);
        break;
    }
    case Op::engagement_update:
    case Op::widget_update:
        engagement_saved(op, true, now);
        continue_save();
        break;
    case Op::widget_get: {
        GuildAdminWidget widget{};
        const bool decoded = guild_admin_decode_widget(body, {p.scratch, kScratchUnits}, &widget).ok();
        engagement_widget_loaded(decoded ? &widget : nullptr);
        ensure_page_data();
        break;
    }
    case Op::profile_get: {
        GuildAdminProfile profile{};
        const bool decoded = guild_admin_decode_profile(body, {p.scratch, kScratchUnits}, &profile).ok();
        server_profile_loaded(decoded ? &profile : nullptr);
        break;
    }
    case Op::role_icon:
        release_picked(&p.role_icon);
        break;
    case Op::safety_update:
        p.safety_base.verification_level = p.safety_draft.verification_level;
        p.safety_base.explicit_content_filter = p.safety_draft.explicit_content_filter;
        p.safety_echo = true;
        p.safety_echo_until = deadline_after(now, kEchoMs);
        continue_save();
        break;
    case Op::mfa:
        p.safety_base.mfa_level = p.safety_draft.mfa_level;
        p.safety_echo = true;
        p.safety_echo_until = deadline_after(now, kEchoMs);
        break;
    case Op::emoji_create:
        // GUILD_EMOJIS_UPDATE refreshes the list.
        release_picked(&p.upload);
        break;
    case Op::sticker_list: {
        const auto decoded = guild_asset_decode_stickers(body, {p.scratch, kScratchUnits}, {p.stickers, kStickerCapacity});
        p.sticker_count = decoded.ok() ? decoded.count : 0;
        p.stickers_loaded = true;
        if (!decoded.ok()) {
            p.failed = true;
            p.status = utf8_literal("The sticker list couldn't be read.");
        }
        break;
    }
    case Op::sticker_create:
    case Op::sticker_update:
    case Op::sticker_delete:
        release_picked(&p.upload);
        if (p.page == Page::stickers)
            queue(Op::sticker_list);
        break;
    case Op::guild_delete:
        application_text_form_cancel_owner(form_valid);
        application_guild_settings_close();
        return true;
    case Op::role_counts: {
        const auto decoded = guild_admin_decode_role_counts(body, {p.scratch, kScratchUnits},
                                                            {p.role_counts, kGuildRoleMaximum});
        p.counts = 0;
        if (decoded.ok())
            for (u32 i = 0; i < decoded.count; ++i) {
                p.count_role[p.counts] = p.role_counts[i].role_id;
                p.count_value[p.counts++] = p.role_counts[i].members;
            }
        break;
    }
    case Op::role_create: {
        GuildAdminRole created{};
        if (guild_admin_decode_role(body, {p.scratch, kScratchUnits}, &created).ok())
            p.open_after_create = created.id;
        break;
    }
    case Op::role_update:
        copy_value(&p.role_base, p.role_draft);
        p.role_echo = true;
        p.role_echo_until = deadline_after(now, kEchoMs);
        continue_save();
        break;
    case Op::role_delete:
        if (p.editing_role == p.request.target)
            p.editing_role = 0;
        break;
    case Op::role_reorder:
        // The gateway republishes positions; keep the draft order until then.
        p.order_dirty = false;
        break;
    case Op::ban_list: {
        // A full page outgrows the shared scratch: its rows and structurals
        // (at most one per body byte) live in a bank for this decode only.
        const u32 units = body.bytes + 1;
        const SIZE_T rows_bytes = SIZE_T{kBanPage} * sizeof(GuildAdminBan);
        auto* bank = static_cast<u8*>(commit_pages(rows_bytes + SIZE_T{units} * sizeof(u32)));
        auto* rows = reinterpret_cast<GuildAdminBan*>(bank);
        const auto decoded =
            bank ? guild_admin_decode_bans(body, {reinterpret_cast<u32*>(bank + rows_bytes), units},
                                           {rows, kBanPage})
                 : GuildAdminResult{0, 0, GuildAdminError::capacity};
        const bool stored =
            decoded.ok() && guild_ban_store_append_page(&p.bans, p.guild_id, {rows, decoded.count},
                                                        decoded.count < kBanPage);
        if (bank)
            pages_release(bank);
        if (!stored) {
            p.bans_failed = true;
            p.status = utf8_literal("The ban list couldn't be read.");
        } else if (!p.bans.complete && p.bans.count < kBanCapacity)
            queue(Op::ban_list);
        else
            p.bans.complete = true;
        break;
    }
    case Op::ban_remove:
        guild_ban_store_apply_remove(&p.bans, p.guild_id, p.request.target);
        for (u32 i = 0; i < p.ban_search_count; ++i)
            if (p.ban_search[i].user.id == p.request.target) {
                copy_value(&p.ban_search[i], p.ban_search[--p.ban_search_count]);
                break;
            }
        break;
    case Op::invite_list: {
        const auto decoded = guild_admin_decode_invites(body, {p.scratch, kScratchUnits}, {p.invites, kInviteCapacity});
        p.invite_count = decoded.ok() ? decoded.count : 0;
        p.invites_loaded = true;
        if (!decoded.ok()) {
            p.failed = true;
            p.status = utf8_literal("The invite list couldn't be read.");
        }
        break;
    }
    case Op::invite_delete:
        if (p.request.target < p.invite_count) {
            sorted_erase_gap(p.invites, p.invite_count, static_cast<u32>(p.request.target));
            --p.invite_count;
        }
        break;
    case Op::audit_page: {
        GuildAdminAuditPage page{{p.audit_page, kAuditPage}, {p.audit_page_users, kAuditPage * 2}, 0, 0};
        if (!guild_admin_decode_audit_log(body, {p.scratch, kScratchUnits}, &page).ok()) {
            p.failed = true;
            p.status = utf8_literal("The audit log couldn't be read.");
            p.audit_complete = true;
            break;
        }
        for (u32 i = 0; i < page.entry_count && p.audit_count < kAuditCapacity; ++i)
            copy_value(&p.audit[p.audit_count++], p.audit_page[i]);
        for (u32 i = 0; i < page.user_count; ++i) {
            bool known = false;
            for (u32 j = 0; j < p.audit_user_count && !known; ++j)
                known = p.audit_users[j].id == p.audit_page_users[i].id;
            if (!known && p.audit_user_count < kAuditUsers)
                copy_value(&p.audit_users[p.audit_user_count++], p.audit_page_users[i]);
        }
        p.audit_complete = page.entry_count < kAuditPage || p.audit_count >= kAuditCapacity;
        break;
    }
    default:
        break;
    }
    repaint();
    return true;
}

void application_guild_settings_gateway(const GatewayClientEvent& event, const Utf8View& body) noexcept {
    if (!owner || event.kind != GatewayClientEventKind::dispatch)
        return;
    if (event.dispatch_kind == GatewayClientDispatchKind::guild_bulk_ban_update) {
        GuildBulkBanUpdate update{};
        if (guild_bulk_ban_update_decode(body, {owner->scratch, kScratchUnits}, &update)) {
            members_bulk_ban_update(update);
            if (owner->scene.active)
                repaint();
        }
        return;
    }
    if (event.dispatch_kind == GatewayClientDispatchKind::guild_stickers_update) {
        // Refetch when the list is on screen; otherwise on the next visit.
        if (event.member_list.guild_id != owner->guild_id || !owner->scene.active)
            return;
        owner->stickers_requested = false;
        if (owner->page == Page::stickers && !owner->request.queued)
            ensure_page_data();
        return;
    }
    if (event.dispatch_kind != GatewayClientDispatchKind::guild_ban_add &&
        event.dispatch_kind != GatewayClientDispatchKind::guild_ban_remove)
        return;
    u64 guild = 0, user = 0;
    Utf8View name{};
    if (!gateway_client_event_guild_membership(event, {reinterpret_cast<const u8*>(body.data), body.bytes}, &guild,
                                               &user, &name) ||
        guild != owner->bans.guild_id)
        return;
    if (event.dispatch_kind == GatewayClientDispatchKind::guild_ban_add)
        guild_ban_store_apply_add(&owner->bans, guild, user, name);
    else
        guild_ban_store_apply_remove(&owner->bans, guild, user);
}

void application_guild_settings_bulk_ban_failed(u64 guild_id) noexcept {
    if (!owner || owner->guild_id != guild_id)
        return;
    owner->selected_count = 0;
    owner->bulk_failed = true;
    owner->bulk_notice = utf8_literal("Mass ban failed");
    if (owner->scene.active)
        repaint();
}

u64 application_guild_settings_caret_wake() noexcept {
    return application_guild_settings_active() ? settings_layer_next_wake(owner->layer) : 0;
}

void application_guild_settings_tick(u64 now) noexcept {
    if (!owner)
        return;
    poll_picker();
    // Soundboard lists and edits complete outside this layer.
    if (application_guild_settings_active() && owner->page == Page::soundboard &&
        owner->soundboard_revision != application_soundboard_revision())
        repaint();
    if (next_form && application_guild_settings_active() && !g_app.text_form && !owner->layer.choice) {
        const u64 kind = next_form, item = next_form_item;
        next_form = next_form_item = 0;
        if (editable())
            open_expression_form(kind, item);
    }
    if (application_guild_settings_active()) {
        members_tick(now);
        if (audit_load_visible())
            repaint();
        settings_layer_tick(&owner->layer, now);
        return;
    }
    // Every page reloads on open, so the closed owner (~2.7 MB of ban, audit,
    // sticker and control banks) is released once no request, file pick or
    // focus reference still reaches it. Open stays blocked while one is in flight.
    if (!owner->request_id && !owner->picker.runtime && !next_form &&
        g_app.context_focus != &owner->scene.widgets)
        application_guild_settings_release();
}

HANDLE application_guild_settings_notification() noexcept {
    return owner && owner->picker.runtime && !owner->picker_consumed ? owner->picker.notification : nullptr;
}

} // namespace maicord::app_detail
