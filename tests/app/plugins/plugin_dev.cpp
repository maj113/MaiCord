// Developer Mode: the watched dev folders, hot reload and the Developer console,
// against the catalog service with no server. Fixture builds of one plugin stand for a developer's rebuilds
// (same layout, changed layout, one more permission, a broken file); the plugin the maicord-plugin tool wrote
// from a clean folder (new, build, dev: cmake runs them) is loaded the way a user's would be, and the effect the
// tool built (effect new, preview) is read by the client's own .mcfx parser.
#include "plugins/catalog_service.hpp"
#include "plugins/dev_console.hpp"
#include "catalog_fixture.hpp"
#include "maicord/app/packages/mcfx.hpp"
#include "maicord/app/plugins/consent.hpp"
#include "maicord/app/plugins/dispatch.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"

namespace maicord::plugin_dev_tests {
using namespace app_detail;
using namespace catalog_fixture;

u32 failures;
void check(bool value, const char* label) noexcept {
    if (value) return;
    ++failures;
    test_write_text("FAIL plugin_dev: ");
    test_write_line(label);
}

// A sideloaded plugin is recorded as dev-<owner id as 16 hex digits>; the fixture's owner is 0x6D63'7265'6C6F'6164.
constexpr Utf8View kSlug = utf8_literal("dev-6d6372656c6f6164");

// The state block of the fixture plugin (tests/fixtures/plugin_reload_fixture.cpp).
struct FixtureState {
    u32 starts, started_as, calls;
};

// Host stubs.
bool g_developer;
u32 g_toasts, g_library_changes;
bool developer_mode() noexcept { return g_developer; }
void toast(Utf8View, bool) noexcept { ++g_toasts; }
bool validate_package(CatalogKind, ConstSlice<u8>, Utf8View) noexcept { return true; }
void library_changed() noexcept { ++g_library_changes; }
void package_removed(CatalogKind, Utf8View, bool) noexcept {}

McfxWorkspace g_workspace;

CatalogPhase phase() noexcept { return application_catalog_view().operation.phase; }
bool finished() noexcept { return phase() == CatalogPhase::done || phase() == CatalogPhase::failed; }
bool consent_or_finished() noexcept { return phase() == CatalogPhase::consent || finished(); }
bool asks() noexcept { return phase() == CatalogPhase::consent; }

// Polls until done says so (about ten seconds at most).
template <typename F> bool settle(F&& done) noexcept {
    for (u32 i = 0; i < 1000; ++i) {
        application_catalog_poll();
        if (done()) return true;
        Sleep(10);
    }
    return false;
}

PluginBinding* running() noexcept {
    PluginRuntime* r = application_catalog_runtime();
    return r && r->binding_count == 1 ? r->bindings[0] : nullptr;
}
bool running_version(const char* version) noexcept {
    const PluginBinding* b = running();
    return b && utf8_equal({b->image.info.version, b->image.info.version_bytes}, {version, c_string_length(version)});
}
FixtureState* state_of(PluginBinding* b) noexcept { return b ? static_cast<FixtureState*>(b->ctx.state) : nullptr; }

// How many lines of the console begin with prefix (and, for plugin lines, which plugin wrote them).
bool console_has(DevLineKind kind, const char* prefix, const char* name = nullptr) noexcept {
    const u32 length = c_string_length(prefix);
    for (u32 i = 0; i < application_dev_console_count(); ++i) {
        const DevLine* line = application_dev_console_line(i);
        if (line && line->kind == kind && line->text_bytes >= length && bytes_equal(line->text, prefix, length) &&
            (!name || (line->name_bytes == c_string_length(name) && bytes_equal(line->name, name, line->name_bytes))))
            return true;
    }
    return false;
}
bool console_contains(const char* needle) noexcept {
    const u32 length = c_string_length(needle);
    for (u32 i = 0; i < application_dev_console_count(); ++i) {
        const DevLine* line = application_dev_console_line(i);
        for (u32 at = 0; line && at + length <= line->text_bytes; ++at)
            if (bytes_equal(line->text + at, needle, length)) return true;
    }
    return false;
}

bool strips_mark(const char* text, const char* expected) noexcept {
    const PluginDraftResult r = plugin_dispatch_draft(PluginHook::pre_send, {1, 0, 0, 0, {text, c_string_length(text)}});
    return expected ? r.verdict == PluginDraftVerdict::replace && utf8_equal(r.text, {expected, c_string_length(expected)})
                    : r.verdict == PluginDraftVerdict::keep;
}

u32 count_files(const wchar_t* base, const wchar_t* relative) noexcept {
    wchar_t pattern[MAX_PATH];
    wide_copy(pattern, wide_copy(pattern, wide_copy(pattern, 0, base), relative), L"\\*.dll");
    WIN32_FIND_DATAW found{};
    const HANDLE scan = FindFirstFileW(pattern, &found);
    u32 count = 0;
    if (scan == INVALID_HANDLE_VALUE) return 0;
    do count += (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0 : 1;
    while (FindNextFileW(scan, &found));
    FindClose(scan);
    return count;
}

// Writes the file over target; one_more_byte appends a zero byte after the image, which changes the file's hash
// and nothing the loader reads.
bool copy_over(const wchar_t* from, const wchar_t* to, bool one_more_byte = false) noexcept {
    u32 bytes = 0;
    u8* data = read_file(from, &bytes);
    if (!data) return false;
    u8* output = one_more_byte ? static_cast<u8*>(VirtualAlloc(nullptr, bytes + 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) : data;
    bool ok = output != nullptr;
    if (ok && one_more_byte) {
        copy_bytes(output, data, bytes);
        output[bytes] = 0;
    }
    ok = ok && write_file(to, output, bytes + (one_more_byte ? 1 : 0));
    if (one_more_byte) release(output);
    release(data);
    return ok;
}

// True when nothing changed (changed stayed false) over a short wait.
template <typename F> bool stays(F&& changed, u32 polls = 40) noexcept {
    for (u32 i = 0; i < polls; ++i) {
        application_catalog_poll();
        if (changed()) return false;
        Sleep(10);
    }
    return true;
}

void native(wchar_t* path) noexcept {
    for (; *path; ++path)
        if (*path == L'/') *path = L'\\';
}

bool initialize(const wchar_t* base) noexcept {
    CatalogServiceOptions options{};
    options.base = {base, static_cast<u32>(wide_length(base, MAX_PATH))};
    options.host = {developer_mode, toast, validate_package, library_changed, package_removed, nullptr, nullptr};
    return application_catalog_initialize(options);
}

// ---- the fixture rebuilds -----------------------------------------------------------------------------------
void run_reload(const wchar_t* directory, const wchar_t* base, const wchar_t* dev, const wchar_t* effects) noexcept {
    wchar_t a[MAX_PATH], b[MAX_PATH], c[MAX_PATH], d[MAX_PATH], target[MAX_PATH];
    wide_copy(a, wide_copy(a, 0, directory), L"\\reload_a.dll");
    wide_copy(b, wide_copy(b, 0, directory), L"\\reload_b.dll");
    wide_copy(c, wide_copy(c, 0, directory), L"\\reload_c.dll");
    wide_copy(d, wide_copy(d, 0, directory), L"\\reload_d.dll");
    wide_copy(target, wide_copy(target, 0, dev), L"\\reload.dll");

    g_developer = true;
    check(initialize(base), "the service starts");
    application_catalog_restore();
    application_catalog_poll();
    check(application_catalog_dev_notification(0) && application_catalog_dev_notification(1) && !application_catalog_runtime() &&
              application_dev_console_enabled() && application_dev_console_count() == 0 &&
              application_catalog_dev_folder(0).bytes && application_catalog_dev_folder(1).bytes,
          "Developer Mode watches both dev folders; nothing is loaded and nothing is logged");

    // A first build: the change notification fires, the sideload consent asks, accepting loads it.
    check(copy_over(a, target), "build 1 is written");
    check(WaitForSingleObject(application_catalog_dev_notification(0), 5000) == WAIT_OBJECT_0,
          "the dev folder's change notification is signalled (it is in the UI wait set)");
    check(settle(consent_or_finished) && phase() == CatalogPhase::consent, "a first build of an owner asks for consent");
    const CatalogConsent* consent = application_catalog_consent();
    check(consent && consent->sideload && !consent->update && utf8_equal(consent->request->name, utf8_literal("Reload Fixture")),
          "it is the sideload consent");
    check(application_catalog_consent_answer(true, true) && running_version("1.0.0"), "accepting loads the plugin");
    FixtureState* state = state_of(running());
    check(state && state->starts == 1 && state->started_as == 1, "it starts as a first start");
    check(console_has(DevLineKind::reload, "Loaded in ") && console_has(DevLineKind::info, "started", "Reload Fixture"),
          "the console shows the load and the plugin's own log line, named after the plugin");
    const auto entry = application_catalog_item_state(CatalogKind::plugin, kSlug);
    check(entry.installed && entry.sideloaded && entry.enabled && entry.running, "the build is recorded as a sideload");

    // The plugin works, and the user changes a setting.
    const u8 on = 1;
    check(plugin_binding_set_setting(running(), 0, &on, 1) == MC_OK, "the setting is on");
    check(strips_mark("mark", "marked") && strips_mark("other", nullptr), "the plugin's hook runs");
    check(state_of(running())->calls == 2, "the plugin counted its calls");

    // Build 2: the same layout. No question; the state, settings and store carry over.
    const u32 toasts = g_toasts;
    check(copy_over(b, target), "build 2 is written");
    check(settle([] { return phase() == CatalogPhase::consent || (running_version("1.0.1") && finished()); }) &&
              phase() == CatalogPhase::done && running_version("1.0.1"),
          "a rebuild within the approval replaces the running build without a question");
    state = state_of(running());
    check(state && state->starts == 2 && state->started_as == 2 && state->calls == 2, "the state was kept and the plugin told it was reloaded");
    check(strips_mark("mark", "marked"), "the setting survived the reload");
    check(console_has(DevLineKind::reload, "Reloaded in ") && console_contains(" ms. State kept."), "the console says the state was kept");
    check(count_files(base, L"\\plugins") == 1, "the replaced build's file is deleted");
    check(g_toasts == toasts, "a good reload shows no failure toast");

    // Build 3: a new layout. The state starts again and the console says why.
    check(copy_over(c, target), "build 3 is written");
    check(settle([] { return running_version("2.0.0") && finished(); }) && phase() == CatalogPhase::done, "a new layout reloads too");
    state = state_of(running());
    check(state && state->starts == 1 && state->started_as == 1 && state->calls == 0, "the state starts zeroed when its layout changed");
    check(console_contains(" State reset: its layout changed.") && strips_mark("mark", "marked"),
          "the console says why, and the settings are kept");

    // A budget strike reaches the console. A 1 us budget: the fixture's slow loop takes about 1 ms on a fast CPU,
    // too close to any budget measured in milliseconds.
    application_catalog_runtime()->ledger->pre_budget_us = 1;
    check(strips_mark("slow", nullptr) && console_has(DevLineKind::strike, "pre_send ran past its time budget", "Reload Fixture"),
          "a time-budget strike is logged");
    check(console_contains("Strike 1 of 3."), "with its count");
    application_catalog_runtime()->ledger->pre_budget_us = kPluginCallPreBudgetUs;

    // A file that is not a plugin leaves the running build alone and says what is wrong.
    u8 junk[256];
    for (u32 i = 0; i < sizeof(junk); ++i) junk[i] = static_cast<u8>('x');
    check(write_file(target, junk, sizeof(junk)), "a broken file is written");
    check(settle([] { return finished() && console_has(DevLineKind::problem, "Not reloaded: "); }) && phase() == CatalogPhase::failed &&
              running_version("2.0.0"),
          "a broken build is refused and the running one stays");
    check(console_contains("isn't an x64 DLL") && count_files(base, L"\\plugins") == 1, "the console says why, and the file is not kept");
    const u32 lines = application_dev_console_count();
    check(stays([lines] { return application_dev_console_count() != lines; }), "a refused build is not tried again until the file changes");

    // A build that asks for more permissions is a new consent; declining leaves the old build.
    check(copy_over(d, target), "build 4 is written");
    check(settle(asks), "more permissions ask again");
    consent = application_catalog_consent();
    check(consent && consent->sideload && consent->update && (consent->request->permissions & MC_PERM_CLIPBOARD) &&
              !(consent->request->approved_permissions & MC_PERM_CLIPBOARD),
          "the consent is for the added permission");
    check(!application_catalog_consent_answer(false, false) && phase() == CatalogPhase::failed && running_version("2.0.0") &&
              console_has(DevLineKind::problem, "Not reloaded: you declined it."),
          "declining keeps the running build and says so");
    const u32 after_decline = application_dev_console_count();
    check(stays([] { return phase() != CatalogPhase::failed; }) && application_dev_console_count() == after_decline,
          "a declined build is not asked again");
    check(copy_over(d, target, true) && settle(asks), "a changed file is asked again");
    check(application_catalog_consent_answer(true, true) && running_version("2.1.0") && state_of(running()) &&
              (application_catalog_item_state(CatalogKind::plugin, kSlug).installed->approved_permissions & MC_PERM_CLIPBOARD) != 0,
          "accepting runs it with the new approval");
    check(count_files(base, L"\\plugins") == 1, "only the running build's file is kept");

    // Reload (the Developer page's button) looks again; the build that already runs is left alone.
    const u32 before_reload = application_dev_console_count();
    const bool reload_asked = application_catalog_dev_reload();
    const bool reload_quiet = stays([&] { return phase() == CatalogPhase::consent || application_dev_console_count() != before_reload; });
    if (!reload_quiet)
        for (u32 i = before_reload > 2 ? before_reload - 2 : 0; i < application_dev_console_count(); ++i) {
            const DevLine* line = application_dev_console_line(i);
            char text[200]{};
            copy_bytes(text, line->text, line->text_bytes);
            test_write_line(text);
        }
    check(reload_asked && reload_quiet, "Reload scans the folder; the build that already runs is left alone");
    check(running_version("2.1.0") && phase() == CatalogPhase::done, "and it is still running");

    // The effects dev folder: a burst of writes settles into one call to the effect library.
    wchar_t effect_file[MAX_PATH];
    wide_copy(effect_file, wide_copy(effect_file, 0, effects), L"\\x.mcfx");
    const u32 library = g_library_changes;
    check(write_file(effect_file, "MCFX", 4) && write_file(effect_file, "MCFX-", 5), "an effect is written twice, quickly");
    check(settle([&] { return g_library_changes != library; }), "the effect library is told");
    check(stays([&] { return g_library_changes != library + 1; }), "once, after the folder settled");

    // Developer Mode off: nothing is watched, nothing is held.
    g_developer = false;
    application_catalog_poll();
    check(!application_catalog_dev_notification(0) && !application_catalog_dev_notification(1) && !application_dev_console_enabled() &&
              application_dev_console_count() == 0,
          "turning Developer Mode off closes the watches and frees the console");
    check(copy_over(a, target) && stays([] { return phase() == CatalogPhase::consent || running_version("1.0.0"); }),
          "a change while Developer Mode is off does nothing");
    g_developer = true;
    check(settle([] { return running_version("1.0.0") && finished(); }) && phase() == CatalogPhase::done &&
              application_dev_console_enabled() && application_catalog_dev_notification(0),
          "Developer Mode on again scans, and a build the user already allowed loads without a question");
    application_catalog_release();
    check(!application_dev_console_enabled(), "release frees the console");
}

// ---- what the tool made --------------------------------------------------------------------------------------
void run_tool_output(const wchar_t* base) noexcept {
#ifdef MAICORD_DX_FOLDER
    wchar_t plugin_dev[MAX_PATH], effect_dev[MAX_PATH], effect_file[MAX_PATH];
    const wchar_t* root = MAICORD_DX_FOLDER;
    wide_copy(plugin_dev, wide_copy(plugin_dev, 0, root), L"/plugin-dev");
    wide_copy(effect_dev, wide_copy(effect_dev, 0, root), L"/effect-dev");
    wide_copy(effect_file, wide_copy(effect_file, 0, effect_dev), L"/soft-rain.mcfx");
    native(plugin_dev);
    native(effect_dev);
    native(effect_file);
    wchar_t hello[MAX_PATH];
    wide_copy(hello, wide_copy(hello, 0, plugin_dev), L"\\hello.dll");
    check(exists(hello), "maicord-plugin new, build and dev left hello.dll in the dev folder");

    // A client whose dev folder is that folder: the plugin loads, asks once, and runs.
    SetEnvironmentVariableW(L"MAICORD_PLUGIN_DEV", plugin_dev);
    SetEnvironmentVariableW(L"MAICORD_EFFECT_DEV", effect_dev);
    wchar_t second[MAX_PATH];
    wide_copy(second, wide_copy(second, 0, base), L"-tool");
    CreateDirectoryW(second, nullptr);
    g_developer = true;
    check(initialize(second), "a client starts with the tool's dev folder");
    application_catalog_restore();
    check(settle(consent_or_finished) && phase() == CatalogPhase::consent, "the plugin the tool built asks for consent");
    const CatalogConsent* consent = application_catalog_consent();
    check(consent && consent->sideload && utf8_equal(consent->request->name, utf8_literal("Hello")) &&
              (consent->request->permissions & MC_PERM_MESSAGE_SEND),
          "its descriptor is what the template declares");
    check(application_catalog_consent_answer(true, true) && running() && utf8_equal({running()->image.info.name, running()->image.info.name_bytes}, utf8_literal("Hello")),
          "it runs in the client");
    check(strips_mark("hi", nullptr), "its hook leaves a message alone while Sign my messages is off");
    const u8 on = 1;
    check(plugin_binding_set_setting(running(), 1, &on, 1) == MC_OK && strips_mark("hi", "hi ~hello"),
          "and signs it once the user turns the setting on");
    application_catalog_release();

    // The effect the tool packed parses with the client's parser.
    u32 bytes = 0;
    u8* package = read_file(effect_file, &bytes);
    check(package && mcfx_parse({package, bytes}, &g_workspace).ok(), "the .mcfx the tool built passes the client's parser");
    if (package) {
        const McfxPackage& p = g_workspace.package;
        check(c_string_length(p.slug) == 9 && bytes_equal(p.slug, "soft-rain", 9) && p.knob_count == 2 && p.blend == EffectBlend::normal &&
                  c_string_length(p.name) == 9 && bytes_equal(p.name, "Soft Rain", 9),
              "with its slug, name, knobs and blend");
        release(package);
    }
    SetEnvironmentVariableW(L"MAICORD_PLUGIN_DEV", nullptr);
    SetEnvironmentVariableW(L"MAICORD_EFFECT_DEV", nullptr);
#else
    (void)base;
#endif
}

void run() noexcept {
    wchar_t directory[MAX_PATH], base[MAX_PATH], dev[MAX_PATH], effects[MAX_PATH];
    const DWORD temp = GetTempPathW(MAX_PATH - 60, base);
    check(test_directory(directory) && temp, "setup");
    if (!temp) return;
    u32 at = wide_copy(base, temp, L"maicord-plugin-dev-");
    u32 pid = GetCurrentProcessId();
    do base[at++] = static_cast<wchar_t>(L'0' + pid % 10), pid /= 10;
    while (pid);
    base[at] = 0;
    remove_tree(base);
    CreateDirectoryW(base, nullptr);
    wide_copy(dev, wide_copy(dev, 0, base), L"\\dev-plugins");
    wide_copy(effects, wide_copy(effects, 0, base), L"\\dev-effects");
    SetEnvironmentVariableW(L"MAICORD_PLUGIN_DEV", dev);
    SetEnvironmentVariableW(L"MAICORD_EFFECT_DEV", effects);
    if (!failures) run_reload(directory, base, dev, effects);
    SetEnvironmentVariableW(L"MAICORD_PLUGIN_DEV", nullptr);
    SetEnvironmentVariableW(L"MAICORD_EFFECT_DEV", nullptr);
    if (!failures) run_tool_output(base);
    remove_tree(base);
    wchar_t second[MAX_PATH];
    wide_copy(second, wide_copy(second, 0, base), L"-tool");
    remove_tree(second);
}

} // namespace maicord::plugin_dev_tests

extern "C" __declspec(noreturn) void WINAPI maicord_plugin_dev_tests_entry() noexcept {
    maicord::plugin_dev_tests::run();
    ExitProcess(maicord::plugin_dev_tests::failures ? 1 : 0);
}
