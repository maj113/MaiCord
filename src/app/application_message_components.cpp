#include "application_message_components.hpp"
#include "application_account_presentation.hpp"
#include "application_composer.hpp"
#include "application_content_input.hpp"
#include "application_message_interaction.hpp"
#include "application_media_context.hpp"
#include "application_search.hpp"
#include "application_state.hpp"
#include "application_text_form.hpp"
#include "application_text_form_state.hpp"
#include "application_friends.hpp"
#include "application_user_settings.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/protocol/message_interaction.hpp"
#include "maicord/ui/message_component_layout.hpp"
#include "maicord/ui/popup_scene.hpp"

#include <Windows.h>

namespace maicord::app_detail {
namespace {

constexpr u32 kSelectActionBase = 0x6D430000u;
constexpr u32 kSelectDoneAction = kSelectActionBase + 0xFFFFu;
constexpr u32 kSelectMaximumChoices = 25;
constexpr u32 kSelectLabelBytes = 4096;
// The server fails an interaction the bot does not acknowledge within three
// seconds; a missing lifecycle event must not leave a control loading.
constexpr u64 kAwaitTimeoutMs = 15'000;
constexpr u64 kBusyRetryMs = 250;
constexpr u64 kModalTextFormCollection = 0x4D4F44414Cull; // "MODAL"

enum class Phase : u8 { idle, queued, sending, awaiting };

struct SelectChoice {
    Utf8View label;
    Utf8View value;
    bool chosen;
};

struct Owner {
    MessageInteractionRequest request;
    u64 account_scope;
    u64 message_id;
    u64 channel_id;
    u64 nonce;
    u64 interaction_id;
    u64 deadline_ms;
    u64 retry_at_ms;
    u64 request_id;
    u64 nonce_serial;
    u16 component_index;
    Phase phase;
    bool failed;
    // Open select menu.
    u64 select_message_id;
    u32 select_revision;
    u16 select_index;
    u16 select_type;
    u8 select_minimum;
    u8 select_maximum;
    u32 choice_count;
    SelectChoice choices[kSelectMaximumChoices];
    char labels[kSelectLabelBytes];
    u32 label_bytes;
    // Modal opened by an interaction (one sequential text form per field).
    MessageModal modal;
    char modal_pool[kMessageModalPoolBytes];
    char values[kMessageModalMaximumFields][kApplicationTextFormBytes];
    u32 value_bytes[kMessageModalMaximumFields];
    char context[2048];
    char step_title[512];
    u64 modal_serial;
    u64 modal_message_id;
    u64 modal_guild_id;
    u32 modal_step;
    Utf8View modal_status;
    bool modal_active;
    bool form_open;
    bool step_saved;
    bool advance;
};
Owner* owner{};

u64 account_scope() noexcept {
    return g_app.plugin_account_scope_serial ? g_app.plugin_account_scope_serial : g_app.account.status.profile_id;
}

bool ensure_owner() noexcept {
    if (owner)
        return true;
    owner = static_cast<Owner*>(VirtualAlloc(nullptr, sizeof(Owner), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    return owner != nullptr;
}

u64 next_nonce() noexcept {
    const u64 stamp = (clock_unix_milliseconds() - 1420070400000ull) << 22;
    owner->nonce_serial = maximum(owner->nonce_serial + 1, stamp);
    return owner->nonce_serial;
}

void publish_feedback() noexcept {
    const bool pending = owner && owner->phase != Phase::idle && !owner->modal_active;
    MessageSceneInteraction value{};
    if (owner && (pending || owner->failed))
        value = {owner->message_id, owner->component_index, pending, owner->failed && !pending};
    if (message_scene_set_interaction(&g_app.message_scene, value)) {
        refresh_account_text(true);
        application_request_frame();
    }
}

void finish(bool failed) noexcept {
    if (!owner)
        return;
    owner->phase = Phase::idle;
    owner->failed = failed;
    owner->deadline_ms = 0;
    owner->request_id = 0;
    publish_feedback();
}

struct Resolved {
    const MessageRecord* record;
    const MessageRichRecord* rich;
    const ChannelRecord* channel;
};

bool resolve_message(u64 message_id, Resolved* output) noexcept {
    if (!g_app.account_ui || !message_id)
        return false;
    const auto found = message_store_find(g_app.message_view.store, message_id);
    const auto* record = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    const auto* rich = record ? message_rich_store_find(g_app.account_ui->message_rich_store, message_id) : nullptr;
    if (!record || !rich || rich->message_revision != record->revision ||
        record->channel_id != g_app.account_ui->active_channel_id)
        return false;
    *output = {record, rich,
               chat_state_store_find_channel(g_app.account_ui->chat_state, record->channel_id)};
    return true;
}

// Builds and queues a component interaction (one in flight at a time).
bool queue_component(const Resolved& message, u16 index, ConstSlice<Utf8View> values) noexcept {
    if (!ensure_owner())
        return false;
    if (owner->phase != Phase::idle || owner->modal_active)
        return true; // The official client likewise ignores clicks while one is pending.
    const auto& store = g_app.account_ui->message_rich_store;
    const auto& component = message.rich->components[index];
    auto& request = owner->request;
    request = {};
    ProtocolWriter data{{reinterpret_cast<u8*>(request.data), sizeof(request.data)}, 0};
    if (!message_interaction_write_component_data(component.type,
                                                  message_rich_store_string(store, component.custom_id), values,
                                                  &data))
        return false;
    // The author of a component message is the application (bots and
    // interaction webhooks alike).
    request.application_id = message.record->author_id;
    request.guild_id = message.channel ? message.channel->guild_id : 0;
    request.channel_id = message.record->channel_id;
    request.message_id = message.record->id;
    request.message_flags = message.record->flags;
    request.nonce = next_nonce();
    request.data_bytes = data.used;
    request.type = MessageInteractionType::message_component;
    if (!message_interaction_request_valid(request))
        return false;
    owner->account_scope = account_scope();
    owner->message_id = message.record->id;
    owner->channel_id = message.record->channel_id;
    owner->nonce = request.nonce;
    owner->interaction_id = 0;
    owner->component_index = index;
    owner->failed = false;
    owner->phase = Phase::queued;
    owner->retry_at_ms = 0;
    publish_feedback();
    return true;
}

Utf8View retain_label(Utf8View text) noexcept {
    if (text.bytes > kSelectLabelBytes - owner->label_bytes)
        text.bytes = kSelectLabelBytes - owner->label_bytes;
    char* at = owner->labels + owner->label_bytes;
    copy_bytes(at, text.data, text.bytes);
    owner->label_bytes += text.bytes;
    return {at, text.bytes};
}

Utf8View retain_number(u64 value) noexcept {
    char digits[20];
    return retain_label({digits, decimal_write(digits, value)});
}

void add_choice(Utf8View label, Utf8View value, bool chosen) noexcept {
    if (owner->choice_count >= kSelectMaximumChoices || !value.bytes)
        return;
    owner->choices[owner->choice_count++] = {retain_label(label.bytes ? label : value), retain_label(value), chosen};
}

// String selects list their options; auto-populated selects list resident
// channels, roles or recent authors of the open channel.
void collect_choices(const Resolved& message, u16 index) noexcept {
    const auto& store = g_app.account_ui->message_rich_store;
    const auto& state = g_app.account_ui->chat_state;
    const auto& component = message.rich->components[index];
    owner->choice_count = 0;
    owner->label_bytes = 0;
    if (component.type == gateway_component_string_select) {
        for (u32 i = index + 1u; i < message.rich->component_count; ++i) {
            const auto& option = message.rich->components[i];
            if (option.parent != index || option.type != gateway_component_select_option)
                continue;
            add_choice(message_rich_store_string(store, option.label), message_rich_store_string(store, option.custom_id),
                       (option.flags & gateway_message_component_default) != 0);
        }
        return;
    }
    const u64 guild = message.channel ? message.channel->guild_id : 0;
    if (component.type == gateway_component_channel_select) {
        const u32 slots = chat_state_store_channel_slot_bound(state);
        for (u32 i = 0; i < slots && owner->choice_count < kSelectMaximumChoices; ++i) {
            const ChannelRecord& channel = state.channels[i];
            if (!channel.occupied || channel.guild_id != guild || !guild || !channel_is_message_capable(channel.type))
                continue;
            add_choice(chat_state_store_channel_name(state, channel), retain_number(channel.id), false);
        }
        return;
    }
    if (component.type == gateway_component_role_select || component.type == gateway_component_mentionable_select) {
        for (u32 i = 0; i < state.role_capacity && owner->choice_count < kSelectMaximumChoices; ++i) {
            const RoleRecord& role = state.roles[i];
            if (!role.id || role.guild_id != guild || !guild || role.id == guild || role.managed)
                continue;
            add_choice(chat_state_store_role_name(state, role), retain_number(role.id), false);
        }
    }
    if (component.type == gateway_component_user_select || component.type == gateway_component_mentionable_select) {
        // Recent distinct authors of the open channel, newest first.
        const auto& messages = g_app.message_view.store;
        for (u32 i = messages.count; i != 0 && owner->choice_count < kSelectMaximumChoices; --i) {
            const auto* record = message_store_resolve(messages, message_store_ordered_handle(messages, i - 1u));
            if (!record || !record->author_id || record->channel_id != message.record->channel_id)
                continue;
            bool seen = false;
            for (u32 c = 0; c < owner->choice_count && !seen; ++c) {
                u64 id = 0;
                for (u32 b = 0; b < owner->choices[c].value.bytes; ++b)
                    id = id * 10u + static_cast<u64>(owner->choices[c].value.data[b] - '0');
                seen = id == record->author_id;
            }
            if (seen)
                continue;
            const auto* user = user_store_find(g_app.account_ui->user_store, record->author_id);
            add_choice(user ? user_store_name(g_app.account_ui->user_store, *user) : Utf8View{},
                       retain_number(record->author_id), false);
        }
    }
}

bool open_select(const Resolved& message, u16 index, UiRect anchor) noexcept {
    if (!ensure_owner())
        return false;
    const auto& component = message.rich->components[index];
    owner->select_message_id = message.record->id;
    owner->select_revision = message.record->revision;
    owner->select_index = index;
    owner->select_type = component.type;
    owner->select_minimum = component.min_values;
    owner->select_maximum = maximum<u8>(1, component.max_values);
    collect_choices(message, index);
    PopupItem items[kSelectMaximumChoices + 1]{};
    u32 count = 0;
    const bool multiple = owner->select_maximum > 1;
    for (u32 i = 0; i < owner->choice_count; ++i)
        items[count++] = {owner->choices[i].label, kSelectActionBase + i,
                          static_cast<u16>(popup_item_enabled | (owner->choices[i].chosen && multiple ? popup_item_checked : 0))};
    if (!count)
        items[count++] = {utf8_literal("No options available"), 0, popup_item_none};
    else if (multiple)
        items[count++] = {utf8_literal("Done"), kSelectDoneAction,
                          static_cast<u16>(popup_item_enabled | popup_item_separator_before)};
    const float width = maximum(200.0f, ui_rect_width(anchor));
    if (!popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, message.record->id, {items, count}, anchor,
                          application_viewport_dips(), width)
             .ok())
        return false;
    set_popup_owner(PopupOwnerKind::message_components);
    return refresh_popup_text();
}

bool submit_select() noexcept {
    Resolved message{};
    if (!owner || !resolve_message(owner->select_message_id, &message) ||
        message.record->revision != owner->select_revision || owner->select_index >= message.rich->component_count ||
        message.rich->components[owner->select_index].type != owner->select_type)
        return dismiss_popup();
    Utf8View values[kSelectMaximumChoices];
    u32 count = 0;
    for (u32 i = 0; i < owner->choice_count && count < owner->select_maximum; ++i)
        if (owner->choices[i].chosen)
            values[count++] = owner->choices[i].value;
    if (count < owner->select_minimum)
        return true; // keep the menu open until enough values are chosen
    dismiss_popup();
    return queue_component(message, owner->select_index, {values, count});
}

// Modal flow: each field is one native text form, then a type 5 submit.
bool modal_valid(TextFormOwnerKey key) noexcept {
    return owner && owner->modal_active && key.scope == owner->account_scope && key.item == owner->modal_serial &&
           account_scope() == owner->account_scope;
}
bool modal_save(TextFormOwnerKey key, Utf8View text) noexcept {
    if (!modal_valid(key) || owner->modal_step >= owner->modal.field_count)
        return false;
    const auto& field = owner->modal.fields[owner->modal_step];
    Utf8View reason{};
    if (!message_modal_value_valid(field, text, &reason)) {
        owner->modal_status = reason;
        return false;
    }
    copy_bytes(owner->values[owner->modal_step], text.data, text.bytes);
    owner->value_bytes[owner->modal_step] = text.bytes;
    owner->modal_status = {};
    owner->step_saved = true;
    return true;
}
ApplicationTextFormProgress modal_progress(TextFormOwnerKey key) noexcept {
    ApplicationTextFormProgress progress{};
    if (!modal_valid(key))
        return progress;
    progress.status = owner->modal_status;
    progress.accept = owner->modal_step + 1u < owner->modal.field_count ? utf8_literal("Next") : utf8_literal("Submit");
    progress.revision = owner->modal_status.bytes;
    return progress;
}
void modal_closed(TextFormOwnerKey key) noexcept {
    if (!owner || key.item != owner->modal_serial)
        return;
    owner->form_open = false;
    if (owner->step_saved) {
        owner->step_saved = false;
        owner->advance = true; // continued from the tick, outside the form's teardown
    } else {
        owner->modal_active = false; // cancelled
    }
}

u32 append_context(u32 at, Utf8View text) noexcept {
    const u32 bytes = minimum<u32>(text.bytes, sizeof(owner->context) - at);
    copy_bytes(owner->context + at, text.data, bytes);
    return at + bytes;
}

bool open_modal_step() noexcept {
    const auto& modal = owner->modal;
    const auto& field = modal.fields[owner->modal_step];
    u32 context = 0;
    if (owner->modal_step == 0)
        for (u32 i = 0; i < modal.paragraph_count; ++i) {
            context = append_context(context, modal.paragraphs[i]);
            context = append_context(context, utf8_literal("\n"));
        }
    if (field.description.bytes) {
        context = append_context(context, field.description);
        context = append_context(context, utf8_literal("\n"));
    }
    if (field.placeholder.bytes && !field.value.bytes)
        context = append_context(context, field.placeholder);
    while (context && owner->context[context - 1] == '\n')
        --context;
    u32 title = 0;
    const Utf8View base = modal.title.bytes ? modal.title : utf8_literal("Form");
    title = minimum<u32>(base.bytes, sizeof(owner->step_title) - 16u);
    copy_bytes(owner->step_title, base.data, title);
    if (modal.field_count > 1) {
        char suffix[16] = {' ', '(', static_cast<char>('1' + owner->modal_step), ' ', 'o', 'f', ' ',
                           static_cast<char>('0' + modal.field_count), ')'};
        copy_bytes(owner->step_title + title, suffix, 9);
        title += 9;
    }
    ApplicationTextFormRequest request{};
    request.title = {owner->step_title, title};
    request.context = {owner->context, context};
    request.label = field.label.bytes ? field.label : utf8_literal("Value");
    request.accept = owner->modal_step + 1u < modal.field_count ? utf8_literal("Next") : utf8_literal("Submit");
    request.initial = field.value;
    request.owner = {owner->account_scope, kModalTextFormCollection, owner->modal_serial};
    request.valid = modal_valid;
    request.save = modal_save;
    request.progress = modal_progress;
    request.closed = modal_closed;
    request.maximum_codepoints = field.max_length ? field.max_length : 4000u;
    request.maximum_bytes = minimum<u32>(kApplicationTextFormBytes, request.maximum_codepoints * 4u);
    request.single_line = field.style == MessageModalInputStyle::short_text;
    owner->modal_status = {};
    owner->form_open = application_text_form_open(request);
    if (!owner->form_open)
        owner->modal_active = false;
    return owner->form_open;
}

bool queue_modal_submit() noexcept {
    auto& request = owner->request;
    request = {};
    Utf8View values[kMessageModalMaximumFields];
    for (u32 i = 0; i < owner->modal.field_count; ++i)
        values[i] = {owner->values[i], owner->value_bytes[i]};
    ProtocolWriter data{{reinterpret_cast<u8*>(request.data), sizeof(request.data)}, 0};
    if (!message_modal_write_submit_data(owner->modal, {values, owner->modal.field_count}, &data))
        return false;
    request.application_id = owner->modal.application_id;
    request.guild_id = owner->modal_guild_id;
    request.channel_id = owner->modal.channel_id;
    request.nonce = next_nonce();
    request.data_bytes = data.used;
    request.type = MessageInteractionType::modal_submit;
    if (!message_interaction_request_valid(request))
        return false;
    owner->nonce = request.nonce;
    owner->message_id = owner->modal_message_id;
    owner->interaction_id = 0;
    owner->failed = false;
    owner->phase = Phase::queued;
    owner->retry_at_ms = 0;
    publish_feedback();
    return true;
}

void open_modal(Utf8View body) noexcept {
    if (!ensure_owner() || owner->modal_active || g_app.text_form || !g_app.account_ui)
        return;
    MessageModal modal{};
    if (!message_modal_decode(body,
                              {g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount},
                              {owner->modal_pool, sizeof(owner->modal_pool)}, &modal))
        return;
    // Only modals answering this session's interaction are shown.
    if (!modal.nonce || modal.nonce != owner->nonce || !modal.channel_id)
        return;
    if (!modal.application_id)
        modal.application_id = owner->request.application_id;
    owner->modal = modal;
    owner->modal_message_id = owner->message_id;
    const auto* channel = chat_state_store_find_channel(g_app.account_ui->chat_state, modal.channel_id);
    owner->modal_guild_id = channel ? channel->guild_id : 0;
    for (u32 i = 0; i < kMessageModalMaximumFields; ++i)
        owner->value_bytes[i] = 0;
    owner->phase = Phase::idle;
    owner->deadline_ms = 0;
    publish_feedback();
    if (modal.unsupported) {
        // Selects and uploads in modals are not supported natively yet.
        owner->failed = true;
        publish_feedback();
        return;
    }
    owner->modal_serial = next_nonce();
    owner->modal_step = 0;
    owner->modal_active = true;
    owner->advance = false;
    open_modal_step();
}

struct MediaStoreVersion {
    const MessageStore* store;
    const MessageRecord* records;
    u32 created, updated, deleted, count;
};
MediaStoreVersion media_store_version(const MessageStore* store) noexcept {
    return store ? MediaStoreVersion{store, store->records, store->created, store->updated, store->deleted, store->count}
                 : MediaStoreVersion{};
}
bool same_media_store(const MediaStoreVersion& a, const MediaStoreVersion& b) noexcept {
    return a.store == b.store && a.records == b.records && a.created == b.created && a.updated == b.updated &&
           a.deleted == b.deleted && a.count == b.count;
}
struct MediaPolicyInputs {
    const ChatStateStore* channels;
    const RelationshipStore* relationships;
    MediaStoreVersion live, search;
    u64 account_scope, self_user_id;
    u32 channel_revision, relationship_revision, live_generation, search_revision;
    AccountContentPolicy content;
    MessageExplicitFilter filters[3];
    bool reveal_allowed;
};
MediaPolicyInputs media_policy_inputs() noexcept {
    MediaPolicyInputs inputs{};
    inputs.account_scope = account_scope();
    inputs.channels = g_app.account_ui ? &g_app.account_ui->chat_state : nullptr;
    if (inputs.channels) {
        inputs.self_user_id = inputs.channels->self_user_id;
        inputs.content = inputs.channels->content_policy;
        inputs.channel_revision = inputs.channels->channel_mutations;
        inputs.live = media_store_version(&g_app.message_view.store);
        inputs.live_generation = g_app.message_view.timeline.reset_generation;
    }
    inputs.relationships = application_friends_relationships();
    inputs.relationship_revision = inputs.relationships ? inputs.relationships->revision : 0;
    if (const auto* timeline = application_search_rich_timeline()) {
        inputs.search = media_store_version(timeline->store);
        const auto* scene = application_search_scene();
        inputs.search_revision = scene && scene->source ? scene->source->revision : 0;
    }
    for (u32 i = 0; i < 3; ++i)
        inputs.filters[i] = application_user_settings_explicit_filter(static_cast<MessageMediaContext>(i));
    inputs.reveal_allowed = !application_user_settings_content_confirmation_required();
    return inputs;
}
bool same_media_sources(const MediaPolicyInputs& a, const MediaPolicyInputs& b) noexcept {
    return a.channels == b.channels && a.relationships == b.relationships &&
           a.channel_revision == b.channel_revision && a.relationship_revision == b.relationship_revision &&
           a.live_generation == b.live_generation && a.search_revision == b.search_revision &&
           same_media_store(a.live, b.live) && same_media_store(a.search, b.search);
}
bool same_media_owner(const MessageMediaOwner& a, const MessageMediaOwner& b) noexcept {
    return a.channel_id == b.channel_id && a.author_id == b.author_id &&
           a.source_channel_id == b.source_channel_id && a.source_message_id == b.source_message_id &&
           a.forwarded == b.forwarded;
}
bool same_media_context(const ApplicationMediaContext& a, const ApplicationMediaContext& b) noexcept {
    return a.author_id == b.author_id && a.context == b.context && a.self_authored == b.self_authored;
}
ApplicationMediaContext resolve_media_context(const MediaPolicyInputs& inputs, const MessageMediaOwner& media) noexcept {
    return application_media_context_resolve(media, inputs.self_user_id, inputs.channels, inputs.relationships,
                                             inputs.live.store, inputs.search.store);
}
// Only queried owners can affect painted media. Store counters admit a bounded
// recheck; unrelated text edits, presence updates and channel metadata changes
// do not invalidate all message geometry when those owners resolve identically.
inline constexpr u32 kMediaPolicyDependencies = 128;
struct MediaPolicyDependency {
    MessageMediaOwner owner;
    ApplicationMediaContext resolved;
};
struct MediaPolicyOwner {
    MediaPolicyInputs inputs;
    MediaPolicyDependency dependencies[kMediaPolicyDependencies];
    u64 revision;
    u32 count;
    bool initialized, overflow;
};
MediaPolicyOwner media_policy{};

MessageMediaPolicyResult query_media_policy(const void*, const MessageMediaOwner& media) noexcept {
    // The callback borrows application state; source records are never retained.
    auto& policy = media_policy;
    const auto* search_timeline = application_search_rich_timeline();
    const auto resolved = application_media_context_resolve(
        media, g_app.account_ui ? g_app.account_ui->chat_state.self_user_id : 0,
        g_app.account_ui ? &g_app.account_ui->chat_state : nullptr, application_friends_relationships(),
        g_app.account_ui ? &g_app.message_view.store : nullptr,
        search_timeline ? search_timeline->store : nullptr);
    u32 dependency = 0;
    for (; dependency < policy.count; ++dependency)
        if (same_media_owner(policy.dependencies[dependency].owner, media))
            break;
    if (dependency == policy.count && dependency < kMediaPolicyDependencies) {
        policy.dependencies[dependency] = {media, resolved};
        ++policy.count;
    } else if (dependency == kMediaPolicyDependencies) {
        // The next admitted source change forces a rebuild and starts a fresh
        // bounded set, so a long scrolling session cannot pin this fallback.
        policy.overflow = true;
    }
    return {resolved.self_authored ? MessageExplicitFilter::show
                                  : application_user_settings_explicit_filter(resolved.context),
            !application_user_settings_content_confirmation_required()};
}

// Age gate: accepted servers (and DM-less channel ids) for this session.
constexpr u32 kAgeGateAccepted = 32;
constexpr u64 kAgeGateCollection = 0x4147455F47415445ull; // "AGE_GATE"
struct AgeGate {
    u64 account_scope;
    u64 accepted[kAgeGateAccepted];
    u32 accepted_count;
    u64 prompted_channel; // the channel whose prompt was shown this visit
    u64 serial;
};
AgeGate age_gate{};

u64 age_gate_key(const ChannelRecord& channel) noexcept {
    return channel.guild_id ? channel.guild_id : channel.id;
}
bool age_gate_accepted(u64 key) noexcept {
    for (u32 i = 0; i < age_gate.accepted_count; ++i)
        if (age_gate.accepted[i] == key)
            return true;
    return false;
}
const ChannelRecord* age_gate_channel() noexcept {
    if (!g_app.account_ui || !g_app.account_ui->active_channel_id)
        return nullptr;
    const auto* channel =
        chat_state_store_find_channel(g_app.account_ui->chat_state, g_app.account_ui->active_channel_id);
    // Threads inherit their parent's age restriction.
    const auto* restricted = channel && channel->parent_id && !(channel->content_policy & kChannelContentAgeRestricted)
                                 ? chat_state_store_find_channel(g_app.account_ui->chat_state, channel->parent_id)
                                 : channel;
    return restricted && (restricted->content_policy & kChannelContentAgeRestricted) ? channel : nullptr;
}
bool age_gate_valid(TextFormOwnerKey key) noexcept {
    const auto* channel = age_gate_channel();
    return channel && key.scope == age_gate.account_scope && key.item == age_gate.serial &&
           key.collection == kAgeGateCollection && channel->id == age_gate.prompted_channel;
}
bool age_gate_save(TextFormOwnerKey key, Utf8View) noexcept {
    const auto* channel = age_gate_valid(key) ? age_gate_channel() : nullptr;
    if (!channel)
        return false;
    const u64 gate = age_gate_key(*channel);
    if (!age_gate_accepted(gate)) {
        age_gate.accepted[age_gate.accepted_count % kAgeGateAccepted] = gate;
        age_gate.accepted_count = minimum(age_gate.accepted_count + 1u, kAgeGateAccepted);
    }
    if (message_scene_set_content_gated(&g_app.message_scene, false)) {
        refresh_account_text(true);
        application_request_frame();
    }
    return true;
}

} // namespace

MessageMediaPolicy application_message_media_policy() noexcept {
    const auto inputs = media_policy_inputs();
    auto& policy = media_policy;
    const auto& previous = policy.inputs;
    const bool scope_changed = !policy.initialized || inputs.account_scope != previous.account_scope ||
                               inputs.self_user_id != previous.self_user_id;
    bool changed = scope_changed || inputs.content.eligibility != previous.content.eligibility ||
                   inputs.content.age_verification != previous.content.age_verification ||
                   inputs.content.flags != previous.content.flags || inputs.reveal_allowed != previous.reveal_allowed;
    for (u32 i = 0; i < 3; ++i)
        changed |= inputs.filters[i] != previous.filters[i];
    if (scope_changed) {
        policy.count = 0;
        policy.overflow = false;
    } else if (!same_media_sources(inputs, previous)) {
        if (policy.overflow) {
            changed = true;
            policy.count = 0;
            policy.overflow = false;
        }
        for (u32 i = 0; i < policy.count; ++i) {
            auto& dependency = policy.dependencies[i];
            const auto resolved = resolve_media_context(inputs, dependency.owner);
            changed |= !same_media_context(resolved, dependency.resolved);
            dependency.resolved = resolved;
        }
    }
    policy.inputs = inputs;
    policy.initialized = true;
    if (changed)
        ++policy.revision;
    return {&policy, query_media_policy, policy.revision};
}

void application_age_gate_synchronize() noexcept {
    if (!g_app.account_ui)
        return;
    if (age_gate.account_scope != account_scope()) {
        age_gate = {};
        age_gate.account_scope = account_scope();
    }
    const auto* channel = age_gate_channel();
    const bool gated = channel && !age_gate_accepted(age_gate_key(*channel));
    if (message_scene_set_content_gated(&g_app.message_scene, gated)) {
        refresh_account_text(true);
        application_request_frame();
    }
    if (!gated) {
        age_gate.prompted_channel = 0;
        return;
    }
    if (age_gate.prompted_channel == channel->id || g_app.text_form)
        return;
    age_gate.prompted_channel = channel->id;
    age_gate.serial = clock_monotonic_milliseconds() | 1u;
    ApplicationTextFormRequest request{};
    request.title = utf8_literal("Age-restricted channel");
    request.context = utf8_literal("This channel may contain content that isn't suitable for everyone. Continue only "
                                   "if you're old enough to view it.");
    request.accept = utf8_literal("Continue");
    request.owner = {age_gate.account_scope, kAgeGateCollection, age_gate.serial};
    request.valid = age_gate_valid;
    request.save = age_gate_save;
    request.confirmation_only = true;
    request.accept_focused = true;
    application_text_form_open(request);
}

bool application_message_component_click(u64 message_id, UiWidgetHandle widget, UiRect anchor) noexcept {
    Resolved message{};
    if (!resolve_message(message_id, &message))
        return false;
    const auto* row = [&]() -> const MessageSceneRowWidget* {
        for (u32 i = 0; i < g_app.message_scene.count; ++i)
            if (g_app.message_scene.rows[i].message_id == message_id)
                return &g_app.message_scene.rows[i];
        return nullptr;
    }();
    if (!row)
        return false;
    for (u32 index = 0; index < message.rich->component_count && index < row->component_item_count; ++index) {
        const auto item = message_scene_rich_item_widget(*row, MessageSceneRichItemKind::component, index);
        if (!item.generation || item.slot != widget.slot || item.generation != widget.generation)
            continue;
        const auto& component = message.rich->components[index];
        // Inert components leave the click to text (links in text displays).
        if (!message_component_interactive(component))
            return false;
        if (message_component_is_link(component))
            return application_open_content_url(
                message_rich_store_string(g_app.account_ui->message_rich_store, component.url));
        if (gateway_component_is_select(component.type))
            return open_select(message, static_cast<u16>(index), anchor);
        return queue_component(message, static_cast<u16>(index), {});
    }
    return false;
}

bool application_message_components_popup_action(u32 action) noexcept {
    if (!owner || action < kSelectActionBase)
        return dismiss_popup();
    if (action == kSelectDoneAction)
        return submit_select();
    const u32 choice = action - kSelectActionBase;
    if (choice >= owner->choice_count)
        return dismiss_popup();
    if (owner->select_maximum <= 1) {
        for (u32 i = 0; i < owner->choice_count; ++i)
            owner->choices[i].chosen = i == choice;
        return submit_select();
    }
    auto& selected = owner->choices[choice];
    u32 chosen = 0;
    for (u32 i = 0; i < owner->choice_count; ++i)
        chosen += owner->choices[i].chosen ? 1u : 0u;
    if (selected.chosen || chosen < owner->select_maximum)
        selected.chosen = !selected.chosen;
    // Rebuild the menu with the updated checks.
    Resolved message{};
    if (!resolve_message(owner->select_message_id, &message))
        return dismiss_popup();
    PopupItem items[kSelectMaximumChoices + 1]{};
    u32 count = 0;
    for (u32 i = 0; i < owner->choice_count; ++i)
        items[count++] = {owner->choices[i].label, kSelectActionBase + i,
                          static_cast<u16>(popup_item_enabled | (owner->choices[i].chosen ? popup_item_checked : 0))};
    items[count++] = {utf8_literal("Done"), kSelectDoneAction,
                      static_cast<u16>(popup_item_enabled | popup_item_separator_before)};
    const UiRect anchor = g_app.popup_scene.anchor;
    if (!popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, owner->select_message_id, {items, count},
                          anchor, application_viewport_dips(), ui_rect_width(g_app.popup_scene.rectangle))
             .ok())
        return dismiss_popup();
    set_popup_owner(PopupOwnerKind::message_components);
    return refresh_popup_text();
}

void application_message_components_gateway(GatewayClientDispatchKind kind, Utf8View body) noexcept {
    if (!owner || !g_app.account_ui || account_scope() != owner->account_scope)
        return;
    if (kind == GatewayClientDispatchKind::interaction_modal_create) {
        open_modal(body);
        return;
    }
    MessageInteractionEvent event{};
    if (!message_interaction_event_decode(
            body, {g_app.account_ui->message_history_page_structurals, kMessageHistoryPageStructuralCount}, &event))
        return;
    const bool ours = (event.nonce && event.nonce == owner->nonce) ||
                      (owner->interaction_id && event.id == owner->interaction_id);
    if (!ours || owner->phase == Phase::idle)
        return;
    if (kind == GatewayClientDispatchKind::interaction_create)
        owner->interaction_id = event.id;
    else if (kind == GatewayClientDispatchKind::interaction_success)
        finish(false);
    else if (kind == GatewayClientDispatchKind::interaction_failure)
        finish(true);
}

void application_message_components_message(u64 message_id) noexcept {
    // An updated component message (deferred update) ends the loading state.
    if (owner && owner->phase == Phase::awaiting && owner->message_id == message_id)
        finish(false);
}

bool application_message_components_pending(u64 now_ms) noexcept {
    return owner && owner->phase == Phase::queued && owner->retry_at_ms <= now_ms && g_app.account_ui &&
           g_app.account.api.message_interaction && account_scope() == owner->account_scope;
}

bool application_message_components_submit(u64 request_id, u64 now_ms) noexcept {
    if (!application_message_components_pending(now_ms))
        return false;
    const auto submitted =
        g_app.account.api.message_interaction(g_app.account.instance, &owner->request, request_id);
    if (submitted.error == RestClientSubmitError::busy) {
        owner->retry_at_ms = now_ms + kBusyRetryMs;
        return false;
    }
    if (!submitted.ok()) {
        log_u64(LogLevel::warning, "components", "interaction_refused", static_cast<u64>(submitted.error));
        finish(true);
        return false;
    }
    owner->request_id = request_id;
    owner->phase = Phase::sending;
    return true;
}

bool application_message_components_response(const RestClientResponse& response, Utf8View) noexcept {
    if (!owner || !owner->request_id || response.request_id != owner->request_id)
        return true;
    owner->request_id = 0;
    const bool ok = response.error == RestClientError::none && response.status_code >= 200 &&
                    response.status_code < 300;
    if (!ok) {
        finish(true);
        return true;
    }
    owner->phase = Phase::awaiting;
    owner->deadline_ms = clock_monotonic_milliseconds() + kAwaitTimeoutMs;
    return true;
}

void application_message_components_tick(u64 now_ms) noexcept {
    if (message_scene_set_media_policy(&g_app.message_scene, application_message_media_policy())) {
        refresh_account_text(true);
        application_request_frame();
    }
    if (!owner)
        return;
    if (account_scope() != owner->account_scope && (owner->phase != Phase::idle || owner->modal_active)) {
        owner->modal_active = false;
        finish(false);
        return;
    }
    if (owner->phase == Phase::awaiting && owner->deadline_ms && now_ms >= owner->deadline_ms)
        finish(false);
    if (owner->advance && !g_app.text_form) {
        owner->advance = false;
        if (!owner->modal_active)
            return;
        if (++owner->modal_step < owner->modal.field_count) {
            open_modal_step();
        } else {
            owner->modal_active = false;
            if (!queue_modal_submit())
                finish(true);
        }
    }
}

u64 application_message_components_next_wake() noexcept {
    if (!owner)
        return 0;
    if (owner->advance)
        return 1;
    if (owner->phase == Phase::queued)
        return maximum<u64>(1, owner->retry_at_ms);
    return owner->phase == Phase::awaiting ? owner->deadline_ms : 0;
}

void application_message_components_release() noexcept {
    media_policy.initialized = false;
    media_policy.count = 0;
    media_policy.overflow = false;
    if (!owner)
        return;
    message_scene_set_interaction(&g_app.message_scene, {});
    SecureZeroMemory(owner, sizeof(Owner));
    VirtualFree(owner, 0, MEM_RELEASE);
    owner = nullptr;
}

} // namespace maicord::app_detail
