// Plugin SDK host, ABI 5: client state reads (mc.read@2), event batches
// (mc.events@1), host-executed actions (mc.actions@1) and background effect
// shaders (mc.fx@1).
#include "plugin_sdk_internal.hpp"
#include "maicord/core/byte_order.hpp"

namespace maicord {
namespace {
using namespace plugin_sdk;

// ---- mc.read@2 ----------------------------------------------------------------------

const NativePluginSdkReadSource* source_for(void* handle, u64 generation, u64 permission, mc_status* status) noexcept {
    auto* binding = active(handle, generation);
    *status = !binding ? MC_E_INACTIVE : !permitted(*binding, permission) ? MC_E_DENIED : MC_OK;
    return *status == MC_OK ? &binding->runtime->read : nullptr;
}
template <typename T, typename Fill> mc_status read_into(T* info, Fill fill) noexcept {
    if (!info) return MC_E_INVALID;
    u32 declared{};
    copy_bytes(&declared, info, sizeof(declared));
    if (declared < 8) return MC_E_INVALID;
    T value{};
    value.size = sizeof(T);
    if (!fill(&value)) return MC_E_NOT_FOUND;
    return write_prefix(info, value);
}
mc_status MC_CALL read_context(void* handle, uint64_t generation, mc_context_info* info) {
    mc_status status{};
    const auto* read = source_for(handle, generation, MC_PERM_READ_CONTEXT, &status);
    if (!read) return status;
    return read_into(info, [&](mc_context_info* value) { return read->context_info && read->context_info(read->context, value); });
}
mc_status MC_CALL read_guild(void* handle, uint64_t generation, uint64_t guild, mc_guild_info* info) {
    mc_status status{};
    const auto* read = source_for(handle, generation, MC_PERM_READ_GUILDS, &status);
    if (!read) return status;
    if (!guild) return MC_E_INVALID;
    return read_into(info, [&](mc_guild_info* value) { return read->guild && read->guild(read->context, guild, value); });
}
mc_status MC_CALL read_channel(void* handle, uint64_t generation, uint64_t channel, mc_channel_info* info) {
    mc_status status{};
    const auto* read = source_for(handle, generation, MC_PERM_READ_GUILDS, &status);
    if (!read) return status;
    if (!channel) return MC_E_INVALID;
    return read_into(info, [&](mc_channel_info* value) { return read->channel && read->channel(read->context, channel, value); });
}
mc_status MC_CALL read_role(void* handle, uint64_t generation, uint64_t guild, uint64_t role, mc_role_info* info) {
    mc_status status{};
    const auto* read = source_for(handle, generation, MC_PERM_READ_GUILDS, &status);
    if (!read) return status;
    if (!guild || !role) return MC_E_INVALID;
    return read_into(info, [&](mc_role_info* value) { return read->role && read->role(read->context, guild, role, value); });
}
mc_status MC_CALL read_member(void* handle, uint64_t generation, uint64_t guild, uint64_t user, mc_member_info* info) {
    mc_status status{};
    const auto* read = source_for(handle, generation, MC_PERM_READ_MEMBERS, &status);
    if (!read) return status;
    if (!guild || !user) return MC_E_INVALID;
    return read_into(info, [&](mc_member_info* value) { return read->member && read->member(read->context, guild, user, value); });
}
mc_status MC_CALL read_user(void* handle, uint64_t generation, uint64_t user, mc_user_info* info) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    const auto& read = binding->runtime->read;
    if (!user) return MC_E_INVALID;
    if (!permitted(*binding, MC_PERM_READ_MEMBERS)) {
        // Your own account is part of the context.
        mc_context_info context{};
        context.size = sizeof(context);
        if (!permitted(*binding, MC_PERM_READ_CONTEXT) || !read.context_info || !read.context_info(read.context, &context) ||
            context.user_id != user) return MC_E_DENIED;
    }
    return read_into(info, [&](mc_user_info* value) { return read.user && read.user(read.context, user, value); });
}
mc_status MC_CALL read_relationship(void* handle, uint64_t generation, uint64_t user, mc_relationship_info* info) {
    mc_status status{};
    const auto* read = source_for(handle, generation, MC_PERM_READ_MEMBERS, &status);
    if (!read) return status;
    if (!user) return MC_E_INVALID;
    return read_into(info, [&](mc_relationship_info* value) {
        return read->relationship && read->relationship(read->context, user, value);
    });
}
mc_status MC_CALL read_profile(void* handle, uint64_t generation, uint64_t user, mc_profile_info* info) {
    mc_status status{};
    const auto* read = source_for(handle, generation, MC_PERM_READ_MEMBERS, &status);
    if (!read) return status;
    if (!user) return MC_E_INVALID;
    return read_into(info, [&](mc_profile_info* value) { return read->profile && read->profile(read->context, user, value); });
}
mc_status MC_CALL read_presence(void* handle, uint64_t generation, uint64_t user, uint64_t guild, mc_presence_info* info) {
    mc_status status{};
    const auto* read = source_for(handle, generation, MC_PERM_READ_PRESENCE, &status);
    if (!read) return status;
    if (!user) return MC_E_INVALID;
    return read_into(info, [&](mc_presence_info* value) {
        return read->presence && read->presence(read->context, user, guild, value);
    });
}
mc_status MC_CALL read_activity(void* handle, uint64_t generation, uint64_t user, uint32_t index, mc_activity_info* info) {
    mc_status status{};
    const auto* read = source_for(handle, generation, MC_PERM_READ_PRESENCE, &status);
    if (!read) return status;
    if (!user) return MC_E_INVALID;
    return read_into(info, [&](mc_activity_info* value) {
        return read->activity && read->activity(read->context, user, index, value);
    });
}
mc_status MC_CALL read_voice(void* handle, uint64_t generation, uint64_t guild, uint64_t user, mc_voice_info* info) {
    mc_status status{};
    const auto* read = source_for(handle, generation, MC_PERM_READ_VOICE, &status);
    if (!read) return status;
    if (!user) return MC_E_INVALID;
    return read_into(info, [&](mc_voice_info* value) { return read->voice && read->voice(read->context, guild, user, value); });
}
mc_status MC_CALL read_message(void* handle, uint64_t generation, uint64_t channel, uint64_t message, mc_message_info* info) {
    mc_status status{};
    const auto* read = source_for(handle, generation, MC_PERM_READ_MESSAGES, &status);
    if (!read) return status;
    if (!info || !message) return MC_E_INVALID;
    mc_message_info request{};
    if (!read_prefix(info, &request, 8)) return MC_E_INVALID;
    mc_message_info value{};
    value.size = sizeof(value);
    Utf8View content{};
    if (!read->message || !read->message(read->context, channel, message, &value, &content)) return MC_E_NOT_FOUND;
    // The caller's buffer fields are inputs: echo them back unchanged.
    value.content = request.content;
    value.content_capacity = request.content_capacity;
    value.content_bytes = content.bytes;
    bool truncated = false;
    if (request.content && request.content_capacity) {
        const u32 bytes = utf8_truncated_length(content, request.content_capacity);
        copy_bytes(request.content, content.data, bytes);
        truncated = bytes < content.bytes;
    } else {
        truncated = content.bytes != 0;
    }
    const mc_status written = write_prefix(info, value);
    return written != MC_OK ? written : truncated && request.content ? MC_E_TRUNCATED : MC_OK;
}
mc_status MC_CALL read_attachment(void* handle, uint64_t generation, uint64_t channel, uint64_t message, uint32_t index,
                                  mc_attachment_info* info) {
    mc_status status{};
    const auto* read = source_for(handle, generation, MC_PERM_READ_MESSAGES, &status);
    if (!read) return status;
    if (!message) return MC_E_INVALID;
    return read_into(info, [&](mc_attachment_info* value) {
        return read->attachment && read->attachment(read->context, channel, message, index, value);
    });
}
mc_status MC_CALL read_embed(void* handle, uint64_t generation, uint64_t channel, uint64_t message, uint32_t index,
                             mc_embed_info* info) {
    mc_status status{};
    const auto* read = source_for(handle, generation, MC_PERM_READ_MESSAGES, &status);
    if (!read) return status;
    if (!message) return MC_E_INVALID;
    return read_into(info, [&](mc_embed_info* value) {
        return read->embed && read->embed(read->context, channel, message, index, value);
    });
}
mc_status MC_CALL read_reaction(void* handle, uint64_t generation, uint64_t channel, uint64_t message, uint32_t index,
                                mc_reaction_info* info) {
    mc_status status{};
    const auto* read = source_for(handle, generation, MC_PERM_READ_MESSAGES, &status);
    if (!read) return status;
    if (!message) return MC_E_INVALID;
    return read_into(info, [&](mc_reaction_info* value) {
        return read->reaction && read->reaction(read->context, channel, message, index, value);
    });
}
u64 list_permission(u32 list) noexcept {
    switch (list) {
    case MC_LIST_GUILDS:
    case MC_LIST_CHANNELS:
    case MC_LIST_ROLES: return MC_PERM_READ_GUILDS;
    case MC_LIST_MEMBERS:
    case MC_LIST_RELATIONSHIPS: return MC_PERM_READ_MEMBERS;
    case MC_LIST_VOICE: return MC_PERM_READ_VOICE;
    case MC_LIST_MESSAGES: return MC_PERM_READ_MESSAGES;
    default: return 0;
    }
}
mc_status MC_CALL read_list(void* handle, uint64_t generation, uint32_t list, uint64_t scope, uint32_t cursor,
                            uint64_t* ids, uint32_t capacity, uint32_t* count, uint32_t* next) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    const u64 permission = list_permission(list);
    if (!permission) return MC_E_INVALID;
    if (!permitted(*binding, permission)) return MC_E_DENIED;
    if (!count || !next || (capacity && !ids)) return MC_E_INVALID;
    *count = 0;
    *next = 0;
    const auto& read = binding->runtime->read;
    u64 local[256];
    u32 copied = 0, after = 0;
    if (!read.list || !read.list(read.context, list, scope, cursor, {local, minimum(capacity, 256u)}, &copied, &after))
        return MC_E_NOT_FOUND;
    copied = minimum(copied, minimum(capacity, 256u));
    copy_bytes(ids, local, copied * 8);
    *count = copied;
    *next = after;
    return MC_OK;
}
mc_status MC_CALL read_revisions(void* handle, uint64_t generation, mc_read_revisions* revisions) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    const auto& read = binding->runtime->read;
    mc_read_revisions value{};
    value.size = sizeof(value);
    if (read.revisions) read.revisions(read.context, &value);
    if (!permitted(*binding, MC_PERM_READ_GUILDS)) value.guilds = 0;
    if (!permitted(*binding, MC_PERM_READ_MEMBERS)) value.members = 0;
    if (!permitted(*binding, MC_PERM_READ_PRESENCE)) value.presence = 0;
    if (!permitted(*binding, MC_PERM_READ_VOICE)) value.voice = 0;
    if (!permitted(*binding, MC_PERM_READ_MESSAGES)) value.messages = 0;
    return write_prefix(revisions, value);
}

