#include "fake_actions.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace plugin_fake_actions {
Recorded recorded;
} // namespace plugin_fake_actions
namespace {

using plugin_fake_actions::recorded;

void note(u64 owner, u64 channel, u64 message_id) noexcept {
    recorded.owner = owner;
    recorded.channel = channel;
    recorded.message = message_id;
}

// Copies the text into storage that holds it with a terminator; 0 bytes (and a cleared storage) when it does not fit.
template <u32 N> u32 keep(char (&storage)[N], Utf8View text) noexcept {
    storage[0] = 0;
    if (text.bytes >= N) return 0;
    copy_bytes(storage, text.data, text.bytes);
    storage[text.bytes] = 0;
    return text.bytes;
}

} // namespace

mc_status plugin_host_send(u64 owner_id, const PluginSend& send) noexcept {
    ++recorded.sends;
    note(owner_id, send.channel_id, send.reply_to);
    recorded.flags = send.flags;
    keep(recorded.text, send.text);
    return recorded.result;
}

mc_status plugin_host_edit(u64 owner_id, u64 channel_id, u64 message_id, Utf8View) noexcept {
    ++recorded.edits;
    note(owner_id, channel_id, message_id);
    return recorded.result;
}

mc_status plugin_host_react(u64 owner_id, u64 channel_id, u64 message_id, Utf8View, bool) noexcept {
    ++recorded.reacts;
    note(owner_id, channel_id, message_id);
    return recorded.result;
}

mc_status plugin_host_delete_own(u64 owner_id, u64 channel_id, u64 message_id) noexcept {
    ++recorded.deletes;
    note(owner_id, channel_id, message_id);
    return recorded.result;
}

mc_status plugin_host_mark_read(u64 owner_id, u32, u64 id) noexcept {
    ++recorded.reads;
    note(owner_id, id, 0);
    return recorded.result;
}

mc_status plugin_host_set_activity(u64, const mc_activity* activity) noexcept {
    ++(activity ? recorded.activities : recorded.cleared);
    return recorded.result;
}

mc_status plugin_host_local_message(u64 owner_id, u64 channel_id, Utf8View text, u64* message_id) noexcept {
    ++recorded.locals;
    note(owner_id, channel_id, 0);
    recorded.local_bytes = keep(recorded.local, text);
    *message_id = 4242;
    return recorded.result;
}

mc_status plugin_host_local_message_remove(u64, u64, u64) noexcept {
    ++recorded.local_removes;
    return recorded.result;
}

mc_status plugin_host_composer_text(void*, u32 capacity, u32* bytes) noexcept {
    ++recorded.composer;
    *bytes = 5;
    return capacity < 5 ? MC_E_TRUNCATED : MC_OK;
}

mc_status plugin_host_composer_insert(Utf8View text) noexcept {
    ++recorded.composer;
    recorded.inserted_bytes = keep(recorded.inserted, text);
    return recorded.result;
}

mc_status plugin_host_composer_replace(Utf8View) noexcept {
    ++recorded.composer;
    return recorded.result;
}

mc_status plugin_host_composer_focus() noexcept {
    ++recorded.composer;
    return recorded.result;
}

mc_status plugin_host_composer_reply(u64, u64 message_id, bool) noexcept {
    ++recorded.composer;
    recorded.message = message_id;
    return recorded.result;
}

mc_status plugin_host_composer_edit(u64, u64) noexcept {
    ++recorded.composer;
    return recorded.result;
}

void plugin_host_keep_changed() noexcept {
    ++recorded.keep_changed;
}

mc_status plugin_host_read_open_channel(mc_open_channel* out) noexcept {
    *out = {9, 4, 31, 0};
    return MC_OK;
}

mc_status plugin_host_read_messages(u64 channel_id, u64 before, mc_message_ref* out, u32 capacity, u32* count) noexcept {
    if (channel_id != 9 || before) return MC_E_NOT_FOUND;
    for (u64 id = 13; id > 10 && *count < capacity; --id) out[(*count)++] = {id, 5, 1000 + id, MC_MESSAGE_OWN, 0};
    return MC_OK;
}

} // namespace maicord
