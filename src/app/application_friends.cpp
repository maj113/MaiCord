#include "application_action_captcha.hpp"
#include "maicord/core/memory.hpp"
#include "application_content_point.hpp"
#include "maicord/app/shortcut_registry.hpp"
#include "application_friends.hpp"
#include "application_state.hpp"
#include "application_settings.hpp"
#include "application_account_presentation.hpp"
#include "application_channel_session.hpp"
#include "application_composer_text_input.hpp"
#include "application_composer.hpp"
#include "application_private_conversation.hpp"
#include "application_profile.hpp"
#include "application_message_interaction.hpp"
#include "application_shell_input.hpp"
#include "application_text_form.hpp"
#include "application_image_viewer.hpp"
#include "application_friends_search.hpp"
#include "application_friends_storage.hpp"
#include "text_edit_keys.hpp"
#include "application_plugins.hpp"
#include "application_user_actions.hpp"
#include "maicord/model/active_now.hpp"
#include "maicord/model/friend_search.hpp"
#include "maicord/ui/clipboard.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/protocol/relationships.hpp"
#include "maicord/model/message_notifications.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/core/text_bank.hpp"
#include "maicord/core/utf.hpp"

#include "maicord/core/win32_user.hpp"
#include "maicord/sys/winsock.hpp"

