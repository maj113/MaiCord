#include "maicord/model/chat/forwarded_input.hpp"
#include "../shell/overlay_input.hpp"
#include "../shell/window_state.hpp"
#include "../shell/surface_motion.hpp"
#include "login/action_captcha.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/app/shell/shortcut_registry.hpp"
#include "friends.hpp"
#include "../shell/state.hpp"
#include "../shell/tabs.hpp"
#include "../settings/settings.hpp"
#include "account_presentation.hpp"
#include "../chat/channel_session.hpp"
#include "../chat/composer/text_input.hpp"
#include "../chat/composer/composer.hpp"
#include "../chat/application_private_conversation.hpp"
#include "../chat/private_channel.hpp"
#include "maicord/app/chat/private_conversation.hpp"
#include "maicord/ui/chat/private_channel_title.hpp"
#include "maicord/ui/account/activity_card.hpp"
#include "maicord/ui/account/profile_media.hpp"
#include "maicord/media/core/asset.hpp"
#include "profile.hpp"
#include "../chat/messages/interaction.hpp"
#include "../shell/input.hpp"
#include "../shell/window_events.hpp"
#include "../chat/composer/text_form.hpp"
#include "../media/image_viewer.hpp"
#include "friends_search.hpp"
#include "friends_storage.hpp"
#include "../chat/composer/text_edit_keys.hpp"
#include "../plugins/plugins.hpp"
#include "../plugins/pipeline_host.hpp"
#include "user_actions.hpp"
#include "maicord/model/user/active_now.hpp"
#include "maicord/model/user/friend_search.hpp"
#include "maicord/platform/window/clipboard.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "maicord/ui/core/geometry.hpp"
#include "maicord/protocol/user/relationships.hpp"
#include "maicord/model/notifications/message_notifications.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/core/utf.hpp"

#include "maicord/platform/window/key.hpp"
#include "maicord/app/gateway/client_dispatch.hpp"
#include "maicord/model/user/private_channel_store.hpp"
#include "maicord/ui/text/timestamp_format.hpp"

