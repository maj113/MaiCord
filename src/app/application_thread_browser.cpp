#include "application_thread_browser.hpp"
#include "maicord/core/memory.hpp"
#include "application_content_point.hpp"
#include "rest_rate_retry.hpp"
#include "application_state.hpp"
#include "application_composer_text_input.hpp"
#include "application_content_input.hpp"
#include "application_composer_input.hpp"
#include "application_message_interaction.hpp"
#include "application_search.hpp"
#include "application_pinned_messages.hpp"
#include "application_settings.hpp"
#include "application_friends.hpp"
#include "application_forum_post.hpp"
#include "application_forum_form.hpp"
#include "application_retained_projection.hpp"
#include "maicord/protocol/thread_browser.hpp"
#include "maicord/protocol/forum_post.hpp"
#include "maicord/ui/timestamp_format.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"
#include "maicord/app/shortcut_registry.hpp"
#include "application_account_access.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/ui/thread_browser_scene.hpp"

namespace maicord::app_detail {
namespace {
struct BrowserScope {
    u64 account_scope, profile, self, guild, parent, underlying_channel;
    const void* instance;
    const AccountUiRuntime* ui;
    u16 generation;
};
struct BrowserBank {
    ThreadBrowserPage page;
    u8* storage;
    u32 committed;
};
struct BrowserFlight {
    BrowserScope scope;
    ThreadBrowserRequest request;
    u64 id, choice, gateway_serial;
    char name[kThreadBrowserNameMaximumBytes];
    bool active, retired, cancel_requested;
};
constexpr u32 bank_row_bytes = kThreadBrowserSearchMaximumRows * sizeof(ThreadBrowserRow);
constexpr u32 bank_name_bytes = kThreadBrowserSearchMaximumRows * kThreadBrowserNameMaximumBytes;
constexpr u32 bank_bytes = (bank_row_bytes + bank_name_bytes + 4095) & ~4095u;
// Active fallback pages (25 rows each) fetched per choice before it settles.
constexpr u32 kThreadBrowserActiveFetchLimit = 10 * kThreadBrowserArchivedLimit;
// Post metadata from every accepted page, keyed by thread ID (oldest evicted).
// Gateway records carry no tags, counts or starter previews, so forum parents
// always load their active posts over REST to fill this cache.
constexpr u32 kForumPostCacheCapacity = 256;
constexpr u32 kForumOrderCapacity = 1024;
struct ForumPostEntry {
    ThreadBrowserPost post;
    u64 stamp;
    char text[kThreadBrowserPostTextMaximumBytes];
};
constexpr u32 forum_cache_bytes = (kForumPostCacheCapacity * sizeof(ForumPostEntry) + 65535) & ~65535u;
constexpr u32 forum_scratch_post_bytes = kThreadBrowserSearchMaximumRows * sizeof(ThreadBrowserPost);
constexpr u32 forum_scratch_bytes = (forum_scratch_post_bytes +
    kThreadBrowserSearchMaximumRows * kThreadBrowserPostTextMaximumBytes + 65535) & ~65535u;
struct ForumMetaFlight {
    BrowserScope scope;
    u64 id;
    bool active, retired, cancel_requested;
};
// Allocated when a forum/media parent first opens; caches stay reserved and
// commit only as posts arrive.
struct ForumState {
    u64 parent, account_scope, stamp, activity_refresh_s, meta_due;
    ForumPostEntry* cache;
    u8* scratch;
    u32 cache_count, cache_committed, scratch_committed, revision, meta_busy, selected_count;
    ForumMetaFlight meta_flight;
    bool meta_queued, meta_loaded, media;
    UiForumSort sort;
    UiForumLayout layout;
    UiForumTagMatch match;
    u64 selected[kUiForumTagCapacity];
    UiForumTag tags[kUiForumTagCapacity];
    UiForumPost posts[kUiThreadBrowserPageCapacity];
    char activity[kUiThreadBrowserPageCapacity][48];
    UiForumChannelView view;
    UiForumSortKey keys[kForumOrderCapacity];
    ForumParentSnapshot meta;
};
}
struct ApplicationThreadBrowser {
    BrowserScope scope;
    BrowserScope pressed_scope;
    BrowserFlight flight;
    BrowserBank banks[2];
    ForumState* forum;
    UiThreadBrowserScene scene;
    UiThreadBrowserView view;
    UiThreadBrowserRow rows[kUiThreadBrowserPageCapacity];
    UiThreadBrowserTarget pressed;
    // The thread-name query submitted from the header search bar.
    char query[kThreadBrowserNameMaximumBytes];
    // session identifies one open..close span for the header search scope.
    u64 choice, publication, gateway_serial, actor_floor, due, session;
    u32 revision, projected_channels, projected_permissions, page_index, total, row_count, query_bytes;
    u32 held_key, busy_count, projected_forum;
    // Active-tab REST fallback when the Gateway active inventory is not synced.
    u32 active_offset, active_retries, indexing_retries;
    bool active_requested, active_loaded;
    float thumb_grab;
    UiThreadBrowserTab tab;
    Utf8View status;
    u8 bank;
    bool panel, queued, error, has_page, has_more, captured, release_pending, scrollbar_dragging;
    // Forum (15) or media (16) parent: rows are presented as post cards.
    bool forum_mode;
};
static_assert(sizeof(ApplicationThreadBrowser) < 8192);
namespace {
ApplicationThreadBrowser* browser_owner{};
u64 browser_publication{}, browser_session{};
u64 default_parent() noexcept {
    const auto* selected = g_app.account_ui ? application_channel_record(g_app.account_ui->active_channel_id) : nullptr;
    return selected ? (channel_is_thread(selected->type) ? selected->parent_id : selected->id) : 0;
}
bool actor_current(const BrowserScope& scope) noexcept {
    return g_app.account_ui && !g_app.login.visible && scope.ui == g_app.account_ui &&
        scope.instance == g_app.account.instance && scope.account_scope == g_app.plugin_account_scope_serial &&
        scope.profile == g_app.account.status.profile_id && scope.self == g_app.account_ui->chat_state.self_user_id;
}
bool parent_readable(u64 id) noexcept {
    const auto* parent = application_channel_record(id);
    if (!parent || !parent->occupied || !parent->guild_id || !channel_is_thread_parent(parent->type) ||
        !parent->permissions_resolved || (parent->flags & kChannelObfuscatedFlag)) return false;
    constexpr u32 required = channel_action_view | channel_action_read_history;
    const auto& state = g_app.account_ui->chat_state;
    const auto* guild = chat_state_store_find_guild(state, parent->guild_id);
    return guild && guild->occupied && !guild->unavailable &&
        (parent->actions & required) == required && chat_state_store_channel_visible(state, *parent);
}
bool current(const ApplicationThreadBrowser& value) noexcept {
    const auto* parent = actor_current(value.scope) ? application_channel_record(value.scope.parent) : nullptr;
    return value.panel && parent && parent_readable(parent->id) && parent->generation == value.scope.generation &&
        parent->guild_id == value.scope.guild && g_app.account_ui->active_channel_id == value.scope.underlying_channel &&
        g_app.account_ui->chat_state.selected_guild_id == value.scope.guild;
}
// No settings page, form, popup, viewer or profile covers the browser's host view.
bool uncovered() noexcept {
    return !application_settings_active() && !g_app.text_form && !g_app.popup_scene.active &&
        !g_app.image_viewer.scene.active && !g_app.reaction_viewer.active &&
        !(g_app.renderer.profile_scene && g_app.renderer.profile_scene->active);
}
bool front() noexcept {
    return application_thread_browser_active() && !application_forum_form_active() && uncovered();
}
void invalidate(ApplicationThreadBrowser& value) noexcept {
    value.publication = ++browser_publication;
    ++value.revision;
    value.row_count = 0;
    value.has_page = false;
    value.projected_channels = ~u32{0};
    value.projected_permissions = ~u32{0};
}
void clear_capture(ApplicationThreadBrowser& value) noexcept {
    reset_value(&value.pressed);
    reset_value(&value.pressed_scope);
    value.scrollbar_dragging = false;
    const bool captured = value.captured;
    value.captured = false;
    if (captured && GetCapture() == g_app.window) ReleaseCapture();
}
void cancel_flight(ApplicationThreadBrowser& value) noexcept {
    auto& flight = value.flight;
    if (!flight.active) return;
    flight.retired = true;
    // Cancellation only stops the result from being shown. Keep the request in
    // flight until its response is consumed, including a late rate-limit observation.
    if (!flight.cancel_requested && flight.scope.instance && flight.scope.instance == g_app.account.instance &&
        g_app.account.api.thread_browser) {
        g_app.account.api.thread_browser(g_app.account.instance, nullptr, flight.id);
        flight.cancel_requested = true;
    }
}
void free_owner() noexcept {
    auto* value = browser_owner;
    if (!value) return;
    renderer_bind_thread_browser_scene(&g_app.renderer, nullptr);
    ui_thread_browser_scene_release(&value->scene);
    for (auto& bank : value->banks) if (bank.storage) VirtualFree(bank.storage, 0, MEM_RELEASE);
    if (auto* forum = value->forum) {
        if (forum->cache) VirtualFree(forum->cache, 0, MEM_RELEASE);
        if (forum->scratch) VirtualFree(forum->scratch, 0, MEM_RELEASE);
        VirtualFree(forum, 0, MEM_RELEASE);
    }
    VirtualFree(value, 0, MEM_RELEASE);
    browser_owner = nullptr;
}
bool ensure_owner() noexcept {
    if (browser_owner) return true;
    browser_owner = static_cast<ApplicationThreadBrowser*>(VirtualAlloc(nullptr, sizeof(ApplicationThreadBrowser), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    return browser_owner != nullptr;
}
bool ensure_bank(BrowserBank& bank, u32 count) noexcept {
    if (!bank.storage) bank.storage = static_cast<u8*>(VirtualAlloc(nullptr, bank_bytes, MEM_RESERVE, PAGE_READWRITE));
    if (!bank.storage) return false;
    const u32 bytes = (count * sizeof(ThreadBrowserRow) + count * kThreadBrowserNameMaximumBytes + 4095) & ~4095u;
    if (bytes > bank.committed) {
        if (!VirtualAlloc(bank.storage + bank.committed, bytes - bank.committed, MEM_COMMIT, PAGE_READWRITE)) return false;
        bank.committed = bytes;
    }
    return true;
}
bool repaint() noexcept {
    application_request_frame();
    return application_thread_browser_synchronize() && refresh_retained_projection();
}
// The Active tab normally projects the Gateway inventory (THREAD_LIST_SYNC).
// Until that inventory is synced for the parent, nothing guarantees it will
// ever arrive, so the tab asks REST for the parent's active threads instead of
// waiting on it.
bool active_fetch_needed(const ApplicationThreadBrowser& value) noexcept {
    const auto* parent = value.tab == UiThreadBrowserTab::active ? application_channel_record(value.scope.parent) : nullptr;
    // Forum cards need REST post metadata even when the Gateway inventory is synced.
    return parent && (!parent->thread_listing_synced || (value.forum_mode && !value.active_loaded));
}
void choose(ApplicationThreadBrowser& value, UiThreadBrowserTab tab, u32 page) noexcept {
    cancel_flight(value);
    ++value.choice;
    value.tab = tab;
    value.page_index = page;
    value.active_offset = value.active_retries = 0;
    value.active_loaded = false;
    value.queued = tab != UiThreadBrowserTab::active || active_fetch_needed(value);
    value.active_requested = tab == UiThreadBrowserTab::active && value.queued;
    value.error = false;
    reset_value(&value.status);
    value.has_more = false;
    value.due = value.actor_floor;
    value.busy_count = 0;
    value.indexing_retries = 0;
    invalidate(value);
}
void fail(ApplicationThreadBrowser& value, Utf8View notice) noexcept {
    value.queued = false;
    value.error = true;
    value.status = notice;
    ++value.revision;
}
// A failed Active refresh keeps saved rows usable (an error disables them);
// with nothing saved, the tab shows the error and its Retry control.
void fail_choice(ApplicationThreadBrowser& value, Utf8View notice, Utf8View saved_notice) noexcept {
    if (value.tab != UiThreadBrowserTab::active || !value.row_count) return fail(value, notice);
    value.queued = false;
    value.status = saved_notice;
    ++value.revision;
}
bool eligible(const ApplicationThreadBrowser& value, const ChannelRecord& row, bool active) noexcept {
    return row.occupied && row.guild_id == value.scope.guild && row.parent_id == value.scope.parent &&
        channel_is_thread(row.type) && !(row.flags & kChannelObfuscatedFlag) &&
        (!active || (row.thread_active_known && row.thread_listed_active)) &&
        chat_state_store_thread_accessible(g_app.account_ui->chat_state, row);
}
UiThreadBrowserRow thread_row(const ChatStateStore& state, const ChannelRecord& row) noexcept {
    return {row.id, chat_state_store_channel_name(state, row), row.thread_archived_known && row.thread_archived,
        row.thread_self_joined, row.thread_self_joined_known, row.type};
}
void project(ApplicationThreadBrowser& value) noexcept {
    const auto& state = g_app.account_ui->chat_state;
    if (value.projected_channels == state.channel_mutations && value.projected_permissions == state.permission_mutations) return;
    value.projected_channels = state.channel_mutations;
    value.projected_permissions = state.permission_mutations;
    value.publication = ++browser_publication;
    value.row_count = 0;
    value.total = 0;
    const u32 first = value.page_index * kUiThreadBrowserPageCapacity;
    const auto take = [&](const ChannelRecord& row) noexcept {
        const u32 index = value.total++;
        if (index < first || value.row_count == kUiThreadBrowserPageCapacity) return;
        value.rows[value.row_count++] = thread_row(state, row);
    };
    if (value.tab == UiThreadBrowserTab::active) {
        // Count and project one page directly. No fixed-size copy or truncation
        // of the Core active inventory, and no unchanged-frame iteration.
        for (u32 i = 0; i < state.channel_count; ++i) {
            const auto& row = state.channels[state.channel_order[i]];
            if (eligible(value, row, true)) take(row);
        }
        if (value.page_index && first >= value.total) {
            value.page_index = value.total ? (value.total - 1) / kUiThreadBrowserPageCapacity : 0;
            value.projected_channels = ~u32{0};
            project(value);
            return;
        }
        value.has_page = true;
        value.has_more = first + value.row_count < value.total;
    } else if (value.has_page) {
        const auto& page = value.banks[value.bank].page;
        // A Gateway deletion/private-membership withdrawal wins immediately.
        // Names come from freshly authorized Core records, never stale banks.
        for (u32 i = 0; i < page.row_count; ++i) {
            const auto* row = application_channel_record(page.rows.data[i].id);
            if (!row || !eligible(value, *row, false)) continue;
            if (value.tab == UiThreadBrowserTab::search) take(*row);
            else if (value.row_count < kUiThreadBrowserPageCapacity) value.rows[value.row_count++] = thread_row(state, *row);
        }
        if (value.tab == UiThreadBrowserTab::search) value.has_more = first + value.row_count < value.total;
    }
    ++value.revision;
}
bool forum_commit(u8* base, u32* committed, u32 bytes, u32 reserve) noexcept {
    bytes = (bytes + 65535) & ~65535u;
    if (bytes > reserve) bytes = reserve;
    if (bytes <= *committed) return true;
    if (!VirtualAlloc(base + *committed, bytes - *committed, MEM_COMMIT, PAGE_READWRITE)) return false;
    *committed = bytes;
    return true;
}
ForumPostEntry* forum_find(ForumState& forum, u64 id) noexcept {
    for (u32 i = 0; i < forum.cache_count; ++i) if (forum.cache[i].post.thread_id == id) return forum.cache + i;
    return nullptr;
}
void forum_cancel_meta(ForumState& forum) noexcept {
    auto& flight = forum.meta_flight;
    if (!flight.active) return;
    flight.retired = true;
    if (!flight.cancel_requested && flight.scope.instance && flight.scope.instance == g_app.account.instance &&
        g_app.account.api.forum_post) {
        g_app.account.api.forum_post(g_app.account.instance, nullptr, flight.id);
        flight.cancel_requested = true;
    }
}
bool forum_meta_active(const ApplicationThreadBrowser& value) noexcept {
    return value.forum && value.forum->meta_flight.active;
}
// Keeps the state of the same forum (cache, tag filter, sort, layout) across reopen.
bool forum_prepare(ApplicationThreadBrowser& value, const ChannelRecord& parent) noexcept {
    if (!value.forum) {
        value.forum = static_cast<ForumState*>(VirtualAlloc(nullptr, sizeof(ForumState), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!value.forum) return false;
    }
    auto& forum = *value.forum;
    if (!forum.cache) forum.cache = static_cast<ForumPostEntry*>(VirtualAlloc(nullptr, forum_cache_bytes, MEM_RESERVE, PAGE_READWRITE));
    if (!forum.scratch) forum.scratch = static_cast<u8*>(VirtualAlloc(nullptr, forum_scratch_bytes, MEM_RESERVE, PAGE_READWRITE));
    if (!forum.cache || !forum.scratch) return false;
    if (forum.parent == parent.id && forum.account_scope == value.scope.account_scope) return true;
    // A metadata request for the previous forum stays in flight until consumed.
    forum_cancel_meta(forum);
    forum.parent = parent.id;
    forum.account_scope = value.scope.account_scope;
    forum.cache_count = forum.selected_count = forum.meta_busy = 0;
    forum.meta_due = forum.activity_refresh_s = 0;
    forum.meta_loaded = false;
    forum.meta_queued = true;
    forum.media = parent.type == 16;
    forum.layout = forum.media ? UiForumLayout::gallery : UiForumLayout::list;
    forum.sort = UiForumSort::latest_activity;
    forum.match = UiForumTagMatch::some;
    ++forum.revision;
    return true;
}
void forum_remember(ForumState& forum, const ThreadBrowserPostPage& page) noexcept {
    for (u32 i = 0; i < page.post_count; ++i) {
        const auto& post = page.posts[i];
        ForumPostEntry* entry = forum_find(forum, post.thread_id);
        if (!entry && forum.cache_count < kForumPostCacheCapacity &&
            forum_commit(reinterpret_cast<u8*>(forum.cache), &forum.cache_committed,
                (forum.cache_count + 1) * static_cast<u32>(sizeof(ForumPostEntry)), forum_cache_bytes))
            entry = forum.cache + forum.cache_count++;
        if (!entry) {
            if (!forum.cache_count) continue;
            entry = forum.cache;
            for (u32 at = 1; at < forum.cache_count; ++at)
                if (forum.cache[at].stamp < entry->stamp) entry = forum.cache + at;
        }
        // The cache keeps the four text parts contiguous from offset zero.
        const Utf8View parts[]{thread_browser_post_preview(page, i), thread_browser_post_author_name(page, i),
            thread_browser_post_avatar_hash(page, i), thread_browser_post_media_url(page, i)};
        entry->post = post;
        entry->post.text_offset = 0;
        u32 at = 0;
        for (const auto& part : parts) { CopyMemory(entry->text + at, part.data, part.bytes); at += part.bytes; }
        entry->stamp = ++forum.stamp;
    }
    if (page.post_count) ++forum.revision;
}
void forum_card(ApplicationThreadBrowser& value, const ChannelRecord& row, u32 slot, u64 now_s) noexcept {
    auto& forum = *value.forum;
    auto& post = forum.posts[slot];
    reset_value(&post);
    post.thread_id = row.id;
    post.pinned = (row.flags & kThreadBrowserPinnedFlag) != 0;
    u64 created_ms = (row.id >> 22) + kSnowflakeEpochMilliseconds;
    if (const auto* entry = forum_find(forum, row.id)) {
        const auto& meta = entry->post;
        post.pinned |= (meta.fields & thread_browser_post_flags) && (meta.flags & kThreadBrowserPinnedFlag);
        post.metadata_known = (meta.fields & thread_browser_post_message_count) != 0;
        post.reply_count = meta.message_count;
        post.tag_count = static_cast<u8>(minimum<u32>(meta.tag_count, kUiForumPostTagCapacity));
        for (u32 i = 0; i < post.tag_count; ++i) post.tags[i] = meta.tags[i];
        post.preview = {entry->text, meta.preview_bytes};
        post.author_name = {entry->text + meta.preview_bytes, meta.author_name_bytes};
        post.author_id = (meta.fields & thread_browser_post_author) ? meta.author_id : meta.owner_id;
        if (post.author_id == meta.author_id)
            post.avatar_hash = {entry->text + meta.preview_bytes + meta.author_name_bytes, meta.avatar_hash_bytes};
        // Static stills only (PNG, JPEG, WebP); a GIF keeps the neutral media well.
        const auto format = meta.media_type == ThreadBrowserMediaType::png ? MediaAttachmentFormat::png
            : meta.media_type == ThreadBrowserMediaType::jpeg ? MediaAttachmentFormat::jpeg
            : meta.media_type == ThreadBrowserMediaType::webp ? MediaAttachmentFormat::webp
                                                                     : MediaAttachmentFormat::unknown;
        post.has_media = (meta.fields & thread_browser_post_media) && format != MediaAttachmentFormat::unknown &&
            meta.media_url_bytes && meta.media_id;
        if (post.has_media) {
            post.media_url = {entry->text + meta.preview_bytes + meta.author_name_bytes + meta.avatar_hash_bytes,
                meta.media_url_bytes};
            post.media_id = meta.media_id;
            post.media_width = meta.media_width;
            post.media_height = meta.media_height;
            post.media_format = static_cast<u8>(format);
        }
        if (meta.fields & thread_browser_post_created) created_ms = meta.create_timestamp_ms;
    }
    // The author's current profile name wins over the response's snapshot.
    if (post.author_id) {
        const auto& users = g_app.account_ui->user_store;
        if (const auto* user = user_store_find(users, post.author_id)) {
            const auto name = user_store_name(users, *user);
            if (name.bytes) post.author_name = name;
            post.avatar_hash = user_store_avatar_hash(*user);
        }
    }
    const u64 activity_ms =
        row.last_message_id ? (row.last_message_id >> 22) + kSnowflakeEpochMilliseconds : created_ms;
    const auto formatted = format_timestamp_local(activity_ms / 1000, 'R', now_s,
        {forum.activity[slot], sizeof(forum.activity[slot])});
    if (formatted.ok) {
        post.activity = {forum.activity[slot], formatted.bytes};
        if (formatted.next_update_unix_seconds && (!forum.activity_refresh_s ||
            formatted.next_update_unix_seconds < forum.activity_refresh_s))
            forum.activity_refresh_s = formatted.next_update_unix_seconds;
    }
}
// Forum projection: filter by the selected tags, order like the official client (pinned,
// then latest activity or creation), then page. Active and Search page the
// whole ordered set; an Archived server page is filtered and ordered in place.
void project_forum(ApplicationThreadBrowser& value) noexcept {
    auto& forum = *value.forum;
    const auto& state = g_app.account_ui->chat_state;
    const u64 now_s = clock_unix_milliseconds() / 1000;
    if (value.projected_channels == state.channel_mutations && value.projected_permissions == state.permission_mutations &&
        value.projected_forum == forum.revision) {
        if (!forum.activity_refresh_s || now_s < forum.activity_refresh_s) return;
        // Only relative times moved on: same rows and publication (no scroll or
        // focus reset), fresh activity labels.
        forum.activity_refresh_s = 0;
        for (u32 i = 0; i < value.row_count; ++i)
            if (const auto* row = application_channel_record(value.rows[i].thread_id)) forum_card(value, *row, i, now_s);
        ++value.revision;
        return;
    }
    value.projected_channels = state.channel_mutations;
    value.projected_permissions = state.permission_mutations;
    value.projected_forum = forum.revision;
    value.publication = ++browser_publication;
    value.row_count = 0;
    value.total = 0;
    forum.activity_refresh_s = 0;
    u32 count = 0;
    const ConstSlice<u64> selected{forum.selected, forum.selected_count};
    const auto consider = [&](const ChannelRecord& row) noexcept {
        const auto* entry = forum_find(forum, row.id);
        const ConstSlice<u64> applied = entry ? ConstSlice<u64>{entry->post.tags, entry->post.tag_count} : ConstSlice<u64>{};
        if (count == kForumOrderCapacity || !ui_forum_post_matches(applied, selected, forum.match)) return;
        const bool pinned = (row.flags & kThreadBrowserPinnedFlag) ||
            (entry && (entry->post.flags & kThreadBrowserPinnedFlag));
        forum.keys[count] = {row.id, row.last_message_id, count, pinned, {}};
        ++count;
    };
    if (value.tab == UiThreadBrowserTab::active) {
        for (u32 i = 0; i < state.channel_count; ++i) {
            const auto& row = state.channels[state.channel_order[i]];
            if (eligible(value, row, true)) consider(row);
        }
        value.has_page = true;
    } else if (value.has_page) {
        const auto& page = value.banks[value.bank].page;
        for (u32 i = 0; i < page.row_count; ++i) {
            const auto* row = application_channel_record(page.rows.data[i].id);
            if (row && eligible(value, *row, false)) consider(*row);
        }
    }
    ui_forum_sort({forum.keys, count}, forum.sort);
    value.total = count;
    const bool paged = value.tab != UiThreadBrowserTab::archived;
    if (paged && value.page_index && value.page_index * kUiThreadBrowserPageCapacity >= count)
        value.page_index = count ? (count - 1) / kUiThreadBrowserPageCapacity : 0;
    const u32 first = paged ? value.page_index * kUiThreadBrowserPageCapacity : 0;
    for (u32 k = first; k < count && value.row_count < kUiThreadBrowserPageCapacity; ++k) {
        const auto* row = application_channel_record(forum.keys[k].thread_id);
        if (!row) continue;
        value.rows[value.row_count] = thread_row(state, *row);
        forum_card(value, *row, value.row_count, now_s);
        ++value.row_count;
    }
    if (paged) value.has_more = first + value.row_count < value.total;
    ++value.revision;
}
// Forum tags and presentation defaults come from the parent channel (GET
// /channels/{id}, the post composer's metadata request); a failure only
// hides the tag chips.
bool forum_meta_due(const ApplicationThreadBrowser& value, u64 now) noexcept {
    return value.forum_mode && value.forum && value.forum->meta_queued && !value.forum->meta_flight.active &&
        !value.flight.active && g_app.account.api.forum_post && now >= maximum(value.forum->meta_due, value.actor_floor);
}
bool forum_submit_meta(ApplicationThreadBrowser& value, u64 id, u64 now) noexcept {
    auto& forum = *value.forum;
    ForumPostRequest request{};
    request.parent_id = value.scope.parent;
    request.guild_id = value.scope.guild;
    request.operation = ForumPostOperation::parent_metadata;
    if (!forum_post_request_valid(request)) { forum.meta_queued = false; repaint(); return false; }
    const auto result = g_app.account.api.forum_post(g_app.account.instance, &request, id);
    if (!result.ok()) {
        if (result.error == RestClientSubmitError::busy && ++forum.meta_busy <= 20) forum.meta_due = deadline_after(now, 50);
        else forum.meta_queued = false;
        repaint(); return false;
    }
    forum.meta_flight = {value.scope, id, true, false, false};
    forum.meta_queued = false;
    repaint(); return true;
}
bool forum_meta_response(ApplicationThreadBrowser& value, const RestClientResponse& response, Utf8View body) noexcept {
    auto& forum = *value.forum;
    const auto flight = forum.meta_flight;
    reset_value(&forum.meta_flight);
    const u64 retry_at = rest_rate_retry_deadline(response);
    if (actor_current(flight.scope) && retry_at)
        value.actor_floor = maximum(value.actor_floor, retry_at);
    const bool same = !flight.retired && current(value) && value.forum_mode && forum.parent == flight.scope.parent &&
        flight.scope.parent == value.scope.parent && flight.scope.generation == value.scope.generation;
    if (!same) {
        if (value.release_pending && !value.flight.active) free_owner(); else repaint();
        return true;
    }
    ForumPostRequest request{};
    request.parent_id = flight.scope.parent;
    request.guild_id = flight.scope.guild;
    request.operation = ForumPostOperation::parent_metadata;
    ForumParentSnapshot candidate{};
    if (retry_at && forum.meta_busy++ < 2) forum.meta_queued = true;
    else if (response.transport_ok() && response.status_code >= 200 && response.status_code < 300 && body.data &&
        body.bytes == response.body_bytes && body.bytes <= kForumPostMaximumResponseBytes &&
        forum_parent_decode(body, request,
            {g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount}, &candidate).ok() &&
        candidate.type == application_channel_record(value.scope.parent)->type) {
        forum.meta = candidate;
        forum.meta_loaded = true;
        if (candidate.fields & forum_parent_default_sort_order)
            forum.sort = candidate.default_sort_order ? UiForumSort::creation_date : UiForumSort::latest_activity;
        if ((candidate.fields & forum_parent_default_forum_layout) && candidate.default_forum_layout)
            forum.layout = candidate.default_forum_layout == 2 ? UiForumLayout::gallery : UiForumLayout::list;
        if (candidate.fields & forum_parent_default_tag_setting)
            forum.match = candidate.default_tag_setting ? UiForumTagMatch::all : UiForumTagMatch::some;
        // A remembered tag filter keeps only tags the forum still offers.
        u32 kept = 0;
        for (u32 i = 0; i < forum.selected_count; ++i)
            for (u32 j = 0; j < candidate.tag_count; ++j)
                if (candidate.tags[j].id == forum.selected[i]) { forum.selected[kept++] = forum.selected[i]; break; }
        forum.selected_count = kept;
        ++forum.revision;
        ++value.revision;
    }
    repaint(); return true;
}
struct BatchContext {
    BrowserBank* bank;
    u32 indices[kThreadBrowserSearchMaximumRows];
    u32 count;
    bool active;
};
bool read_input(const void* opaque, u32 index, ChannelInput* output) noexcept {
    const auto& context = *static_cast<const BatchContext*>(opaque);
    if (!output || index >= context.count) return false;
    const u32 raw = context.indices[index];
    const auto& row = context.bank->page.rows.data[raw];
    *output = {row.id, row.guild_id, row.parent_id, thread_browser_row_name(context.bank->page, raw), 0, 0, row.type};
    const auto* old = application_channel_record(row.id);
    if (old) { output->position = old->position; output->flags = old->flags; }
    output->name_known = true;
    output->last_message_id = row.last_message_id;
    output->last_message_known = (row.fields & thread_browser_last_message_known) != 0;
    output->thread_archived_known = (row.fields & thread_browser_archived_known) != 0;
    output->thread_archived = (row.fields & thread_browser_archived) != 0;
    // Positive archived evidence excludes an old active observation. Search
    // matches alone never establish a complete positive active inventory.
    if (output->thread_archived_known && output->thread_archived) {
        output->thread_active_known = true;
        output->thread_listed_active = false;
    } else if (context.active && output->thread_archived_known) {
        // A row from the server's active listing is positive active evidence.
        output->thread_active_known = true;
        output->thread_listed_active = true;
    }
    output->thread_locked_known = (row.fields & thread_browser_locked_known) != 0;
    output->thread_locked = (row.fields & thread_browser_locked) != 0;
    // An omitted membership object is unknown. It cannot clear existing proof.
    output->thread_self_joined_known = (row.fields & thread_browser_self_joined) != 0;
    output->thread_self_joined = output->thread_self_joined_known;
    if (output->thread_self_joined)
        output->thread_join_seconds = thread_join_seconds_from_ms(row.self_join_timestamp_ms);
    return true;
}
bool apply_page(ApplicationThreadBrowser& value, BrowserBank& bank, const BrowserFlight& flight) noexcept {
    BatchContext context{&bank, {}, 0, flight.request.mode == ThreadBrowserMode::active};
    const bool changed = flight.gateway_serial != value.gateway_serial;
    for (u32 i = 0; i < bank.page.row_count; ++i) {
        const auto& row = bank.page.rows.data[i];
        const auto* old = application_channel_record(row.id);
        if (old && (old->guild_id != value.scope.guild || old->parent_id != value.scope.parent || old->type != row.type)) return false;
        // A later matching Gateway update takes over this scope. Existing rows
        // are resolved from Core; unknown/deleted rows are not resurrected.
        if (!changed) context.indices[context.count++] = i;
    }
    auto& state = g_app.account_ui->chat_state;
    if (chat_state_store_preflight_channel_batch(state, context.count, read_input, &context) != ChatStateStoreError::none) return false;
    for (u32 i = 0; i < context.count; ++i) {
        ChannelInput input{};
        if (!read_input(&context, i, &input) || !chat_state_store_upsert_channel(&state, input).ok()) return false;
    }
    if (context.count) chat_state_store_recompute_guild_permissions(&state, value.scope.guild);
    return true;
}
}

bool application_thread_browser_available(u64 parent_id) noexcept {
    if (!g_app.account_ui || g_app.login.visible || application_friends_active() || !g_app.plugin_account_scope_serial ||
        !g_app.account.status.profile_id || !g_app.account_ui->chat_state.self_user_id) return false;
    const u64 id = parent_id ? parent_id : default_parent();
    const auto* parent = application_channel_record(id);
    return parent_readable(id) && parent && g_app.account_ui->chat_state.selected_guild_id == parent->guild_id;
}
bool application_thread_browser_active() noexcept { return browser_owner && current(*browser_owner); }
bool application_thread_browser_read_blocked() noexcept { return application_thread_browser_active(); }
bool application_thread_browser_open(u64 parent_id) noexcept {
    const u64 id = parent_id ? parent_id : default_parent();
    if (!application_thread_browser_available(id)) return false;
    if (!application_search_close_panel()) return false;
    application_pinned_messages_close();
    if (!ensure_owner()) return false;
    auto& value = *browser_owner;
    const auto* parent = application_channel_record(id);
    BrowserScope next{g_app.plugin_account_scope_serial, g_app.account.status.profile_id, g_app.account_ui->chat_state.self_user_id,
        parent->guild_id, id, g_app.account_ui->active_channel_id, g_app.account.instance, g_app.account_ui, parent->generation};
    if (!actor_current(value.scope)) value.actor_floor = 0;
    value.scope = next;
    value.forum_mode = (parent->type == 15 || parent->type == 16) && forum_prepare(value, *parent);
    value.panel = true;
    value.release_pending = false;
    value.session = ++browser_session;
    choose(value, UiThreadBrowserTab::active, 0);
    end_message_composer_pointer_selection();
    end_message_text_selection();
    ui_scroll_host_cancel(&g_app.scroll_host);
    if (GetCapture() == g_app.window) ReleaseCapture();
    application_composer_text_input_suspend();
    g_app.message_composer.focused = false;
    reset_value(&g_app.message_action_bar);
    if (!repaint()) return false;
    g_app.context_focus = &value.scene.widgets;
    return true;
}
bool application_thread_browser_toggle() noexcept {
    if (application_thread_browser_active()) { application_thread_browser_close(); return repaint(); }
    return application_thread_browser_open();
}
void application_thread_browser_close() noexcept {
    renderer_bind_thread_browser_scene(&g_app.renderer, nullptr);
    if (!browser_owner) return;
    auto& value = *browser_owner;
    cancel_flight(value);
    if (value.forum) forum_cancel_meta(*value.forum);
    value.panel = false;
    value.queued = false;
    clear_capture(value);
    invalidate(value);
    if (g_app.context_focus == &value.scene.widgets) g_app.context_focus = nullptr;
    application_request_frame();
}
void application_thread_browser_release() noexcept {
    application_thread_browser_close();
    if (!browser_owner) return;
    browser_owner->release_pending = true;
    // A stopped/unloaded account has joined its worker before this teardown
    // boundary. Ordinary Close never uses this shortcut.
    if (!g_app.account.instance || (!g_app.account.status.running && !g_app.account.stop_failed)) {
        reset_value(&browser_owner->flight);
        if (browser_owner->forum) reset_value(&browser_owner->forum->meta_flight);
    }
    if (!browser_owner->flight.active && !forum_meta_active(*browser_owner)) free_owner();
}
UiRect application_thread_browser_viewport() noexcept {
    auto viewport = ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::content);
    viewport.top = ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::title_bar).bottom;
    viewport.right = static_cast<float>(g_app.renderer.width) * 96.f / maximum(g_app.renderer.dpi, 1u);
    return viewport;
}
bool application_thread_browser_synchronize() noexcept {
    auto* value = browser_owner;
    if (!value) { renderer_bind_thread_browser_scene(&g_app.renderer, nullptr); return true; }
    if (value->captured && value->pressed.thread_id == ~u64{0}) {
        const auto* parent = application_channel_record(value->pressed_scope.parent);
        if (!actor_current(value->pressed_scope) || !parent || !parent_readable(parent->id) ||
            parent->generation != value->pressed_scope.generation || default_parent() != value->pressed_scope.parent ||
            !ui_widget_manager_input_enabled(g_app.shell_scene.widgets, value->pressed.widget)) clear_capture(*value);
    }
    if (!current(*value)) {
        if (value->panel) application_thread_browser_close();
        renderer_bind_thread_browser_scene(&g_app.renderer, nullptr);
        return true;
    }
    if (!front()) clear_capture(*value);
    const auto* parent = application_channel_record(value->scope.parent);
    if (value->tab == UiThreadBrowserTab::active && !value->flight.active) {
        if (parent->thread_listing_synced && (!value->forum_mode || value->active_loaded)) {
            // The Gateway inventory arrived first; it is authoritative.
            value->queued = false;
        } else if (!value->active_requested && !value->queued) {
            // The inventory became unsynced while the tab was open.
            value->active_requested = value->queued = true;
            value->active_offset = 0;
            value->active_loaded = false;
            reset_value(&value->status);
            ++value->revision;
        }
    }
    if (value->forum_mode && value->forum) project_forum(*value);
    else project(*value);
    if (value->queued && g_app.account.status.required_action != AccountRequirement::none)
        fail_choice(*value, utf8_literal("Verify your account to view threads."),
            utf8_literal("Showing saved threads. Verify your account to refresh."));
    if (value->queued && (!g_app.account.status.running || g_app.account.stop_failed ||
        !g_app.account.instance || !g_app.account.api.thread_browser))
        fail_choice(*value, utf8_literal("Can't load threads while offline."),
            utf8_literal("Showing saved threads. Can't refresh while offline."));
    const bool in_flight = value->queued || (value->flight.active && !value->flight.retired);
    // Saved Active rows stay usable while REST refreshes them.
    const bool saved_rows = value->tab == UiThreadBrowserTab::active && value->row_count != 0;
    Utf8View status = value->status;
    if (!status.bytes && in_flight)
        status = saved_rows ? utf8_literal("Showing saved threads. Updating…") : utf8_literal("Loading threads…");
    const bool inventory_synced = parent->thread_listing_synced || value->active_loaded;
    const bool loading = in_flight && !saved_rows;
    const bool can_create = application_forum_post_available(value->scope.parent);
    if (can_create != value->view.can_create_post) ++value->revision;
    const UiForumChannelView* forum_view = nullptr;
    if (value->forum_mode && value->forum) {
        auto& forum = *value->forum;
        const u32 tags = forum.meta_loaded ? minimum<u32>(forum.meta.tag_count, kUiForumTagCapacity) : 0;
        for (u32 i = 0; i < tags; ++i) {
            bool chosen = false;
            for (u32 j = 0; j < forum.selected_count; ++j) chosen |= forum.selected[j] == forum.meta.tags[i].id;
            forum.tags[i] = {forum.meta.tags[i].id, forum_tag_name_view(forum.meta.tags[i]), chosen};
        }
        forum.view = {{forum.tags, tags}, {forum.posts, value->row_count}, forum.sort, forum.layout, forum.match, forum.media};
        forum_view = &forum.view;
    }
    value->view = {{value->rows, value->row_count}, chat_state_store_channel_name(g_app.account_ui->chat_state, *parent),
        status, value->publication, value->revision, value->page_index, value->total,
        value->tab, loading, value->error, value->page_index > 0, value->has_more, inventory_synced,
        value->scope.parent, can_create, forum_view};
    if (!ui_thread_browser_scene_update(&value->scene, &value->view, application_thread_browser_viewport())) return false;
    renderer_bind_thread_browser_scene(&g_app.renderer, &value->scene);
    return true;
}
bool application_thread_browser_refresh_after_scroll() noexcept {
    return application_thread_browser_synchronize() && refresh_retained_projection(false);
}
bool application_thread_browser_pending(u64 now) noexcept {
    application_thread_browser_synchronize();
    if (browser_owner && current(*browser_owner) && forum_meta_due(*browser_owner, now) && g_app.account.instance &&
        g_app.account.status.running && !g_app.account.stop_failed &&
        g_app.account.status.required_action == AccountRequirement::none) return true;
    return browser_owner && current(*browser_owner) && browser_owner->queued && !browser_owner->flight.active &&
        !forum_meta_active(*browser_owner) &&
        g_app.account.instance && g_app.account.status.running && !g_app.account.stop_failed && g_app.account.api.thread_browser &&
        g_app.account.status.required_action == AccountRequirement::none && now >= maximum(browser_owner->due, browser_owner->actor_floor);
}
bool application_thread_browser_submit(u64 id, u64 now) noexcept {
    if (!id || !application_thread_browser_pending(now)) return false;
    auto& value = *browser_owner;
    if (forum_meta_due(value, now)) return forum_submit_meta(value, id, now);
    ThreadBrowserRequest request{value.scope.parent, value.scope.guild,
        value.tab == UiThreadBrowserTab::search ? Utf8View{value.query, value.query_bytes} : Utf8View{},
        value.tab == UiThreadBrowserTab::archived ? value.page_index * kThreadBrowserArchivedLimit
            : value.tab == UiThreadBrowserTab::active ? value.active_offset : 0,
        value.tab == UiThreadBrowserTab::search ? ThreadBrowserMode::search
            : value.tab == UiThreadBrowserTab::active ? ThreadBrowserMode::active
                                                      : ThreadBrowserMode::archived, {}};
    if (!thread_browser_request_valid(request)) { fail(value, utf8_literal("Thread names can be up to 100 characters.")); repaint(); return false; }
    const auto result = g_app.account.api.thread_browser(g_app.account.instance, &request, id);
    if (!result.ok()) {
        if (result.error == RestClientSubmitError::busy && ++value.busy_count <= 20) value.due = deadline_after(now, 50);
        else fail_choice(value, utf8_literal("Couldn't load threads. Try again."),
            utf8_literal("Showing saved threads. Couldn't refresh them."));
        repaint(); return false;
    }
    value.flight = {value.scope, request, id, value.choice, value.gateway_serial, {}, true, false, false};
    if (request.name.bytes) CopyMemory(value.flight.name, request.name.data, request.name.bytes);
    value.flight.request.name = {value.flight.name, request.name.bytes};
    value.queued = false;
    ++value.revision;
    repaint(); return true;
}
bool application_thread_browser_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (browser_owner && response.request_id && forum_meta_active(*browser_owner) &&
        response.request_id == browser_owner->forum->meta_flight.id)
        return forum_meta_response(*browser_owner, response, body);
    if (!browser_owner || !browser_owner->flight.active || !response.request_id || response.request_id != browser_owner->flight.id) return false;
    auto& value = *browser_owner;
    auto flight = value.flight;
    flight.request.name = {flight.name, flight.request.name.bytes};
    reset_value(&value.flight);
    const u64 now = clock_monotonic_milliseconds();
    if (actor_current(flight.scope) && (response.rate_limited || response.status_code == 429))
        value.actor_floor = maximum(value.actor_floor, maximum(response.retry_at_ms, deadline_after(now, maximum(response.rate.retry_after_ms, u64{50}))));
    const bool same = !flight.retired && actor_current(flight.scope) && current(value) && flight.choice == value.choice &&
        flight.scope.parent == value.scope.parent && flight.scope.generation == value.scope.generation;
    if (!same) {
        if (value.release_pending && !forum_meta_active(value)) free_owner(); else repaint();
        return true;
    }
    if (response.status_code == 401 || response.error == RestClientError::account_restricted ||
        api_rejection_requires_action(response.rejection) || g_app.account.status.required_action != AccountRequirement::none) {
        fail_choice(value, response.status_code == 401 ? utf8_literal("Your session expired. Sign in again.") : utf8_literal("Verify your account to view threads."),
            utf8_literal("Showing saved threads. Sign in or verify your account to refresh."));
    } else if (response.rate_limited || response.status_code == 429) {
        fail_choice(value, utf8_literal("Too many requests. Try again in a moment."),
            utf8_literal("Showing saved threads. Too many requests to refresh now."));
    } else if (response.status_code == 403) {
        fail(value, utf8_literal("You no longer have permission to browse these threads."));
        invalidate(value);
    } else if (response.transport_ok() && response.status_code == 202) {
        u64 delay = maximum(response.rate.retry_after_ms,
            response.retry_at_ms > now ? response.retry_at_ms - now : 0);
        if (body.data && body.bytes == response.body_bytes && body.bytes <= kThreadBrowserMaximumBodyBytes) {
            const auto indexing = decode_search_indexing_body(body,
                {g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount}, 64);
            if (indexing.ok()) delay = maximum(delay, indexing.retry_after_ms ? indexing.retry_after_ms : 5000);
        }
        // Index readiness belongs to this exact choice, never the actor's 429
        // floor. Only recognized replies may omit their optional retry_after.
        if (!delay || delay > ~u64{0} - now || value.indexing_retries >= 5) {
            fail_choice(value, utf8_literal("Couldn't load threads. Try again."),
                utf8_literal("Showing saved threads. Couldn't refresh them."));
        } else {
            ++value.indexing_retries;
            value.due = now + delay;
            value.queued = true;
            value.error = false;
            value.status = value.tab == UiThreadBrowserTab::active && value.row_count
                ? utf8_literal("Showing saved threads. Preparing threads…") : utf8_literal("Preparing threads…");
            ++value.revision;
        }
    } else if (!response.transport_ok() || response.status_code < 200 || response.status_code >= 300 ||
        !body.data || body.bytes != response.body_bytes || body.bytes > kThreadBrowserMaximumBodyBytes) {
        fail_choice(value, utf8_literal("Couldn't load threads. Try again."),
            utf8_literal("Showing saved threads. Couldn't refresh them."));
    } else {
        auto& candidate = value.banks[value.bank ^ 1];
        const u32 capacity = flight.request.mode == ThreadBrowserMode::search ? kThreadBrowserSearchMaximumRows : kThreadBrowserArchivedLimit;
        ThreadBrowserPage page{};
        ThreadBrowserPostPage posts{};
        bool accepted = ensure_bank(candidate, capacity);
        // Forum parents also retain post metadata (tags, counts, starter preview).
        const bool forum = accepted && value.forum_mode && value.forum &&
            forum_commit(value.forum->scratch, &value.forum->scratch_committed,
                forum_scratch_post_bytes + capacity * kThreadBrowserPostTextMaximumBytes, forum_scratch_bytes);
        if (accepted) {
            auto* rows = reinterpret_cast<ThreadBrowserRow*>(candidate.storage);
            auto* names = reinterpret_cast<char*>(candidate.storage + capacity * sizeof(ThreadBrowserRow));
            const Slice<u32> structurals{g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount};
            accepted = forum
                ? thread_browser_decode_posts(body, flight.request, flight.scope.self, structurals,
                    {rows, capacity}, {names, capacity * kThreadBrowserNameMaximumBytes},
                    {reinterpret_cast<ThreadBrowserPost*>(value.forum->scratch), capacity},
                    {reinterpret_cast<char*>(value.forum->scratch + forum_scratch_post_bytes),
                     capacity * kThreadBrowserPostTextMaximumBytes}, &page, &posts).ok()
                : thread_browser_decode(body, flight.request, flight.scope.self, structurals,
                    {rows, capacity}, {names, capacity * kThreadBrowserNameMaximumBytes}, &page).ok();
        }
        if (accepted) { candidate.page = page; accepted = apply_page(value, candidate, flight); }
        if (accepted && forum) forum_remember(*value.forum, posts);
        if (accepted && current(value) && flight.request.mode == ThreadBrowserMode::active) {
            // Rows were merged into Core as positive active observations; the
            // tab keeps projecting Core. Further server pages are fetched in
            // order, bounded, before the listing counts as loaded.
            value.error = false;
            reset_value(&value.status);
            value.projected_channels = ~u32{0};
            if (flight.gateway_serial != value.gateway_serial && page.row_count && value.active_retries < 2) {
                // A Gateway thread mutation raced this page, so none of its rows
                // were merged (no resurrection). Fetch the same page again.
                ++value.active_retries;
                value.queued = true;
            } else if (page.has_more && page.next_offset < kThreadBrowserActiveFetchLimit) {
                value.active_offset = page.next_offset;
                value.queued = true;
            } else value.active_loaded = true;
            ++value.revision;
        } else if (accepted && current(value)) {
            value.bank ^= 1;
            value.has_page = true;
            value.has_more = page.has_more;
            value.error = false;
            reset_value(&value.status);
            value.publication = ++browser_publication;
            value.projected_channels = ~u32{0};
            ++value.revision;
        } else fail_choice(value, utf8_literal("Couldn't load threads. Try again."),
            utf8_literal("Showing saved threads. Couldn't refresh them."));
    }
    repaint(); return true;
}
u64 application_thread_browser_next_wake() noexcept {
    return browser_owner && current(*browser_owner) && browser_owner->queued && !browser_owner->flight.active &&
        g_app.account.status.running && g_app.account.status.required_action == AccountRequirement::none
        ? maximum<u64>(1, maximum(browser_owner->due, browser_owner->actor_floor)) : 0;
}
void application_thread_browser_gateway(u64 guild, u64 parent) noexcept {
    if (!browser_owner) return;
    auto& value = *browser_owner;
    if ((!guild || guild == value.scope.guild) && (!parent || parent == value.scope.parent)) {
        ++value.gateway_serial;
        value.projected_channels = ~u32{0};
        ++value.revision;
        application_request_frame();
    }
}
UiThreadBrowserScene* application_thread_browser_scene() noexcept { return application_thread_browser_active() ? &browser_owner->scene : nullptr; }
bool application_thread_browser_activate(UiThreadBrowserTarget target) noexcept {
    if (!front() || !ui_thread_browser_scene_target_current(browser_owner->scene, target)) return false;
    auto& value = *browser_owner;
    switch (target.action) {
    case UiThreadBrowserAction::create_post: return application_forum_post_open(value.scope.parent);
    case UiThreadBrowserAction::close: application_thread_browser_close(); return repaint();
    case UiThreadBrowserAction::active: choose(value, UiThreadBrowserTab::active, 0); break;
    case UiThreadBrowserAction::archived: choose(value, UiThreadBrowserTab::archived, 0); break;
    case UiThreadBrowserAction::previous:
    case UiThreadBrowserAction::next: {
        const bool next = target.action == UiThreadBrowserAction::next;
        if ((next && !value.has_more) || (!next && !value.page_index)) return false;
        const u32 page = next ? value.page_index + 1 : value.page_index - 1;
        if (value.tab == UiThreadBrowserTab::archived) {
            if (page > kThreadBrowserMaximumOffset / kThreadBrowserArchivedLimit) return false;
            choose(value, value.tab, page);
        } else {
            value.page_index = page;
            value.publication = ++browser_publication;
            value.projected_channels = ~u32{0};
            ++value.revision;
        }
        break;
    }
    case UiThreadBrowserAction::retry: choose(value, value.tab, value.page_index); break;
    case UiThreadBrowserAction::forum_sort:
    case UiThreadBrowserAction::forum_list:
    case UiThreadBrowserAction::forum_gallery:
    case UiThreadBrowserAction::forum_tag: {
        if (!value.forum_mode || !value.forum) return false;
        auto& forum = *value.forum;
        if (target.action == UiThreadBrowserAction::forum_sort)
            forum.sort = forum.sort == UiForumSort::latest_activity ? UiForumSort::creation_date : UiForumSort::latest_activity;
        else if (target.action == UiThreadBrowserAction::forum_list) forum.layout = UiForumLayout::list;
        else if (target.action == UiThreadBrowserAction::forum_gallery) forum.layout = UiForumLayout::gallery;
        else {
            u32 found = forum.selected_count;
            for (u32 i = 0; i < forum.selected_count; ++i) if (forum.selected[i] == target.thread_id) found = i;
            if (found < forum.selected_count) forum.selected[found] = forum.selected[--forum.selected_count];
            else if (forum.selected_count < kUiForumTagCapacity) forum.selected[forum.selected_count++] = target.thread_id;
            // A changed filter starts from the first page of matching posts.
            if (value.tab != UiThreadBrowserTab::archived) value.page_index = 0;
        }
        ++forum.revision;
        ++value.revision;
        break;
    }
    case UiThreadBrowserAction::open_thread: {
        const auto* row = application_channel_record(target.thread_id);
        if (!row || !eligible(value, *row, value.tab == UiThreadBrowserTab::active)) return false;
        if (!application_navigate_known_message(row->id, 0)) return false;
        application_thread_browser_close(); return repaint();
    }
    default: return false;
    }
    return repaint();
}
bool application_thread_browser_search_scope(ApplicationThreadBrowserSearchScope* out) noexcept {
    if (!out || !application_thread_browser_active()) return false;
    const auto& value = *browser_owner;
    const auto* parent = application_channel_record(value.scope.parent);
    *out = {value.scope.parent, value.session, chat_state_store_channel_name(g_app.account_ui->chat_state, *parent),
        value.forum_mode};
    return true;
}
bool application_thread_browser_search(Utf8View name) noexcept {
    if (!application_thread_browser_active() || (name.bytes && !name.data)) return false;
    auto& value = *browser_owner;
    while (name.bytes && (name.data[0] == ' ' || name.data[0] == '\t')) { ++name.data; --name.bytes; }
    while (name.bytes && (name.data[name.bytes - 1] == ' ' || name.data[name.bytes - 1] == '\t')) --name.bytes;
    if (!name.bytes) {
        // A cleared header query returns from the results to the Active list.
        if (value.tab == UiThreadBrowserTab::search) choose(value, UiThreadBrowserTab::active, 0);
        return repaint();
    }
    const ThreadBrowserRequest request{value.scope.parent, value.scope.guild, name, 0, ThreadBrowserMode::search, {}};
    if (!thread_browser_request_valid(request)) {
        fail(value, utf8_literal("Thread names can be up to 100 characters."));
        repaint();
        return false;
    }
    CopyMemory(value.query, name.data, name.bytes);
    value.query_bytes = name.bytes;
    choose(value, UiThreadBrowserTab::search, 0);
    return repaint();
}
bool application_thread_browser_scroll_range(UiScrollRange* out) noexcept {
    if (!out || !front()) return false;
    *out = {browser_owner->scene.body, browser_owner->scene.scroll_dips,
        ui_thread_browser_scene_scroll_maximum(browser_owner->scene), 56, 1, 0, browser_owner->publication};
    return true;
}
bool application_thread_browser_scroll_to(double offset) noexcept {
    if (!front()) return false;
    if (ui_thread_browser_scene_scroll(&browser_owner->scene, static_cast<float>(offset))) application_request_frame();
    return true;
}
bool application_thread_browser_window_message(UINT message, WPARAM, LPARAM lparam) noexcept {
    const float scale = 96.f / maximum(g_app.renderer.dpi, 1u);
    const float client_x = static_cast<short>(LOWORD(lparam)) * scale;
    const float client_y = static_cast<short>(HIWORD(lparam)) * scale;
    const bool header = ui_rect_contains(ui_scene_header_geometry(g_app.shell_scene).threads, client_x, client_y);
    const bool header_front = application_thread_browser_available() && !application_forum_form_active() && uncovered();
    if (message == WM_LBUTTONDOWN && header && header_front) {
        if (!ensure_owner()) return true;
        auto& value = *browser_owner;
        const auto* parent = application_channel_record(default_parent());
        value.pressed_scope = {g_app.plugin_account_scope_serial, g_app.account.status.profile_id, g_app.account_ui->chat_state.self_user_id,
            parent->guild_id, parent->id, g_app.account_ui->active_channel_id, g_app.account.instance, g_app.account_ui, parent->generation};
        value.pressed = {UiThreadBrowserAction::none, ~u64{0}, 0,
            g_app.shell_scene.semantics[static_cast<u32>(UiSemantic::threads_toggle)]};
        value.captured = true;
        SetCapture(g_app.window);
        return true;
    }
    auto* value = browser_owner;
    if (!value) return false;
    if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE || message == WM_KILLFOCUS) {
        clear_capture(*value);
        return false;
    }
    if (value->captured && value->pressed.thread_id == ~u64{0}) {
        if (message == WM_LBUTTONUP) {
            const auto captured = value->pressed_scope;
            const auto* parent = application_channel_record(captured.parent);
            const bool accepted = header && header_front && actor_current(captured) && parent &&
                parent->generation == captured.generation && parent->guild_id == captured.guild &&
                g_app.account_ui->active_channel_id == captured.underlying_channel && default_parent() == captured.parent &&
                ui_widget_manager_input_enabled(g_app.shell_scene.widgets, value->pressed.widget);
            clear_capture(*value);
            if (accepted) application_thread_browser_toggle();
            return true;
        }
        if (message == WM_MOUSEMOVE) return true;
    }
    if (!front()) return false;
    if (message == WM_MOUSELEAVE) {
        bool changed{};
        ui_thread_browser_scene_set_hover(&value->scene, -1.f, -1.f, &changed);
        if (changed) application_request_frame();
        return false;
    }
    POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
    if (message == WM_CONTEXTMENU && lparam == -1) return g_app.context_focus == &value->scene.widgets;
    if (message == WM_CONTEXTMENU || message == WM_MOUSEHWHEEL || message == WM_MOUSEWHEEL)
        application_screen_to_content(&point);
    const float x = point.x * scale;
    const float y = point.y * scale;
    const bool inside = ui_thread_browser_scene_contains(value->scene, x, y);
    if (message == WM_LBUTTONUP && value->captured) {
        const auto pressed = value->pressed;
        const auto hit = inside ? ui_thread_browser_scene_hit_test(value->scene, x, y) : UiThreadBrowserTarget{};
        const bool drag = value->scrollbar_dragging;
        clear_capture(*value);
        if (!drag && ui_thread_browser_target_equal(pressed, hit)) application_thread_browser_activate(hit);
        return true;
    }
    if (message == WM_MOUSEMOVE && value->captured && value->scrollbar_dragging) {
        const auto bar = ui_scrollbar_geometry(value->scene.body, value->scene.scroll_dips,
            ui_rect_height(value->scene.body) + ui_thread_browser_scene_scroll_maximum(value->scene));
        application_thread_browser_scroll_to(ui_scrollbar_scroll_from_pointer(bar, y, value->thumb_grab));
        return true;
    }
    if (!inside) return false;
    if (message == WM_MOUSEMOVE) {
        bool changed{}; ui_thread_browser_scene_set_hover(&value->scene, x, y, &changed);
        if (changed) application_request_frame(); return true;
    }
    if (message == WM_LBUTTONDOWN) {
        // The header search bar (this panel's search field) gives up focus
        // and any open suggestions to the panel.
        application_search_close_panel();
        value->pressed = ui_thread_browser_scene_hit_test(value->scene, x, y);
        const auto bar = ui_scrollbar_geometry(value->scene.body, value->scene.scroll_dips,
            ui_rect_height(value->scene.body) + ui_thread_browser_scene_scroll_maximum(value->scene));
        if (ui_scrollbar_track_hit(bar, x, y)) {
            reset_value(&value->pressed);
            value->scrollbar_dragging = ui_scrollbar_thumb_hit(bar, x, y);
            value->thumb_grab = y - bar.thumb.top;
            if (!value->scrollbar_dragging) application_thread_browser_scroll_to(value->scene.scroll_dips +
                (y < bar.thumb.top ? -1 : 1) * ui_rect_height(value->scene.body));
        }
        ui_thread_browser_scene_focus(&value->scene, value->pressed);
        value->captured = true; g_app.context_focus = &value->scene.widgets;
        SetFocus(g_app.window); SetCapture(g_app.window); application_request_frame(); return true;
    }
    return message == WM_LBUTTONDBLCLK || message == WM_CONTEXTMENU || message == WM_MOUSEHWHEEL ||
        message == WM_RBUTTONDOWN || message == WM_RBUTTONUP || message == WM_MBUTTONDOWN || message == WM_MBUTTONUP;
}
bool application_thread_browser_input(UiInputKind kind, u32 code, bool repeated) noexcept {
    auto* value = browser_owner;
    if ((kind == UiInputKind::key_down || kind == UiInputKind::system_key_down) &&
        (code == VK_RETURN || code == VK_SPACE) && application_thread_browser_available() && uncovered() &&
        g_app.context_focus == &g_app.shell_scene.widgets) {
        const auto handle = g_app.shell_scene.semantics[static_cast<u32>(UiSemantic::threads_toggle)];
        const auto focused = g_app.shell_scene.widgets.focused;
        if (handle.generation && handle.slot == focused.slot && handle.generation == focused.generation &&
            ui_widget_manager_input_enabled(g_app.shell_scene.widgets, handle)) {
            if (!repeated && application_thread_browser_toggle() && browser_owner) browser_owner->held_key = code;
            return true;
        }
    }
    if (!value) return false;
    if (ui_input_consume_key_tail(&value->held_key, {kind, code, repeated})) return true;
    if (!front() || g_app.context_focus != &value->scene.widgets) return false;
    if (kind == UiInputKind::character || kind == UiInputKind::system_character) return true;
    if (kind != UiInputKind::key_down && kind != UiInputKind::system_key_down) return false;
    // The search shortcut belongs to the header search bar, which scopes
    // itself to this panel while it is open.
    if (shortcut_matches(ShortcutAction::search, shortcut_chord_from_keyboard(code))) return false;
    if (code == VK_ESCAPE) { value->held_key = code; application_thread_browser_close(); repaint(); return true; }
    if (code == VK_TAB) {
        ui_thread_browser_scene_move_focus(&value->scene, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        application_request_frame(); return true;
    }
    if (code == VK_RETURN || code == VK_SPACE) {
        if (!repeated) application_thread_browser_activate(ui_thread_browser_scene_focused_target(value->scene));
        value->held_key = code; return true;
    }
    if (code == VK_DOWN || code == VK_UP) {
        ui_thread_browser_scene_move_focus(&value->scene, code == VK_UP); application_request_frame(); return true;
    }
    return true;
}
bool application_thread_browser_consume_key_tail(UiInputKind kind, u32 code, bool repeated) noexcept {
    return browser_owner && ui_input_consume_key_tail(&browser_owner->held_key, {kind, code, repeated});
}
ApplicationThreadBrowserMetrics application_thread_browser_metrics() noexcept {
    if (!browser_owner) return {};
    return {sizeof(ApplicationThreadBrowser), (sizeof(ApplicationThreadBrowser) + 4095u) & ~4095u,
        browser_owner->banks[0].committed + browser_owner->banks[1].committed,
        browser_owner->has_page && browser_owner->tab != UiThreadBrowserTab::active ? browser_owner->banks[browser_owner->bank].page.row_count : browser_owner->total,
        browser_owner->publication, browser_owner->flight.active};
}
} // namespace maicord::app_detail
