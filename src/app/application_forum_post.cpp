#include "application_forum_post.hpp"
#include "application_state.hpp"
#include "application_content_input.hpp"
#include "application_account_presentation.hpp"
#include "application_thread_browser.hpp"
#include "application_thread_creation.hpp"
#include "application_settings.hpp"
#include "application_friends.hpp"
#include "application_forum_attachments.hpp"
#include "maicord/app/message_attachment_storage.hpp"
#include "maicord/protocol/forum_post.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "application_account_access.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace {
using ForumPhase = ApplicationForumPostPhase;
using ForumOperation = ForumPostOperation;
struct ForumMessageMutation { u64 channel, message; };
struct ForumSource {
    u64 scope, profile, self, guild, parent, origin_active, origin_selected;
    const void* instance;
    const AccountUiRuntime* ui;
    u16 parent_generation;
    u8 parent_type;
};
// Activity and read-state updates change ChannelRecord::revision while a
// metadata GET or POST is in flight. Only retained settings and permissions
// participate here; explicit parent/guild observations also advance the epoch.
struct ForumParentAuthority {
    u64 permissions;
    u32 flags, actions;
};
struct ForumFlight {
    ForumSource source;
    ForumParentAuthority parent_authority;
    ForumPostRequest request;
    u64 id, serial, metadata_epoch;
    u64 changed_threads[8];
    ForumMessageMutation changed_messages[8];
    u32 draft_revision, publication;
    u64 attachment_id;
    u8* attachment_bank;
    u32 attachment_bank_bytes;
    UploadedAttachment attachment_views[kAttachmentUploadMaximumCount];
    char upload_filename[kAttachmentUploadMaximumFilenameBytes];
    char title[kForumPostTitleMaximumBytes];
    char body[kForumPostContentMaximumBytes];
    bool active, retired, cancel_requested, changed_all, changed_messages_all;
};
struct ForumOwner {
    ForumSource source;
    ForumFlight flight;
    ForumParentAuthority metadata_parent_authority;
    ForumParentSnapshot metadata;
    ForumPostSnapshot candidate;
    ApplicationForumPostTag tag_views[kForumPostAvailableTagMaximum];
    UploadedAttachment prepared_attachments[kAttachmentUploadMaximumCount];
    u64 prepared_sizes[kAttachmentUploadMaximumCount];
    char title[kForumPostTitleMaximumBytes];
    char body[kForumPostContentMaximumBytes];
    char parent_name[kForumPostTitleMaximumBytes];
    u64 selected[kForumPostAppliedTagMaximum], uncertain_parents[8];
    u64 serial, metadata_epoch, metadata_checked_epoch, actor_floor, due, known_created;
    Utf8View status;
    u32 publication, revision, metadata_revision, draft_revision;
    u32 title_bytes, body_bytes, parent_name_bytes, busy_count, pins;
    u8 selected_count;
    ForumPhase phase;
    bool metadata_ready, retired, error, uncertain, uncertain_all, terminal, release_pending, navigating, starter_stale, known_media;
};
static_assert(sizeof(ForumOwner) <= 32768);
ForumOwner* forum_owner{};
u64 forum_serial{};
u32 forum_publication{};
ForumParentAuthority forum_parent_authority(const ChannelRecord& parent) noexcept {
    return {parent.effective_permissions, parent.flags, parent.actions};
}
bool forum_parent_authority_current(u64 id, const ForumParentAuthority& authority) noexcept {
    const auto* parent = application_channel_record(id);
    return parent && authority.permissions == parent->effective_permissions &&
        authority.flags == parent->flags && authority.actions == parent->actions;
}
bool forum_actor_current(const ForumSource& source) noexcept {
    return g_app.account_ui && !g_app.login.visible && source.ui == g_app.account_ui &&
           source.instance == g_app.account.instance && source.scope == g_app.plugin_account_scope_serial &&
           source.profile == g_app.account.status.profile_id &&
           source.self == g_app.account_ui->chat_state.self_user_id;
}
bool forum_parent_authorized(u64 id) noexcept {
    const auto* parent = application_channel_record(id);
    if (!parent || !parent->occupied || (parent->type != 15 && parent->type != 16) || !parent->guild_id ||
        !parent->permissions_resolved || (parent->flags & kChannelObfuscatedFlag))
        return false;
    constexpr u32 required = channel_action_view | channel_action_read_history |
                             channel_action_send_messages;
    const auto& state = g_app.account_ui->chat_state;
    const auto* guild = chat_state_store_find_guild(state, parent->guild_id);
    return guild && guild->occupied && !guild->unavailable && (parent->actions & required) == required &&
           chat_state_store_channel_visible(state, *parent);
}
bool forum_source_current(const ForumSource& source) noexcept {
    const auto* parent = forum_actor_current(source) ? application_channel_record(source.parent) : nullptr;
    return parent && forum_parent_authorized(source.parent) &&
           parent->generation == source.parent_generation && parent->type == source.parent_type && parent->guild_id == source.guild &&
           g_app.account_ui->active_channel_id == source.origin_active &&
           g_app.account_ui->chat_state.selected_channel_id == source.origin_selected &&
           g_app.account_ui->chat_state.selected_guild_id == source.guild;
}
bool forum_owner_current(const ForumOwner& owner) noexcept {
    if (forum_source_current(owner.source))
        return true;
    const auto* parent = forum_actor_current(owner.source) ? application_channel_record(owner.source.parent) : nullptr;
    return owner.navigating && owner.known_created && parent && forum_parent_authorized(parent->id) &&
           parent->generation == owner.source.parent_generation && parent->type == owner.source.parent_type && parent->guild_id == owner.source.guild &&
           g_app.account_ui->chat_state.selected_guild_id == owner.source.guild &&
           g_app.account_ui->active_channel_id == owner.known_created &&
           g_app.account_ui->chat_state.selected_channel_id == owner.known_created;
}
ApplicationForumPostTicket forum_ticket(const ForumOwner& owner) noexcept {
    return {owner.serial, owner.source.parent, owner.publication};
}
bool forum_ticket_equal(const ForumOwner& owner, const ApplicationForumPostTicket& ticket) noexcept {
    return ticket.serial && ticket.serial == owner.serial && ticket.parent_id == owner.source.parent &&
           ticket.publication_revision == owner.publication;
}
void forum_publish(ForumOwner& owner, ForumPhase phase, Utf8View status = {}, bool error = false) noexcept {
    if (owner.phase == phase && owner.error == error && utf8_equal(owner.status, status))
        return;
    owner.phase = phase;
    owner.status = status;
    owner.error = error;
    ++owner.revision;
    application_request_frame();
}
void forum_draft_changed(ForumOwner& owner) noexcept {
    if (owner.phase == ForumPhase::editing && owner.error) {
        owner.error = false;
        reset_value(&owner.status);
    }
    ++owner.draft_revision;
    ++owner.revision;
    application_request_frame();
}
bool forum_ledger_contains(const ForumOwner& owner, u64 parent) noexcept {
    if (owner.uncertain_all)
        return true;
    for (u64 value : owner.uncertain_parents)
        if (value == parent)
            return true;
    return false;
}
void forum_ledger_add(ForumOwner& owner, u64 parent) noexcept {
    if (forum_ledger_contains(owner, parent))
        return;
    for (auto& value : owner.uncertain_parents)
        if (!value) {
            value = parent;
            return;
        }
    owner.uncertain_all = true;
}
void forum_ledger_remove(ForumOwner& owner, u64 parent) noexcept {
    for (auto& value : owner.uncertain_parents)
        if (value == parent)
            value = 0;
}
void forum_fail(ForumOwner& owner, Utf8View status, bool uncertain = false,
                bool terminal = false) noexcept {
    owner.uncertain = owner.uncertain || uncertain;
    owner.terminal = terminal;
    if (uncertain)
        forum_ledger_add(owner, owner.source.parent);
    application_forum_attachments_lock(forum_ticket(owner), false);
    forum_publish(owner, ForumPhase::failed, status, true);
}
void forum_cancel_flight(ForumOwner& owner) noexcept {
    auto& flight = owner.flight;
    if (!flight.active)
        return;
    flight.retired = true;
    if (flight.request.operation == ForumOperation::create)
        forum_ledger_add(owner, flight.source.parent);
    if (!flight.cancel_requested && flight.source.instance == g_app.account.instance &&
        g_app.account.instance && g_app.account.api.forum_post) {
        g_app.account.api.forum_post(g_app.account.instance, nullptr, flight.id);
        flight.cancel_requested = true;
    }
}
void forum_retire(ForumOwner& owner) noexcept {
    if (owner.retired)
        return;
    owner.retired = true;
    forum_cancel_flight(owner);
    forum_publish(owner, ForumPhase::cancelled);
    application_forum_form_close(forum_ticket(owner));
    application_forum_attachments_retire(forum_ticket(owner));
}
void forum_free() noexcept {
    if (!forum_owner)
        return;
    if (forum_owner->flight.attachment_bank) {
        secure_clear_bytes(forum_owner->flight.attachment_bank, forum_owner->flight.attachment_bank_bytes);
        VirtualFree(forum_owner->flight.attachment_bank, 0, MEM_RELEASE);
    }
    secure_clear_bytes(forum_owner, sizeof(*forum_owner));
    VirtualFree(forum_owner, 0, MEM_RELEASE);
    forum_owner = nullptr;
}
struct ForumPin {
    ForumOwner& owner;
    explicit ForumPin(ForumOwner& value) noexcept : owner(value) {
        ++owner.pins;
    }
    ~ForumPin() noexcept {
        --owner.pins;
        if (!owner.pins && owner.release_pending &&
            (!owner.flight.active || !g_app.account.instance ||
             (!g_app.account.status.running && !g_app.account.stop_failed)))
            forum_free();
    }
};
struct ForumFlightBank {
    u8* data;
    u32 bytes;
    ~ForumFlightBank() noexcept { if (data) { secure_clear_bytes(data, bytes); VirtualFree(data, 0, MEM_RELEASE); } }
};
bool forum_metadata_fresh(const ForumOwner& owner) noexcept {
    return owner.metadata_ready && forum_parent_authority_current(owner.source.parent, owner.metadata_parent_authority) &&
           owner.metadata_checked_epoch == owner.metadata_epoch;
}
bool forum_manage_threads(const ForumOwner& owner) noexcept {
    const auto* parent = application_channel_record(owner.source.parent);
    return parent && (parent->actions & channel_action_manage_threads);
}
void forum_tags(ForumOwner& owner) noexcept {
    const bool manage = forum_manage_threads(owner);
    for (u32 i = 0; i < owner.metadata.tag_count; ++i) {
        const auto& tag = owner.metadata.tags[i];
        bool selected{};
        for (u32 j = 0; j < owner.selected_count; ++j)
            selected = selected || owner.selected[j] == tag.id;
        owner.tag_views[i] = {tag.id, forum_tag_name_view(tag), selected,
                              forum_metadata_fresh(owner) && (!tag.moderated || manage),
                              tag.moderated};
    }
}
void forum_queue_metadata(ForumOwner& owner) noexcept {
    application_forum_attachments_lock(forum_ticket(owner), false);
    owner.metadata_ready = false;
    owner.due = owner.actor_floor;
    owner.busy_count = 0;
    owner.terminal = false;
    forum_publish(owner, ForumPhase::metadata_queued, utf8_literal("Loading forum tags…"));
}
bool forum_queued(const ForumOwner& owner) noexcept {
    return owner.phase == ForumPhase::metadata_queued || owner.phase == ForumPhase::queued;
}
ForumPostRequest forum_request(ForumOwner& owner, ForumOperation operation) noexcept {
    ForumPostRequest request{};
    request.parent_id = owner.source.parent;
    request.guild_id = owner.source.guild;
    request.operation = operation;
    if (operation == ForumOperation::create) {
        request.title = {owner.title, owner.title_bytes};
        request.content = {owner.body, owner.body_bytes};
        request.auto_archive_duration = owner.metadata.default_auto_archive_duration;
        request.tag_count = owner.selected_count;
        for (u32 i = 0; i < request.tag_count; ++i)
            request.applied_tag_ids[i] = owner.selected[i];
        u32 count{};
        if (application_forum_attachments_prepare(forum_ticket(owner),
            {owner.prepared_attachments, kAttachmentUploadMaximumCount}, {owner.prepared_sizes, kAttachmentUploadMaximumCount}, &count))
            request.attachments = count ? ConstSlice<UploadedAttachment>{owner.prepared_attachments, count} : ConstSlice<UploadedAttachment>{};
        else request.attachments = {owner.prepared_attachments, kAttachmentUploadMaximumCount + 1};
    }
    return request;
}
bool forum_draft_text(Utf8View value, u32 bytes, u32 scalars, bool single_line) noexcept {
    if (value.bytes > bytes || (value.bytes && !value.data))
        return false;
    const auto validation = utf8_validate(value);
    if (!validation.valid || validation.codepoints > scalars)
        return false;
    for (u32 at = 0; at < value.bytes;) {
        const auto scalar = utf8_decode_one(value.data + at, value.bytes - at);
        if (!scalar.codepoint ||
            (single_line && (scalar.codepoint < 32 || scalar.codepoint == 0x7f ||
                             scalar.codepoint == 0x2028 || scalar.codepoint == 0x2029)))
            return false;
        at += scalar.width;
    }
    return true;
}
bool forum_known_created(const ForumOwner& owner) noexcept {
    const auto* row = application_channel_record(owner.known_created);
    return row && row->occupied && row->type == 11 && row->parent_id == owner.source.parent &&
           row->guild_id == owner.source.guild && row->thread_archived_known && row->thread_locked_known &&
           chat_state_store_channel_name(g_app.account_ui->chat_state, *row).bytes &&
           !(row->flags & kChannelObfuscatedFlag) &&
           chat_state_store_thread_accessible(g_app.account_ui->chat_state, *row);
}
bool forum_publish_starter(ForumOwner& owner) noexcept {
    if (!forum_actor_current(owner.source) ||
        g_app.account_ui->active_channel_id != owner.known_created ||
        g_app.account_ui->chat_state.selected_channel_id != owner.known_created ||
        g_app.message_view.timeline.store != &g_app.message_view.store ||
        g_app.message_composer.channel_id != owner.known_created)
        return false;
    const auto& receipt = owner.candidate;
    if (owner.starter_stale)
        return true; // Normal navigation/history owns the fresher observation.
    const auto found = message_store_find(g_app.message_view.store, receipt.message_id);
    if (found.ok()) {
        const auto* message = message_store_resolve(g_app.message_view.store, found.handle);
        return message && message->occupied && message->delivery == MessageDeliveryState::authoritative &&
               message->channel_id == receipt.message_channel_id &&
               message->author_id == receipt.message_author_id;
    }
    GatewayMessageEvent message{};
    message.id = receipt.message_id;
    message.channel_id = receipt.message_channel_id;
    message.author_id = receipt.message_author_id;
    message.timestamp_ms = receipt.message_timestamp_ms;
    message.content = forum_post_content_view(receipt);
    message.flags = receipt.message_flags;
    message.fields = gateway_message_field_id | gateway_message_field_channel_id |
                     gateway_message_field_author_id | gateway_message_field_timestamp |
                     gateway_message_field_content | gateway_message_field_flags |
                       gateway_message_field_type;
    if (owner.known_media) {
        message.attachments = application_forum_attachments_receipt();
        if (!message.attachments.count) return false;
        message.fields |= gateway_message_field_attachments;
    }
    return message_timeline_apply_created(&g_app.message_view.timeline, message).ok();
}
bool forum_navigate(ForumOwner& owner) noexcept {
    if (!forum_source_current(owner.source) || !forum_known_created(owner))
        return false;
    application_forum_form_close(forum_ticket(owner));
    owner.navigating = true;
    const bool navigated =
        application_navigate_known_message(owner.known_created, owner.candidate.message_id);
    const bool current = !owner.retired && forum_owner_current(owner);
    owner.navigating = false;
    if (!current) {
        forum_retire(owner);
        return false;
    }
    const bool selected = g_app.account_ui->active_channel_id == owner.known_created &&
                          g_app.account_ui->chat_state.selected_channel_id == owner.known_created;
    if (selected)
        owner.source.origin_active = owner.source.origin_selected = owner.known_created;
    const bool presented =
        navigated && selected && forum_publish_starter(owner) && refresh_account_text(true);
    if (owner.retired || !forum_source_current(owner.source)) {
        forum_retire(owner);
        return false;
    }
    if (!presented) {
        forum_fail(owner, utf8_literal("Post created, but couldn't be opened. Try again or view posts."));
        application_forum_form_open(forum_ticket(owner));
        return false;
    }
    owner.uncertain = owner.terminal = false;
    forum_ledger_remove(owner, owner.source.parent);
    forum_publish(owner, ForumPhase::settled, utf8_literal("Post created."));
    application_forum_form_close(forum_ticket(owner));
    application_forum_attachments_retire(forum_ticket(owner));
    return true;
}
bool forum_publish_thread(ForumOwner& owner, const ForumFlight& flight) noexcept {
    const auto& receipt = owner.candidate.thread;
    auto& state = g_app.account_ui->chat_state;
    const auto* old = application_channel_record(receipt.id);
    bool changed = flight.changed_all;
    for (u64 id : flight.changed_threads)
        changed = changed || id == receipt.id;
    if (changed && !old)
        return false;
    if (old) {
        // An already retained Gateway publication wins; do not overwrite its
        // metadata with an HTTP observation whose relative order is unknown.
        if (!old->occupied || old->parent_id != receipt.parent_id || old->guild_id != receipt.guild_id ||
            old->type != receipt.type || !old->thread_archived_known || !old->thread_locked_known)
            return false;
        owner.known_created = receipt.id;
    } else {
        ChannelInput input{};
        if (!application_thread_creation_receipt_input(receipt, application_channel_record(receipt.id), &input) ||
            chat_state_store_preflight_channel(state, input) != ChatStateStoreError::none ||
            !chat_state_store_upsert_channel(&state, input).ok())
            return false;
        chat_state_store_recompute_guild_permissions(&state, owner.source.guild);
    }
    const auto* parent = application_channel_record(receipt.parent_id);
    const u64 previous_parent_cursor = parent ? parent->last_read_message_id : 0;
    if (chat_state_store_note_thread(&state, receipt.parent_id, receipt.id, state.self_user_id) &&
        parent && parent->last_read_message_id > previous_parent_cursor &&
        g_app.message_pump.read_ack_cancel)
        g_app.message_pump.read_ack_cancel(g_app.message_pump.read_ack_cancel_context, receipt.parent_id);
    owner.known_created = receipt.id;
    return forum_known_created(owner);
}
bool forum_phase_editable(const ForumOwner& owner) noexcept {
    return !owner.known_created && owner.phase != ForumPhase::queued && owner.phase != ForumPhase::creating &&
           owner.phase != ForumPhase::settled;
}
} // namespace

