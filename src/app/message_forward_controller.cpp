#include "maicord/app/message_forward_controller.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/message_composer.hpp"

namespace maicord {
namespace {
void changed(MessageForwardController* c) noexcept { ++c->revision; if (!c->revision) ++c->revision; }
bool fail(MessageForwardController* c, MessageForwardError error) noexcept {
    if (c) { c->error = error; changed(c); } return false;
}
u16 copy_text(char* out, u32 capacity, Utf8View text) noexcept {
    return static_cast<u16>(utf8_copy_truncated(out, capacity, text));
}
bool valid_source_channel(QuickSwitcherSources sources, const MessageForwardSource& source) noexcept {
    if (!sources.state) return false;
    const auto* channel = chat_state_store_find_channel(*sources.state, source.reference.channel_id);
    if (!channel || channel->generation != source.channel_generation || channel->guild_id != source.reference.guild_id ||
        !channel_is_message_capable(channel->type) || (channel->flags & kChannelObfuscatedFlag)) return false;
    if (!channel->guild_id) return channel->type == 1 || channel->type == 3;
    const auto* guild = chat_state_store_find_guild(*sources.state, channel->guild_id);
    return guild && !guild->unavailable && channel->permissions_resolved &&
        (channel->actions & channel_action_view) &&
        chat_state_store_channel_visible(*sources.state, *channel);
}
bool source_age_restricted(QuickSwitcherSources sources, const MessageForwardSource& source) noexcept {
    const auto* channel = sources.state ? chat_state_store_find_channel(*sources.state, source.reference.channel_id) : nullptr;
    if (channel && (channel->type == 10 || channel->type == 11 || channel->type == 12) &&
        !chat_state_store_find_channel(*sources.state, channel->parent_id)) return source.age_restricted;
    return channel && chat_state_store_channel_age_restricted(*sources.state, *channel);
}
bool eligible(QuickSwitcherSources sources, const QuickSwitcherTarget& target, bool age_restricted) noexcept {
    if (!sources.state || !target.channel_id) return false;
    const auto* channel = chat_state_store_find_channel(*sources.state, target.channel_id);
    return channel && channel->generation == target.generation && channel->guild_id == target.guild_id &&
        quick_switcher_channel_forwardable(sources, *channel, age_restricted);
}
const MessageComposerRequest* request(const MessageComposerController& outbox, u64 nonce) noexcept {
    for (const auto& r : outbox.requests) if (r.occupied && r.nonce == nonce) return &r;
    return nullptr;
}
bool pending(MessageForwardDestinationState state) noexcept {
    return state == MessageForwardDestinationState::queued || state == MessageForwardDestinationState::sending ||
        state == MessageForwardDestinationState::retry_wait;
}
bool queue_one(MessageForwardController* c, MessageForwardDestination& d, QuickSwitcherSources sources,
    MessageComposerController* outbox, u64 unix_ms, u64 monotonic_ms) noexcept {
    if (!eligible(sources, d.row.target, source_age_restricted(sources, c->source))) { d.state = MessageForwardDestinationState::unavailable; return false; }
    const auto result = d.nonce ? message_composer_controller_retry(outbox, d.nonce, monotonic_ms) :
        message_composer_controller_queue_forward(outbox, d.row.target.channel_id, {c->caption, c->caption_bytes},
            c->source.reference, c->scope.self_user_id, unix_ms, monotonic_ms);
    if (!result.ok()) { d.state = MessageForwardDestinationState::failed; return false; }
    d.nonce = result.nonce; d.http_status = 0; d.state = MessageForwardDestinationState::queued; return true;
}
} // namespace

bool message_forward_current(const MessageForwardController& c, QuickSwitcherScope scope, QuickSwitcherSources sources) noexcept {
    if (!c.initialized || !scope.account_serial || !scope.profile_key || !scope.self_user_id || !quick_switcher_scope_equal(c.scope, scope) ||
        !sources.state || sources.state->self_user_id != scope.self_user_id || !valid_source_channel(sources, c.source)) return false;
    // A loaded identity must still be the captured generation. Absence is
    // expected when another history window replaces the source timeline.
    if (c.source.store) {
        const auto found = message_store_find(*c.source.store, c.source.reference.message_id);
        if (found.ok()) {
            const auto* record = message_store_resolve(*c.source.store, c.source.handle);
            if (!record || record->id != c.source.reference.message_id || record->channel_id != c.source.reference.channel_id ||
                record->author_id != c.source.author_id || !message_forward_source_available(sources, *c.source.store, c.source.handle)) return false;
        }
    }
    return true;
}
bool message_forward_source_available(QuickSwitcherSources sources, const MessageStore& store, MessageHandle handle) noexcept {
    const auto* record = message_store_resolve(store, handle);
    if (!sources.state || !record || !record->id || !record->author_id || record->delivery != MessageDeliveryState::authoritative ||
        record->content_availability != MessageContentAvailability::available ||
        (record->type != 0 && record->type != 19 && record->type != 20 && record->type != 23) || (record->flags & (1u << 6)) ||
        (record->presentation_flags & kMessagePresentationForwardingObstacles)) return false;
    const auto* channel = chat_state_store_find_channel(*sources.state, record->channel_id);
    if (!channel) return false;
    MessageForwardSource source{}; source.reference = {record->id, record->channel_id, channel->guild_id};
    source.channel_generation = channel->generation;
    return valid_source_channel(sources, source);
}
bool message_forward_open(MessageForwardController* c, QuickSwitcherScope scope, QuickSwitcherSources sources,
    const MessageStore* store, MessageHandle handle, Utf8View author, Utf8View media_summary) noexcept {
    if (!c || !store || !sources.state || !scope.account_serial || !scope.profile_key || !scope.self_user_id ||
        sources.state->self_user_id != scope.self_user_id || !utf8_validate(author).valid || !utf8_validate(media_summary).valid)
        return fail(c, MessageForwardError::invalid_source);
    for (u32 i = 0; i < c->destination_count; ++i) {
        const auto& d = c->destinations[i];
        if (d.nonce && d.state != MessageForwardDestinationState::sent && d.state != MessageForwardDestinationState::cancelled)
            return fail(c, MessageForwardError::busy);
    }
    if (!message_forward_source_available(sources, *store, handle)) return fail(c, MessageForwardError::invalid_source);
    const auto* record = message_store_resolve(*store, handle);
    const auto* channel = chat_state_store_find_channel(*sources.state, record->channel_id);
    if (!channel) return fail(c, MessageForwardError::invalid_source);
    MessageForwardSource source{};
    source.reference = {record->id, record->channel_id, channel->guild_id};
    source.store = store; source.handle = handle; source.author_id = record->author_id; source.channel_generation = channel->generation;
    source.age_restricted = source_age_restricted(sources, source);
    if (!valid_source_channel(sources, source)) return fail(c, MessageForwardError::invalid_source);
    source.author_bytes = copy_text(source.author, sizeof(source.author), author);
    source.preview_bytes = copy_text(source.preview, sizeof(source.preview), message_store_content(*store, handle));
    source.media_summary_bytes = copy_text(source.media_summary, sizeof(source.media_summary), media_summary);
    const u32 revision = c->revision;
    reset_value(c); c->source = source; c->scope = scope; c->initialized = c->caption_valid = true; c->revision = revision;
    changed(c);
    quick_switcher_synchronize(&c->candidates, scope, sources, {}, QuickSwitcherMode::forwarding, source.age_restricted);
    return true;
}
bool message_forward_synchronize(MessageForwardController* c, QuickSwitcherScope scope, QuickSwitcherSources sources,
    Utf8View query, MessageComposerController* outbox) noexcept {
    if (!c) return false;
    if (!c->initialized) {
        if (!c->committed || !outbox || !quick_switcher_scope_equal(c->scope, scope)) return false;
        bool did_change = message_forward_refresh(c, *outbox);
        for (u32 i = 0; i < c->destination_count; ++i) {
            auto& d = c->destinations[i];
            if (d.nonce && d.state == MessageForwardDestinationState::failed &&
                message_composer_controller_discard_failed(outbox, d.nonce).ok()) {
                d.nonce = 0; d.state = MessageForwardDestinationState::cancelled; did_change = true;
            }
        }
        if (did_change) changed(c);
        return did_change;
    }
    if (!message_forward_current(*c, scope, sources)) {
        message_forward_cancel_pending(c, outbox);
        c->candidates.count = 0; c->source.store = nullptr; c->initialized = false;
        return fail(c, quick_switcher_scope_equal(c->scope, scope) ? MessageForwardError::invalid_source : MessageForwardError::stale_scope);
    }
    const bool age_restricted = source_age_restricted(sources, c->source);
    bool did_change = c->source.age_restricted != age_restricted;
    c->source.age_restricted = age_restricted;
    did_change = quick_switcher_synchronize(&c->candidates, scope, sources, query, QuickSwitcherMode::forwarding, age_restricted) || did_change;
    if (!c->committed) for (u32 i = 0; i < c->destination_count; ++i) {
        auto& d = c->destinations[i];
        const auto next = eligible(sources, d.row.target, age_restricted) ? MessageForwardDestinationState::ready : MessageForwardDestinationState::unavailable;
        if (d.state != next) { d.state = next; did_change = true; }
    }
    if (c->committed && outbox) for (u32 i = 0; i < c->destination_count; ++i) {
        auto& d = c->destinations[i];
        if (d.state == MessageForwardDestinationState::sent || eligible(sources, d.row.target, age_restricted)) continue;
        const auto* r = request(*outbox, d.nonce);
        if (r && (r->phase == MessageComposerRequestPhase::queued || r->phase == MessageComposerRequestPhase::retry_wait) &&
            message_composer_controller_cancel_unsent(outbox, d.nonce).ok()) {
            d.nonce = 0; d.state = MessageForwardDestinationState::unavailable; did_change = true;
        }
    }
    if (outbox) did_change = message_forward_refresh(c, *outbox) || did_change;
    if (did_change) changed(c);
    return did_change;
}
bool message_forward_remove(MessageForwardController* c, u64 channel_id) noexcept {
    if (!c || !c->initialized || c->committed) return fail(c, MessageForwardError::busy);
    for (u32 i = 0; i < c->destination_count; ++i) if (c->destinations[i].row.target.channel_id == channel_id) {
        for (u32 j = i + 1; j < c->destination_count; ++j) c->destinations[j - 1] = c->destinations[j];
        reset_value(&c->destinations[--c->destination_count]); c->error = MessageForwardError::none; changed(c); return true;
    }
    return false;
}
bool message_forward_toggle(MessageForwardController* c, QuickSwitcherScope scope, QuickSwitcherSources sources,
    u32 index, u32 candidate_revision) noexcept {
    if (!c || !message_forward_current(*c, scope, sources)) return fail(c, MessageForwardError::stale_scope);
    if (c->committed) return fail(c, MessageForwardError::busy);
    QuickSwitcherTarget target{};
    if (!quick_switcher_resolve(c->candidates, scope, sources, index, candidate_revision, &target))
        return fail(c, MessageForwardError::stale_results);
    for (u32 i = 0; i < c->destination_count; ++i) if (c->destinations[i].row.target.channel_id == target.channel_id)
        return message_forward_remove(c, target.channel_id);
    if (!eligible(sources, target, source_age_restricted(sources, c->source))) return fail(c, MessageForwardError::destination_unavailable);
    if (c->destination_count == kMessageForwardMaximumDestinations) return fail(c, MessageForwardError::selection_limit);
    auto& d = c->destinations[c->destination_count++]; reset_value(&d); d.row = c->candidates.rows[index];
    c->error = MessageForwardError::none; changed(c); return true;
}
bool message_forward_set_caption(MessageForwardController* c, Utf8View caption) noexcept {
    if (!c || !c->initialized || c->committed) return fail(c, MessageForwardError::busy);
    const auto valid = utf8_validate(caption);
    if (caption.bytes > sizeof(c->caption) || !valid.valid || valid.codepoints > kMessageComposerDefaultMaximumCodepoints) {
        c->caption_valid = false;
        return fail(c, MessageForwardError::invalid_caption);
    }
    const bool recovered = !c->caption_valid;
    c->caption_valid = true;
    if (c->caption_bytes == caption.bytes && bytes_equal(c->caption, caption.data, caption.bytes)) {
        if (recovered) { c->error = MessageForwardError::none; changed(c); }
        return recovered;
    }
    copy_bytes(c->caption, caption.data, caption.bytes);
    if (caption.bytes < c->caption_bytes) clear_bytes(c->caption + caption.bytes, c->caption_bytes - caption.bytes);
    c->caption_bytes = caption.bytes; c->error = MessageForwardError::none; changed(c); return true;
}
bool message_forward_confirm(MessageForwardController* c, QuickSwitcherScope scope, QuickSwitcherSources sources,
    MessageComposerController* outbox, u64 unix_ms, u64 monotonic_ms) noexcept {
    if (!c || !message_forward_current(*c, scope, sources)) return fail(c, MessageForwardError::stale_scope);
    if (c->committed) return fail(c, MessageForwardError::busy);
    if (!c->caption_valid) return fail(c, MessageForwardError::invalid_caption);
    if (!outbox || !outbox->initialized || !unix_ms || !monotonic_ms) return fail(c, MessageForwardError::queue_failed);
    if (!c->destination_count) return fail(c, MessageForwardError::no_destinations);
    // Validate the whole selection before sending to its first destination.
    for (u32 i = 0; i < c->destination_count; ++i) if (!eligible(sources, c->destinations[i].row.target, source_age_restricted(sources, c->source)))
        return fail(c, MessageForwardError::destination_unavailable);
    c->committed = true; bool all = true;
    for (u32 i = 0; i < c->destination_count; ++i)
        all = queue_one(c, c->destinations[i], sources, outbox, unix_ms, monotonic_ms) && all;
    c->error = all ? MessageForwardError::none : MessageForwardError::queue_failed; changed(c); return all;
}
bool message_forward_retry_failed(MessageForwardController* c, QuickSwitcherScope scope, QuickSwitcherSources sources,
    MessageComposerController* outbox, u64 unix_ms, u64 monotonic_ms, u64 channel_id) noexcept {
    if (!c || !message_forward_current(*c, scope, sources)) return fail(c, MessageForwardError::stale_scope);
    if (!c->committed || !outbox || !outbox->initialized || !unix_ms || !monotonic_ms) return fail(c, MessageForwardError::busy);
    message_forward_refresh(c, *outbox); bool admitted = false, all = true;
    for (u32 i = 0; i < c->destination_count; ++i) {
        auto& d = c->destinations[i];
        if (channel_id && d.row.target.channel_id != channel_id) continue;
        if (d.state != MessageForwardDestinationState::failed && d.state != MessageForwardDestinationState::unavailable) continue;
        admitted = true; all = queue_one(c, d, sources, outbox, unix_ms, monotonic_ms) && all;
    }
    if (!admitted) return false;
    c->error = all ? MessageForwardError::none : MessageForwardError::queue_failed; changed(c); return all;
}
bool message_forward_observe_completion(MessageForwardController* c, const MessageSendCompletion& completion) noexcept {
    if (!c || (!c->initialized && !c->committed) || !completion.nonce) return false;
    for (u32 i = 0; i < c->destination_count; ++i) {
        auto& d = c->destinations[i];
        if (d.nonce != completion.nonce || d.row.target.channel_id != completion.channel_id) continue;
        const auto next = completion.kind == MessageSendCompletionKind::accepted ? MessageForwardDestinationState::sent : MessageForwardDestinationState::failed;
        if (d.state == MessageForwardDestinationState::sent || d.state == next) return false;
        d.state = next; d.http_status = completion.http_status; changed(c); return true;
    }
    return false;
}
bool message_forward_refresh(MessageForwardController* c, const MessageComposerController& outbox) noexcept {
    if (!c || (!c->initialized && !c->committed)) return false;
    bool did_change = false;
    for (u32 i = 0; i < c->destination_count; ++i) {
        auto& d = c->destinations[i];
        if (!d.nonce || d.state == MessageForwardDestinationState::sent || d.state == MessageForwardDestinationState::cancelled) continue;
        const auto* r = request(outbox, d.nonce);
        if (!r) continue; // The host forwards finished tracker completions.
        auto next = d.state;
        switch (r->phase) {
        case MessageComposerRequestPhase::queued: next = MessageForwardDestinationState::queued; break;
        case MessageComposerRequestPhase::submitting: case MessageComposerRequestPhase::awaiting_gateway: next = MessageForwardDestinationState::sending; break;
        case MessageComposerRequestPhase::retry_wait: next = MessageForwardDestinationState::retry_wait; break;
        case MessageComposerRequestPhase::failed: next = MessageForwardDestinationState::failed; break;
        case MessageComposerRequestPhase::confirmed: next = MessageForwardDestinationState::sent; break;
        }
        if (next != d.state || d.http_status != r->http_status) { d.state = next; d.http_status = r->http_status; did_change = true; }
    }
    const auto& tracker = outbox.sends;
    for (u32 i = 0; i < tracker.completion_count; ++i)
        did_change = message_forward_observe_completion(c, tracker.completions[(tracker.completion_read + i) % kMessageSendTrackerCapacity]) || did_change;
    if (did_change) changed(c); return did_change;
}
bool message_forward_cancel_pending(MessageForwardController* c, MessageComposerController* outbox) noexcept {
    if (!c || !outbox) return false;
    bool did_change = false;
    for (u32 i = 0; i < c->destination_count; ++i) {
        auto& d = c->destinations[i];
        if (!d.nonce) { if (d.state != MessageForwardDestinationState::sent) { d.state = MessageForwardDestinationState::cancelled; did_change = true; } continue; }
        const auto* r = request(*outbox, d.nonce);
        if (!r) continue;
        const auto result = r->phase == MessageComposerRequestPhase::failed ? message_composer_controller_discard_failed(outbox, d.nonce) :
            message_composer_controller_cancel_unsent(outbox, d.nonce);
        if (result.ok()) { d.nonce = 0; d.state = MessageForwardDestinationState::cancelled; did_change = true; }
    }
    if (did_change) changed(c); return did_change;
}
bool message_forward_pending(const MessageForwardController& c) noexcept {
    for (u32 i = 0; i < c.destination_count; ++i) if (pending(c.destinations[i].state)) return true;
    return false;
}
Utf8View message_forward_destination_status(const MessageForwardDestination& d) noexcept {
    switch (d.state) {
    case MessageForwardDestinationState::ready: return utf8_literal("Ready");
    case MessageForwardDestinationState::queued: return utf8_literal("Queued");
    case MessageForwardDestinationState::sending: return utf8_literal("Forwarding…");
    case MessageForwardDestinationState::retry_wait: return utf8_literal("Retrying…");
    case MessageForwardDestinationState::sent: return utf8_literal("Forwarded");
    case MessageForwardDestinationState::unavailable: return utf8_literal("Destination unavailable");
    case MessageForwardDestinationState::cancelled: return utf8_literal("Cancelled");
    case MessageForwardDestinationState::failed:
        if (d.http_status == 403) return utf8_literal("Permission denied. Choose another channel.");
        if (d.http_status == 404) return utf8_literal("Message or destination no longer available.");
        return utf8_literal("Forward failed. Retry this destination.");
    }
    return {};
}
Utf8View message_forward_status(const MessageForwardController& c) noexcept {
    switch (c.error) {
    case MessageForwardError::invalid_source: return utf8_literal("This message is no longer available to forward.");
    case MessageForwardError::stale_scope: return utf8_literal("The account changed. Open the message again.");
    case MessageForwardError::stale_results: return utf8_literal("The list changed. Choose again.");
    case MessageForwardError::destination_unavailable: return utf8_literal("A selected destination is unavailable. Remove it to continue.");
    case MessageForwardError::selection_limit: return utf8_literal("Select up to 5 destinations.");
    case MessageForwardError::no_destinations: return utf8_literal("Select a destination to forward this message.");
    case MessageForwardError::invalid_caption: return utf8_literal("Use a caption of up to 2,000 characters.");
    case MessageForwardError::busy: return utf8_literal("Already forwarding this message.");
    case MessageForwardError::queue_failed: return utf8_literal("Couldn't forward to some destinations. Retry them.");
    case MessageForwardError::none: break;
    }
    if (message_forward_pending(c)) return utf8_literal("Forwarding…");
    if (c.committed) {
        for (u32 i = 0; i < c.destination_count; ++i) if (c.destinations[i].state != MessageForwardDestinationState::sent)
            return utf8_literal("Some forwards failed. Select one to retry.");
        return utf8_literal("Message forwarded.");
    }
    return c.source.age_restricted ? utf8_literal("Choose up to 5 age-restricted channels.") :
        utf8_literal("Choose up to 5 destinations.");
}
} // namespace maicord
