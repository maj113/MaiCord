// The people a view shows (message authors, voice participants, member list rows, the Server Hub's members): their
// user and member records stay pinned, the gateway follows them, and missing ones are requested by ID.
#include "application_account_module.hpp"
#include "account_ui_runtime.hpp"
#include "friends.hpp"
#include "../guild/hub.hpp"
#include "../shell/state.hpp"
#include "maicord/app/account/account_module_api.hpp"
#include "maicord/app/chat/visible_author_hydration_plan.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/render/d3d11/renderer_settings_visibility.hpp"

namespace maicord::app_detail {
namespace {
bool author_hydration_visible() noexcept {
    return g_app.account_ui && g_app.account.instance && !g_app.account.stop_failed &&
           !g_app.login.visible && !g_app.startup_shell_provisional && !application_friends_active() &&
           !renderer_settings_hides_shell(g_app.renderer) && g_app.message_scene.active &&
           ui_rect_has_area(g_app.message_scene.viewport);
}
} // namespace

void pin_resident_members(AccountUiRuntime& ui, const MemberListScene& scene, u64 guild_id) noexcept {
    for (u32 i = 0; i < scene.count; ++i)
        if (member_list_scene_row_resident(scene, i) && scene.rows[i].kind == MemberListRowKind::member) {
            user_store_pin(&ui.user_store, scene.rows[i].user_id);
            guild_member_store_pin_user(&ui.guild_member_store, guild_id, scene.rows[i].user_id);
        }
}

u64 application_author_hydration_next_wake() noexcept {
    if (!author_hydration_visible())
        return 0;
    const auto& ui = *g_app.account_ui;
    const auto& owner = ui.author_hydration;
    return ui.active_channel_id == owner.channel_id &&
                   ui.chat_state.selected_channel_id == owner.channel_id &&
                   ui.chat_state.selected_guild_id == owner.guild_id
               ? owner.next_at_ms
               : 0;
}

// Pin epochs seen when the author interest was last applied: any clear of
// member or user pins since (member list events, the thread planner, a
// channel switch) makes the interest reapply its pins.
static u32 g_author_member_pin_epoch;
static u16 g_author_user_pin_epoch;
// The people whose pins were last applied (authors, voice users, list rows).
static u64 g_author_people;

// The guild whose users the gateway is following for this view, and how many (the members key of op 37).
static u64 g_followed_guild;
static u64 g_followed_digest;
static u32 g_followed_count;

// Follows the people shown (authors, voice participants, list rows) in the selected guild so their presence
// and member updates keep arriving, as the official client does for the users it displays, and releases the
// guild followed before. The wire list is the first 50 distinct users other than the current one.
static void follow_visible_members(const AccountUiRuntime& ui, u64 guild_id, const u64* ids, u32 count) noexcept {
    u64 wanted[kGatewayGuildSubscriptionMaximumMembers];
    u32 wanted_count = 0;
    for (u32 i = 0; guild_id && i < count && wanted_count < kGatewayGuildSubscriptionMaximumMembers; ++i) {
        bool duplicate = ids[i] == 0 || ids[i] == ui.chat_state.self_user_id;
        for (u32 j = 0; j < wanted_count && !duplicate; ++j)
            duplicate = wanted[j] == ids[i];
        if (!duplicate)
            wanted[wanted_count++] = ids[i];
    }
    const u64 digest = hash64_bytes(hash64_mix(kHash64Seed, guild_id), wanted,
                                    wanted_count * static_cast<u32>(sizeof(wanted[0])));
    if (guild_id == g_followed_guild && digest == g_followed_digest)
        return;
    const auto send = [&](u64 guild, const u64* users, u32 users_count) noexcept {
        GatewayGuildSubscription frame{};
        frame.guild_id = guild;
        frame.omit_channel_ranges = frame.omit_features = frame.members_present = true;
        frame.member_count = users_count;
        for (u32 i = 0; i < users_count; ++i)
            frame.members[i] = users[i];
        return account_module_guild_subscribe(g_app.account.instance, &frame);
    };
    if (g_followed_guild && g_followed_guild != guild_id && g_followed_count)
        send(g_followed_guild, nullptr, 0);
    if (wanted_count ? !send(guild_id, wanted, wanted_count)
                     : (guild_id == g_followed_guild && g_followed_count && !send(guild_id, nullptr, 0)))
        return;
    g_followed_guild = guild_id;
    g_followed_digest = digest;
    g_followed_count = wanted_count;
}

// The pin and request work, with the large ID scratch. Kept out of line so the per-frame caller
// returns on an unchanged interest without a stack probe.
__declspec(noinline) static void apply_visible_author_interest(AccountUiRuntime& ui, const ChannelRecord* selected,
                                                        bool eligible, const VisibleAuthorInterestKey& key,
                                                        u64 voice_guild, u64 now_ms, bool pins_reset) noexcept {
    auto& owner = ui.author_hydration;
    const auto retain_interest = [&]() noexcept {
        owner.interest = key;
        owner.member_pin_revision = ui.guild_member_store.revision;
        owner.user_pin_revision = ui.user_store.revision;
        owner.voice_revision = ui.voice_states.revision;
        owner.interest_current = true;
        g_author_member_pin_epoch = ui.guild_member_store.user_pin_epoch;
        g_author_user_pin_epoch = ui.user_store.pin_epoch;
    };
    u64 ids[kVisibleAuthorMaximum];
    const u32 author_count =
        eligible ? visible_author_ids(g_app.message_scene, selected->id, {ids, kVisibleAuthorMaximum}) : 0;
    const u32 voice_end = visible_voice_user_ids(ui.voice_states, voice_guild, {ids, kVisibleAuthorMaximum}, author_count);
    // Visible member list rows keep their user while that member's record or
    // profile may have been evicted meanwhile (the list kept its own rows):
    // ask for those members by ID, as the official client does for users it shows.
    u32 count = voice_end;
    const auto& shown = ui.guild_member_store.visible;
    if (selected && shown.guild_id && shown.guild_id == selected->guild_id) {
        u64 rows[128];
        const u32 row_count = guild_member_store_list_users(ui.guild_member_store, shown.guild_id, shown.list_id,
                                                            shown.first, shown.last, {rows, 128});
        for (u32 i = 0; i < row_count && count < kVisibleAuthorMaximum; ++i) {
            if (guild_member_store_find(ui.guild_member_store, shown.guild_id, rows[i]) &&
                user_store_find(ui.user_store, rows[i]))
                continue;
            bool duplicate = false;
            for (u32 j = 0; j < count && !duplicate; ++j)
                duplicate = ids[j] == rows[i];
            if (!duplicate)
                ids[count++] = rows[i];
        }
    }
    // The Server Hub's members: pinned while it shows, asked for when their profile or member record is missing.
    const u32 hub_begin = count;
    if (selected)
        count = application_guild_hub_people(selected->guild_id, ids, count, kVisibleAuthorMaximum);
    // Scrolling changes the interest key every frame, but the people shown
    // change only as rows cross the viewport. With the same people, scope,
    // revisions and pins, the pins applied last time still stand.
    const u64 people =
        hash64_bytes(hash64_mix(kHash64Seed, (u64{author_count} << 32) | voice_end), ids,
                     count * static_cast<u32>(sizeof(ids[0])));
    do {
        if (!pins_reset && people == g_author_people && owner.interest_current &&
            owner.interest.eligible == key.eligible && owner.interest.guild == key.guild &&
            owner.interest.channel == key.channel && owner.voice_revision == ui.voice_states.revision &&
            owner.member_pin_revision == ui.guild_member_store.revision &&
            owner.user_pin_revision == ui.user_store.revision && (!owner.next_at_ms || owner.next_at_ms > now_ms))
            break;
        g_author_people = people;
        follow_visible_members(ui, selected ? selected->guild_id : 0, ids, count);
        u64 missing[kVisibleAuthorMaximum];
        u32 missing_count = 0;
        // Rebuild only finite resident interest, retaining positional range pins.
        // Otherwise scrolling through authors could permanently pin the whole cache.
        guild_member_store_clear_user_pins(&ui.guild_member_store);
        guild_member_store_refresh_visible_users(ui.guild_member_store, &ui.user_store);
        const auto& members = g_app.member_list_scene;
        if (g_app.member_pane_visible && members.active && ui.member_list.thread_mode && selected &&
            ui.member_list.guild_id == selected->guild_id && ui.member_list.list_id == selected->id)
            pin_resident_members(ui, members, selected->guild_id);
        for (u32 i = 0; i < count; ++i) {
            const auto* member =
                guild_member_store_find(ui.guild_member_store, selected->guild_id, ids[i]);
            if (member)
                guild_member_store_pin_user(&ui.guild_member_store, selected->guild_id, ids[i]);
            user_store_pin(&ui.user_store, ids[i]);
            // Authors need their roles (name colour), voice rows and the hub's members their profile and record;
            // list rows were gathered because their record is missing.
            if (i < author_count ? !member || !member->roles_known
                : i < voice_end || i >= hub_begin
                    ? visible_voice_user_missing(ui.user_store, ui.guild_member_store, selected->guild_id, ids[i])
                    : true)
                missing[missing_count++] = ids[i];
        }
        const bool scope_changed =
            eligible && (owner.guild_id != selected->guild_id || owner.channel_id != selected->id);
        if ((!missing_count || scope_changed) && owner.pending_interest) {
            account_module_guild_members_request(g_app.account.instance, nullptr);
            owner.pending_interest = false;
        }
        if (!missing_count) {
            owner.next_at_ms = 0;
            owner.retry_at_ms = 0;
            owner.retry_delay_ms = 0;
            break;
        }
        GatewayGuildMemberRequest request{};
        if (!visible_author_hydration_plan(&owner, selected->guild_id, selected->id, {missing, missing_count},
                                           now_ms, &request))
            break;
        const bool admitted = account_module_guild_members_request(g_app.account.instance, &request);
        visible_author_hydration_complete(&owner, request, now_ms, admitted);
    } while (false);
    retain_interest();
}

void synchronize_visible_author_hydration(u64 now_ms) noexcept {
    // The interest follows people and asks for members through the running account: without one (a logout or
    // account switch while its UI stays, the offline shell) nothing is followed or asked for.
    if (!g_app.account_ui || !g_app.account.instance)
        return;
    auto& ui = *g_app.account_ui;
    const bool pins_reset = g_author_member_pin_epoch != ui.guild_member_store.user_pin_epoch ||
                            g_author_user_pin_epoch != ui.user_store.pin_epoch;
    const auto& owner = ui.author_hydration;
    const auto* selected =
        chat_state_store_find_channel(ui.chat_state, ui.chat_state.selected_channel_id);
    const bool eligible = author_hydration_visible() && selected && selected->guild_id &&
                          ui.active_channel_id == selected->id;
    const auto key = visible_author_interest_key(g_app.message_scene, selected ? selected->guild_id : 0,
                                                 selected ? selected->id : 0, eligible);
    // The selected server's voice participants show in the sidebar whatever
    // the content column holds.
    const u64 voice_guild = selected && selected->guild_id && !g_app.login.visible
                                ? selected->guild_id
                                : 0;
    if (!pins_reset && owner.voice_revision == ui.voice_states.revision &&
        visible_author_hydration_interest_current(owner, key, ui.guild_member_store.revision,
                                                  ui.user_store.revision, now_ms))
        return;
    apply_visible_author_interest(ui, selected, eligible, key, voice_guild, now_ms, pins_reset);
}

void application_author_hydration_reset() noexcept {
    if (g_app.account_ui)
        reset_value(&g_app.account_ui->author_hydration);
    g_followed_guild = g_followed_count = 0;
}

} // namespace maicord::app_detail
