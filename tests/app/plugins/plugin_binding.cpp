#include "maicord/app/plugins/binding.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "plugins/binding_internal.hpp"
#include "../../support/test_output.hpp"

#include "maicord/core/win32_error.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_process_launch.hpp"

// The plugin loader, bindings and hook dispatch against real DLLs: the ABI-6 fixtures
// (tests/fixtures/plugin_runtime_fixture.c), the clear-urls sample and the C fixture.
namespace maicord::plugin_binding_tests {
using namespace plugin_runtime_detail;

u32 failures;
void check(bool value, const char* label) noexcept {
    if (value) return;
    ++failures;
    test_write_text("FAIL plugin_binding: ");
    test_write_line(label);
}

constexpr wchar_t kChildVariable[] = L"MAICORD_PLUGIN_BINDING_CHILD";
constexpr u64 kFullOwner = 0x4D43525446554C4Cull, kSlowOwner = 0x4D4352545357464Cull,
              kCrashOwner = 0x4D43525443525348ull, kClearUrlsOwner = 0x6D63'636C'7572'6C73ull,
              kCOwner = 0x6D63635F61626943ull;

// What the full fixture records and reads; the layouts of tests/fixtures/plugin_runtime_fixture.c.
struct FixtureState {
    u32 starts, stops, sends, start_flags;
    i32 toast_status, setting_status, store_set_status, store_get_status, store_missing_status, store_truncated_status;
    u32 label_before, label_during, store_bytes, count;
};
struct FixtureSettings {
    u8 enabled;
    u8 reserved[3];
    i32 limit;
    mc_str label;
    mc_list words;
    u32 mode;
    u32 reserved2;
};
struct ClearUrlsSettings {
    u8 on_edit;
    u8 reserved[7];
    mc_list extra;
};

struct Toasts {
    u32 count;
    u64 owner;
    u32 tone;
    char text[64];
    u32 bytes;
};
void collect_toast(void* context, u64 owner, Utf8View text, u32 tone) noexcept {
    auto* toasts = static_cast<Toasts*>(context);
    ++toasts->count;
    toasts->owner = owner;
    toasts->tone = tone;
    toasts->bytes = text.bytes < sizeof(toasts->text) ? text.bytes : 0;
    copy_bytes(toasts->text, text.data, toasts->bytes);
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
    u32 count = 0;
    while (expected[count]) ++count;
    return text.bytes == count && bytes_equal(text.data, expected, count);
}
bool same(mc_str text, const char* expected) noexcept {
    return same(Utf8View{text.data, text.bytes}, expected);
}
void join(wchar_t (&output)[MAX_PATH], const wchar_t* directory, const wchar_t* name) noexcept {
    u32 at = 0;
    for (; directory[at]; ++at) output[at] = directory[at];
    output[at++] = L'\\';
    for (u32 i = 0; name[i]; ++i) output[at++] = name[i];
    output[at] = 0;
}
bool exists(const wchar_t* path) noexcept {
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

struct Root {
    wchar_t base[MAX_PATH], plugins[MAX_PATH], data[MAX_PATH], marker[MAX_PATH];
};
bool make_paths(Root* root, const wchar_t* base) noexcept {
    u32 at = 0;
    for (; base[at] && at < MAX_PATH - 1; ++at) root->base[at] = base[at];
    root->base[at] = 0;
    join(root->plugins, root->base, L"plugins");
    join(root->data, root->base, L"data");
    join(root->marker, root->base, L"marker.bin");
    return true;
}
bool make_root(Root* root) noexcept {
    wchar_t temp[MAX_PATH], path[MAX_PATH];
    const DWORD count = GetTempPathW(MAX_PATH, temp);
    if (!count || count >= MAX_PATH || !GetTempFileNameW(temp, L"mcb", 0, path) || !DeleteFileW(path) ||
        !CreateDirectoryW(path, nullptr))
        return false;
    make_paths(root, path);
    return CreateDirectoryW(root->plugins, nullptr) && CreateDirectoryW(root->data, nullptr);
}
void remove_tree(const wchar_t* directory) noexcept {
    wchar_t pattern[MAX_PATH];
    join(pattern, directory, L"*");
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW(pattern, &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if ((found.cFileName[0] == L'.' && (!found.cFileName[1] || (found.cFileName[1] == L'.' && !found.cFileName[2]))))
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

bool test_directory(wchar_t (&output)[MAX_PATH]) noexcept {
    const DWORD count = GetModuleFileNameW(nullptr, output, MAX_PATH);
    if (!count || count >= MAX_PATH) return false;
    u32 at = count;
    while (at && output[at - 1] != L'\\') --at;
    if (!at) return false;
    output[at - 1] = 0;
    return true;
}
// Copies a built fixture next to the pinned plugins directory's other DLLs under a short name.
bool install(const Root& root, const wchar_t* built, const wchar_t* name) noexcept {
    wchar_t directory[MAX_PATH], source[MAX_PATH], destination[MAX_PATH];
    if (!test_directory(directory)) return false;
    join(source, directory, built);
    join(destination, root.plugins, name);
    return CopyFileW(source, destination, FALSE) != 0;
}
// Overwrites bytes of an installed copy.
bool patch(const Root& root, const wchar_t* name, u64 offset, const void* bytes, u32 count) noexcept {
    wchar_t path[MAX_PATH];
    join(path, root.plugins, name);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    Win32LargeInteger position{static_cast<LONGLONG>(offset)};
    DWORD written = 0;
    const bool ok = SetFilePointerEx(file, *position.native(), nullptr, FILE_BEGIN) &&
                    WriteFile(file, bytes, count, &written, nullptr) && written == count;
    CloseHandle(file);
    return ok;
}
bool read_head(const Root& root, const wchar_t* name, u8* output, u32 count) noexcept {
    wchar_t path[MAX_PATH];
    join(path, root.plugins, name);
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD read = 0;
    const bool ok = ReadFile(file, output, count, &read, nullptr) && read == count;
    CloseHandle(file);
    return ok;
}
bool write_file(const Root& root, const wchar_t* name, const u8* bytes, u32 count) noexcept {
    wchar_t path[MAX_PATH];
    join(path, root.plugins, name);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = !count || (WriteFile(file, bytes, count, &written, nullptr) && written == count);
    CloseHandle(file);
    return ok;
}
bool loaded(const wchar_t* name) noexcept {
    return GetModuleHandleW(name) != nullptr;
}

PluginDraftResult send(const char* text, PluginHook hook = PluginHook::pre_send) noexcept {
    u32 bytes = 0;
    while (text[bytes]) ++bytes;
    const PluginDraft draft{77, 88, hook == PluginHook::pre_edit ? 99u : 0u, 0, {text, bytes}};
    return plugin_dispatch_draft(hook, draft);
}
PluginDraftResult edit(const char* text) noexcept {
    return send(text, PluginHook::pre_edit);
}

PluginEnableResult enable(PluginRuntime* runtime, const wchar_t* name, u64 approved = MC_PERM_MESSAGE_SEND) noexcept {
    return plugin_runtime_enable(runtime, leaf(name), approved);
}

// ---- Tests -------------------------------------------------------------------------

// With nothing enabled a hook is one branch: no call, no allocation.
void test_no_plugins() noexcept {
    const PluginDispatchStats before = plugin_dispatch_stats();
    const PluginDraftResult sent = send("hello"), edited = edit("hello");
    check(!plugin_hook_subscribed(PluginHook::pre_send) && !plugin_hook_subscribed(PluginHook::pre_edit),
          "nothing is subscribed before a plugin is enabled");
    check(sent.verdict == PluginDraftVerdict::keep && edited.verdict == PluginDraftVerdict::keep &&
              plugin_dispatch_stats().calls == before.calls &&
              plugin_dispatch_stats().allocations == before.allocations,
          "dispatching with no plugin calls nothing and allocates nothing");
}

// Every entry plugin_abi.h declares is implemented: the table is complete and has no null slot.
void test_api_table() noexcept {
    const auto* entries = reinterpret_cast<void* const*>(reinterpret_cast<const u8*>(&kApi) + 8);
    const u32 count = (sizeof(mc_api) - 8) / sizeof(void*);
    u32 missing = 0;
    for (u32 i = 0; i < count; ++i)
        missing += entries[i] == nullptr;
    check(kApi.size == sizeof(mc_api) && kApi.level == MC_API_LEVEL && missing == 0,
          "the host table implements every mc_api entry");
}

void test_loader_failures(PluginRuntime* runtime, const Root& root) noexcept {
    struct Case {
        const wchar_t* built;
        PluginLoadError error;
        const char* label;
    };
    const Case cases[]{
        {L"MaiCordPluginRuntimeEntryFixture.dll", PluginLoadError::entry_point, "a DLL with an entry point is refused"},
        {L"MaiCordPluginRuntimeTlsFixture.dll", PluginLoadError::tls, "a DLL with a TLS directory is refused"},
        {L"MaiCordPluginRuntimeExtraFixture.dll", PluginLoadError::exports, "a DLL with a second export is refused"},
        {L"MaiCordPluginRuntimeBadFixture.dll", PluginLoadError::descriptor, "an invalid descriptor is refused"},
        {L"MaiCordPluginRuntimeLevelFixture.dll", PluginLoadError::api_level, "a plugin needing a newer api level is refused"},
    };
    for (const Case& test : cases) {
        install(root, test.built, L"candidate.dll");
        PluginImage image{};
        const PluginLoadResult result = plugin_image_load(&image, runtime->directory, leaf(L"candidate.dll"),
                                                          MC_PERM_MESSAGE_SEND);
        check(result.error == test.error && !image.module && !loaded(L"candidate.dll"), test.label);
        if (test.error == PluginLoadError::descriptor)
            check(result.problem == PluginDescriptorProblem::text, "the invalid descriptor names the rule it broke");
        wchar_t path[MAX_PATH];
        join(path, root.plugins, L"candidate.dll");
        DeleteFileW(path);
    }
    PluginImage image{};
    check(plugin_image_load(&image, runtime->directory, leaf(L"missing.dll"), MC_PERM_MESSAGE_SEND).error ==
              PluginLoadError::open,
          "a missing file is an open failure");

    // Damaged copies of a good DLL.
    install(root, L"MaiCordPluginRuntimeFullFixture.dll", L"damaged.dll");
    u8 head[1024];
    read_head(root, L"damaged.dll", head, sizeof(head));
    const u32 nt = byte_load_le32(head + 0x3C);
    const u32 table = nt + 24 + byte_load_le16(head + nt + 20);
    const u16 other_machine = 0x14c;
    patch(root, L"damaged.dll", nt + 4, &other_machine, 2);
    check(plugin_image_load(&image, runtime->directory, leaf(L"damaged.dll"), MC_PERM_MESSAGE_SEND).error ==
              PluginLoadError::machine,
          "a non-x64 image is refused");
    patch(root, L"damaged.dll", nt + 4, head + nt + 4, 2);
    const u32 writable_code = byte_load_le32(head + table + 36) | 0x80000000u;
    patch(root, L"damaged.dll", table + 36, &writable_code, 4);
    check(plugin_image_load(&image, runtime->directory, leaf(L"damaged.dll"), MC_PERM_MESSAGE_SEND).error ==
              PluginLoadError::sections,
          "a section that is writable and executable is refused");
    patch(root, L"damaged.dll", table + 36, head + table + 36, 4);
    const u8 junk[16]{'M', 'Z'};
    write_file(root, L"truncated.dll", head, 1024);
    write_file(root, L"junk.dll", junk, sizeof(junk));
    write_file(root, L"empty.dll", junk, 0);
    check(plugin_image_load(&image, runtime->directory, leaf(L"truncated.dll"), MC_PERM_MESSAGE_SEND).error ==
              PluginLoadError::sections,
          "a file cut off inside its sections is refused");
    check(plugin_image_load(&image, runtime->directory, leaf(L"junk.dll"), MC_PERM_MESSAGE_SEND).error ==
              PluginLoadError::not_pe,
          "a file that is not a PE image is refused");
    check(plugin_image_load(&image, runtime->directory, leaf(L"empty.dll"), MC_PERM_MESSAGE_SEND).error ==
              PluginLoadError::open,
          "an empty file is refused");
    check(!loaded(L"damaged.dll") && !image.module, "no refused file was loaded");

    // The descriptor loads, but the user has not approved what its hooks need.
    install(root, L"MaiCordPluginRuntimeFullFixture.dll", L"full.dll");
    check(plugin_image_load(&image, runtime->directory, leaf(L"full.dll"), 0).error == PluginLoadError::permissions &&
              !loaded(L"full.dll"),
          "missing approval for a hook's permission refuses the plugin");
    const PluginEnableResult denied = enable(runtime, L"full.dll", 0);
    check(denied.error == PluginEnableError::load && denied.load.error == PluginLoadError::permissions &&
              !runtime->binding_count && !loaded(L"full.dll"),
          "enable reports the refusal and leaves nothing behind");
    check(plugin_image_load(&image, runtime->directory, leaf(L"full.dll"), MC_PERM_MESSAGE_SEND).ok() &&
              image.owner_id == kFullOwner && image.permissions == MC_PERM_MESSAGE_SEND &&
              same(Utf8View{image.info.name, image.info.name_bytes}, "Runtime fixture") &&
              same(Utf8View{image.info.version, image.info.version_bytes}, "1.0.0") && image.executable_count >= 1,
          "a good DLL is mapped and its descriptor text is kept");
    check(plugin_image_unload(&image).ok() && !image.module && !loaded(L"full.dll"), "unload frees the DLL");
}

void test_full_plugin(PluginRuntime* runtime, const Root& root, Toasts* toasts) noexcept {
    const PluginEnableResult enabled = enable(runtime, L"full.dll");
    check(enabled.ok() && enabled.binding && runtime->binding_count == 1, "a valid plugin is enabled");
    if (!enabled.ok()) return;
    PluginBinding* binding = enabled.binding;
    const auto* state = static_cast<const FixtureState*>(binding->ctx.state);
    const auto* settings = static_cast<const FixtureSettings*>(binding->ctx.settings);
    check(state->starts == 1 && state->start_flags == MC_CTX_FIRST_START && binding->started &&
              binding->ctx.call == 0 && binding->ctx.api->level == 1 && binding->ctx.api->size == sizeof(mc_api),
          "start ran once with a zeroed state and the first-start flag");
    check(settings->enabled == 1 && settings->limit == 10 && same(settings->label, "hello") &&
              settings->words.count == 2 && same(settings->words.items[0], "a") && same(settings->words.items[1], "b") &&
              settings->mode == 2,
          "the settings block holds each setting's default");
    check(binding->permissions == MC_PERM_MESSAGE_SEND && plugin_hook_subscribed(PluginHook::pre_send) &&
              plugin_hook_subscribed(PluginHook::pre_edit),
          "the hooks are published and imply their permission");

    // Verdicts.
    const u32 calls = plugin_dispatch_stats().calls;
    const PluginDraftResult cancelled = send("cancel");
    check(cancelled.verdict == PluginDraftVerdict::cancel && cancelled.owner_id == kFullOwner, "cancel names its plugin");
    const PluginDraftResult replaced = send("replace");
    check(replaced.verdict == PluginDraftVerdict::replace && same(replaced.text, "replaced"), "replace returns the new text");
    check(send("invalid").verdict == PluginDraftVerdict::keep, "an invalid UTF-8 replacement is ignored");
    check(send("fail").verdict == PluginDraftVerdict::keep, "output from a call that failed is ignored");
    check(send("hello").verdict == PluginDraftVerdict::keep, "a plugin that changes nothing keeps the text");
    const PluginDraftResult edited = edit("abc");
    check(edited.verdict == PluginDraftVerdict::replace && same(edited.text, "abc!"), "pre_edit runs its own hook");
    check(plugin_dispatch_stats().calls == calls + 6 && state->sends == 5, "each hook call is counted once");
    char large[4100];
    for (char& c : large) c = 'a';
    const PluginDraft too_long{1, 0, 0, 0, {large, sizeof(large)}};
    check(plugin_dispatch_draft(PluginHook::pre_send, too_long).verdict == PluginDraftVerdict::keep &&
              plugin_dispatch_stats().calls == calls + 6,
          "text beyond the draft limit is not offered to plugins");

    // Host table.
    check(send("toast").verdict == PluginDraftVerdict::keep && state->toast_status == MC_E_UNSUPPORTED,
          "a toast fails without a sink");
    runtime->toast = collect_toast;
    runtime->sink_context = toasts;
    send("toast");
    check(state->toast_status == MC_OK && toasts->count == 1 && same(Utf8View{toasts->text, toasts->bytes}, "hello") &&
              toasts->owner == kFullOwner && toasts->tone == MC_TONE_ACCENT,
          "a toast reaches the sink with its owner and tone");
    send("setting");
    check(state->setting_status == MC_OK && state->label_before == 5 && state->label_during == 5 &&
              same(settings->label, "new label") && binding->settings_dirty,
          "a setting changed during a call applies when the call returns");
    send("store");
    check(state->store_set_status == MC_OK && state->store_get_status == MC_OK && state->store_bytes == 5 &&
              state->store_missing_status == MC_E_NOT_FOUND && state->store_truncated_status == MC_E_TRUNCATED,
          "the store sets, gets, reports missing keys and refuses to truncate");
    for (u32 i = 0; i < 3; ++i) send("count");
    check(state->count == 3, "store values accumulate across calls");

    // Host-side setting changes validate like the plugin's.
    const i32 fifty = 50, over = 101;
    const u8 two = 2;
    const u32 choice_outside = 3, choice_first = 0;
    check(plugin_binding_set_setting(binding, 1, &over, 4) == MC_E_INVALID &&
              plugin_binding_set_setting(binding, 1, &fifty, 4) == MC_OK && settings->limit == 50 &&
              plugin_binding_set_setting(binding, 0, &two, 1) == MC_E_INVALID &&
              plugin_binding_set_setting(binding, 99, &two, 1) == MC_E_INVALID &&
              plugin_binding_set_setting(binding, 1, &fifty, 3) == MC_E_INVALID,
          "integer and toggle settings are range-checked");
    check(plugin_binding_set_setting(binding, 2, "0123456789abcdefg", 17) == MC_E_CAPACITY &&
              plugin_binding_set_setting(binding, 2, "a\0b", 3) == MC_E_INVALID &&
              plugin_binding_set_setting(binding, 2, "\xff", 1) == MC_E_INVALID &&
              plugin_binding_set_setting(binding, 2, "", 0) == MC_OK && settings->label.bytes == 0 &&
              plugin_binding_set_setting(binding, 2, "again", 5) == MC_OK && same(settings->label, "again"),
          "text settings are length- and UTF-8-checked");
    check(plugin_binding_set_setting(binding, 3, "x\n\ny\n", 5) == MC_OK && settings->words.count == 2 &&
              same(settings->words.items[0], "x") && same(settings->words.items[1], "y") &&
              plugin_binding_set_setting(binding, 3, "0123456789", 10) == MC_E_CAPACITY &&
              plugin_binding_set_setting(binding, 4, &choice_outside, 4) == MC_E_INVALID &&
              plugin_binding_set_setting(binding, 4, &choice_first, 4) == MC_OK && settings->mode == 0,
          "list and choice settings keep non-empty items and valid indexes");
    check(plugin_binding_set_setting(binding, 2, "new label", 9) == MC_OK, "the label is restored for the persistence check");

    // Changes reach disk after the delay, not before.
    wchar_t store_file[MAX_PATH], settings_file[MAX_PATH], owner_directory[MAX_PATH];
    wchar_t owner_name[17];
    for (u32 i = 0; i < 16; ++i) owner_name[i] = L"0123456789abcdef"[(kFullOwner >> (60 - 4 * i)) & 15];
    owner_name[16] = 0;
    join(owner_directory, root.data, owner_name);
    join(store_file, owner_directory, L"store.bin");
    join(settings_file, owner_directory, L"settings.bin");
    const u64 now = clock_monotonic_milliseconds();
    check(plugin_runtime_poll(runtime, now) == 0 && !exists(store_file) && !exists(settings_file),
          "changes wait out the flush delay");
    check(plugin_runtime_poll(runtime, now + kPluginFlushDelayMilliseconds + 500) == 0 && exists(store_file) &&
              exists(settings_file) && !binding->settings_dirty && !binding->store.dirty,
          "changes older than the delay are written");

    // A disable runs stop and keeps what the plugin stored and what was set.
    check(plugin_runtime_disable(runtime, binding) && !runtime->binding_count && !loaded(L"full.dll") &&
              !plugin_hook_subscribed(PluginHook::pre_send) && !plugin_hook_subscribed(PluginHook::pre_edit),
          "disable unpublishes the hooks and frees the DLL");
    check(send("replace").verdict == PluginDraftVerdict::keep, "a disabled plugin is no longer called");

    const PluginEnableResult again = enable(runtime, L"full.dll");
    check(again.ok(), "the plugin can be enabled again");
    if (!again.ok()) return;
    binding = again.binding;
    state = static_cast<const FixtureState*>(binding->ctx.state);
    settings = static_cast<const FixtureSettings*>(binding->ctx.settings);
    check(state->starts == 1 && settings->limit == 50 && same(settings->label, "new label") && settings->mode == 0 &&
              settings->words.count == 2 && same(settings->words.items[0], "x"),
          "persisted settings are applied at the next enable");
    u32 stops = 0, bytes = 0;
    const PluginKeyValue* store = store_table(binding);
    check(store && kv_get(*store, {"stops", 5, 0}, &stops, 4, &bytes) == MC_OK && stops == 1 && bytes == 4,
          "stop ran before the plugin was unloaded");
    send("count");
    check(state->count == 4, "the persisted store is read at the next enable");
    check(plugin_runtime_disable(runtime, binding), "the second disable succeeds");
}

void test_start_failure_and_duplicates(PluginRuntime* runtime, const Root& root) noexcept {
    install(root, L"MaiCordPluginRuntimeFailStartFixture.dll", L"failstart.dll");
    const u32 allocations = plugin_dispatch_stats().allocations;
    const PluginEnableResult failed = enable(runtime, L"failstart.dll");
    check(failed.error == PluginEnableError::start_failed && !failed.binding && !runtime->binding_count &&
              !loaded(L"failstart.dll") && !plugin_hook_subscribed(PluginHook::pre_send) &&
              plugin_dispatch_stats().allocations == allocations,
          "a start that fails leaves nothing enabled");
    install(root, L"MaiCordPluginRuntimeFullFixture.dll", L"full2.dll");
    const PluginEnableResult first = enable(runtime, L"full.dll");
    const PluginEnableResult second = enable(runtime, L"full2.dll");
    check(first.ok() && second.error == PluginEnableError::duplicate_owner && runtime->binding_count == 1 &&
              !loaded(L"full2.dll"),
          "a second plugin with the same owner id is refused");
    check(first.ok() && plugin_runtime_disable(runtime, first.binding), "the first plugin is disabled");
}

void test_budget(PluginRuntime* runtime, PluginCallLedger* ledger, const Root& root) noexcept {
    install(root, L"MaiCordPluginRuntimeSlowFixture.dll", L"slow.dll");
    ledger->pre_budget_us = 1000;
    const PluginEnableResult enabled = enable(runtime, L"slow.dll");
    check(enabled.ok(), "the slow plugin is enabled");
    if (!enabled.ok()) {
        ledger->pre_budget_us = kPluginCallPreBudgetUs;
        return;
    }
    PluginBinding* binding = enabled.binding;
    const u32 strikes = plugin_dispatch_stats().strikes;
    check(send("hi").verdict == PluginDraftVerdict::keep && binding->strikes == 1 &&
              plugin_dispatch_stats().strikes == strikes + 1 && !binding->struck_out,
          "an overrun leaves the draft unchanged and costs a strike");
    send("hi");
    send("hi");
    check(binding->strikes == 3 && binding->struck_out && plugin_call_ledger_disable_requested(ledger, kSlowOwner),
          "the third strike flags the plugin");
    const u32 calls = plugin_dispatch_stats().calls;
    check(send("hi").verdict == PluginDraftVerdict::keep && plugin_dispatch_stats().calls == calls,
          "a flagged plugin is skipped at once");
    check(plugin_runtime_poll(runtime, clock_monotonic_milliseconds()) == kSlowOwner && !runtime->binding_count &&
              !plugin_hook_subscribed(PluginHook::pre_send) && !loaded(L"slow.dll"),
          "the next poll disables a plugin that kept overrunning");
    check(!plugin_call_ledger_disable_requested(ledger, kSlowOwner), "its accounting starts fresh");
    ledger->pre_budget_us = kPluginCallPreBudgetUs;
    check(plugin_runtime_poll(runtime, clock_monotonic_milliseconds()) == 0, "nothing else is flagged");
}

void test_ledger_classes(const Root& root) noexcept {
    PluginCallLedger ledger{};
    PluginCallMarker leftover{};
    check(plugin_call_ledger_open(&ledger, nullptr, &leftover) && ledger.decorate_budget_us == 1000 &&
              ledger.pre_budget_us == 10'000 && ledger.event_budget_us == 10'000 && ledger.budget_us == 50'000 &&
              ledger.lifecycle_budget_us == 500'000,
          "every call class starts with its documented budget");
    auto ticket = plugin_call_begin(&ledger, 7, PluginCallKind::decorate, PluginCallClass::decorate);
    Sleep(8);
    PluginCallOutcome outcome = plugin_call_finish(&ledger, ticket);
    check(outcome.overran && !outcome.disable && outcome.elapsed_us >= 1000, "a decorate call past 1 ms is a strike");
    ticket = plugin_call_begin(&ledger, 8, PluginCallKind::decorate);
    Sleep(8);
    outcome = plugin_call_finish(&ledger, ticket);
    check(!outcome.overran, "a legacy call keeps the gesture budget");
    for (u32 i = 0; i < 2; ++i) {
        ticket = plugin_call_begin(&ledger, 7, PluginCallKind::decorate, PluginCallClass::decorate);
        Sleep(8);
        outcome = plugin_call_finish(&ledger, ticket);
    }
    check(outcome.overran && outcome.disable && plugin_call_ledger_disable_requested(&ledger, 7),
          "three strikes request a disable");

    // The watchdog flags a call that outlasts its limit in the marker, so a killed process is attributed.
    PluginCallLedger watched{};
    wchar_t copy[MAX_PATH];
    join(copy, root.base, L"marker-copy.bin");
    check(plugin_call_ledger_open(&watched, root.marker, &leftover) && plugin_call_watchdog_start(&watched, 40, 10),
          "the watchdog starts on an open marker");
    ticket = plugin_call_begin(&watched, 0x77, PluginCallKind::pre_send, PluginCallClass::pre);
    Sleep(250);
    const bool flagged = watched.view[21] == 1;
    u8 record[32];
    HANDLE marker = CreateFileW(root.marker, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD count = 0;
    const bool read = marker != INVALID_HANDLE_VALUE && ReadFile(marker, record, sizeof(record), &count, nullptr) &&
                      count == sizeof(record);
    if (marker != INVALID_HANDLE_VALUE) CloseHandle(marker);
    HANDLE out = CreateFileW(copy, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    const bool copied = read && out != INVALID_HANDLE_VALUE && WriteFile(out, record, sizeof(record), &written, nullptr);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    plugin_call_finish(&watched, ticket);
    check(flagged && watched.view[21] == 0, "the watchdog marks a long call, and the mark clears when it ends");
    PluginCallMarker hung{};
    check(copied && plugin_call_ledger_peek(copy, &hung) && hung.present && hung.hung && hung.owner_id == 0x77 &&
              hung.kind == PluginCallKind::pre_send,
          "a process killed during a long call is reported as hung with its owner");
    plugin_call_ledger_close(&watched);
}

void test_clear_urls(PluginRuntime* runtime, const Root& root) noexcept {
    install(root, L"clear-urls.dll", L"clear-urls.dll");
    const PluginEnableResult enabled = enable(runtime, L"clear-urls.dll");
    check(enabled.ok() && enabled.binding && enabled.binding->owner_id == kClearUrlsOwner, "clear-urls loads");
    if (!enabled.ok()) {
        test_write_metric("  enable error ", static_cast<u64>(enabled.error));
        test_write_metric("  load error ", static_cast<u64>(enabled.load.error));
        test_write_metric("  descriptor problem ", static_cast<u64>(enabled.load.problem));
        test_write_metric("  index ", enabled.load.index);
        test_write_metric("  win32 ", enabled.load.win32_error);
        return;
    }
    PluginBinding* binding = enabled.binding;
    const PluginDraftResult tracked = send("see https://example.com/a?utm_source=news&id=7 now");
    check(tracked.verdict == PluginDraftVerdict::replace && same(tracked.text, "see https://example.com/a?id=7 now"),
          "clear-urls removes tracking parameters before a message is sent");
    check(send("https://example.com/a?utm_medium=x&fbclid=y").verdict == PluginDraftVerdict::replace &&
              same(send("https://example.com/a?utm_medium=x&fbclid=y").text, "https://example.com/a"),
          "a link left with no parameters loses its query");
    check(send("https://example.com/a?id=7 and plain text").verdict == PluginDraftVerdict::keep &&
              send("no links here").verdict == PluginDraftVerdict::keep,
          "clear-urls leaves clean messages alone");
    const PluginDraftResult edited = edit("edited https://example.com/b?gclid=1&page=2");
    check(edited.verdict == PluginDraftVerdict::replace && same(edited.text, "edited https://example.com/b?page=2"),
          "clear-urls cleans an edit while its setting is on");
    const u8 off = 0;
    check(plugin_binding_set_setting(binding, 0, &off, 1) == MC_OK &&
              edit("edited https://example.com/b?gclid=1&page=2").verdict == PluginDraftVerdict::keep &&
              send("https://example.com/b?gclid=1&page=2").verdict == PluginDraftVerdict::replace,
          "turning its edit setting off stops edits only");
    check(send("https://x.com/p?si=abc&k=1").verdict == PluginDraftVerdict::keep &&
              plugin_binding_set_setting(binding, 1, "si\nfoo", 6) == MC_OK &&
              same(send("https://x.com/p?si=abc&k=1").text, "https://x.com/p?k=1"),
          "the extra-parameters list setting adds patterns");

    // A second plugin sees the first one's output.
    install(root, L"MaiCordPluginAbiCFixture.dll", L"abi-c.dll");
    const u32 allocations = plugin_dispatch_stats().allocations;
    // Its message decoration implies MC_PERM_READ_MESSAGES.
    const PluginEnableResult second = enable(runtime, L"abi-c.dll", MC_PERM_MESSAGE_SEND | MC_PERM_READ_MESSAGES);
    check(second.ok() && second.binding->owner_id == kCOwner && plugin_dispatch_stats().allocations == allocations,
          "a second plugin shares the dispatch buffers");
    if (second.ok()) {
        const PluginDraftResult chained = send("https://a.com/?utm_source=x");
        check(chained.verdict == PluginDraftVerdict::replace && same(chained.text, "[c] https://a.com/"),
              "hooks chain in enable order, each seeing the previous output");
        const PluginDraftResult stopped = send("cancel");
        check(stopped.verdict == PluginDraftVerdict::cancel && stopped.owner_id == kCOwner, "a later plugin can cancel");
        check(plugin_runtime_disable(runtime, second.binding), "the C plugin is disabled");
    }
    check(plugin_hook_subscribed(PluginHook::pre_send) && plugin_runtime_disable(runtime, binding) &&
              !plugin_hook_subscribed(PluginHook::pre_send) && !plugin_hook_subscribed(PluginHook::pre_edit) &&
              !loaded(L"clear-urls.dll"),
          "the last disable empties the hook lists");
}

// The child of the crash test: loads the crash fixture and runs its hook, which faults.
u32 run_crash_child(const wchar_t* base) noexcept {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    Root root;
    make_paths(&root, base);
    static PluginRuntime runtime;
    static PluginCallLedger ledger;
    PluginCallMarker leftover{};
    if (!plugin_call_ledger_open(&ledger, root.marker, &leftover) ||
        !plugin_runtime_open(&runtime, {root.plugins, length(root.plugins)}, {root.data, length(root.data)}, &ledger).ok())
        return 2;
    if (!enable(&runtime, L"crash.dll").ok()) return 3;
    send("boom");
    return 4;
}

// A plugin that faults inside a hook leaves its owner and call in the marker.
void test_crash(const Root& root) noexcept {
    install(root, L"MaiCordPluginRuntimeCrashFixture.dll", L"crash.dll");
    wchar_t executable[MAX_PATH], command[MAX_PATH + 4];
    const DWORD count = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    if (!count || count >= MAX_PATH) {
        check(false, "the test executable's path");
        return;
    }
    command[0] = L'"';
    for (u32 i = 0; i < count; ++i) command[i + 1] = executable[i];
    command[count + 1] = L'"';
    command[count + 2] = 0;
    SetEnvironmentVariableW(kChildVariable, root.base);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const bool started = CreateProcessW(executable, command, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                                        &process) != 0;
    SetEnvironmentVariableW(kChildVariable, nullptr);
    check(started, "the crash child starts");
    if (!started) return;
    const bool exited = WaitForSingleObject(process.hProcess, 30'000) == 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    check(exited, "the crash child ends");
    PluginCallMarker leftover{};
    check(plugin_call_ledger_peek(root.marker, &leftover) && leftover.present && leftover.owner_id == kCrashOwner &&
              leftover.kind == PluginCallKind::pre_send && !leftover.hung,
          "the call marker attributes the crash to the plugin and hook that faulted");
}

void run() noexcept {
    Root root;
    if (!make_root(&root)) {
        check(false, "temporary directory");
        return;
    }
    static PluginRuntime runtime;
    static PluginCallLedger ledger;
    static Toasts toasts;
    PluginCallMarker leftover{};
    wchar_t ledger_path[MAX_PATH];
    join(ledger_path, root.base, L"ledger.bin");
    const bool ledger_ready = plugin_call_ledger_open(&ledger, ledger_path, &leftover) && !leftover.present;
    const PluginLoadResult opened =
        plugin_runtime_open(&runtime, {root.plugins, length(root.plugins)}, {root.data, length(root.data)}, &ledger);
    check(ledger_ready && opened.ok(), "the plugins directory is pinned once");
    if (opened.ok()) {
        test_no_plugins();
        test_api_table();
        test_loader_failures(&runtime, root);
        test_full_plugin(&runtime, root, &toasts);
        test_start_failure_and_duplicates(&runtime, root);
        test_budget(&runtime, &ledger, root);
        test_clear_urls(&runtime, root);
        test_ledger_classes(root);
        check(runtime.binding_count == 0, "every test left its plugin disabled");
        check(plugin_runtime_close(&runtime).ok(), "closing the runtime unpins the directory");
        test_crash(root);
    }
    plugin_call_ledger_close(&ledger);
    remove_tree(root.base);
}

} // namespace maicord::plugin_binding_tests

extern "C" __declspec(noreturn) void WINAPI maicord_plugin_binding_tests_entry() noexcept {
    using namespace maicord;
    wchar_t base[MAX_PATH];
    const DWORD count = GetEnvironmentVariableW(plugin_binding_tests::kChildVariable, base, MAX_PATH);
    if (count && count < MAX_PATH) ExitProcess(plugin_binding_tests::run_crash_child(base));
    plugin_binding_tests::run();
    ExitProcess(plugin_binding_tests::failures ? 1 : 0);
}