// ---- mc.events@1 ------------------------------------------------------------------------

u64 event_permission(u32 kind) noexcept {
    switch (kind) {
    case MC_EVENT_MESSAGE_CREATE:
    case MC_EVENT_MESSAGE_UPDATE:
    case MC_EVENT_MESSAGE_DELETE:
    case MC_EVENT_TYPING:
    case MC_EVENT_REACTION_ADD:
    case MC_EVENT_REACTION_REMOVE: return MC_PERM_READ_MESSAGES;
    case MC_EVENT_PRESENCE: return MC_PERM_READ_PRESENCE;
    case MC_EVENT_VOICE_STATE: return MC_PERM_READ_VOICE;
    case MC_EVENT_CHANNEL_CREATE:
    case MC_EVENT_CHANNEL_UPDATE:
    case MC_EVENT_CHANNEL_DELETE:
    case MC_EVENT_GUILD_CREATE:
    case MC_EVENT_GUILD_UPDATE:
    case MC_EVENT_GUILD_DELETE: return MC_PERM_READ_GUILDS;
    case MC_EVENT_MEMBER_ADD:
    case MC_EVENT_MEMBER_UPDATE:
    case MC_EVENT_MEMBER_REMOVE: return MC_PERM_READ_MEMBERS;
    default: return 0;
    }
}
constexpr u64 kEventKindsAll = (1ull << MC_EVENT_KIND_COUNT) - 1;
mc_status MC_CALL events_subscribe(void* handle, uint64_t generation, uint64_t kinds, mc_event_fn handler) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (kinds & ~kEventKindsAll) return MC_E_INVALID;
    for (u32 kind = 0; kind < MC_EVENT_KIND_COUNT; ++kind)
        if ((kinds & MC_EVENT_MASK(kind)) && !permitted(*binding, event_permission(kind))) return MC_E_DENIED;
    if (!kinds || !handler) {
        binding->event_kinds = 0;
        binding->event_handler = nullptr;
        return MC_OK;
    }
    binding->event_kinds = kinds;
    binding->event_handler = handler;
    binding->event_next = binding->runtime->event_head;
    return MC_OK;
}

