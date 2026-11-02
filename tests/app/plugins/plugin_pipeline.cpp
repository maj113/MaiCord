#include "maicord/app/plugins/binding.hpp"
#include "maicord/app/plugins/keep.hpp"
#include "maicord/app/plugins/pipeline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "plugins/binding_internal.hpp"
#include "../../support/test_output.hpp"

#include "maicord/core/win32_kernel.hpp"

// The message pipeline against real DLLs (tests/fixtures/plugin_runtime_fixture.c, variants PIPELINE,
// PIPELINEAUTO and PIPELINEBARE, plus FULL and the clear-urls sample): the pre_send verdict matrix, the hook
// and event call counts, kept messages and their caps, timers, and the permission, gesture and rate gates.
namespace maicord::plugin_pipeline_tests {
using namespace plugin_runtime_detail;

u32 failures;
void check(bool value, const char* label) noexcept {
    if (value) return;
    ++failures;
    test_write_text("FAIL plugin_pipeline: ");
    test_write_line(label);
}

constexpr u64 kFullOwner = 0x4D43525446554C4Cull, kPipelineOwner = 0x4D43525450495045ull,
              kAutoOwner = 0x4D43525450495041ull, kBareOwner = 0x4D43525450495042ull;
constexpr u64 kChannel = 9;

// The pipeline fixtures' state block (tests/fixtures/plugin_runtime_fixture.c).
struct PipelineState {
    u32 clicks, deleted, edited, typing, timers, batches, events, overflows;
    u32 click_flags, click_button, click_count, click_mods;
    u32 deleted_flags, deleted_bytes, edited_before, edited_after, last_event_flags, last_event_text;
    u32 kinds_seen, largest_batch, kept_flags, kept_edits, kept_text_bytes;
    u32 padding;
    u64 overflow_value, last_timer, local_id;
    i32 status[16];
    i32 click_react_status, timer_send_status, truncated_status;
    i32 level2[9];
    u64 reply_author, clock_ms, open_reply, first_message, voice;
    u32 message_count, group_count;
    i32 offset;
};
struct FullState {
    u32 starts, stops, sends, start_flags;
    i32 toast_status, setting_status, store_set_status, store_get_status, store_missing_status, store_truncated_status;
    u32 label_before, label_during, store_bytes, count;
};

u32 length(const char* text) noexcept {
    u32 count = 0;
    while (text[count]) ++count;
    return count;
}
u32 length(const wchar_t* text) noexcept {
    u32 count = 0;
    while (text[count]) ++count;
    return count;
}
ConstSlice<wchar_t> leaf(const wchar_t* name) noexcept {
    return {name, length(name)};
}
bool same(Utf8View text, const char* expected) noexcept {
    const u32 count = length(expected);
    return text.bytes == count && bytes_equal(text.data, expected, count);
}
void join(wchar_t (&output)[MAX_PATH], const wchar_t* directory, const wchar_t* name) noexcept {
    u32 at = 0;
    for (; directory[at]; ++at) output[at] = directory[at];
    output[at++] = L'\\';
    for (u32 i = 0; name[i]; ++i) output[at++] = name[i];
    output[at] = 0;
}

struct Root {
    wchar_t base[MAX_PATH], plugins[MAX_PATH], data[MAX_PATH];
};
bool make_root(Root* root) noexcept {
    wchar_t temp[MAX_PATH], path[MAX_PATH];
    const DWORD count = GetTempPathW(MAX_PATH, temp);
    if (!count || count >= MAX_PATH || !GetTempFileNameW(temp, L"mcp", 0, path) || !DeleteFileW(path) ||
        !CreateDirectoryW(path, nullptr))
        return false;
    u32 at = 0;
    for (; path[at]; ++at) root->base[at] = path[at];
    root->base[at] = 0;
    join(root->plugins, root->base, L"plugins");
    join(root->data, root->base, L"data");
    return CreateDirectoryW(root->plugins, nullptr) && CreateDirectoryW(root->data, nullptr);
}
void remove_tree(const wchar_t* directory) noexcept {
    wchar_t pattern[MAX_PATH];
    join(pattern, directory, L"*");
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW(pattern, &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (found.cFileName[0] == L'.' && (!found.cFileName[1] || (found.cFileName[1] == L'.' && !found.cFileName[2])))
                continue;
            wchar_t child[MAX_PATH];
            join(child, directory, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(child);
            else DeleteFileW(child);
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    RemoveDirectoryW(directory);
}
bool install(const Root& root, const wchar_t* built, const wchar_t* name) noexcept {
    wchar_t directory[MAX_PATH], source[MAX_PATH], destination[MAX_PATH];
    const DWORD count = GetModuleFileNameW(nullptr, directory, MAX_PATH);
    if (!count || count >= MAX_PATH) return false;
    u32 at = count;
    while (at && directory[at - 1] != L'\\') --at;
    if (!at) return false;
    directory[at - 1] = 0;
    join(source, directory, built);
    join(destination, root.plugins, name);
    return CopyFileW(source, destination, FALSE) != 0;
}

PluginEnableResult enable(PluginRuntime* runtime, const Root& root, const wchar_t* built, const wchar_t* name,
                          u64 approved = MC_PERM_KNOWN) noexcept {
    install(root, built, name);
    return plugin_runtime_enable(runtime, leaf(name), approved);
}

PluginMessage message(u64 id, const char* text, u32 flags = 0, u64 channel = kChannel) noexcept {
    PluginMessage value{};
    value.message_id = id;
    value.channel_id = channel;
    value.guild_id = 4;
    value.author_id = 5;
    value.timestamp_ms = 1000 + id;
    value.content = {text, length(text)};
    value.author_name = {"Ann", 3};
    value.flags = flags;
    return value;
}

PluginDraftResult draft(const char* text, PluginHook hook = PluginHook::pre_send, u32 flags = 0) noexcept {
    return plugin_dispatch_draft(hook, {77, 88, hook == PluginHook::pre_edit ? 99u : 0u, 0, {text, length(text)}, flags});
}

PluginEvent event(u32 kind, u64 value = 0) noexcept {
    PluginEvent result{};
    result.kind = kind;
    result.channel_id = kChannel;
    result.user_id = 5;
    result.value = value;
    result.time_ms = 123;
    return result;
}

// What the application would do for the plugin, counted.
struct Sink {
    u32 sends, edits, reacts, deletes, reads, activities, cleared, locals, local_removes, composer, keep_changed;
    u64 owner, channel, message;
    u32 flags;
    char text[16];
    mc_status result;
};
Sink g_sink;
void note(u64 owner, u64 channel, u64 message_id) noexcept {
    g_sink.owner = owner;
    g_sink.channel = channel;
    g_sink.message = message_id;
}
mc_status sink_send(u64 owner, const PluginSend& send) noexcept {
    ++g_sink.sends;
    note(owner, send.channel_id, send.reply_to);
    g_sink.flags = send.flags;
    g_sink.text[0] = 0;
    if (send.text.bytes < sizeof(g_sink.text)) {
        copy_bytes(g_sink.text, send.text.data, send.text.bytes);
        g_sink.text[send.text.bytes] = 0;
    }
    return g_sink.result;
}
mc_status sink_edit(u64 owner, u64 channel, u64 message_id, Utf8View) noexcept {
    ++g_sink.edits;
    note(owner, channel, message_id);
    return g_sink.result;
}
mc_status sink_react(u64 owner, u64 channel, u64 message_id, Utf8View, bool) noexcept {
    ++g_sink.reacts;
    note(owner, channel, message_id);
    return g_sink.result;
}
mc_status sink_delete(u64 owner, u64 channel, u64 message_id) noexcept {
    ++g_sink.deletes;
    note(owner, channel, message_id);
    return g_sink.result;
}
mc_status sink_read(u64 owner, u32, u64 id) noexcept {
    ++g_sink.reads;
    note(owner, id, 0);
    return g_sink.result;
}
mc_status sink_activity(u64, const mc_activity* activity) noexcept {
    ++(activity ? g_sink.activities : g_sink.cleared);
    return g_sink.result;
}
mc_status sink_local(u64 owner, u64 channel, Utf8View, u64* id) noexcept {
    ++g_sink.locals;
    note(owner, channel, 0);
    *id = 4242;
    return g_sink.result;
}
mc_status sink_local_remove(u64, u64, u64) noexcept {
    ++g_sink.local_removes;
    return g_sink.result;
}
mc_status sink_composer_text(void*, u32 capacity, u32* bytes) noexcept {
    ++g_sink.composer;
    *bytes = 5;
    return capacity < 5 ? MC_E_TRUNCATED : MC_OK;
}
mc_status sink_composer_insert(Utf8View) noexcept {
    ++g_sink.composer;
    return g_sink.result;
}
mc_status sink_composer_replace(Utf8View) noexcept {
    ++g_sink.composer;
    return g_sink.result;
}
mc_status sink_composer_focus() noexcept {
    ++g_sink.composer;
    return g_sink.result;
}
mc_status sink_composer_reply(u64, u64 message_id, bool) noexcept {
    ++g_sink.composer;
    g_sink.message = message_id;
    return g_sink.result;
}
mc_status sink_composer_edit(u64, u64) noexcept {
    ++g_sink.composer;
    return g_sink.result;
}
void sink_keep_changed() noexcept {
    ++g_sink.keep_changed;
}
mc_status sink_open_channel(mc_open_channel* out) noexcept {
    *out = {kChannel, 4, 31, 0};
    return MC_OK;
}
// Three loaded messages, newest 13; capacity and `before` are the runtime's to pass through.
mc_status sink_messages(u64 channel, u64 before, mc_message_ref* out, u32 capacity, u32* count) noexcept {
    if (channel != 9 || before) return MC_E_NOT_FOUND;
    for (u64 id = 13; id > 10 && *count < capacity; --id) out[(*count)++] = {id, 5, 1000 + id, MC_MESSAGE_OWN, 0};
    return MC_OK;
}
const PluginPipelineSink kSink{sink_send,           sink_edit,           sink_react,         sink_delete,
                               sink_read,           sink_activity,       sink_local,         sink_local_remove,
                               sink_composer_text,  sink_composer_insert, sink_composer_replace, sink_composer_focus,
                               sink_composer_reply, sink_composer_edit,  sink_keep_changed,  sink_open_channel,
                               sink_messages};

PipelineState& state_of(PluginBinding* binding) noexcept {
    return *static_cast<PipelineState*>(binding->ctx.state);
}

// ---- Tests --------------------------------------------------------------------------------

// With nothing enabled every entry point is a branch: no call, no allocation, no state.
void test_no_plugins() noexcept {
    const PluginPipelineStats before = plugin_pipeline_stats();
    const PluginMessage text = message(1, "hello");
    const PluginClick click{MC_CLICK_LEFT, 1, 0};
    check(!plugin_dispatch_deleted(text) && !plugin_dispatch_edited(text, text) && !plugin_dispatch_click(text, click) &&
              !plugin_dispatch_typing(kChannel),
          "nothing handles or keeps a message without plugins");
    check(plugin_events_interest() == 0 && !plugin_events_push(event(MC_EVENT_TYPING)) && !plugin_events_deliver() &&
              !plugin_events_pending(),
          "no event is translated, queued or delivered without a subscriber");
    check(plugin_timers_next_due() == 0 && plugin_timers_poll(clock_monotonic_milliseconds()) == 0,
          "no timer exists without a plugin");
    const PluginPipelineStats after = plugin_pipeline_stats();
    check(bytes_equal(&before, &after, sizeof(before)) && keep_stats().messages == 0 && !keep_is_deleted(1),
          "the pipeline counted no call and the keep store holds nothing");
}

// pre_send: every verdict, flags, confirm and ordering across plugins.
void test_draft_matrix(PluginRuntime* runtime, const Root& root) noexcept {
    const PluginEnableResult full = enable(runtime, root, L"MaiCordPluginRuntimeFullFixture.dll", L"full.dll",
                                           MC_PERM_MESSAGE_SEND);
    check(full.ok(), "the full fixture is enabled for the draft matrix");
    if (!full.ok()) return;
    const PluginDraftResult plain = draft("hello", PluginHook::pre_send, MC_DRAFT_SILENT);
    check(plain.verdict == PluginDraftVerdict::keep && plain.flags == MC_DRAFT_SILENT && !plain.text.bytes &&
              !plain.question.bytes,
          "unchanged text keeps the draft and the flags it came with");
    const PluginDraftResult replaced = draft("replace");
    check(replaced.verdict == PluginDraftVerdict::replace && same(replaced.text, "replaced") && replaced.flags == 0,
          "replace carries the new text");
    const PluginDraftResult cancelled = draft("cancel");
    check(cancelled.verdict == PluginDraftVerdict::cancel && cancelled.owner_id == kFullOwner && !cancelled.text.bytes,
          "cancel names the plugin that stopped the draft");
    check(draft("invalid").verdict == PluginDraftVerdict::keep, "an invalid replacement leaves the draft alone");
    check(draft("fail").verdict == PluginDraftVerdict::keep, "a failing hook leaves the draft alone");
    const PluginDraftResult flagged = draft("silent", PluginHook::pre_send, MC_DRAFT_SUPPRESS_EMBEDS);
    check(flagged.verdict == PluginDraftVerdict::keep &&
              flagged.flags == (MC_DRAFT_SUPPRESS_EMBEDS | MC_DRAFT_SILENT | MC_DRAFT_REPLY_PING),
          "a hook can change the flags without touching the text");
    check(draft("silentfail", PluginHook::pre_send, 0).flags == 0, "flags from a failing hook are dropped");
    check(draft("silent", PluginHook::pre_edit, 0).flags == 0, "an edit ignores flag changes");
    const PluginDraftResult asked = draft("confirm");
    check(asked.verdict == PluginDraftVerdict::confirm && same(asked.question, "Sure?") && asked.owner_id == kFullOwner &&
              !asked.text.bytes,
          "confirm hands over the plugin's question and leaves the text unchanged");
    check(draft("confirm", PluginHook::pre_edit).verdict != PluginDraftVerdict::confirm,
          "an edit is never confirmed");
    // pre_send runs as the user's gesture.
    draft("gesture");
    check(static_cast<FullState*>(full.binding->ctx.state)->count == MC_CTX_GESTURE, "pre_send runs inside a gesture");

    // A second plugin sees what the first produced.
    const PluginEnableResult second = enable(runtime, root, L"clear-urls.dll", L"clear-urls.dll", MC_PERM_MESSAGE_SEND);
    check(second.ok(), "clear-urls is enabled behind the full fixture");
    if (second.ok()) {
        const PluginDraftResult cleaned = draft("https://example.com/a?utm_source=x&id=7");
        check(cleaned.verdict == PluginDraftVerdict::replace && same(cleaned.text, "https://example.com/a?id=7"),
              "the second plugin rewrites text the first left alone");
        check(draft("silent").flags == (MC_DRAFT_SILENT | MC_DRAFT_REPLY_PING),
              "flags set by the first plugin survive the second");
        check(draft("cancel").verdict == PluginDraftVerdict::cancel, "a cancel from the first plugin wins");
        check(draft("confirm").verdict == PluginDraftVerdict::confirm, "a question survives a later plugin's pass");
        plugin_runtime_disable(runtime, second.binding);
    }
    plugin_runtime_disable(runtime, full.binding);
}

void test_hooks(PluginRuntime* runtime, const Root& root) noexcept {
    const PluginEnableResult enabled = enable(runtime, root, L"MaiCordPluginRuntimePipelineFixture.dll", L"pipeline.dll");
    check(enabled.ok() && enabled.binding->owner_id == kPipelineOwner, "the pipeline fixture is enabled");
    if (!enabled.ok()) return;
    PluginBinding* binding = enabled.binding;
    PipelineState& state = state_of(binding);
    check(plugin_hook_subscribed(PluginHook::message_click) && plugin_hook_subscribed(PluginHook::message_deleted) &&
              plugin_hook_subscribed(PluginHook::message_edited) && plugin_hook_subscribed(PluginHook::events) &&
              plugin_hook_subscribed(PluginHook::pre_typing) && plugin_hook_subscribed(PluginHook::timer),
          "every pipeline hook is published");
    const PluginPipelineStats start = plugin_pipeline_stats();

    // message_click.
    const PluginMessage plain = message(10, "plain");
    check(!plugin_dispatch_click(plain, {MC_CLICK_LEFT, 1, MC_MOD_SHIFT}) && state.clicks == 1 &&
              state.click_button == MC_CLICK_LEFT && state.click_count == 1 && state.click_mods == MC_MOD_SHIFT &&
              (state.click_flags & MC_CTX_GESTURE),
          "a click reaches the hook inside a gesture and passes");
    check(plugin_dispatch_click(message(11, "handle it"), {MC_CLICK_LEFT, 2, 0}) && state.clicks == 2 &&
              state.click_count == 2,
          "a double click the hook handles is reported as handled");
    check(!plugin_dispatch_click(plain, {MC_CLICK_MIDDLE, 1, MC_MOD_CTRL | MC_MOD_ALT}) && state.clicks == 3 &&
              state.click_button == MC_CLICK_MIDDLE && state.click_mods == (MC_MOD_CTRL | MC_MOD_ALT),
          "a middle click carries its modifiers");
    check(plugin_pipeline_stats().click_calls == start.click_calls + 3, "three clicks are three calls");

    // message_deleted: keep or drop, with the cached copy.
    const PluginMessage dropped = message(100, "drop me", MC_MESSAGE_CACHED);
    check(!plugin_dispatch_deleted(dropped) && state.deleted == 1 && state.deleted_bytes == 7 &&
              state.deleted_flags == MC_MESSAGE_CACHED && !keep_is_deleted(100) && keep_stats().messages == 0,
          "a message the plugin drops is forgotten");
    check(plugin_dispatch_deleted(message(101, "keep this", MC_MESSAGE_CACHED)) && keep_is_deleted(101) &&
              keep_stats().messages == 1 && state.deleted == 2,
          "a message the plugin keeps becomes a tombstone");
    KeepInfo info{};
    Utf8View text{};
    check(keep_info(kChannel, 101, &info) && info.deleted && info.edit_count == 0 && info.author_id == 5 &&
              info.timestamp_ms == 1101 && keep_text(kChannel, 101, 0, &text) && same(text, "keep this"),
          "the tombstone holds the message as it was");
    check(!plugin_dispatch_deleted(message(102, "")) && !keep_is_deleted(102),
          "a message the plugin has no text to judge is dropped");

    // message_edited: the earlier text joins the history.
    const PluginMessage before = message(110, "first text"), after_drop = message(110, "plain edit");
    check(!plugin_dispatch_edited(before, after_drop) && state.edited == 1 && state.edited_before == 10 &&
              state.edited_after == 10 && keep_stats().messages == 1,
          "an edit the plugin does not keep leaves no history");
    PluginMessage after = message(110, "keep second");
    after.edited_ms = 2000;
    check(plugin_dispatch_edited(before, after) && keep_info(kChannel, 110, &info) && info.edit_count == 1 &&
              !info.deleted && info.edited_ms == 2000 && keep_text(kChannel, 110, 0, &text) && same(text, "keep second") &&
              keep_text(kChannel, 110, 1, &text) && same(text, "first text"),
          "a kept edit holds the latest text and the one before");
    PluginMessage third = message(110, "keep third");
    third.edited_ms = 3000;
    check(plugin_dispatch_edited(after, third) && keep_info(kChannel, 110, &info) && info.edit_count == 2 &&
              keep_text(kChannel, 110, 1, &text) && same(text, "keep second") && keep_text(kChannel, 110, 2, &text) &&
              same(text, "first text") && !keep_text(kChannel, 110, 3, &text),
          "earlier texts are listed most recent first");
    check(plugin_dispatch_deleted(message(110, "keep third")) && keep_is_deleted(110) &&
              keep_info(kChannel, 110, &info) && info.deleted && info.edit_count == 2,
          "deleting an edited message keeps its history");

    // pre_typing: the first suppression ends the pass.
    check(!plugin_dispatch_typing(kChannel) && plugin_dispatch_typing(0x5555) && state.typing == 2 &&
              plugin_pipeline_stats().typing_calls == start.typing_calls + 2,
          "typing is suppressed only where the plugin says");

    // Events: batches, kinds, text limits and overflow.
    check(plugin_events_interest() == MC_EVENT_KINDS, "the events interest is the plugin's filter");
    check(!plugin_events_push(event(MC_EVENT_KIND_COUNT)) && !plugin_events_push(event(MC_EVENT_OVERFLOW)),
          "an unknown kind is never queued");
    const u32 calls = plugin_pipeline_stats().event_calls;
    check(plugin_events_push(event(MC_EVENT_TYPING)) && plugin_events_push(event(MC_EVENT_PRESENCE, MC_PRESENCE_IDLE)) &&
              plugin_events_push(event(MC_EVENT_RELATIONSHIP_ADD, 1)) && plugin_events_pending() &&
              state.batches == 0,
          "events wait in the queue until the host delivers them");
    check(!plugin_events_deliver() && state.batches == 1 && state.events == 3 && state.largest_batch == 3 &&
              state.kinds_seen == ((1u << MC_EVENT_TYPING) | (1u << MC_EVENT_PRESENCE) | (1u << MC_EVENT_RELATIONSHIP_ADD)) &&
              plugin_pipeline_stats().event_calls == calls + 1,
          "three events arrive in one batch, one call");
    check(!plugin_events_deliver() && state.batches == 1, "a delivered event is not delivered twice");
    PluginEvent big = event(MC_EVENT_MESSAGE_CREATE);
    char long_text[600];
    for (char& character : long_text) character = 'x';
    big.text = {long_text, sizeof(long_text)};
    big.flags = MC_EVENT_TEXT;
    plugin_events_push(big);
    plugin_events_deliver();
    check(state.last_event_text == kPluginEventTextBytes && (state.last_event_flags & MC_EVENT_TRUNCATED) &&
              (state.last_event_flags & MC_EVENT_TEXT),
          "text over the slot is cut and marked");
    for (u32 i = 0; i < 70; ++i) plugin_events_push(event(MC_EVENT_TYPING));
    const u32 batches = state.batches, delivered = state.events;
    check(!plugin_events_deliver() && state.batches == batches + 2 && state.events == delivered + 70 &&
              state.largest_batch == MC_EVENT_BATCH_MAX,
          "seventy events take two batches of at most sixty-four");
    const u32 lost = plugin_pipeline_stats().events_lost;
    for (u32 i = 0; i < kPluginEventSlots + 72; ++i) plugin_events_push(event(MC_EVENT_TYPING));
    const u32 before_events = state.events;
    check(!plugin_events_deliver() && state.overflows == 1 && state.overflow_value == 72 &&
              state.events == before_events + kPluginEventSlots &&
              plugin_pipeline_stats().events_lost == lost + 72,
          "a plugin that fell behind is told how many events it lost");

    // Timers.

    draft("timer");
    check(state.status[12] == MC_OK && state.status[13] == MC_E_INVALID && plugin_timers_next_due() != 0,
          "a timer starts; an interval under the minimum is refused");
    check(plugin_timers_poll(clock_monotonic_milliseconds()) == 0 && state.timers == 0,
          "a timer is not called before it is due");
    g_sink.result = MC_OK;
    check(plugin_timers_poll(plugin_timers_next_due()) == 1 && state.timers == 1 && state.last_timer == 1 &&
              plugin_timers_next_due() == 0,
          "a one-shot timer fires once and is gone");
    check(state.timer_send_status == MC_E_GESTURE && g_sink.sends == 0,
          "a timer is no gesture: its send is refused without the automated permission");
    check(plugin_pipeline_stats().timer_calls == start.timer_calls + 1, "the timer call is counted");

    check(plugin_runtime_disable(runtime, binding) && plugin_events_interest() == 0 && plugin_timers_next_due() == 0 &&
              !plugin_hook_subscribed(PluginHook::message_click) && !plugin_events_push(event(MC_EVENT_TYPING)),
          "disabling drops the hooks, the event queue and the timers");
    check(keep_is_deleted(101), "kept messages outlive the plugin that kept them until the store is cleared");
    keep_clear();
}

void test_actions(PluginRuntime* runtime, const Root& root) noexcept {
    plugin_pipeline_set_sink(&kSink);

    // A plugin with the permissions but no automation: actions work inside a gesture.
    const PluginEnableResult enabled = enable(runtime, root, L"MaiCordPluginRuntimePipelineFixture.dll", L"pipeline.dll");
    check(enabled.ok(), "the pipeline fixture is enabled for the action tests");
    if (!enabled.ok()) return;
    PipelineState& state = state_of(enabled.binding);
    g_sink = {};
    g_sink.result = MC_OK;
    const PluginPipelineStats start = plugin_pipeline_stats();
    draft("send");
    check(state.status[0] == MC_OK && g_sink.sends == 1 && g_sink.owner == kPipelineOwner && g_sink.channel == kChannel &&
              g_sink.text[0] == 'h' && g_sink.flags == 0,
          "a send inside a gesture reaches the application");
    draft("edit");
    draft("react");
    draft("delete");
    draft("read");
    check(state.status[1] == MC_OK && state.status[2] == MC_OK && state.status[3] == MC_OK && state.status[4] == MC_OK &&
              g_sink.edits == 1 && g_sink.reacts == 1 && g_sink.deletes == 1 && g_sink.reads == 1,
          "edit, react, delete and mark read work inside a gesture");
    draft("activity");
    check(state.status[5] == MC_OK && state.status[6] == MC_OK && g_sink.activities == 1 && g_sink.cleared == 1,
          "an activity is set and cleared");
    draft("activity");
    draft("activity");
    check(state.status[5] == MC_E_RATE_LIMITED && state.status[6] == MC_OK && g_sink.activities == 2 &&
              g_sink.cleared == 3,
          "clearing an activity is never rate-limited");
    draft("local");
    check(state.status[7] == MC_OK && g_sink.locals == 1 && state.local_id == 4242,
          "a local message returns its id");
    draft("composer");
    check(state.status[8] == MC_OK && state.status[9] == MC_OK && state.status[10] == MC_OK &&
              state.status[11] == MC_E_TRUNCATED && g_sink.composer == 4,
          "the composer is inserted into, replaced, focused and read");
    // Level 2: the replied-to author, the clock, the open channel, loaded messages, clearing a reply; the cache
    // reads answer unsupported without the application's reader.
    g_sink.composer = 0;
    plugin_dispatch_draft(PluginHook::pre_send, {77, 88, 0, 31, {"level2", 6}, 0, 12345});
    check(state.reply_author == 12345, "pre_send sees the replied-to message's author");
    check(state.level2[0] == MC_OK && state.clock_ms > 1'600'000'000'000ull && state.level2[1] == MC_OK &&
              state.offset >= -14 * 3600 && state.offset <= 14 * 3600,
          "the clock and the local offset answer");
    check(state.level2[2] == MC_OK && state.open_reply == 31, "the open channel carries the reply target");
    check(state.level2[3] == MC_OK && state.message_count == 3 && state.first_message == 13,
          "loaded messages come newest first");
    check(state.level2[4] == MC_OK && g_sink.composer == 1 && g_sink.message == 0 && state.level2[5] == MC_E_INVALID,
          "message 0 clears the reply, and only without a ping");
    check(state.level2[6] == MC_E_UNSUPPORTED && state.level2[7] == MC_E_UNSUPPORTED,
          "the cache reads need the application's reader");
    check(state.level2[8] == MC_OK && g_sink.locals == 2 && g_sink.channel == 0,
          "a local message in channel 0 goes to the open channel");
    // A click is a gesture: its reaction goes through.
    plugin_dispatch_click(message(10, "react please"), {MC_CLICK_LEFT, 1, 0});
    check(state.click_react_status == MC_OK && g_sink.reacts == 2, "a reaction from a click reaches the application");

    // The rate limit: five message actions per window.
    u32 refused = 0;
    for (u32 i = 0; i < 6; ++i) {
        draft("send");
        refused += state.status[0] == MC_E_RATE_LIMITED;
    }
    check(refused == 4 && g_sink.sends + g_sink.edits + g_sink.deletes == kPluginActionsPerWindow &&
              plugin_pipeline_stats().action_denials > start.action_denials,
          "sends past the window's limit are refused and never reach the application");

    // Errors from the application come back to the plugin.
    g_sink.result = MC_E_NOT_FOUND;
    draft("react");
    check(state.status[2] == MC_E_NOT_FOUND, "the application's status is the plugin's status");
    g_sink.result = MC_OK;

    // keep_restore needs MC_PERM_PERSISTENT_LOG; this plugin declares it.
    draft("restore");
    check(state.status[14] == MC_OK && g_sink.keep_changed == 1 && keep_is_deleted(77), "a log restores a tombstone");
    draft("keep");
    check(state.status[15] == MC_OK && state.kept_flags == MC_KEPT_DELETED && state.kept_edits == 2 &&
              state.truncated_status == MC_E_TRUNCATED && state.kept_text_bytes == 5,
          "the restored message reads back with its earlier texts");
    KeepMessage tombstones[4];
    check(keep_tombstones(kChannel, tombstones, 4) == 1 && tombstones[0].message_id == 77 &&
              same(tombstones[0].content, "gone") && same(tombstones[0].author_name, "Ann"),
          "the restored tombstone is listed for its channel");
    Utf8View older{};
    check(keep_text(kChannel, 77, 1, &older) && same(older, "older") && keep_text(kChannel, 77, 2, &older) &&
              same(older, "oldest"),
          "restored earlier texts keep their order");
    keep_clear();

    // Without a sink the actions are unsupported, after the gates.
    plugin_pipeline_set_sink(nullptr);
    draft("send");
    check(state.status[0] == MC_E_UNSUPPORTED, "an application without a sink reports the action unsupported");
    plugin_pipeline_set_sink(&kSink);
    const u32 cleared = g_sink.cleared;
    plugin_runtime_disable(runtime, enabled.binding);
    check(g_sink.cleared == cleared + 1, "disabling a plugin that may set an activity clears it");


    // A plugin with no permissions: every account action is denied; local messages and kept reads are not.
    const PluginEnableResult bare =
        enable(runtime, root, L"MaiCordPluginRuntimePipelineBareFixture.dll", L"bare.dll");
    check(bare.ok() && bare.binding->owner_id == kBareOwner, "the permission-free fixture is enabled");
    if (bare.ok()) {
        PipelineState& denied = state_of(bare.binding);
        g_sink = {};
        g_sink.result = MC_OK;
        draft("send");
        draft("edit");
        draft("react");
        draft("delete");
        draft("read");
        draft("activity");
        draft("composer");
        draft("restore");
        check(denied.status[0] == MC_E_DENIED && denied.status[1] == MC_E_DENIED && denied.status[2] == MC_E_DENIED &&
                  denied.status[3] == MC_E_DENIED && denied.status[4] == MC_E_DENIED && denied.status[5] == MC_E_DENIED &&
                  denied.status[8] == MC_E_DENIED && denied.status[14] == MC_E_DENIED,
              "actions, activity, composer and restore are denied without their permissions");
        check(g_sink.sends + g_sink.edits + g_sink.reacts + g_sink.deletes + g_sink.reads + g_sink.activities +
                      g_sink.composer + g_sink.keep_changed == 0,
              "a denied action never reaches the application");
        draft("local");
        check(denied.status[7] == MC_OK && g_sink.locals == 1, "a local message needs no permission");
        draft("timer");
        plugin_timers_poll(plugin_timers_next_due());
        check(denied.timer_send_status == MC_E_DENIED, "a timer's send without any permission is denied, not a gesture error");
        plugin_runtime_disable(runtime, bare.binding);
    }

    // The automated permission lifts the gesture requirement for timers and events.
    const PluginEnableResult automated =
        enable(runtime, root, L"MaiCordPluginRuntimePipelineAutoFixture.dll", L"auto.dll");
    check(automated.ok() && automated.binding->owner_id == kAutoOwner, "the automated fixture is enabled");
    if (automated.ok()) {
        PipelineState& auto_state = state_of(automated.binding);
        g_sink = {};
        g_sink.result = MC_OK;
        draft("timer");
        plugin_timers_poll(plugin_timers_next_due());
        check(auto_state.timers == 1 && auto_state.timer_send_status == MC_OK && g_sink.sends == 1,
              "with the automated permission a timer sends without a gesture");
        plugin_runtime_disable(runtime, automated.binding);
    }
    plugin_pipeline_set_sink(nullptr);
}

// ---- The keep store: lifecycle and caps ------------------------------------------------------

void fill_text(char* text, u32 bytes, u32 seed) noexcept {
    for (u32 i = 0; i < bytes; ++i) text[i] = static_cast<char>('a' + (seed + i) % 26);
}

void test_keep_store() noexcept {
    keep_clear();
    check(keep_stats().messages == 0 && !keep_is_deleted(5), "the store starts empty");
    char text[kKeepContentMax + 1];
    fill_text(text, sizeof(text), 0);
    KeepMessage item{1, 1000, 5, 7, 0, {text, 20}, {"Ann", 3}};
    check(keep_deleted(item) && keep_is_deleted(1000) && keep_stats().messages == 1, "a deleted message is kept");
    check(!keep_deleted({1, 1001, 5, 7, 0, {text, 0}, {"Ann", 3}}) && !keep_deleted({0, 1001, 5, 7, 0, {text, 3}, {}}) &&
              !keep_deleted({1, 0, 5, 7, 0, {text, 3}, {}}) &&
              !keep_deleted({1, 1001, 5, 7, 0, {text, kKeepContentMax + 1}, {}}) && keep_stats().messages == 1,
          "empty, anonymous and oversized messages are refused");
    check(keep_deleted({1, 1002, 5, 7, 0, {text, kKeepContentMax}, {}}), "content up to the cap is kept");
    const char invalid[] = {'\xff', 'a'};
    check(!keep_deleted({1, 1003, 5, 7, 0, {invalid, 2}, {}}), "text that is not UTF-8 is refused");
    keep_clear();

    // 32 per channel: the oldest tombstone makes room.
    for (u32 i = 0; i < kKeepPerChannelMax + 8; ++i) keep_deleted({2, 2000 + i, 5, 7, 0, {text, 10}, {}});
    KeepStats stats = keep_stats();
    check(stats.messages == kKeepPerChannelMax && stats.evicted == 8 && !keep_is_deleted(2000) && !keep_is_deleted(2007) &&
              keep_is_deleted(2008) && keep_is_deleted(2000 + kKeepPerChannelMax + 7),
          "a channel keeps at most thirty-two messages, newest first");
    KeepMessage listed[kKeepPerChannelMax];
    check(keep_tombstones(2, listed, kKeepPerChannelMax) == kKeepPerChannelMax && listed[0].message_id == 2008 &&
              listed[kKeepPerChannelMax - 1].message_id == 2000 + kKeepPerChannelMax + 7,
          "tombstones list oldest first");
    check(keep_tombstones(2, listed, 4) == kKeepPerChannelMax && listed[3].message_id == 2011,
          "a short buffer receives the oldest entries and the full count is reported");
    keep_clear();

    // 500 overall: the oldest message of any channel goes first.
    for (u32 channel = 0; channel < 20; ++channel)
        for (u32 i = 0; i < kKeepPerChannelMax; ++i)
            keep_deleted({100 + channel, 10000 + channel * 100 + i, 5, 7, 0, {text, 10}, {}});
    stats = keep_stats();
    check(stats.messages == kKeepMessagesMax && stats.evicted == 20 * kKeepPerChannelMax - kKeepMessagesMax &&
              !keep_is_deleted(10000) && keep_is_deleted(10000 + 19 * 100 + 31),
          "five hundred messages in all, oldest first");
    keep_clear();

    // Edit history: eight earlier texts.
    const KeepMessage first{3, 3000, 5, 7, 0, {text, 10}, {"Ann", 3}};
    KeepMessage before = first;
    for (u32 i = 0; i < 11; ++i) {
        before.content = {text + i, 10 + i};
        keep_edited(before, {text + i + 1, 11 + i}, 100 + i);
    }
    KeepInfo info{};
    Utf8View version{};
    check(keep_info(3, 3000, &info) && info.edit_count == kKeepEditsMax && info.edited_ms == 110 && !info.deleted &&
              keep_text(3, 3000, 0, &version) && version.bytes == 21 && keep_text(3, 3000, 1, &version) &&
              version.bytes == 20 && keep_text(3, 3000, kKeepEditsMax, &version) && version.bytes == 13 &&
              !keep_text(3, 3000, kKeepEditsMax + 1, &version),
          "a message keeps its latest text and eight earlier ones");
    check(!keep_is_deleted(3000) && keep_tombstones(3, listed, 4) == 0, "an edited message is no tombstone");
    check(keep_deleted(first) && keep_is_deleted(3000) && keep_tombstones(3, listed, 4) == 1 &&
              listed[0].content.bytes == 21,
          "deleting it afterwards makes the tombstone show the latest text");
    check(!keep_info(4, 3000, &info) && !keep_text(4, 3000, 0, &version), "a message is looked up in its own channel");
    keep_forget(3000);
    check(!keep_is_deleted(3000) && keep_stats().messages == 0, "forgetting removes a message");
    keep_clear();

    // The text pool: big messages push the oldest out and survive compaction intact.
    u32 stored = 0;
    for (u32 i = 0; i < 300; ++i) {
        fill_text(text, kKeepContentMax, i);
        stored += keep_deleted({200 + i / 30, 50000 + i, 5, 7, 0, {text, kKeepContentMax}, {"Ann", 3}});
    }
    stats = keep_stats();
    check(stored == 300 && stats.messages < 300 && stats.evicted > 0 && keep_is_deleted(50299) && !keep_is_deleted(50000),
          "a full text pool drops the oldest messages");
    bool intact = true;
    for (u32 i = 299; i > 299 - 20; --i) {
        fill_text(text, kKeepContentMax, i);
        Utf8View kept{};
        intact = intact && keep_text(200 + i / 30, 50000 + i, 0, &kept) && kept.bytes == kKeepContentMax &&
                 bytes_equal(kept.data, text, kKeepContentMax);
    }
    check(intact, "the newest messages read back byte for byte after the pool was compacted");
    keep_clear();
    check(keep_stats().messages == 0 && keep_stats().evicted == 0, "clearing resets the store");
}

// The silent-toggle and translate samples through the real runtime: flags and a confirmation from pre_send.
void test_samples(PluginRuntime* runtime, const Root& root) noexcept {
    constexpr u64 kTranslateOwner = 0x6D63'7472'616E'736Cull;
    const u8 on = 1, off = 0;
    install(root, L"silent-toggle.dll", L"silent.dll");
    const PluginEnableResult toggle = plugin_runtime_enable(runtime, leaf(L"silent.dll"), MC_PERM_KNOWN);
    check(toggle.ok(), "the silent-toggle sample is enabled");
    if (toggle.ok()) {
        PluginBinding* binding = toggle.binding;
        check(draft("hello").flags == 0 && draft("hello").verdict == PluginDraftVerdict::keep, "silent mode starts off");
        check(plugin_binding_set_setting(binding, 0, &on, 1) == MC_OK, "the silent setting turns on");
        const PluginDraftResult silent = draft("hello");
        check(silent.verdict == PluginDraftVerdict::keep && silent.flags == MC_DRAFT_SILENT && !silent.text.bytes,
              "silent mode sends the message unchanged with the silent flag");
        check(draft("hello", PluginHook::pre_send, MC_DRAFT_SUPPRESS_EMBEDS).flags == (MC_DRAFT_SUPPRESS_EMBEDS | MC_DRAFT_SILENT),
              "the flag joins the ones already set");
        check(draft("edit", PluginHook::pre_edit).flags == 0, "an edit is not silenced");
        check(plugin_binding_set_setting(binding, 1, &on, 1) == MC_OK && draft("once").flags == MC_DRAFT_SILENT &&
                  draft("twice").flags == 0,
              "the one-shot setting turns silent mode off after a message");
        plugin_binding_set_setting(binding, 0, &off, 1);
        plugin_runtime_disable(runtime, binding);
    }
    install(root, L"translate.dll", L"translate.dll");
    const PluginEnableResult translate = plugin_runtime_enable(runtime, leaf(L"translate.dll"), MC_PERM_KNOWN, 7);
    check(translate.ok() && translate.binding->owner_id == kTranslateOwner, "the translate sample is enabled");
    if (translate.ok()) {
        PluginBinding* binding = translate.binding;
        check(draft("hello").verdict == PluginDraftVerdict::keep, "outgoing messages are sent without asking by default");
        check(plugin_binding_set_setting(binding, 1, &on, 1) == MC_OK, "the confirmation setting turns on");
        const PluginDraftResult asked = draft("hello");
        check(asked.verdict == PluginDraftVerdict::confirm && asked.owner_id == kTranslateOwner &&
                  asked.question.bytes > 10 && asked.question.bytes <= MC_CONFIRM_TEXT_MAX && !asked.text.bytes,
              "translate asks before an untranslated message goes out");
        check(draft("hello", PluginHook::pre_edit).verdict == PluginDraftVerdict::keep, "an edit is not questioned");
        plugin_binding_set_setting(binding, 1, &off, 1);
        plugin_runtime_disable(runtime, binding);
    }
}

void run() noexcept {
    Root root;
    if (!make_root(&root)) {
        check(false, "temporary directory");
        return;
    }
    static PluginRuntime runtime;
    static PluginCallLedger ledger;
    PluginCallMarker leftover{};
    wchar_t ledger_path[MAX_PATH];
    join(ledger_path, root.base, L"ledger.bin");
    const bool ledger_ready = plugin_call_ledger_open(&ledger, ledger_path, &leftover) && !leftover.present;
    const PluginLoadResult opened = plugin_runtime_open(&runtime, {root.plugins, length(root.plugins)},
                                                        {root.data, length(root.data)}, &ledger);
    check(ledger_ready && opened.ok(), "the plugins directory is pinned");
    if (opened.ok()) {
        test_no_plugins();
        test_draft_matrix(&runtime, root);
        test_hooks(&runtime, root);
        test_actions(&runtime, root);
        test_samples(&runtime, root);
        test_keep_store();
        check(runtime.binding_count == 0, "every test left its plugin disabled");
        check(plugin_runtime_close(&runtime).ok(), "closing the runtime unpins the directory");
    }
    plugin_call_ledger_close(&ledger);
    remove_tree(root.base);
}

} // namespace maicord::plugin_pipeline_tests

extern "C" __declspec(noreturn) void WINAPI maicord_plugin_pipeline_tests_entry() noexcept {
    maicord::plugin_pipeline_tests::run();
    ExitProcess(maicord::plugin_pipeline_tests::failures ? 1 : 0);
}
