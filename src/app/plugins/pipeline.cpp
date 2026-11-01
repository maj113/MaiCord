#include "pipeline_internal.hpp"
#include "maicord/app/plugins/keep.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {

static_assert(MC_MESSAGE_OWN == 1 && MC_EVENT_KIND_COUNT <= 64);

namespace plugin_runtime_detail {
namespace {

PluginPipelineStats g_stats;
const PluginPipelineSink* g_sink;
PluginSide g_sides[kPluginMaximumBindings];

struct BucketPolicy {
    u32 capacity, period_ms;
};
// Messages (send, edit, delete, composer-free writes), reactions, mark read, activity, local messages.
constexpr BucketPolicy kBuckets[bucket_count]{{kPluginActionsPerWindow, kPluginActionWindowMilliseconds},
                                              {10, 10000}, {10, 10000}, {2, 10000}, {10, 10000}};

} // namespace

PluginSide* plugin_side(PluginBinding* binding, bool create) noexcept {
    PluginSide* free = nullptr;
    for (auto& side : g_sides) {
        if (side.binding == binding) return &side;
        if (!side.binding && !free) free = &side;
    }
    if (!create || !free) return nullptr;
    reset_value(free);
    free->binding = binding;
    return free;
}

bool plugin_take(PluginBinding* binding, PluginBucket bucket) noexcept {
    PluginSide* side = plugin_side(binding, true);
    if (!side) return false;
    const u64 now = clock_monotonic_milliseconds();
    const BucketPolicy policy = kBuckets[bucket];
    if (!side->window_start[bucket] || now - side->window_start[bucket] >= policy.period_ms) {
        side->window_start[bucket] = now;
        side->window_count[bucket] = 0;
    }
    if (side->window_count[bucket] >= policy.capacity) return false;
    ++side->window_count[bucket];
    return true;
}

const PluginPipelineSink* plugin_sink() noexcept {
    return g_sink;
}
PluginPipelineStats& plugin_stats() noexcept {
    return g_stats;
}

// Whether the plugin declares the hook; true while it starts, before its hooks are published.
bool plugin_subscribed(const PluginBinding* binding, PluginHook hook) noexcept {
    const mc_descriptor* descriptor = binding->image.descriptor;
    for (u32 i = 0; descriptor && i < descriptor->hook_count; ++i)
        if (descriptor->hooks[i].kind == static_cast<u32>(hook)) return true;
    return false;
}

} // namespace plugin_runtime_detail

using namespace plugin_runtime_detail;

void plugin_pipeline_set_sink(const PluginPipelineSink* sink) noexcept {
    g_sink = sink;
}

const PluginPipelineStats& plugin_pipeline_stats() noexcept {
    return g_stats;
}

