#include "application_emoji_preferences.hpp"
#include "application_media_picker.hpp"
#include "application_state.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/time.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/protocol/emoji_preferences.hpp"
#include "maicord/ui/reaction_picker.hpp"

#include <Windows.h>

namespace maicord::app_detail {
namespace {
constexpr u64 kUseFlushDelayMs = 60'000, kFailureBackoffMs = 30'000;
constexpr u32 kKeyBytes = kEmojiPreferencesMaximumKeyBytes;
constexpr u32 kScratchBytes = 3 * (kEmojiPreferencesMaximumGroupBytes + 8) + 32;
// The patch codec bounds edits per write; later edits follow in the next write.
constexpr u32 kPatchEdits = 64;

struct Broker {
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
    u8* response_body;
    u64 profile_id, self_user_id, request_id, gateway_revision, retry_at;
    u32 generation, submitted_generation, submitted_edits, submitted_uses;
    AccountGifFavoritesOperation operation;
    u8 attempts;
    bool authoritative, stale, fetch_wanted, was_running, receipt_consumed, admission_busy;
};
Broker* broker;

void dispose(void* data, u32 bytes) noexcept {
    if (data) {
        SecureZeroMemory(data, bytes);
        VirtualFree(data, 0, MEM_RELEASE);
    }
}
template <class T> T* allocate(u32 bytes) noexcept {
    return static_cast<T*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}
ReactionPreferenceStore* store() noexcept {
    return g_app.account_ui ? &g_app.account_ui->reaction_preferences : nullptr;
}
bool online() noexcept {
    return g_app.account.instance && g_app.account_ui && g_app.account.status.profile_id &&
           g_app.account_ui->chat_state.self_user_id && g_app.account.status.running &&
           !g_app.account.status.failed && g_app.account.api.emoji_preferences &&
           g_app.account.status.required_action == AccountRequirement::none;
}
bool scope(const Broker& p) noexcept {
    return g_app.account.instance && g_app.account_ui && p.profile_id && p.self_user_id &&
           p.profile_id == g_app.account.status.profile_id &&
           p.self_user_id == g_app.account_ui->chat_state.self_user_id;
}
void replace_groups(Broker& p, u8* groups, u32 bytes, const EmojiPreferencesSnapshot& snapshot) noexcept {
    dispose(p.groups, p.group_bytes);
    p.groups = groups;
    p.group_bytes = bytes;
    // An account without emoji groups still has a data version to write against.
    p.snapshot = snapshot;
    if (!groups)
        p.snapshot.favorites = p.snapshot.frecency = p.snapshot.reaction_frecency = {};
}
void clear_scope(Broker& p) noexcept {
    if (!++p.generation)
        ++p.generation;
    replace_groups(p, nullptr, 0, {});
    p.authoritative = p.stale = p.fetch_wanted = false;
    p.attempts = 0;
    p.retry_at = 0;
}
void bind_scope(Broker& p) noexcept {
    p.profile_id = g_app.account.status.profile_id;
    p.self_user_id = g_app.account_ui->chat_state.self_user_id;
    p.gateway_revision = g_app.account.status.frecency_settings_revision;
    p.was_running = g_app.account.status.running;
    // Fetched lazily: on the first picker open or before the first write.
    p.fetch_wanted = false;
    p.retry_at = 0;
}
Broker* ensure() noexcept {
    if (!broker) {
        broker = allocate<Broker>(sizeof(Broker));
        if (broker)
            broker->generation = 1;
    }
    return broker;
}
void preferences_changed() noexcept {
    application_media_picker_emoji_preferences_changed();
    application_request_frame();
}

struct Envelope {
    JsonString settings;
    bool settings_seen, out_of_date_seen, out_of_date;
};
bool member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& envelope = *static_cast<Envelope*>(opaque);
    if (json_string_equals_ascii(name, utf8_literal("settings"))) {
        if (envelope.settings_seen || !json_value_as_string(value, &envelope.settings))
            return false;
        envelope.settings_seen = true;
    } else if (json_string_equals_ascii(name, utf8_literal("out_of_date"))) {
        if (envelope.out_of_date_seen || !json_value_to_bool(value, &envelope.out_of_date))
            return false;
        envelope.out_of_date_seen = true;
    }
    return true;
}
// Base64 settings to an owned group bank; false on any malformed input.
bool decode_settings(Utf8View settings, u8** groups, u32* group_bytes, EmojiPreferencesSnapshot* snapshot) noexcept {
    const auto measured = base64_decode(settings, {});
    if (measured.error != Base64Error::output_too_small && !measured.ok())
        return false;
    if (!measured.bytes) {
        // An empty proto is a valid account without frecency settings.
        *groups = nullptr;
        *group_bytes = 0;
        *snapshot = {};
        snapshot->versions_present = false;
        return true;
    }
    if (measured.bytes > kRestClientMaximumResponseBytes)
        return false;
    u8* raw = allocate<u8>(measured.bytes);
    if (!raw)
        return false;
    bool ok = base64_decode(settings, {raw, measured.bytes}).ok();
    const ConstSlice<u8> proto{raw, measured.bytes};
    EmojiPreferencesSnapshot measure{};
    const auto size = ok ? emoji_preferences_decode(proto, {}, &measure) : EmojiPreferencesResult{};
    ok = ok && size.ok();
    u8* bank = ok && size.group_bytes ? allocate<u8>(size.group_bytes) : nullptr;
    ok = ok && (bank || !size.group_bytes) &&
         emoji_preferences_decode(proto, {bank, size.group_bytes}, snapshot).ok();
    if (ok && !bank) {
        snapshot->favorites = snapshot->frecency = snapshot->reaction_frecency = {};
    }
    dispose(raw, measured.bytes);
    if (!ok) {
        dispose(bank, size.group_bytes);
        return false;
    }
    *groups = bank;
    *group_bytes = size.group_bytes;
    return true;
}
bool decode_response(Utf8View body, bool* out_of_date, u8** groups, u32* group_bytes,
                     EmojiPreferencesSnapshot* snapshot) noexcept {
    if (!body.data || !body.bytes || body.bytes > kRestClientMaximumResponseBytes)
        return false;
    u32 indexes[256]{};
    const auto scanned = json_scan_structurals_avx1(body, {indexes, 256});
    Envelope envelope{};
    if (!scanned.ok() || !json_visit_root_object(body, {indexes, scanned.count}, 16, member, &envelope).ok() ||
        !envelope.settings_seen)
        return false;
    *out_of_date = envelope.out_of_date;
    if (!envelope.settings.has_escapes)
        return decode_settings(envelope.settings.encoded, groups, group_bytes, snapshot);
    const auto measured = json_string_decode_utf8(envelope.settings, {});
    if (measured.error != JsonStringDecodeError::output_too_small || !measured.bytes ||
        measured.bytes > kRestClientMaximumResponseBytes)
        return false;
    char* temporary = allocate<char>(measured.bytes);
    if (!temporary)
        return false;
    const auto decoded = json_string_decode_utf8(envelope.settings, {temporary, measured.bytes});
    const bool ok = decoded.ok() && decode_settings({temporary, decoded.bytes}, groups, group_bytes, snapshot);
    dispose(temporary, measured.bytes);
    return ok;
}

bool visit_favorite(void* context, Utf8View key) noexcept {
    auto& p = *static_cast<Broker*>(context);
    const auto emoji = application_emoji_preference_ref(key);
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
    const auto emoji = application_emoji_preference_ref(key);
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
    target = {};
    target.emoji = emoji;
    target.total_uses = item.total_uses;
    target.recent_count = minimum<u8>(item.recent_count, static_cast<u8>(kReactionPreferenceRecentUseCapacity));
    for (u32 i = 0; i < target.recent_count; ++i)
        target.recent_uses[i] = item.recent_uses[item.recent_count - target.recent_count + i];
    return true;
}
void apply_snapshot(Broker& p) noexcept {
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
    preferences_changed();
}

// Keys match observed entries by emoji, so keys other clients wrote are sent
// back as received; this client's own key is only used for new entries.
u64 preference_identity(void*, Utf8View key) noexcept {
    const auto emoji = application_emoji_preference_ref(key);
    return !emoji.valid() ? 0 : emoji.custom() ? emoji.custom_id : (1ull << 63) | emoji.catalog_index;
}
bool use_pending(const ReactionPreferenceStore& preferences, u32 kind, ReactionEmojiRef emoji) noexcept {
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
    if (!preferences)
        return 0;
    const u64 retry = maximum<u64>(1, p.retry_at);
    if (preferences->favorite_edit_count || p.fetch_wanted)
        return retry;
    const u64 uses = uses_due(*preferences);
    return uses ? maximum(retry, uses) : 0;
}

void fail(Broker& p, AccountGifFavoritesOperation operation) noexcept {
    p.retry_at = clock_monotonic_milliseconds() + kFailureBackoffMs;
    if (++p.attempts < 3)
        return;
    // Give up on this batch; a fresh snapshot restores the visible truth.
    if (operation == AccountGifFavoritesOperation::update && store())
        reaction_preferences_commit(store(), p.submitted_edits, 0);
    p.attempts = 0;
    p.authoritative = false;
    p.fetch_wanted = false;
}
} // namespace

