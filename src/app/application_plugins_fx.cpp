// Plugin background effects (mc.fx@1) on the host side: the saved choice,
// the picker rows after the built-ins, per-frame selection for the renderer,
// and the GPU guard's and device's verdicts. No plugin code runs here: the
// renderer builds a pixel shader from the runtime's validated bytecode copy.
#include "application_plugins.hpp"
#include "application_plugins_runtime.hpp"
#include "application_settings.hpp"
#include "application_state.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/log.hpp"
#include "maicord/render/background_fx.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord::app_detail {
namespace {

constexpr u32 kEffectRows = 16;
struct EffectRow {
    BackgroundFxChoice key;
    u32 name_bytes;
    char name[kPluginSdkEffectNameBytes];
};
struct EffectState {
    BackgroundFxChoice saved;
    bool loaded;
    u64 polled_revision, rows_signature;
    u32 count;
    EffectRow rows[kEffectRows];
    Utf8View choices[kBackgroundEffectCount + kEffectRows];
};
EffectState g_effects;

bool same(BackgroundFxChoice a, BackgroundFxChoice b) noexcept {
    return a.owner_id == b.owner_id && a.effect_id == b.effect_id;
}

// Below %LOCALAPPDATA%\MaiCord.
constexpr wchar_t kChoiceFile[] = L"plugin-data\\effect.bin";

void load() noexcept {
    if (g_effects.loaded)
        return;
    g_effects.loaded = true;
    wchar_t path[512];
    if (!data_path(path, 512, kChoiceFile).ok())
        return;
    const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    u8 bytes[kBackgroundFxChoiceBytes + 1]{};
    DWORD read = 0;
    const bool ok = ReadFile(file, bytes, sizeof(bytes), &read, nullptr) != FALSE;
    CloseHandle(file);
    if (ok)
        background_fx_choice_decode(bytes, read, &g_effects.saved);
}

// Written atomically; clearing the choice removes the file.
bool save(BackgroundFxChoice choice) noexcept {
    const bool valid = background_fx_choice_valid(choice);
    g_effects.saved = valid ? choice : BackgroundFxChoice{};
    wchar_t path[512];
    // Leaves the atomic write's spare characters; a save creates the directories.
    const DataPathResult built = data_path(path, 504, kChoiceFile, valid);
    if (!built.ok())
        return false;
    if (!valid)
        return DeleteFileW(path) || GetLastError() == ERROR_FILE_NOT_FOUND;
    u8 bytes[kBackgroundFxChoiceBytes];
    background_fx_choice_encode(choice, bytes);
    const auto written = file_persistence_write_atomic(path, built.length, {bytes, sizeof(bytes)});
    if (!written.ok())
        log_u64(LogLevel::warning, "plugins", "effect_choice_save_error", written.native_error);
    return written.ok();
}

// Registered effects ordered by name; returns the count and a signature of
// the set (ids and names, not parameters).
u32 list(u64* signature = nullptr) noexcept {
    g_effects.count = 0;
    u64 hash = 0xcbf29ce484222325ull;
    if (const auto* rt = application_plugins_runtime()) {
        NativePluginSdkEffectView views[kEffectRows]{};
        const u32 count = native_plugin_sdk_effects(*rt, {views, kEffectRows});
        for (u32 i = 0; i < count && i < kEffectRows; ++i) {
            EffectRow row{{views[i].owner_id, views[i].effect_id}, 0, {}};
            row.name_bytes = utf8_copy_truncated(row.name, views[i].name);
            u32 at = g_effects.count++;
            while (at && background_fx_choice_before({row.name, row.name_bytes}, row.key,
                                                     {g_effects.rows[at - 1].name, g_effects.rows[at - 1].name_bytes},
                                                     g_effects.rows[at - 1].key)) {
                g_effects.rows[at] = g_effects.rows[at - 1];
                --at;
            }
            g_effects.rows[at] = row;
        }
        for (u32 i = 0; i < g_effects.count; ++i) {
            const auto& row = g_effects.rows[i];
            hash = (hash ^ row.key.owner_id) * 0x100000001b3ull;
            hash = (hash ^ row.key.effect_id) * 0x100000001b3ull;
            for (u32 b = 0; b < row.name_bytes; ++b)
                hash = (hash ^ static_cast<u8>(row.name[b])) * 0x100000001b3ull;
        }
    }
    if (signature)
        *signature = hash;
    return g_effects.count;
}

// Turns a plugin effect off for good this session (it is unregistered; the
// plugin stays on) and forgets it as the selection.
void turn_off(NativePluginSdkRuntime* rt, BackgroundFxChoice key, Utf8View reason) noexcept {
    char name[kPluginSdkEffectNameBytes];
    u32 name_bytes = 0;
    NativePluginSdkEffectView view{};
    if (rt && native_plugin_sdk_effect(*rt, key.owner_id, key.effect_id, &view))
        name_bytes = utf8_copy_truncated(name, view.name);
    if (rt)
        native_plugin_sdk_effect_fault(rt, key.owner_id, key.effect_id);
    renderer_clear_plugin_effect(&g_app.renderer);
    if (same(g_effects.saved, key))
        save({});
    if (g_app.plugins)
        application_plugins_set_notice(*g_app.plugins,
                                       name_bytes ? Utf8View{name, name_bytes} : utf8_literal("A background effect"),
                                       reason);
    log_u64(LogLevel::warning, "plugins", "effect_turned_off_owner", key.owner_id);
    application_settings_refresh_pages();
    application_request_frame();
}

// The renderer's GPU guard turned the effect off during a draw.
void apply_guard() noexcept {
    auto& fx = g_app.renderer.background_fx;
    if (!fx.tripped)
        return;
    fx.tripped = false;
    turn_off(application_plugins_runtime(), {fx.tripped_owner_id, fx.tripped_effect_id},
             utf8_literal(" was turned off because it slowed down drawing"));
}

} // namespace