namespace maicord::app_detail {
namespace {
constexpr u32 relationship_capacity = kFriendsRelationshipCapacity;
enum class FriendsRequestPhase : u8 { idle, queued, awaiting, retry, failed };
struct FriendsRequest {
    u64 user_id, request_id, retry_at, membership_epoch, intent_epoch;
    char username[kRelationshipUsernameMaximumBytes];
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
    u32 query_revision, total_count, edit_revision;
    u64 search_blink_at;
    u32 search_blink_ms;
    UiFriendsTab tab;
    bool visible, ready, snapshot_active, snapshot_failed, snapshot_sorted, incomplete, dragging;
    bool search_focused, search_caret_visible, search_dragging, stranger_confirmation;
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
};
const RelationshipStore* application_friends_relationships() noexcept {
    const auto* friends = g_app.friends;
    return friends && friends->ready && !friends->incomplete && !friends->snapshot_active
               ? &friends->store : nullptr;
}
bool application_friends_notification_gates(u64 user_id,u32* gates) noexcept {
    if(!gates || !g_app.friends || !g_app.friends->ready || g_app.friends->incomplete || g_app.friends->snapshot_active) return false;
    *gates=0;
    const auto* row=relationship_store_find(g_app.friends->store,user_id);
    if(row && row->type==RelationshipType::blocked) *gates|=notification_gate_blocked;
    if(row && row->ignored) *gates|=notification_gate_ignored;
    return true;
}
ApplicationFriendsStorageMetrics application_friends_storage_metrics() noexcept {
    const auto* friends = g_app.friends;
    if (!friends) return {sizeof(ApplicationFriends), {}, {}, {}, {}};
    return {sizeof(ApplicationFriends), friends_relationship_storage_metrics(friends->storage.active),
        friends_relationship_storage_metrics(friends->storage.candidate),
        friends_relationship_storage_metrics(friends->storage.indices), friends_relationship_storage_metrics(friends->requests_indices)};
}
namespace {
void close_requests(ApplicationFriends&) noexcept;
UiFriendsScene& input_scene(ApplicationFriends& friends) noexcept {
    return friends.requests_visible ? friends.requests_scene : friends.scene;
}
void invalidate_sources(ApplicationFriends& friends) noexcept {
    ++friends.source.revision;++friends.requests_source.revision;
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
void recovery(ApplicationFriends& friends, Utf8View status) noexcept {
    friends.incomplete = true;friends.status = status;
    if (!request_busy(friends)) {
        reset_value(&friends.request);friends.request.operation = RelationshipOperation::list;
        friends.request.phase = FriendsRequestPhase::failed;
    }
}
void sort_snapshot(ApplicationFriends& friends) noexcept {
    if (friends.snapshot_sorted) return;
    heap_sort(friends.candidate, friends.snapshot.count,
        [](const auto& a, const auto& b) noexcept { return a.user_id < b.user_id; });
    for (u32 i = 1; i < friends.snapshot.count; ++i)
        if (friends.candidate[i - 1].user_id == friends.candidate[i].user_id) friends.snapshot_failed = true;
    friends.snapshot_sorted = true;
}
bool resolve_record(const ApplicationFriends& friends, u32 index, UiFriendsRow* row, bool hidden) noexcept {
    if (!g_app.account_ui || index >= friends.store.count) return false;
    const auto& record = friends.records[index];
    const auto& users = g_app.account_ui->user_store;
    const auto* user = user_store_find(users, record.user_id);
    const auto name = relationship_display_name(record, users);
    *row = {record.user_id, name.bytes ? name : utf8_literal("Unknown user"),
        user ? user_store_username(users, *user) : Utf8View{},
        hidden ? record.ignored ? utf8_literal("Ignored") : utf8_literal("Potential spam") :
        record.type == RelationshipType::incoming ? utf8_literal("Incoming friend request") :
        record.type == RelationshipType::outgoing ? utf8_literal("Outgoing friend request") :
        record.type == RelationshipType::blocked ? utf8_literal("Blocked") :
        record.presence == RelationshipPresence::online ? utf8_literal("Online") :
        record.presence == RelationshipPresence::idle ? utf8_literal("Idle") :
        record.presence == RelationshipPresence::dnd ? utf8_literal("Do Not Disturb") :
        record.presence == RelationshipPresence::offline ? utf8_literal("Offline") : utf8_literal("Status unavailable"),
        application_friends_presence(record.presence), record.type == RelationshipType::incoming ? UiFriendsKind::incoming :
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
ApplicationFriends* ensure() noexcept {
    if (g_app.friends) return g_app.friends;
    auto* friends = static_cast<ApplicationFriends*>(VirtualAlloc(nullptr, sizeof(ApplicationFriends), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!friends) return nullptr;
    if (!friends_search_documents_initialize(&friends->search) ||
        !friends_relationship_storage_initialize(&friends->storage)) {
        friends_search_documents_release(&friends->search);
        friends_relationship_storage_release(&friends->storage);
        VirtualFree(friends, 0, MEM_RELEASE); return nullptr;
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
    friends->sections = {friends->section_rows, 2, 1, false, false, 0};
    friends->requests_sections = {nullptr, 0, 1, false, true, 0};
    friends->tab = UiFriendsTab::online;
    friends->search_blink_ms = GetCaretBlinkTime();
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
void sift(ApplicationFriends& friends, u32 root, u32 end) noexcept {
    for (;;) {
        u32 child = root * 2 + 1;
        if (child >= end) return;
        if (child + 1 < end && before(friends, friends.indices[child], friends.indices[child + 1])) ++child;
        if (!before(friends, friends.indices[root], friends.indices[child])) return;
        const auto value = friends.indices[root];friends.indices[root] = friends.indices[child];friends.indices[child] = value;root = child;
    }
}
void project(ApplicationFriends& friends, bool force = false) noexcept {
    const auto* query_model = friends_search_document(&friends.search, friends.tab);
    const auto query = query_model ? message_composer_text(*query_model) : Utf8View{};
    const u32 query_revision = query_model ? query_model->text_revision : 0;
    if (!g_app.account_ui || (!force && friends.store_revision == friends.store.revision &&
        friends.user_revision == g_app.account_ui->user_store.revision && friends.query_revision == query_revision)) return;
    if (!friends_relationship_storage_indices(&friends.storage, friends.store.count)) {
        recovery(friends, utf8_literal("Couldn't update your friends list. Try again."));
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
        friends.tab == UiFriendsTab::pending ? RelationshipFilter::pending :
        friends.tab == UiFriendsTab::blocked ? RelationshipFilter::blocked : RelationshipFilter::all;
    if (friends.tab != UiFriendsTab::add_friend)
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
    for (u32 i = friends.source.count / 2; i; --i) sift(friends, i - 1, friends.source.count);
    for (u32 end = friends.source.count; end > 1; --end) {
        const auto value = friends.indices[0];friends.indices[0] = friends.indices[end - 1];friends.indices[end - 1] = value;sift(friends, 0, end - 1);
    }
    friends.store_revision = friends.store.revision;friends.user_revision = g_app.account_ui->user_store.revision;
    friends.query_revision = query_revision;
    u32 incoming_count = 0;
    if (friends.tab == UiFriendsTab::pending)
        while (incoming_count < friends.source.count && friends.records[friends.indices[incoming_count]].type == RelationshipType::incoming) ++incoming_count;
    friends.section_rows[0] = {UiFriendsSectionKind::incoming, 0, incoming_count, incoming_total, friends.collapsed[0], true};
    friends.section_rows[1] = {UiFriendsSectionKind::outgoing, incoming_count, friends.source.count - incoming_count,
        outgoing_total, friends.collapsed[1]};
    friends.sections.hidden_available = hidden_count != 0;friends.sections.hidden_count = hidden_count;++friends.sections.revision;
    friends.requests_sections.hidden_count = hidden_count;++friends.requests_sections.revision;
    if (friends.requests_visible) {
        if (!friends.requests_indices.arena.base && !stable_storage_initialize(&friends.requests_indices,
            relationship_capacity, 0, sizeof(u32), alignof(u32))) {
            close_requests(friends);invalidate_sources(friends);
            recovery(friends, utf8_literal("Couldn't load other requests. Try again."));return;
        }
        if (hidden_count && !stable_storage_reserve(&friends.requests_indices, hidden_count)) {
            close_requests(friends);invalidate_sources(friends);
            recovery(friends, utf8_literal("Couldn't load other requests. Try again."));return;
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
            heap_sort(indices + first, count - first,
                [&friends](u32 a, u32 b) noexcept { return before(friends, a, b); });
        }
        friends.requests_source.count = count;
    }
    invalidate_sources(friends);
}
bool repaint() noexcept {
    return application_friends_synchronize() && application_request_frame();
}
bool search_available() noexcept {
    return application_friends_active() && !application_friends_requests_active() && !application_settings_active() && !g_app.text_form && !g_app.popup_scene.active &&
        !(g_app.profile && g_app.profile->scene.active) && !g_app.reaction_viewer.active && !g_app.image_viewer.scene.active;
}
void search_focus(ApplicationFriends& friends, bool focused) noexcept {
    auto* model = friends_search_document(&friends.search, friends.tab);
    focused = focused && model && search_available();
    if (friends.search_focused != focused) application_composer_text_input_suspend();
    for (auto& document : friends.search.models) document.focused = focused && &document == model;
    friends.search_focused = focused;friends.search_caret_visible = focused;
    friends.search_blink_at = focused && friends.search_blink_ms != INFINITE ?
        deadline_after(clock_monotonic_milliseconds(), friends.search_blink_ms) : 0;
    if (focused) {
        message_composer_enable_history(model, 2048);
        ui_friends_scene_focus(&friends.scene, {UiFriendsAction::search, 0, friends.tab});
        g_app.context_focus = &friends.scene.widgets;
    }
}
void close_requests(ApplicationFriends& friends) noexcept {
    if (!friends.requests_visible) return;
    application_composer_text_input_suspend();
    if (g_app.context_focus == &friends.requests_scene.widgets) g_app.context_focus = &friends.scene.widgets;
    if (GetCapture() == g_app.window && (friends.pressed.action != UiFriendsAction::none || friends.dragging)) ReleaseCapture();
    reset_value(&friends.pressed);friends.dragging = false;friends.requests_visible = false;
    renderer_bind_friends_requests_scene(&g_app.renderer, nullptr);ui_friends_scene_release(&friends.requests_scene);
    friends.requests_source.count = 0;++friends.requests_source.revision;
}
void project_content(ApplicationFriends& friends) noexcept {
    auto* model = friends_search_document(&friends.search, friends.tab);
    UiEditView edit{};
    if (model) {
        edit.text = message_composer_text(*model);edit.active = true;
        edit.anchor = model->anchor;edit.caret = model->caret;
        edit.focused = application_friends_search_focused();edit.caret_visible = edit.focused && friends.search_caret_visible;
    }
    const u32 revision = model ? model->revision : 0;
    const auto old = friends.content.search;
    if (revision != friends.edit_revision || old.text.data != edit.text.data || old.text.bytes != edit.text.bytes ||
        old.anchor != edit.anchor || old.caret != edit.caret || old.active != edit.active || old.focused != edit.focused ||
        old.caret_visible != edit.caret_visible || friends.content.total_count != friends.total_count) {
        friends.content.search = edit;friends.content.total_count = friends.total_count;friends.edit_revision = revision;
        ++friends.content.revision;
    }
}
bool search_pointer(ApplicationFriends& friends, float x, float y, bool extend, bool word = false) noexcept {
    auto* model = friends_search_document(&friends.search, friends.tab);
    u32 position = 0, offset = 0;
    if (!model || !text_edit_layout_hit_test(g_app.renderer.composer_layout, x, y, &position) ||
        !utf8_byte_offset_for_utf16_position(message_composer_text(*model), position, &offset)) return false;
    search_focus(friends, true);
    const auto result = word ? message_composer_select_word(model, offset) :
        message_composer_set_selection(model, extend ? model->anchor : offset, offset);
    return result != MessageComposerResult::invalid_input && application_friends_search_changed(false);
}
bool friend_activities(const void*, u64 user_id, UserActivitySet* out) noexcept {
    return g_app.account_ui && user_activity_find(g_app.account_ui->user_store.activities, user_id, out);
}
// Card text, cut on a UTF-8 boundary when the buffer fills.
struct ActiveText : TextBank {
    void add(Utf8View text) noexcept {
        used += utf8_copy_truncated(data + used, capacity - used, text);
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
    u64 online[1024];
    u32 count = 0;
    for (u32 i = 0; i < friends.store.count && count < sizeof(online) / sizeof(online[0]); ++i) {
        const auto& record = friends.records[i];
        if (record.type == RelationshipType::friend_ && (record.presence == RelationshipPresence::online ||
                                                          record.presence == RelationshipPresence::idle ||
                                                          record.presence == RelationshipPresence::dnd))
            online[count++] = record.user_id;
    }
    ActiveNowCard cards[kActiveNowMaximumCards];
    const u32 projected = active_now_project({{online, count}, &ui.voice_states, nullptr, friend_activities},
                                             {cards, kActiveNowMaximumCards});
    const bool same = active_now_equal({cards, projected}, {friends.active_projection, friends.active_projection_count});
    if (same && !users_changed && friends.active_now.collapsed == friends.active_collapsed && friends.active_now.revision)
        return;
    for (u32 i = 0; i < projected; ++i)
        friends.active_projection[i] = cards[i];
    friends.active_projection_count = projected;
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
            if (guild) {
                subtitle.add(utf8_literal(" \xC2\xB7 "));
                subtitle.add(chat_state_store_guild_name(ui.chat_state, *guild));
            } else if (card.name_bytes) {
                subtitle.add(utf8_literal(" \xC2\xB7 "));
                subtitle.add({card.name, card.name_bytes});
            }
            out.kind = UiFriendsActiveKind::voice;
        } else {
            char heading[160];
            const u32 bytes = user_activity_summary(card.activity_type, {card.name, card.name_bytes}, {},
                                                    {heading, sizeof(heading)});
            title.add(bytes ? Utf8View{heading, bytes} : Utf8View{card.name, card.name_bytes});
            member_names(subtitle, card);
            if (card.detail_bytes) {
                subtitle.add(utf8_literal(" \xC2\xB7 "));
                subtitle.add({card.detail, card.detail_bytes});
            }
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
bool more(ApplicationFriends& friends, UiFriendsTarget target, UiRect anchor = {}) noexcept {
    auto& scene = input_scene(friends);
    UiFriendsTarget checked{};
    if (!ui_friends_scene_activation(scene, {UiFriendsAction::more, target.user_id, friends.tab}, &checked)) return false;
    const auto* relationship = relationship_store_find(friends.store, target.user_id);
    if (!relationship) return false;
    PopupItem items[4 + kApplicationPluginMenuRows]{};u32 count = 0;
    items[count++] = {utf8_literal("Profile"), friends_popup_action_base + static_cast<u32>(UiFriendsAction::profile), popup_item_enabled};
    if (relationship->type == RelationshipType::friend_)
        items[count++] = {utf8_literal("Remove friend"), friends_popup_action_base + static_cast<u32>(UiFriendsAction::remove),
            static_cast<u16>(popup_item_enabled | popup_item_danger | popup_item_separator_before)};
    if (relationship->type != RelationshipType::blocked) {
        items[count++] = {utf8_literal("Block"), friends_popup_action_base + static_cast<u32>(UiFriendsAction::block),
            static_cast<u16>(popup_item_enabled | popup_item_danger)};
        items[count++] = {relationship->ignored ? utf8_literal("Unignore") : utf8_literal("Ignore"),
            friends_popup_action_base + static_cast<u32>(relationship->ignored ? UiFriendsAction::unignore : UiFriendsAction::ignore), popup_item_enabled};
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
    auto opened = popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, target.user_id, {items, count}, anchor,
        application_viewport_dips(), 200.f);
    if (!opened.ok() && count != builtins) {
        application_plugins_menu_dismissed();
        opened = popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, target.user_id, {items, builtins}, anchor,
            application_viewport_dips(), 200.f);
    }
    if (!opened.ok()) return false;
    set_popup_owner(PopupOwnerKind::friends);g_app.context_focus = &scene.widgets;
    if (!application_plugins_menu_bind()) return false;
    return renderer_rebuild_content_text(&g_app.renderer, application_theme()) && application_request_frame();
}
// Nonzero key of a relationship request a CAPTCHA belongs to.
u64 captcha_subject(RelationshipOperation operation, u64 id, Utf8View username) noexcept {
    u64 hash = 1469598103934665603ull ^ (static_cast<u64>(operation) << 56) ^ id;
    for (u32 i = 0; i < username.bytes; ++i)
        hash = (hash ^ static_cast<u8>(username.data[i])) * 1099511628211ull;
    return hash | 1;
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
        username.bytes > kRelationshipUsernameMaximumBytes || (!username.data && username.bytes)) return false;
    u8 target[96], body[256];
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
    friends->stranger_confirmation = false;
    friends->status = operation == RelationshipOperation::list ? utf8_literal("Loading friends…") : utf8_literal("Sending request…");
    project(*friends, true);repaint();
    if (g_app.window) PostMessageW(g_app.window, WM_NULL, 0, 0);
    return true;
}
bool text_valid(TextFormOwnerKey key) noexcept {
    return g_app.friends && g_app.account_ui && key.scope == g_app.account.status.profile_id;
}
bool text_save(TextFormOwnerKey key, Utf8View username) noexcept {
    return text_valid(key) && queue(RelationshipOperation::request, 0, username);
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
bool confirm(ApplicationFriends& friends, UiFriendsTarget target, bool allow_hidden = false) noexcept {
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
    if (!++friends.confirmation_serial) ++friends.confirmation_serial;
    ProtocolWriter writer{{reinterpret_cast<u8*>(friends.confirmation_text), sizeof(friends.confirmation_text)}, 0};
    if (bulk) {
        u32 count = 0;
        for (u32 i = 0; i < friends.store.count; ++i) if (friends.records[i].type == RelationshipType::incoming) ++count;
        if (!count || !protocol_writer_append_c_string(&writer, "Clear all ") || !protocol_writer_append_u64(&writer, count) ||
            !protocol_writer_append_c_string(&writer, " friend requests?")) return false;
    } else {
        Utf8View name{};
        if (current)
            name = relationship_display_name(*current, g_app.account_ui->user_store);
        else if (const auto* user = user_store_find(g_app.account_ui->user_store, target.user_id))
            name = user_store_name(g_app.account_ui->user_store, *user);
        if (!protocol_writer_append_c_string(&writer, target.action == UiFriendsAction::accept ?
            "Accept a friend request from " : target.action == UiFriendsAction::block ? "Block " :
            target.action == UiFriendsAction::ignore ? "Ignore " : "Remove ") ||
            !protocol_writer_append_bytes(&writer, reinterpret_cast<const u8*>(name.bytes ? name.data : "this user"), name.bytes ? name.bytes : 9) ||
            !protocol_writer_append_c_string(&writer, target.action == UiFriendsAction::remove ? " from your friends?" :
                target.action == UiFriendsAction::ignore ? "? You'll stay friends." : "?")) return false;
    }
    search_focus(friends, false);
    ApplicationTextFormRequest request{target.action == UiFriendsAction::accept ? utf8_literal("Accept friend request") :
        target.action == UiFriendsAction::block ? utf8_literal("Block user") : target.action == UiFriendsAction::ignore ? utf8_literal("Ignore user") :
        bulk ? utf8_literal("Clear requests") : utf8_literal("Remove friend"),
        {friends.confirmation_text, writer.used}, {}, target.action == UiFriendsAction::accept ? utf8_literal("Accept") :
        target.action == UiFriendsAction::block ? utf8_literal("Block") : target.action == UiFriendsAction::ignore ? utf8_literal("Ignore") :
        bulk ? utf8_literal("Clear all") : utf8_literal("Remove friend"), {},
        {g_app.account.status.profile_id, friends.confirmation_serial, target.user_id}, confirmation_valid, confirmation_save, 0, true};
    request.danger = target.action != UiFriendsAction::accept && target.action != UiFriendsAction::ignore;
    return application_text_form_open(request);
}
bool activate(UiFriendsTarget target) noexcept {
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
        search_focus(*friends, false);friends->tab = checked.tab;project(*friends, true);return repaint();
    }
    if (checked.action == UiFriendsAction::search) { search_focus(*friends, true);return repaint(); }
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
    if (checked.action == UiFriendsAction::add_friend)
        return application_text_form_open({utf8_literal("Add friend"), utf8_literal("Enter their username."),
            utf8_literal("Username"), utf8_literal("Send friend request"), {}, {g_app.account.status.profile_id, 0, 0},
            text_valid, text_save, kRelationshipUsernameMaximumBytes});
    if (checked.action == UiFriendsAction::retry && friends->stranger_confirmation) {
        const auto* current = relationship_store_find(friends->store, friends->request.user_id);
        if (!current || !request_current(*friends, friends->request)) {
            friends->request.phase = FriendsRequestPhase::idle;friends->stranger_confirmation = false;
            recovery(*friends, utf8_literal("This request changed. Refreshing…"));
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
TextInputTarget search_delegate(const TextInputTarget& target) noexcept {
    auto delegate = message_composer_input_target(static_cast<MessageComposer*>(target.context));
    delegate.identity = target.identity;return delegate;
}
bool search_ready(const TextInputTarget& target) noexcept {
    return application_friends_search_focused() && target.context == application_friends_search_model() &&
        text_input_ready(search_delegate(target));
}
u32 search_revision(const TextInputTarget& target) noexcept { return text_input_revision(search_delegate(target)); }
TextInputResult search_length(const TextInputTarget& target, u32* length) noexcept { return text_input_length(search_delegate(target), length); }
TextInputResult search_selection(const TextInputTarget& target, TextInputSelection* selection) noexcept { return text_input_selection(search_delegate(target), selection); }
TextInputResult search_select(const TextInputTarget& target, TextInputSelection selection) noexcept { return text_input_select(search_delegate(target), selection); }
TextInputResult search_copy(const TextInputTarget& target, u32 a, u32 b, Slice<u16> output, u32* length, u32* next) noexcept {
    return text_input_copy(search_delegate(target), a, b, output, length, next);
}
TextInputResult search_replace(const TextInputTarget& target, u32 a, u32 b, ConstSlice<u16> input, TextInputChange* change) noexcept {
    if (!change || a > b || (!input.data && input.count)) return TextInputResult::invalid_input;
    auto* model = static_cast<MessageComposer*>(target.context);
    u32 first = 0, last = 0;
    if (!utf8_byte_offset_for_utf16_position(message_composer_text(*model), a, &first) ||
        !utf8_byte_offset_for_utf16_position(message_composer_text(*model), b, &last)) return TextInputResult::invalid_utf16;
    char text[kFriendsSearchBytes];u32 bytes = 0;
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
    const auto result = message_composer_replace_range(model, first, last, {text, bytes});
    if (result == MessageComposerResult::changed || result == MessageComposerResult::unchanged) {
        *change = {a, b, a + input.count};return result == MessageComposerResult::changed ? TextInputResult::changed : TextInputResult::unchanged;
    }
    return result == MessageComposerResult::capacity ? TextInputResult::capacity : TextInputResult::invalid_input;
}
void search_cancel(const TextInputTarget& target) noexcept { text_input_cancel(search_delegate(target)); }
bool search_begin(const TextInputTarget& target) noexcept {
    auto* model = static_cast<MessageComposer*>(target.context);
    return (model->history || message_composer_enable_history(model, 2048)) && text_input_begin_group(search_delegate(target));
}
void search_end(const TextInputTarget& target) noexcept { text_input_end_group(search_delegate(target)); }
constexpr TextInputOperations search_operations{search_ready, search_revision, search_length, search_selection, search_select,
    search_copy, search_replace, search_cancel, search_begin, search_end};
bool search_keys(ApplicationFriends& friends, UiInputKind kind, u32 key) noexcept {
    auto* model = friends_search_document(&friends.search, friends.tab);
    const bool raw_control = (GetKeyState(VK_CONTROL) & 0x8000) != 0, alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    const bool control = raw_control && !alt, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (kind == UiInputKind::key_down && shortcut_matches(ShortcutAction::search, shortcut_chord_from_keyboard(key)) &&
        model) {
        search_focus(friends, true);message_composer_select_all(model);return application_friends_search_changed(false);
    }
    if (!model || !application_friends_search_focused()) return false;
    if (kind == UiInputKind::character) {
        if (control || (alt && !raw_control) || key < 32 || key == 127 || key == 0x2028 || key == 0x2029) return true;
        const auto result = message_composer_insert_utf16(model, static_cast<u16>(key), clock_monotonic_milliseconds());
        if (result == MessageComposerResult::changed) application_friends_search_changed(true);
        return true;
    }
    if (kind != UiInputKind::key_down || alt) return false;
    if (key == VK_SPACE && !control) return true;
    if (key == VK_UP || key == VK_DOWN || key == VK_PRIOR || key == VK_NEXT) return true;
    if (key == VK_TAB) { search_focus(friends, false);return false; }
    if (key == VK_RETURN || key == VK_ESCAPE) {
        if (key == VK_ESCAPE && model->bytes) message_composer_clear(model);
        else { search_focus(friends, false);ui_friends_scene_navigate(&friends.scene, UiListMove::first); }
        friends.held_key = key;application_friends_search_changed(true);return true;
    }
    const u32 text_revision = model->text_revision;
    MessageComposerResult result = MessageComposerResult::unchanged;
    if (key == VK_BACK) friends.held_key = key;
    if (control && (key == 'C' || key == 'X')) {
        const u32 first = minimum(model->anchor, model->caret), last = maximum(model->anchor, model->caret);
        if (last > first && clipboard_write_utf8(g_app.window, {model->text + first, last - first}) && key == 'X')
            result = message_composer_replace_range(model, first, last, {});
    } else if (control && key == 'V') {
        char text[kFriendsSearchBytes];const auto read = clipboard_read_utf8(g_app.window, {text, sizeof(text)});
        bool line = read.ok();
        for (u32 i = 0; line && i < read.bytes;) {
            const auto scalar = utf8_decode_one(text + i, read.bytes - i);
            line = scalar.valid && scalar.codepoint && scalar.codepoint != '\r' && scalar.codepoint != '\n' &&
                scalar.codepoint != 0x2028 && scalar.codepoint != 0x2029;
            if (line) i += scalar.width;
        }
        if (line) result = message_composer_insert_utf8(model, {text, read.bytes});
    } else if (!message_composer_edit_key(model, key, control, shift, &result)) return false;
    if (result == MessageComposerResult::changed) application_friends_search_changed(text_revision != model->text_revision);
    return true;
}
}
MessageComposer* application_friends_search_model() noexcept {
    return g_app.friends ? friends_search_document(&g_app.friends->search, g_app.friends->tab) : nullptr;
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
    for (auto& model : g_app.friends->search.models) {
        if (target.context != &model || target.identity != model.document_identity) continue;
        const auto cleanup = message_composer_input_target(&model);
        text_input_end_group(cleanup);text_input_cancel(cleanup);return;
    }
}
bool application_friends_search_changed(bool content) noexcept {
    auto* friends = g_app.friends;if (!friends) return false;
    friends->search_caret_visible = friends->search_focused;
    friends->search_blink_at = friends->search_focused && friends->search_blink_ms != INFINITE ?
        deadline_after(clock_monotonic_milliseconds(), friends->search_blink_ms) : 0;
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
    }
    return false;
}
bool application_friends_popup_action(u32 action) noexcept {
    auto* friends = g_app.friends;
    if (!friends || g_app.popup_owner_kind != PopupOwnerKind::friends || !g_app.popup_scene.active ||
        action < friends_popup_action_base || action > friends_popup_action_base + static_cast<u32>(UiFriendsAction::unignore)) return false;
    const UiFriendsTarget target{static_cast<UiFriendsAction>(action - friends_popup_action_base), g_app.popup_scene.owner_id, friends->tab};
    UiFriendsTarget checked{};
    if ((target.action != UiFriendsAction::profile && target.action != UiFriendsAction::remove && target.action != UiFriendsAction::block &&
        target.action != UiFriendsAction::ignore && target.action != UiFriendsAction::unignore) ||
        !ui_friends_scene_activation(input_scene(*friends), target, &checked)) { dismiss_popup();return true; }
    dismiss_popup();return activate(checked);
}
void application_friends_tick(u64 now) noexcept {
    auto* friends = g_app.friends;if (!friends || !friends->search_blink_at || !deadline_due(now, friends->search_blink_at)) return;
    if (!application_friends_search_focused() || (g_app.window && GetFocus() != g_app.window)) {
        friends->search_blink_at = 0;friends->search_caret_visible = false;
    } else {
        friends->search_caret_visible = !friends->search_caret_visible;
        friends->search_blink_at = deadline_after(now, friends->search_blink_ms);
    }
    repaint();
}
u64 application_friends_search_next_wake() noexcept {
    return g_app.friends && application_friends_search_focused() ? g_app.friends->search_blink_at : 0;
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
bool application_friends_show() noexcept {
    if (!g_app.account_ui || !g_app.renderer_ready || g_app.login.visible || g_app.text_form || application_settings_active()) return false;
    auto* friends = ensure();if (!friends) return false;
    const auto selected = chat_state_store_select_home(&g_app.account_ui->chat_state);
    if (!selected.ok()) return false;
    friends->visible = true;
    application_composer_text_input_suspend();dismiss_popup();application_profile_close();application_image_viewer_close();
    g_app.message_composer.focused = false;g_app.context_focus = nullptr;ui_scroll_host_cancel(&g_app.scroll_host);
    if (!synchronize_navigation_view() || !synchronize_selected_channel() || !synchronize_shell_layout() || !repaint()) {
        application_friends_hide();return false;
    }
    g_app.context_focus = &friends->scene.widgets;
    if (!friends->ready && !friends->snapshot_active && !request_busy(*friends)) queue(RelationshipOperation::list);
    return true;
}
void application_friends_hide() noexcept {
    auto* friends = g_app.friends;if (!friends) return;
    application_text_form_cancel_owner(confirmation_valid);
    if (g_app.popup_owner_kind == PopupOwnerKind::friends) dismiss_popup();
    close_requests(*friends);
    search_focus(*friends, false);
    if (g_app.context_focus == &friends->scene.widgets) g_app.context_focus = nullptr;
    if (GetCapture() == g_app.window && (friends->pressed.action != UiFriendsAction::none || friends->dragging || friends->search_dragging)) ReleaseCapture();
    friends->visible = false;reset_value(&friends->pressed);friends->dragging = false;friends->search_dragging = false;
    renderer_bind_friends_scene(&g_app.renderer, nullptr);ui_friends_scene_release(&friends->scene);
}
void application_friends_release() noexcept {
    application_text_form_cancel_owner(text_valid);
    application_text_form_cancel_owner(confirmation_valid);
    application_friends_hide();
    auto* friends = g_app.friends;g_app.friends = nullptr;
    if (friends) {
        friends_search_documents_release(&friends->search);
        stable_storage_release(&friends->requests_indices);
        friends_relationship_storage_release(&friends->storage);VirtualFree(friends, 0, MEM_RELEASE);
    }
}
bool application_friends_synchronize() noexcept {
    auto* friends = g_app.friends;if (!friends) return true;
    if (!application_friends_active()) { if (friends->scene.active) application_friends_hide();return true; }
    if (application_settings_active()) { close_requests(*friends);return true; }
    project(*friends);
    if (friends->requests_visible && !friends->requests_sections.hidden_count) close_requests(*friends);
    project_content(*friends);
    const auto phase = friends->request.phase == FriendsRequestPhase::failed ? UiFriendsPhase::error :
        !friends->ready || (request_busy(*friends) && friends->request.operation == RelationshipOperation::list) ? UiFriendsPhase::loading :
        friends->incomplete ? UiFriendsPhase::error : UiFriendsPhase::ready;
    ui_friends_scene_set_focus_visible(&friends->scene, g_app.keyboard_focus_visible);
    project_active(*friends);
    if (!ui_friends_scene_update(&friends->scene, &friends->source, friends->tab, phase, friends->status,
        ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::message_view), &friends->content,
        friends->tab == UiFriendsTab::pending ? &friends->sections : nullptr, &friends->active_now)) return false;
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
    if (!id || !application_friends_pending(now) || !g_app.account.instance || !g_app.account.api.relationships) return false;
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
    const auto result = g_app.account.api.relationships(g_app.account.instance, request.operation, request.user_id,
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
            recovery(*friends, utf8_literal("This request changed. Try again."));
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
    bool ok = response.transport_ok() && response.status_code >= 200 && response.status_code < 300;
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
            if (friends->incomplete) recovery(*friends, status);else friends->status = status;
            return repaint();
        }
        const auto decoded = relationships_replace(&friends->store, body,
            {g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount},
            {friends->candidate, relationship_capacity}, {}, {&friends->storage,
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
        }
    } else if (ok && request.user_id && request.membership_epoch == friends->membership_epoch) {
        if (request.operation == RelationshipOperation::remove) ok = relationship_store_remove(&friends->store, request.user_id).ok();
        else {
            const auto* old = relationship_store_find(friends->store, request.user_id);
            RelationshipRecord record = old ? *old : RelationshipRecord{};
            record.user_id = request.user_id;
            if (request.operation == RelationshipOperation::ignore || request.operation == RelationshipOperation::unignore)
                record.ignored = request.operation == RelationshipOperation::ignore;
            else record.type = request.operation == RelationshipOperation::block ? RelationshipType::blocked :
                request.unknown_target ? RelationshipType::outgoing : RelationshipType::friend_;
            ok = friends_relationship_storage_upsert(&friends->storage, friends->store, record.user_id) &&
                relationship_store_upsert(&friends->store, record).ok();
        }
        if (ok) ++friends->membership_epoch;
    }
    request.phase = ok ? FriendsRequestPhase::idle : FriendsRequestPhase::failed;
    const bool captcha = !ok && application_action_captcha_capture(ApplicationActionCaptchaKind::friend_request,
        captcha_subject(request), response, body);
    friends->status = ok ? Utf8View{} :
        captcha ? utf8_literal("Discord needs a CAPTCHA first. Send again to verify in your browser.") :
        api_rejection_requires_action(response.rejection) ? utf8_literal("Verify your account to continue.") :
        utf8_literal("Something went wrong. Try again.");
    if (ok && friends->incomplete)
        recovery(*friends, utf8_literal("Your friends list is out of date. Try again to refresh."));
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
        recovery(*friends, utf8_literal("Couldn't update your friends list. Try again."));
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
            if (!request_busy(*friends)) { reset_value(&friends->request);reset_value(&friends->status); }
        } else {
            recovery(*friends, utf8_literal("Couldn't load your friends. Try again."));
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
        if (applied.changed) { ++friends->membership_epoch;invalidate_sources(*friends); }
        if (friends->snapshot_active) friends->snapshot_failed = true;
        if (applied.changed) application_request_frame();
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
        recovery(*friends, utf8_literal("Couldn't update your friends list. Try again."));
        invalidate_sources(*friends);
        application_request_frame();
    }
}
bool application_friends_input(UiInputKind kind, u32 key, bool repeated) noexcept {
    auto* friends = g_app.friends;
    if (friends && ui_input_consume_key_tail(&friends->held_key, {kind, key, repeated})) return true;
    if (!application_friends_active() || application_settings_active() || g_app.text_form || g_app.popup_scene.active ||
        (g_app.profile && g_app.profile->scene.active) || g_app.reaction_viewer.active || g_app.image_viewer.scene.active) return false;
    auto& scene = input_scene(*friends);
    const bool modal = friends->requests_visible;
    if (!modal && search_keys(*friends, kind, key)) return true;
    if (modal && kind == UiInputKind::key_down && key == VK_ESCAPE) {
        friends->held_key = key;close_requests(*friends);return repaint();
    }
    if (!modal && g_app.context_focus && g_app.context_focus != &scene.widgets) return false;
    if (kind == UiInputKind::character) return true;
    if (kind != UiInputKind::key_down || (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000)) return modal;
    if (key == VK_TAB) {
        const auto next = ui_widget_manager_next_focus(scene.widgets, scene.widgets.focused,
            (GetKeyState(VK_SHIFT) & 0x8000) != 0, true);
        if (next.generation) {
            const auto* node = ui_widget_manager_layout_node(scene.widgets, next);
            if (node) ui_friends_scene_focus(&scene, ui_friends_scene_hit_test(scene,
                (node->rectangle.left + node->rectangle.right) * .5f, (node->rectangle.top + node->rectangle.bottom) * .5f));
            search_focus(*friends, ui_friends_scene_focused(scene).action == UiFriendsAction::search);
        }
    } else if (key == VK_UP || key == VK_DOWN || key == VK_HOME || key == VK_END)
        ui_friends_scene_navigate(&scene, key == VK_UP ? UiListMove::previous : key == VK_DOWN ? UiListMove::next : key == VK_HOME ? UiListMove::first : UiListMove::last);
    else if (key == VK_PRIOR || key == VK_NEXT) ui_friends_scene_page(&scene, key == VK_PRIOR);
    else if ((key == VK_RETURN || key == VK_SPACE) && !repeated) { friends->held_key = key;activate(ui_friends_scene_focused(scene)); }
    else return modal;
    if (!g_app.text_form && !g_app.popup_scene.active && !(g_app.profile && g_app.profile->scene.active))
        g_app.context_focus = &input_scene(*friends).widgets;
    return repaint();
}
bool application_friends_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    auto* friends = g_app.friends;
    if (!application_friends_active()) return false;
    if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE || message == WM_KILLFOCUS) {
        reset_value(&friends->pressed);friends->dragging = false;friends->search_dragging = false;
        if (message == WM_KILLFOCUS) { friends->search_caret_visible = false;friends->search_blink_at = 0;application_composer_text_input_suspend();repaint(); }
        return false;
    }
    if (message == WM_SETFOCUS && friends->search_focused) { application_friends_search_changed(false);return false; }
    if (application_settings_active() || g_app.text_form || g_app.popup_scene.active ||
        (g_app.profile && g_app.profile->scene.active) || g_app.reaction_viewer.active || g_app.image_viewer.scene.active) return false;
    auto& scene = input_scene(*friends);
    const bool modal = friends->requests_visible;
    float x = client_dip_x(lparam), y = client_dip_y(lparam);
    if (message == WM_CONTEXTMENU && lparam == -1) {
        if (!modal && g_app.context_focus != &scene.widgets) return false;
        const auto focused = ui_friends_scene_focused(scene);
        if (focused.user_id) more(*friends, focused);
        return true;
    }
    if (message == WM_MOUSEWHEEL || message == WM_CONTEXTMENU) {
        POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        if (!application_screen_to_content(&point)) return false;
        const float scale = 96.f / maximum(g_app.renderer.dpi, 1u);x = point.x * scale;y = point.y * scale;
        if (!ui_rect_contains(scene.viewport, x, y)) return modal;
        if (message == WM_MOUSEWHEEL) {
            ui_friends_scene_scroll(&scene, -static_cast<short>(HIWORD(wparam)) * .8f);repaint();
        } else {
            const auto hit = ui_friends_scene_hit_test(scene, x, y);
            if (hit.action == UiFriendsAction::active_card)
                application_user_menu_open(hit.user_id, {x, y, x + 1, y + 1}, 0, UserMenuSource::profile);
            else if (hit.user_id) more(*friends, hit, {x, y, x + 1, y + 1});
        }
        return true;
    }
    if (message == WM_MOUSEMOVE && friends->dragging) {
        const auto geometry = ui_friends_scene_scrollbar(scene);
        const float target = ui_scrollbar_scroll_from_pointer(geometry, y, friends->thumb_grab);
        ui_friends_scene_scroll(&scene, target - scene.scroll_dips);repaint();return true;
    }
    if (message == WM_MOUSEMOVE && friends->search_dragging) {
        search_pointer(*friends, x, y, true);
        return true;
    }
    const bool inside = ui_rect_contains(scene.viewport, x, y);
    if (message == WM_LBUTTONDBLCLK &&
        (!inside || ui_friends_scene_hit_test(scene, x, y).action != UiFriendsAction::search))
        message = WM_LBUTTONDOWN;
    if (message == WM_MOUSEMOVE && ui_friends_scene_active_hover(&scene, inside ? x : -1.f, inside ? y : -1.f))
        application_request_frame();
    if (message == WM_MOUSEMOVE) {
        const auto hit = inside && ui_friends_scene_hit_test(scene, x, y).action != UiFriendsAction::none ?
            ui_widget_manager_hit_test(scene.widgets, x, y) : ui_widget_invalid_handle();
        const auto old = scene.widgets.hovered;
        if (old.slot != hit.slot || old.generation != hit.generation) {
            ui_widget_manager_hover(&scene.widgets, hit);application_request_frame();
        }
    }
    if (!inside && friends->pressed.action == UiFriendsAction::none && !friends->dragging && !friends->search_dragging)
        return modal && (message == WM_MOUSEMOVE || message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK ||
            message == WM_RBUTTONDOWN || message == WM_RBUTTONUP || message == WM_MBUTTONDOWN || message == WM_MBUTTONUP);
    if (message == WM_LBUTTONDOWN) {
        const auto geometry = ui_friends_scene_scrollbar(scene);
        if (ui_scrollbar_track_hit(geometry, x, y)) {
            if (ui_scrollbar_thumb_hit(geometry, x, y)) { friends->dragging = true;friends->thumb_grab = y - geometry.thumb.top;SetCapture(g_app.window); }
            else { const float page = maximum(1.f, geometry.track.bottom - geometry.track.top - 56.f);
                ui_friends_scene_scroll(&scene, y < geometry.thumb.top ? -page : page);repaint(); }
            return true;
        }
        friends->pressed = ui_friends_scene_hit_test(scene, x, y);
        if (friends->pressed.action == UiFriendsAction::search) {
            search_focus(*friends, true);repaint();
            search_pointer(*friends, x, y, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            reset_value(&friends->pressed);friends->search_dragging = true;SetCapture(g_app.window);return true;
        }
        search_focus(*friends, false);
        if (friends->pressed.action != UiFriendsAction::none) {
            ui_friends_scene_focus(&scene, friends->pressed);g_app.context_focus = &scene.widgets;
            repaint();friends->pressed_revision = scene.revision;SetCapture(g_app.window);
        }
        return true;
    }
    if (message == WM_LBUTTONUP) {
        const auto pressed = friends->pressed;const auto revision = friends->pressed_revision;
        reset_value(&friends->pressed);friends->dragging = false;friends->search_dragging = false;if (GetCapture() == g_app.window) ReleaseCapture();
        if (revision == scene.revision && ui_friends_target_equal(pressed, ui_friends_scene_hit_test(scene, x, y))) activate(pressed);
        return true;
    }
    if (message == WM_LBUTTONDBLCLK && inside && ui_friends_scene_hit_test(scene, x, y).action == UiFriendsAction::search)
        return search_pointer(*friends, x, y, false, true);
    return inside && (message == WM_MOUSEMOVE || message == WM_RBUTTONDOWN || message == WM_RBUTTONUP || message == WM_CONTEXTMENU ||
        (modal && (message == WM_LBUTTONDBLCLK || message == WM_MBUTTONDOWN || message == WM_MBUTTONUP)));
}
} // namespace maicord::app_detail