bool application_forum_post_available(u64 id) noexcept {
    if (!g_app.account_ui || !g_app.account.instance || !g_app.account.api.forum_post ||
        g_app.login.visible || application_friends_active() || application_settings_active() ||
        !g_app.plugin_account_scope_serial || !g_app.account.status.profile_id ||
        !g_app.account_ui->chat_state.self_user_id ||
        g_app.account.status.required_action != AccountRequirement::none)
        return false;
    if (!id)
        id = g_app.account_ui->chat_state.selected_channel_id;
    const auto* parent = application_channel_record(id);
    return parent && forum_parent_authorized(id) &&
           g_app.account_ui->chat_state.selected_guild_id == parent->guild_id;
}
bool application_forum_post_begin(u64 id, ApplicationForumPostTicket* output) noexcept {
    if (!id && g_app.account_ui)
        id = g_app.account_ui->chat_state.selected_channel_id;
    if (!application_forum_post_available(id))
        return false;
    if (!forum_owner) {
        forum_owner = static_cast<ForumOwner*>(
            VirtualAlloc(nullptr, sizeof(ForumOwner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!forum_owner)
            return false;
    }
    auto& owner = *forum_owner;
    if (!owner.retired && owner.source.parent == id && owner.phase != ForumPhase::settled &&
        forum_source_current(owner.source)) {
        if (output)
            *output = forum_ticket(owner);
        return true;
    }
    application_forum_form_close(forum_ticket(owner));
    forum_cancel_flight(owner);
    application_forum_attachments_retire(forum_ticket(owner));
    if (!forum_actor_current(owner.source)) {
        clear_bytes(owner.uncertain_parents, sizeof(owner.uncertain_parents));
        owner.uncertain_all = false;
        owner.actor_floor = 0;
    }
    const auto* parent = application_channel_record(id);
    owner.source = {g_app.plugin_account_scope_serial,
                    g_app.account.status.profile_id,
                    g_app.account_ui->chat_state.self_user_id,
                    parent->guild_id,
                    id,
                    g_app.account_ui->active_channel_id,
                    g_app.account_ui->chat_state.selected_channel_id,
                    g_app.account.instance,
                    g_app.account_ui,
                    parent->generation, parent->type};
    const auto name = chat_state_store_channel_name(g_app.account_ui->chat_state, *parent);
    owner.parent_name_bytes = utf8_copy_truncated(owner.parent_name, name);
    owner.serial = ++forum_serial;
    owner.publication = ++forum_publication;
    owner.title_bytes = owner.body_bytes = owner.selected_count = 0;
    reset_value(&owner.metadata);
    owner.metadata_ready = false;
    owner.known_created = 0;
    owner.known_media = false;
    owner.starter_stale = false;
    owner.retired = owner.error = owner.terminal = owner.release_pending = false;
    owner.uncertain = forum_ledger_contains(owner, id);
    ++owner.draft_revision;
    ++owner.revision;
    ++owner.metadata_epoch;
    forum_queue_metadata(owner);
    if (output)
        *output = forum_ticket(owner);
    return true;
}
bool application_forum_post_open(u64 id) noexcept {
    ApplicationForumPostTicket ticket{};
    if (!application_forum_post_begin(id, &ticket))
        return false;
    if (application_forum_form_open(ticket))
        return true;
    application_forum_post_cancel(ticket);
    return false;
}
bool application_forum_post_current(const ApplicationForumPostTicket& value) noexcept {
    return forum_owner && forum_ticket_equal(*forum_owner, value) && !forum_owner->retired &&
           forum_owner_current(*forum_owner);
}
bool application_forum_post_editable(const ApplicationForumPostTicket& value) noexcept {
    return application_forum_post_current(value) && forum_phase_editable(*forum_owner);
}
bool application_forum_post_cancel_upload(const ApplicationForumPostTicket& value, u64 draft_id) noexcept {
    if (!application_forum_post_current(value) || !forum_owner->flight.active ||
        forum_owner->flight.request.operation != ForumOperation::attachment_url || forum_owner->flight.attachment_id != draft_id) return false;
    ForumPin pin(*forum_owner);
    forum_cancel_flight(*forum_owner);
    application_forum_attachments_url_unadmitted(value, draft_id);
    ++forum_owner->revision; application_request_frame(); return true;
}
void application_forum_post_attachment_changed(const ApplicationForumPostTicket& value) noexcept {
    if (application_forum_post_editable(value)) forum_draft_changed(*forum_owner);
}
ApplicationForumPostView application_forum_post_view() noexcept {
    if (!forum_owner)
        return {};
    auto& owner = *forum_owner;
    const bool current = !owner.retired && forum_owner_current(owner);
    const bool editable = current && forum_phase_editable(owner);
    forum_tags(owner);
    return {forum_ticket(owner),
            {owner.parent_name, owner.parent_name_bytes},
            {owner.title, owner.title_bytes},
            {owner.body, owner.body_bytes},
            owner.status,
            {owner.tag_views, owner.metadata.tag_count},
            {owner.selected, owner.selected_count},
            owner.revision,
            owner.metadata_revision,
            owner.phase,
            current,
            owner.error,
            editable,
            editable && owner.phase == ForumPhase::editing && !owner.uncertain && !owner.terminal &&
                forum_metadata_fresh(owner) && application_forum_attachments_view(forum_ticket(owner)).ready,
            current &&
                (owner.phase == ForumPhase::failed ||
                 (owner.uncertain && owner.phase == ForumPhase::editing)) &&
                !owner.terminal && !owner.flight.active,
            current && owner.phase != ForumPhase::settled,
            owner.uncertain,
            owner.metadata_ready && (owner.metadata.flags & kForumRequireTagFlag) != 0};
}
bool application_forum_post_draft_update(const ApplicationForumPostTicket& value, Utf8View title,
                                         Utf8View body) noexcept {
    if (!application_forum_post_editable(value) ||
        !forum_draft_text(title, kForumPostTitleMaximumBytes, kForumPostTitleMaximumScalars,
                          true) ||
        !forum_draft_text(body, kForumPostContentMaximumBytes,
                          kForumPostContentMaximumScalars, false))
        return false;
    auto& owner = *forum_owner;
    if (utf8_equal({owner.title, owner.title_bytes}, title) &&
        utf8_equal({owner.body, owner.body_bytes}, body))
        return true;
    copy_bytes(owner.title, title.data, title.bytes);
    copy_bytes(owner.body, body.data, body.bytes);
    owner.title_bytes = title.bytes;
    owner.body_bytes = body.bytes;
    forum_draft_changed(owner);
    return true;
}
bool application_forum_post_tag_toggle(const ApplicationForumPostTicket& value, u64 id) noexcept {
    if (!application_forum_post_editable(value))
        return false;
    auto& owner = *forum_owner;
    for (u32 i = 0; i < owner.selected_count; ++i)
        if (owner.selected[i] == id) {
            for (u32 j = i + 1; j < owner.selected_count; ++j)
                owner.selected[j - 1] = owner.selected[j];
            owner.selected[--owner.selected_count] = 0;
            forum_draft_changed(owner);
            return true;
        }
    if (!forum_metadata_fresh(owner) || owner.selected_count == kForumPostAppliedTagMaximum)
        return false;
    for (u32 i = 0; i < owner.metadata.tag_count; ++i)
        if (owner.metadata.tags[i].id == id &&
            (!owner.metadata.tags[i].moderated || forum_manage_threads(owner))) {
            owner.selected[owner.selected_count++] = id;
            forum_draft_changed(owner);
            return true;
        }
    return false;
}
bool application_forum_post_draft_submit(const ApplicationForumPostTicket& value) noexcept {
    if (!application_forum_post_current(value) || !application_forum_post_view().can_submit)
        return false;
    auto& owner = *forum_owner;
    const auto request = forum_request(owner, ForumOperation::create);
    if (!forum_post_request_valid(request)) {
        forum_publish(owner, ForumPhase::editing,
                      utf8_literal("Add a title (up to 100 characters) and a message or file."), true);
        return false;
    }
    if (!forum_post_body_bytes(request).ok()) {
        forum_publish(owner, ForumPhase::editing, utf8_literal("Attachment descriptions are too long. Shorten them or remove a file."), true);
        return false;
    }
    if (!forum_post_selection_valid(owner.metadata, {owner.selected, owner.selected_count},
                                    forum_manage_threads(owner))) {
        forum_publish(owner, ForumPhase::editing,
                      utf8_literal("Choose up to 5 tags."), true);
        return false;
    }
    if (!g_app.account.status.running || g_app.account.stop_failed) {
        forum_fail(owner, utf8_literal("You're offline. Reconnect to post."));
        return false;
    }
    owner.due = owner.actor_floor;
    owner.busy_count = 0;
    application_forum_attachments_lock(value, true);
    forum_publish(owner, ForumPhase::queued, utf8_literal("Creating post…"));
    return true;
}
bool application_forum_post_retry(const ApplicationForumPostTicket& value, bool acknowledged) noexcept {
    if (!application_forum_post_current(value) || !application_forum_post_view().can_retry)
        return false;
    auto& owner = *forum_owner;
    ForumPin pin(owner);
    if (owner.known_created)
        return forum_navigate(owner);
    if (owner.uncertain && !acknowledged)
        return false;
    if (owner.uncertain) {
        owner.uncertain = false;
        forum_ledger_remove(owner, owner.source.parent);
        // The overflow ledger stays conservative for other/new intents, but
        // this exact explicit acknowledgment authorizes the current draft.
    }
    application_forum_attachments_refresh(value);
    forum_queue_metadata(owner);
    return true;
}
bool application_forum_post_inspect(const ApplicationForumPostTicket& value) noexcept {
    if (!application_forum_post_current(value))
        return false;
    auto& owner = *forum_owner;
    if (owner.phase == ForumPhase::creating || owner.phase == ForumPhase::queued)
        return false;
    if (!application_thread_browser_open(owner.source.parent))
        return false;
    application_forum_form_close(value);
    return true;
}
bool application_forum_post_cancel(const ApplicationForumPostTicket& value) noexcept {
    if (!forum_owner || !forum_ticket_equal(*forum_owner, value) ||
        forum_owner->phase == ForumPhase::settled)
        return false;
    ForumPin pin(*forum_owner);
    forum_retire(*forum_owner);
    return true;
}
bool application_forum_post_synchronize() noexcept {
    if (!forum_owner)
        return true;
    auto& owner = *forum_owner;
    ForumPin pin(owner);
    if (owner.release_pending) {
        forum_cancel_flight(owner);
        return true;
    }
    if (owner.retired || owner.phase == ForumPhase::settled)
        return true;
    if (!forum_owner_current(owner)) {
        forum_retire(owner);
        return true;
    }
    if (owner.navigating)
        return true;
    if (forum_queued(owner) && g_app.account.status.required_action != AccountRequirement::none)
        forum_fail(owner, utf8_literal("Verify your account to post."), false,
                   true);
    else if (forum_queued(owner) && (!g_app.account.status.running || g_app.account.stop_failed))
        forum_fail(owner, utf8_literal("You're offline. Reconnect to post."));
    else if (!owner.known_created && owner.metadata_ready && !forum_metadata_fresh(owner) &&
             !owner.flight.active)
        forum_queue_metadata(owner);
    return true;
}
bool application_forum_post_pending(u64 now) noexcept {
    application_forum_post_synchronize();
    if (!forum_owner || forum_owner->retired || forum_owner->flight.active ||
        !forum_source_current(forum_owner->source) || !g_app.account.status.running || g_app.account.stop_failed ||
        !g_app.account.api.forum_post || g_app.account.status.required_action != AccountRequirement::none ||
        now < maximum(forum_owner->due, forum_owner->actor_floor)) return false;
    if (forum_queued(*forum_owner)) return true;
    ForumPostRequest request{}; u64 draft{};
    return forum_owner->phase == ForumPhase::editing && !forum_owner->uncertain && forum_metadata_fresh(*forum_owner) &&
        application_forum_attachments_url_candidate(forum_ticket(*forum_owner), now, &request, &draft);
}
bool application_forum_post_submit(u64 id, u64 now) noexcept {
    if (!id || !application_forum_post_pending(now))
        return false;
    auto& owner = *forum_owner;
    ForumPin pin(owner);
    const auto operation = owner.phase == ForumPhase::metadata_queued ? ForumOperation::parent_metadata :
        owner.phase == ForumPhase::queued ? ForumOperation::create : ForumOperation::attachment_url;
    auto request = forum_request(owner, operation);
    u64 attachment_id{};
    if (operation == ForumOperation::attachment_url &&
        !application_forum_attachments_url_candidate(forum_ticket(owner), now, &request, &attachment_id)) return false;
    if (!forum_post_request_valid(request) ||
        (operation == ForumOperation::create &&
         (!forum_metadata_fresh(owner) || owner.uncertain ||
          !forum_post_selection_valid(owner.metadata, {owner.selected, owner.selected_count},
                                      forum_manage_threads(owner))))) {
        forum_fail(owner,
                   utf8_literal("Forum settings changed. Check your tags before posting."));
        return false;
    }
    ForumFlight admitted{};
    copy_value(&admitted.source, owner.source);
    admitted.request = request;
    admitted.id = id;
    admitted.serial = owner.serial;
    admitted.publication = owner.publication;
    admitted.metadata_epoch = owner.metadata_epoch;
    admitted.draft_revision = owner.draft_revision;
    admitted.parent_authority = forum_parent_authority(*application_channel_record(owner.source.parent));
    admitted.active = true;
    admitted.attachment_id = attachment_id;
    if (operation == ForumOperation::create) {
        copy_bytes(admitted.title, request.title.data, request.title.bytes);
        copy_bytes(admitted.body, request.content.data, request.content.bytes);
        admitted.request.title = {admitted.title, request.title.bytes};
        admitted.request.content = {admitted.body, request.content.bytes};
        if (request.attachments.count) {
            const auto bytes = message_attachment_storage_bytes(request.attachments, {owner.prepared_sizes, request.attachments.count});
            admitted.attachment_bank_bytes = (bytes + 4095u) & ~4095u;
            if (!bytes || bytes > kForumPostMaximumBodyBytes) { forum_fail(owner, utf8_literal("Couldn't prepare attachments. Try again.")); return false; }
            admitted.attachment_bank = static_cast<u8*>(VirtualAlloc(nullptr, admitted.attachment_bank_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            u32 count{};
            if (!admitted.attachment_bank || !message_attachment_storage_write(request.attachments, {admitted.attachment_bank, bytes}, {owner.prepared_sizes, request.attachments.count}) ||
                !message_attachment_storage_read({admitted.attachment_bank, bytes}, {admitted.attachment_views, kAttachmentUploadMaximumCount}, &count)) {
                if (admitted.attachment_bank) VirtualFree(admitted.attachment_bank, 0, MEM_RELEASE);
                forum_fail(owner, utf8_literal("Couldn't prepare attachments. Try again.")); return false;
            }
            admitted.request.attachments = {admitted.attachment_views, count};
        }
    } else if (operation == ForumOperation::attachment_url) {
        copy_bytes(admitted.upload_filename, request.upload_filename.data, request.upload_filename.bytes);
        admitted.request.upload_filename = {admitted.upload_filename, request.upload_filename.bytes};
        if (!application_forum_attachments_url_admitted(forum_ticket(owner), attachment_id)) return false;
    }
    const auto result = g_app.account.api.forum_post(g_app.account.instance, &admitted.request, id);
    if (!result.ok()) {
        if (admitted.attachment_bank) { secure_clear_bytes(admitted.attachment_bank, admitted.attachment_bank_bytes); VirtualFree(admitted.attachment_bank, 0, MEM_RELEASE); }
        if (operation == ForumOperation::attachment_url) application_forum_attachments_url_unadmitted(forum_ticket(owner), attachment_id);
        if (owner.serial != admitted.serial || owner.retired)
            return false;
        if (result.error == RestClientSubmitError::busy && ++owner.busy_count <= 20)
            owner.due = deadline_after(now, 50);
        else
            forum_fail(owner, utf8_literal("Something went wrong. Try again."));
        return false;
    }
    owner.flight = admitted;
    if (operation == ForumOperation::create) {
        owner.flight.request.title = {owner.flight.title, request.title.bytes};
        owner.flight.request.content = {owner.flight.body, request.content.bytes};
        if (request.attachments.count) owner.flight.request.attachments = {owner.flight.attachment_views, request.attachments.count};
    } else if (operation == ForumOperation::attachment_url) {
        owner.flight.request.upload_filename = {owner.flight.upload_filename, request.upload_filename.bytes};
    }
    if (owner.serial != admitted.serial || owner.retired || !forum_source_current(owner.source)) {
        forum_cancel_flight(owner);
        return true;
    }
    if (operation == ForumOperation::attachment_url) { ++owner.revision; application_request_frame(); return true; }
    forum_publish(owner,
                  operation == ForumOperation::parent_metadata ? ForumPhase::metadata_loading
                                                               : ForumPhase::creating,
                  operation == ForumOperation::parent_metadata ? utf8_literal("Loading forum tags…")
                                                               : utf8_literal("Creating post…"));
    return true;
}
bool application_forum_post_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!forum_owner || !forum_owner->flight.active || !response.request_id ||
        forum_owner->flight.id != response.request_id)
        return false;
    auto& owner = *forum_owner;
    ForumPin pin(owner);
    auto flight = owner.flight;
    ForumFlightBank bank{flight.attachment_bank, flight.attachment_bank_bytes};
    if (flight.request.operation == ForumOperation::create) {
        flight.request.title = {flight.title, flight.request.title.bytes};
        flight.request.content = {flight.body, flight.request.content.bytes};
        if (flight.request.attachments.count) flight.request.attachments = {flight.attachment_views, flight.request.attachments.count};
    } else if (flight.request.operation == ForumOperation::attachment_url) {
        flight.request.upload_filename = {flight.upload_filename, flight.request.upload_filename.bytes};
    }
    reset_value(&owner.flight);
    const auto now = clock_monotonic_milliseconds();
    const bool limited = response.rate_limited || response.status_code == 429;
    if (forum_actor_current(flight.source) && limited)
        owner.actor_floor =
            maximum(owner.actor_floor,
                    maximum(response.retry_at_ms,
                            deadline_after(now, maximum(response.rate.retry_after_ms, u64{50}))));
    const bool same = !flight.retired && !owner.retired && flight.serial == owner.serial &&
                      forum_source_current(owner.source) &&
                      (flight.request.operation != ForumOperation::create ||
                       flight.draft_revision == owner.draft_revision);
    if (!same) {
        if (flight.request.operation == ForumOperation::attachment_url)
            application_forum_attachments_url_unadmitted({flight.serial, flight.source.parent, flight.publication}, flight.attachment_id);
        if (!owner.release_pending)
            application_forum_post_synchronize();
        return true;
    }
    const bool create = flight.request.operation == ForumOperation::create;
    const bool action_required = response.status_code == 401 || response.error == RestClientError::account_restricted ||
                                 api_rejection_requires_action(response.rejection) ||
                                 g_app.account.status.required_action != AccountRequirement::none;
    if (flight.request.operation == ForumOperation::attachment_url) {
        auto upload_response = response;
        if (flight.metadata_epoch != owner.metadata_epoch ||
            !forum_parent_authority_current(owner.source.parent, flight.parent_authority))
            upload_response.error = RestClientError::response;
        application_forum_attachments_url_response(forum_ticket(owner), flight.attachment_id, upload_response, body);
        if (action_required)
            forum_fail(owner, utf8_literal("Verify your account to upload files."), false, true);
        else if (limited) forum_publish(owner, ForumPhase::editing, utf8_literal("You're uploading too fast. Wait a moment and try again."), true);
        else if (response.status_code == 403) forum_fail(owner, utf8_literal("You no longer have permission to upload here."), false, true);
        else { ++owner.revision; application_request_frame(); application_forum_post_synchronize(); }
        return true;
    }
    if (action_required) {
        forum_fail(owner,
                   response.status_code == 401
                       ? utf8_literal("Your session expired. Sign in again.")
                       : utf8_literal("Verify your account to post."),
                   create, true);
        return true;
    }
    if (limited) {
        forum_fail(owner, utf8_literal("You're doing that too fast. Wait a moment and try again."));
        return true;
    }
    if (response.status_code == 403) {
        forum_fail(owner, utf8_literal("You no longer have permission to post here."), false, true);
        return true;
    }
    const bool complete = response.transport_ok() && response.status_code >= 200 &&
                          response.status_code < 300 && body.data && body.bytes == response.body_bytes &&
                          body.bytes <= kForumPostMaximumResponseBytes;
    const auto scratch =
        Slice<u32>{g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount};
    if (!create) {
        if (flight.metadata_epoch != owner.metadata_epoch ||
            !forum_parent_authority_current(owner.source.parent, flight.parent_authority)) {
            forum_fail(owner, utf8_literal("Forum settings changed. Try again."));
            return true;
        }
        ForumParentSnapshot candidate{};
        if (!complete || !forum_parent_decode(body, flight.request, scratch, &candidate).ok() ||
            candidate.type != owner.source.parent_type ||
            (candidate.fields & (forum_parent_flags | forum_parent_tags)) !=
                (forum_parent_flags | forum_parent_tags)) {
            forum_fail(owner, utf8_literal("Couldn't load tags. Try again."));
            return true;
        }
        owner.metadata = candidate;
        owner.metadata_ready = true;
        owner.metadata_checked_epoch = owner.metadata_epoch;
        owner.metadata_parent_authority = flight.parent_authority;
        ++owner.metadata_revision;
        ++owner.revision;
        forum_tags(owner);
        forum_publish(owner, ForumPhase::editing,
                      owner.uncertain ? utf8_literal("Your post may exist. Check posts before retrying.")
                                      : Utf8View{});
        return true;
    }
    const bool media = flight.request.attachments.count != 0;
    const bool decoded = complete && (media ? application_forum_attachments_decode_media(body, flight.request, flight.source.self, scratch, &owner.candidate) :
        forum_post_decode(body, flight.request, flight.source.self, scratch, &owner.candidate).ok());
    if (decoded) {
        owner.known_media = media;
        application_forum_attachments_lock(forum_ticket(owner), false);
        if (flight.metadata_epoch != owner.metadata_epoch ||
            !forum_parent_authority_current(owner.source.parent, flight.parent_authority)) {
            forum_fail(
                owner,
                utf8_literal(
                    "Your post may exist. Check posts before retrying."),
                true);
            return true;
        }
        owner.starter_stale = flight.changed_messages_all;
        for (const auto& changed : flight.changed_messages)
            owner.starter_stale = owner.starter_stale ||
                (changed.channel == owner.candidate.message_channel_id && changed.message == owner.candidate.message_id);
        if (forum_publish_thread(owner, flight)) {
            forum_navigate(owner);
            return true;
        }
        if (owner.known_created)
            forum_fail(owner, utf8_literal("Post created, but couldn't be opened. Try again or view posts."));
        else
            forum_fail(
                owner,
                utf8_literal("Couldn't open your post. Check the posts list."),
                true);
        return true;
    }
    if (response.transport_ok() && response.status_code == 400 && response.rejection.has_code &&
        (response.rejection.code == 50035 || response.rejection.code == 40066 ||
         response.rejection.code == 40067)) {
        owner.metadata_ready = false;
        forum_fail(
            owner,
            utf8_literal(
                "Couldn't create the post. Check the title, message and tags."));
    } else
        forum_fail(
            owner,
            utf8_literal(
                "Your post may exist. Check posts before retrying."),
            true);
    return true;
}
u64 application_forum_post_next_wake() noexcept {
    if (!forum_owner || forum_owner->retired || forum_owner->flight.active || !forum_source_current(forum_owner->source) ||
        !g_app.account.status.running || g_app.account.stop_failed || g_app.account.status.required_action != AccountRequirement::none) return 0;
    if (forum_queued(*forum_owner)) return maximum(u64{1}, maximum(forum_owner->due, forum_owner->actor_floor));
    ForumPostRequest request{}; u64 draft{};
    const auto now = clock_monotonic_milliseconds();
    if (forum_owner->phase == ForumPhase::editing && !forum_owner->uncertain && forum_metadata_fresh(*forum_owner) &&
        application_forum_attachments_url_candidate(forum_ticket(*forum_owner), now, &request, &draft))
        return maximum(u64{1}, maximum(forum_owner->due, forum_owner->actor_floor));
    return 0;
}
void application_forum_post_gateway(u64 guild, u64 parent, u64 thread) noexcept {
    if (!forum_owner)
        return;
    auto& flight = forum_owner->flight;
    if (thread && flight.active && flight.request.operation == ForumOperation::create &&
        (!guild || guild == flight.source.guild) && (!parent || parent == flight.source.parent)) {
        for (u64 id : flight.changed_threads)
            if (id == thread)
                return;
        for (auto& id : flight.changed_threads)
            if (!id) {
                id = thread;
                return;
            }
        flight.changed_all = true;
        return;
    }
    if (thread || (guild && guild != forum_owner->source.guild) ||
        (parent && parent != forum_owner->source.parent))
        return;
    ++forum_owner->metadata_epoch;
    application_forum_post_synchronize();
}
bool application_forum_post_observation_needed() noexcept {
    if (!forum_owner || forum_owner->retired || forum_owner->phase == ForumPhase::settled)
        return false;
    const auto& owner = *forum_owner;
    return (owner.flight.active && !owner.flight.retired &&
            owner.flight.request.operation == ForumOperation::create && forum_actor_current(owner.flight.source)) ||
           (owner.known_created && forum_actor_current(owner.source));
}
void application_forum_post_message_deleted(u64 channel_id, u64 message_id) noexcept {
    if (!channel_id || !message_id || !application_forum_post_observation_needed())
        return;
    auto& owner = *forum_owner;
    if (owner.known_created == channel_id && owner.candidate.message_id == message_id &&
        forum_actor_current(owner.source))
        owner.starter_stale = true;
    auto& flight = owner.flight;
    if (!flight.active || flight.retired || flight.request.operation != ForumOperation::create ||
        !forum_actor_current(flight.source))
        return;
    const auto* channel = application_channel_record(channel_id);
    if (channel && (channel->type != 11 || channel->parent_id != flight.source.parent ||
                    channel->guild_id != flight.source.guild))
        return;
    for (const auto& changed : flight.changed_messages)
        if (changed.channel == channel_id && changed.message == message_id)
            return;
    for (auto& changed : flight.changed_messages)
        if (!changed.message) {
            changed = {channel_id, message_id};
            return;
        }
    flight.changed_messages_all = true;
}
void application_forum_post_message_observe(const GatewayMessageEvent& message) noexcept {
    application_forum_post_message_deleted(message.channel_id, message.id);
}
void application_forum_post_release() noexcept {
    if (!forum_owner)
        return;
    ForumPin pin(*forum_owner);
    forum_retire(*forum_owner);
    forum_owner->release_pending = true;
}
ApplicationForumPostMetrics application_forum_post_metrics() noexcept {
    if (!forum_owner)
        return {};
    return {sizeof(ForumOwner), (sizeof(ForumOwner) + 4095u) & ~4095u, forum_owner->publication,
            forum_owner->flight.active};
}
} // namespace maicord::app_detail
