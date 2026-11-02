#include "pipeline_internal.hpp"
#include "maicord/app/plugins/keep.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"

// mc_api entries for the message pipeline: local messages, the composer, actions, timers and kept
// messages. Each recovers its binding from the context, refuses a stale one, checks permission
// and gesture before it looks at arguments, and only then reaches the application's sink.
namespace maicord::plugin_runtime_detail {
namespace {

constexpr u32 kDraftFlags = MC_DRAFT_SILENT | MC_DRAFT_SUPPRESS_EMBEDS | MC_DRAFT_REPLY_PING;
constexpr u32 kTimerIntervalMax = 24u * 60u * 60u * 1000u;
constexpr u32 kEmojiBytesMax = 64;
constexpr u32 kActivityTextMax = 128;
constexpr u32 kActivityUrlMax = 256;

PluginBinding* active(mc_ctx* ctx) noexcept {
    return ctx && ctx->call ? reinterpret_cast<PluginBinding*>(ctx) : nullptr;
}

Utf8View view(const mc_str& text) noexcept {
    return {text.data, text.bytes};
}

// UTF-8 of 1..maximum bytes; line breaks are allowed.
bool text_ok(const mc_str& text, u32 maximum) noexcept {
    return text.data && text.bytes && text.bytes <= maximum && utf8_validate(view(text)).valid;
}
bool optional_text_ok(const mc_str& text, u32 maximum) noexcept {
    return !text.bytes || text_ok(text, maximum);
}

// A write to the account needs MC_PERM_ACT_MESSAGES and a gesture, or MC_PERM_AUTOMATED_ACTIONS.
mc_status write_gate(const PluginBinding& binding) noexcept {
    if (!(binding.permissions & (MC_PERM_ACT_MESSAGES | MC_PERM_AUTOMATED_ACTIONS))) {
        ++plugin_stats().action_denials;
        return MC_E_DENIED;
    }
    if (!(binding.ctx.flags & MC_CTX_GESTURE) && !(binding.permissions & MC_PERM_AUTOMATED_ACTIONS)) {
        ++plugin_stats().action_denials;
        return MC_E_GESTURE;
    }
    return MC_OK;
}

mc_status composer_gate(const PluginBinding& binding) noexcept {
    if (binding.permissions & MC_PERM_COMPOSER) return MC_OK;
    ++plugin_stats().action_denials;
    return MC_E_DENIED;
}

// After the checks: the rate limit, then the sink.
template <typename Member> const PluginPipelineSink* sink_for(Member member) noexcept {
    const PluginPipelineSink* sink = plugin_sink();
    return sink && sink->*member ? sink : nullptr;
}

mc_status limited(PluginBinding* binding, PluginBucket bucket) noexcept {
    if (plugin_take(binding, bucket)) {
        ++plugin_stats().actions;
        return MC_OK;
    }
    ++plugin_stats().action_denials;
    return MC_E_RATE_LIMITED;
}

} // namespace

mc_status MC_CALL api_local_message(mc_ctx* ctx, uint64_t channel, mc_str text, uint64_t* message_id) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    // Channel 0 is the open channel.
    if (!text_ok(text, MC_ACTION_TEXT_MAX)) return MC_E_INVALID;
    const auto* sink = sink_for(&PluginPipelineSink::local_message);
    if (!sink) return MC_E_UNSUPPORTED;
    if (const mc_status status = limited(binding, bucket_local); status != MC_OK) return status;
    u64 id = 0;
    const mc_status status = sink->local_message(binding->owner_id, channel, view(text), &id);
    if (status == MC_OK && message_id) *message_id = id;
    return status;
}

mc_status MC_CALL api_local_message_remove(mc_ctx* ctx, uint64_t channel, uint64_t message) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (!channel || !message) return MC_E_INVALID;
    const auto* sink = sink_for(&PluginPipelineSink::local_message_remove);
    return sink ? sink->local_message_remove(binding->owner_id, channel, message) : MC_E_UNSUPPORTED;
}

mc_status MC_CALL api_composer_text(mc_ctx* ctx, void* out, uint32_t capacity, uint32_t* bytes) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (const mc_status gate = composer_gate(*binding); gate != MC_OK) return gate;
    if (!bytes || (capacity && !out)) return MC_E_INVALID;
    const auto* sink = sink_for(&PluginPipelineSink::composer_text);
    return sink ? sink->composer_text(out, capacity, bytes) : MC_E_UNSUPPORTED;
}

