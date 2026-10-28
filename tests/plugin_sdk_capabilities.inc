// ABI 5 capabilities: reads, events, decorators, actions, keybinds, slots v2,
// effects and the in-tree showcase sample. Included by tests/plugin_sdk.cpp
// inside namespace maicord::plugin_sdk_tests (shares its host fixture).

// ---- Fake host sources ---------------------------------------------------------------

constexpr u64 kSelf = 99, kAlice = 6, kChannel = 77, kGuild = 88;
template <u32 N> u32 fill_text(char (&out)[N], const char* text) noexcept {
    u32 bytes = 0;
    while (text[bytes] && bytes < N) {
        out[bytes] = text[bytes];
        ++bytes;
    }
    return bytes;
}
struct FakeMessage {
    u64 id, author;
    const char* text;
};
constexpr FakeMessage kMessages[]{{601, kSelf, "my own words"}, {600, kAlice, "hello there, friend \xC3\xA9t\xC3\xA9"}};
u32 g_reads;
bool fake_context(void*, mc_context_info* info) noexcept {
    ++g_reads;
    info->channel_id = kChannel;
    info->guild_id = kGuild;
    info->user_id = kSelf;
    info->channel_name_bytes = fill_text(info->channel_name, "general");
    info->user_name_bytes = fill_text(info->user_name, "tester");
    return true;
}
bool fake_guild(void*, u64 guild, mc_guild_info* info) noexcept {
    if (guild != kGuild) return false;
    info->guild_id = guild;
    info->owner_id = kSelf;
    info->flags = MC_GUILD_OWNER | MC_GUILD_COUNTS;
    info->member_count = 12;
    info->online_count = 5;
    info->name_bytes = fill_text(info->name, "Test guild");
    return true;
}
bool fake_member(void*, u64 guild, u64 user, mc_member_info* info) noexcept {
    if (guild != kGuild || user != kAlice) return false;
    info->guild_id = guild;
    info->user_id = user;
    info->role_count = 1;
    info->role_ids[0] = 4;
    info->flags = MC_MEMBER_ROLES;
    info->nick_bytes = fill_text(info->nick, "Ally");
    return true;
}
bool fake_user(void*, u64 user, mc_user_info* info) noexcept {
    info->user_id = user;
    if (user == kSelf) {
        info->flags = MC_USER_SELF;
        info->name_bytes = fill_text(info->name, "tester");
        info->username_bytes = fill_text(info->username, "tester");
        return true;
    }
    if (user != kAlice) return false;
    info->name_bytes = fill_text(info->name, "Alice A");
    info->username_bytes = fill_text(info->username, "alice");
    return true;
}
bool fake_presence(void*, u64 user, u64, mc_presence_info* info) noexcept {
    if (user != kAlice) return false;
    info->user_id = user;
    info->status = MC_STATUS_IDLE;
    info->desktop = MC_STATUS_OFFLINE;
    info->mobile = MC_STATUS_IDLE;
    info->web = MC_STATUS_OFFLINE;
    return true;
}
bool fake_voice(void*, u64, u64 user, mc_voice_info* info) noexcept {
    if (user != kAlice) return false;
    info->user_id = user;
    info->guild_id = kGuild;
    info->channel_id = 500;
    info->flags = MC_VOICE_SELF_MUTE;
    return true;
}
bool fake_message(void*, u64 channel, u64 message, mc_message_info* info, Utf8View* content) noexcept {
    if (channel != kChannel) return false;
    for (const auto& entry : kMessages)
        if (entry.id == message) {
            info->channel_id = channel;
            info->message_id = message;
            info->author_id = entry.author;
            info->guild_id = kGuild;
            info->flags = entry.author == kSelf ? MC_MESSAGE_OWN : 0u;
            info->revision = 3;
            info->author_name_bytes = fill_text(info->author_name, entry.author == kSelf ? "tester" : "Alice A");
            *content = {entry.text, c_string_length(entry.text)};
            return true;
        }
    return false;
}
bool fake_list(void*, u32 list, u64 scope, u32 cursor, Slice<u64> ids, u32* count, u32* next) noexcept {
    if (list != MC_LIST_MESSAGES || scope != kChannel) return false;
    constexpr u32 total = sizeof(kMessages) / sizeof(kMessages[0]);
    u32 copied = 0;
    while (cursor + copied < total && copied < ids.count) {
        ids[copied] = kMessages[cursor + copied].id;
        ++copied;
    }
    *count = copied;
    *next = cursor + copied < total ? cursor + copied : 0;
    return true;
}
void fake_revisions(void*, mc_read_revisions* revisions) noexcept {
    revisions->guilds = 11;
    revisions->members = 12;
    revisions->presence = 13;
    revisions->voice = 14;
    revisions->messages = 15;
}
void install_fake_read() noexcept {
    g_runtime.read = {};
    g_runtime.read.context_info = fake_context;
    g_runtime.read.guild = fake_guild;
    g_runtime.read.member = fake_member;
    g_runtime.read.user = fake_user;
    g_runtime.read.presence = fake_presence;
    g_runtime.read.voice = fake_voice;
    g_runtime.read.message = fake_message;
    g_runtime.read.list = fake_list;
    g_runtime.read.revisions = fake_revisions;
}

struct SinkLog {
    u32 sends, edits, reacts, reads, statuses, activities, cleared, opens, profiles, pages, copies, urls, notices, modals;
    u64 last_owner, last_channel, last_reply, last_emoji;
    u32 last_flags, last_tone, last_type;
    char text[2048];
    u32 text_bytes;
    NativePluginSdkModal modal;
    char modal_text[1024];
};
SinkLog g_sink;
void sink_text(Utf8View text) noexcept {
    g_sink.text_bytes = minimum(text.bytes, static_cast<u32>(sizeof(g_sink.text)));
    copy_bytes(g_sink.text, text.data, g_sink.text_bytes);
}
mc_status sink_send(void*, u64 owner, const NativePluginSdkSend& send) noexcept {
    ++g_sink.sends;
    g_sink.last_owner = owner;
    g_sink.last_channel = send.channel_id;
    g_sink.last_reply = send.reply_to;
    g_sink.last_flags = send.flags;
    sink_text(send.text);
    return MC_OK;
}
mc_status sink_edit(void*, u64, u64, u64, Utf8View text) noexcept { ++g_sink.edits; sink_text(text); return MC_OK; }
mc_status sink_react(void*, u64, u64, u64, u64 emoji, Utf8View name) noexcept {
    ++g_sink.reacts;
    g_sink.last_emoji = emoji;
    sink_text(name);
    return MC_OK;
}
mc_status sink_read(void*, u64, u64) noexcept { ++g_sink.reads; return MC_OK; }
mc_status sink_status(void*, u64, Utf8View text, Utf8View, u64) noexcept { ++g_sink.statuses; sink_text(text); return MC_OK; }
mc_status sink_activity(void*, u64, const NativePluginSdkActivity* activity) noexcept {
    if (!activity) { ++g_sink.cleared; return MC_OK; }
    ++g_sink.activities;
    g_sink.last_type = activity->type;
    sink_text(activity->name);
    return MC_OK;
}
mc_status sink_open(void*, u64, u64 channel, u64) noexcept { ++g_sink.opens; g_sink.last_channel = channel; return MC_OK; }
mc_status sink_profile(void*, u64, u64, u64) noexcept { ++g_sink.profiles; return MC_OK; }
mc_status sink_page(void*, UiPageKey) noexcept { ++g_sink.pages; return MC_OK; }
mc_status sink_copy(void*, u64, Utf8View text) noexcept { ++g_sink.copies; sink_text(text); return MC_OK; }
mc_status sink_url(void*, u64, Utf8View url) noexcept { ++g_sink.urls; sink_text(url); return MC_OK; }
mc_status sink_notice(void*, u64, Utf8View text, u32 tone) noexcept {
    ++g_sink.notices;
    g_sink.last_tone = tone;
    sink_text(text);
    return MC_OK;
}
mc_status sink_modal(void*, const NativePluginSdkModal& modal) noexcept {
    ++g_sink.modals;
    g_sink.modal = modal;
    // Keep the strings: the request only borrows them until return.
    u32 at = 0;
    const auto keep = [&](Utf8View& text) noexcept {
        const u32 bytes = minimum(text.bytes, static_cast<u32>(sizeof(g_sink.modal_text)) - at);
        copy_bytes(g_sink.modal_text + at, text.data, bytes);
        text = {g_sink.modal_text + at, bytes};
        at += bytes;
    };
    keep(g_sink.modal.title);
    for (u32 i = 0; i < g_sink.modal.node_count; ++i) {
        keep(g_sink.modal.nodes[i].text);
        keep(g_sink.modal.nodes[i].value);
    }
    return MC_OK;
}
void install_fake_sink() noexcept {
    g_sink = {};
    g_runtime.actions = {nullptr, sink_send, sink_edit, sink_react, sink_read, sink_status, sink_activity, sink_open,
                         sink_profile, sink_page, sink_copy, sink_url, sink_notice, sink_modal};
}
bool sink_says(Utf8View text) noexcept { return utf8_equal({g_sink.text, g_sink.text_bytes}, text); }