u32 application_emoji_preference_key(ReactionEmojiRef emoji, Slice<char> output) noexcept {
    return reaction_emoji_settings_key(emoji, output);
}

ReactionEmojiRef application_emoji_preference_ref(Utf8View key) noexcept {
    return reaction_emoji_from_settings_key(key);
}

bool application_emoji_preferences_toggle_favorite(ReactionEmojiRef emoji) noexcept {
    auto* preferences = store();
    if (!preferences || !reaction_preferences_toggle_favorite_emoji(preferences, emoji).ok())
        return false;
    if (ensure())
        application_emoji_preferences_synchronize();
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
    for (u32 i = 0; i < minimum(uses.count, kApplicationEmojiUseCapacity); ++i)
        reaction_preferences_track_emoji(preferences, ReactionPreferenceKind::message, uses.emoji[i], now);
    if (ensure())
        application_emoji_preferences_synchronize();
}

void application_emoji_preferences_open() noexcept {
    if (!online() || !ensure())
        return;
    application_emoji_preferences_synchronize();
    auto& p = *broker;
    if (scope(p) && (p.stale || !p.authoritative) && !p.request_id) {
        p.fetch_wanted = true;
        p.retry_at = 0;
    }
}

void application_emoji_preferences_synchronize() noexcept {
    auto* p = broker;
    if (!p)
        return;
    if (!scope(*p)) {
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
    if (!p || !scope(*p) || !online() || p->request_id || p->response_body)
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
            const u32 bytes = application_emoji_preference_key(preferences->favorite_edits[i].emoji,
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
                    const u32 bytes =
                        application_emoji_preference_key(usage.emoji, {p.entry_keys[kind][counts[kind]], kKeyBytes});
                    if (!bytes)
                        continue;
                    auto& entry = p.entries[kind][counts[kind]];
                    entry = {};
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
        write = allocate<u8>(write_bytes);
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
    p.response_body = allocate<u8>(kRestClientMaximumResponseBytes);
    if (!p.response_body) {
        dispose(write, write_bytes);
        p.retry_at = now_ms + kFailureBackoffMs;
        return false;
    }
    const auto admitted = g_app.account.api.emoji_preferences(g_app.account.instance, operation, patch, id);
    dispose(write, write_bytes);
    if (!admitted.ok()) {
        dispose(p.response_body, kRestClientMaximumResponseBytes);
        p.response_body = nullptr;
        p.admission_busy = admitted.error == RestClientSubmitError::busy;
        if (!p.admission_busy)
            fail(p, operation);
        return false;
    }
    p.request_id = id;
    p.operation = operation;
    p.submitted_generation = p.generation;
    p.admission_busy = false;
    return true;
}

bool application_emoji_preferences_response(const RestClientResponse& response, Utf8View body) noexcept {
    auto* owner = broker;
    if (!owner || !owner->request_id || owner->request_id != response.request_id)
        return false;
    auto& p = *owner;
    const auto operation = p.operation;
    const bool current = scope(p) && p.submitted_generation == p.generation && store();
    p.request_id = 0;
    p.admission_busy = false;
    p.receipt_consumed = true;
    const u64 now = clock_monotonic_milliseconds();
    if (!current) {
        p.authoritative = false;
        p.fetch_wanted = scope(p);
        return true;
    }
    if (response.transport_ok() && (response.rate_limited || response.status_code == 429) &&
        response.retry_at_ms > now) {
        p.retry_at = response.retry_at_ms;
        return true;
    }
    if (!response.transport_ok() || response.status_code < 200 || response.status_code >= 300) {
        fail(p, operation);
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
    auto* p = broker;
    if (!p || p->request_id || !p->receipt_consumed)
        return;
    dispose(p->response_body, kRestClientMaximumResponseBytes);
    p->response_body = nullptr;
    p->receipt_consumed = false;
}

Slice<u8> application_emoji_preferences_response_buffer() noexcept {
    const auto* p = broker;
    return p && p->response_body ? Slice<u8>{p->response_body, kRestClientMaximumResponseBytes}
                                 : Slice<u8>{};
}

u64 application_emoji_preferences_next_wake() noexcept {
    const auto* p = broker;
    if (!p || !scope(*p) || !online() || p->request_id || p->response_body || p->admission_busy ||
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
    dispose(p->response_body, kRestClientMaximumResponseBytes);
    dispose(p, sizeof(Broker));
}
} // namespace maicord::app_detail