bool overlaps(u32 a, u32 a_bytes, u32 b, u32 b_bytes) noexcept {
    return a_bytes && b_bytes && a < b + b_bytes && b < a + a_bytes;
}
// Drops queued events whose text lies in [start, start + bytes).
void reclaim(NativePluginSdkRuntime& runtime, u32 start, u32 bytes) noexcept {
    u64 last = 0;
    bool found = false;
    for (u64 sequence = runtime.event_tail; sequence < runtime.event_head; ++sequence) {
        const auto& record = runtime.events[sequence % kPluginSdkEventQueue];
        if (overlaps(record.text_offset, record.text_bytes + record.name_bytes, start, bytes)) {
            last = sequence;
            found = true;
        }
    }
    if (found) runtime.event_tail = last + 1;
}

// ---- mc.actions@1 -----------------------------------------------------------------------

struct BucketPolicy {
    u32 capacity, window_ms;
};
constexpr BucketPolicy kBuckets[bucket_count]{{5, 5000}, {10, 10000}, {10, 10000}, {2, 10000}, {3, 10000}};
bool take(NativePluginSdkBinding& binding, BucketKind kind) noexcept {
    auto& bucket = binding.buckets[kind];
    const auto policy = kBuckets[kind];
    const u64 now = now_ms(*binding.runtime);
    const u32 full = policy.capacity * 1000;
    if (!bucket.updated_ms) {
        bucket.tokens_milli = full;
    } else if (now > bucket.updated_ms) {
        const u64 refill = (now - bucket.updated_ms) * full / policy.window_ms;
        bucket.tokens_milli = static_cast<u32>(minimum<u64>(full, bucket.tokens_milli + refill));
    }
    bucket.updated_ms = now ? now : 1;
    if (bucket.tokens_milli < 1000) return false;
    bucket.tokens_milli -= 1000;
    return true;
}
NativePluginSdkBinding* action_binding(void* handle, u64 generation, u64 permission, bool gesture, mc_status* status) noexcept {
    auto* binding = active(handle, generation);
    *status = !binding ? MC_E_INACTIVE : !permitted_all(*binding, permission) ? MC_E_DENIED :
              binding->phase != Phase::enabled ? MC_E_INACTIVE :
              gesture && !binding->runtime->gesture_depth ? MC_E_INACTIVE : MC_OK;
    return *status == MC_OK ? binding : nullptr;
}
// "name:id" custom emoji or a Unicode emoji.
bool parse_emoji(mc_str emoji, u64* id, Utf8View* name) noexcept {
    *id = 0;
    *name = {};
    if (!text_ok(emoji, 64, true)) return false;
    u32 colon = emoji.bytes;
    for (u32 i = 0; i < emoji.bytes; ++i)
        if (emoji.data[i] == ':') { colon = i; break; }
    if (colon == emoji.bytes) {
        for (u32 i = 0; i < emoji.bytes; ++i)
            if (emoji.data[i] == ' ' || emoji.data[i] == '<' || emoji.data[i] == '>') return false;
        *name = view(emoji);
        return true;
    }
    if (!colon || colon + 1 >= emoji.bytes || emoji.bytes - colon - 1 > 20) return false;
    for (u32 i = 0; i < colon; ++i) {
        const char c = emoji.data[i];
        if (!ascii_is_alnum(c) && c != '_') return false;
    }
    u64 value = 0;
    for (u32 i = colon + 1; i < emoji.bytes; ++i) {
        const char c = emoji.data[i];
        if (c < '0' || c > '9' || value > (~0ull - 9) / 10) return false;
        value = value * 10 + static_cast<u64>(c - '0');
    }
    if (!value) return false;
    *id = value;
    *name = {emoji.data, colon};
    return true;
}
mc_status MC_CALL action_send(void* handle, uint64_t generation, const mc_send_request* source) {
    mc_status status{};
    auto* binding = action_binding(handle, generation, MC_PERM_ACT_MESSAGES, false, &status);
    if (!binding) return status;
    mc_send_request request{};
    if (!read_prefix(source, &request, offsetof(mc_send_request, text) + sizeof(mc_str)) || !request.channel_id ||
        (request.flags & ~(MC_SEND_SILENT | MC_SEND_MENTION_REPLY)) ||
        !text_ok(request.text, kPluginSdkMessageBytes, true, true)) return MC_E_INVALID;
    const auto& sink = binding->runtime->actions;
    if (!sink.send_message) return MC_E_UNSUPPORTED;
    if (!take(*binding, bucket_messages)) return MC_E_RATE_LIMITED;
    copy_bytes(binding->runtime->output[0], request.text.data, request.text.bytes);
    const NativePluginSdkSend send{request.channel_id, request.reply_to, request.flags,
                                   {binding->runtime->output[0], request.text.bytes}};
    return sink.send_message(sink.context, binding->owner_id, send);
}
mc_status MC_CALL action_edit(void* handle, uint64_t generation, uint64_t channel, uint64_t message, mc_str text) {
    mc_status status{};
    auto* binding = action_binding(handle, generation, MC_PERM_ACT_MESSAGES, false, &status);
    if (!binding) return status;
    if (!message || !text_ok(text, kPluginSdkMessageBytes, true, true)) return MC_E_INVALID;
    const auto& sink = binding->runtime->actions;
    if (!sink.edit_message) return MC_E_UNSUPPORTED;
    if (!take(*binding, bucket_messages)) return MC_E_RATE_LIMITED;
    copy_bytes(binding->runtime->output[0], text.data, text.bytes);
    return sink.edit_message(sink.context, binding->owner_id, channel, message, {binding->runtime->output[0], text.bytes});
}
mc_status MC_CALL action_react(void* handle, uint64_t generation, uint64_t channel, uint64_t message, mc_str emoji) {
    mc_status status{};
    auto* binding = action_binding(handle, generation, MC_PERM_ACT_MESSAGES, false, &status);
    if (!binding) return status;
    u64 emoji_id = 0;
    Utf8View name{};
    if (!message || !parse_emoji(emoji, &emoji_id, &name)) return MC_E_INVALID;
    const auto& sink = binding->runtime->actions;
    if (!sink.react) return MC_E_UNSUPPORTED;
    if (!take(*binding, bucket_reactions)) return MC_E_RATE_LIMITED;
    char copy[64];
    const u32 bytes = copy_text(copy, sizeof(copy), name);
    return sink.react(sink.context, binding->owner_id, channel, message, emoji_id, {copy, bytes});
}
mc_status MC_CALL action_mark_read(void* handle, uint64_t generation, uint64_t channel) {
    mc_status status{};
    auto* binding = action_binding(handle, generation, MC_PERM_ACT_MESSAGES, false, &status);
    if (!binding) return status;
    if (!channel) return MC_E_INVALID;
    const auto& sink = binding->runtime->actions;
    if (!sink.mark_read) return MC_E_UNSUPPORTED;
    if (!take(*binding, bucket_read)) return MC_E_RATE_LIMITED;
    return sink.mark_read(sink.context, binding->owner_id, channel);
}
mc_status MC_CALL action_custom_status(void* handle, uint64_t generation, mc_str text, mc_str emoji, uint64_t expires) {
    mc_status status{};
    auto* binding = action_binding(handle, generation, MC_PERM_ACT_PRESENCE, false, &status);
    if (!binding) return status;
    u64 emoji_id = 0;
    Utf8View emoji_name{};
    if (!text_ok(text, 128, false) || (emoji.bytes && !parse_emoji(emoji, &emoji_id, &emoji_name))) return MC_E_INVALID;
    const auto& sink = binding->runtime->actions;
    if (!sink.custom_status) return MC_E_UNSUPPORTED;
    if (!take(*binding, bucket_presence)) return MC_E_RATE_LIMITED;
    char copy[200];
    const u32 text_bytes = copy_text(copy, 128, view(text));
    const u32 emoji_bytes = copy_text(copy + text_bytes, 64, view(emoji));
    return sink.custom_status(sink.context, binding->owner_id, {copy, text_bytes}, {copy + text_bytes, emoji_bytes}, expires);
}
mc_status MC_CALL action_activity(void* handle, uint64_t generation, const mc_activity* source) {
    mc_status status{};
    auto* binding = action_binding(handle, generation, MC_PERM_ACT_PRESENCE, false, &status);
    if (!binding) return status;
    const auto& sink = binding->runtime->actions;
    if (!sink.activity) return MC_E_UNSUPPORTED;
    if (!source) return sink.activity(sink.context, binding->owner_id, nullptr);
    mc_activity activity{};
    if (!read_prefix(source, &activity, offsetof(mc_activity, name) + sizeof(mc_str))) return MC_E_INVALID;
    if ((activity.type != MC_ACTIVITY_PLAYING && activity.type != MC_ACTIVITY_LISTENING &&
         activity.type != MC_ACTIVITY_WATCHING && activity.type != MC_ACTIVITY_COMPETING) ||
        !text_ok(activity.name, 128, true) || !text_ok(activity.details, 128, false) || !text_ok(activity.state, 128, false) ||
        (activity.end_ms && activity.end_ms < activity.start_ms)) return MC_E_INVALID;
    if (!take(*binding, bucket_presence)) return MC_E_RATE_LIMITED;
    char copy[384];
    const u32 name = copy_text(copy, 128, view(activity.name));
    const u32 details = copy_text(copy + 128, 128, view(activity.details));
    const u32 state = copy_text(copy + 256, 128, view(activity.state));
    const NativePluginSdkActivity value{activity.application_id, activity.start_ms, activity.end_ms, activity.type,
                                        {copy, name}, {copy + 128, details}, {copy + 256, state}};
    return sink.activity(sink.context, binding->owner_id, &value);
}
mc_status MC_CALL action_open_channel(void* handle, uint64_t generation, uint64_t channel, uint64_t message) {
    mc_status status{};
    auto* binding = action_binding(handle, generation, MC_PERM_NAVIGATE, true, &status);
    if (!binding) return status;
    if (!channel) return MC_E_INVALID;
    const auto& sink = binding->runtime->actions;
    return sink.open_channel ? sink.open_channel(sink.context, binding->owner_id, channel, message) : MC_E_UNSUPPORTED;
}
mc_status MC_CALL action_open_profile(void* handle, uint64_t generation, uint64_t user, uint64_t guild) {
    mc_status status{};
    auto* binding = action_binding(handle, generation, MC_PERM_NAVIGATE, true, &status);
    if (!binding) return status;
    if (!user) return MC_E_INVALID;
    const auto& sink = binding->runtime->actions;
    return sink.open_profile ? sink.open_profile(sink.context, binding->owner_id, user, guild) : MC_E_UNSUPPORTED;
}
mc_status MC_CALL action_open_page(void* handle, uint64_t generation, uint64_t page_id) {
    mc_status status{};
    auto* binding = action_binding(handle, generation, MC_PERM_NAVIGATE | MC_PERM_SETTINGS, true, &status);
    if (!binding) return status;
    bool found = false;
    for (const auto& page : binding->pages) found = found || (page.registered && page.page_id == page_id);
    if (!found) return MC_E_NOT_FOUND;
    const auto& sink = binding->runtime->actions;
    return sink.open_page ? sink.open_page(sink.context, {binding->owner_id, page_id}) : MC_E_UNSUPPORTED;
}
mc_status MC_CALL action_copy(void* handle, uint64_t generation, mc_str text) {
    mc_status status{};
    auto* binding = action_binding(handle, generation, MC_PERM_CLIPBOARD, true, &status);
    if (!binding) return status;
    if (!text.bytes || text.bytes > kPluginSdkClipboardBytes || !text.data || !utf8_validate(view(text)).valid)
        return MC_E_INVALID;
    const auto& sink = binding->runtime->actions;
    if (!sink.copy_text) return MC_E_UNSUPPORTED;
    copy_bytes(binding->runtime->input, text.data, text.bytes);
    return sink.copy_text(sink.context, binding->owner_id, {binding->runtime->input, text.bytes});
}
mc_status MC_CALL action_open_url(void* handle, uint64_t generation, mc_str url) {
    mc_status status{};
    auto* binding = action_binding(handle, generation, MC_PERM_OPEN_LINKS, true, &status);
    if (!binding) return status;
    if (!url.data || !url_ok(view(url))) return MC_E_INVALID;
    const auto& sink = binding->runtime->actions;
    if (!sink.open_url) return MC_E_UNSUPPORTED;
    copy_bytes(binding->runtime->input, url.data, url.bytes);
    return sink.open_url(sink.context, binding->owner_id, {binding->runtime->input, url.bytes});
}
mc_status MC_CALL action_notice(void* handle, uint64_t generation, mc_str text, uint32_t tone) {
    mc_status status{};
    auto* binding = action_binding(handle, generation, MC_PERM_NOTIFY, false, &status);
    if (!binding) return status;
    if (!text_ok(text, kPluginSdkNoticeBytes, true) || tone > MC_TONE_CRITICAL) return MC_E_INVALID;
    const auto& sink = binding->runtime->actions;
    if (!sink.notice) return MC_E_UNSUPPORTED;
    if (!take(*binding, bucket_notice)) return MC_E_RATE_LIMITED;
    char copy[kPluginSdkNoticeBytes];
    const u32 bytes = copy_text(copy, sizeof(copy), view(text));
    return sink.notice(sink.context, binding->owner_id, {copy, bytes}, tone);
}
mc_status MC_CALL action_open_modal(void* handle, uint64_t generation, const mc_modal* source) {
    mc_status status{};
    auto* binding = action_binding(handle, generation, MC_PERM_NOTIFY, true, &status);
    if (!binding) return status;
    mc_modal modal{};
    if (!read_prefix(source, &modal, offsetof(mc_modal, input_max_bytes)) || !text_ok(modal.title, 64, true) ||
        modal.node_count > kPluginSdkModalNodes || (modal.node_count && (!modal.nodes ||
        modal.node_stride < offsetof(mc_slot_node, tooltip))) || modal.input_max_bytes > kPluginSdkMessageBytes)
        return MC_E_INVALID;
    if (binding->modal_id) return MC_E_BUSY;
    const auto& sink = binding->runtime->actions;
    if (!sink.open_modal) return MC_E_UNSUPPORTED;
    // Copies live in the runtime output buffers until the sink returns.
    auto& runtime = *binding->runtime;
    NativePluginSdkModal copy{};
    copy.owner_id = binding->owner_id;
    copy.modal_id = modal.modal_id;
    copy.input_max_bytes = modal.input_max_bytes ? modal.input_max_bytes : 256;
    u32 at = 0, inputs = 0, buttons = 0;
    char* text = runtime.output[0];
    const auto put = [&](Utf8View value) noexcept -> Utf8View {
        const u32 bytes = minimum(value.bytes, kPluginSdkOutputBytes - at);
        copy_bytes(text + at, value.data, bytes);
        at += bytes;
        return {text + at - bytes, bytes};
    };
    copy.title = put(view(modal.title));
    const NodeLimits limits{kNodeLabelBytes, kNodeValueBytes,
                            (1u << MC_NODE_LABEL) | (1u << MC_NODE_INPUT) | (1u << MC_NODE_BUTTON) |
                                (1u << MC_NODE_LINK) | (1u << MC_NODE_SEPARATOR)};
    for (u32 i = 0; i < modal.node_count; ++i) {
        mc_slot_node node{};
        if (!read_node(modal.nodes, modal.node_stride, i, limits, &node)) return MC_E_INVALID;
        inputs += node.kind == MC_NODE_INPUT;
        buttons += node.kind == MC_NODE_BUTTON;
        if (inputs > 1 || buttons > 1 || (node.kind == MC_NODE_INPUT && node.value.bytes > copy.input_max_bytes))
            return MC_E_INVALID;
        copy.nodes[i] = {node.kind, node.tone, put(view(node.text)), put(view(node.value))};
    }
    copy.node_count = modal.node_count;
    const mc_status opened = sink.open_modal(sink.context, copy);
    if (opened == MC_OK) binding->modal_id = modal.modal_id ? modal.modal_id : ~0ull;
    return opened;
}
mc_status MC_CALL action_set_modal_handler(void* handle, uint64_t generation, mc_modal_fn handler) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_NOTIFY)) return MC_E_DENIED;
    binding->modal_handler = handler;
    return MC_OK;
}

