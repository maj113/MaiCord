#include "application_dm_profile.hpp"
#include "application_account_presentation.hpp"
#include "application_content_input.hpp"
#include "application_friends.hpp"
#include "application_guild_card.hpp"
#include "application_link_trust.hpp"
#include "application_profile.hpp"
#include "application_search.hpp"
#include "application_state.hpp"
#include "application_user_actions.hpp"

#include "maicord/core/deadline.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/ui/private_channel_rows.hpp"
#include "maicord/ui/profile_media.hpp"
#include "maicord/ui/scene.hpp"

namespace maicord::app_detail {
namespace {
constexpr u64 kBusyRetryMs = 100;
constexpr u32 kCacheCapacity = 4;

struct CachedProfile {
    UserProfile data;
    u64 user_id, used_ms, loaded_at, failed_at;
    u32 seeded_revision;
    u16 status;
};
struct DmProfilePanel {
    ProfileScene scene;
    CachedProfile cache[kCacheCapacity];
    // Public applications whose icons stand in for missing activity art, as
    // the popout keeps them (application_profile.cpp); the batch in flight.
    ProfileApplication applications[kProfileApplicationCapacity];
    u32 application_count, application_next, asked_count;
    u64 asked[kUserActivityMaximum];
    // The shown user, the profile request in flight (zero: none) and when
    // the profile or the icons may next be asked for.
    u64 shown, requested, retry_at, icons_retry_at;
    bool fetch, icons_requested;
};
DmProfilePanel* g_panel;

u64 now_ms() noexcept {
    return clock_monotonic_milliseconds();
}
bool application_known(u64 id) noexcept {
    for (u32 i = 0; i < g_panel->application_count; ++i)
        if (g_panel->applications[i].id == id)
            return true;
    return false;
}
void remember_application(u64 id, Utf8View icon) noexcept {
    auto& p = *g_panel;
    ProfileApplication* slot = nullptr;
    for (u32 i = 0; i < p.application_count && !slot; ++i)
        if (p.applications[i].id == id)
            slot = &p.applications[i];
    if (!slot) {
        if (p.application_count < kProfileApplicationCapacity)
            slot = &p.applications[p.application_count++];
        else {
            slot = &p.applications[p.application_next];
            p.application_next = (p.application_next + 1) % kProfileApplicationCapacity;
        }
    }
    slot->id = id;
    slot->icon.bytes = static_cast<u16>(icon.bytes <= sizeof(slot->icon.data) ? icon.bytes : 0);
    copy_bytes(slot->icon.data, icon.data, slot->icon.bytes);
    p.scene.applications = {p.applications, p.application_count};
}
// Laid-out cards that would show an application icon not known yet.
bool missing_icons() noexcept {
    const auto& s = g_panel->scene;
    for (u32 i = 0; i < s.missing_icon_count; ++i)
        if (s.missing_icons[i] && !application_known(s.missing_icons[i]))
            return true;
    return false;
}
// Takes over icons the popout already fetched; true when any was new.
bool adopt_popout_icons() noexcept {
    const auto& s = g_panel->scene;
    const auto* p = g_app.profile;
    bool adopted = false;
    for (u32 i = 0; p && i < s.missing_icon_count; ++i) {
        const u64 id = s.missing_icons[i];
        for (u32 k = 0; id && k < p->application_count && !application_known(id); ++k)
            if (p->applications[k].id == id) {
                remember_application(id, p->applications[k].icon.view());
                adopted = true;
            }
    }
    return adopted;
}
bool icons_due(u64 now) noexcept {
    return g_panel->scene.active && !g_panel->requested && !g_panel->icons_requested &&
           g_panel->icons_retry_at <= now && missing_icons();
}
// The scheduler admits the fetch once due (application_scheduler).
void update_wake() noexcept {
    u64 wake = 0;
    if (g_panel && g_panel->scene.active && !g_panel->requested && !g_panel->icons_requested) {
        if (g_panel->fetch)
            wake = maximum<u64>(1, g_panel->retry_at);
        else if (missing_icons())
            wake = maximum<u64>(1, g_panel->icons_retry_at);
    }
    g_app.dm_profile_wake_ms = wake;
}
bool repaint() noexcept {
    return refresh_account_text(true) && (!g_app.window || application_request_frame() != FALSE);
}
u64 selected_peer() noexcept {
    if (!g_app.account_ui)
        return 0;
    const auto& state = g_app.account_ui->chat_state;
    const auto* channel = chat_state_store_find_channel(state, state.selected_channel_id);
    u64 peer[2]{};
    return state.home_selected && channel && channel->type == 1 &&
                   ui_private_row_group_members(state, channel->id, peer)
               ? peer[0]
               : 0;
}
CachedProfile* find(u64 user) noexcept {
    for (auto& entry : g_panel->cache)
        if (entry.user_id == user)
            return &entry;
    return nullptr;
}
// The entry for user, reusing the least recently shown one (never the shown).
CachedProfile* claim(u64 user) noexcept {
    if (auto* entry = find(user))
        return entry;
    CachedProfile* oldest = nullptr;
    for (auto& entry : g_panel->cache)
        if ((!g_panel->shown || entry.user_id != g_panel->shown) && (!oldest || entry.used_ms < oldest->used_ms))
            oldest = &entry;
    if (!oldest)
        return nullptr;
    *oldest = {};
    oldest->user_id = user;
    oldest->data.user_id = user;
    return oldest;
}
bool fresh(const CachedProfile& entry, u64 now) noexcept {
    return !ui_profile_fetch_due(entry.loaded_at, entry.failed_at, now);
}
// What the user store knows before the profile arrives.
void seed(CachedProfile* entry) noexcept {
    auto& users = g_app.account_ui->user_store;
    auto& d = entry->data;
    entry->seeded_revision = users.revision;
    const auto assign = [](auto* to, Utf8View from) noexcept {
        to->bytes = static_cast<u16>(minimum<u32>(sizeof(to->data), from.bytes));
        copy_bytes(to->data, from.data, to->bytes);
    };
    if (const auto* u = user_store_find(users, entry->user_id)) {
        assign(&d.display_name, user_store_name(users, *u));
        assign(&d.username, user_store_username(users, *u));
        assign(&d.avatar, user_store_avatar_hash(*u));
    }
    if (const auto* a = user_appearance_find(users.appearance, entry->user_id))
        user_appearance_expand(*a, &d.user_appearance);
}
// A fresh popout profile of the same user (no server) stands in for a fetch.
bool adopt_popout(CachedProfile* entry, u64 now) noexcept {
    const auto* p = g_app.profile;
    if (!p || p->data.user_id != entry->user_id || p->data.guild_id || ui_profile_fetch_due(p->loaded_at, 0, now))
        return false;
    entry->data = p->data;
    entry->loaded_at = p->loaded_at;
    entry->failed_at = 0;
    entry->status = 200;
    return true;
}
UiPresence presence_of(u64 user) noexcept {
    const auto* relationships = application_friends_relationships();
    const auto* relation = relationships ? relationship_store_find(*relationships, user) : nullptr;
    return relation && relation->type == RelationshipType::friend_
               ? application_friends_presence(relation->presence, UiPresence::offline)
               : UiPresence::unknown;
}
u8 relation_of(u64 user) noexcept {
    u8 relation = 0;
    return application_friends_user_relation(user, &relation) ? relation : 0;
}
UiRect slot_rectangle() noexcept {
    const auto* node = ui_scene_resolve(g_app.shell_scene, ui_scene_find(g_app.shell_scene, UiSemantic::member_sidebar));
    return node ? node->rectangle : UiRect{};
}
void hide() noexcept {
    if (g_app.renderer.dm_profile_panel) {
        g_app.renderer.dm_profile_panel = nullptr;
        application_request_frame();
    }
    if (g_panel && g_panel->scene.active) {
        g_panel->scene.active = false;
        ++g_panel->scene.revision;
    }
    if (g_panel)
        g_panel->shown = 0;
    update_wake();
}
bool show(u64 user, UiRect slot) noexcept {
    if (!g_panel) {
        g_panel = static_cast<DmProfilePanel*>(
            VirtualAlloc(nullptr, sizeof(DmProfilePanel), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!g_panel)
            return false;
    }
    auto& s = g_panel->scene;
    const u64 now = now_ms();
    auto* entry = claim(user);
    if (!entry)
        return false;
    entry->used_ms = now;
    const bool changed = g_panel->shown != user || !s.active;
    // Until the profile loads, the panel shows what the user store knows,
    // including what it learns meanwhile.
    if (!entry->loaded_at && (changed || entry->seeded_revision != g_app.account_ui->user_store.revision))
        seed(entry);
    s.applications = {g_panel->applications, g_panel->application_count};
    if (changed) {
        g_panel->shown = user;
        // Fetched on showing when stale, never on a timer while shown.
        g_panel->fetch = !fresh(*entry, now) && !adopt_popout(entry, now);
        g_panel->retry_at = 0;
        s.presentation = ProfilePresentation::panel;
        s.phase = entry->loaded_at ? ProfilePhase::ready
                  : entry->failed_at ? ProfilePhase::failed
                                     : ProfilePhase::loading;
        s.http_status = entry->status;
        s.presence = presence_of(user);
        s.presence_mobile = ui_presence_mobile(s.presence, user_store_client_status(g_app.account_ui->user_store, user));
        s.relation = relation_of(user);
        s.effect = nullptr;
        s.note_visible = false;
        s.sections = 0;
        BOOL animations = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
        s.reduced_motion = !animations || g_app.preferences.reduced_motion;
        s.measure = &g_app.account_ui->text_measure_cache;
        const auto& state = g_app.account_ui->chat_state;
        if (!profile_scene_open(&s, &entry->data, &state, &g_app.account_ui->user_store, state.self_user_id, slot,
                                application_viewport_dips()))
            return false;
    } else {
        // Friend changes show in the header buttons and the presence dot.
        const u8 relation = relation_of(user);
        const UiPresence presence = presence_of(user);
        const bool moved = !ui_rect_equal(s.anchor, slot) || relation != s.relation || presence != s.presence;
        s.anchor = slot;
        s.relation = relation;
        s.presence = presence;
        if (!profile_scene_layout(&s, application_viewport_dips(), moved))
            return false;
    }
    // Inside the frame: the renderer picks the scene up as it presents.
    g_app.renderer.dm_profile_panel = &s;
    return true;
}
bool open_full(u64 user, UiRect anchor) noexcept {
    return application_profile_open(user, anchor) && (application_profile_expand(user) || true);
}
// Shown activity timers (elapsed, track progress) advance each second. True
// when the scene changed.
bool tick_clock() noexcept {
    auto* s = g_panel && g_panel->scene.active ? &g_panel->scene : nullptr;
    g_app.dm_profile_clock_ms = 0;
    if (!s || !s->activity_timed)
        return false;
    const u64 unix_ms = clock_unix_milliseconds();
    const bool changed = profile_scene_tick(s, unix_ms);
    g_app.dm_profile_clock_ms = now_ms() + (1000 - unix_ms % 1000);
    return changed;
}
bool relayout() noexcept {
    return profile_scene_layout(&g_panel->scene, application_viewport_dips(), true) && repaint();
}
bool activate(const ProfileScene& s, UiWidgetHandle target) noexcept {
    const auto* part = profile_scene_part(s, target);
    if (!part)
        return false;
    const u64 user = s.profile->user_id;
    const auto* node = ui_widget_manager_layout_node(s.widgets, target);
    const UiRect anchor = node ? node->rectangle : s.rectangle;
    switch (part->action) {
    case ProfileAction::expand:
    case ProfileAction::view_bio:
    case ProfileAction::card:
    case ProfileAction::activity:
    case ProfileAction::activity_button:
        return open_full(user, s.rectangle);
    case ProfileAction::servers:
    case ProfileAction::friends:
        g_panel->scene.sections ^= part->action == ProfileAction::servers ? 1u : 2u;
        return relayout();
    case ProfileAction::open_friend:
    case ProfileAction::open_server: {
        const bool friends = part->action == ProfileAction::open_friend;
        const auto& p = *s.profile;
        if (part->source >= (friends ? p.mutual_friends_count : p.mutual_guild_count))
            return false;
        const u64 id = friends ? p.mutual_friends[part->source].id : p.mutual_guilds[part->source].id;
        return friends ? open_full(id, anchor) : select_content_guild(id);
    }
    case ProfileAction::guild_tag:
        return !part->target || application_guild_card_open(part->target, anchor) || true;
    case ProfileAction::badge:
        if (part->source < s.profile->badge_count && s.profile->badges[part->source].link.bytes)
            application_open_content_url(s.profile->badges[part->source].link.view());
        return true;
    case ProfileAction::activity_watch:
        if (s.activity_set && part->source < s.activity_set->count)
            application_link_open(user_activity_text(s.activity_set->items[part->source], UserActivityField::url),
                                  true);
        return true;
    case ProfileAction::add_friend:
    case ProfileAction::accept_friend:
        if (!application_friends_user_action(user, part->action == ProfileAction::add_friend
                                                       ? ApplicationFriendsUserAction::add
                                                       : ApplicationFriendsUserAction::accept))
            log_u64(LogLevel::warning, "profile", "panel_friend_action_refused", user);
        return true;
    case ProfileAction::more:
    case ProfileAction::relationship:
        return g_app.popup_scene.active || g_app.text_form ||
               application_user_menu_open(user, anchor, 0, UserMenuSource::profile);
    case ProfileAction::retry:
        if (auto* entry = find(user)) {
            entry->failed_at = 0;
            g_panel->fetch = true;
            g_panel->retry_at = 0;
            g_panel->scene.phase = ProfilePhase::loading;
            update_wake();
        }
        return relayout();
    default:
        return false;
    }
}
} // namespace

UiSidePanelInput application_side_panel_input() noexcept {
    UiSidePanelInput in{};
    in.members_visible = g_app.member_pane_visible;
    in.profile_visible = g_app.preferences.dm_profile_visible;
    in.member_width = g_app.member_pane_width;
    in.friends = application_friends_active();
    in.window_width = static_cast<float>(g_app.renderer.width) * 96.0f /
                      static_cast<float>(maximum<u32>(g_app.renderer.dpi, 1));
    if (!g_app.account_ui)
        return in;
    const auto& state = g_app.account_ui->chat_state;
    in.home = state.home_selected;
    const auto* channel = chat_state_store_find_channel(state, state.selected_channel_id);
    in.channel_type = channel ? (channel->type ? channel->type : 0xFE) : kUiSidePanelNoChannel;
    return in;
}
void application_dm_profile_synchronize() noexcept {
    const u64 user = selected_peer();
    const UiRect slot = slot_rectangle();
    const bool shown = user && g_app.account_ui && !g_app.login.visible && !application_search_active() &&
                       ui_side_panel_plan(application_side_panel_input()).panel == UiSidePanel::profile &&
                       ui_rect_width(slot) >= kUiDmProfilePanelWidth - 1 && ui_rect_height(slot) > 0;
    if (!shown || !show(user, slot))
        hide();
    else if (adopt_popout_icons())
        relayout();
    update_wake();
    tick_clock();
}
void application_dm_profile_tick(u64 now) noexcept {
    // A second boundary brings no frame of its own.
    if (deadline_due(now, g_app.dm_profile_clock_ms) && tick_clock())
        repaint();
}
ProfileScene* application_dm_profile_scene() noexcept {
    return g_panel && g_panel->scene.active ? &g_panel->scene : nullptr;
}
bool application_dm_profile_pending(u64 now) noexcept {
    return g_panel && g_panel->scene.active && g_app.account.api.profile_get &&
           ((g_panel->fetch && !g_panel->requested && !g_panel->icons_requested && g_panel->retry_at <= now) ||
            icons_due(now));
}
bool application_dm_profile_submit(u64 request_id, u64 now) noexcept {
    if (!application_dm_profile_pending(now))
        return false;
    auto& host = g_app.account;
    const u64 user = g_panel->shown;
    if (!g_panel->fetch || g_panel->retry_at > now) {
        // GET /applications/public for the cards' missing icons, as the popout asks.
        char ids[kUserActivityMaximum * 21];
        u32 used = 0, count = 0;
        const auto& s = g_panel->scene;
        for (u32 i = 0; i < s.missing_icon_count && count < kUserActivityMaximum; ++i) {
            const u64 id = s.missing_icons[i];
            if (!id || application_known(id))
                continue;
            if (used)
                ids[used++] = ',';
            used += decimal_write(ids + used, id);
            g_panel->asked[count++] = id;
        }
        const RestClientSubmitResult result =
            host.api.profile_get(host.instance, user, 0, request_id, 5, {ids, used});
        if (result.error == RestClientSubmitError::busy) {
            g_panel->icons_retry_at = now + kBusyRetryMs;
            update_wake();
            return false;
        }
        if (!result.ok()) {
            // Not retried: the card keeps its placeholder.
            for (u32 i = 0; i < count; ++i)
                remember_application(g_panel->asked[i], {});
            update_wake();
            return false;
        }
        g_panel->asked_count = count;
        g_panel->icons_requested = true;
        update_wake();
        return true;
    }
    // with_mutual_guilds and the mutual friends with their count, as the full profile asks.
    const RestClientSubmitResult result = host.api.profile_get(host.instance, user, 0, request_id, 0, {});
    if (result.error == RestClientSubmitError::busy) {
        g_panel->retry_at = now + kBusyRetryMs;
        update_wake();
        return false;
    }
    g_panel->fetch = false;
    update_wake();
    if (!result.ok()) {
        if (auto* entry = find(user)) {
            entry->failed_at = maximum<u64>(1, now);
            g_panel->scene.phase = entry->loaded_at ? ProfilePhase::ready : ProfilePhase::failed;
        }
        relayout();
        return false;
    }
    g_panel->requested = user;
    update_wake();
    return true;
}
namespace {
void visit_application(void*, u64 id, Utf8View icon) noexcept {
    for (u32 i = 0; i < g_panel->asked_count; ++i)
        if (g_panel->asked[i] == id)
            remember_application(id, icon);
}
bool icons_response(const RestClientResponse& response, Utf8View body) noexcept {
    g_panel->icons_requested = false;
    if (response.error == RestClientError::none && response.status_code == 200)
        user_activity_applications_decode(body, {g_app.account_ui->profile_decode.structurals, 16384},
                                          visit_application, nullptr);
    // Unknown or failed IDs are not asked for again this session; a rate
    // limit tries again later.
    if (response.status_code == 429)
        g_panel->icons_retry_at = now_ms() + 5000;
    else
        for (u32 i = 0; i < g_panel->asked_count; ++i)
            if (!application_known(g_panel->asked[i]))
                remember_application(g_panel->asked[i], {});
    g_panel->asked_count = 0;
    const bool ok = !g_panel->scene.active || relayout();
    update_wake();
    return ok;
}
} // namespace
bool application_dm_profile_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!g_panel || !g_app.account_ui)
        return true;
    if (g_panel->icons_requested)
        return icons_response(response, body);
    if (!g_panel->requested)
        return true;
    const u64 user = g_panel->requested;
    g_panel->requested = 0;
    // The shown user may have changed meanwhile: its own fetch is due now.
    update_wake();
    auto* entry = find(user);
    if (!entry)
        return true;
    const u64 now = maximum<u64>(1, now_ms());
    entry->status = response.status_code;
    const bool ok = response.error == RestClientError::none && response.status_code == 200;
    const bool parsed = ok && g_app.account_ui->profile_decode.decode_into(body, user, 0, entry->data);
    if (parsed) {
        entry->loaded_at = now;
        entry->failed_at = 0;
        auto& users = g_app.account_ui->user_store;
        user_store_upsert(&users, user, entry->data.display_name.view(), entry->data.username.view());
        user_store_set_avatar(&users, user, entry->data.avatar.view());
        user_appearance_upsert(&users.appearance, entry->data.user_appearance);
        ++users.revision;
    } else {
        entry->failed_at = now;
        if (ok)
            log_u64(LogLevel::warning, "profile", "panel_decode_rejected_bytes", body.bytes);
    }
    if (g_panel->shown != user || !g_panel->scene.active)
        return true;
    auto& s = g_panel->scene;
    s.phase = parsed || entry->loaded_at ? ProfilePhase::ready : ProfilePhase::failed;
    s.http_status = response.status_code;
    const bool laid_out = relayout();
    update_wake();
    return laid_out;
}
bool application_dm_profile_hover(UiPoint point) noexcept {
    auto* s = application_dm_profile_scene();
    if (!s)
        return false;
    const bool inside = ui_rect_contains(s->rectangle, point.x, point.y);
    const auto hit = inside ? profile_scene_hit_test(*s, point.x, point.y) : ui_widget_invalid_handle();
    const bool hover_changed = ui_widget_manager_hover(&s->widgets, hit);
    bool changed = hover_changed;
    if (s->hovered != inside) {
        s->hovered = inside;
        s->hover_changed_ms = now_ms();
        changed = true;
    }
    if (changed) {
        ++s->revision;
        repaint();
    }
    return inside;
}
bool application_dm_profile_click(UiPoint point) noexcept {
    auto* s = application_dm_profile_scene();
    if (!s || !ui_rect_contains(s->rectangle, point.x, point.y))
        return false;
    const auto hit = profile_scene_hit_test(*s, point.x, point.y);
    if (hit.generation)
        activate(*s, hit);
    return true;
}
void application_dm_profile_release() noexcept {
    g_app.renderer.dm_profile_panel = nullptr;
    g_app.dm_profile_wake_ms = g_app.dm_profile_clock_ms = 0;
    if (!g_panel)
        return;
    profile_scene_release(&g_panel->scene);
    VirtualFree(g_panel, 0, MEM_RELEASE);
    g_panel = nullptr;
}
} // namespace maicord::app_detail