namespace maicord::app_detail {
namespace {
constexpr u32 relationship_capacity = kFriendsRelationshipCapacity;
// An Active Now card's details span the column less its 16-DIP margins and the card's 14-DIP insets.
constexpr float kActiveDetailWidth = kUiFriendsActiveNowWidth - 2 * 16 - 2 * 14;
// The Message Requests page lists at most this many request DMs per tab, newest first.
constexpr u32 kFriendsDmRequestCapacity = 256;
enum class FriendsRequestPhase : u8 { idle, queued, awaiting, retry, failed };
struct FriendsRequest {
    u64 user_id, request_id, retry_at, membership_epoch, intent_epoch;
    // A username to send a request to (with its note after a line feed), or a friend's new nickname.
    char username[kRelationshipRequestTextMaximumBytes];
    static_assert(kRelationshipRequestTextMaximumBytes >= kRelationshipNicknameMaximumBytes);
    u32 username_bytes;
    RelationshipOperation operation;
    RelationshipType expected_type;
    FriendsRequestPhase phase;
    bool allow_hidden, expected_ignored, expected_spam, expected_stranger, expected_stranger_known;
    // Sent from a user menu to someone without a relationship (add or block).
    bool unknown_target;
};
}
struct ApplicationFriends {
    RelationshipStore store, snapshot;
    FriendsRelationshipStorage storage;
    RelationshipRecord* records;
    RelationshipRecord* candidate;
    u32* indices;
    StableStorage requests_indices;
    UiFriendsSource source, requests_source;
    UiFriendsSections sections, requests_sections;
    UiFriendsSection section_rows[2];
    UiFriendsContent content;
    FriendsSearchDocuments search;
    UiFriendsScene scene, requests_scene;
    FriendsRequest request;
    Utf8View status;
    UiFriendsTarget pressed;
    UiFriendsTarget confirmation;
    RelationshipType confirmation_type;
    char confirmation_text[512];
    u64 membership_epoch, snapshot_sequence, confirmation_serial, confirmation_epoch;
    u32 snapshot_items;
    u32 store_revision, user_revision, pressed_revision, held_key;
    u32 query_revision, total_count, edit_revision, note_revision;
    u64 search_blink_at;
    u32 search_blink_ms;
    UiFriendsTab tab;
    bool visible, ready, snapshot_active, snapshot_failed, snapshot_sorted, incomplete, dragging;
    bool search_focused, search_caret_visible, search_dragging, stranger_confirmation;
    // On Add Friend the keyboard edits the request note rather than the username.
    bool note_focused;
    bool requests_visible, collapsed[2], confirmation_hidden, confirmation_ignored, confirmation_spam, confirmation_stranger, confirmation_stranger_known;
    float thumb_grab;
    // Active Now: the projection, its card strings and the inputs it saw.
    ActiveNowCard active_projection[kActiveNowMaximumCards];
    u32 active_projection_count;
    UiFriendsActiveCard active_cards[kUiFriendsActiveNowCards];
    char active_text[kUiFriendsActiveNowCards][2][256];
    UiFriendsActiveNow active_now;
    u32 active_store_revision, active_user_revision, active_voice_revision, active_activity_revision;
    bool active_collapsed, active_projected;
    // Each card's activity (a copy the laid-out card's timer reads) and its
    // compact layout; the next once-a-second timer refresh, zero for none.
    UserActivity active_sources[kUiFriendsActiveNowCards];
    UiActivityCard active_details[kUiFriendsActiveNowCards];
    ProfileMedia active_images[kUiFriendsActiveNowCards][2];
    u64 active_tick_at;
    // Add Friend: the last send's result line and its colour; the incoming
    // requests badging Pending; a friend whose DM opens into a voice call.
    char add_status[192];
    UiFriendsTone add_tone;
    u32 incoming_badge;
    u64 call_user, nickname_serial;
    // The list rows' hover fade.
    UiMotionHover row_hover;
    // The Message Requests page: the shown tab's request DMs, newest first, with their other person and the last
    // message's date; the inputs they were projected from; the Friends tab to return to.
    UiFriendsSource dm_source;
    u64 dm_channels[kFriendsDmRequestCapacity], dm_users[kFriendsDmRequestCapacity];
    char dm_dates[kFriendsDmRequestCapacity][40];
    u8 dm_date_bytes[kFriendsDmRequestCapacity];
    u32 dm_store_revision, dm_user_revision, dm_spam_count;
    u64 dm_busy;
    UiFriendsTab dm_tab, friends_tab;
};
const RelationshipStore* application_friends_relationships() noexcept {
    const auto* friends = g_app.friends;
    return friends && friends->ready && !friends->incomplete && !friends->snapshot_active
               ? &friends->store : nullptr;
}
namespace {
// Friend nicknames name one-to-one DMs: the loaded relationships go to the
// chat state, and the list and header text are built again with them.
void publish_dm_names() noexcept {
    if (!g_app.account_ui) return;
    g_app.account_ui->chat_state.relationships = application_friends_relationships();
    refresh_account_text(true);
}
}
bool application_friends_notification_gates(u64 user_id, u32* gates) noexcept {
    const auto* store = application_friends_relationships();
    if (!gates || !store)
        return false;
    const auto* row = relationship_store_find(*store, user_id);
    *gates = (row && row->type == RelationshipType::blocked ? notification_gate_blocked : 0) |
             (row && row->ignored ? notification_gate_ignored : 0);
    return true;
}
namespace {
void close_requests(ApplicationFriends&) noexcept;
UiFriendsScene& input_scene(ApplicationFriends& friends) noexcept {
    return friends.requests_visible ? friends.requests_scene : friends.scene;
}
void invalidate_sources(ApplicationFriends& friends) noexcept {
    ++friends.source.revision;++friends.requests_source.revision;
}
// Relationship presence as a UI dot; an unreported presence shows as unknown.
UiPresence presence_dot(RelationshipPresence value) noexcept {
    return value == RelationshipPresence::online    ? UiPresence::online
           : value == RelationshipPresence::idle    ? UiPresence::idle
           : value == RelationshipPresence::dnd     ? UiPresence::busy
           : value == RelationshipPresence::offline ? UiPresence::offline
                                                    : UiPresence::unknown;
}
bool request_busy(const ApplicationFriends& friends) noexcept {
    return friends.request.phase == FriendsRequestPhase::queued || friends.request.phase == FriendsRequestPhase::awaiting ||
        friends.request.phase == FriendsRequestPhase::retry;
}
bool request_current(const ApplicationFriends& friends, const FriendsRequest& request) noexcept {
    if (request.operation == RelationshipOperation::clear_incoming || request.operation == RelationshipOperation::clear_hidden)
        return request.intent_epoch == friends.membership_epoch;
    if (!request.user_id) return true;
    const auto* current = relationship_store_find(friends.store, request.user_id);
    if (request.unknown_target) return !current;
    if (!current || current->type != request.expected_type || current->ignored != request.expected_ignored || current->spam != request.expected_spam) return false;
    if (request.operation != RelationshipOperation::accept && request.operation != RelationshipOperation::accept_stranger) return true;
    return current->type == RelationshipType::incoming && (request.allow_hidden || (!current->ignored && !current->spam)) &&
        current->stranger == request.expected_stranger && current->stranger_known == request.expected_stranger_known;
}
void recovery(ApplicationFriends& friends, const char* text, u32 bytes) noexcept {
    friends.incomplete = true;friends.status = {text, bytes};
    if (!request_busy(friends)) {
        reset_value(&friends.request);friends.request.operation = RelationshipOperation::list;
        friends.request.phase = FriendsRequestPhase::failed;
    }
}
template <u32 N> void recovery(ApplicationFriends& friends, const char (&text)[N]) noexcept {
    recovery(friends, text, N - 1);
}
void sort_snapshot(ApplicationFriends& friends) noexcept {
    if (friends.snapshot_sorted) return;
    heap_sort_shared(friends.candidate, friends.snapshot.count,
        [](const auto& a, const auto& b) noexcept { return a.user_id < b.user_id; });
    for (u32 i = 1; i < friends.snapshot.count; ++i)
        if (friends.candidate[i - 1].user_id == friends.candidate[i].user_id) friends.snapshot_failed = true;
    friends.snapshot_sorted = true;
}
// The status line of a row, in the order of status_text's chain below.
constexpr char kStatusTexts[] = "Ignored\0Potential spam\0Incoming friend request\0Outgoing friend request\0Blocked\0"
                                "Online\0Idle\0Do Not Disturb\0Offline\0Status unavailable\0";
Utf8View status_text(const RelationshipRecord& record, bool hidden) noexcept {
    const u32 index = hidden ? (record.ignored ? 0u : 1u)
                      : record.type == RelationshipType::incoming ? 2u
                      : record.type == RelationshipType::outgoing ? 3u
                      : record.type == RelationshipType::blocked ? 4u
                      : record.presence == RelationshipPresence::online ? 5u
                      : record.presence == RelationshipPresence::idle ? 6u
                      : record.presence == RelationshipPresence::dnd ? 7u
                      : record.presence == RelationshipPresence::offline ? 8u : 9u;
    return name_list_at(kStatusTexts, index);
}
bool resolve_record(const ApplicationFriends& friends, u32 index, UiFriendsRow* row, bool hidden) noexcept {
    if (!g_app.account_ui || index >= friends.store.count) return false;
    const auto& record = friends.records[index];
    const auto& users = g_app.account_ui->user_store;
    const auto* user = user_store_find(users, record.user_id);
    const auto name = relationship_display_name(record, users);
    *row = {record.user_id, name.bytes ? name : utf8_literal("Unknown user"),
        user ? user_store_username(users, *user) : Utf8View{}, status_text(record, hidden),
        presence_dot(record.presence), record.type == RelationshipType::incoming ? UiFriendsKind::incoming :
        record.type == RelationshipType::outgoing ? UiFriendsKind::outgoing :
        record.type == RelationshipType::blocked ? UiFriendsKind::blocked : UiFriendsKind::friend_user,
        request_busy(friends), record.spam, record.ignored};
    row->mobile = user && record.type == RelationshipType::friend_ &&
                  ui_presence_mobile(row->presence, user->client_status);
    return true;
}
bool resolve(void* opaque, u32 index, UiFriendsRow* row) noexcept {
    const auto& friends = *static_cast<const ApplicationFriends*>(opaque);
    return index < friends.source.count && resolve_record(friends, friends.indices[index], row, false);
}
bool requests_resolve(void* opaque, u32 index, UiFriendsRow* row) noexcept {
    const auto& friends = *static_cast<const ApplicationFriends*>(opaque);
    return index < friends.requests_source.count && resolve_record(friends,
        stable_storage_data<u32>(friends.requests_indices)[index], row, true);
}
bool find(void* opaque, u64 id, u32* index) noexcept {
    const auto& friends = *static_cast<const ApplicationFriends*>(opaque);
    for (u32 i = 0; i < friends.source.count; ++i) if (friends.records[friends.indices[i]].user_id == id) {
        *index = i; return true;
    }
    return false;
}
bool requests_find(void* opaque, u64 id, u32* index) noexcept {
    const auto& friends = *static_cast<const ApplicationFriends*>(opaque);
    const auto* indices = stable_storage_data<u32>(friends.requests_indices);
    for (u32 i = 0; i < friends.requests_source.count; ++i) if (friends.records[indices[i]].user_id == id) {
        *index = i;return true;
    }
    return false;
}
// A message request row: the other person, and the last message's date under their name.
bool dm_resolve(void* opaque, u32 index, UiFriendsRow* row) noexcept {
    const auto& friends = *static_cast<const ApplicationFriends*>(opaque);
    if (!g_app.account_ui || index >= friends.dm_source.count) return false;
    const auto& users = g_app.account_ui->user_store;
    const auto* user = user_store_find(users, friends.dm_users[index]);
    const auto name = user ? user_store_name(users, *user) : Utf8View{};
    *row = {friends.dm_users[index], name.bytes ? name : utf8_literal("Unknown user"),
        user ? user_store_username(users, *user) : Utf8View{},
        {friends.dm_dates[index], friends.dm_date_bytes[index]}, UiPresence::offline, UiFriendsKind::message_request,
        friends.dm_busy == friends.dm_channels[index], friends.dm_tab == UiFriendsTab::spam};
    return true;
}
bool dm_find(void* opaque, u64 id, u32* index) noexcept {
    const auto& friends = *static_cast<const ApplicationFriends*>(opaque);
    for (u32 i = 0; i < friends.dm_source.count; ++i) if (friends.dm_users[i] == id) { *index = i;return true; }
    return false;
}
// The request DM a Message Requests row stands for.
u64 dm_channel(const ApplicationFriends& friends, u64 user_id) noexcept {
    u32 index = 0;
    return dm_find(const_cast<ApplicationFriends*>(&friends), user_id, &index) ? friends.dm_channels[index] : 0;
}
// Lists the shown tab's request DMs (pending or spam), newest first, when they, their people or the busy one changed.
void project_dm(ApplicationFriends& friends) noexcept {
    const auto& state = g_app.account_ui->chat_state;
    const auto* store = state.private_channels;
    const auto& users = g_app.account_ui->user_store;
    u64 busy = 0;
    for (u32 i = 0; i < friends.dm_source.count && !busy; ++i)
        if (application_private_channel_request_busy(friends.dm_channels[i])) busy = friends.dm_channels[i];
    const u32 revision = store ? store->revision : 0;
    if (friends.dm_tab == friends.tab && friends.dm_store_revision == revision && friends.dm_user_revision == users.revision &&
        friends.dm_busy == busy && friends.dm_source.revision)
        return;
    friends.dm_tab = friends.tab;friends.dm_store_revision = revision;friends.dm_user_revision = users.revision;
    friends.dm_busy = busy;
    friends.dm_spam_count = store ? private_channel_store_request_count(*store, true) : 0;
    const bool spam = friends.tab == UiFriendsTab::spam;
    const u64 now = clock_unix_milliseconds() / 1000;
    u64 recency[kFriendsDmRequestCapacity];
    u32 count = 0;
    for (u32 i = 0; store && i < store->count; ++i) {
        const auto& record = store->records[i];
        if (!private_channel_is_request(record) || ((record.request_flags & private_request_spam) != 0) != spam) continue;
        const auto recipients = private_channel_store_recipients(*store, record);
        if (!recipients.count) continue;
        const auto* channel = chat_state_store_find_channel(state, record.id);
        const u64 sort = channel && channel->last_message_id ? channel->last_message_id : record.id;
        // Insertion keeps the newest first; past the capacity the oldest drop off.
        u32 at = count;
        while (at && recency[at - 1] < sort) --at;
        if (at >= kFriendsDmRequestCapacity) continue;
        const u32 last = minimum(count, kFriendsDmRequestCapacity - 1);
        for (u32 j = last; j > at; --j) {
            recency[j] = recency[j - 1];friends.dm_channels[j] = friends.dm_channels[j - 1];
            friends.dm_users[j] = friends.dm_users[j - 1];
        }
        recency[at] = sort;friends.dm_channels[at] = record.id;friends.dm_users[at] = recipients[0];
        if (count < kFriendsDmRequestCapacity) ++count;
    }
    for (u32 i = 0; i < count; ++i) {
        // Snowflakes count milliseconds from 2015-01-01.
        const u64 sent = ((recency[i] >> 22) + 1420070400000ull) / 1000;
        const auto formatted = format_timestamp_calendar(sent, now, {friends.dm_dates[i], sizeof(friends.dm_dates[i])});
        friends.dm_date_bytes[i] = static_cast<u8>(formatted.ok ? formatted.bytes : 0);
    }
    friends.dm_source.count = count;
    if (++friends.dm_source.revision == 0) ++friends.dm_source.revision;
}
ApplicationFriends* ensure() noexcept {
    if (g_app.friends) return g_app.friends;
    auto* friends = commit_pages<ApplicationFriends>();
    if (!friends) return nullptr;
    if (!friends_search_documents_initialize(&friends->search) ||
        !friends_relationship_storage_initialize(&friends->storage)) {
        friends_search_documents_release(&friends->search);
        friends_relationship_storage_release(&friends->storage);
        pages_release(friends); return nullptr;
    }
    friends->records = stable_storage_data<RelationshipRecord>(friends->storage.active);
    friends->candidate = stable_storage_data<RelationshipRecord>(friends->storage.candidate);
    friends->indices = stable_storage_data<u32>(friends->storage.indices);
    // The usual model initializer clears its entire slice. These caller-owned
    // stores instead advertise a reserved bound, with every writer admitted
    // before use; empty lookup/replacement never dereferences uncommitted rows.
    friends->store = {{friends->records, relationship_capacity}, 0, 0};
    friends->snapshot = {{friends->candidate, relationship_capacity}, 0, 0};
    friends->source = {friends, 0, 1, resolve, find};
    friends->requests_source = {friends, 0, 1, requests_resolve, requests_find};
    friends->dm_source = {friends, 0, 0, dm_resolve, dm_find};
    friends->sections = {friends->section_rows, 2, 1, false, false, 0};
    friends->requests_sections = {nullptr, 0, 1, false, true, 0};
    friends->search_blink_ms = application_caret_blink_ms();
    if (!friends->search_blink_ms) friends->search_blink_ms = 500;
    g_app.friends = friends;
    return friends;
}
bool before(const ApplicationFriends& friends, u32 left, u32 right) noexcept {
    const auto& users = g_app.account_ui->user_store;
    if (friends.records[left].type != friends.records[right].type)
        return friends.records[left].type < friends.records[right].type;
    const auto a = relationship_display_name(friends.records[left], users);
    const auto b = relationship_display_name(friends.records[right], users);
    const auto compared = friend_name_compare(a, b);
    return compared.ok() && compared.order ? compared.order < 0 : friends.records[left].user_id < friends.records[right].user_id;
}
void project(ApplicationFriends& friends, bool force = false) noexcept {
    const auto* query_model = friends_search_document(&friends.search, friends.tab);
    const auto query = query_model ? message_composer_text(*query_model) : Utf8View{};
    const u32 query_revision = query_model ? query_model->text_revision : 0;
    if (!g_app.account_ui || (!force && friends.store_revision == friends.store.revision &&
        friends.user_revision == g_app.account_ui->user_store.revision && friends.query_revision == query_revision)) return;
    if (!friends_relationship_storage_indices(&friends.storage, friends.store.count)) {
        recovery(friends, "Couldn't update your friends list. Try again.");
        friends.source.count = 0;friends.total_count = 0;
        friends.section_rows[0] = {UiFriendsSectionKind::incoming, 0, 0, 0, friends.collapsed[0]};
        friends.section_rows[1] = {UiFriendsSectionKind::outgoing, 0, 0, 0, friends.collapsed[1]};
        ++friends.sections.revision;close_requests(friends);invalidate_sources(friends);return;
    }
    friends.source.count = 0;friends.total_count = 0;
    u32 incoming_total = 0, outgoing_total = 0, hidden_count = 0;
    for (u32 i = 0; i < friends.store.count; ++i) {
        const auto& record = friends.records[i];
        if (record.type == RelationshipType::incoming) {
            if (record.ignored || record.spam) ++hidden_count;else ++incoming_total;
        } else if (record.type == RelationshipType::outgoing) ++outgoing_total;
    }
    const auto filter = friends.tab == UiFriendsTab::online ? RelationshipFilter::online :
        friends.tab == UiFriendsTab::pending ? RelationshipFilter::pending : RelationshipFilter::all;
    if (friends.tab <= UiFriendsTab::pending)
        for (u32 i = 0; i < friends.store.count; ++i)
            if (relationship_matches(friends.records[i], filter)) {
                ++friends.total_count;
                const auto* user = user_store_find(g_app.account_ui->user_store, friends.records[i].user_id);
                if (!query.bytes || (user && friend_search(query,
                    user_store_username(g_app.account_ui->user_store, *user),
                    {friends.records[i].nickname, friends.records[i].nickname_bytes},
                    user_store_name(g_app.account_ui->user_store, *user)).matched))
                    friends.indices[friends.source.count++] = i;
            }
    heap_sort_shared(friends.indices, friends.source.count,
              [&friends](u32 a, u32 b) noexcept { return before(friends, a, b); });
    friends.store_revision = friends.store.revision;friends.user_revision = g_app.account_ui->user_store.revision;
    friends.query_revision = query_revision;
    u32 incoming_count = 0;
    if (friends.tab == UiFriendsTab::pending)
        while (incoming_count < friends.source.count && friends.records[friends.indices[incoming_count]].type == RelationshipType::incoming) ++incoming_count;
    friends.section_rows[0] = {UiFriendsSectionKind::incoming, 0, incoming_count, incoming_total, friends.collapsed[0], true};
    friends.section_rows[1] = {UiFriendsSectionKind::outgoing, incoming_count, friends.source.count - incoming_count,
        outgoing_total, friends.collapsed[1]};
    friends.sections.hidden_available = hidden_count != 0;friends.sections.hidden_count = hidden_count;++friends.sections.revision;
    friends.incoming_badge = incoming_total;
    friends.requests_sections.hidden_count = hidden_count;++friends.requests_sections.revision;
    if (friends.requests_visible) {
        if ((!friends.requests_indices.arena.base && !stable_storage_initialize(&friends.requests_indices,
            relationship_capacity, 0, sizeof(u32), alignof(u32))) ||
            (hidden_count && !stable_storage_reserve(&friends.requests_indices, hidden_count))) {
            close_requests(friends);invalidate_sources(friends);
            recovery(friends, "Couldn't load other requests. Try again.");return;
        }
        auto* indices = stable_storage_data<u32>(friends.requests_indices);u32 count = 0;
        // Unique IDs remain a scene invariant, including records with both flags.
        for (u32 pass = 0; pass != 2; ++pass) {
            const u32 first = count;
            for (u32 i = 0; i < friends.store.count; ++i) {
                const auto& row = friends.records[i];
                if (row.type == RelationshipType::incoming &&
                    (pass == 0 ? row.ignored : row.spam && !row.ignored)) indices[count++] = i;
            }
            heap_sort_shared(indices + first, count - first,
                [&friends](u32 a, u32 b) noexcept { return before(friends, a, b); });
        }
        friends.requests_source.count = count;
    }
    invalidate_sources(friends);
}
bool repaint() noexcept {
    return application_friends_synchronize() && application_request_frame();
}
// Another surface owns the window's input.
bool overlay_open() noexcept {
    return application_settings_active() || g_app.text_form || g_app.popup_scene.active ||
        (g_app.profile && g_app.profile->scene.active) || g_app.reaction_viewer.active || g_app.image_viewer.scene.active;
}
bool search_available() noexcept {
    return application_friends_active() && !application_friends_requests_active() && !overlay_open();
}
void restart_search_blink(ApplicationFriends& friends) noexcept {
    friends.search_caret_visible = friends.search_focused;
    friends.search_blink_at = friends.search_focused && friends.search_blink_ms != INFINITE
                                  ? deadline_after(clock_monotonic_milliseconds(), friends.search_blink_ms)
                                  : 0;
}
// The document the keyboard edits: Add Friend's note while it holds the focus, else the tab's query or username.
MessageComposer* edit_model(ApplicationFriends& friends) noexcept {
    return friends.tab == UiFriendsTab::add_friend && friends.note_focused
        ? &friends.search.note : friends_search_document(&friends.search, friends.tab);
}
void search_focus(ApplicationFriends& friends, bool focused, bool note = false) noexcept {
    note = focused && note && friends.tab == UiFriendsTab::add_friend;
    const bool moved = friends.note_focused != note;
    friends.note_focused = note;
    auto* model = edit_model(friends);
    focused = focused && model && search_available();
    if (friends.search_focused != focused || moved) application_composer_text_input_suspend();
    for (auto& document : friends.search.models) document.focused = focused && &document == model;
    friends.search.note.focused = focused && note;
    friends.search_focused = focused;
    restart_search_blink(friends);
    if (focused) {
        message_composer_enable_history(model, 2048);
        ui_friends_scene_focus(&friends.scene, {note ? UiFriendsAction::note : UiFriendsAction::search, 0, friends.tab});
        g_app.context_focus = &friends.scene.widgets;
    }
}
void close_requests(ApplicationFriends& friends) noexcept {
    if (!friends.requests_visible) return;
    application_composer_text_input_suspend();
    if (g_app.context_focus == &friends.requests_scene.widgets) g_app.context_focus = &friends.scene.widgets;
    if (friends.pressed.action != UiFriendsAction::none || friends.dragging) application_release_capture();
    reset_value(&friends.pressed);friends.dragging = false;friends.requests_visible = false;
    renderer_bind_friends_requests_scene(&g_app.renderer, nullptr);ui_friends_scene_release(&friends.requests_scene);
    friends.requests_source.count = 0;++friends.requests_source.revision;
}
UiEditView edit_view(const MessageComposer* model, bool focused, bool caret_visible) noexcept {
    UiEditView edit{};
    if (model) {
        edit.text = message_composer_text(*model);edit.active = true;
        edit.anchor = model->anchor;edit.caret = model->caret;
        edit.focused = focused;edit.caret_visible = focused && caret_visible;
    }
    return edit;
}
bool edit_view_equal(const UiEditView& a, const UiEditView& b) noexcept {
    return a.text.data == b.text.data && a.text.bytes == b.text.bytes && a.anchor == b.anchor && a.caret == b.caret &&
        a.active == b.active && a.focused == b.focused && a.caret_visible == b.caret_visible;
}
void project_content(ApplicationFriends& friends) noexcept {
    auto* model = friends_search_document(&friends.search, friends.tab);
    const bool focused = application_friends_search_focused();
    const auto edit = edit_view(model, focused && !friends.note_focused, friends.search_caret_visible);
    // Add Friend's request note sits under its username.
    const bool adding = friends.tab == UiFriendsTab::add_friend;
    const auto note = edit_view(adding ? &friends.search.note : nullptr, focused && friends.note_focused,
                                friends.search_caret_visible);
    const u32 revision = model ? model->revision : 0, note_revision = adding ? friends.search.note.revision : 0;
    // The tone colours only the Add Friend result the status shows now.
    const auto tone = friends.status.data == friends.add_status ? friends.add_tone : UiFriendsTone::neutral;
    const auto* users = g_app.account_ui ? &g_app.account_ui->user_store : nullptr;
    if (revision != friends.edit_revision || note_revision != friends.note_revision ||
        !edit_view_equal(friends.content.search, edit) || !edit_view_equal(friends.content.note, note) ||
        friends.content.total_count != friends.total_count ||
        friends.content.incoming_count != friends.incoming_badge || friends.content.tone != tone ||
        friends.content.users != users || friends.content.spam_count != friends.dm_spam_count) {
        friends.content.search = edit;friends.content.note = note;friends.content.total_count = friends.total_count;
        friends.edit_revision = revision;friends.note_revision = note_revision;
        friends.content.incoming_count = friends.incoming_badge;friends.content.tone = tone;friends.content.users = users;
        friends.content.spam_count = friends.dm_spam_count;
        ++friends.content.revision;
    }
}
bool search_pointer(ApplicationFriends& friends, float x, float y, bool extend, bool word = false) noexcept {
    auto* model = edit_model(friends);
    u32 position = 0, offset = 0;
    if (!model || !text_edit_layout_hit_test(g_app.renderer.composer_layout, x, y, &position) ||
        !utf8_byte_offset_for_utf16_position(message_composer_text(*model), position, &offset)) return false;
    search_focus(friends, true, friends.note_focused);
    const auto result = word ? message_composer_select_word(model, offset) :
        message_composer_set_selection(model, extend ? model->anchor : offset, offset);
    return result != MessageComposerResult::invalid_input && application_friends_search_changed(false);
}
// Card text, cut on a UTF-8 boundary when the buffer fills.
struct ActiveText : TextBank {
    void add(const Utf8View& text) noexcept {
        used += utf8_copy_truncated(data + used, capacity - used, text);
    }
    // "text" after a middle dot.
    void add_after_dot(const Utf8View& text) noexcept {
        add(utf8_literal(" \xC2\xB7 "));
        add(text);
    }
};
Utf8View friend_name(u64 user_id) noexcept {
    const auto& users = g_app.account_ui->user_store;
    const auto* user = user_store_find(users, user_id);
    return user ? user_store_name(users, *user) : utf8_literal("A friend");
}
// "Ada", "Ada and Bo", "Ada and 3 others".
void member_names(ActiveText& text, const ActiveNowCard& card) noexcept {
    text.add(friend_name(card.members[0]));
    if (card.total == 2 && card.member_count > 1) {
        text.add(utf8_literal(" and "));
        text.add(friend_name(card.members[1]));
    } else if (card.total > 2) {
        text.add(utf8_literal(" and "));
        text.number(card.total - 1);
        text.add(utf8_literal(" others"));
    }
}
// An activity image the card shows (its art or the art's corner badge), keyed
// by URL as the profile card keys the same image; false without one.
bool active_image(const UserActivity& activity, const UiActivityCardItem& item, bool large, ProfileMedia* out) noexcept {
    reset_value(out);
    MediaAttachmentFormat format{};
    const u32 bytes = user_activity_image_url(activity, large, {out->url.data, sizeof(out->url.data)}, &format);
    if (!bytes) return false;
    out->url.bytes = static_cast<u16>(bytes);
    out->key = media_asset_embed(hash64_bytes(kHash64Seed, out->url.data, bytes), 1, 0, MediaEmbedResource::image,
                                 format, false, MediaResolutionTier::px512);
    out->key.kind = MediaAssetKind::profile_image;
    out->rectangle = out->mask = item.rect;
    out->circular = item.role == UiActivityCardRole::badge;
    out->mask_radius = out->circular ? ui_rect_width(item.rect) * .5f : 4.f;
    return media_asset_key_valid(out->key);
}
// A laid-out card's images, collected again whenever it is laid out.
void active_card_images(ApplicationFriends& friends, u32 slot, UiFriendsActiveCard* out) noexcept {
    const auto& detail = friends.active_details[slot];
    out->images = friends.active_images[slot];
    out->image_count = 0;
    for (u32 j = 0; j < detail.count && out->image_count < 2; ++j) {
        const auto& part = detail.items[j];
        const bool art = part.role == UiActivityCardRole::art && detail.art.large != UserActivityArt::application;
        if ((art || part.role == UiActivityCardRole::badge) &&
            active_image(friends.active_sources[slot], part, art && detail.art.large == UserActivityArt::large,
                         &friends.active_images[slot][out->image_count]))
            ++out->image_count;
    }
}
// The activity behind an Active Now card (its first member's), laid out as the
// shared compact activity card in the card's slot with its images; nothing
// when it has no details to show. A ticking timer schedules the column's
// once-a-second refresh.
void active_detail(ApplicationFriends& friends, u32 slot, const ActiveNowCard& card, UiFriendsActiveCard* out) noexcept {
    UserActivitySet set{};
    if (!card.member_count || !user_activity_find(g_app.account_ui->user_store.activities, card.members[0], &set))
        return;
    const Utf8View name{card.name, card.name_bytes};
    for (u32 i = 0; i < set.count; ++i) {
        const auto& item = set.items[i];
        if (item.type != card.activity_type ||
            (card.application_id ? item.application_id != card.application_id
                                 : !utf8_equal(user_activity_text(item, UserActivityField::name), name)))
            continue;
        auto& source = friends.active_sources[slot];
        auto& detail = friends.active_details[slot];
        source = item;
        const UiActivityCardInput input{UiActivityCardSize::compact, kActiveDetailWidth, clock_unix_milliseconds(),
                                        false, false, false, false, nullptr, nullptr};
        if (!ui_activity_card_layout(source, input, &detail) || !detail.count)
            return;
        out->activity = &detail;
        active_card_images(friends, slot, out);
        if (ui_activity_card_timed(detail) && !friends.active_tick_at)
            friends.active_tick_at = deadline_after(clock_monotonic_milliseconds(), 1000);
        return;
    }
}
// Rebuilds the projection when friends, presence, voice or activities moved;
// called from synchronize right before the scene update.
void project_active(ApplicationFriends& friends) noexcept {
    auto& ui = *g_app.account_ui;
    const u32 voice_revision = ui.voice_states.revision, activity_revision = ui.user_store.activities.revision;
    const bool users_changed = friends.active_user_revision != ui.user_store.revision;
    if (friends.active_projected && friends.active_store_revision == friends.store.revision &&
        friends.active_user_revision == ui.user_store.revision && friends.active_voice_revision == voice_revision &&
        friends.active_activity_revision == activity_revision && friends.active_now.collapsed == friends.active_collapsed)
        return;
    friends.active_store_revision = friends.store.revision;
    friends.active_user_revision = ui.user_store.revision;
    friends.active_voice_revision = voice_revision;
    friends.active_activity_revision = activity_revision;
    friends.active_projected = true;
    // Scratch on the UI thread, not re-entered: on the stack it cost a guard-page walk on every
    // call, including the unchanged ones above; only as many entries as there are friends are touched.
    static u64 online[1024];
    u32 count = 0;
    for (u32 i = 0; i < friends.store.count && count < sizeof(online) / sizeof(online[0]); ++i) {
        const auto& record = friends.records[i];
        if (record.type == RelationshipType::friend_ && (record.presence == RelationshipPresence::online ||
                                                          record.presence == RelationshipPresence::idle ||
                                                          record.presence == RelationshipPresence::dnd))
            online[count++] = record.user_id;
    }
    static ActiveNowCard cards[kActiveNowMaximumCards];
    const u32 projected = active_now_project({{online, count}, &ui.voice_states, &ui.user_store.activities},
                                             {cards, kActiveNowMaximumCards});
    const bool same = active_now_equal({cards, projected}, {friends.active_projection, friends.active_projection_count});
    if (same && !users_changed && friends.active_now.collapsed == friends.active_collapsed && friends.active_now.revision)
        return;
    for (u32 i = 0; i < projected; ++i)
        friends.active_projection[i] = cards[i];
    friends.active_projection_count = projected;
    friends.active_tick_at = 0;
    const u32 shown = minimum(projected, kUiFriendsActiveNowCards);
    for (u32 i = 0; i < shown; ++i) {
        const auto& card = cards[i];
        auto& out = friends.active_cards[i];
        reset_value(&out);
        ActiveText title{{friends.active_text[i][0], sizeof(friends.active_text[i][0]), 0}};
        ActiveText subtitle{{friends.active_text[i][1], sizeof(friends.active_text[i][1]), 0}};
        if (card.kind == ActiveNowKind::voice) {
            const auto* channel = chat_state_store_find_channel(ui.chat_state, card.channel_id);
            const auto* guild = card.guild_id ? chat_state_store_find_guild(ui.chat_state, card.guild_id) : nullptr;
            if (channel && card.guild_id)
                title.add(chat_state_store_channel_name(ui.chat_state, *channel));
            else
                title.add(card.guild_id ? utf8_literal("Voice channel") : utf8_literal("In a call"));
            member_names(subtitle, card);
            if (guild)
                subtitle.add_after_dot(chat_state_store_guild_name(ui.chat_state, *guild));
            else if (card.name_bytes)
                subtitle.add_after_dot({card.name, card.name_bytes});
            out.kind = UiFriendsActiveKind::voice;
        } else {
            char heading[160];
            const u32 bytes = user_activity_summary(card.activity_type, {card.name, card.name_bytes}, {},
                                                    {heading, sizeof(heading)});
            const Utf8View summary = bytes ? Utf8View{heading, bytes} : Utf8View{card.name, card.name_bytes};
            // One friend: their name over "Playing X", as the official column heads a card; a
            // shared activity names it over who is in it.
            if (card.total == 1) {
                title.add(friend_name(card.members[0]));
                subtitle.add(summary);
            } else {
                title.add(summary);
                member_names(subtitle, card);
            }
            active_detail(friends, i, card, &out);
            if (card.detail_bytes && !out.activity)
                subtitle.add_after_dot({card.detail, card.detail_bytes});
            out.kind = UiFriendsActiveKind::activity;
        }
        out.title = title.view();
        out.subtitle = subtitle.view();
        out.member_count = minimum(card.member_count, kUiFriendsActiveNowAvatars);
        for (u32 m = 0; m < out.member_count; ++m)
            out.members[m] = card.members[m];
        out.total = card.total;
    }
    friends.active_now.cards = friends.active_cards;
    friends.active_now.count = shown;
    friends.active_now.collapsed = friends.active_collapsed;
    if (++friends.active_now.revision == 0)
        ++friends.active_now.revision;
}
constexpr u32 friends_popup_action_base = 0xF100;
bool more(ApplicationFriends& friends, const UiFriendsTarget& target, UiRect anchor = {}) noexcept {
    auto& scene = input_scene(friends);
    UiFriendsTarget checked{};
    if (!ui_friends_scene_activation(scene, {UiFriendsAction::more, target.user_id, friends.tab}, &checked)) return false;
    const auto* relationship = relationship_store_find(friends.store, target.user_id);
    if (!relationship) return false;
    PopupItem items[6 + kApplicationPluginMenuRows]{};u32 count = 0;
    popup_item_set(&items[count++], utf8_literal("Profile"), friends_popup_action_base + static_cast<u32>(UiFriendsAction::profile), popup_item_enabled);
    if (relationship->type == RelationshipType::friend_) {
        popup_item_set(&items[count++], utf8_literal("Start Voice Call"),
            friends_popup_action_base + static_cast<u32>(UiFriendsAction::voice_call), popup_item_enabled);
        popup_item_set(&items[count++], relationship->nickname_bytes ? utf8_literal("Change Friend Nickname") :
            utf8_literal("Add Friend Nickname"), friends_popup_action_base + static_cast<u32>(UiFriendsAction::nickname),
            popup_item_enabled);
        popup_item_set(&items[count++], utf8_literal("Remove Friend"), friends_popup_action_base + static_cast<u32>(UiFriendsAction::remove),
            static_cast<u16>(popup_item_enabled | popup_item_danger | popup_item_separator_before));
    }
    if (relationship->type != RelationshipType::blocked) {
        popup_item_set(&items[count++], utf8_literal("Block"), friends_popup_action_base + static_cast<u32>(UiFriendsAction::block),
            static_cast<u16>(popup_item_enabled | popup_item_danger));
        popup_item_set(&items[count++], relationship->ignored ? utf8_literal("Unignore") : utf8_literal("Ignore"),
            friends_popup_action_base + static_cast<u32>(relationship->ignored ? UiFriendsAction::unignore : UiFriendsAction::ignore), popup_item_enabled);
    }
    const u32 builtins = count;
    count += application_plugins_menu_items(ApplicationPluginMenuSurface::friends, target.user_id,
        {items + count, kApplicationPluginMenuRows});
    if (anchor.right <= anchor.left || anchor.bottom <= anchor.top) {
        const auto handle = scene.widgets.focused;
        const auto* node = ui_widget_manager_layout_node(scene.widgets, handle);
        anchor = node ? node->rectangle : scene.surface;
    }
    search_focus(friends, false);
    // Without the plugins' rows when the whole menu cannot open.
    for (u32 shown = count;; shown = builtins) {
        if (popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, target.user_id, {items, shown}, anchor,
                             application_viewport_dips(), 200.f).ok()) break;
        if (shown == builtins) return false;
        application_plugins_menu_dismissed();
    }
    set_popup_owner(PopupOwnerKind::friends);g_app.context_focus = &scene.widgets;
    if (!application_plugins_menu_bind()) return false;
    return renderer_rebuild_content_text(&g_app.renderer, application_theme()) && application_request_frame();
}
// Nonzero key of a relationship request a CAPTCHA belongs to.
u64 captcha_subject(RelationshipOperation operation, u64 id, const Utf8View& username) noexcept {
    return hash64_bytes(1469598103934665603ull ^ (static_cast<u64>(operation) << 56) ^ id, username.data,
                        username.bytes) | 1;
}
u64 captcha_subject(const FriendsRequest& request) noexcept {
    return captcha_subject(request.operation, request.user_id, {request.username, request.username_bytes});
}
// Set once a solved CAPTCHA requeued the request; its submit arms the retry.
bool captcha_retry;
bool queue(RelationshipOperation operation, u64 id = 0, Utf8View username = {}, bool allow_hidden = false,
           bool unknown_target = false) noexcept {
    const u64 subject = captcha_subject(operation, id, username);
    const auto captcha =
        application_action_captcha_phase(ApplicationActionCaptchaKind::friend_request, subject);
    if (g_app.friends && (captcha == ApplicationActionCaptchaPhase::captured ||
                          captcha == ApplicationActionCaptchaPhase::failed)) {
        // Sending the same request again opens the browser challenge.
        application_action_captcha_verify(ApplicationActionCaptchaKind::friend_request, subject);
        g_app.friends->status = application_action_captcha_status(
            application_action_captcha_phase(ApplicationActionCaptchaKind::friend_request, subject));
        project(*g_app.friends, true);
        repaint();
        return true;
    }
    if (captcha == ApplicationActionCaptchaPhase::waiting)
        return true;
    auto* friends = ensure();
    if (!friends || !g_app.account_ui || !g_app.account.instance || request_busy(*friends) ||
        username.bytes > sizeof(FriendsRequest::username) || (!username.data && username.bytes)) return false;
    u8 target[96], body[kRelationshipBodyMaximumBytes];
    if (!write_relationship_target(operation, id, {target, sizeof(target)}).ok() ||
        !write_relationship_body(operation, username, {body, sizeof(body)}).ok()) return false;
    FriendsRequest pending{};
    const auto* relationship = id ? relationship_store_find(friends->store, id) : nullptr;
    if (id && !relationship && (!unknown_target || (operation != RelationshipOperation::accept &&
                                                    operation != RelationshipOperation::block)))
        return false;
    if (relationship) unknown_target = false;
    pending.unknown_target = unknown_target;
    pending.expected_type = relationship ? relationship->type : RelationshipType::none;
    pending.expected_ignored = relationship && relationship->ignored;pending.expected_spam = relationship && relationship->spam;
    pending.expected_stranger = relationship && relationship->stranger;pending.expected_stranger_known = relationship && relationship->stranger_known;
    pending.allow_hidden = allow_hidden;pending.intent_epoch = friends->membership_epoch;
    pending.operation = operation;pending.user_id = id;pending.phase = FriendsRequestPhase::queued;
    pending.username_bytes = username.bytes;
    if (username.bytes) copy_bytes(pending.username, username.data, username.bytes);
    friends->request = pending;
    if (operation == RelationshipOperation::remove) application_plugins_own_removal(PluginOwnRemoval::relationship, id);
    friends->stranger_confirmation = false;
    friends->status = operation == RelationshipOperation::list ? utf8_literal("Loading friends…") : utf8_literal("Sending request…");
    project(*friends, true);repaint();
    if (g_app.window) window_wake(g_app.window);
    return true;
}
// Add Friend's result line under the username field: "Success! ..." or why it failed.
void add_result(ApplicationFriends& friends, UiFriendsTone tone, Utf8View first, Utf8View name = {},
                Utf8View last = {}) noexcept {
    u32 used = utf8_copy_truncated(friends.add_status, first);
    used += utf8_copy_truncated(friends.add_status + used, sizeof(friends.add_status) - used, name);
    used += utf8_copy_truncated(friends.add_status + used, sizeof(friends.add_status) - used, last);
    friends.status = {friends.add_status, used};
    friends.add_tone = tone;
}
// Add Friend's failures by Discord error code, then the text of each (the codes' text index follows kAddFailureText);
// a code not listed gets the last text.
constexpr u32 kAddFailureCodes[]{80000, 80007, 30002, 30078, 30059, 40068, 340007};
constexpr u8 kAddFailureText[]{0, 1, 2, 3, 4, 5, 5, 6};
constexpr char kAddFailureTexts[] =
    " is not accepting friend requests. They\xE2\x80\x99ll have to add you to become friends.\0"
    "You're already friends with that user!\0"
    "You've maxed out your friend list. Welcome to the elite 1,000 friends club!\0"
    "You've maxed out your pending outgoing friend requests.\0"
    "You've maxed out your block list.\0"
    "Your account has limited access\0"
    "Hm, didn't work. Double check that the username is correct.";
Utf8View trimmed(Utf8View text) noexcept {
    while (text.bytes && text.data[0] == ' ') { ++text.data;--text.bytes; }
    while (text.bytes && text.data[text.bytes - 1] == ' ') --text.bytes;
    return text;
}
// Sends the Add Friend field's username with the note, if any; a malformed one fails here as the server would.
bool submit_add(ApplicationFriends& friends) noexcept {
    auto* model = friends_search_document(&friends.search, UiFriendsTab::add_friend);
    const auto name = model ? trimmed(message_composer_text(*model)) : Utf8View{};
    if (!name.bytes || request_busy(friends)) return false;
    // The request text: the username, then a line feed and the note.
    const auto note = trimmed(message_composer_text(friends.search.note));
    char text[kRelationshipRequestTextMaximumBytes];
    u32 used = 0;
    u8 body[kRelationshipBodyMaximumBytes];
    if (name.bytes <= kRelationshipUsernameMaximumBytes && note.bytes <= kRelationshipNoteMaximumBytes) {
        copy_bytes(text, name.data, name.bytes);
        used = name.bytes;
        if (note.bytes) { text[used++] = '\n';copy_bytes(text + used, note.data, note.bytes);used += note.bytes; }
    }
    if (!used || !write_relationship_body(RelationshipOperation::request, {text, used}, {body, sizeof(body)}).ok()) {
        add_result(friends, UiFriendsTone::danger, utf8_literal("Hm, didn't work. Double check that the username is correct."));
        return repaint();
    }
    return queue(RelationshipOperation::request, 0, {text, used});
}
// A friend's personal nickname (PATCH /users/@me/relationships/{id}): empty clears it.
bool nickname_valid(TextFormOwnerKey key) noexcept {
    const auto* friends = g_app.friends;
    if (!friends || !g_app.account_ui || key.scope != g_app.account.status.profile_id ||
        key.collection != friends->nickname_serial) return false;
    const auto* current = relationship_store_find(friends->store, key.item);
    return current && current->type == RelationshipType::friend_;
}
bool nickname_save(TextFormOwnerKey key, Utf8View text) noexcept {
    return nickname_valid(key) && queue(RelationshipOperation::nickname, key.item, trimmed(text));
}
bool open_nickname(ApplicationFriends& friends, u64 user_id) noexcept {
    const auto* current = relationship_store_find(friends.store, user_id);
    if (!current || current->type != RelationshipType::friend_ || request_busy(friends) || g_app.text_form) return false;
    search_focus(friends, false);
    ApplicationTextFormRequest request{current->nickname_bytes ? utf8_literal("Change Friend Nickname") :
        utf8_literal("Add Friend Nickname"),
        utf8_literal("Find a friend faster with a personal nickname. It will only be visible to you in your direct messages."),
        utf8_literal("Friend Nickname"), utf8_literal("Save"), {current->nickname, current->nickname_bytes},
        {g_app.account.status.profile_id, ++friends.nickname_serial, user_id}, nickname_valid, nickname_save,
        kRelationshipNicknameMaximumCodepoints};
    request.maximum_bytes = kRelationshipNicknameMaximumBytes;
    request.single_line = true;
    // A form that does not open leaves the serial of the one already open.
    if (application_text_form_open(request)) return true;
    --friends.nickname_serial;
    return false;
}
bool confirmation_valid(TextFormOwnerKey key) noexcept {
    const auto* friends = g_app.friends;
    if (!friends || !g_app.account_ui || key.scope != g_app.account.status.profile_id ||
        key.item != friends->confirmation.user_id || key.collection != friends->confirmation_serial) return false;
    if (friends->confirmation.action == UiFriendsAction::clear_incoming)
        return !key.item && friends->membership_epoch == friends->confirmation_epoch;
    const auto* current = relationship_store_find(friends->store, key.item);
    if (friends->confirmation_type == RelationshipType::none)
        return !current && friends->confirmation.action == UiFriendsAction::block;
    return current && current->type == friends->confirmation_type &&
        current->ignored == friends->confirmation_ignored && current->spam == friends->confirmation_spam &&
        (friends->confirmation.action != UiFriendsAction::accept ||
            (current->type == RelationshipType::incoming &&
                current->stranger == friends->confirmation_stranger && current->stranger_known == friends->confirmation_stranger_known &&
                (friends->confirmation_hidden || (!current->ignored && !current->spam))));
}
bool confirmation_save(TextFormOwnerKey key, Utf8View) noexcept {
    if (!confirmation_valid(key) || request_busy(*g_app.friends)) return false;
    const auto action = g_app.friends->confirmation.action;
    const bool queued = queue(action == UiFriendsAction::accept ? RelationshipOperation::accept_stranger :
        action == UiFriendsAction::block ? RelationshipOperation::block :
        action == UiFriendsAction::ignore ? RelationshipOperation::ignore :
        action == UiFriendsAction::clear_incoming ? RelationshipOperation::clear_incoming : RelationshipOperation::remove,
        key.item, {}, g_app.friends->confirmation_hidden, g_app.friends->confirmation_type == RelationshipType::none);
    if (queued) g_app.friends->stranger_confirmation = false;
    return queued;
}
// Each confirmation (accept, block, ignore, remove, clear all): its title, its button, then the question around the
// name or the count.
constexpr const char* kConfirmTexts[]{
    "Accept friend request\0" "Accept\0" "Accept a friend request from \0" "?",
    "Block user\0" "Block\0" "Block \0" "?",
    "Ignore user\0" "Ignore\0" "Ignore \0" "? You'll stay friends.",
    "Remove Friend\0" "Remove Friend\0" "Remove \0" " from your friends?",
    "Clear requests\0" "Clear all\0" "Clear all \0" " friend requests?",
};
bool confirm(ApplicationFriends& friends, const UiFriendsTarget& target, bool allow_hidden = false) noexcept {
    allow_hidden = allow_hidden || friends.requests_visible;
    const bool bulk = target.action == UiFriendsAction::clear_incoming;
    const auto* current = relationship_store_find(friends.store, target.user_id);
    const bool stranger_block = !bulk && !current && target.action == UiFriendsAction::block && target.user_id;
    if ((!bulk && !current && !stranger_block) || request_busy(friends) || (target.action == UiFriendsAction::accept &&
        (current->type != RelationshipType::incoming ||
            (!allow_hidden && (current->ignored || current->spam))))) return false;
    friends.confirmation = target;friends.confirmation_type = current ? current->type : RelationshipType::none;
    friends.confirmation_epoch = friends.membership_epoch;friends.confirmation_hidden = allow_hidden;
    friends.confirmation_ignored = current && current->ignored;friends.confirmation_spam = current && current->spam;
    friends.confirmation_stranger = current && current->stranger;friends.confirmation_stranger_known = current && current->stranger_known;
    ++friends.confirmation_serial;
    NameList texts{kConfirmTexts[target.action == UiFriendsAction::accept ? 0 : target.action == UiFriendsAction::block ? 1 :
        target.action == UiFriendsAction::ignore ? 2 : bulk ? 4 : 3]};
    const Utf8View title = texts.name();
    texts.next();
    const Utf8View accept = texts.name();
    texts.next();
    u32 count = 0;
    for (u32 i = 0; bulk && i < friends.store.count; ++i) if (friends.records[i].type == RelationshipType::incoming) ++count;
    if (bulk && !count) return false;
    ProtocolWriter writer{{reinterpret_cast<u8*>(friends.confirmation_text), sizeof(friends.confirmation_text)}, 0};
    const auto append = [&writer](Utf8View text) noexcept {
        return protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(text.data), text.bytes);
    };
    bool written = append(texts.name());
    texts.next();
    if (bulk) {
        written = written && protocol_writer_append_u64(&writer, count);
    } else {
        Utf8View name{};
        if (current)
            name = relationship_display_name(*current, g_app.account_ui->user_store);
        else if (const auto* user = user_store_find(g_app.account_ui->user_store, target.user_id))
            name = user_store_name(g_app.account_ui->user_store, *user);
        written = written && append(name.bytes ? name : utf8_literal("this user"));
    }
    if (!written || !append(texts.name())) return false;
    search_focus(friends, false);
    ApplicationTextFormRequest request{title, {friends.confirmation_text, writer.used}, {}, accept, {},
        {g_app.account.status.profile_id, friends.confirmation_serial, target.user_id}, confirmation_valid, confirmation_save, 0, true};
    request.danger = target.action != UiFriendsAction::accept && target.action != UiFriendsAction::ignore;
    return application_text_form_open(request);
}
bool activate(const UiFriendsTarget& target) noexcept {
    auto* friends = g_app.friends;
    UiFriendsTarget checked{};
    if (!friends || !ui_friends_scene_activation(input_scene(*friends), target, &checked)) return false;
    if (checked.action == UiFriendsAction::active_toggle) {
        friends->active_collapsed = !friends->active_collapsed;
        return repaint();
    }
    if (checked.action == UiFriendsAction::active_card) {
        const auto& scene = input_scene(*friends);
        return application_profile_open(checked.user_id, scene.active_rect);
    }
    if (checked.action == UiFriendsAction::toggle_section) {
        const u32 index = checked.section == UiFriendsSectionKind::incoming ? 0 : 1;
        friends->collapsed[index] = !friends->collapsed[index];project(*friends, true);return repaint();
    }
    if (checked.action == UiFriendsAction::toggle_hidden) {
        if (friends->requests_visible) close_requests(*friends);
        else { search_focus(*friends, false);friends->requests_visible = true;project(*friends, true); }
        if (!repaint()) { close_requests(*friends);return false; }
        g_app.context_focus = &input_scene(*friends).widgets;return true;
    }
    if (checked.action == UiFriendsAction::clear_incoming) return confirm(*friends, checked);
    if (checked.action == UiFriendsAction::clear_hidden) {
        const bool queued = queue(RelationshipOperation::clear_hidden);
        if (queued) { close_requests(*friends);return repaint(); }return false;
    }
    if (checked.action == UiFriendsAction::select_tab) {
        // A finished Add Friend result stays with its tab.
        if (friends->status.data == friends->add_status && !request_busy(*friends)) friends->status = {};
        search_focus(*friends, false);friends->tab = checked.tab;project(*friends, true);
        if (!ui_friends_tab_requests(checked.tab)) friends->friends_tab = checked.tab;
        // Add Friend opens on its username field.
        if (checked.tab == UiFriendsTab::add_friend) {
            if (!repaint()) return false;
            search_focus(*friends, true);
        }
        return repaint();
    }
    if (checked.action == UiFriendsAction::search || checked.action == UiFriendsAction::note) {
        search_focus(*friends, true, checked.action == UiFriendsAction::note);return repaint();
    }
    if (checked.action == UiFriendsAction::clear_search) {
        auto* model = friends_search_document(&friends->search, friends->tab);
        if (!model) return false;
        message_composer_clear(model);search_focus(*friends, true);return application_friends_search_changed(true);
    }
    if (checked.action == UiFriendsAction::more) return more(*friends, checked);
    if (checked.action == UiFriendsAction::profile) return application_profile_open(checked.user_id, input_scene(*friends).surface);
    if (checked.action == UiFriendsAction::open_dm) {
        const bool queued = queue_private_conversation(PrivateConversationOperation::open, checked.user_id);
        if (queued) { application_friends_hide();return synchronize_navigation_view() && synchronize_shell_layout() && refresh_account_text(true) && application_request_frame(); }
        return false;
    }
    // A message request: Accept DM moves it to the conversations, Ignore drops it.
    if (checked.action == UiFriendsAction::accept_dm || checked.action == UiFriendsAction::ignore_dm) {
        const u64 channel = dm_channel(*friends, checked.user_id);
        return channel && (checked.action == UiFriendsAction::accept_dm ? application_private_channel_accept_request(channel)
                                                                       : application_private_channel_ignore_request(channel)) &&
            repaint();
    }
    if (checked.action == UiFriendsAction::add_friend) {
        if (friends->tab == UiFriendsTab::add_friend) return submit_add(*friends);
        // An empty Online or All tab leads to Add Friend.
        return activate({UiFriendsAction::select_tab, 0, UiFriendsTab::add_friend});
    }
    if (checked.action == UiFriendsAction::nickname) return open_nickname(*friends, checked.user_id);
    if (checked.action == UiFriendsAction::voice_call) {
        // The DM opens first; the call starts once it is the selected conversation.
        if (!queue_private_conversation(PrivateConversationOperation::open, checked.user_id)) return false;
        friends->call_user = checked.user_id;
        application_friends_hide();
        return synchronize_navigation_view() && synchronize_shell_layout() && refresh_account_text(true) &&
               application_request_frame();
    }
    if (checked.action == UiFriendsAction::retry && friends->stranger_confirmation) {
        const auto* current = relationship_store_find(friends->store, friends->request.user_id);
        if (!current || !request_current(*friends, friends->request)) {
            friends->request.phase = FriendsRequestPhase::idle;friends->stranger_confirmation = false;
            recovery(*friends, "This request changed. Refreshing…");
            return queue(RelationshipOperation::list);
        }
        return confirm(*friends, {UiFriendsAction::accept, friends->request.user_id, friends->tab}, friends->request.allow_hidden);
    }
    if (checked.action == UiFriendsAction::retry) {
        if (!request_current(*friends, friends->request))
            return queue(RelationshipOperation::list);
        return queue(friends->request.operation, friends->request.user_id,
            {friends->request.username, friends->request.username_bytes}, friends->request.allow_hidden);
    }
    const auto* relationship = relationship_store_find(friends->store, checked.user_id);
    if (checked.action == UiFriendsAction::remove || checked.action == UiFriendsAction::block || checked.action == UiFriendsAction::ignore ||
        (checked.action == UiFriendsAction::accept && relationship && relationship->stranger_known && relationship->stranger))
        return confirm(*friends, checked);
    const auto operation = checked.action == UiFriendsAction::accept ?
        relationship && relationship->stranger_known ? RelationshipOperation::accept_stranger : RelationshipOperation::accept :
        checked.action == UiFriendsAction::unignore ? RelationshipOperation::unignore :
        checked.action == UiFriendsAction::block ? RelationshipOperation::block : RelationshipOperation::remove;
    return queue(operation, checked.user_id, {}, friends->requests_visible);
}
bool search_host_ready(const TextInputTarget& target) noexcept {
    return application_friends_search_focused() && target.context == application_friends_search_model();
}
using SearchInput = ForwardedInput<search_host_ready, text_input_single_line_forbidden, true>;
TextInputResult search_replace(const TextInputTarget& target, u32 a, u32 b, ConstSlice<u16> input, TextInputChange* change) noexcept {
    if (!change || a > b || (!input.data && input.count)) return TextInputResult::invalid_input;
    auto* model = static_cast<MessageComposer*>(target.context);
    u32 first = 0, last = 0;
    if (!utf8_byte_offset_for_utf16_position(message_composer_text(*model), a, &first) ||
        !utf8_byte_offset_for_utf16_position(message_composer_text(*model), b, &last)) return TextInputResult::invalid_utf16;
    char text[kRelationshipNoteMaximumBytes];u32 bytes = 0;
    const u32 available = model->capacity - (model->bytes - (last - first));
    if (input.count > available) return TextInputResult::capacity;
    for (u32 i = 0; i < input.count; ++i) {
        u32 scalar = input[i];
        if (scalar == '\r' || scalar == '\n' || scalar == 0x2028 || scalar == 0x2029 || !scalar) return TextInputResult::invalid_input;
        if (scalar >= 0xD800 && scalar <= 0xDBFF) {
            if (i + 1 == input.count || input[i + 1] < 0xDC00 || input[i + 1] > 0xDFFF) return TextInputResult::invalid_utf16;
            scalar = 0x10000 + ((scalar - 0xD800) << 10) + input[++i] - 0xDC00;
        } else if (scalar >= 0xDC00 && scalar <= 0xDFFF) return TextInputResult::invalid_utf16;
        char encoded[4];const u32 count = utf8_encode(scalar, encoded);
        if (!count) return TextInputResult::invalid_utf16;
        if (count > available - bytes || count > sizeof(text) - bytes) return TextInputResult::capacity;
        copy_bytes(text + bytes, encoded, count);bytes += count;
    }
    // Add Friend's note keeps to its 120 characters.
    if (g_app.friends && model == &g_app.friends->search.note &&
        utf8_validate(message_composer_text(*model)).codepoints -
                utf8_validate({model->text + first, last - first}).codepoints + utf8_validate({text, bytes}).codepoints >
            kRelationshipNoteMaximumCodepoints)
        return TextInputResult::capacity;
    const auto result = message_composer_replace_range(model, first, last, {text, bytes});
    if (result == MessageComposerResult::changed || result == MessageComposerResult::unchanged) {
        *change = {a, b, a + input.count};return result == MessageComposerResult::changed ? TextInputResult::changed : TextInputResult::unchanged;
    }
    return result == MessageComposerResult::capacity ? TextInputResult::capacity : TextInputResult::invalid_input;
}
// Everything but the replacement forwards to the composer's own operations (the shared table), as the other single-line
// fields do; this field decodes its text itself.
constexpr TextInputOperations search_operations = [] {
    TextInputOperations operations = SearchInput::operations;
    operations.replace = search_replace;
    return operations;
}();
// Add Friend's note takes at most 120 characters: what replacing the selection with `added` would leave.
bool note_fits(const ApplicationFriends& friends, const MessageComposer& model, u32 added) noexcept {
    if (&model != &friends.search.note) return true;
    const auto text = message_composer_text(model);
    const u32 first = minimum(model.anchor, model.caret), last = maximum(model.anchor, model.caret);
    return utf8_validate(text).codepoints - utf8_validate({text.data + first, last - first}).codepoints + added <=
        kRelationshipNoteMaximumCodepoints;
}
bool search_keys(ApplicationFriends& friends, UiInputKind kind, u32 key) noexcept {
    auto* query = friends_search_document(&friends.search, friends.tab);
    const bool raw_control = application_control_down(), alt = application_alt_down();
    const bool control = raw_control && !alt, shift = application_shift_down();
    if (kind == UiInputKind::key_down && shortcut_matches(ShortcutAction::search, shortcut_chord_from_keyboard(key)) &&
        query) {
        search_focus(friends, true);message_composer_select_all(query);return application_friends_search_changed(false);
    }
    auto* model = edit_model(friends);
    if (!model || !application_friends_search_focused()) return false;
    if (kind == UiInputKind::character) {
        if (control || (alt && !raw_control) || key < 32 || key == 127 || key == 0x2028 || key == 0x2029) return true;
        // A low surrogate completes the character its high half already counted.
        if (!note_fits(friends, *model, key >= 0xDC00 && key <= 0xDFFF ? 0 : 1)) return true;
        const auto result = message_composer_insert_utf16(model, static_cast<u16>(key), clock_monotonic_milliseconds());
        if (result == MessageComposerResult::changed) application_friends_search_changed(true);
        return true;
    }
    if (kind != UiInputKind::key_down || alt) return false;
    if (key == key::space && !control) return true;
    if (key == key::up || key == key::down || key == key::page_up || key == key::page_down) return true;
    if (key == key::tab) { search_focus(friends, false);return false; }
    // The note is one line: Enter there does nothing, as in the official client.
    if (key == key::enter && friends.note_focused) { friends.held_key = key;return true; }
    if (key == key::enter && friends.tab == UiFriendsTab::add_friend) {
        // Enter sends the request and keeps the field for the next name.
        friends.held_key = key;
        if (model->bytes) submit_add(friends);
        return true;
    }
    if (key == key::enter || key == key::escape) {
        if (key == key::escape && model->bytes) message_composer_clear(model);
        else { search_focus(friends, false);ui_friends_scene_navigate(&friends.scene, UiListMove::first); }
        friends.held_key = key;application_friends_search_changed(true);return true;
    }
    const u32 text_revision = model->text_revision;
    MessageComposerResult result = MessageComposerResult::unchanged;
    if (key == key::backspace) friends.held_key = key;
    if (control && key == 'V') {
        char text[kRelationshipNoteMaximumBytes];const auto read = clipboard_read_utf8({text, sizeof(text)});
        bool line = read.ok();
        for (u32 i = 0; line && i < read.bytes;) {
            const auto scalar = utf8_decode_one(text + i, read.bytes - i);
            line = scalar.valid && scalar.codepoint && scalar.codepoint != '\r' && scalar.codepoint != '\n' &&
                scalar.codepoint != 0x2028 && scalar.codepoint != 0x2029;
            if (line) i += scalar.width;
        }
        if (line && note_fits(friends, *model, utf8_validate({text, read.bytes}).codepoints))
            result = message_composer_insert_utf8(model, {text, read.bytes});
    } else if (!message_composer_edit_key(model, key, control, shift, &result)) return false;
    if (result == MessageComposerResult::changed) application_friends_search_changed(text_revision != model->text_revision);
    return true;
}
}
MessageComposer* application_friends_search_model() noexcept {
    return g_app.friends ? edit_model(*g_app.friends) : nullptr;
}
bool application_friends_search_focused() noexcept {
    return g_app.friends && g_app.friends->search_focused && application_friends_search_model() && search_available() &&
        (!g_app.context_focus || g_app.context_focus == &g_app.friends->scene.widgets);
}
TextInputTarget application_friends_search_target() noexcept {
    auto* model = application_friends_search_model();return {model, &search_operations, model ? model->document_identity : 0};
}
void application_friends_search_cancel_native_group(const TextInputTarget& target) noexcept {
    if (!g_app.friends || target.operations != &search_operations) return;
    auto& documents = g_app.friends->search;
    for (u32 i = 0; i <= kFriendsSearchDocuments; ++i) {
        auto* model = i < kFriendsSearchDocuments ? &documents.models[i] : &documents.note;
        if (target.context != model || target.identity != model->document_identity) continue;
        const auto cleanup = message_composer_input_target(model);
        text_input_end_group(cleanup);text_input_cancel(cleanup);return;
    }
}
bool application_friends_search_changed(bool content) noexcept {
    auto* friends = g_app.friends;if (!friends) return false;
    restart_search_blink(*friends);
    // Typing a new name clears the previous send's result.
    if (content && friends->tab == UiFriendsTab::add_friend && friends->status.data == friends->add_status)
        friends->status = {};
    if (content) project(*friends);
    return repaint();
}
bool application_friends_user_relation(u64 user_id, u8* relation) noexcept {
    const auto* store = application_friends_relationships();
    if (!store)
        return false;
    const auto* current = relationship_store_find(*store, user_id);
    *relation = !current ? 0 : current->type == RelationshipType::friend_ ? 1 : current->type == RelationshipType::incoming ? 2 :
        current->type == RelationshipType::outgoing ? 3 : current->type == RelationshipType::blocked ? 4 : 0;
    return true;
}
bool application_friends_user_action(u64 user_id, ApplicationFriendsUserAction action) noexcept {
    auto* friends = g_app.friends;
    if (!friends || !g_app.account_ui || !user_id || !application_friends_relationships() || request_busy(*friends))
        return false;
    const auto* current = relationship_store_find(friends->store, user_id);
    switch (action) {
    case ApplicationFriendsUserAction::add:
        return !current && queue(RelationshipOperation::accept, user_id, {}, false, true);
    case ApplicationFriendsUserAction::accept:
        if (!current || current->type != RelationshipType::incoming) return false;
        if (current->stranger_known && current->stranger)
            return confirm(*friends, {UiFriendsAction::accept, user_id, friends->tab}, true);
        return queue(current->stranger_known ? RelationshipOperation::accept_stranger : RelationshipOperation::accept,
            user_id, {}, true);
    case ApplicationFriendsUserAction::cancel:
        return current && current->type == RelationshipType::outgoing && queue(RelationshipOperation::remove, user_id, {}, true);
    case ApplicationFriendsUserAction::remove:
        return current && current->type == RelationshipType::friend_ &&
            confirm(*friends, {UiFriendsAction::remove, user_id, friends->tab}, true);
    case ApplicationFriendsUserAction::block:
        return (!current || current->type != RelationshipType::blocked) &&
            confirm(*friends, {UiFriendsAction::block, user_id, friends->tab}, true);
    case ApplicationFriendsUserAction::unblock:
        return current && current->type == RelationshipType::blocked && queue(RelationshipOperation::remove, user_id, {}, true);
    case ApplicationFriendsUserAction::unignore:
        return current && current->ignored && queue(RelationshipOperation::unignore, user_id, {}, true);
    }
    return false;
}
bool application_friends_popup_action(u32 action) noexcept {
    auto* friends = g_app.friends;
    if (!friends || g_app.popup_owner_kind != PopupOwnerKind::friends || !g_app.popup_scene.active ||
        action < friends_popup_action_base || action > friends_popup_action_base + static_cast<u32>(UiFriendsAction::voice_call)) return false;
    const UiFriendsTarget target{static_cast<UiFriendsAction>(action - friends_popup_action_base), g_app.popup_scene.owner_id, friends->tab};
    UiFriendsTarget checked{};
    if ((target.action != UiFriendsAction::profile && target.action != UiFriendsAction::remove && target.action != UiFriendsAction::block &&
        target.action != UiFriendsAction::ignore && target.action != UiFriendsAction::unignore &&
        target.action != UiFriendsAction::nickname && target.action != UiFriendsAction::voice_call) ||
        !ui_friends_scene_activation(input_scene(*friends), target, &checked)) { dismiss_popup();return true; }
    dismiss_popup();return activate(checked);
}
namespace {
// Whether a shown Active Now card has a timer to advance.
bool active_timed(const ApplicationFriends& friends) noexcept {
    if (!friends.scene.active_visible || friends.scene.active_collapsed) return false;
    for (u32 i = 0; i < friends.active_now.count; ++i)
        if (friends.active_cards[i].activity && ui_activity_card_timed(friends.active_details[i])) return true;
    return false;
}
// Advances the Active Now cards' timers; a timer whose text changed length lays its card out again.
void tick_active(ApplicationFriends& friends, u64 now) noexcept {
    friends.active_tick_at = 0;
    if (!application_friends_active() || !active_timed(friends)) return;
    bool changed = false, timed = false;
    const u64 unix_now = clock_unix_milliseconds();
    for (u32 i = 0; i < friends.active_now.count; ++i) {
        auto& card = friends.active_cards[i];
        if (!card.activity) continue;
        auto& detail = friends.active_details[i];
        const auto tick = ui_activity_card_tick(&detail, friends.active_sources[i], unix_now);
        if (tick == UiActivityCardTick::relayout) {
            if (ui_activity_card_layout(friends.active_sources[i], {UiActivityCardSize::compact, kActiveDetailWidth,
                                                                    unix_now, false, false, false, false, nullptr,
                                                                    nullptr}, &detail))
                active_card_images(friends, i, &card);
            else {
                card.activity = nullptr;
                card.image_count = 0;
            }
        }
        changed = changed || tick != UiActivityCardTick::unchanged;
        timed = timed || (card.activity && ui_activity_card_timed(detail));
    }
    if (timed) friends.active_tick_at = deadline_after(now, 1000);
    if (changed) {
        if (++friends.active_now.revision == 0) ++friends.active_now.revision;
        repaint();
    }
}
}
void application_friends_tick(u64 now) noexcept {
    auto* friends = g_app.friends;if (!friends) return;
    if (friends->active_tick_at && deadline_due(now, friends->active_tick_at)) tick_active(*friends, now);
    if (!friends->search_blink_at || !deadline_due(now, friends->search_blink_at)) return;
    if (!application_friends_search_focused() || (g_app.window && !application_window_keyboard_focus())) {
        friends->search_blink_at = 0;friends->search_caret_visible = false;
    } else {
        friends->search_caret_visible = !friends->search_caret_visible;
        friends->search_blink_at = deadline_after(now, friends->search_blink_ms);
    }
    repaint();
}
// The caret blink, and the Active Now timers while the column shows them.
u64 application_friends_search_next_wake() noexcept {
    const auto* friends = g_app.friends;
    if (!friends) return 0;
    const u64 caret = application_friends_search_focused() ? friends->search_blink_at : 0;
    const u64 timers = application_friends_active() ? friends->active_tick_at : 0;
    return !caret ? timers : !timers ? caret : minimum(caret, timers);
}
bool application_friends_active() noexcept {
    return g_app.friends && g_app.friends->visible && g_app.account_ui && g_app.account_ui->chat_state.home_selected && !g_app.login.visible;
}
bool application_friends_requests_active() noexcept {
    return application_friends_active() && g_app.friends->requests_visible;
}
void application_friends_close_requests() noexcept {
    if (g_app.friends) close_requests(*g_app.friends);
}
namespace {
bool show(UiFriendsTab tab) noexcept {
    if (!g_app.account_ui || !g_app.renderer_ready || g_app.login.visible || g_app.text_form || application_settings_active()) return false;
    auto* friends = ensure();if (!friends) return false;
    const auto selected = chat_state_store_select_home(&g_app.account_ui->chat_state);
    if (!selected.ok()) return false;
    // Friends and Message Requests share the page; the sidebar marks Message Requests while it shows.
    if (friends->tab != tab) { search_focus(*friends, false);friends->tab = tab;project(*friends, true); }
    application_private_channel_set_requests_mode(ui_friends_tab_requests(tab));
    friends->visible = true;
    application_composer_text_input_suspend();dismiss_popup();application_profile_close();application_image_viewer_close();application_tabs_note_navigation();
    g_app.message_composer.focused = false;g_app.context_focus = nullptr;ui_scroll_host_cancel(&g_app.scroll_host);
    if (!synchronize_navigation_view() || !synchronize_selected_channel() || !synchronize_shell_layout() || !repaint()) {
        application_friends_hide();return false;
    }
    g_app.context_focus = &friends->scene.widgets;
    if (!friends->ready && !friends->snapshot_active && !request_busy(*friends)) queue(RelationshipOperation::list);
    return true;
}
}
bool application_friends_show() noexcept {
    const auto* friends = g_app.friends;
    return show(friends && !ui_friends_tab_requests(friends->tab) ? friends->tab
                : friends ? friends->friends_tab : UiFriendsTab::online);
}
bool application_friends_show_requests() noexcept {
    const auto* friends = g_app.friends;
    return show(friends && ui_friends_tab_requests(friends->tab) ? friends->tab : UiFriendsTab::requests);
}
void application_friends_hide() noexcept {
    auto* friends = g_app.friends;if (!friends) return;
    // The sidebar stops marking Message Requests once the page gives way to a conversation.
    if (friends->visible && ui_friends_tab_requests(friends->tab)) application_private_channel_set_requests_mode(false);
    application_text_form_cancel_owner(confirmation_valid);
    if (g_app.popup_owner_kind == PopupOwnerKind::friends) dismiss_popup();
    close_requests(*friends);
    search_focus(*friends, false);
    if (g_app.context_focus == &friends->scene.widgets) g_app.context_focus = nullptr;
    if (friends->pressed.action != UiFriendsAction::none || friends->dragging || friends->search_dragging) application_release_capture();
    friends->visible = false;reset_value(&friends->pressed);friends->dragging = false;friends->search_dragging = false;
    renderer_bind_friends_scene(&g_app.renderer, nullptr);ui_friends_scene_release(&friends->scene);
}
void application_friends_release() noexcept {
    application_text_form_cancel_owner(nickname_valid);
    application_text_form_cancel_owner(confirmation_valid);
    application_friends_hide();
    auto* friends = g_app.friends;g_app.friends = nullptr;
    if (g_app.account_ui) g_app.account_ui->chat_state.relationships = nullptr;
    if (friends) {
        friends_search_documents_release(&friends->search);
        stable_storage_release(&friends->requests_indices);
        friends_relationship_storage_release(&friends->storage);pages_release(friends);
    }
}
namespace {
// Start Voice Call: once the friend's DM finished opening and is the selected
// conversation, its call starts (and rings them); a failed or redirected open drops it.
void start_pending_call(ApplicationFriends& friends) noexcept {
    if (!friends.call_user || !g_app.account_ui) return;
    auto& ui = *g_app.account_ui;
    const auto phase = ui.private_conversation.phase;
    if (phase == PrivateConversationPhase::queued || phase == PrivateConversationPhase::awaiting ||
        phase == PrivateConversationPhase::retry_wait) return;
    const u64 user = friends.call_user, channel = ui.chat_state.selected_channel_id;
    friends.call_user = 0;
    const auto* record = chat_state_store_find_channel(ui.chat_state, channel);
    if (phase == PrivateConversationPhase::idle && record && record->type == 1 &&
        private_channel_peer(ui.chat_state, channel) == user)
        application_private_channel_call(channel);
}
}
bool application_friends_synchronize() noexcept {
    auto* friends = g_app.friends;if (!friends) return true;
    start_pending_call(*friends);
    if (!application_friends_active()) { if (friends->scene.active) application_friends_hide();return true; }
    if (application_settings_active()) { close_requests(*friends);return true; }
    project(*friends);
    const bool requests_page = ui_friends_tab_requests(friends->tab);
    if (requests_page) project_dm(*friends);
    if (friends->requests_visible && !friends->requests_sections.hidden_count) close_requests(*friends);
    project_content(*friends);
    const auto phase = friends->request.phase == FriendsRequestPhase::failed ? UiFriendsPhase::error :
        !friends->ready || (request_busy(*friends) && friends->request.operation == RelationshipOperation::list) ? UiFriendsPhase::loading :
        friends->incomplete ? UiFriendsPhase::error : UiFriendsPhase::ready;
    ui_friends_scene_set_focus_visible(&friends->scene, g_app.keyboard_focus_visible);
    project_active(*friends);
    // The title and tabs sit in the shell header's free span (its title label, the header's height).
    const auto* bar = ui_scene_node(g_app.shell_scene, UiSemantic::title_bar);
    const auto* label = ui_scene_node(g_app.shell_scene, UiSemantic::title_label);
    const UiRect toolbar = bar && label ? UiRect{label->rectangle.left, bar->rectangle.top, label->rectangle.right,
                                                 bar->rectangle.bottom} : UiRect{};
    if (!ui_friends_scene_update(&friends->scene, requests_page ? &friends->dm_source : &friends->source, friends->tab,
        requests_page ? UiFriendsPhase::ready : phase, requests_page ? Utf8View{} : friends->status,
        ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::message_view), &friends->content,
        friends->tab == UiFriendsTab::pending ? &friends->sections : nullptr, requests_page ? nullptr : &friends->active_now,
        toolbar)) return false;
    // The column's timers run while it shows them (it can reappear with a wider window).
    if (!friends->active_tick_at && active_timed(*friends))
        friends->active_tick_at = deadline_after(clock_monotonic_milliseconds(), 1000);
    // Row plates fade with the pointer (the shared hover motion; reduced motion snaps). The
    // renderer keys its presentation on the fade, so input revisions stay put while it runs.
    friends->scene.row_hover = &friends->row_hover;
    if (application_motion_hover_tick(&friends->row_hover, ui_friends_scene_hovered_row(friends->scene),
                                      clock_monotonic_milliseconds(), kUiMotionRowHover))
        application_request_frame();
    ui_friends_scene_name_hover(&friends->scene);
    bool changed = g_app.renderer.friends_scene != &friends->scene ||
        g_app.renderer.interaction_text_friends_revision != friends->scene.revision;
    renderer_bind_friends_scene(&g_app.renderer, &friends->scene);
    if (friends->requests_visible) {
        ui_friends_scene_set_focus_visible(&friends->requests_scene, g_app.keyboard_focus_visible);
        if (!ui_friends_scene_update(&friends->requests_scene, &friends->requests_source, UiFriendsTab::pending,
            phase, friends->status, application_viewport_dips(), nullptr, &friends->requests_sections)) return false;
        changed = changed || g_app.renderer.friends_requests_scene != &friends->requests_scene ||
            g_app.renderer.interaction_text_friends_requests_revision != friends->requests_scene.revision;
        renderer_bind_friends_requests_scene(&g_app.renderer, &friends->requests_scene);
    } else {
        changed = changed || g_app.renderer.friends_requests_scene != nullptr;
        renderer_bind_friends_requests_scene(&g_app.renderer, nullptr);
    }
    if (changed && !renderer_rebuild_content_text(&g_app.renderer, application_theme())) return false;
    application_composer_text_input_notify_layout();
    return application_composer_text_input_synchronize();
}
bool application_friends_pending(u64 now) noexcept {
    if (g_app.friends && g_app.friends->request.phase == FriendsRequestPhase::failed) {
        auto& request = g_app.friends->request;
        const auto phase = application_action_captcha_phase(ApplicationActionCaptchaKind::friend_request,
                                                            captcha_subject(request));
        if (phase == ApplicationActionCaptchaPhase::solved) {
            request.phase = FriendsRequestPhase::queued;
            captcha_retry = true;
            g_app.friends->status = application_action_captcha_status(phase);
            project(*g_app.friends, true);
            repaint();
        } else if (phase == ApplicationActionCaptchaPhase::failed &&
                   g_app.friends->status.data != application_action_captcha_status(phase).data) {
            g_app.friends->status = application_action_captcha_status(phase);
            project(*g_app.friends, true);
            repaint();
        }
    }
    return g_app.friends && (g_app.friends->request.phase == FriendsRequestPhase::queued ||
        (g_app.friends->request.phase == FriendsRequestPhase::retry && deadline_due(now, g_app.friends->request.retry_at)));
}
u64 application_friends_next_wake() noexcept {
    if (!g_app.friends || g_app.message_history_request_owner != MessageHistoryRequestOwner::none) return 0;
    const auto& request = g_app.friends->request;
    return request.phase == FriendsRequestPhase::queued ? 1 : request.phase == FriendsRequestPhase::retry ? request.retry_at : 0;
}
bool application_friends_submit(u64 id, u64 now) noexcept {
    if (!id || !application_friends_pending(now) || !g_app.account.instance) return false;
    auto& friends = *g_app.friends;auto& request = friends.request;
    if (!request_current(friends, request)) {
            request.phase = FriendsRequestPhase::failed;request.operation = RelationshipOperation::list;
            request.user_id = 0;request.username_bytes = 0;
            friends.status = utf8_literal("Your friends list changed. Try again.");
            project(friends, true);repaint();return false;
    }
    if (captcha_retry) {
        captcha_retry = false;
        application_action_captcha_arm(ApplicationActionCaptchaKind::friend_request,
                                       captcha_subject(request), id, false);
    }
    const auto result = account_module_relationships(g_app.account.instance, request.operation, request.user_id,
        {request.username, request.username_bytes}, id);
    if (!result.ok()) {
        if (result.error == RestClientSubmitError::busy) { request.phase = FriendsRequestPhase::retry;request.retry_at = deadline_after(now, 100); }
        else { request.phase = FriendsRequestPhase::failed;friends.status = utf8_literal("Something went wrong. Try again."); }
        repaint();return false;
    }
    request.phase = FriendsRequestPhase::awaiting;request.request_id = id;request.membership_epoch = friends.membership_epoch;
    return true;
}
bool application_friends_response(const RestClientResponse& response, Utf8View body) noexcept {
    auto* friends = g_app.friends;if (!friends) return true;
    auto& request = friends->request;
    if (request.phase != FriendsRequestPhase::awaiting || response.request_id != request.request_id) return true;
    if (response.transport_ok() && response.rejection.code == 80013 && request.operation == RelationshipOperation::accept) {
        const auto* current = relationship_store_find(friends->store, request.user_id);
        if (!current || !request_current(*friends, request)) {
            request.phase = FriendsRequestPhase::idle;friends->stranger_confirmation = false;
            recovery(*friends, "This request changed. Try again.");
            project(*friends, true);return repaint();
        }
        auto learned = *current;learned.stranger = true;learned.stranger_known = true;
        relationship_store_upsert(&friends->store, learned);invalidate_sources(*friends);
        request.expected_stranger = true;request.expected_stranger_known = true;
        request.phase = FriendsRequestPhase::failed;friends->stranger_confirmation = true;
        friends->status = utf8_literal("You may not know this person. Try again to confirm.");
        const bool painted = repaint();
        if (application_friends_active() && !application_settings_active() && !g_app.text_form && !g_app.popup_scene.active &&
            !(g_app.profile && g_app.profile->scene.active)) confirm(*friends, {UiFriendsAction::accept, request.user_id, friends->tab}, request.allow_hidden);
        return painted;
    }
    if (response.transport_ok() && response.status_code == 429 && response.retry_at_ms &&
        !api_rejection_requires_action(response.rejection)) {
        request.phase = FriendsRequestPhase::retry;request.retry_at = response.retry_at_ms;
        friends->status = utf8_literal("You're doing that too fast. Try again in a moment.");return repaint();
    }
    bool ok = response.succeeded();
    const bool bulk = request.operation == RelationshipOperation::clear_incoming || request.operation == RelationshipOperation::clear_hidden;
    if (ok && bulk) {
        // Server filter algebra is not assumed. The shared GET lane reconciles
        // the complete result and its epoch guard preserves newer live events.
        request.phase = FriendsRequestPhase::idle;
        friends->incomplete = true;
        return queue(RelationshipOperation::list);
    }
    if (ok && request.operation == RelationshipOperation::list) {
        if (friends->snapshot_active || request.membership_epoch != friends->membership_epoch) {
            request.phase = FriendsRequestPhase::idle;
            const Utf8View status{};
            if (friends->incomplete) recovery(*friends, nullptr, 0);else friends->status = status;
            return repaint();
        }
        const auto decoded = relationships_replace(&friends->store, body,
            {g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount},
            {friends->candidate, relationship_capacity}, {&friends->storage,
                +[](void* opaque, u32 count, bool active) noexcept {
                    auto* storage = static_cast<FriendsRelationshipStorage*>(opaque);
                    return active ? friends_relationship_storage_active(storage, count) :
                        friends_relationship_storage_candidate(storage, count);
                }});
        ok = decoded.ok();
        if (ok) {
            const auto hydrated = relationships_hydrate(&g_app.account_ui->user_store, body,
                {g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount});
            if (!hydrated.ok()) log_u64(LogLevel::warning, "friends", "user_hydration_error", static_cast<u64>(hydrated.error));
            friends->ready = true;friends->incomplete = false;++friends->membership_epoch;
            publish_dm_names();
        }
    } else if (ok && request.user_id && request.membership_epoch == friends->membership_epoch) {
        if (request.operation == RelationshipOperation::remove) ok = relationship_store_remove(&friends->store, request.user_id).ok();
        else {
            const auto* old = relationship_store_find(friends->store, request.user_id);
            RelationshipRecord record = old ? *old : RelationshipRecord{};
            record.user_id = request.user_id;
            if (request.operation == RelationshipOperation::ignore || request.operation == RelationshipOperation::unignore)
                record.ignored = request.operation == RelationshipOperation::ignore;
            else if (request.operation == RelationshipOperation::nickname) {
                copy_bytes(record.nickname, request.username, request.username_bytes);
                record.nickname_bytes = static_cast<u16>(request.username_bytes);
            } else record.type = request.operation == RelationshipOperation::block ? RelationshipType::blocked :
                request.unknown_target ? RelationshipType::outgoing : RelationshipType::friend_;
            ok = friends_relationship_storage_upsert(&friends->storage, friends->store, record.user_id) &&
                relationship_store_upsert(&friends->store, record).ok();
        }
        if (ok) ++friends->membership_epoch;
        if (ok && request.operation == RelationshipOperation::nickname) publish_dm_names();
    }
    request.phase = ok ? FriendsRequestPhase::idle : FriendsRequestPhase::failed;
    const bool captcha = !ok && application_action_captcha_capture(ApplicationActionCaptchaKind::friend_request,
        captcha_subject(request), response, body);
    friends->status = ok ? Utf8View{} :
        captcha ? utf8_literal("Discord needs a CAPTCHA first. Send again to verify in your browser.") :
        api_rejection_requires_action(response.rejection) ? utf8_literal("Verify your account to continue.") :
        utf8_literal("Something went wrong. Try again.");
    if (request.operation == RelationshipOperation::request && !captcha && !api_rejection_requires_action(response.rejection)) {
        // Add Friend reports under its field and stays ready for another name.
        // The request text is the username, then the note after a line feed.
        Utf8View name{request.username, request.username_bytes};
        for (u32 i = 0; i < name.bytes; ++i)
            if (name.data[i] == '\n') { name.bytes = i;break; }
        Utf8View first{}, last{};
        if (ok) {
            first = utf8_literal("Success! Your friend request to ");
            last = utf8_literal(" was sent.");
            if (auto* model = friends_search_document(&friends->search, UiFriendsTab::add_friend))
                message_composer_clear(model);
            message_composer_clear(&friends->search.note);
        } else {
            request.phase = FriendsRequestPhase::idle;
            u32 at = 0;
            while (at < sizeof(kAddFailureCodes) / sizeof(kAddFailureCodes[0]) &&
                   kAddFailureCodes[at] != response.rejection.code)
                ++at;
            const Utf8View text = name_list_at(kAddFailureTexts, kAddFailureText[at]);
            // Only a user not accepting requests is named, before the text.
            if (at)
                name = {};
            (at ? first : last) = text;
        }
        add_result(*friends, ok ? UiFriendsTone::positive : UiFriendsTone::danger, first, name, last);
    }
    if (ok && friends->incomplete)
        recovery(*friends, "Your friends list is out of date. Try again to refresh.");
    project(*friends, true);return repaint();
}
void application_friends_gateway(const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    auto* friends = g_app.friends;
    if (event.kind == GatewayClientEventKind::ready) {
        if (friends) {
            friends->snapshot_active = false;friends->snapshot_failed = false;
            ++friends->membership_epoch;
        }
        return;
    }
    if (event.kind != GatewayClientEventKind::dispatch || !g_app.account_ui) return;
    const auto kind = event.dispatch_kind;
    if (kind == GatewayClientDispatchKind::relationship_state_incomplete) {
        friends = ensure();if (!friends) return;
        if (friends->snapshot_active) friends->snapshot_failed = true;
        recovery(*friends, "Couldn't update your friends list. Try again.");
        invalidate_sources(*friends);
        application_request_frame();return;
    }
    if (kind == GatewayClientDispatchKind::relationship_snapshot_begin) {
        friends = ensure();if (!friends) return;
        friends->snapshot.count = 0;friends->snapshot_active = true;friends->snapshot_failed = false;friends->snapshot_sorted = false;
        friends->snapshot_sequence = event.sequence;friends->snapshot_items = 0;
        invalidate_sources(*friends);
        ++friends->membership_epoch;
        return;
    }
    if (kind == GatewayClientDispatchKind::presence_update) {
        if (!friends || event.presence.guild_id) return;
        if (friends->snapshot_active) sort_snapshot(*friends);
        const auto status = event.presence.status;
        const auto value = status == GatewayClientMemberListStatus::online ? RelationshipPresence::online :
            status == GatewayClientMemberListStatus::idle ? RelationshipPresence::idle :
            status == GatewayClientMemberListStatus::dnd ? RelationshipPresence::dnd :
            status == GatewayClientMemberListStatus::offline ? RelationshipPresence::offline : RelationshipPresence::unknown;
        const auto changed = relationship_store_presence(
            friends->snapshot_active ? &friends->snapshot : &friends->store, event.presence.user_id, value);
        if (changed.changed && !friends->snapshot_active) { invalidate_sources(*friends);application_request_frame(); }
        return;
    }
    if (kind == GatewayClientDispatchKind::relationship_snapshot_complete) {
        if (!friends || !friends->snapshot_active || event.sequence != friends->snapshot_sequence) return;
        sort_snapshot(*friends);
        const bool valid = !friends->snapshot_failed && event.state.record_count == friends->snapshot_items;
        const auto committed = valid && friends_relationship_storage_active(&friends->storage, friends->snapshot.count) ? relationship_store_replace(&friends->store,
            {friends->candidate, friends->snapshot.count}) : RelationshipStoreResult{RelationshipStoreError::invalid_input, false};
        friends->snapshot_active = false;
        invalidate_sources(*friends);
        if (committed.ok()) {
            friends->ready = true;friends->incomplete = false;++friends->membership_epoch;
            publish_dm_names();
            if (!request_busy(*friends)) { reset_value(&friends->request);reset_value(&friends->status); }
        } else {
            recovery(*friends, "Couldn't load your friends. Try again.");
        }
        application_request_frame();return;
    }
    const bool snapshot = kind == GatewayClientDispatchKind::relationship_snapshot_item;
    if (!snapshot && kind != GatewayClientDispatchKind::relationship_add && kind != GatewayClientDispatchKind::relationship_update &&
        kind != GatewayClientDispatchKind::relationship_remove) return;
    if (!friends && !snapshot) friends = ensure();
    if (!friends || (snapshot && (!friends->snapshot_active || event.sequence != friends->snapshot_sequence))) return;
    // The producer emits complete, pointer-free records. Never retain mailbox
    // storage or commit a partially decoded snapshot.
    RelationshipMutation mutation{};u32 cursor = 0;
    bool valid = event.state.record_count == 1 && gateway_client_relationship_record(payload, &cursor, &mutation) && cursor == payload.count;
    if (snapshot) {
        ++friends->snapshot_items;
        valid = valid && !friends->snapshot_sorted && friends->snapshot.count < relationship_capacity &&
            friends_relationship_storage_candidate(&friends->storage, friends->snapshot.count + 1);
        if (valid) friends->candidate[friends->snapshot.count++] = mutation.record;
        if (!valid) friends->snapshot_failed = true;
    } else if (valid) {
        const auto operation = kind == GatewayClientDispatchKind::relationship_remove ? RelationshipEvent::remove :
            kind == GatewayClientDispatchKind::relationship_add ? RelationshipEvent::add : RelationshipEvent::update;
        const auto applied = operation == RelationshipEvent::remove ||
            friends_relationship_storage_upsert(&friends->storage, friends->store, mutation.record.user_id) ?
            relationship_apply_decoded(&friends->store, operation, mutation) :
            RelationshipStoreResult{RelationshipStoreError::capacity, false};
        valid = applied.ok();
        if (applied.changed) {
            ++friends->membership_epoch;invalidate_sources(*friends);application_request_frame();
            publish_dm_names();
        }
        if (friends->snapshot_active) friends->snapshot_failed = true;
    }
    if (valid && mutation.user.id) {
        const u32 revision = g_app.account_ui->user_store.revision;
        const auto hydrated = relationship_users_apply(&g_app.account_ui->user_store, {&mutation.user, 1});
        if (revision != g_app.account_ui->user_store.revision) {
            invalidate_sources(*friends);if (!snapshot) application_request_frame();
        }
        if (!hydrated.ok()) log_u64(LogLevel::warning, "friends", "user_hydration_error", static_cast<u64>(hydrated.error));
    }
    if (!valid && !snapshot) {
        recovery(*friends, "Couldn't update your friends list. Try again.");
        invalidate_sources(*friends);
        application_request_frame();
    }
}
bool application_friends_input(UiInputKind kind, u32 key, bool repeated) noexcept {
    auto* friends = g_app.friends;
    if (friends && ui_input_consume_key_tail(&friends->held_key, {kind, key, repeated})) return true;
    if (!application_friends_active() || overlay_open()) return false;
    auto& scene = input_scene(*friends);
    const bool modal = friends->requests_visible;
    if (!modal && search_keys(*friends, kind, key)) return true;
    if (modal && kind == UiInputKind::key_down && key == key::escape) {
        friends->held_key = key;close_requests(*friends);return repaint();
    }
    if (!modal && g_app.context_focus && g_app.context_focus != &scene.widgets) return false;
    if (kind == UiInputKind::character) return true;
    if (kind != UiInputKind::key_down || application_control_down() || application_alt_down()) return modal;
    if (key == key::tab) {
        const auto next = ui_widget_manager_next_focus(scene.widgets, scene.widgets.focused,
            application_shift_down(), true);
        if (next.generation) {
            const auto* node = ui_widget_manager_layout_node(scene.widgets, next);
            if (node) ui_friends_scene_focus(&scene, ui_friends_scene_hit_test(scene,
                (node->rectangle.left + node->rectangle.right) * .5f, (node->rectangle.top + node->rectangle.bottom) * .5f));
            const auto focused = ui_friends_scene_focused(scene).action;
            search_focus(*friends, focused == UiFriendsAction::search || focused == UiFriendsAction::note,
                         focused == UiFriendsAction::note);
        }
    } else if (key == key::up || key == key::down || key == key::home || key == key::end)
        ui_friends_scene_navigate(&scene, key == key::up ? UiListMove::previous : key == key::down ? UiListMove::next : key == key::home ? UiListMove::first : UiListMove::last);
    else if (key == key::page_up || key == key::page_down) ui_friends_scene_page(&scene, key == key::page_up);
    else if ((key == key::enter || key == key::space) && !repeated) { friends->held_key = key;activate(ui_friends_scene_focused(scene)); }
    else return modal;
    if (!g_app.text_form && !g_app.popup_scene.active && !(g_app.profile && g_app.profile->scene.active))
        g_app.context_focus = &input_scene(*friends).widgets;
    return repaint();
}
bool application_friends_window_event(const WindowEvent& event) noexcept {
    auto* friends = g_app.friends;
    if (!application_friends_active()) return false;
    if (event.type == EventType::capture_lost || event.type == EventType::cancel_input || input_focus_lost(event)) {
        reset_value(&friends->pressed);friends->dragging = false;friends->search_dragging = false;
        if (input_focus_lost(event)) { friends->search_caret_visible = false;friends->search_blink_at = 0;application_composer_text_input_suspend();repaint(); }
        return false;
    }
    if (event.type == EventType::focus_changed && friends->search_focused) { application_friends_search_changed(false);return false; }
    if (!input_pointer(event) && event.type != EventType::context_menu) return false;
    if (overlay_open()) return false;
    auto& scene = input_scene(*friends);
    const bool modal = friends->requests_visible;
    float x = client_dip_x(event), y = client_dip_y(event);
    if (event.type == EventType::context_menu && (event.flags & ev_keyboard) != 0) {
        if (!modal && g_app.context_focus != &scene.widgets) return false;
        const auto focused = ui_friends_scene_focused(scene);
        if (focused.user_id) more(*friends, focused);
        return true;
    }
    if (input_vertical_wheel(event) || event.type == EventType::context_menu) {
        const UiPoint point = event.type == EventType::wheel ? client_wheel_dip(event) : client_context_dip(event);
        x = point.x;y = point.y;
        if (!ui_rect_contains(scene.viewport, x, y)) return modal;
        if (event.type == EventType::wheel) {
            ui_friends_scene_scroll(&scene, -static_cast<i16>(event.wheel.dy120) * .8f);repaint();
        } else {
            const auto hit = ui_friends_scene_hit_test(scene, x, y);
            if (hit.action == UiFriendsAction::active_card)
                application_user_menu_open(hit.user_id, {x, y, x + 1, y + 1}, 0, UserMenuSource::profile);
            else if (hit.user_id) more(*friends, hit, {x, y, x + 1, y + 1});
        }
        return true;
    }
    if (event.type == EventType::pointer_move && friends->dragging) {
        const auto geometry = ui_friends_scene_scrollbar(scene);
        const float target = ui_scrollbar_scroll_from_pointer(geometry, y, friends->thumb_grab);
        ui_friends_scene_scroll(&scene, target - scene.scroll_dips);repaint();return true;
    }
    if (event.type == EventType::pointer_move && friends->search_dragging) {
        search_pointer(*friends, x, y, true);
        return true;
    }
    const bool inside = ui_friends_scene_contains(scene, x, y);
    // A second press anywhere but the search field is another press.
    const auto double_hit = input_double(event) && inside ? ui_friends_scene_hit_test(scene, x, y).action : UiFriendsAction::none;
    const bool search_double = double_hit == UiFriendsAction::search || double_hit == UiFriendsAction::note;
    const bool press = input_press(event) || (input_double(event) && !search_double);
    if (event.type == EventType::pointer_move && ui_friends_scene_active_hover(&scene, inside ? x : -1.f, inside ? y : -1.f))
        application_request_frame();
    if (event.type == EventType::pointer_move) {
        const auto hit = inside && ui_friends_scene_hit_test(scene, x, y).action != UiFriendsAction::none ?
            ui_widget_manager_hit_test(scene.widgets, x, y) : ui_widget_invalid_handle();
        const auto old = scene.widgets.hovered;
        if (old.slot != hit.slot || old.generation != hit.generation) {
            ui_widget_manager_hover(&scene.widgets, hit);application_request_frame();
        }
    }
    if (!inside && friends->pressed.action == UiFriendsAction::none && !friends->dragging && !friends->search_dragging)
        return modal && (event.type == EventType::pointer_move || press || input_release(event) ||
            input_press(event, PointerButton::right) || input_release(event, PointerButton::right) ||
            input_press(event, PointerButton::middle) || input_release(event, PointerButton::middle));
    if (press) {
        const auto geometry = ui_friends_scene_scrollbar(scene);
        if (ui_scrollbar_track_hit(geometry, x, y)) {
            if (ui_scrollbar_thumb_hit(geometry, x, y)) { friends->dragging = true;friends->thumb_grab = y - geometry.thumb.top;window_capture_pointer(g_app.window); }
            else { const float page = maximum(1.f, geometry.track.bottom - geometry.track.top - 56.f);
                ui_friends_scene_scroll(&scene, y < geometry.thumb.top ? -page : page);repaint(); }
            return true;
        }
        friends->pressed = ui_friends_scene_hit_test(scene, x, y);
        if (friends->pressed.action == UiFriendsAction::search || friends->pressed.action == UiFriendsAction::note) {
            // The renderer measured the focused field: a press in the other one moves the focus there, caret last.
            const bool note = friends->pressed.action == UiFriendsAction::note, measured = note == friends->note_focused;
            search_focus(*friends, true, note);repaint();
            auto* model = edit_model(*friends);
            if (measured) search_pointer(*friends, x, y, input_shift(event));
            else if (model) { message_composer_set_selection(model, model->bytes, model->bytes);application_friends_search_changed(false); }
            reset_value(&friends->pressed);friends->search_dragging = measured;
            if (measured) window_capture_pointer(g_app.window);
            return true;
        }
        search_focus(*friends, false);
        if (friends->pressed.action != UiFriendsAction::none) {
            ui_friends_scene_focus(&scene, friends->pressed);g_app.context_focus = &scene.widgets;
            repaint();friends->pressed_revision = scene.layout_revision;window_capture_pointer(g_app.window);
        }
        return true;
    }
    if (input_release(event)) {
        const auto pressed = friends->pressed;const auto revision = friends->pressed_revision;
        reset_value(&friends->pressed);friends->dragging = false;friends->search_dragging = false;application_release_capture();
        if (revision == scene.layout_revision && ui_friends_target_equal(pressed, ui_friends_scene_hit_test(scene, x, y))) activate(pressed);
        return true;
    }
    if (search_double)
        return search_pointer(*friends, x, y, false, true);
    return inside && (event.type == EventType::pointer_move || input_press(event, PointerButton::right) ||
        input_release(event, PointerButton::right) ||
        (modal && (input_press(event, PointerButton::middle) || input_release(event, PointerButton::middle))));
}
} // namespace maicord::app_detail