mc_status MC_CALL api_composer_insert(mc_ctx* ctx, mc_str text) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (const mc_status gate = composer_gate(*binding); gate != MC_OK) return gate;
    if (!text_ok(text, MC_ACTION_TEXT_MAX)) return MC_E_INVALID;
    const auto* sink = sink_for(&PluginPipelineSink::composer_insert);
    return sink ? sink->composer_insert(view(text)) : MC_E_UNSUPPORTED;
}

mc_status MC_CALL api_composer_replace(mc_ctx* ctx, mc_str text) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (const mc_status gate = composer_gate(*binding); gate != MC_OK) return gate;
    // An empty text clears the draft.
    if (!optional_text_ok(text, MC_ACTION_TEXT_MAX) || (text.bytes && !text.data)) return MC_E_INVALID;
    const auto* sink = sink_for(&PluginPipelineSink::composer_replace);
    return sink ? sink->composer_replace(view(text)) : MC_E_UNSUPPORTED;
}

mc_status MC_CALL api_composer_focus(mc_ctx* ctx) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (const mc_status gate = composer_gate(*binding); gate != MC_OK) return gate;
    const auto* sink = sink_for(&PluginPipelineSink::composer_focus);
    return sink ? sink->composer_focus() : MC_E_UNSUPPORTED;
}

mc_status MC_CALL api_composer_reply(mc_ctx* ctx, uint64_t channel, uint64_t message, uint32_t ping) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (const mc_status gate = composer_gate(*binding); gate != MC_OK) return gate;
    // Message 0 clears the reply.
    if (!channel || ping > 1 || (!message && ping)) return MC_E_INVALID;
    const auto* sink = sink_for(&PluginPipelineSink::composer_reply);
    return sink ? sink->composer_reply(channel, message, ping != 0) : MC_E_UNSUPPORTED;
}

mc_status MC_CALL api_composer_edit(mc_ctx* ctx, uint64_t channel, uint64_t message) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (const mc_status gate = composer_gate(*binding); gate != MC_OK) return gate;
    if (!channel || !message) return MC_E_INVALID;
    const auto* sink = sink_for(&PluginPipelineSink::composer_edit);
    return sink ? sink->composer_edit(channel, message) : MC_E_UNSUPPORTED;
}

mc_status MC_CALL api_send_message(mc_ctx* ctx, const mc_send* send) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (const mc_status gate = write_gate(*binding); gate != MC_OK) return gate;
    if (!send || !send->channel_id || !text_ok(send->text, MC_DRAFT_TEXT_MAX) || (send->flags & ~kDraftFlags))
        return MC_E_INVALID;
    const auto* sink = sink_for(&PluginPipelineSink::send);
    if (!sink) return MC_E_UNSUPPORTED;
    if (const mc_status status = limited(binding, bucket_messages); status != MC_OK) return status;
    return sink->send(binding->owner_id, {send->channel_id, send->reply_to, view(send->text), send->flags});
}

mc_status MC_CALL api_edit_message(mc_ctx* ctx, uint64_t channel, uint64_t message, mc_str text) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (const mc_status gate = write_gate(*binding); gate != MC_OK) return gate;
    if (!channel || !message || !text_ok(text, MC_ACTION_TEXT_MAX)) return MC_E_INVALID;
    const auto* sink = sink_for(&PluginPipelineSink::edit);
    if (!sink) return MC_E_UNSUPPORTED;
    if (const mc_status status = limited(binding, bucket_messages); status != MC_OK) return status;
    return sink->edit(binding->owner_id, channel, message, view(text));
}

mc_status MC_CALL api_react(mc_ctx* ctx, uint64_t channel, uint64_t message, mc_str emoji, uint32_t flags) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (const mc_status gate = write_gate(*binding); gate != MC_OK) return gate;
    if (!channel || !message || !text_ok(emoji, kEmojiBytesMax) || (flags & ~MC_REACT_REMOVE)) return MC_E_INVALID;
    const auto* sink = sink_for(&PluginPipelineSink::react);
    if (!sink) return MC_E_UNSUPPORTED;
    if (const mc_status status = limited(binding, bucket_reactions); status != MC_OK) return status;
    return sink->react(binding->owner_id, channel, message, view(emoji), (flags & MC_REACT_REMOVE) != 0);
}

