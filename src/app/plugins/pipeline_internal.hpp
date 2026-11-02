#pragma once

#include "maicord/app/plugins/binding.hpp"
#include "maicord/app/plugins/pipeline.hpp"

// Shared by pipeline.cpp (hooks, events, timers) and pipeline_api.cpp (the mc_api entries).
namespace maicord::plugin_runtime_detail {

// Write actions are limited per plugin by kind: this many per period.
enum PluginBucket : u32 { bucket_messages, bucket_reactions, bucket_read, bucket_presence, bucket_local, bucket_count };

// range-for over a hook's subscribers.
struct SubscriberRange {
    const PluginSubscriber *first, *last;
    const PluginSubscriber* begin() const noexcept { return first; }
    const PluginSubscriber* end() const noexcept { return last; }
};
inline SubscriberRange subscribers(PluginHook hook) noexcept {
    const ConstSlice<PluginSubscriber> list = plugin_dispatch_subscribers(hook);
    return {list.data, list.data + list.count};
}

struct PluginTimer {
    u64 id, due_ms;
    u32 interval_ms;
    bool repeat, active;
};

// What the pipeline keeps for one enabled plugin; claimed on first use, released at disable.
struct PluginSide {
    PluginBinding* binding;
    u64 cursor; // the next event (by sequence) this plugin has not seen
    u64 window_start[bucket_count];
    u32 window_count[bucket_count];
    PluginTimer timers[MC_TIMERS_MAX];
};

PluginSide* plugin_side(PluginBinding*, bool create) noexcept;
// True when the plugin may take one more action of this kind now (and counts it).
bool plugin_take(PluginBinding*, PluginBucket) noexcept;
const PluginPipelineSink* plugin_sink() noexcept;
PluginPipelineStats& plugin_stats() noexcept;
// Whether the plugin subscribed to the hook (timer creation needs its TIMER hook).
bool plugin_subscribed(const PluginBinding*, PluginHook) noexcept;

// The mc_api entries of this slice (pipeline_api.cpp); api.cpp lists them in kApi.
mc_status MC_CALL api_local_message(mc_ctx*, uint64_t channel, mc_str text, uint64_t* message_id) noexcept;
mc_status MC_CALL api_local_message_remove(mc_ctx*, uint64_t channel, uint64_t message) noexcept;
mc_status MC_CALL api_composer_text(mc_ctx*, void* out, uint32_t capacity, uint32_t* bytes) noexcept;
mc_status MC_CALL api_composer_insert(mc_ctx*, mc_str text) noexcept;
mc_status MC_CALL api_composer_replace(mc_ctx*, mc_str text) noexcept;
mc_status MC_CALL api_composer_focus(mc_ctx*) noexcept;
mc_status MC_CALL api_composer_reply(mc_ctx*, uint64_t channel, uint64_t message, uint32_t ping) noexcept;
mc_status MC_CALL api_composer_edit(mc_ctx*, uint64_t channel, uint64_t message) noexcept;
mc_status MC_CALL api_send_message(mc_ctx*, const mc_send*) noexcept;
mc_status MC_CALL api_edit_message(mc_ctx*, uint64_t channel, uint64_t message, mc_str text) noexcept;
mc_status MC_CALL api_react(mc_ctx*, uint64_t channel, uint64_t message, mc_str emoji, uint32_t flags) noexcept;
mc_status MC_CALL api_delete_own(mc_ctx*, uint64_t channel, uint64_t message) noexcept;
mc_status MC_CALL api_mark_read(mc_ctx*, uint32_t scope, uint64_t id) noexcept;
mc_status MC_CALL api_set_activity(mc_ctx*, const mc_activity*) noexcept;
mc_status MC_CALL api_timer(mc_ctx*, uint64_t id, uint32_t interval_ms, uint32_t repeat) noexcept;
mc_status MC_CALL api_cancel_timer(mc_ctx*, uint64_t id) noexcept;
mc_status MC_CALL api_keep_info(mc_ctx*, uint64_t channel, uint64_t message, mc_kept_info*) noexcept;
mc_status MC_CALL api_keep_text(mc_ctx*, uint64_t channel, uint64_t message, uint32_t version, void* out,
                                uint32_t capacity, uint32_t* bytes) noexcept;
mc_status MC_CALL api_keep_restore(mc_ctx*, const mc_kept* items, uint32_t count) noexcept;
mc_status MC_CALL api_read_open_channel(mc_ctx*, mc_open_channel* out) noexcept;
mc_status MC_CALL api_read_messages(mc_ctx*, uint64_t channel, uint64_t before, mc_message_ref* out,
                                    uint32_t capacity, uint32_t* count) noexcept;

} // namespace maicord::plugin_runtime_detail