// ---- mc.fx@1 ---------------------------------------------------------------------------

Effect* find_effect(NativePluginSdkBinding& binding, u64 id) noexcept {
    for (auto& effect : binding.effects)
        if (effect.registered && effect.id == id) return &effect;
    return nullptr;
}
mc_status MC_CALL fx_register(void* handle, uint64_t generation, const mc_fx_effect* source) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_EFFECTS)) return MC_E_DENIED;
    mc_fx_effect effect{};
    if (!read_prefix(source, &effect, offsetof(mc_fx_effect, parameter_count)) || !effect.effect_id ||
        !text_ok(effect.name, kPluginSdkEffectNameBytes, true) || !effect.bytecode || !effect.bytecode_bytes ||
        effect.parameter_count > MC_FX_PARAMETERS) return MC_E_INVALID;
    if (effect.bytecode_bytes > kPluginSdkEffectBytecodeBytes) return MC_E_CAPACITY;
    if (!native_plugin_sdk_effect_bytecode_valid({static_cast<const u8*>(effect.bytecode), effect.bytecode_bytes}))
        return MC_E_INVALID;
    for (u32 i = 0; i < effect.parameter_count; ++i)
        if (!is_finite(effect.parameters[i])) return MC_E_INVALID;
    Effect* slot = find_effect(*binding, effect.effect_id);
    if (!slot)
        for (auto& candidate : binding->effects)
            if (!candidate.registered) { slot = &candidate; break; }
    if (!slot) return MC_E_BUDGET;
    const u32 revision = slot->revision + 1;
    slot->id = effect.effect_id;
    slot->registered = true;
    slot->name_bytes = copy_text(slot->name, kPluginSdkEffectNameBytes, view(effect.name));
    slot->bytecode_bytes = effect.bytecode_bytes;
    copy_bytes(slot->bytecode, effect.bytecode, effect.bytecode_bytes);
    clear_bytes(slot->parameters, sizeof(slot->parameters));
    copy_bytes(slot->parameters, effect.parameters, effect.parameter_count * 4);
    slot->revision = revision;
    ++binding->runtime->effect_revision;
    return MC_OK;
}
mc_status MC_CALL fx_unregister(void* handle, uint64_t generation, uint64_t id) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_EFFECTS)) return MC_E_DENIED;
    Effect* effect = find_effect(*binding, id);
    if (!effect) return MC_E_NOT_FOUND;
    effect->registered = false;
    effect->bytecode_bytes = 0;
    ++binding->runtime->effect_revision;
    return MC_OK;
}
mc_status MC_CALL fx_parameters(void* handle, uint64_t generation, uint64_t id, const float* values, uint32_t count) {
    auto* binding = active(handle, generation);
    if (!binding) return MC_E_INACTIVE;
    if (!permitted(*binding, MC_PERM_EFFECTS)) return MC_E_DENIED;
    if (count > MC_FX_PARAMETERS || (count && !values)) return MC_E_INVALID;
    float copy[MC_FX_PARAMETERS]{};
    copy_bytes(copy, values, count * 4);
    for (u32 i = 0; i < count; ++i)
        if (!is_finite(copy[i])) return MC_E_INVALID;
    Effect* effect = find_effect(*binding, id);
    if (!effect) return MC_E_NOT_FOUND;
    copy_bytes(effect->parameters, copy, count * 4);
    ++effect->revision;
    ++binding->runtime->effect_revision;
    return MC_OK;
}

} // namespace

