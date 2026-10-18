#include "application_guild_community.hpp"
#include "application_invite_cards.hpp"
#include "application_account_presentation.hpp"
#include "application_content_input.hpp"
#include "application_guild_invite.hpp"
#include "application_state.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/invite_card_cache.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace {

constexpr u64 kBusyRetryMs = 250;

struct Owner {
    InviteCardCache cache;
    u32 decode_scratch[8192];
    u64 account_scope;
    u64 request_id;
    u64 retry_at_ms;
    u32 published_revision;
    char code[kGuildInviteCodeMaximumBytes];
    u8 code_bytes;
};
Owner* owner{};

bool visible_invites(bool touch, u64 now_ms) noexcept {
    auto& scene = g_app.message_scene;
    if (!scene.active || !g_app.account_ui)
        return false;
    const u32 reset_generation = g_app.message_view.timeline.reset_generation;
    if (scene.invite_scan_reset_generation != reset_generation) {
        for (u32 i = 0; i < scene.count; ++i)
            scene.rows[i].invite_scan_negative = false;
        scene.invite_scan_reset_generation = reset_generation;
    }
    bool found = false;
    for (u32 i = 0; i < scene.count; ++i) {
        auto& row = scene.rows[i];
        const auto* record = message_store_resolve(g_app.message_view.store, row.message);
        if (!record || record->content_availability != MessageContentAvailability::available ||
            record->content_bytes < 12) {
            row.invite_scan_negative = false;
            continue;
        }
        // The store resolves the exact slot/generation. A stale scene row must
        // also match the ID and revision before it can reuse a negative result.
        const bool current = row.message_id == record->id && row.message_revision == record->revision &&
                             record->revision != ~u32{0};
        if (current && row.invite_scan_negative)
            continue;
        InviteLink links[kMessageInviteCardMaximum];
        const u32 count = message_markdown_find_invites(message_store_content(g_app.message_view.store, row.message),
                                                        {links, kMessageInviteCardMaximum});
        row.invite_scan_negative = current && count == 0;
        found = found || count != 0;
        for (u32 l = 0; touch && owner && l < count; ++l) {
            // Event links read the scheduled events store; unknown ones are fetched.
            if (invite_link_event(links[l]))
                application_guild_community_want_event(links[l].event_guild_id, links[l].event_id);
            else
                invite_card_cache_touch(&owner->cache, {links[l].code, links[l].code_bytes}, now_ms);
        }
    }
    return found;
}

} // namespace