void application_plugins_fx_poll(ApplicationPluginsSdk& sdk) noexcept {
    // A stopped effect may leave no frame to report from.
    apply_guard();
    if (g_effects.polled_revision == sdk.runtime.effect_revision)
        return;
    g_effects.polled_revision = sdk.runtime.effect_revision;
    application_request_frame();
    u64 signature = 0;
    list(&signature);
    if (signature != g_effects.rows_signature) {
        g_effects.rows_signature = signature;
        if (application_settings_active())
            application_settings_refresh_pages();
    }
}

void application_plugins_fx_synchronize() noexcept {
    apply_guard();
    auto* rt = application_plugins_runtime();
    if (!g_app.renderer_ready)
        return;
    load();
    NativePluginSdkEffectView view{};
    if (!rt || !background_fx_choice_valid(g_effects.saved) ||
        !native_plugin_sdk_effect(*rt, g_effects.saved.owner_id, g_effects.saved.effect_id, &view)) {
        // Unregistered or its plugin is off: the built-in choice applies and
        // the saved choice returns when the effect is registered again.
        renderer_clear_plugin_effect(&g_app.renderer);
        return;
    }
    const HRESULT result = renderer_select_plugin_effect(&g_app.renderer, view.owner_id, view.effect_id,
                                                         rt->effect_revision, view.bytecode, view.parameters);
    if (FAILED(result)) {
        log_u64(LogLevel::warning, "plugins", "effect_shader_error", static_cast<u32>(result));
        turn_off(rt, g_effects.saved, utf8_literal(" was turned off because it could not be drawn"));
    }
}

bool application_plugins_fx_project(UiSettingsControl* control) noexcept {
    if (!control || control->choices.count != kBackgroundEffectCount)
        return false;
    load();
    const u32 count = list();
    if (!count)
        return false;
    for (u32 i = 0; i < kBackgroundEffectCount; ++i)
        g_effects.choices[i] = control->choices[i];
    for (u32 i = 0; i < count; ++i) {
        g_effects.choices[kBackgroundEffectCount + i] = {g_effects.rows[i].name, g_effects.rows[i].name_bytes};
        if (same(g_effects.rows[i].key, g_effects.saved))
            control->selected_choice = kBackgroundEffectCount + i;
    }
    control->choices = {g_effects.choices, kBackgroundEffectCount + count};
    return true;
}

bool application_plugins_fx_activate(u32 choice, bool* result) noexcept {
    if (result)
        *result = false;
    load();
    if (choice < kBackgroundEffectCount) {
        // A built-in: its own preference is written by the caller.
        if (background_fx_choice_valid(g_effects.saved)) {
            save({});
            renderer_clear_plugin_effect(&g_app.renderer);
            application_request_frame();
        }
        return false;
    }
    const u32 index = choice - kBackgroundEffectCount;
    if (index >= list())
        return true;
    const bool saved = save(g_effects.rows[index].key);
    application_request_frame();
    if (result)
        *result = saved;
    return true;
}

} // namespace maicord::app_detail
