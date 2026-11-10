#include "emoji_preferences.hpp"
#include "maicord/core/memory.hpp"
#include "../media/picker.hpp"
#include "account_broker.hpp"
#include "../shell/state.hpp"
#include "maicord/core/time.hpp"
#include "maicord/protocol/encoding/base64.hpp"
#include "maicord/protocol/user/emoji_preferences.hpp"
#include "maicord/ui/chat/messages/reaction_picker.hpp"
#include "../account/account_ui_runtime.hpp"


namespace maicord::app_detail {
namespace {
constexpr u64 kUseFlushDelayMs = 60'000, kFailureBackoffMs = 30'000;
constexpr u32 kKeyBytes = kEmojiPreferencesMaximumKeyBytes;
constexpr u32 kScratchBytes = 3 * (kEmojiPreferencesMaximumGroupBytes + 8) + 32;
// The patch codec bounds edits per write; later edits follow in the next write.
constexpr u32 kPatchEdits = 64;

struct Broker : AccountBroker {
    ReactionPreferenceRemoteUsage remote[kReactionPreferenceKindCount][kReactionPreferenceUsageCapacity];
    ReactionEmojiRef remote_favorites[kReactionPreferenceFavoriteCapacity];
    EmojiFrecencyEntry entries[kReactionPreferenceKindCount][kReactionPreferenceUsageCapacity];
    char entry_keys[kReactionPreferenceKindCount][kReactionPreferenceUsageCapacity][kKeyBytes];
    EmojiFavoriteEdit edits[kPatchEdits];
    char edit_keys[kPatchEdits][kKeyBytes];
    u32 remote_count[kReactionPreferenceKindCount], remote_favorite_count;
    u8* groups; // observed snapshot groups
    u32 group_bytes;
    EmojiPreferencesSnapshot snapshot;
    u64 retry_at;
    u32 submitted_edits, submitted_uses;
    bool stale, terminal_rate_limit;
};
Broker* broker;

ReactionPreferenceStore* store() noexcept {
    return g_app.account_ui ? &g_app.account_ui->reaction_preferences : nullptr;
}
bool online() noexcept {
    return account_online() && g_app.account.api.emoji_preferences;
}
void replace_groups(Broker& p, u8* groups, u32 bytes, const EmojiPreferencesSnapshot& snapshot) noexcept {
    dispose(p.groups, p.group_bytes);
    p.groups = groups;
    p.group_bytes = bytes;
    // An account without emoji groups still has a data version to write against.
    p.snapshot = snapshot;
    if (!groups)
        p.snapshot.favorites = p.snapshot.frecency = p.snapshot.reaction_frecency = p.snapshot.sticker_frecency = {};
}
__forceinline void clear_scope(Broker& p) noexcept {
    p.generation = next_generation(p.generation);
    replace_groups(p, nullptr, 0, {});
    p.authoritative = p.stale = p.fetch_wanted = p.terminal_rate_limit = false;
    p.attempts = 0;
    p.retry_at = 0;
}
__forceinline void bind_scope(Broker& p) noexcept {
    broker_bind(p);
    // Fetched lazily: on the first picker open or before the first write.
    p.fetch_wanted = false;
    p.retry_at = 0;
}
Broker* ensure() noexcept {
    if (!broker) {
        broker = commit_pages<Broker>();
        if (broker)
            broker->generation = 1;
    }
    return broker;
}

// Base64 settings to an owned group bank; false on any malformed input.
__forceinline bool decode_settings(Utf8View settings, u8** groups, u32* group_bytes, EmojiPreferencesSnapshot* snapshot) noexcept {
    const auto measured = base64_decode(settings, {});
    if (measured.error != Base64Error::output_too_small && !measured.ok())
        return false;
    if (!measured.bytes) {
        // An empty proto is a valid account without frecency settings.
        *groups = nullptr;
        *group_bytes = 0;
        reset_value(snapshot);
        return true;
    }
    if (measured.bytes > kRestClientMaximumResponseBytes)
        return false;
    u8* raw = commit_pages<u8>(measured.bytes);
    if (!raw)
        return false;
    bool ok = base64_decode(settings, {raw, measured.bytes}).ok();
    const ConstSlice<u8> proto{raw, measured.bytes};
    EmojiPreferencesSnapshot measure{};
    const auto size = ok ? emoji_preferences_decode(proto, {}, &measure) : EmojiPreferencesResult{};
    ok = ok && size.ok();
    u8* bank = ok && size.group_bytes ? commit_pages<u8>(size.group_bytes) : nullptr;
    ok = ok && (bank || !size.group_bytes) &&
         emoji_preferences_decode(proto, {bank, size.group_bytes}, snapshot).ok();
    dispose(raw, measured.bytes);
    if (!ok) {
        dispose(bank, size.group_bytes);
        return false;
    }
    *groups = bank;
    *group_bytes = size.group_bytes;
    return true;
}
__forceinline bool decode_response(Utf8View body, bool* out_of_date, u8** groups, u32* group_bytes,
                     EmojiPreferencesSnapshot* snapshot) noexcept {
    struct Target {
        u8** groups;
        u32* group_bytes;
        EmojiPreferencesSnapshot* snapshot;
    } target{groups, group_bytes, snapshot};
    return application_settings_envelope(
        body, out_of_date,
        [](void* context, Utf8View settings) noexcept {
            const auto& found = *static_cast<Target*>(context);
            return decode_settings(settings, found.groups, found.group_bytes, found.snapshot);
        },
        &target);
}

bool visit_favorite(void* context, Utf8View key) noexcept {
    auto& p = *static_cast<Broker*>(context);
    const auto emoji = reaction_emoji_from_settings_key(key);
    if (emoji.valid() && p.remote_favorite_count < kReactionPreferenceFavoriteCapacity)
        p.remote_favorites[p.remote_favorite_count++] = emoji;
    return true;
}
struct FrecencyVisit {
    Broker* p;
    u32 kind;
};
bool visit_frecency(void* context, Utf8View key, const EmojiFrecencyItem& item) noexcept {
    auto& visit = *static_cast<FrecencyVisit*>(context);
    auto& p = *visit.p;
    const auto emoji = reaction_emoji_from_settings_key(key);
    if (!emoji.valid())
        return true;
    u32 slot = 0;
    while (slot < p.remote_count[visit.kind] && !(p.remote[visit.kind][slot].emoji == emoji))
        ++slot;
    if (slot == p.remote_count[visit.kind]) {
        if (slot == kReactionPreferenceUsageCapacity)
            return true;
        ++p.remote_count[visit.kind];
    }
    auto& target = p.remote[visit.kind][slot];
    reset_value(&target);
    target.emoji = emoji;
    target.total_uses = item.total_uses;
    target.recent_count = minimum<u8>(item.recent_count, static_cast<u8>(kReactionPreferenceRecentUseCapacity));
    for (u32 i = 0; i < target.recent_count; ++i)
        target.recent_uses[i] = item.recent_uses[item.recent_count - target.recent_count + i];
    return true;
}
__forceinline void apply_snapshot(Broker& p) noexcept {
    auto* preferences = store();
    if (!preferences)
        return;
    p.remote_favorite_count = 0;
    p.remote_count[0] = p.remote_count[1] = 0;
    emoji_preferences_visit_favorites(p.snapshot.favorites, visit_favorite, &p);
    FrecencyVisit reaction{&p, static_cast<u32>(ReactionPreferenceKind::reaction)};
    FrecencyVisit message{&p, static_cast<u32>(ReactionPreferenceKind::message)};
    emoji_preferences_visit_frecency(p.snapshot.reaction_frecency, visit_frecency, &reaction);
    emoji_preferences_visit_frecency(p.snapshot.frecency, visit_frecency, &message);
    reaction_preferences_apply_remote(preferences, {p.remote_favorites, p.remote_favorite_count},
                                      {p.remote[0], p.remote_count[0]}, {p.remote[1], p.remote_count[1]},
                                      clock_unix_milliseconds());
    application_media_picker_emoji_preferences_changed();
    application_request_frame();
}

// Keys match observed entries by emoji, so keys other clients wrote are sent
// back as received; this client's own key is only used for new entries.
u64 preference_identity(Utf8View key) noexcept {
    const auto emoji = reaction_emoji_from_settings_key(key);
    return !emoji.valid() ? 0 : emoji.custom() ? emoji.custom_id : (1ull << 63) | emoji.catalog_index;
}
__forceinline bool use_pending(const ReactionPreferenceStore& preferences, u32 kind, ReactionEmojiRef emoji) noexcept {
    for (u32 i = 0; i < preferences.pending_use_count; ++i)
        if (static_cast<u32>(preferences.pending_uses[i].kind) == kind && preferences.pending_uses[i].emoji == emoji)
            return true;
    return false;
}

// Monotonic time when the batched uses should be written, or 0 when none.
u64 uses_due(const ReactionPreferenceStore& preferences) noexcept {
    if (!preferences.pending_use_count)
        return 0;
    // Batch composer and reaction uses; the official client sends them with later saves.
    u64 oldest = ~u64{0};
    for (u32 i = 0; i < preferences.pending_use_count; ++i)
        oldest = minimum(oldest, preferences.pending_uses[i].timestamp_ms);
    const u64 unix_now = clock_unix_milliseconds(), now = clock_monotonic_milliseconds();
    return now + (oldest + kUseFlushDelayMs > unix_now ? oldest + kUseFlushDelayMs - unix_now : 0);
}
u64 due_at(const Broker& p) noexcept {
    const auto* preferences = store();
    if (!preferences || p.terminal_rate_limit)
        return 0;
    const u64 retry = maximum<u64>(1, p.retry_at);
    if (preferences->favorite_edit_count || p.fetch_wanted)
        return retry;
    const u64 uses = uses_due(*preferences);
    return uses ? maximum(retry, uses) : 0;
}

void fail(Broker& p, AccountGifFavoritesOperation operation, bool retry = true) noexcept {
    p.terminal_rate_limit = !retry;
    p.retry_at = clock_monotonic_milliseconds() + kFailureBackoffMs;
    if (retry && ++p.attempts < 3)
        return;
    // Give up on this batch; a fresh snapshot restores the visible truth.
    if (operation == AccountGifFavoritesOperation::update && store())
        reaction_preferences_commit(store(), p.submitted_edits, 0);
    p.attempts = 0;
    p.authoritative = false;
    p.fetch_wanted = false;
}
} // namespace

bool application_emoji_preferences_toggle_favorite(ReactionEmojiRef emoji) noexcept {
    auto* preferences = store();
    if (!preferences || !reaction_preferences_toggle_favorite_emoji(preferences, emoji).ok())
        return false;
    if (ensure()) {
        broker->terminal_rate_limit = false;
        application_emoji_preferences_synchronize();
    }
    application_request_frame();
    return true;
}

void application_emoji_preferences_track_reaction(ReactionEmojiRef emoji) noexcept {
    auto* preferences = store();
    if (!preferences || !emoji.valid())
        return;
    // A reaction counts toward both the reaction and composer histories.
    const u64 now = clock_unix_milliseconds();
    reaction_preferences_track_emoji(preferences, ReactionPreferenceKind::reaction, emoji, now);
    reaction_preferences_track_emoji(preferences, ReactionPreferenceKind::message, emoji, now);
    if (ensure())
        application_emoji_preferences_synchronize();
}

ApplicationEmojiUse application_emoji_preferences_scan(Utf8View content) noexcept {
    ApplicationEmojiUse uses{};
    uses.count = reaction_emoji_scan_message(content, {uses.emoji, kApplicationEmojiUseCapacity});
    return uses;
}

void application_emoji_preferences_track_message(const ApplicationEmojiUse& uses) noexcept {
    auto* preferences = store();
    if (!preferences || !uses.count)
        return;
    const u64 now = clock_unix_milliseconds();
    for (u32 i = 0; i < uses.count; ++i)
        reaction_preferences_track_emoji(preferences, ReactionPreferenceKind::message, uses.emoji[i], now);
    if (ensure())
        application_emoji_preferences_synchronize();
}

ConstSlice<u8> application_emoji_preferences_sticker_frecency() noexcept {
    const auto* p = broker;
    return p && broker_scope(*p) ? p->snapshot.sticker_frecency : ConstSlice<u8>{};
}

void application_emoji_preferences_open() noexcept {
    if (!online() || !ensure())
        return;
    application_emoji_preferences_synchronize();
    auto& p = *broker;
    p.terminal_rate_limit = false;
    if (broker_scope(p) && (p.stale || !p.authoritative) && !p.request_id) {
        p.fetch_wanted = true;
        p.retry_at = 0;
    }
}

void application_emoji_preferences_synchronize() noexcept {
    auto* p = broker;
    if (!p)
        return;
    if (!broker_scope(*p)) {
        if (p->profile_id || p->self_user_id) {
            clear_scope(*p);
            p->profile_id = p->self_user_id = 0;
        }
        if (!p->request_id && online())
            bind_scope(*p);
        return;
    }
    const bool running = g_app.account.status.running;
    if (running && !p->was_running) {
        p->fetch_wanted = true;
        p->stale = true;
    }
    p->was_running = running;
    if (p->gateway_revision != g_app.account.status.frecency_settings_revision) {
        // Another session (or this one's own write) changed the settings.
        p->gateway_revision = g_app.account.status.frecency_settings_revision;
        p->stale = true;
    }
}

bool application_emoji_preferences_pending(u64 now_ms) noexcept {
    if (!broker && online() && store())
        ensure();
    application_emoji_preferences_synchronize();
    const auto* p = broker;
    if (!p || !broker_scope(*p) || !online() || p->request_id || p->response_body)
        return false;
    const u64 due = due_at(*p);
    return due && due <= now_ms;
}

bool application_emoji_preferences_submit(u64 id, u64 now_ms) noexcept {
    if (!id || !application_emoji_preferences_pending(now_ms))
        return false;
    auto& p = *broker;
    auto* preferences = store();
    const u64 uses = uses_due(*preferences);
    const bool writing = preferences->favorite_edit_count || (uses && uses <= now_ms);
    const auto operation = p.authoritative && writing ? AccountGifFavoritesOperation::update
                                                    : AccountGifFavoritesOperation::fetch;
    u8* write = nullptr;
    const u32 json_capacity = kEmojiPreferencesMaximumPatchBytes;
    const u32 write_bytes = kScratchBytes + json_capacity;
    ConstSlice<u8> patch{};
    p.submitted_edits = p.submitted_uses = 0;
    if (operation == AccountGifFavoritesOperation::update) {
        EmojiPreferencesPatch request{};
        u32 edit_count = 0;
        const u32 sending = minimum(preferences->favorite_edit_count, kPatchEdits);
        for (u32 i = 0; i < sending; ++i) {
            const u32 bytes = reaction_emoji_settings_key(preferences->favorite_edits[i].emoji,
                                                          {p.edit_keys[edit_count], kKeyBytes});
            if (bytes) {
                p.edits[edit_count] = {{p.edit_keys[edit_count], bytes}, preferences->favorite_edits[i].favorite};
                ++edit_count;
            }
        }
        request.observed_favorites = p.snapshot.favorites;
        request.favorite_edits = {p.edits, edit_count};
        request.write_frecency = preferences->pending_use_count != 0;
        if (request.write_frecency) {
            u32 counts[kReactionPreferenceKindCount]{};
            for (u32 kind = 0; kind < kReactionPreferenceKindCount; ++kind) {
                const auto& table = preferences->tables[kind];
                for (const auto& usage : table.usage) {
                    if (!usage.occupied)
                        continue;
                    const u32 bytes = reaction_emoji_settings_key(
                        usage.emoji, {p.entry_keys[kind][counts[kind]], kKeyBytes});
                    if (!bytes)
                        continue;
                    auto& entry = p.entries[kind][counts[kind]];
                    reset_value(&entry);
                    entry.key = {p.entry_keys[kind][counts[kind]], bytes};
                    entry.updated = use_pending(*preferences, kind, usage.emoji);
                    entry.item.total_uses = usage.total_uses;
                    entry.item.frecency = static_cast<i32>(usage.frecency);
                    entry.item.score = usage.score;
                    entry.item.recent_count = usage.recent_count;
                    for (u32 i = 0; i < usage.recent_count; ++i)
                        entry.item.recent_uses[i] = usage.recent_uses[i];
                    ++counts[kind];
                }
            }
            request.observed_reaction_frecency = p.snapshot.reaction_frecency;
            request.observed_frecency = p.snapshot.frecency;
            request.reaction_frecency = {p.entries[0], counts[0]};
            request.frecency = {p.entries[1], counts[1]};
        }
        request.identity = preference_identity;
        request.required_data_version = p.snapshot.data_version;
        if (!request.favorite_edits.count && !request.write_frecency) {
            // Only unrepresentable edits remain; nothing can be written.
            reaction_preferences_commit(preferences, sending, 0);
            return false;
        }
        write = commit_pages<u8>(write_bytes);
        if (!write) {
            p.retry_at = now_ms + kFailureBackoffMs;
            return false;
        }
        const auto encoded =
            emoji_preferences_write_patch(request, {write, kScratchBytes}, {write + kScratchBytes, json_capacity});
        if (!encoded.ok()) {
            dispose(write, write_bytes);
            // Over the favorite limit or malformed: drop this batch, keep uses.
            reaction_preferences_commit(preferences, sending, 0);
            p.authoritative = false;
            p.fetch_wanted = true;
            return false;
        }
        patch = {write + kScratchBytes, encoded.bytes};
        p.submitted_edits = sending;
        p.submitted_uses = preferences->pending_use_count;
    }
    p.response_body = commit_pages<u8>(kRestClientMaximumResponseBytes);
    if (!p.response_body) {
        dispose(write, write_bytes);
        p.retry_at = now_ms + kFailureBackoffMs;
        return false;
    }
    const auto admitted = g_app.account.api.emoji_preferences(g_app.account.instance, operation, patch, id);
    dispose(write, write_bytes);
    if (!admitted.ok()) {
        broker_drop_response(p);
        p.admission_busy = admitted.error == RestClientSubmitError::busy;
        if (!p.admission_busy)
            fail(p, operation);
        return false;
    }
    broker_admitted(p, id, operation);
    return true;
}

bool application_emoji_preferences_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!broker_awaits(broker, response.request_id))
        return false;
    auto& p = *broker;
    const auto operation = p.operation;
    const bool current = broker_scope(p) && p.submitted_generation == p.generation && store();
    broker_received(p);
    const u64 now = clock_monotonic_milliseconds();
    if (!current) {
        p.authoritative = false;
        p.fetch_wanted = broker_scope(p);
        return true;
    }
    if (response.transport_ok() && response.rate_limited && response.status_code == 429 &&
        response.retry_at_ms > now) {
        p.retry_at = response.retry_at_ms;
        return true;
    }
    if (!response.transport_ok() || response.status_code < 200 || response.status_code >= 300) {
        fail(p, operation, response.status_code != 429);
        return true;
    }
    bool out_of_date = false;
    u8* groups = nullptr;
    u32 group_bytes = 0;
    EmojiPreferencesSnapshot snapshot{};
    if (!decode_response(body, &out_of_date, &groups, &group_bytes, &snapshot)) {
        fail(p, operation);
        return true;
    }
    replace_groups(p, groups, group_bytes, snapshot);
    if (operation == AccountGifFavoritesOperation::update) {
        if (out_of_date) {
            // Rejected against newer settings: rebase the same edits and retry.
            if (++p.attempts >= 3) {
                reaction_preferences_commit(store(), p.submitted_edits, 0);
                p.attempts = 0;
            }
        } else {
            reaction_preferences_commit(store(), p.submitted_edits, p.submitted_uses);
            p.attempts = 0;
        }
    } else
        p.attempts = 0;
    p.authoritative = true;
    p.stale = false;
    p.fetch_wanted = false;
    p.retry_at = 0;
    apply_snapshot(p);
    return true;
}

void application_emoji_preferences_response_drained() noexcept {
    broker_drain(broker);
}

Slice<u8> application_emoji_preferences_response_buffer() noexcept {
    return broker_response_buffer(broker);
}

u64 application_emoji_preferences_next_wake() noexcept {
    const auto* p = broker;
    if (!p || !broker_scope(*p) || !online() || p->request_id || p->response_body || p->admission_busy ||
        g_app.message_history_request_owner != MessageHistoryRequestOwner::none)
        return 0;
    return due_at(*p);
}

void application_emoji_preferences_release() noexcept {
    auto* p = broker;
    if (!p)
        return;
    broker = nullptr;
    dispose(p->groups, p->group_bytes);
    broker_drop_response(*p);
    dispose(p, sizeof(Broker));
}
} // namespace maicord::app_detail