mc_status MC_CALL api_delete_own(mc_ctx* ctx, uint64_t channel, uint64_t message) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (const mc_status gate = write_gate(*binding); gate != MC_OK) return gate;
    if (!channel || !message) return MC_E_INVALID;
    const auto* sink = sink_for(&PluginPipelineSink::delete_own);
    if (!sink) return MC_E_UNSUPPORTED;
    if (const mc_status status = limited(binding, bucket_messages); status != MC_OK) return status;
    return sink->delete_own(binding->owner_id, channel, message);
}

mc_status MC_CALL api_mark_read(mc_ctx* ctx, uint32_t scope, uint64_t id) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (const mc_status gate = write_gate(*binding); gate != MC_OK) return gate;
    if (scope < MC_READ_CHANNEL || scope > MC_READ_ALL || (scope == MC_READ_ALL ? id != 0 : id == 0))
        return MC_E_INVALID;
    const auto* sink = sink_for(&PluginPipelineSink::mark_read);
    if (!sink) return MC_E_UNSUPPORTED;
    if (const mc_status status = limited(binding, bucket_read); status != MC_OK) return status;
    return sink->mark_read(binding->owner_id, scope, id);
}

mc_status MC_CALL api_set_activity(mc_ctx* ctx, const mc_activity* activity) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (!(binding->permissions & MC_PERM_ACT_PRESENCE)) {
        ++plugin_stats().action_denials;
        return MC_E_DENIED;
    }
    if (activity) {
        const bool custom = activity->type == MC_ACTIVITY_CUSTOM;
        if (activity->type > MC_ACTIVITY_COMPETING || activity->flags ||
            !optional_text_ok(activity->name, kActivityTextMax) || !optional_text_ok(activity->details, kActivityTextMax) ||
            !optional_text_ok(activity->state, kActivityTextMax) || !optional_text_ok(activity->url, kActivityUrlMax) ||
            !optional_text_ok(activity->emoji, kEmojiBytesMax) ||
            (activity->end_ms && activity->end_ms < activity->start_ms) ||
            (custom ? !activity->state.bytes && !activity->emoji.bytes : !activity->name.bytes))
            return MC_E_INVALID;
    }
    const auto* sink = sink_for(&PluginPipelineSink::set_activity);
    if (!sink) return MC_E_UNSUPPORTED;
    // Clearing is never limited, so a plugin can always take its activity back.
    if (activity)
        if (const mc_status status = limited(binding, bucket_presence); status != MC_OK) return status;
    return sink->set_activity(binding->owner_id, activity);
}

mc_status MC_CALL api_timer(mc_ctx* ctx, uint64_t id, uint32_t interval_ms, uint32_t repeat) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (interval_ms < MC_TIMER_INTERVAL_MIN || interval_ms > kTimerIntervalMax || repeat > 1 ||
        !plugin_subscribed(binding, PluginHook::timer))
        return MC_E_INVALID;
    PluginSide* side = plugin_side(binding, true);
    if (!side) return MC_E_BUDGET;
    PluginTimer* slot = nullptr;
    for (auto& timer : side->timers) {
        if (timer.active && timer.id == id) {
            slot = &timer;
            break;
        }
        if (!timer.active && !slot) slot = &timer;
    }
    if (!slot) return MC_E_BUDGET;
    *slot = {id, clock_monotonic_milliseconds() + interval_ms, interval_ms, repeat != 0, true};
    return MC_OK;
}

mc_status MC_CALL api_cancel_timer(mc_ctx* ctx, uint64_t id) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (PluginSide* side = plugin_side(binding, false))
        for (auto& timer : side->timers)
            if (timer.active && timer.id == id) {
                timer.active = false;
                return MC_OK;
            }
    return MC_E_NOT_FOUND;
}

namespace {
mc_status history_gate(const PluginBinding& binding) noexcept {
    if (binding.permissions & MC_PERM_MESSAGE_HISTORY) return MC_OK;
    ++plugin_stats().action_denials;
    return MC_E_DENIED;
}

bool kept_valid(const mc_kept& item) noexcept {
    if ((item.flags & ~MC_KEPT_DELETED) || !item.channel_id || !item.message_id || item.edit_count > MC_KEEP_EDITS_MAX ||
        !text_ok(item.content, MC_KEEP_CONTENT_MAX) || !optional_text_ok(item.author_name, kKeepNameMax * 4) ||
        (item.edit_count && !item.edits))
        return false;
    // Something to keep: a deleted message, or one with earlier texts.
    if (!(item.flags & MC_KEPT_DELETED) && !item.edit_count) return false;
    for (u32 i = 0; i < item.edit_count; ++i)
        if (!text_ok(item.edits[i], MC_KEEP_CONTENT_MAX)) return false;
    return true;
}
} // namespace