// ---- mc.read@2 ------------------------------------------------------------------------

void test_read() noexcept {
    install_fake_read();
    auto plugin = start_inproc(0x5101, MC_PERM_READ_GUILDS, MC_PERM_ALL);
    const auto* read = table<mc_read_v2>("mc.read", 2);
    check(plugin.enabled && read && read->size == sizeof(mc_read_v2) && MC_HAS_FIELD(read, mc_read_v2, revisions),
          "mc.read@2 is granted by any read permission");
    if (!read) { stop_inproc(plugin); return; }
    void* b = g_inproc.binding;
    const u64 g = g_inproc.generation;
    mc_guild_info guild{};
    guild.size = sizeof(guild);
    mc_member_info member{};
    member.size = sizeof(member);
    mc_user_info user{};
    user.size = sizeof(user);
    check(read->guild(b, g, kGuild, &guild) == MC_OK && guild.member_count == 12 && guild.flags == (MC_GUILD_OWNER | MC_GUILD_COUNTS) &&
              utf8_equal({guild.name, guild.name_bytes}, utf8_literal("Test guild")),
          "guild reads copy the held record");
    check(read->guild(b, g, 1234, &guild) == MC_E_NOT_FOUND, "a guild MaiCord doesn't hold is not found, never fetched");
    check(read->member(b, g, kGuild, kAlice, &member) == MC_E_DENIED && read->user(b, g, kSelf, &user) == MC_E_DENIED &&
              read->message(b, g, kChannel, 600, nullptr) == MC_E_DENIED,
          "each data class needs its own read permission");
    u64 ids[4]{};
    u32 count = 0, next = 0;
    check(read->list(b, g, MC_LIST_MESSAGES, kChannel, 0, ids, 4, &count, &next) == MC_E_DENIED &&
              read->list(b, g, 99, 0, 0, ids, 4, &count, &next) == MC_E_INVALID,
          "lists are gated per list kind");
    mc_read_revisions revisions{};
    revisions.size = sizeof(revisions);
    check(read->revisions(b, g, &revisions) == MC_OK && revisions.guilds == 11 && !revisions.members && !revisions.presence &&
              !revisions.voice && !revisions.messages,
          "revision counters are reported only for permitted classes");
    const auto* events = table<mc_events_v1>("mc.events", 1);
    check(events && events->subscribe(b, g, MC_EVENT_MASK(MC_EVENT_MESSAGE_CREATE), [](void*, uint32_t, const mc_event*, uint32_t, uint32_t) {}) ==
                        MC_E_DENIED &&
              events->subscribe(b, g, MC_EVENT_MASK(MC_EVENT_GUILD_UPDATE), [](void*, uint32_t, const mc_event*, uint32_t, uint32_t) {}) == MC_OK &&
              events->subscribe(b, g, 1ull << 40, nullptr) == MC_E_INVALID,
          "event kinds are permitted by the read permission of their data class");
    stop_inproc(plugin);

    plugin = start_inproc(0x5102, MC_PERM_READ_CONTEXT | MC_PERM_READ_MEMBERS | MC_PERM_READ_PRESENCE | MC_PERM_READ_VOICE |
                                      MC_PERM_READ_MESSAGES, MC_PERM_ALL);
    read = table<mc_read_v2>("mc.read", 2);
    b = g_inproc.binding;
    const u64 g2 = g_inproc.generation;
    mc_presence_info presence{};
    presence.size = sizeof(presence);
    mc_voice_info voice{};
    voice.size = sizeof(voice);
    check(read && read->member(b, g2, kGuild, kAlice, &member) == MC_OK && member.role_ids[0] == 4 &&
              utf8_equal({member.nick, member.nick_bytes}, utf8_literal("Ally")) &&
              read->user(b, g2, kAlice, &user) == MC_OK && utf8_equal({user.username, user.username_bytes}, utf8_literal("alice")) &&
              read->presence(b, g2, kAlice, 0, &presence) == MC_OK && presence.mobile == MC_STATUS_IDLE &&
              read->voice(b, g2, 0, kAlice, &voice) == MC_OK && voice.channel_id == 500 && voice.flags == MC_VOICE_SELF_MUTE,
          "members, users, presence (client_status) and voice states copy out");
    char small[8];
    mc_message_info message{};
    message.size = sizeof(message);
    message.content = small;
    message.content_capacity = sizeof(small);
    const mc_status truncated = read->message(b, g2, kChannel, 600, &message);
    check(truncated == MC_E_TRUNCATED && message.content_bytes == 25 && message.author_id == kAlice && message.revision == 3 &&
              message.content == small && bytes_equal(small, "hello th", 8),
          "message text fills the caller's buffer and reports the full size");
    char exact[64];
    message.content = exact;
    message.content_capacity = sizeof(exact);
    check(read->message(b, g2, kChannel, 600, &message) == MC_OK && bytes_equal(exact, "hello there, friend ", 20),
          "a large enough buffer receives the whole UTF-8 text");
    message.content = nullptr;
    message.content_capacity = 0;
    check(read->message(b, g2, kChannel, 601, &message) == MC_OK && (message.flags & MC_MESSAGE_OWN),
          "metadata alone needs no buffer");
    u64 one[1]{};
    check(read->list(b, g2, MC_LIST_MESSAGES, kChannel, 0, one, 1, &count, &next) == MC_OK && count == 1 && one[0] == 601 &&
              next == 1 && read->list(b, g2, MC_LIST_MESSAGES, kChannel, next, one, 1, &count, &next) == MC_OK &&
              one[0] == 600 && next == 0,
          "message lists iterate newest first with a cursor");
    struct OldUser {
        u32 size, flags;
        u64 user_id;
        u64 sentinel;
    } old{};
    old.size = offsetof(mc_user_info, name_bytes);
    old.sentinel = 0x5555;
    check(read->user(b, g2, kSelf, reinterpret_cast<mc_user_info*>(&old)) == MC_OK && old.user_id == kSelf &&
              old.sentinel == 0x5555 && old.flags == MC_USER_SELF,
          "an older output struct receives only its declared prefix");
    stop_inproc(plugin);

    plugin = start_inproc(0x5103, MC_PERM_READ_CONTEXT, MC_PERM_ALL);
    read = table<mc_read_v2>("mc.read", 2);
    check(read && read->user(g_inproc.binding, g_inproc.generation, kSelf, &user) == MC_OK &&
              read->user(g_inproc.binding, g_inproc.generation, kAlice, &user) == MC_E_DENIED,
          "read_context alone reads only your own user");
    stop_inproc(plugin);
    g_runtime.read = {};
}