void application_invite_cards_synchronize(u64 now_ms) noexcept {
    if (!g_app.account_ui || !g_app.message_scene.active)
        return;
    if (owner && owner->account_scope != application_account_scope()) {
        // A different account never sees another account's resolutions.
        invite_card_cache_clear(&owner->cache);
        owner->account_scope = application_account_scope();
        owner->request_id = 0;
    }
    if (!owner) {
        // Allocated on the first invite link on screen.
        if (!visible_invites(false, now_ms))
            return;
        owner = static_cast<Owner*>(VirtualAlloc(nullptr, sizeof(Owner), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!owner)
            return;
        owner->account_scope = application_account_scope();
        message_scene_bind_invite_cards(&g_app.message_scene, &owner->cache);
    }
    if (g_app.message_scene.invite_cards != &owner->cache)
        message_scene_bind_invite_cards(&g_app.message_scene, &owner->cache);
    if (message_scene_bind_scheduled_events(&g_app.message_scene, application_guild_community_events()))
        refresh_account_text(true);
    visible_invites(true, now_ms);
    for (u32 i = 0; i < kInviteCardCacheEntries; ++i) {
        const auto& entry = owner->cache.entries[i];
        if (entry.invite.guild_id)
            invite_card_cache_set_joined(&owner->cache, i,
                                         chat_state_store_find_guild(g_app.account_ui->chat_state,
                                                                     entry.invite.guild_id) != nullptr);
    }
    if (owner->published_revision != owner->cache.revision) {
        owner->published_revision = owner->cache.revision;
        refresh_account_text(true);
        application_request_frame();
    }
}

bool application_invite_card_click(u64 message_id, UiWidgetHandle widget, float x, float y) noexcept {
    if (!owner || !g_app.account_ui)
        return false;
    const auto& scene = g_app.message_scene;
    for (u32 r = 0; r < scene.count; ++r) {
        const auto& row = scene.rows[r];
        if (row.message_id != message_id)
            continue;
        const auto* record = message_store_resolve(g_app.message_view.store, row.message);
        InviteLink links[kMessageInviteCardMaximum];
        const u32 count = record ? message_scene_invite_codes(scene, *record, {links, kMessageInviteCardMaximum}) : 0;
        for (u32 i = 0; i < count && i < row.invite_item_count; ++i) {
            const auto item = message_scene_rich_item_widget(row, MessageSceneRichItemKind::invite, i);
            if (!item.generation || item.slot != widget.slot || item.generation != widget.generation)
                continue;
            const auto* node = message_scene_rich_item_layout_node(scene, row, MessageSceneRichItemKind::invite, i);
            if (!node)
                return true;
            const auto geometry = message_invite_card_geometry(node->rectangle);
            if (invite_link_event(links[i])) {
                // The whole event card opens the event.
                return application_guild_community_open_event(links[i].event_guild_id, links[i].event_id) || true;
            }
            if (!ui_rect_contains(geometry.button, x, y))
                return true; // the card body is inert, like the official client's
            const Utf8View code{links[i].code, links[i].code_bytes};
            const auto* entry = invite_card_cache_find(owner->cache, code);
            if (!entry || !entry->invite.guild_id)
                return true;
            if (entry->joined)
                return select_content_guild(entry->invite.guild_id);
            return application_guild_invite_open(code) || true;
        }
    }
    return false;
}

bool application_invite_cards_pending(u64 now_ms) noexcept {
    return owner && !owner->request_id && owner->retry_at_ms <= now_ms && g_app.account_ui &&
           g_app.account.api.guild_invite && invite_card_cache_next(owner->cache) != nullptr;
}

bool application_invite_cards_submit(u64 request_id, u64 now_ms) noexcept {
    if (!application_invite_cards_pending(now_ms))
        return false;
    const InviteCardEntry* entry = invite_card_cache_next(owner->cache);
    GuildInviteRequest request{};
    copy_bytes(request.code, entry->code, entry->code_bytes);
    request.code_bytes = entry->code_bytes;
    request.operation = GuildInviteOperation::resolve;
    const Utf8View code{entry->code, entry->code_bytes};
    const auto submitted = g_app.account.api.guild_invite(g_app.account.instance, &request, request_id);
    if (!submitted.ok()) {
        owner->retry_at_ms = now_ms + kBusyRetryMs;
        if (submitted.error != RestClientSubmitError::busy &&
            invite_card_cache_begin(&owner->cache, code))
            invite_card_cache_complete(&owner->cache, code, GuildInviteOutcome::unavailable, nullptr, now_ms);
        return false;
    }
    invite_card_cache_begin(&owner->cache, code);
    copy_bytes(owner->code, entry->code, entry->code_bytes);
    owner->code_bytes = entry->code_bytes;
    owner->request_id = request_id;
    return true;
}

bool application_invite_cards_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!owner || !owner->request_id || response.request_id != owner->request_id)
        return true;
    owner->request_id = 0;
    GuildInvite decoded{};
    const auto outcome =
        response.transport_ok()
            ? guild_invite_decode_response(response.status_code, body, {owner->decode_scratch, 8192}, &decoded)
            : GuildInviteOutcome::unavailable;
    if (outcome == GuildInviteOutcome::rate_limited)
        owner->retry_at_ms = response.retry_at_ms ? response.retry_at_ms : clock_monotonic_milliseconds() + 2000;
    invite_card_cache_complete(&owner->cache, {owner->code, owner->code_bytes}, outcome,
                               outcome == GuildInviteOutcome::resolved ? &decoded : nullptr,
                               clock_monotonic_milliseconds());
    return true;
}

u64 application_invite_cards_next_wake() noexcept {
    if (!owner || owner->request_id || !invite_card_cache_next(owner->cache))
        return 0;
    return maximum<u64>(1, owner->retry_at_ms);
}

void application_invite_cards_release() noexcept {
    if (!owner)
        return;
    message_scene_bind_invite_cards(&g_app.message_scene, nullptr);
    VirtualFree(owner, 0, MEM_RELEASE);
    owner = nullptr;
}

} // namespace maicord::app_detail