namespace plugin_sdk {
const mc_read_v2 kRead{sizeof(mc_read_v2), 2, read_context, read_guild, read_channel, read_role, read_member, read_user,
                       read_relationship, read_profile, read_presence, read_activity, read_voice, read_message,
                       read_attachment, read_embed, read_reaction, read_list, read_revisions};
const mc_events_v1 kEvents{sizeof(mc_events_v1), 1, events_subscribe};
const mc_actions_v1 kActions{sizeof(mc_actions_v1), 1, action_send, action_edit, action_react, action_mark_read,
                             action_custom_status, action_activity, action_open_channel, action_open_profile,
                             action_open_page, action_copy, action_open_url, action_notice, action_open_modal,
                             action_set_modal_handler};
const mc_fx_v1 kFx{sizeof(mc_fx_v1), 1, fx_register, fx_unregister, fx_parameters};

void data_reset(NativePluginSdkBinding& binding) noexcept {
    auto& runtime = *binding.runtime;
    bool effects = false;
    for (auto& effect : binding.effects)
        if (effect.registered) {
            effects = true;
            effect.registered = false;
            effect.bytecode_bytes = 0;
        }
    if (effects) ++runtime.effect_revision;
    // A plugin's activity never outlives it.
    if (permitted(binding, MC_PERM_ACT_PRESENCE) && runtime.actions.activity)
        runtime.actions.activity(runtime.actions.context, binding.owner_id, nullptr);
    binding.event_kinds = 0;
    binding.event_handler = nullptr;
    binding.modal_handler = nullptr;
    binding.modal_id = 0;
}
} // namespace plugin_sdk