namespace {

mc_message to_abi(const PluginMessage& message) noexcept {
    return {message.message_id,
            message.channel_id,
            message.guild_id,
            message.author_id,
            message.reply_to,
            message.timestamp_ms,
            message.edited_ms,
            {message.content.data, message.content.bytes, 0},
            {message.author_name.data, message.author_name.bytes, 0},
            message.flags,
            message.type,
            message.revision,
            message.relationship,
            message.attachment_count,
            message.embed_count};
}

KeepMessage to_keep(const PluginMessage& message) noexcept {
    return {message.channel_id, message.message_id, message.author_id, message.timestamp_ms,
            message.edited_ms,  message.content,    message.author_name};
}

// One call into a hook that returns a number; the verdict lands in result.
struct Verdict {
    const mc_message* first;
    const mc_message* second;
    const mc_click* click;
    u64 channel_id;
    mc_hook_fn function;
    u32 result;
};
mc_status run_deleted(mc_ctx* ctx, void* argument) noexcept {
    auto& call = *static_cast<Verdict*>(argument);
    call.result = call.function.deleted(ctx, call.first);
    return MC_OK;
}
mc_status run_edited(mc_ctx* ctx, void* argument) noexcept {
    auto& call = *static_cast<Verdict*>(argument);
    call.result = call.function.edited(ctx, call.first, call.second);
    return MC_OK;
}
mc_status run_click(mc_ctx* ctx, void* argument) noexcept {
    auto& call = *static_cast<Verdict*>(argument);
    call.result = call.function.click(ctx, call.first, call.click);
    return MC_OK;
}
mc_status run_typing(mc_ctx* ctx, void* argument) noexcept {
    auto& call = *static_cast<Verdict*>(argument);
    call.result = call.function.typing(ctx, call.channel_id);
    return MC_OK;
}

// Runs the verdict function of every subscriber of hook; returns whether any answered `wanted`.
// stop_at_first ends the pass at the first such answer.
bool run_verdicts(PluginHook hook, PluginCallKind kind, PluginCallClass call_class, u32 context_flags, PluginInvoke invoke,
                  Verdict call, u32 wanted, bool stop_at_first, u32* counter, u32 filter_bit) noexcept {
    bool answered = false;
    for (const auto& subscriber : subscribers(hook)) {
        if (subscriber.binding->struck_out) continue;
        if (filter_bit && subscriber.filter && !(subscriber.filter & filter_bit)) continue;
        call.function = subscriber.function;
        call.result = 0;
        const PluginCallResult ran = plugin_binding_call(subscriber.binding, kind, call_class, context_flags, invoke, &call);
        ++*counter;
        if (ran.overran) {
            ++g_stats.strikes;
            continue;
        }
        if (call.result == wanted) {
            answered = true;
            if (stop_at_first) break;
        }
    }
    return answered;
}

// ---- Events ---------------------------------------------------------------------------

struct EventSlot {
    mc_event event;
    char text[kPluginEventTextBytes];
    char name[kPluginEventNameBytes];
};
static_assert(kPluginEventSlots * sizeof(EventSlot) <= (1u << 20));

EventSlot* g_ring;
u64 g_head; // events pushed so far; the slot of event n is n % kPluginEventSlots
u64 g_interest;

void recompute_events() noexcept {
    u64 interest = 0;
    for (const auto& subscriber : subscribers(PluginHook::events)) interest |= subscriber.filter;
    g_interest = interest & MC_EVENT_KINDS;
    if (!interest && g_ring) {
        VirtualFree(g_ring, 0, MEM_RELEASE);
        g_ring = nullptr;
        g_head = 0;
    }
}

struct EventCall {
    mc_events_fn function;
    const mc_event* events;
    u32 count;
};
mc_status run_events(mc_ctx* ctx, void* argument) noexcept {
    auto& call = *static_cast<EventCall*>(argument);
    call.function(ctx, call.events, call.count);
    return MC_OK;
}

// Delivers up to kPluginEventCallsPerPass batches to one subscriber.
void deliver_to(const PluginSubscriber& subscriber) noexcept {
    PluginSide* side = plugin_side(subscriber.binding, true);
    if (!side) return;
    mc_event batch[MC_EVENT_BATCH_MAX];
    for (u32 calls = 0; calls < kPluginEventCallsPerPass && !subscriber.binding->struck_out; ++calls) {
        u32 count = 0, delivered = 0;
        if (g_head - side->cursor > kPluginEventSlots) {
            // The plugin was too slow: it learns how many events it missed.
            const u64 lost = g_head - kPluginEventSlots - side->cursor;
            side->cursor = g_head - kPluginEventSlots;
            g_stats.events_lost += static_cast<u32>(lost);
            reset_value(&batch[count]);
            batch[count].kind = MC_EVENT_OVERFLOW;
            batch[count].value = lost;
            ++count;
        }
        while (side->cursor < g_head && count < MC_EVENT_BATCH_MAX) {
            const EventSlot& slot = g_ring[side->cursor % kPluginEventSlots];
            ++side->cursor;
            if (!(subscriber.filter & MC_EVENT_BIT(slot.event.kind))) continue;
            batch[count++] = slot.event;
            ++delivered;
        }
        if (!count) return;
        EventCall call{subscriber.function.events, batch, count};
        const PluginCallResult ran = plugin_binding_call(subscriber.binding, PluginCallKind::event,
                                                         PluginCallClass::event, 0, run_events, &call);
        ++g_stats.event_calls;
        g_stats.events_delivered += delivered;
        if (ran.overran) ++g_stats.strikes;
    }
}

} // namespace

bool plugin_dispatch_deleted(const PluginMessage& message) noexcept {
    if (!plugin_hook_subscribed(PluginHook::message_deleted)) return false;
    const mc_message abi = to_abi(message);
    const bool keep = run_verdicts(PluginHook::message_deleted, PluginCallKind::event, PluginCallClass::pre, 0,
                                   run_deleted, Verdict{&abi, nullptr, nullptr, 0, {}, 0}, MC_KEEP, false,
                                   &g_stats.deleted_calls, 0);
    return keep && keep_deleted(to_keep(message));
}

bool plugin_dispatch_edited(const PluginMessage& before, const PluginMessage& after) noexcept {
    if (!plugin_hook_subscribed(PluginHook::message_edited)) return false;
    const mc_message abi_before = to_abi(before), abi_after = to_abi(after);
    const bool keep = run_verdicts(PluginHook::message_edited, PluginCallKind::event, PluginCallClass::pre, 0,
                                   run_edited, Verdict{&abi_before, &abi_after, nullptr, 0, {}, 0}, MC_KEEP, false,
                                   &g_stats.edited_calls, 0);
    return keep && keep_edited(to_keep(before), after.content, after.edited_ms);
}

bool plugin_dispatch_click(const PluginMessage& message, const PluginClick& click) noexcept {
    if (!plugin_hook_subscribed(PluginHook::message_click)) return false;
    const u32 bit = click.button == MC_CLICK_MIDDLE ? MC_CLICK_FILTER_MIDDLE
                    : click.count >= 2               ? MC_CLICK_FILTER_DOUBLE
                                                     : MC_CLICK_FILTER_LEFT;
    const mc_message abi = to_abi(message);
    const mc_click abi_click{click.button, click.count, click.modifiers, 0};
    return run_verdicts(PluginHook::message_click, PluginCallKind::slot_action, PluginCallClass::gesture,
                        MC_CTX_GESTURE, run_click, Verdict{&abi, nullptr, &abi_click, 0, {}, 0}, MC_CLICK_HANDLED,
                        false, &g_stats.click_calls, bit);
}