// ---- mc.events@1 ------------------------------------------------------------------------

struct EventLog {
    u32 calls, events, overflow_calls, slow_spin_us;
    u64 overflow_lost, last_sequence;
    u32 max_batch;
    bool texts_ok, ordered;
};
EventLog g_events;
void MC_CALL record_events(void*, uint32_t, const mc_event* events, uint32_t count, uint32_t stride) {
    ++g_events.calls;
    g_events.max_batch = maximum(g_events.max_batch, count);
    for (u32 i = 0; i < count; ++i) {
        const auto* event = reinterpret_cast<const mc_event*>(reinterpret_cast<const u8*>(events) + i * stride);
        if (event->kind == MC_EVENT_OVERFLOW) {
            ++g_events.overflow_calls;
            g_events.overflow_lost += event->value;
            continue;
        }
        ++g_events.events;
        if (g_events.last_sequence && event->sequence <= g_events.last_sequence) g_events.ordered = false;
        g_events.last_sequence = event->sequence;
        if (event->kind == MC_EVENT_MESSAGE_DELETE &&
            (!(event->flags & MC_EVENT_CACHED) || !(event->flags & MC_EVENT_TEXT) ||
             !utf8_equal({event->text.data, event->text.bytes}, utf8_literal("gone")) ||
             !utf8_equal({event->name.data, event->name.bytes}, utf8_literal("Alice A"))))
            g_events.texts_ok = false;
    }
    if (g_events.slow_spin_us) {
        LARGE_INTEGER frequency{}, start{}, now{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        do QueryPerformanceCounter(&now);
        while ((now.QuadPart - start.QuadPart) * 1'000'000 / frequency.QuadPart < g_events.slow_spin_us);
    }
}
const mc_events_v1* g_events_table;
void subscribe_messages() noexcept {
    g_events_table = table<mc_events_v1>("mc.events", 1);
    if (g_events_table)
        g_events_table->subscribe(g_inproc.binding, g_inproc.generation,
                                  MC_EVENT_MASK(MC_EVENT_MESSAGE_CREATE) | MC_EVENT_MASK(MC_EVENT_MESSAGE_DELETE), record_events);
}
void push_events(u32 count, u32 kind = MC_EVENT_MESSAGE_CREATE) noexcept {
    for (u32 i = 0; i < count; ++i) {
        NativePluginSdkEvent event{kind, 0, kGuild, kChannel, 1000 + i, kAlice, 0, utf8_literal("hi"), utf8_literal("Alice A")};
        native_plugin_sdk_event_push(&g_runtime, event);
    }
}

void test_events() noexcept {
    g_events = {};
    g_events.texts_ok = g_events.ordered = true;
    auto plugin = start_inproc(0x5201, MC_PERM_READ_MESSAGES, MC_PERM_ALL, subscribe_messages);
    check(plugin.enabled && g_events_table, "an event subscription can be made during start");
    NativePluginSdkEvent typing{MC_EVENT_TYPING, 0, 0, kChannel, 0, kAlice, 0, {}, {}};
    check(!native_plugin_sdk_event_push(&g_runtime, typing) &&
              native_plugin_sdk_event_interest(g_runtime) ==
                  (MC_EVENT_MASK(MC_EVENT_MESSAGE_CREATE) | MC_EVENT_MASK(MC_EVENT_MESSAGE_DELETE)),
          "events nobody subscribed to are not queued");
    push_events(40);
    NativePluginSdkEvent deleted{MC_EVENT_MESSAGE_DELETE, MC_EVENT_CACHED, kGuild, kChannel, 77, kAlice, 0,
                                 utf8_literal("gone"), utf8_literal("Alice A")};
    native_plugin_sdk_event_push(&g_runtime, deleted);
    auto polled = native_plugin_sdk_poll(&g_runtime);
    check(g_events.events == 41 && g_events.calls == 2 && g_events.max_batch == kPluginSdkEventBatch && !polled.events_pending &&
              g_events.texts_ok && g_events.ordered,
          "queued events arrive in order in batches of 32 with copied texts");
    g_events.calls = g_events.events = 0;
    push_events(200);
    polled = native_plugin_sdk_poll(&g_runtime);
    check(g_events.events == kPluginSdkEventBatch * kPluginSdkEventCallsPerPass && polled.events_pending,
          "one pass delivers at most four batches per plugin and reports the rest");
    native_plugin_sdk_poll(&g_runtime);
    check(g_events.events == 200 && !native_plugin_sdk_poll(&g_runtime).events_pending, "the next pass delivers the remainder");
    g_events.events = 0;
    push_events(kPluginSdkEventQueue + 88);
    while (native_plugin_sdk_poll(&g_runtime).events_pending) {}
    check(g_events.overflow_calls == 1 && g_events.overflow_lost == 88 && g_events.events == kPluginSdkEventQueue,
          "a plugin that falls behind gets one overflow event with the number lost");
    // Budget enforcement on event delivery: three slow batches turn the plugin off.
    const u64 budget = g_ledger.budget_us;
    g_ledger.budget_us = 100;
    g_events.slow_spin_us = 400;
    g_events.calls = 0;
    for (u32 i = 0; i < 4; ++i) {
        push_events(1);
        native_plugin_sdk_poll(&g_runtime);
    }
    check(g_events.calls == 3 && plugin_call_ledger_disable_requested(&g_ledger, 0x5201) &&
              !(native_plugin_sdk_event_interest(g_runtime) & MC_EVENT_MASK(MC_EVENT_MESSAGE_CREATE)),
          "an event handler over its budget three times gets no further batches");
    g_events.slow_spin_us = 0;
    g_ledger.budget_us = budget;
    plugin_call_ledger_take_disable(&g_ledger);
    stop_inproc(plugin);
    plugin_call_ledger_forget(&g_ledger, 0x5201);
}

// ---- mc.msg.render@1 -------------------------------------------------------------------

struct RenderLog {
    u32 calls, actions;
    const mc_render_v1* table;
    mc_slot_node nodes[4];
    char long_prefix[40];
};
RenderLog g_render;
mc_status MC_CALL decorate(void*, uint32_t, const mc_render_message* message, mc_render_decoration* out) {
    ++g_render.calls;
    switch (message->message_id) {
    case 1:
        out->name_suffix = {"@alice", 6};
        out->name_tone = MC_TONE_ROLE;
        break;
    case 2:
        if (message->relationship == MC_RELATIONSHIP_BLOCKED) out->visibility = MC_RENDER_COLLAPSED;
        out->media = MC_MEDIA_BLUR;
        break;
    case 3:
        out->replace_content = 1;
        out->content = {"*translated*\nline two", 21};
        break;
    case 4:
        g_render.nodes[0] = {sizeof(mc_slot_node), MC_NODE_BADGE, {"bot", 3}, {}, {}, MC_TONE_ACCENT, 0, 0, {}};
        g_render.nodes[1] = {sizeof(mc_slot_node), MC_NODE_LABEL, {"Seen by 3", 9}, {}, {}, 0, 0, 0, {}};
        g_render.nodes[2] = {sizeof(mc_slot_node), MC_NODE_BUTTON, {"Details", 7}, {}, {}, 0, 0, 5, {}};
        g_render.nodes[3] = {sizeof(mc_slot_node), MC_NODE_LINK, {"Docs", 4}, {}, {}, 0, 0, 0, {"https://example.com", 19}};
        out->badges = g_render.nodes;
        out->badge_count = 1;
        out->badge_stride = sizeof(mc_slot_node);
        out->accessories = g_render.nodes + 1;
        out->accessory_count = 3;
        out->accessory_stride = sizeof(mc_slot_node);
        break;
    case 5:
        for (u32 i = 0; i < 40; ++i) g_render.long_prefix[i] = 'x';
        out->name_prefix = {g_render.long_prefix, 40}; // over the 32-byte limit: the whole verdict is dropped
        out->visibility = MC_RENDER_HIDDEN;
        break;
    default: break;
    }
    return MC_OK;
}
mc_status MC_CALL render_accessory(void*, uint32_t, const mc_slot_event* event, mc_popover*) {
    ++g_render.actions;
    return event->surface == MC_SLOT_MESSAGE_ACCESSORY && event->message_id == 4 && event->action_id == 5 ? MC_OK : MC_E_INVALID;
}
void install_decorator() noexcept {
    g_render.table = table<mc_render_v1>("mc.msg.render", 1);
    if (!g_render.table) return;
    g_render.table->set_decorator(g_inproc.binding, g_inproc.generation, decorate);
    g_render.table->set_action_handler(g_inproc.binding, g_inproc.generation, render_accessory);
}
NativePluginSdkRenderMessage render_message(u64 id, u32 revision, u32 relationship = 0) noexcept {
    return {kChannel, id, kAlice, kGuild, 1, 0, 0, revision, relationship, 1, 0, utf8_literal("original text"),
            utf8_literal("Alice A")};
}

void test_render() noexcept {
    g_render = {};
    auto plugin = start_inproc(0x5301, MC_PERM_MESSAGE_RENDER, MC_PERM_ALL, install_decorator);
    check(plugin.enabled && g_render.table && native_plugin_sdk_has_decorators(g_runtime), "a decorator registers");
    NativePluginSdkDecoration decoration{};
    check(!native_plugin_sdk_decorate(&g_runtime, render_message(1, 1), false, &decoration) && !g_render.calls &&
              !native_plugin_sdk_decoration_current(g_runtime, 1, 1),
          "a cache-only query never runs plugin code");
    check(native_plugin_sdk_decorate(&g_runtime, render_message(1, 1), true, &decoration) && g_render.calls == 1 &&
              utf8_equal(decoration.name_suffix, utf8_literal("@alice")) && decoration.name_tone == MC_TONE_ROLE &&
              native_plugin_sdk_decoration_current(g_runtime, 1, 1),
          "evaluation runs the decorator and caches the verdict");
    check(native_plugin_sdk_decorate(&g_runtime, render_message(1, 1), false, &decoration) &&
              native_plugin_sdk_decorate(&g_runtime, render_message(1, 1), true, &decoration) && g_render.calls == 1,
          "the same revision is answered from the cache");
    check(native_plugin_sdk_decorate(&g_runtime, render_message(1, 2), true, &decoration) && g_render.calls == 2,
          "a new message revision asks again");
    check(native_plugin_sdk_decorate(&g_runtime, render_message(2, 1, MC_RELATIONSHIP_BLOCKED), true, &decoration) &&
              decoration.visibility == MC_RENDER_COLLAPSED && decoration.media == MC_MEDIA_BLUR,
          "collapse and media verdicts are kept");
    check(native_plugin_sdk_decorate(&g_runtime, render_message(3, 1), true, &decoration) && decoration.replace_content &&
              utf8_equal(decoration.content, utf8_literal("*translated*\nline two")),
          "display-only content replacement keeps line breaks");
    check(native_plugin_sdk_decorate(&g_runtime, render_message(4, 1), true, &decoration) && decoration.badge_count == 1 &&
              utf8_equal(decoration.badges[0].text, utf8_literal("bot")) && decoration.accessory_count == 3 &&
              decoration.accessories[1].action_id == 5 && decoration.accessories[1].owner_id == 0x5301 &&
              utf8_equal(decoration.accessories[2].value, utf8_literal("https://example.com")),
          "badges and accessory rows are copied with their owner");
    const u32 before = g_render.calls;
    check(!native_plugin_sdk_decorate(&g_runtime, render_message(5, 1), true, &decoration) && g_render.calls == before + 1 &&
              native_plugin_sdk_decoration_current(g_runtime, 5, 1),
          "an invalid verdict is ignored but still cached (no retry storm)");
    check(native_plugin_sdk_render_action(&g_runtime, 0x5301, kChannel, 4, 5) && g_render.actions == 1 &&
              !native_plugin_sdk_render_action(&g_runtime, 0x5301, kChannel, 4, 6) &&
              !native_plugin_sdk_render_action(&g_runtime, 0x5301, kChannel, 1, 5),
          "accessory buttons dispatch only what the decorator published");
    const u64 revision = g_runtime.render_revision;
    g_render.table->invalidate(g_inproc.binding, g_inproc.generation, kChannel, 1);
    check(g_runtime.render_revision != revision && !native_plugin_sdk_decoration_current(g_runtime, 1, 2) &&
              native_plugin_sdk_decoration_current(g_runtime, 3, 1),
          "invalidating one message leaves the others cached");
    g_render.table->invalidate(g_inproc.binding, g_inproc.generation, 0, 0);
    check(!native_plugin_sdk_decoration_current(g_runtime, 3, 1), "invalidating everything asks again for every message");
    stop_inproc(plugin);
    check(!native_plugin_sdk_has_decorators(g_runtime) && !native_plugin_sdk_decorate(&g_runtime, render_message(1, 2), true, &decoration),
          "stopping the plugin drops its decorations");
    auto denied = start_inproc(0x5302, MC_PERM_STORE, MC_PERM_ALL);
    check(lookup("mc.msg.render", 1) == MC_E_DENIED, "decorators need the message_render permission");
    stop_inproc(denied);
}

// ---- mc.actions@1 -----------------------------------------------------------------------

const mc_actions_v1* g_actions;
const mc_menus_v2* g_menus;
mc_status g_gesture_status;
u32 g_modal_results;
char g_modal_text[64];
u32 g_modal_bytes;
mc_status MC_CALL gesture_menu(void*, uint32_t, const mc_menu_context* context) {
    if (context->item_id == 1) g_gesture_status = g_actions->copy_text(g_inproc.binding, g_inproc.generation, context->url);
    if (context->item_id == 2) {
        mc_slot_node nodes[3]{};
        nodes[0] = {sizeof(mc_slot_node), MC_NODE_LABEL, {"Reply to Alice", 14}, {}, {}, 0, 0, 0, {}};
        nodes[1] = {sizeof(mc_slot_node), MC_NODE_INPUT, {"Reply", 5}, {}, {}, 0, 0, 0, {"draft", 5}};
        nodes[2] = {sizeof(mc_slot_node), MC_NODE_BUTTON, {"Send", 4}, {}, {}, MC_TONE_CRITICAL, 0, 1, {}};
        mc_modal modal{sizeof(mc_modal), 3, 42, {"Quick reply", 11}, nodes, sizeof(mc_slot_node), 100};
        g_gesture_status = g_actions->open_modal(g_inproc.binding, g_inproc.generation, &modal);
    }
    return MC_OK;
}
mc_status MC_CALL modal_done(void*, uint32_t, const mc_modal_result* result) {
    ++g_modal_results;
    g_modal_bytes = result->accepted ? minimum(result->text.bytes, static_cast<u32>(sizeof(g_modal_text))) : 0;
    copy_bytes(g_modal_text, result->text.data, g_modal_bytes);
    // A modal result is a gesture too: copying works here.
    return result->accepted ? g_actions->copy_text(g_inproc.binding, g_inproc.generation, result->text) : MC_OK;
}
void install_actions() noexcept {
    g_actions = table<mc_actions_v1>("mc.actions", 1);
    g_menus = table<mc_menus_v2>("mc.ui.menus", 2);
    if (g_actions) g_actions->set_modal_handler(g_inproc.binding, g_inproc.generation, modal_done);
    if (g_menus) {
        g_menus->register_item(g_inproc.binding, g_inproc.generation, 1, MC_MENU_LINK, {"Copy link", 9}, 0);
        g_menus->register_item(g_inproc.binding, g_inproc.generation, 2, MC_MENU_MESSAGE, {"Quick reply", 11}, 0);
        g_menus->set_handler(g_inproc.binding, g_inproc.generation, gesture_menu);
    }
}

void test_actions() noexcept {
    install_fake_sink();
    g_modal_results = 0;
    g_clock = 50'000;
    auto plugin = start_inproc(0x5401, MC_PERM_ACT_MESSAGES | MC_PERM_ACT_PRESENCE | MC_PERM_CLIPBOARD | MC_PERM_NOTIFY |
                                           MC_PERM_MENUS, MC_PERM_ALL, install_actions);
    check(plugin.enabled && g_actions && g_actions->size == sizeof(mc_actions_v1), "mc.actions@1 is granted by an action permission");
    if (!g_actions) { stop_inproc(plugin); return; }
    void* b = g_inproc.binding;
    const u64 g = g_inproc.generation;
    mc_send_request send{sizeof(mc_send_request), MC_SEND_SILENT, kChannel, 600, {"hello", 5}};
    check(g_actions->send_message(b, g, &send) == MC_OK && g_sink.sends == 1 && g_sink.last_owner == 0x5401 &&
              g_sink.last_reply == 600 && g_sink.last_flags == MC_SEND_SILENT && sink_says(utf8_literal("hello")),
          "send_message reaches the host with its flags and reply");
    mc_send_request bad = send;
    bad.text = {"", 0};
    mc_send_request no_channel = send;
    no_channel.channel_id = 0;
    mc_send_request flags = send;
    flags.flags = 8;
    check(g_actions->send_message(b, g, &bad) == MC_E_INVALID && g_actions->send_message(b, g, &no_channel) == MC_E_INVALID &&
              g_actions->send_message(b, g, &flags) == MC_E_INVALID && g_sink.sends == 1,
          "empty text, a missing channel and unknown flags are refused before the host");
    u32 accepted = 0;
    for (u32 i = 0; i < 5; ++i) accepted += g_actions->send_message(b, g, &send) == MC_OK;
    check(accepted == 4 && g_actions->send_message(b, g, &send) == MC_E_RATE_LIMITED && g_sink.sends == 5,
          "messages are limited to 5 per 5 seconds");
    g_clock += 1100;
    check(g_actions->send_message(b, g, &send) == MC_OK && g_actions->send_message(b, g, &send) == MC_E_RATE_LIMITED,
          "the message bucket refills over time");
    g_clock += 5000;
    check(g_actions->react(b, g, kChannel, 600, {"\xF0\x9F\x91\x8D", 4}) == MC_OK && g_sink.last_emoji == 0 &&
              g_actions->react(b, g, kChannel, 600, {"party:123456", 12}) == MC_OK && g_sink.last_emoji == 123456 &&
              sink_says(utf8_literal("party")) && g_actions->react(b, g, kChannel, 600, {"bad:", 4}) == MC_E_INVALID &&
              g_actions->react(b, g, kChannel, 600, {"<:x:1>", 6}) == MC_E_INVALID,
          "reactions take a Unicode emoji or name:id");
    check(g_actions->edit_message(b, g, kChannel, 601, {"fixed", 5}) == MC_OK && g_sink.edits == 1 &&
              g_actions->mark_read(b, g, kChannel) == MC_OK && g_sink.reads == 1,
          "edit and mark read reach the host");
    mc_activity activity{sizeof(mc_activity), MC_ACTIVITY_LISTENING, 0, 0, 0, {"Radio", 5}, {"Song", 4}, {"Artist", 6}};
    mc_activity streaming = activity;
    streaming.type = MC_ACTIVITY_STREAMING;
    check(g_actions->set_activity(b, g, &activity) == MC_OK && g_sink.activities == 1 && g_sink.last_type == MC_ACTIVITY_LISTENING &&
              g_actions->set_activity(b, g, &streaming) == MC_E_INVALID && g_actions->set_activity(b, g, nullptr) == MC_OK &&
              g_sink.cleared == 1,
          "a plugin activity is validated and can be cleared");
    check(g_actions->set_custom_status(b, g, {"busy", 4}, {}, 0) == MC_OK && sink_says(utf8_literal("busy")) &&
              g_actions->set_custom_status(b, g, {"away", 4}, {}, 0) == MC_E_RATE_LIMITED,
          "presence changes share a 2 per 10 seconds bucket");
    check(g_actions->notice(b, g, {"Saved", 5}, MC_TONE_POSITIVE) == MC_OK && g_sink.last_tone == MC_TONE_POSITIVE &&
              g_actions->notice(b, g, {"Saved", 5}, 9) == MC_E_INVALID,
          "notices carry a tone");
    check(g_actions->copy_text(b, g, {"secret", 6}) == MC_E_INACTIVE && g_actions->open_url(b, g, {"https://x.y", 11}) == MC_E_DENIED &&
              g_actions->open_channel(b, g, kChannel, 0) == MC_E_DENIED && g_sink.copies == 0,
          "copy outside a gesture is refused and ungranted actions are denied");
    NativePluginSdkMenuContext link{};
    link.target = MC_MENU_LINK;
    link.channel_id = kChannel;
    link.message_id = 600;
    link.url = utf8_literal("https://example.com/a");
    check(native_plugin_sdk_menu_activate(&g_runtime, 0x5401, 1, link) && g_gesture_status == MC_OK && g_sink.copies == 1 &&
              sink_says(utf8_literal("https://example.com/a")),
          "a menu handler runs inside a gesture and may copy text");
    NativePluginSdkMenuContext message{};
    message.target = MC_MENU_MESSAGE;
    message.channel_id = kChannel;
    message.message_id = 600;
    check(native_plugin_sdk_menu_activate(&g_runtime, 0x5401, 2, message) && g_gesture_status == MC_OK && g_sink.modals == 1 &&
              g_sink.modal.modal_id == 42 && g_sink.modal.node_count == 3 && g_sink.modal.input_max_bytes == 100 &&
              g_sink.modal.nodes[1].kind == MC_NODE_INPUT && utf8_equal(g_sink.modal.nodes[1].value, utf8_literal("draft")) &&
              g_sink.modal.nodes[2].tone == MC_TONE_CRITICAL && utf8_equal(g_sink.modal.title, utf8_literal("Quick reply")),
          "a modal is built from declarative nodes");
    check(native_plugin_sdk_menu_activate(&g_runtime, 0x5401, 2, message) && g_gesture_status == MC_E_BUSY && g_sink.modals == 1,
          "one modal per plugin at a time");
    check(!native_plugin_sdk_modal_result(&g_runtime, 0x5401, 41, true, utf8_literal("x")) &&
              native_plugin_sdk_modal_result(&g_runtime, 0x5401, 42, true, utf8_literal("thanks!")) && g_modal_results == 1 &&
              utf8_equal({g_modal_text, g_modal_bytes}, utf8_literal("thanks!")) && sink_says(utf8_literal("thanks!")) &&
              !native_plugin_sdk_modal_result(&g_runtime, 0x5401, 42, false, {}),
          "exactly one result reaches the modal handler, as a gesture");
    const u32 cleared = g_sink.cleared;
    stop_inproc(plugin);
    check(g_sink.cleared == cleared + 1, "a stopping plugin's activity is cleared");
    auto limited = start_inproc(0x5402, MC_PERM_STORE, MC_PERM_ALL);
    check(lookup("mc.actions", 1) == MC_E_DENIED, "mc.actions needs an action permission");
    stop_inproc(limited);
    g_runtime.actions = {};
}

// ---- Keybinds ----------------------------------------------------------------------------

u32 g_keybind_fires;
u64 g_keybind_control;
void MC_CALL keybind_fired(void*, uint32_t, uint64_t page, uint64_t control) {
    ++g_keybind_fires;
    g_keybind_control = page == 1 ? control : 0;
}
bool keybind_check(void*, NativePluginSdkChord chord, Utf8View* conflict) noexcept {
    if (chord.key == 'K' && chord.modifiers == 1) {
        *conflict = utf8_literal("Search");
        return false;
    }
    return true;
}
const mc_settings_v3* g_settings3;
void publish_keybinds() noexcept {
    const auto* pages = table<mc_pages_v1>("mc.ui.pages", 1);
    g_settings3 = table<mc_settings_v3>("mc.settings", 3);
    if (!pages || !g_settings3) return;
    pages->register_page(g_inproc.binding, g_inproc.generation, 1, {"Keys", 4});
    mc_setting controls[4]{};
    const char* chords[]{"ctrl+k", "Ctrl+Shift+Q", "q", "F5"};
    for (u32 i = 0; i < 4; ++i) {
        controls[i].size = sizeof(mc_setting);
        controls[i].kind = MC_SETTING_KEYBIND;
        controls[i].id = 10 + i;
        controls[i].label = {"Shortcut", 8};
        controls[i].enabled = 1;
        controls[i].text = {chords[i], c_string_length(chords[i])};
    }
    mc_settings_page page{sizeof(mc_settings_page), 4, {"Keys", 4}, {}, controls, sizeof(mc_setting), 0};
    g_settings3->set_keybind_handler(g_inproc.binding, g_inproc.generation, keybind_fired);
    g_settings3->set_page(g_inproc.binding, g_inproc.generation, 1, &page);
}

void test_keybinds() noexcept {
    g_runtime.keybind_check = keybind_check;
    g_keybind_fires = 0;
    NativePluginSdkChord chord{};
    check(native_plugin_sdk_keybind_chord(utf8_literal("shift+ctrl+f12"), &chord) && chord.key == 0x7B && chord.modifiers == 5 &&
              native_plugin_sdk_keybind_chord(utf8_literal("Alt+PageDown"), &chord) && chord.key == 0x22 && chord.modifiers == 2 &&
              !native_plugin_sdk_keybind_chord(utf8_literal("Win+A"), &chord),
          "normal-form chords map to virtual keys; Win chords are refused");
    auto plugin = start_inproc(0x5501, MC_PERM_SETTINGS | MC_PERM_KEYBINDS, MC_PERM_ALL, publish_keybinds);
    const auto* descriptor = ui_page_registry_find(g_host.pages, {0x5501, 1});
    NativePluginSettingsSnapshot snapshot{};
    UiSettingsControl controls[8]{};
    UiSettingsPageContent content{};
    const bool projected = descriptor &&
        native_plugin_settings_snapshot(descriptor->context, &snapshot) == NativePluginSettingsError::none &&
        native_plugin_settings_project(snapshot, {controls, 8}, &content) == NativePluginSettingsError::none &&
        content.controls.count == 4;
    check(plugin.enabled && projected && test_contains(controls[0].description, utf8_literal("Conflicts with Search")) &&
              !test_contains(controls[1].description, utf8_literal("Conflicts")) &&
              test_contains(controls[2].description, utf8_literal("Conflicts with a shortcut that needs Ctrl or Alt")) &&
              !test_contains(controls[3].description, utf8_literal("Conflicts")),
          "keybind rows say what a refused chord conflicts with");
    NativePluginSdkKeybindView views[8]{};
    check(native_plugin_sdk_keybinds(g_runtime, {views, 8}) == 2, "only usable chords are registered");
    check(native_plugin_sdk_keybind_press(&g_runtime, {'Q', 5}, false) && g_keybind_fires == 1 && g_keybind_control == 11,
          "a registered chord fires the plugin's keybind handler");
    check(native_plugin_sdk_keybind_press(&g_runtime, {'Q', 5}, true) && g_keybind_fires == 2 &&
              !native_plugin_sdk_keybind_press(&g_runtime, {0x74, 0}, true) &&
              native_plugin_sdk_keybind_press(&g_runtime, {0x74, 0}, false) && g_keybind_fires == 3,
          "while typing only Ctrl/Alt chords fire");
    check(!native_plugin_sdk_keybind_press(&g_runtime, {'K', 1}, false) && g_keybind_fires == 3,
          "a chord that conflicts with MaiCord never reaches the plugin");
    stop_inproc(plugin);
    check(!native_plugin_sdk_keybinds(g_runtime, {views, 8}) && !native_plugin_sdk_keybind_press(&g_runtime, {'Q', 5}, false),
          "stopping the plugin releases its chords");
    auto denied = start_inproc(0x5502, MC_PERM_SETTINGS, MC_PERM_ALL);
    const auto* settings = table<mc_settings_v3>("mc.settings", 3);
    check(settings && settings->set_keybind_handler(g_inproc.binding, g_inproc.generation, keybind_fired) == MC_E_DENIED,
          "keybinds need the keybinds permission");
    stop_inproc(denied);
    g_runtime.keybind_check = nullptr;
}

// ---- mc.ui.slots@2 ----------------------------------------------------------------------

void test_slots_v2() noexcept {
    auto plugin = start_inproc(0x5601, MC_PERM_UI_SLOTS, MC_PERM_ALL);
    const auto* slots = table<mc_slots_v2>("mc.ui.slots", 2);
    check(plugin.enabled && slots && MC_HAS_FIELD(slots, mc_slots_v2, set_handler), "mc.ui.slots@2 is available");
    if (!slots) { stop_inproc(plugin); return; }
    void* b = g_inproc.binding;
    const u64 g = g_inproc.generation;
    mc_slot_node badge{sizeof(mc_slot_node), MC_NODE_BADGE, {"3", 1}, {}, {"Typing", 6}, MC_TONE_ACCENT, 0, 0, {}};
    mc_slot_node label{sizeof(mc_slot_node), MC_NODE_LABEL, {"x", 1}, {}, {}, 0, 0, 0, {}};
    mc_slot_node button{sizeof(mc_slot_node), MC_NODE_BUTTON, {}, {"lucide/star", 11}, {"Star", 4}, 0, 0, 9, {}};
    mc_slot_node link{sizeof(mc_slot_node), MC_NODE_LINK, {"Site", 4}, {}, {}, 0, 0, 0, {"https://example.com", 19}};
    mc_slot_node insecure = link;
    insecure.value = {"http://example.com", 18};
    const auto set = [&](u32 surface, u64 channel, u64 user, u64 guild, const mc_slot_node& node) noexcept {
        mc_slot_target target{sizeof(mc_slot_target), surface, channel, 0, user, guild};
        mc_slot_content content{sizeof(mc_slot_content), 1, &node, sizeof(mc_slot_node), 0};
        return slots->set(b, g, &target, &content);
    };
    check(set(MC_SLOT_CHANNEL_ROW, kChannel, 0, 0, badge) == MC_OK && set(MC_SLOT_CHANNEL_ROW, 0, 0, 0, badge) == MC_E_INVALID &&
              set(MC_SLOT_GUILD_RAIL, 0, 0, kGuild, badge) == MC_OK && set(MC_SLOT_GUILD_RAIL, kChannel, 0, 0, badge) == MC_E_INVALID,
          "channel rows need a channel and rail badges a guild");
    check(set(MC_SLOT_ACCOUNT_PANEL, 0, 0, 0, button) == MC_OK && set(MC_SLOT_ACCOUNT_PANEL, 0, 0, 0, label) == MC_E_INVALID &&
              set(MC_SLOT_MESSAGE_HOVER, 0, 0, 0, button) == MC_OK,
          "button surfaces take buttons and icons only");
    check(set(MC_SLOT_PROFILE_SECTION, 0, kAlice, 0, link) == MC_OK && set(MC_SLOT_PROFILE_SECTION, 0, kAlice, 0, insecure) == MC_E_INVALID &&
              set(MC_SLOT_MEMBER_BADGE, 0, kAlice, 0, link) == MC_E_INVALID,
          "links need https and are profile rows only");
    NativePluginSdkSlotNode nodes[4]{};
    check(native_plugin_sdk_slots(g_runtime, {MC_SLOT_CHANNEL_ROW, kChannel, 0, 0, 0}, {nodes, 4}) == 1 &&
              utf8_equal(nodes[0].tooltip, utf8_literal("Typing")) &&
              native_plugin_sdk_slots(g_runtime, {MC_SLOT_GUILD_RAIL, 0, 0, 0, kGuild}, {nodes, 4}) == 1 &&
              native_plugin_sdk_slots(g_runtime, {MC_SLOT_PROFILE_SECTION, 0, 0, kAlice, 0}, {nodes, 4}) == 1 &&
              utf8_equal(nodes[0].value, utf8_literal("https://example.com")),
          "new surfaces project their nodes");
    u32 handled = 0;
    static mc_slot_event seen;
    slots->set_handler(b, g, [](void*, uint32_t, const mc_slot_event* event, mc_popover*) -> mc_status {
        seen = *event;
        return MC_OK;
    });
    handled = native_plugin_sdk_slot_action(&g_runtime, 0x5601, {MC_SLOT_MESSAGE_HOVER, kChannel, 600, 0, 0}, 9) ? 1u : 0u;
    check(handled && seen.surface == MC_SLOT_MESSAGE_HOVER && seen.channel_id == kChannel && seen.message_id == 600 &&
              seen.action_id == 9,
          "hover toolbar clicks carry the hovered message");
    for (u32 i = 0; i < kPluginSdkSlotEntries + 10; ++i) set(MC_SLOT_CHANNEL_ROW, 1000 + i, 0, 0, badge);
    check(!native_plugin_sdk_slots(g_runtime, {MC_SLOT_CHANNEL_ROW, 1000, 0, 0, 0}, {nodes, 4}) &&
              native_plugin_sdk_slots(g_runtime, {MC_SLOT_CHANNEL_ROW, 1000 + kPluginSdkSlotEntries + 9, 0, 0, 0}, {nodes, 4}) == 1,
          "256 row targets fit and the oldest are evicted beyond that");
    set(MC_SLOT_GUILD_RAIL, 0, 0, kGuild, badge);
    const u64 revision = g_runtime.slot_revision;
    check(slots->clear_surface(b, g, MC_SLOT_CHANNEL_ROW) == MC_OK && g_runtime.slot_revision != revision &&
              !native_plugin_sdk_slots(g_runtime, {MC_SLOT_CHANNEL_ROW, 1000 + kPluginSdkSlotEntries + 9, 0, 0, 0}, {nodes, 4}) &&
              native_plugin_sdk_slots(g_runtime, {MC_SLOT_GUILD_RAIL, 0, 0, 0, kGuild}, {nodes, 4}) == 1,
          "clearing one surface keeps the others");
    stop_inproc(plugin);
}

// ---- mc.fx@1 ---------------------------------------------------------------------------

// Minimal DXBC container: header, one chunk with a program version token.
u32 fake_dxbc(u8 (&out)[64], const char* fourcc, u32 version) noexcept {
    clear_bytes(out, sizeof(out));
    copy_bytes(out, "DXBC", 4);
    byte_store_le32(out + 20, 1);
    byte_store_le32(out + 24, 64);
    byte_store_le32(out + 28, 1);
    byte_store_le32(out + 32, 36);
    copy_bytes(out + 36, fourcc, 4);
    byte_store_le32(out + 40, 16);
    byte_store_le32(out + 44, version);
    return 64;
}

void test_fx() noexcept {
    u8 code[64];
    check(native_plugin_sdk_effect_bytecode_valid({code, fake_dxbc(code, "SHEX", 0x0000'0050)}) &&
              !native_plugin_sdk_effect_bytecode_valid({code, fake_dxbc(code, "SHEX", 0x0001'0050)}) &&
              !native_plugin_sdk_effect_bytecode_valid({code, fake_dxbc(code, "SHDR", 0x0000'0040)}) &&
              !native_plugin_sdk_effect_bytecode_valid({code, fake_dxbc(code, "RDEF", 0x0000'0050)}) &&
              !native_plugin_sdk_effect_bytecode_valid({code, 60}),
          "effects accept only DXBC ps_5_0 containers with consistent sizes");
    auto plugin = start_inproc(0x5701, MC_PERM_EFFECTS, MC_PERM_ALL);
    const auto* fx = table<mc_fx_v1>("mc.fx", 1);
    check(plugin.enabled && fx, "mc.fx@1 is granted by the effects permission");
    if (!fx) { stop_inproc(plugin); return; }
    void* b = g_inproc.binding;
    const u64 g = g_inproc.generation;
    fake_dxbc(code, "SHEX", 0x0000'0050);
    mc_fx_effect effect{};
    effect.size = sizeof(effect);
    effect.effect_id = 1;
    effect.name = {"Aurora", 6};
    effect.bytecode = code;
    effect.bytecode_bytes = 64;
    effect.parameter_count = 2;
    effect.parameters[0] = 0.5f;
    effect.parameters[1] = 2.0f;
    const u64 revision = g_runtime.effect_revision;
    check(fx->register_effect(b, g, &effect) == MC_OK && g_runtime.effect_revision != revision, "an effect registers");
    NativePluginSdkEffectView views[4]{};
    check(native_plugin_sdk_effects(g_runtime, {views, 4}) == 1 && utf8_equal(views[0].name, utf8_literal("Aurora")) &&
              views[0].bytecode.count == 64 && views[0].parameters[1] == 2.0f,
          "registered effects are listed with copied bytecode and parameters");
    code[44] = 0x40; // the plugin's buffer changes after registration
    NativePluginSdkEffectView view{};
    check(native_plugin_sdk_effect(g_runtime, 0x5701, 1, &view) && view.bytecode.data[44] == 0x50, "bytecode is copied");
    u32 nan_bits = 0x7FC0'0000;
    float values[2]{1.0f, 0.0f};
    copy_bytes(&values[1], &nan_bits, 4);
    check(fx->set_parameters(b, g, 1, values, 2) == MC_E_INVALID && fx->set_parameters(b, g, 1, values, 1) == MC_OK &&
              native_plugin_sdk_effect(g_runtime, 0x5701, 1, &view) && view.parameters[0] == 1.0f,
          "parameters must be finite");
    effect.effect_id = 2;
    effect.bytecode = code;
    fake_dxbc(code, "SHEX", 0x0000'0050);
    mc_fx_effect third = effect;
    third.effect_id = 3;
    check(fx->register_effect(b, g, &effect) == MC_OK && fx->register_effect(b, g, &third) == MC_E_BUDGET,
          "two effects per plugin");
    check(native_plugin_sdk_effect_fault(&g_runtime, 0x5701, 1) && !native_plugin_sdk_effect(g_runtime, 0x5701, 1, &view) &&
              native_plugin_sdk_effects(g_runtime, {views, 4}) == 1,
          "a GPU guard fault unregisters only that effect");
    stop_inproc(plugin);
    check(!native_plugin_sdk_effects(g_runtime, {views, 4}), "effects leave with their plugin");
}

// ---- Showcase sample ----------------------------------------------------------------------

constexpr u64 kShowcaseOwner = 0x4D43534844574341ull;
bool read_sibling_file(const wchar_t* leaf, char (&buffer)[4096], u32* bytes) noexcept {
    wchar_t path[1024];
    u32 characters = 0;
    if (!sibling(leaf, path, &characters)) return false;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD read = 0;
    const bool ok = ReadFile(file, buffer, sizeof(buffer), &read, nullptr) != FALSE;
    CloseHandle(file);
    *bytes = read;
    return ok && read < sizeof(buffer);
}

void test_showcase() noexcept {
    install_fake_read();
    install_fake_sink();
    g_runtime.keybind_check = nullptr;
    g_clock = 200'000;
    char document[4096];
    u32 document_bytes = 0;
    NativePluginManifest manifest{};
    check(read_sibling_file(L"showcase.maicord-plugin.json", document, &document_bytes) &&
              native_plugin_manifest_parse({document, document_bytes}, &manifest).ok() &&
              native_plugin_manifest_valid(manifest) && manifest.owner_id == kShowcaseOwner,
          "the sample's stamped manifest parses");
    wchar_t path[1024];
    u32 characters = 0;
    NativePluginModule module{};
    const bool found = sibling(L"showcase.dll", path, &characters);
    const auto loaded = found ? native_plugin_module_load(&module, &g_host.host, {path, characters}, manifest.permissions)
                              : NativePluginModuleResult{NativePluginModuleError::invalid_argument};
    check(loaded.ok() && module.sdk && native_plugin_sdk_permissions(module.sdk) == manifest.permissions,
          "the sample loads with exactly the permissions its manifest declares");
    if (!loaded.ok()) {
        g_runtime.read = {};
        g_runtime.actions = {};
        return;
    }
    NativePluginSdkMenuItem items[4]{};
    NativePluginSdkEffectView effects[2]{};
    NativePluginSdkSlotNode nodes[4]{};
    check(native_plugin_sdk_menu_items(g_runtime, MC_MENU_MESSAGE, {items, 4}) == 1 &&
              native_plugin_sdk_menu_items(g_runtime, MC_MENU_USER, {items, 4}) == 1 &&
              native_plugin_sdk_menu_items(g_runtime, MC_MENU_LINK, {items, 4}) == 1 &&
              native_plugin_sdk_effects(g_runtime, {effects, 2}) == 1 &&
              utf8_equal(effects[0].name, utf8_literal("Showcase gradient")) &&
              native_plugin_sdk_effect_bytecode_valid(effects[0].bytecode) &&
              native_plugin_sdk_slots(g_runtime, {MC_SLOT_COMPOSER_TOOLBAR, 0, 0, 0, 0}, {nodes, 4}) == 1,
          "the sample registers menus, a composer button and a real ps_5_0 gradient");
    // Name suffix decorator, collapse for blocked authors.
    NativePluginSdkDecoration decoration{};
    auto message = render_message(600, 1);
    check(native_plugin_sdk_decorate(&g_runtime, message, true, &decoration) &&
              utf8_equal(decoration.name_suffix, utf8_literal("@alice")) && decoration.name_tone == MC_TONE_ROLE,
          "the sample adds @username after a differing display name in the role tone");
    message = render_message(602, 1, MC_RELATIONSHIP_BLOCKED);
    check(native_plugin_sdk_decorate(&g_runtime, message, true, &decoration) && decoration.visibility == MC_RENDER_COLLAPSED,
          "the sample collapses blocked users' messages");
    // Message logger: an edit is logged with its original and marked on the message.
    NativePluginSdkEvent created{MC_EVENT_MESSAGE_CREATE, 0, kGuild, kChannel, 600, kAlice, 0, utf8_literal("first take"),
                                 utf8_literal("Alice A")};
    NativePluginSdkEvent updated = created;
    updated.kind = MC_EVENT_MESSAGE_UPDATE;
    updated.text = utf8_literal("second take");
    NativePluginSdkEvent deleted{MC_EVENT_MESSAGE_DELETE, MC_EVENT_CACHED, kGuild, kChannel, 603, kAlice, 0,
                                 utf8_literal("oops"), utf8_literal("Alice A")};
    native_plugin_sdk_event_push(&g_runtime, created);
    native_plugin_sdk_event_push(&g_runtime, updated);
    native_plugin_sdk_event_push(&g_runtime, deleted);
    native_plugin_sdk_poll(&g_runtime);
    message = render_message(600, 2);
    const bool marked = native_plugin_sdk_decorate(&g_runtime, message, true, &decoration) && decoration.accessory_count == 2 &&
                        decoration.accessories[1].kind == MC_NODE_BUTTON;
    check(marked, "a logged edit adds an accessory row with Show original");
    check(marked && native_plugin_sdk_render_action(&g_runtime, kShowcaseOwner, kChannel, 600, decoration.accessories[1].action_id) &&
              g_runtime.popover_count == 3 && utf8_equal({g_runtime.popover[0].text, g_runtime.popover[0].text_bytes},
                                                         utf8_literal("first take")) &&
              native_plugin_sdk_popover_action(&g_runtime, g_runtime.popover_revision, 2) && g_sink.copies == 1 &&
              sink_says(utf8_literal("first take")),
          "Show original opens a popover whose Copy row copies the original text");
    const auto* descriptor = ui_page_registry_find(g_host.pages, {kShowcaseOwner, 1});
    NativePluginSettingsSnapshot snapshot{};
    UiSettingsControl controls[24]{};
    UiSettingsPageContent content{};
    bool logged_delete = false, logged_edit = false;
    if (descriptor && native_plugin_settings_snapshot(descriptor->context, &snapshot) == NativePluginSettingsError::none &&
        native_plugin_settings_project(snapshot, {controls, 24}, &content) == NativePluginSettingsError::none)
        for (u32 i = 0; i < content.controls.count; ++i) {
            logged_delete = logged_delete || test_contains(controls[i].label, utf8_literal("Deleted \xC2\xB7 Alice A: oops"));
            logged_edit = logged_edit || test_contains(controls[i].label, utf8_literal("Edited \xC2\xB7 Alice A: first take"));
        }
    check(logged_delete && logged_edit, "the settings page lists deleted and edited messages");
    // Quick reply from the message menu: modal, then a reply through mc.actions.
    NativePluginSdkMenuContext reply{};
    reply.target = MC_MENU_MESSAGE;
    reply.channel_id = kChannel;
    reply.message_id = 600;
    reply.user_id = kAlice;
    check(native_plugin_sdk_menu_activate(&g_runtime, kShowcaseOwner, 1, reply) && g_sink.modals == 1 &&
              utf8_equal(g_sink.modal.title, utf8_literal("Reply to Alice A")) &&
              native_plugin_sdk_modal_result(&g_runtime, kShowcaseOwner, g_sink.modal.modal_id, true, utf8_literal("on it")) &&
              g_sink.sends == 1 && g_sink.last_reply == 600 && g_sink.last_channel == kChannel && sink_says(utf8_literal("on it")),
          "quick reply from the menu sends the modal text as a reply");
    // The same through the keybind: the newest message from someone else.
    check(native_plugin_sdk_keybind_press(&g_runtime, {'R', 5}, true) && g_sink.modals == 2 && g_sink.modal.modal_id == 600,
          "the quick reply keybind targets the newest message from someone else");
    native_plugin_sdk_modal_result(&g_runtime, kShowcaseOwner, g_sink.modal.modal_id, false, {});
    reply.target = MC_MENU_USER;
    check(native_plugin_sdk_menu_activate(&g_runtime, kShowcaseOwner, 2, reply) && sink_says(utf8_literal("6")),
          "Copy user ID copies the decimal id");
    check(native_plugin_module_unload(&module).ok() && !native_plugin_sdk_effects(g_runtime, {effects, 2}) &&
              !native_plugin_sdk_menu_items(g_runtime, MC_MENU_MESSAGE, {items, 4}),
          "unloading the sample removes everything it registered");
    remove_owner_store(kShowcaseOwner);
    g_runtime.read = {};
    g_runtime.actions = {};
}
