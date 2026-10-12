#include "application_thread_creation.hpp"
#include "application_state.hpp"
#include "application_text_form.hpp"
#include "application_content_input.hpp"
#include "application_thread_browser.hpp"
#include "application_settings.hpp"
#include "application_friends.hpp"
#include "maicord/protocol/thread_creation.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"

#include <Windows.h>

namespace maicord::app_detail {
namespace {
using Phase = ApplicationThreadCreationPhase;
using Operation = ThreadCreationOperation;
constexpr u32 has_thread = 1u << 5;
struct CreationSource {
    u64 account_scope, profile, self, guild, parent, message, author, timestamp, edited_timestamp, content_signature;
    const void* instance;
    const AccountUiRuntime* ui;
    const MessageStore* store;
    MessageHandle handle;
    u32 message_revision, message_flags;
    u16 parent_generation, message_type;
    u8 thread_type, presentation_flags;
    MessageContentAvailability content_availability;
};
struct CreationFlight {
    CreationSource source;
    ThreadCreationRequest request;
    u64 id, serial, gateway_serial;
    char name[kThreadCreationNameMaximumBytes];
    bool active, retired, cancel_requested;
};
struct CreationOwner {
    CreationSource source;
    CreationFlight flight;
    ThreadCreationSnapshot candidate;
    char name[kThreadCreationNameMaximumBytes];
    char parent_name[kThreadCreationNameMaximumBytes];
    // An uncertainty ledger survives closing/replacing the dialog. On
    // overflow, every later creation in this actor requires a read first.
    u64 uncertain_messages[8];
    u64 serial, gateway_serial, actor_floor, due;
    Utf8View status;
    TextFormOwnerKey form;
    u32 publication, revision, name_bytes, parent_name_bytes, checked_source_revision, busy_count;
    Phase phase;
    Operation operation;
    bool retired, error, uncertain, reconcile_all, fix_name, terminal, release_pending, form_bound;
};
static_assert(sizeof(CreationOwner) <= 4096);
CreationOwner* creation_owner{};
u64 creation_serial{};
u32 creation_publication{};

const ChannelRecord* channel(u64 id) noexcept {
    return g_app.account_ui ? chat_state_store_find_channel(g_app.account_ui->chat_state, id) : nullptr;
}
bool actor_current(const CreationSource& source) noexcept {
    return g_app.account_ui && !g_app.login.visible && source.ui == g_app.account_ui &&
        source.instance == g_app.account.instance && source.account_scope == g_app.plugin_account_scope_serial &&
        source.profile == g_app.account.status.profile_id && source.self == g_app.account_ui->chat_state.self_user_id;
}
bool parent_authorized(u64 id) noexcept {
    const auto* parent = channel(id);
    if (!parent || !parent->occupied || (parent->type != 0 && parent->type != 5) || !parent->guild_id ||
        !parent->permissions_resolved || (parent->flags & kChannelObfuscatedFlag)) return false;
    constexpr u32 required = channel_action_view | channel_action_read_history |
        channel_action_create_public_threads;
    const auto& state = g_app.account_ui->chat_state;
    const auto* guild = chat_state_store_find_guild(state, parent->guild_id);
    return guild && guild->occupied && !guild->unavailable && (parent->actions & required) == required &&
        chat_state_store_channel_visible(state, *parent);
}
bool user_message(u16 type) noexcept { return type == 0 || type == 19 || type == 20 || type == 23; }
const MessageRecord* resident(u64 id, MessageHandle* handle = nullptr) noexcept {
    const auto found = message_store_find(g_app.message_view.store, id);
    if (!found.ok()) return nullptr;
    const auto* message = message_store_resolve(g_app.message_view.store, found.handle);
    if (message && handle) *handle = found.handle;
    return message;
}
u64 content_signature(MessageHandle handle) noexcept {
    const auto content = message_store_content(g_app.message_view.store, handle);
    return hash64_counted_bytes(kHash64Seed, content.data, content.bytes);
}
bool scope_current(const CreationSource& source) noexcept {
    const auto* parent = actor_current(source) ? channel(source.parent) : nullptr;
    return parent && parent_authorized(source.parent) && parent->generation == source.parent_generation &&
        parent->guild_id == source.guild && source.store == &g_app.message_view.store &&
        g_app.account_ui->active_channel_id == source.parent &&
        g_app.account_ui->chat_state.selected_channel_id == source.parent &&
        g_app.account_ui->chat_state.selected_guild_id == source.guild;
}
const ChannelRecord* known_thread(const CreationSource& source) noexcept {
    if (!actor_current(source) || !parent_authorized(source.parent)) return nullptr;
    const auto* parent = channel(source.parent);
    const auto* thread = channel(source.message);
    return parent && parent->generation == source.parent_generation && parent->guild_id == source.guild &&
        thread && thread->occupied && thread->guild_id == source.guild && thread->parent_id == source.parent &&
        thread->type == source.thread_type && thread->thread_archived_known && thread->thread_locked_known &&
        chat_state_store_channel_name(g_app.account_ui->chat_state, *thread).bytes &&
        !(thread->flags & kChannelObfuscatedFlag) &&
        chat_state_store_thread_accessible(g_app.account_ui->chat_state, *thread) ? thread : nullptr;
}
bool source_current(CreationOwner& owner, bool association = true) noexcept {
    if (!scope_current(owner.source)) return false;
    const auto* message = message_store_resolve(*owner.source.store, owner.source.handle);
    if (!message || !message->occupied || message->id != owner.source.message || message->channel_id != owner.source.parent ||
        message->delivery != MessageDeliveryState::authoritative || message->author_id != owner.source.author ||
        message->type != owner.source.message_type || !user_message(message->type) ||
        message->timestamp_ms != owner.source.timestamp || message->edited_timestamp_ms != owner.source.edited_timestamp ||
        message->content_availability != owner.source.content_availability ||
        message->presentation_flags != owner.source.presentation_flags) return false;
    if (owner.phase == Phase::editing && ((message->flags & has_thread) || known_thread(owner.source))) return false;
    if (message->revision == owner.checked_source_revision) return true;
    // THREAD_CREATE can be followed by MESSAGE_UPDATE(HAS_THREAD) before HTTP
    // completes. That canonical association is progress, not a source replacement.
    // Other revisions cancel the captured operation. Hash only when the
    // message changed, never during unchanged form/frame polling.
    const bool linked = association && owner.phase != Phase::editing &&
        message->flags == (owner.source.message_flags | has_thread) &&
        message->edited_timestamp_ms == owner.source.edited_timestamp &&
        owner.checked_source_revision == owner.source.message_revision &&
        content_signature(owner.source.handle) == owner.source.content_signature;
    if (!linked) return false;
    owner.checked_source_revision = message->revision;
    return true;
}
ApplicationThreadCreationTicket ticket(const CreationOwner& owner) noexcept {
    return {owner.serial, owner.source.parent, owner.source.message, owner.publication};
}
bool ticket_equal(const CreationOwner& owner, const ApplicationThreadCreationTicket& value) noexcept {
    return value.serial && owner.serial == value.serial && owner.source.parent == value.parent_id &&
        owner.source.message == value.source_message_id && owner.publication == value.publication_revision;
}
void publish(CreationOwner& owner, Phase phase, Utf8View status = {}, bool error = false) noexcept {
    if (owner.phase == phase && owner.error == error && utf8_equal(owner.status, status)) return;
    owner.phase = phase; owner.status = status; owner.error = error;
    ++owner.revision;
    application_request_frame();
}
bool ledger_contains(const CreationOwner& owner, u64 id) noexcept {
    if (owner.reconcile_all) return true;
    for (u64 value : owner.uncertain_messages) if (value == id) return true;
    return false;
}
void ledger_add(CreationOwner& owner, u64 id) noexcept {
    for (u64 value : owner.uncertain_messages) if (value == id) return;
    for (auto& value : owner.uncertain_messages) if (!value) { value = id; return; }
    owner.reconcile_all = true;
}
void ledger_remove(CreationOwner& owner, u64 id) noexcept {
    for (auto& value : owner.uncertain_messages) if (value == id) value = 0;
}
void cancel_flight(CreationOwner& owner) noexcept {
    auto& flight = owner.flight;
    if (!flight.active) return;
    flight.retired = true;
    if (flight.request.operation == Operation::from_message) ledger_add(owner, flight.source.message);
    if (!flight.cancel_requested && flight.source.instance && flight.source.instance == g_app.account.instance &&
        g_app.account.api.thread_creation) {
        g_app.account.api.thread_creation(g_app.account.instance, nullptr, flight.id);
        flight.cancel_requested = true;
    }
}
bool form_valid(TextFormOwnerKey) noexcept;
void retire(CreationOwner& owner) noexcept {
    cancel_flight(owner);
    owner.retired = true;
    publish(owner, Phase::cancelled);
    application_text_form_cancel_owner(form_valid);
    owner.form_bound = false;
}
void fail(CreationOwner& owner, Utf8View status, bool uncertain = false, bool terminal = false, bool fix_name = false) noexcept {
    owner.uncertain = uncertain;
    owner.terminal = terminal;
    owner.fix_name = fix_name;
    if (uncertain) ledger_add(owner, owner.source.message);
    publish(owner, Phase::failed, status, true);
}
void free_owner() noexcept {
    if (!creation_owner) return;
    SecureZeroMemory(creation_owner, sizeof(*creation_owner));
    VirtualFree(creation_owner, 0, MEM_RELEASE);
    creation_owner = nullptr;
}
bool form_key(const CreationOwner& owner, TextFormOwnerKey key) noexcept {
    return key.scope == owner.form.scope && key.collection == owner.form.collection && key.item == owner.form.item;
}
bool form_valid(TextFormOwnerKey key) noexcept {
    return creation_owner && form_key(*creation_owner, key) && !creation_owner->retired &&
        (creation_owner->phase == Phase::settled || source_current(*creation_owner));
}
bool form_save(TextFormOwnerKey key, Utf8View text) noexcept {
    if (!form_valid(key)) return false;
    auto& owner = *creation_owner;
    if (owner.phase == Phase::failed && !owner.fix_name) application_thread_creation_retry(ticket(owner));
    else application_thread_creation_name_submit(ticket(owner), text);
    return false; // The form remains visible while its asynchronous request is pending.
}
ApplicationTextFormProgress form_progress(TextFormOwnerKey key) noexcept {
    if (!creation_owner || !form_key(*creation_owner, key)) return {{}, {}, 0, false, false, true};
    const auto view = application_thread_creation_view();
    return {view.status, view.uncertain ? utf8_literal("Check again") : view.can_retry ? utf8_literal("Retry") : utf8_literal("Create"),
        view.revision, view.can_submit, view.can_submit || view.can_retry, view.phase == Phase::settled};
}
void form_closed(TextFormOwnerKey key) noexcept {
    if (!creation_owner || !form_key(*creation_owner, key)) return;
    creation_owner->form_bound = false;
    if (creation_owner->phase != Phase::settled && creation_owner->phase != Phase::cancelled)
        retire(*creation_owner);
}
bool settle(CreationOwner& owner) noexcept {
    if (owner.retired || !source_current(owner) || !known_thread(owner.source)) return false;
    const auto source = owner.source;
    ledger_remove(owner, source.message);
    owner.uncertain = owner.terminal = owner.fix_name = false;
    publish(owner, Phase::settled, utf8_literal("Thread created."));
    // A matching Gateway creation is the server-confirmed result even before HTTP completes.
    // Cancel that redundant request, but keep its response/late floor alive.
    if (owner.flight.active) {
        cancel_flight(owner);
        ledger_remove(owner, source.message);
    }
    application_text_form_cancel_owner(form_valid);
    owner.form_bound = false;
    if (scope_current(source) && known_thread(source))
        application_navigate_known_message(source.message, 0);
    return true;
}
bool receipt_input(const ThreadCreationSnapshot& receipt, ChannelInput* output) noexcept {
    if (!output) return false;
    const auto* old = channel(receipt.id);
    if (old && (old->parent_id != receipt.parent_id || old->guild_id != receipt.guild_id || old->type != receipt.type)) return false;
    *output = {receipt.id, receipt.guild_id, receipt.parent_id, thread_creation_name_view(receipt),
        old ? old->position : 0, old ? old->flags : 0, receipt.type};
    output->name_known = true;
    output->last_message_known = (receipt.fields & thread_creation_last_message_known) != 0;
    output->last_message_id = receipt.last_message_id;
    output->thread_archived_known = output->thread_locked_known = true;
    output->thread_archived = (receipt.fields & thread_creation_archived) != 0;
    output->thread_locked = (receipt.fields & thread_creation_locked) != 0;
    output->thread_active_known = true;
    output->thread_listed_active = !output->thread_archived;
    output->thread_self_joined_known = (receipt.fields & thread_creation_self_joined) != 0;
    output->thread_self_joined = output->thread_self_joined_known;
    return true;
}
CreationSource capture(const ChannelRecord& parent, const MessageRecord& message, MessageHandle handle) noexcept {
    return {g_app.plugin_account_scope_serial, g_app.account.status.profile_id, g_app.account_ui->chat_state.self_user_id,
        parent.guild_id, parent.id, message.id, message.author_id, message.timestamp_ms, message.edited_timestamp_ms, content_signature(handle),
        g_app.account.instance, g_app.account_ui, &g_app.message_view.store, handle, message.revision, message.flags,
        parent.generation, message.type, static_cast<u8>(parent.type == 5 ? 10 : 11),
        message.presentation_flags, message.content_availability};
}
void queue(CreationOwner& owner, Operation operation) noexcept {
    owner.operation = operation;
    owner.due = owner.actor_floor;
    owner.busy_count = 0;
    owner.error = owner.terminal = owner.fix_name = false;
    publish(owner, operation == Operation::reconcile ? Phase::reconcile_queued : Phase::queued,
        operation == Operation::reconcile ? utf8_literal("Checking for the thread…") : utf8_literal("Creating thread…"));
}
bool queued(const CreationOwner& owner) noexcept {
    return owner.phase == Phase::queued || owner.phase == Phase::reconcile_queued;
}
}

bool application_thread_creation_available(u64 id) noexcept {
    if (!id || !g_app.account_ui || !g_app.account.instance || !g_app.account.api.thread_creation || g_app.login.visible ||
        application_friends_active() || application_settings_active() || application_thread_browser_active() ||
        !g_app.plugin_account_scope_serial || !g_app.account.status.profile_id || !g_app.account_ui->chat_state.self_user_id ||
        g_app.account.status.required_action != AccountRequirement::none) return false;
    const auto* message = resident(id);
    if (!message || !message->occupied || message->delivery != MessageDeliveryState::authoritative || !message->author_id ||
        !user_message(message->type) || (message->flags & has_thread) || !parent_authorized(message->channel_id)) return false;
    const auto* parent = channel(message->channel_id);
    const auto* existing = channel(message->id);
    return !existing && parent && g_app.account_ui->active_channel_id == parent->id &&
        g_app.account_ui->chat_state.selected_channel_id == parent->id &&
        g_app.account_ui->chat_state.selected_guild_id == parent->guild_id;
}
bool application_thread_creation_begin(u64 id, ApplicationThreadCreationTicket* output) noexcept {
    if (!application_thread_creation_available(id)) return false;
    if (!creation_owner) {
        creation_owner = static_cast<CreationOwner*>(VirtualAlloc(nullptr, sizeof(CreationOwner), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!creation_owner) return false;
    }
    auto& owner = *creation_owner;
    if (!owner.retired && owner.source.message == id && source_current(owner) &&
        (owner.phase == Phase::editing || queued(owner) || owner.flight.active)) {
        if (output) *output = ticket(owner);
        return true;
    }
    MessageHandle handle{};
    const auto* message = resident(id, &handle);
    const auto* parent = message ? channel(message->channel_id) : nullptr;
    if (!parent || !message) return false;
    // The form retains context across frames. Detach an older form before
    // replacing the controller, then copy its origin label out of mutable Core banks.
    application_text_form_cancel_owner(form_valid);
    cancel_flight(owner);
    if (!actor_current(owner.source)) {
        clear_bytes(owner.uncertain_messages, sizeof(owner.uncertain_messages));
        owner.reconcile_all = false;
        owner.actor_floor = 0;
    }
    owner.source = capture(*parent, *message, handle);
    const auto parent_name = chat_state_store_channel_name(g_app.account_ui->chat_state, *parent);
    owner.parent_name_bytes = 0;
    while (owner.parent_name_bytes < parent_name.bytes) {
        const auto scalar = utf8_decode_one(parent_name.data + owner.parent_name_bytes, parent_name.bytes - owner.parent_name_bytes);
        if (!scalar.valid || owner.parent_name_bytes + scalar.width > sizeof(owner.parent_name)) break;
        owner.parent_name_bytes += scalar.width;
    }
    copy_bytes(owner.parent_name, parent_name.data, owner.parent_name_bytes);
    owner.serial = ++creation_serial;
    owner.publication = ++creation_publication;
    owner.checked_source_revision = message->revision;
    owner.name_bytes = 0;
    owner.name[0] = 0;
    owner.retired = owner.error = owner.terminal = owner.fix_name = owner.release_pending = false;
    owner.uncertain = ledger_contains(owner, id);
    owner.form_bound = false;
    owner.form = {owner.source.account_scope, owner.source.parent, owner.serial};
    ++owner.revision;
    publish(owner, Phase::editing, owner.uncertain ? utf8_literal("Your thread may already exist. Checking first.") : Utf8View{});
    if (output) *output = ticket(owner);
    return true;
}
bool application_thread_creation_open(u64 id) noexcept {
    if (g_app.text_form || !application_thread_creation_begin(id)) return false;
    auto& owner = *creation_owner;
    ApplicationTextFormRequest request{utf8_literal("Create thread"),
        {owner.parent_name, owner.parent_name_bytes},
        utf8_literal("Thread name"), utf8_literal("Create"), {owner.name, owner.name_bytes}, owner.form,
        form_valid, form_save, kThreadCreationNameMaximumScalars};
    request.maximum_bytes = kThreadCreationNameMaximumBytes;
    request.single_line = true;
    request.progress = form_progress;
    request.closed = form_closed;
    if (!application_text_form_open(request)) { retire(owner); return false; }
    owner.form_bound = true;
    return true;
}
bool application_thread_creation_current(const ApplicationThreadCreationTicket& value) noexcept {
    return creation_owner && ticket_equal(*creation_owner, value) && !creation_owner->retired && source_current(*creation_owner);
}
ApplicationThreadCreationView application_thread_creation_view() noexcept {
    if (!creation_owner) return {};
    auto& owner = *creation_owner;
    const bool current = !owner.retired && source_current(owner);
    const auto* parent = current ? channel(owner.source.parent) : nullptr;
    // A replacement intent can hold its name/queued choice while the older
    // canceled request still holds DETAILS. Enqueueing stays guarded by
    // pending(), which requires the older response to be consumed first.
    const bool name_available = !owner.flight.active ||
        (owner.flight.retired && owner.flight.serial != owner.serial);
    const bool can_submit = current && name_available && (owner.phase == Phase::editing ||
        (owner.phase == Phase::failed && owner.fix_name && !owner.terminal));
    const bool can_retry = current && !owner.flight.active && owner.phase == Phase::failed && !owner.terminal &&
        !owner.fix_name && owner.name_bytes;
    return {ticket(owner), parent ? chat_state_store_channel_name(g_app.account_ui->chat_state, *parent) : Utf8View{},
        {owner.name, owner.name_bytes}, owner.status, owner.revision, owner.phase, current, owner.error,
        can_submit, can_retry, current && owner.phase != Phase::settled && owner.phase != Phase::cancelled, owner.uncertain};
}
bool application_thread_creation_name_submit(const ApplicationThreadCreationTicket& value, Utf8View name) noexcept {
    if (!application_thread_creation_current(value) || !application_thread_creation_view().can_submit) return false;
    auto& owner = *creation_owner;
    ThreadCreationRequest request{owner.source.parent, owner.source.guild, owner.source.message, name,
        kThreadCreationDefaultArchiveDuration, owner.source.thread_type, Operation::from_message, {}};
    bool valid = thread_creation_request_valid(request);
    for (u32 at = 0; valid && at < name.bytes;) {
        const auto scalar = utf8_decode_one(name.data + at, name.bytes - at);
        if (!scalar.valid || scalar.codepoint == 0x2028 || scalar.codepoint == 0x2029) valid = false;
        at += scalar.width;
    }
    if (!valid) { fail(owner, utf8_literal("Thread names must be 1–100 characters on one line."), owner.uncertain, false, true); return false; }
    copy_bytes(owner.name, name.data, name.bytes);
    owner.name_bytes = name.bytes;
    if (!g_app.account.status.running || g_app.account.stop_failed || !g_app.account.instance || !g_app.account.api.thread_creation) {
        fail(owner, utf8_literal("Can't create threads while offline.")); return false;
    }
    if (g_app.account.status.required_action != AccountRequirement::none) {
        fail(owner, utf8_literal("Verify your account to create threads."), owner.uncertain, true); return false;
    }
    owner.uncertain = owner.uncertain || ledger_contains(owner, owner.source.message);
    queue(owner, owner.uncertain ? Operation::reconcile : Operation::from_message);
    return true;
}
bool application_thread_creation_retry(const ApplicationThreadCreationTicket& value) noexcept {
    if (!application_thread_creation_current(value) || !application_thread_creation_view().can_retry) return false;
    auto& owner = *creation_owner;
    if (!g_app.account.status.running || g_app.account.stop_failed || !g_app.account.instance || !g_app.account.api.thread_creation) {
        fail(owner, utf8_literal("Can't create threads while offline."), owner.uncertain); return false;
    }
    if (g_app.account.status.required_action != AccountRequirement::none) {
        fail(owner, utf8_literal("Verify your account to create threads."), owner.uncertain, true); return false;
    }
    queue(owner, owner.uncertain ? Operation::reconcile : Operation::from_message);
    return true;
}
bool application_thread_creation_cancel(const ApplicationThreadCreationTicket& value) noexcept {
    if (!creation_owner || !ticket_equal(*creation_owner, value) || creation_owner->phase == Phase::settled) return false;
    retire(*creation_owner); return true;
}
bool application_thread_creation_synchronize() noexcept {
    auto* owner = creation_owner;
    if (!owner) return true;
    if (owner->release_pending) {
        cancel_flight(*owner);
        if (!owner->flight.active || !g_app.account.instance || (!g_app.account.status.running && !g_app.account.stop_failed)) free_owner();
        return true;
    }
    if (owner->retired || owner->phase == Phase::settled) return true;
    if (!source_current(*owner)) { retire(*owner); return true; }
    if (owner->phase != Phase::editing && known_thread(owner->source)) { settle(*owner); return true; }
    if (owner->phase == Phase::queued) {
        const auto* message = message_store_resolve(*owner->source.store, owner->source.handle);
        if (message && (message->flags & has_thread)) {
            owner->uncertain = true;
            ledger_add(*owner, owner->source.message);
            queue(*owner, Operation::reconcile);
        }
    }
    if (queued(*owner) && g_app.account.status.required_action != AccountRequirement::none)
        fail(*owner, utf8_literal("Verify your account to create threads."), owner->uncertain, true);
    else if (queued(*owner) && (!g_app.account.status.running || g_app.account.stop_failed ||
        !g_app.account.instance || !g_app.account.api.thread_creation))
        fail(*owner, utf8_literal("Can't create threads while offline."), owner->uncertain);
    return true;
}
bool application_thread_creation_pending(u64 now) noexcept {
    application_thread_creation_synchronize();
    return creation_owner && !creation_owner->retired && !creation_owner->flight.active && queued(*creation_owner) &&
        source_current(*creation_owner) && g_app.account.status.running && !g_app.account.stop_failed &&
        g_app.account.instance && g_app.account.api.thread_creation &&
        g_app.account.status.required_action == AccountRequirement::none &&
        now >= maximum(creation_owner->due, creation_owner->actor_floor);
}
bool application_thread_creation_submit(u64 id, u64 now) noexcept {
    if (!id || !application_thread_creation_pending(now)) return false;
    auto& owner = *creation_owner;
    ThreadCreationRequest request{owner.source.parent, owner.source.guild, owner.source.message,
        {owner.name, owner.name_bytes}, kThreadCreationDefaultArchiveDuration, owner.source.thread_type, owner.operation, {}};
    if (!thread_creation_request_valid(request)) { fail(owner, utf8_literal("Enter a valid thread name."), false, false, true); return false; }
    CreationFlight admitted{owner.source, request, id, owner.serial, owner.gateway_serial, {}, true, false, false};
    copy_bytes(admitted.name, owner.name, owner.name_bytes);
    admitted.request.name = {admitted.name, owner.name_bytes};
    const auto result = g_app.account.api.thread_creation(g_app.account.instance, &admitted.request, id);
    if (!result.ok()) {
        if (owner.serial != admitted.serial || owner.retired) return false;
        if (result.error == RestClientSubmitError::busy && ++owner.busy_count <= 20) owner.due = deadline_after(now, 50);
        else fail(owner, utf8_literal("Couldn't create the thread. Try again."), owner.uncertain);
        return false;
    }
    owner.flight = admitted;
    owner.flight.request.name = {owner.flight.name, admitted.request.name.bytes};
    if (owner.serial != admitted.serial || owner.retired || !source_current(owner)) {
        cancel_flight(owner);
        return true; // Accepted canceled work still holds the DETAILS response.
    }
    publish(owner, owner.operation == Operation::reconcile ? Phase::reconciling : Phase::creating,
        owner.operation == Operation::reconcile ? utf8_literal("Checking for the thread…") : utf8_literal("Creating thread…"));
    return true;
}
bool application_thread_creation_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!creation_owner || !creation_owner->flight.active || !response.request_id || creation_owner->flight.id != response.request_id) return false;
    auto& owner = *creation_owner;
    auto flight = owner.flight;
    flight.request.name = {flight.name, flight.request.name.bytes};
    owner.flight = {};
    const u64 now = clock_monotonic_milliseconds();
    if (actor_current(flight.source) && (response.rate_limited || response.status_code == 429)) {
        owner.actor_floor = maximum(owner.actor_floor, maximum(response.retry_at_ms,
            deadline_after(now, maximum(response.rate.retry_after_ms, u64{50}))));
        if (flight.request.operation == Operation::from_message) ledger_remove(owner, flight.source.message);
    }
    const bool same = !flight.retired && !owner.retired && flight.serial == owner.serial &&
        owner.phase != Phase::settled && source_current(owner);
    if (!same) {
        if (owner.release_pending) free_owner(); else application_thread_creation_synchronize();
        return true;
    }
    // A fresh Core thread may arrive before either success or failure.
    // The transport outcome cannot roll back that Gateway update.
    if (known_thread(owner.source)) { settle(owner); return true; }
    if (response.status_code == 401 || response.error == RestClientError::account_restricted ||
        api_rejection_requires_action(response.rejection) || g_app.account.status.required_action != AccountRequirement::none) {
        fail(owner, response.status_code == 401 ? utf8_literal("Your session expired. Sign in again.") :
            utf8_literal("Verify your account to create threads."),
            flight.request.operation == Operation::from_message, true);
        return true;
    }
    if (response.rate_limited || response.status_code == 429) {
        fail(owner, utf8_literal("You're creating threads too fast. Try again in a moment."), owner.uncertain);
        return true;
    }
    if (response.status_code == 403) {
        fail(owner, utf8_literal("You no longer have permission to create this thread."), owner.uncertain, true);
        return true;
    }
    if (flight.request.operation == Operation::reconcile && response.transport_ok() && response.status_code == 404) {
        const auto* message = message_store_resolve(*owner.source.store, owner.source.handle);
        if (flight.gateway_serial != owner.gateway_serial || !message || (message->flags & has_thread)) {
            fail(owner, utf8_literal("The thread changed. Check again."), true);
        } else {
            ledger_remove(owner, owner.source.message);
            owner.uncertain = false;
            queue(owner, Operation::from_message);
        }
        return true;
    }
    const bool success = response.transport_ok() && response.status_code >= 200 && response.status_code < 300 &&
        body.data && body.bytes == response.body_bytes && body.bytes <= kThreadCreationMaximumResponseBytes &&
        thread_creation_decode(body, flight.request, flight.source.self,
            {g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount}, &owner.candidate).ok();
    if (success) {
        if (flight.gateway_serial != owner.gateway_serial) {
            fail(owner, utf8_literal("Thread created, but it changed. Check again."), true);
            return true;
        }
        ChannelInput input{};
        auto& state = g_app.account_ui->chat_state;
        if (receipt_input(owner.candidate, &input) &&
            chat_state_store_preflight_channel(state, input) == ChatStateStoreError::none &&
            chat_state_store_upsert_channel(&state, input).ok()) {
            chat_state_store_recompute_guild_permissions(&state, owner.source.guild);
            if (settle(owner)) return true;
        }
        fail(owner, utf8_literal("Thread created, but couldn't be opened. Check again."), true);
    } else if (response.transport_ok() && response.rejection.has_code && response.rejection.code == 160004) {
        fail(owner, utf8_literal("A thread already exists for this message."), true);
    } else if (response.transport_ok() && response.status_code == 400 && response.rejection.has_code && response.rejection.code == 50035) {
        fail(owner, utf8_literal("Couldn't create the thread. Try a different name."), false, false, true);
    } else {
        const bool uncertain = owner.uncertain || flight.request.operation == Operation::from_message;
        fail(owner, uncertain ? utf8_literal("Your thread may already exist. Check before trying again.") :
            utf8_literal("Couldn't check the thread. Try again."), uncertain);
    }
    return true;
}
u64 application_thread_creation_next_wake() noexcept {
    return creation_owner && !creation_owner->retired && !creation_owner->flight.active && queued(*creation_owner) &&
        source_current(*creation_owner) && g_app.account.status.running &&
        g_app.account.status.required_action == AccountRequirement::none
        ? maximum(u64{1}, maximum(creation_owner->due, creation_owner->actor_floor)) : 0;
}
void application_thread_creation_gateway(u64 guild, u64 parent, u64 thread) noexcept {
    if (!creation_owner) return;
    auto& owner = *creation_owner;
    if ((!guild || guild == owner.source.guild) && (!parent || parent == owner.source.parent) &&
        (!thread || thread == owner.source.message)) {
        ++owner.gateway_serial;
        application_thread_creation_synchronize();
    }
}
void application_thread_creation_release() noexcept {
    if (!creation_owner) return;
    retire(*creation_owner);
    creation_owner->release_pending = true;
    if (!creation_owner->flight.active || !g_app.account.instance || (!g_app.account.status.running && !g_app.account.stop_failed)) free_owner();
}
ApplicationThreadCreationMetrics application_thread_creation_metrics() noexcept {
    if (!creation_owner) return {};
    return {sizeof(CreationOwner), (sizeof(CreationOwner) + 4095u) & ~4095u,
        creation_owner->publication, creation_owner->flight.active};
}
} // namespace maicord::app_detail