// ---- Public API ---------------------------------------------------------------------------

u64 native_plugin_sdk_event_interest(const NativePluginSdkRuntime& runtime) noexcept {
    u64 interest = 0;
    for (const auto* binding : g_live)
        if (usable(binding, runtime) && binding->event_handler) interest |= binding->event_kinds;
    return interest;
}

bool native_plugin_sdk_event_push(NativePluginSdkRuntime* runtime, const NativePluginSdkEvent& event) noexcept {
    if (!runtime || event.kind >= MC_EVENT_KIND_COUNT ||
        !(native_plugin_sdk_event_interest(*runtime) & MC_EVENT_MASK(event.kind))) return false;
    const u32 text_bytes = event.text.data ? utf8_truncated_length(event.text, kPluginSdkEventContentBytes) : 0;
    const u32 name_bytes = event.name.data ? utf8_truncated_length(event.name, 128) : 0;
    const u32 bytes = text_bytes + name_bytes;
    u32 start = runtime->event_text_head;
    if (start + bytes > kPluginSdkEventTextBytes) start = 0;
    if (runtime->event_head - runtime->event_tail >= kPluginSdkEventQueue) ++runtime->event_tail;
    if (bytes) reclaim(*runtime, start, bytes);
    copy_bytes(runtime->event_text + start, event.text.data, text_bytes);
    copy_bytes(runtime->event_text + start + text_bytes, event.name.data, name_bytes);
    runtime->event_text_head = start + bytes;
    auto& record = runtime->events[runtime->event_head % kPluginSdkEventQueue];
    record = {};
    record.sequence = runtime->event_head;
    record.kind = event.kind;
    record.flags = event.flags & (MC_EVENT_OWN | MC_EVENT_BOT | MC_EVENT_BULK | MC_EVENT_CACHED | MC_EVENT_TEXT);
    if (text_bytes) record.flags |= MC_EVENT_TEXT;
    record.guild_id = event.guild_id;
    record.channel_id = event.channel_id;
    record.message_id = event.message_id;
    record.user_id = event.user_id;
    record.value = event.value;
    record.time_ms = runtime->unix_ms ? runtime->unix_ms() : 0;
    record.text_offset = start;
    record.text_bytes = text_bytes;
    record.name_offset = start + text_bytes;
    record.name_bytes = name_bytes;
    ++runtime->event_head;
    return true;
}