mc_status MC_CALL api_keep_info(mc_ctx* ctx, uint64_t channel, uint64_t message, mc_kept_info* out) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (const mc_status gate = history_gate(*binding); gate != MC_OK) return gate;
    if (!out) return MC_E_INVALID;
    KeepInfo info{};
    if (!keep_info(channel, message, &info)) return MC_E_NOT_FOUND;
    *out = {info.deleted ? MC_KEPT_DELETED : 0u, info.edit_count, info.author_id, info.timestamp_ms, info.edited_ms};
    return MC_OK;
}

mc_status MC_CALL api_keep_text(mc_ctx* ctx, uint64_t channel, uint64_t message, uint32_t version, void* out,
                                uint32_t capacity, uint32_t* bytes) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (const mc_status gate = history_gate(*binding); gate != MC_OK) return gate;
    if (!bytes || (capacity && !out)) return MC_E_INVALID;
    Utf8View text{};
    if (!keep_text(channel, message, version, &text)) return MC_E_NOT_FOUND;
    *bytes = text.bytes;
    if (text.bytes > capacity) return MC_E_TRUNCATED;
    copy_bytes(out, text.data, text.bytes);
    return MC_OK;
}

mc_status MC_CALL api_keep_restore(mc_ctx* ctx, const mc_kept* items, uint32_t count) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (!(binding->permissions & MC_PERM_PERSISTENT_LOG)) {
        ++plugin_stats().action_denials;
        return MC_E_DENIED;
    }
    if (!items || !count) return MC_E_INVALID;
    if (count > MC_KEEP_RESTORE_MAX) return MC_E_CAPACITY;
    for (u32 i = 0; i < count; ++i)
        if (!kept_valid(items[i])) return MC_E_INVALID;
    mc_status result = MC_OK;
    for (u32 i = 0; i < count; ++i) {
        const mc_kept& item = items[i];
        KeepInfo existing{};
        const bool known = keep_info(item.channel_id, item.message_id, &existing);
        const KeepMessage message{item.channel_id, item.message_id, item.author_id, item.timestamp_ms,
                                  item.edited_ms,  view(item.content), view(item.author_name)};
        u32 first_edit = 0;
        bool stored;
        if (item.flags & MC_KEPT_DELETED) {
            stored = keep_deleted(message);
        } else {
            // Not deleted: the newest earlier text and the latest one make the edit.
            KeepMessage before = message;
            before.content = view(item.edits[0]);
            stored = keep_edited(before, view(item.content), item.edited_ms);
            first_edit = 1;
        }
        // Earlier texts of a message already kept stay as they are.
        if (stored && !(known && existing.edit_count))
            for (u32 edit = first_edit; edit < item.edit_count; ++edit) keep_add_edit(item.channel_id, item.message_id, view(item.edits[edit]));
        if (!stored) result = MC_E_BUDGET;
    }
    if (const auto* sink = plugin_sink(); sink && sink->keep_changed) sink->keep_changed();
    return result;
}

mc_status MC_CALL api_read_open_channel(mc_ctx* ctx, mc_open_channel* out) noexcept {
    if (!active(ctx)) return MC_E_INACTIVE;
    if (!out) return MC_E_INVALID;
    *out = {};
    const auto* sink = sink_for(&PluginPipelineSink::open_channel);
    return sink ? sink->open_channel(out) : MC_E_UNSUPPORTED;
}

mc_status MC_CALL api_read_messages(mc_ctx* ctx, uint64_t channel, uint64_t before, mc_message_ref* out,
                                    uint32_t capacity, uint32_t* count) noexcept {
    PluginBinding* binding = active(ctx);
    if (!binding) return MC_E_INACTIVE;
    if (!(binding->permissions & MC_PERM_READ_MESSAGES)) return MC_E_DENIED;
    if (!channel || !out || !count || !capacity || capacity > MC_READ_MESSAGES_MAX) return MC_E_INVALID;
    *count = 0;
    const auto* sink = sink_for(&PluginPipelineSink::messages);
    return sink ? sink->messages(channel, before, out, capacity, count) : MC_E_UNSUPPORTED;
}

} // namespace maicord::plugin_runtime_detail