bool plugin_dispatch_typing(u64 channel_id) noexcept {
    if (!plugin_hook_subscribed(PluginHook::pre_typing)) return false;
    return run_verdicts(PluginHook::pre_typing, PluginCallKind::event, PluginCallClass::pre, 0, run_typing,
                        Verdict{nullptr, nullptr, nullptr, channel_id, {}, 0}, MC_SUPPRESS, true,
                        &g_stats.typing_calls, 0);
}

u64 plugin_events_interest() noexcept {
    return g_interest;
}

bool plugin_events_push(const PluginEvent& event) noexcept {
    if (!g_ring || event.kind >= MC_EVENT_KIND_COUNT || !(g_interest & MC_EVENT_BIT(event.kind))) return false;
    EventSlot& slot = g_ring[g_head % kPluginEventSlots];
    ++g_head;
    slot.event = {event.kind, event.flags, event.guild_id, event.channel_id, event.message_id, event.user_id,
                  event.value, event.time_ms, {}, {}};
    u32 text = event.text.bytes, name = event.name.bytes;
    if (text > kPluginEventTextBytes) {
        text = utf8_truncated_length(event.text, kPluginEventTextBytes);
        slot.event.flags |= MC_EVENT_TRUNCATED;
    }
    if (name > kPluginEventNameBytes) name = utf8_truncated_length(event.name, kPluginEventNameBytes);
    if (text) copy_bytes(slot.text, event.text.data, text);
    if (name) copy_bytes(slot.name, event.name.data, name);
    slot.event.text = {slot.text, text, 0};
    slot.event.name = {slot.name, name, 0};
    return true;
}

bool plugin_events_pending() noexcept {
    for (const auto& subscriber : subscribers(PluginHook::events)) {
        const PluginSide* side = plugin_side(subscriber.binding, false);
        if (side && side->cursor < g_head && !subscriber.binding->struck_out) return true;
    }
    return false;
}

bool plugin_events_deliver() noexcept {
    if (!g_ring) return false;
    for (const auto& subscriber : subscribers(PluginHook::events)) deliver_to(subscriber);
    return plugin_events_pending();
}

// ---- Timers ---------------------------------------------------------------------------

u64 plugin_timers_next_due() noexcept {
    u64 next = 0;
    for (const auto& side : g_sides) {
        if (!side.binding) continue;
        for (const auto& timer : side.timers)
            if (timer.active && (!next || timer.due_ms < next)) next = timer.due_ms;
    }
    return next;
}

namespace {
struct TimerCall {
    mc_timer_fn function;
    u64 id;
};
mc_status run_timer(mc_ctx* ctx, void* argument) noexcept {
    auto& call = *static_cast<TimerCall*>(argument);
    call.function(ctx, call.id);
    return MC_OK;
}
} // namespace

u32 plugin_timers_poll(u64 now_ms) noexcept {
    u32 total = 0;
    for (const auto& subscriber : subscribers(PluginHook::timer)) {
        PluginSide* side = plugin_side(subscriber.binding, false);
        if (!side) continue;
        u32 calls = 0;
        for (u32 i = 0; i < MC_TIMERS_MAX && calls < kPluginEventCallsPerPass && !subscriber.binding->struck_out; ++i) {
            PluginTimer& timer = side->timers[i];
            if (!timer.active || timer.due_ms > now_ms) continue;
            // The callee may replace or cancel its own timer: settle the schedule first.
            const u64 id = timer.id;
            if (timer.repeat) timer.due_ms = now_ms + timer.interval_ms;
            else timer.active = false;
            TimerCall call{subscriber.function.timer, id};
            const PluginCallResult ran = plugin_binding_call(subscriber.binding, PluginCallKind::timer,
                                                             PluginCallClass::event, 0, run_timer, &call);
            ++calls;
            ++g_stats.timer_calls;
            if (ran.overran) ++g_stats.strikes;
        }
        total += calls;
    }
    return total;
}

// ---- Per-plugin state --------------------------------------------------------------------

void plugin_pipeline_add(PluginBinding* binding) noexcept {
    if (plugin_subscribed(binding, PluginHook::events)) {
        if (!g_ring) {
            g_ring = static_cast<EventSlot*>(VirtualAlloc(nullptr, kPluginEventSlots * sizeof(EventSlot),
                                                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        }
        if (PluginSide* side = plugin_side(binding, true)) side->cursor = g_head;
    }
    recompute_events();
}

void plugin_pipeline_remove(PluginBinding* binding) noexcept {
    if (PluginSide* side = plugin_side(binding, false)) reset_value(side);
    recompute_events();
}

} // namespace maicord