bool native_plugin_sdk_event_deliver(NativePluginSdkRuntime* runtime) noexcept {
    if (!runtime) return false;
    bool pending = false;
    u64 oldest = runtime->event_head;
    for (auto* binding : g_live) {
        if (!usable(binding, *runtime) || !binding->event_handler || !binding->event_kinds) continue;
        for (u32 call = 0; call < kPluginSdkEventCallsPerPass && binding->event_next < runtime->event_head; ++call) {
            if (!callable(*binding, reinterpret_cast<const void*>(binding->event_handler))) break;
            mc_event batch[kPluginSdkEventBatch];
            u32 count = 0, text_used = 0;
            if (binding->event_next < runtime->event_tail) {
                auto& overflow = batch[count++];
                overflow = {};
                overflow.size = sizeof(mc_event);
                overflow.kind = MC_EVENT_OVERFLOW;
                overflow.value = runtime->event_tail - binding->event_next;
                overflow.sequence = runtime->event_tail;
                overflow.time_ms = runtime->unix_ms ? runtime->unix_ms() : 0;
                binding->event_next = runtime->event_tail;
            }
            while (count < kPluginSdkEventBatch && binding->event_next < runtime->event_head) {
                const auto& record = runtime->events[binding->event_next % kPluginSdkEventQueue];
                if (!(binding->event_kinds & MC_EVENT_MASK(record.kind))) {
                    ++binding->event_next;
                    continue;
                }
                // Copy-only: texts go into the runtime input buffer for this call.
                const u32 bytes = record.text_bytes + record.name_bytes;
                if (text_used + bytes > kPluginSdkInputBytes) break;
                copy_bytes(runtime->input + text_used, runtime->event_text + record.text_offset, bytes);
                auto& out = batch[count++];
                out = {};
                out.size = sizeof(mc_event);
                out.kind = record.kind;
                out.sequence = record.sequence;
                out.guild_id = record.guild_id;
                out.channel_id = record.channel_id;
                out.message_id = record.message_id;
                out.user_id = record.user_id;
                out.value = record.value;
                out.time_ms = record.time_ms;
                out.flags = record.flags;
                out.text = {runtime->input + text_used, record.text_bytes};
                out.name = {runtime->input + text_used + record.text_bytes, record.name_bytes};
                text_used += bytes;
                ++binding->event_next;
            }
            if (!count) break;
            Call call_scope(*binding, PluginCallKind::event);
            binding->event_handler(binding->plugin_state, binding->plugin_state_bytes, batch, count, sizeof(mc_event));
        }
        if (binding->event_next < runtime->event_head) pending = true;
        oldest = minimum(oldest, binding->event_next);
    }
    // Everything every subscriber has seen is released.
    if (oldest > runtime->event_tail) runtime->event_tail = minimum(oldest, runtime->event_head);
    return pending;
}

