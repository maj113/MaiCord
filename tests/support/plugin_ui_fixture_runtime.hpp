#pragma once

#include "maicord/app/plugins/binding.hpp"
#include "maicord/app/plugins/ui.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/memory.hpp"

#include "maicord/core/win32_kernel.hpp"
#if !defined(_WIN32)
#include "../app/plugins/plugin_fixture.hpp"
#endif

// The ABI-6 UI fixture (tests/fixtures/plugin_ui_fixture.c) enabled through the real plugin runtime, for the
// tests of the application's side of plugin menus: a private temporary plugins directory, the DLL copied next
// to the test executable's build output, and the fixture's state block as it records what the plugin was asked.
// The fixture declares these menu items (index: label, targets): 0 Inspect (user, message), 1 More (message, link;
// a submenu), 2 Copy link (link), 3 Pin to top (message; checkbox), 4 Wipe (channel, server; danger),
// 5 Others only (user; hidden on your own account), 6 Greyed (image; disabled), 7 Media (emoji, image),
// 8 Broken (server; its handler fails).
namespace maicord::plugin_ui_fixture_runtime {

// The fixture's state block, as plugin_ui_fixture.c lays it out.
struct State {
    u32 menu_calls, menu_kind, menu_item, menu_flags, menu_gesture;
    u64 user_id, channel_id, guild_id, message_id, item_id;
    u32 attachment_index, width, height, url_bytes, text_bytes, type_bytes;
    char url[96], text[48], type[32];
    u32 checked, visible_calls;
};

constexpr u64 kOwner = 0x4D43525455494658ull;

struct Fixture {
    wchar_t base[MAX_PATH], plugins[MAX_PATH], data[MAX_PATH];
    PluginRuntime runtime;
    PluginBinding* binding;
    bool opened;

    static u32 length(const wchar_t* text) noexcept {
        u32 count = 0;
        while (text[count]) ++count;
        return count;
    }
    static void join(wchar_t (&output)[MAX_PATH], const wchar_t* directory, const wchar_t* name) noexcept {
        u32 at = 0;
        for (; directory[at]; ++at) output[at] = directory[at];
        output[at++] = kPathSeparator;
        for (u32 i = 0; name[i]; ++i) output[at++] = name[i];
        output[at] = 0;
    }
    State* state() const noexcept { return binding ? static_cast<State*>(binding->ctx.state) : nullptr; }

    // Enables a fixture DLL built next to the test (by default the UI fixture, whose menus are registered).
    bool enable(const wchar_t* built = L"MaiCordPluginUiFixture.dll") noexcept {
        wchar_t temp[MAX_PATH], module[MAX_PATH], source[MAX_PATH], destination[MAX_PATH];
        const DWORD temp_count = GetTempPathW(MAX_PATH, temp);
        if (!temp_count || temp_count >= MAX_PATH || !GetTempFileNameW(temp, L"mcf", 0, base) || !DeleteFileW(base) ||
            !CreateDirectoryW(base, nullptr))
            return false;
        join(plugins, base, L"plugins");
        join(data, base, L"data");
        if (!CreateDirectoryW(plugins, nullptr) || !CreateDirectoryW(data, nullptr)) return false;
#if defined(_WIN32)
        const DWORD count = GetModuleFileNameW(nullptr, module, MAX_PATH);
        u32 at = count;
        while (at && module[at - 1] != L'\\') --at;
        if (!count || count >= MAX_PATH || !at) return false;
        module[at - 1] = 0;
        join(source, module, built);
        join(destination, plugins, L"ui.dll");
        if (!CopyFileW(source, destination, FALSE)) return false;
#else
        (void)module;
        (void)source;
        (void)destination;
        plugin_fixture::Root root{};
        plugin_fixture::make_paths(&root, base);
        if (!plugin_fixture::install(root, built, L"ui.dll")) return false;
#endif
        const PluginLoadResult open_result =
            plugin_runtime_open(&runtime, {plugins, length(plugins)}, {data, length(data)}, nullptr);
        opened = open_result.ok();
        if (!opened) return false;
        const wchar_t leaf[] = L"ui.dll";
        const PluginEnableResult enabled = plugin_runtime_enable(&runtime, {leaf, length(leaf)}, MC_PERM_KNOWN);
        binding = enabled.ok() ? enabled.binding : nullptr;
        return binding != nullptr;
    }
    // Disables the plugin (its rows and commands go; the registry revision moves).
    bool disable() noexcept {
        const bool done = !binding || plugin_runtime_disable(&runtime, binding);
        binding = nullptr;
        return done;
    }
    // Enables the plugin again after disable(): a fresh binding with a zeroed state block.
    bool restart() noexcept {
        const wchar_t leaf[] = L"ui.dll";
        const PluginEnableResult enabled = plugin_runtime_enable(&runtime, {leaf, length(leaf)}, MC_PERM_KNOWN);
        binding = enabled.ok() ? enabled.binding : nullptr;
        return binding != nullptr;
    }
#if defined(_WIN32)
    void remove_tree(const wchar_t* directory) noexcept {
        wchar_t pattern[MAX_PATH];
        join(pattern, directory, L"*");
        WIN32_FIND_DATAW found;
        HANDLE find = FindFirstFileW(pattern, &found);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (found.cFileName[0] == L'.' &&
                    (!found.cFileName[1] || (found.cFileName[1] == L'.' && !found.cFileName[2])))
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
#else
    void remove_tree(const wchar_t* directory) noexcept {
        plugin_fixture::remove_tree(directory);
    }
#endif
    // Disables anything left, closes the runtime and deletes the private directory.
    void release() noexcept {
        disable();
        if (opened) plugin_runtime_close(&runtime);
        opened = false;
        if (base[0]) remove_tree(base);
        base[0] = 0;
    }
};

} // namespace maicord::plugin_ui_fixture_runtime
