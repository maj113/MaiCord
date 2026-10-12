#include "application_gif_favorites.hpp"
#include "application_state.hpp"
#include "application_media_picker.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/time.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/protocol/media_catalog.hpp"

#include <Windows.h>

namespace maicord::app_detail {
namespace {
constexpr u32 kQueueCapacity = 8, kUrlBytes = kMediaCatalogMaximumUrlBytes;
struct FavoriteBank {
    u32 bytes, group_bytes, record_count, index_words, data_version, entry_count;
    u8* group;
    GifFavoriteRecord* records;
    u32* index;
};
struct FavoriteAction {
    char url[kUrlBytes], src[kUrlBytes];
    u32 url_bytes, src_bytes, width, height;
    GifFavoriteFormat format;
    bool desired;
};
bool equal(Utf8View a, Utf8View b) noexcept {
    return a.bytes == b.bytes && bytes_equal(a.data, b.data, a.bytes);
}
[[maybe_unused]] bool usable_text(Utf8View text) noexcept {
    return text.data && text.bytes && text.bytes <= kUrlBytes && utf8_validate(text).valid;
}
void dispose(void* data, u32 bytes) noexcept {
    if (data) {
        SecureZeroMemory(data, bytes);
        VirtualFree(data, 0, MEM_RELEASE);
    }
}
void dispose(FavoriteBank* bank) noexcept {
    if (bank)
        dispose(bank, bank->bytes);
}
FavoriteBank* decode(Utf8View settings) noexcept {
    const auto measure = gif_favorites_measure_base64(settings);
    if (!measure.ok() || !measure.versions_present)
        return nullptr;
    const u32 records_offset = (static_cast<u32>(sizeof(FavoriteBank)) + measure.group_bytes + 7) & ~7u;
    const u32 index_offset =
        records_offset + measure.entry_count * static_cast<u32>(sizeof(GifFavoriteRecord));
    const u32 bytes = index_offset + measure.index_words * static_cast<u32>(sizeof(u32));
    auto* bank =
        static_cast<FavoriteBank*>(VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!bank)
        return nullptr;
    bank->bytes = bytes;
    bank->group = reinterpret_cast<u8*>(bank) + sizeof(FavoriteBank);
    bank->records = reinterpret_cast<GifFavoriteRecord*>(reinterpret_cast<u8*>(bank) + records_offset);
    bank->index = reinterpret_cast<u32*>(reinterpret_cast<u8*>(bank) + index_offset);
    const auto result = gif_favorites_decode_base64(settings, {bank->group, measure.group_bytes},
                                                    {bank->records, measure.entry_count},
                                                    {bank->index, measure.index_words});
    if (!result.ok() || !result.versions_present) {
        dispose(bank);
        return nullptr;
    }
    bank->group_bytes = result.group_bytes;
    bank->record_count = result.record_count;
    bank->index_words = result.index_words;
    bank->data_version = result.data_version;
    bank->entry_count = result.entry_count;
    return bank;
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
FavoriteBank* decode_response(Utf8View body, bool* conflict) noexcept {
    if (!body.data || !body.bytes || body.bytes > kRestClientMaximumResponseBytes)
        return nullptr;
    u32 indexes[256]{};
    const auto scanned = json_scan_structurals_avx1(body, {indexes, 256});
    Envelope envelope{};
    if (!scanned.ok() ||
        !json_visit_root_object(body, {indexes, scanned.count}, 16, member, &envelope).ok() ||
        !envelope.settings_seen)
        return nullptr;
    *conflict = envelope.out_of_date;
    if (!envelope.settings.has_escapes)
        return decode(envelope.settings.encoded);
    const auto measured = json_string_decode_utf8(envelope.settings, {});
    if (measured.error != JsonStringDecodeError::output_too_small || !measured.bytes ||
        measured.bytes > kRestClientMaximumResponseBytes)
        return nullptr;
    auto* temporary =
        static_cast<char*>(VirtualAlloc(nullptr, measured.bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!temporary)
        return nullptr;
    const auto decoded = json_string_decode_utf8(envelope.settings, {temporary, measured.bytes});
    FavoriteBank* bank = decoded.ok() ? decode({temporary, decoded.bytes}) : nullptr;
    dispose(temporary, measured.bytes);
    return bank;
}
} // namespace

struct ApplicationGifFavorites {
    FavoriteBank* bank;
    FavoriteAction* queue;
    u8* response_body;
    u64 profile_id, self_user_id, request_id, due_at, revision, data_revision;
    u64 gateway_revision, submitted_generation;
    u32 generation, queue_count, data_version;
    AccountGifFavoritesOperation operation;
    u8 attempts;
    bool visible, authoritative, failed, fetch_wanted, admission_busy, was_running, receipt_consumed;
    char failed_url[kUrlBytes];
    u32 failed_url_bytes;
    Utf8View status;
};
namespace {
bool scope(const ApplicationGifFavorites& p) noexcept {
    return g_app.account.instance && g_app.account_ui && p.profile_id && p.self_user_id &&
           p.profile_id == g_app.account.status.profile_id &&
           p.self_user_id == g_app.account_ui->chat_state.self_user_id;
}
bool online() noexcept {
    return g_app.account.instance && g_app.account_ui && g_app.account.status.profile_id &&
           g_app.account_ui->chat_state.self_user_id && g_app.account.status.running &&
           !g_app.account.status.failed &&
           g_app.account.status.required_action == AccountRequirement::none;
}
void changed(ApplicationGifFavorites& p, bool data = false) noexcept {
    if (!++p.revision)
        ++p.revision;
    if (data && !++p.data_revision)
        ++p.data_revision;
    application_media_picker_favorites_changed();
    application_request_frame();
}
bool favorite(const ApplicationGifFavorites& p, Utf8View url) noexcept {
    return p.bank && gif_favorites_find({p.bank->records, p.bank->record_count},
                                        {p.bank->index, p.bank->index_words}, url);
}
Utf8View url(const FavoriteAction& action) noexcept {
    return {action.url, action.url_bytes};
}
void drop_action(ApplicationGifFavorites& p) noexcept {
    if (!p.queue_count)
        return;
    --p.queue_count;
    if (p.queue_count)
        memmove(p.queue, p.queue + 1, p.queue_count * sizeof(FavoriteAction));
    SecureZeroMemory(p.queue + p.queue_count, sizeof(FavoriteAction));
    if (!p.queue_count) {
        dispose(p.queue, kQueueCapacity * sizeof(FavoriteAction));
        p.queue = nullptr;
    }
}
void mark_failure(ApplicationGifFavorites& p, Utf8View status, bool drop = false) noexcept {
    p.failed = true;
    p.status = status;
    if (drop && p.queue_count) {
        p.failed_url_bytes = p.queue[0].url_bytes;
        copy_bytes(p.failed_url, p.queue[0].url, p.failed_url_bytes);
        drop_action(p);
    }
}
void replace_bank(ApplicationGifFavorites& p, FavoriteBank* bank) noexcept {
    auto* old = p.bank;
    p.data_version = bank ? bank->data_version : 0;
    if (old && bank && old->group_bytes == bank->group_bytes &&
        bytes_equal(old->group, bank->group, bank->group_bytes)) {
        // Only the conditional version/authority changed. Preserve immutable
        // record borrows and visible preview identities for identical data.
        dispose(bank);
        p.authoritative = true;
        changed(p);
        return;
    }
    p.bank = bank;
    p.authoritative = bank != nullptr;
    // Refresh borrowed picker records before disposing any previous bank.
    changed(p, true);
    dispose(old);
}
void clear_scope(ApplicationGifFavorites& p) noexcept {
    if (!++p.generation)
        ++p.generation;
    p.authoritative = false;
    p.fetch_wanted = false;
    p.due_at = 0;
    dispose(p.queue, kQueueCapacity * sizeof(FavoriteAction));
    p.queue = nullptr;
    p.queue_count = 0;
    p.failed_url_bytes = 0;
    p.failed = false;
    p.status = {};
    replace_bank(p, nullptr);
}
void bind_scope(ApplicationGifFavorites& p) noexcept {
    p.profile_id = g_app.account.status.profile_id;
    p.self_user_id = g_app.account_ui->chat_state.self_user_id;
    p.gateway_revision = g_app.account.status.frecency_settings_revision;
    p.was_running = g_app.account.status.running;
    p.fetch_wanted = p.visible;
    p.due_at = p.fetch_wanted ? 1 : 0;
}
void cancel_fetch(ApplicationGifFavorites& p) noexcept {
    if (!p.request_id || p.operation != AccountGifFavoritesOperation::fetch)
        return;
    if (g_app.account.api.gif_favorites && g_app.account.instance)
        g_app.account.api.gif_favorites(g_app.account.instance, AccountGifFavoritesOperation::cancel, {},
                                        p.request_id);
}
void schedule(ApplicationGifFavorites& p) noexcept {
    if (p.request_id)
        return;
    p.admission_busy = false;
    if (p.queue_count && !p.authoritative)
        p.fetch_wanted = true;
    // An unrelated queued action must not shorten the rejected head action's
    // server deadline. The accepted submission clears this fence.
    if (p.due_at > 1)
        return;
    p.due_at = (p.queue_count || (p.visible && p.fetch_wanted)) ? 1 : 0;
}
} // namespace

bool application_gif_favorites_open() noexcept {
    if (!online())
        return false;
    if (!g_app.gif_favorites) {
        g_app.gif_favorites = static_cast<ApplicationGifFavorites*>(VirtualAlloc(
            nullptr, sizeof(ApplicationGifFavorites), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!g_app.gif_favorites)
            return false;
        g_app.gif_favorites->generation = 1;
    }
    auto& p = *g_app.gif_favorites;
    const bool became_visible = !p.visible;
    p.visible = true;
    application_gif_favorites_synchronize();
    if (!scope(p) && !p.request_id)
        bind_scope(p);
    bool scheduled = false;
    if (!p.authoritative && !p.request_id && (!p.fetch_wanted || !p.due_at) &&
        (!p.failed || became_visible)) {
        p.fetch_wanted = true;
        p.due_at = 1;
        p.failed = false;
        p.status = {};
        scheduled = true;
    }
    if (became_visible || scheduled)
        changed(p);
    return scope(p);
}
void application_gif_favorites_set_visible(bool visible) noexcept {
    if (visible) {
        application_gif_favorites_open();
        return;
    }
    auto* p = g_app.gif_favorites;
    if (!p || !p->visible)
        return;
    p->visible = false;
    if (p->operation == AccountGifFavoritesOperation::fetch && p->request_id && !p->queue_count) {
        if (!++p->generation)
            ++p->generation;
        cancel_fetch(*p);
    }
    if (!p->queue_count) {
        p->fetch_wanted = false;
        p->due_at = 0;
    }
    changed(*p);
}
void application_gif_favorites_release() noexcept {
    auto* p = g_app.gif_favorites;
    if (!p)
        return;
    g_app.gif_favorites = nullptr;
    application_media_picker_favorites_changed();
    dispose(p->bank);
    dispose(p->queue, kQueueCapacity * sizeof(FavoriteAction));
    dispose(p->response_body, kRestClientMaximumResponseBytes);
    dispose(p, sizeof(ApplicationGifFavorites));
}
void application_gif_favorites_synchronize() noexcept {
    auto* p = g_app.gif_favorites;
    if (!p)
        return;
    if (!scope(*p)) {
        if (p->profile_id || p->self_user_id) {
            cancel_fetch(*p);
            clear_scope(*p);
            p->profile_id = p->self_user_id = 0;
        }
        if (!p->request_id && online()) {
            bind_scope(*p);
            changed(*p);
        }
        return;
    }
    const bool running = g_app.account.status.running;
    const bool reconnect = running && !p->was_running;
    const bool update = p->gateway_revision != g_app.account.status.frecency_settings_revision;
    p->was_running = running;
    p->gateway_revision = g_app.account.status.frecency_settings_revision;
    if (reconnect || update) {
        if (!++p->generation)
            ++p->generation;
        p->authoritative = false;
        p->fetch_wanted = true;
        p->failed = false;
        schedule(*p);
        changed(*p);
    }
}
ApplicationGifFavoritesView application_gif_favorites_view() noexcept {
    const auto* p = g_app.gif_favorites;
    if (!p || !scope(*p))
        return {};
    return {p->bank ? ConstSlice<GifFavoriteRecord>{p->bank->records, p->bank->record_count}
                    : ConstSlice<GifFavoriteRecord>{},
            p->revision,
            p->data_revision,
            p->authoritative,
            (p->request_id && p->operation == AccountGifFavoritesOperation::fetch) ||
                (p->fetch_wanted && (p->visible || p->queue_count)),
            p->failed,
            p->status};
}
ApplicationGifFavoriteState application_gif_favorites_state(Utf8View key) noexcept {
    const auto* p = g_app.gif_favorites;
    if (!p || !scope(*p))
        return {false, false, false, online()};
    bool pending = false;
    for (u32 i = 0; i < p->queue_count; ++i)
        if (equal(url(p->queue[i]), key)) {
            pending = true;
            break;
        }
    return {favorite(*p, key), pending,
            equal({p->failed_url, p->failed_url_bytes}, key) && p->failed_url_bytes, online() && !pending};
}
bool application_gif_favorites_toggle(Utf8View key, Utf8View src, u32 width, u32 height,
                                      GifFavoriteFormat format) noexcept {
    if (!usable_text(key) || !online() || src.bytes > kUrlBytes || (!src.data && src.bytes) ||
        (src.bytes && !utf8_validate(src).valid))
        return false;
    const bool was_visible = g_app.gif_favorites && g_app.gif_favorites->visible;
    if (!g_app.gif_favorites && !application_gif_favorites_open())
        return false;
    auto& p = *g_app.gif_favorites;
    if (!was_visible)
        p.visible = false;
    application_gif_favorites_synchronize();
    if (!scope(p))
        return false;
    const bool desired = !favorite(p, key);
    // Opaque or older saved entries can still be removed by their exact key.
    // Adding requires the catalog's supported source and representation.
    if (desired &&
        (!usable_text(src) || (format != GifFavoriteFormat::image && format != GifFavoriteFormat::video)))
        return false;
    for (u32 i = 0; i < p.queue_count; ++i)
        if (equal(url(p.queue[i]), key))
            return false;
    if (p.queue_count == kQueueCapacity) {
        mark_failure(p, utf8_literal("Too many changes at once. Try again in a moment."));
        changed(p);
        return false;
    }
    if (!p.queue) {
        p.queue = static_cast<FavoriteAction*>(VirtualAlloc(
            nullptr, sizeof(FavoriteAction) * kQueueCapacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!p.queue) {
            mark_failure(p, utf8_literal("Couldn't update favorites. Try again."));
            changed(p);
            return false;
        }
    }
    auto& action = p.queue[p.queue_count++];
    copy_bytes(action.url, key.data, key.bytes);
    action.url_bytes = key.bytes;
    copy_bytes(action.src, src.data, src.bytes);
    action.src_bytes = src.bytes;
    action.width = width;
    action.height = height;
    action.format = format;
    action.desired = desired;
    p.failed = false;
    p.status = {};
    if (equal({p.failed_url, p.failed_url_bytes}, key))
        p.failed_url_bytes = 0;
    if (!p.request_id && p.queue_count == 1)
        p.attempts = 0;
    schedule(p);
    changed(p);
    return true;
}
bool application_gif_favorites_pending(u64 now_ms) noexcept {
    application_gif_favorites_synchronize();
    const auto* p = g_app.gif_favorites;
    return p && scope(*p) && online() && !p->request_id && !p->response_body && p->due_at &&
           p->due_at <= now_ms;
}
bool application_gif_favorites_submit(u64 id, u64 now_ms) noexcept {
    if (!id || !application_gif_favorites_pending(now_ms) || !g_app.account.api.gif_favorites)
        return false;
    auto& p = *g_app.gif_favorites;
    const auto operation = p.authoritative && p.queue_count ? AccountGifFavoritesOperation::update
                                                            : AccountGifFavoritesOperation::fetch;
    u8* write = nullptr;
    u32 write_bytes = 0;
    ConstSlice<u8> patch{};
    if (operation == AccountGifFavoritesOperation::update) {
        const auto& action = p.queue[0];
        // A distinct queued action can already have been satisfied by a newer
        // authoritative snapshot. Complete it without writing redundant data.
        if (favorite(p, url(action)) == action.desired) {
            drop_action(p);
            schedule(p);
            changed(p);
            return false;
        }
        const u32 group_bound =
            minimum(kGifFavoritesMaximumGroupBytes,
                    p.bank->group_bytes + (action.desired ? action.url_bytes + action.src_bytes + 96 : 0));
        const u32 scratch_bytes = gif_favorites_write_scratch_bytes(group_bound, p.bank->entry_count);
        const u32 json_bytes =
            minimum(kGifFavoritesMaximumPatchBytes, base64_encoded_size(group_bound + 6) + 128);
        write_bytes = scratch_bytes + json_bytes;
        write =
            static_cast<u8*>(VirtualAlloc(nullptr, write_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!write) {
            mark_failure(p, utf8_literal("Couldn't update favorites. Try again."), true);
            schedule(p);
            changed(p);
            return false;
        }
        const GifFavoriteMutation mutation{url(action),
                                           {action.src, action.src_bytes},
                                           action.width,
                                           action.height,
                                           0,
                                           action.format,
                                           action.desired ? GifFavoriteMutationKind::add
                                                          : GifFavoriteMutationKind::remove};
        const auto encoded =
            gif_favorites_write_patch({p.bank->group, p.bank->group_bytes}, p.data_version, mutation,
                                      {write, scratch_bytes}, {write + scratch_bytes, json_bytes});
        if (!encoded.ok()) {
            dispose(write, write_bytes);
            mark_failure(
                p, utf8_literal("Couldn't update favorites. You may have reached the limit."),
                true);
            schedule(p);
            changed(p);
            return false;
        }
        patch = {write + scratch_bytes, encoded.bytes};
    }
    p.response_body = static_cast<u8*>(VirtualAlloc(nullptr, kRestClientMaximumResponseBytes,
                                                    MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!p.response_body) {
        dispose(write, write_bytes);
        mark_failure(p, utf8_literal("Couldn't load favorites. Try again."),
                     operation == AccountGifFavoritesOperation::update);
        p.due_at = 0;
        changed(p);
        return false;
    }
    const auto admitted = g_app.account.api.gif_favorites(g_app.account.instance, operation, patch, id);
    dispose(write, write_bytes);
    if (!admitted.ok()) {
        dispose(p.response_body, kRestClientMaximumResponseBytes);
        p.response_body = nullptr;
        p.admission_busy = admitted.error == RestClientSubmitError::busy;
        if (!p.admission_busy) {
            mark_failure(p, utf8_literal("Couldn't update favorites. Try again."),
                         operation == AccountGifFavoritesOperation::update);
            p.due_at = 0;
        }
        changed(p);
        return false;
    }
    p.request_id = id;
    p.operation = operation;
    p.submitted_generation = p.generation;
    p.due_at = 0;
    p.admission_busy = false;
    ++p.attempts;
    changed(p);
    return true;
}
bool application_gif_favorites_response(const RestClientResponse& response, Utf8View body) noexcept {
    auto* owner = g_app.gif_favorites;
    if (!owner || !owner->request_id || owner->request_id != response.request_id)
        return false;
    auto& p = *owner;
    const auto operation = p.operation;
    const bool current = scope(p) && p.submitted_generation == p.generation;
    const u64 now_ms = clock_monotonic_milliseconds();
    p.request_id = 0;
    p.admission_busy = false;
    p.receipt_consumed = true;
    if (!current) {
        // Stale writes have an ambiguous outcome. Discard this action; preserve
        // distinct queued choices for a fresh authoritative rebase.
        if (scope(p) && operation == AccountGifFavoritesOperation::update)
            mark_failure(
                p, utf8_literal("Favorites changed on another device. Try again."),
                true);
        p.authoritative = false;
        p.fetch_wanted = scope(p) && (p.visible || p.queue_count);
        schedule(p);
    } else if (response.transport_ok() && (response.rate_limited || response.status_code == 429) &&
               response.retry_at_ms > now_ms && p.attempts < 3) {
        p.due_at = response.retry_at_ms;
    } else if (response.transport_ok() && response.status_code >= 200 && response.status_code < 300) {
        bool conflict = false;
        auto* bank = decode_response(body, &conflict);
        if (bank) {
            const bool target_ok =
                operation != AccountGifFavoritesOperation::update ||
                (p.queue_count &&
                 (gif_favorites_find({bank->records, bank->record_count}, {bank->index, bank->index_words},
                                     url(p.queue[0])) != nullptr) == p.queue[0].desired);
            replace_bank(p, bank);
            p.fetch_wanted = false;
            if (operation == AccountGifFavoritesOperation::update && (conflict || !target_ok))
                mark_failure(
                    p,
                    utf8_literal("Favorites changed on another device. Try again."),
                    true);
            else {
                if (operation == AccountGifFavoritesOperation::update) {
                    if (p.queue_count && equal({p.failed_url, p.failed_url_bytes}, url(p.queue[0])))
                        p.failed_url_bytes = 0;
                    drop_action(p);
                }
                p.failed = false;
                p.status = {};
            }
            p.attempts = 0;
            schedule(p);
        } else {
            mark_failure(p, utf8_literal("Couldn't update favorites. Try again."),
                         operation == AccountGifFavoritesOperation::update);
            p.authoritative = false;
            p.fetch_wanted = false;
            p.due_at = 0;
            if (operation == AccountGifFavoritesOperation::update && p.queue_count)
                schedule(p);
        }
    } else {
        mark_failure(
            p,
            operation == AccountGifFavoritesOperation::update
                ? utf8_literal("Couldn't confirm the change. Check your favorites.")
                : utf8_literal("Couldn't load favorites. Try again."),
            operation == AccountGifFavoritesOperation::update);
        p.authoritative = false;
        p.fetch_wanted = false;
        p.due_at = 0;
        if (operation == AccountGifFavoritesOperation::update && p.queue_count)
            schedule(p);
    }
    changed(p);
    return true;
}
void application_gif_favorites_response_drained() noexcept {
    auto* p = g_app.gif_favorites;
    if (!p || p->request_id || !p->receipt_consumed)
        return;
    dispose(p->response_body, kRestClientMaximumResponseBytes);
    p->response_body = nullptr;
    p->receipt_consumed = false;
    if (!scope(*p))
        application_gif_favorites_synchronize();
}
Slice<u8> application_gif_favorites_response_buffer() noexcept {
    const auto* p = g_app.gif_favorites;
    return p && p->response_body ? Slice<u8>{p->response_body, kRestClientMaximumResponseBytes}
                                 : Slice<u8>{};
}
u64 application_gif_favorites_next_wake() noexcept {
    const auto* p = g_app.gif_favorites;
    return p && scope(*p) && online() && !p->request_id && !p->response_body && !p->admission_busy &&
                   g_app.message_history_request_owner == MessageHistoryRequestOwner::none
               ? p->due_at
               : 0;
}
u32 application_gif_favorites_owner_bytes() noexcept {
    const auto* p = g_app.gif_favorites;
    return p ? static_cast<u32>(sizeof(ApplicationGifFavorites)) + (p->bank ? p->bank->bytes : 0) +
                   (p->queue ? static_cast<u32>(sizeof(FavoriteAction)) * kQueueCapacity : 0) +
                   (p->response_body ? kRestClientMaximumResponseBytes : 0)
             : 0;
}
} // namespace maicord::app_detail