bool native_plugin_sdk_modal_result(NativePluginSdkRuntime* runtime, u64 owner_id, u64 modal_id, bool accepted,
                                    Utf8View text) noexcept {
    auto* binding = runtime ? binding_for(*runtime, owner_id) : nullptr;
    if (!binding || !binding->modal_id || (binding->modal_id != modal_id && !(binding->modal_id == ~0ull && !modal_id)))
        return false;
    binding->modal_id = 0;
    if (!callable(*binding, reinterpret_cast<const void*>(binding->modal_handler))) return false;
    const u32 bytes = accepted ? utf8_truncated_length(text, kPluginSdkMessageBytes) : 0;
    copy_bytes(runtime->input, text.data, bytes);
    mc_modal_result result{};
    result.size = sizeof(result);
    result.accepted = accepted ? 1u : 0u;
    result.modal_id = modal_id;
    result.text = {runtime->input, bytes};
    mc_status status{};
    {
        Call call(*binding, PluginCallKind::modal);
        Gesture gesture(runtime);
        status = binding->modal_handler(binding->plugin_state, binding->plugin_state_bytes, &result);
    }
    return status == MC_OK;
}

u32 native_plugin_sdk_effects(const NativePluginSdkRuntime& runtime, Slice<NativePluginSdkEffectView> output) noexcept {
    u32 count = 0;
    for (const auto* binding : g_live) {
        if (!usable(binding, runtime) || !permitted(*binding, MC_PERM_EFFECTS)) continue;
        for (const auto& effect : binding->effects)
            if (effect.registered && count < output.count)
                output[count++] = {binding->owner_id, effect.id, {effect.name, effect.name_bytes},
                                   {effect.bytecode, effect.bytecode_bytes}, effect.parameters, effect.revision};
    }
    return count;
}

bool native_plugin_sdk_effect(const NativePluginSdkRuntime& runtime, u64 owner_id, u64 effect_id,
                              NativePluginSdkEffectView* output) noexcept {
    if (output) *output = {};
    const auto* binding = binding_for(runtime, owner_id);
    if (!output || !binding || !usable(binding, runtime) || !permitted(*binding, MC_PERM_EFFECTS)) return false;
    for (const auto& effect : binding->effects)
        if (effect.registered && effect.id == effect_id) {
            *output = {owner_id, effect.id, {effect.name, effect.name_bytes}, {effect.bytecode, effect.bytecode_bytes},
                       effect.parameters, effect.revision};
            return true;
        }
    return false;
}

bool native_plugin_sdk_effect_fault(NativePluginSdkRuntime* runtime, u64 owner_id, u64 effect_id) noexcept {
    auto* binding = runtime ? binding_for(*runtime, owner_id) : nullptr;
    Effect* effect = binding ? find_effect(*binding, effect_id) : nullptr;
    if (!effect) return false;
    effect->registered = false;
    effect->bytecode_bytes = 0;
    ++runtime->effect_revision;
    return true;
}

bool native_plugin_sdk_effect_bytecode_valid(ConstSlice<u8> code) noexcept {
    // DXBC: "DXBC", 16-byte checksum, 1, total size, chunk count, chunk offsets.
    if (!code.data || code.count < 32 || code.count > kPluginSdkEffectBytecodeBytes ||
        !bytes_equal(code.data, "DXBC", 4) || byte_load_le32(code.data + 20) != 1 ||
        byte_load_le32(code.data + 24) != code.count) return false;
    const u32 chunks = byte_load_le32(code.data + 28);
    if (!chunks || chunks > 32 || 32 + chunks * 4 > code.count) return false;
    bool shader = false;
    for (u32 i = 0; i < chunks; ++i) {
        const u32 offset = byte_load_le32(code.data + 32 + i * 4);
        if (offset < 32 + chunks * 4 || offset > code.count - 8) return false;
        const u32 bytes = byte_load_le32(code.data + offset + 4);
        if (bytes > code.count - offset - 8) return false;
        const u8* fourcc = code.data + offset;
        if (bytes_equal(fourcc, "SHEX", 4) || bytes_equal(fourcc, "SHDR", 4)) {
            if (bytes < 8 || shader) return false;
            const u32 version = byte_load_le32(code.data + offset + 8);
            // Program type 0 (pixel), shader model 5.0.
            if ((version >> 16) != 0 || ((version >> 4) & 0xF) != 5 || (version & 0xF) != 0) return false;
            shader = true;
        }
    }
    return shader;
}

} // namespace maicord
