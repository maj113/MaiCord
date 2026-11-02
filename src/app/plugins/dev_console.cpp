#include "dev_console.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_system_time.hpp"

namespace maicord::app_detail {
namespace {

struct Console {
    DevLine* lines;
    u32 count, first, revision;
    bool enabled;
};
Console g;

Utf8View owner_name(const PluginRuntime* runtime, u64 owner_id) noexcept {
    if (runtime)
        for (u32 i = 0; i < runtime->binding_count; ++i) {
            const PluginBinding& b = *runtime->bindings[i];
            if (b.owner_id == owner_id) return {b.image.info.name, b.image.info.name_bytes};
        }
    return utf8_literal("plugin");
}

constexpr Utf8View kCallNames[]{
    utf8_literal("a call"), utf8_literal("query"), utf8_literal("start"), utf8_literal("stop"), utf8_literal("a settings call"),
    utf8_literal("a menu call"), utf8_literal("pre_send"), utf8_literal("observe"), utf8_literal("a command"),
    utf8_literal("a button"), utf8_literal("a timer"), utf8_literal("a keybind"), utf8_literal("an event"),
    utf8_literal("decorate"), utf8_literal("a modal"), utf8_literal("pre_edit")};
static_assert(sizeof(kCallNames) / sizeof(kCallNames[0]) == static_cast<u32>(PluginCallKind::last) + 1);

} // namespace

void DevText::add(Utf8View text) noexcept {
    const u32 room = kDevLineTextBytes - used;
    const u32 bytes = utf8_truncated_length(text, room);
    copy_bytes(data + used, text.data, bytes);
    used += bytes;
}

void DevText::number(u64 value) noexcept {
    char digits[20];
    u32 count = 0;
    do digits[count++] = static_cast<char>('0' + value % 10), value /= 10;
    while (value);
    while (count) add({&digits[--count], 1});
}

void application_dev_console_enable(bool on) noexcept {
    if (g.enabled == on) return;
    g.enabled = on;
    if (!on) {
        if (g.lines) VirtualFree(g.lines, 0, MEM_RELEASE);
        g = {};
    }
    ++g.revision;
}

bool application_dev_console_enabled() noexcept { return g.enabled; }

void application_dev_console_add(DevLineKind kind, Utf8View name, Utf8View text) noexcept {
    if (!g.enabled) return;
    if (!g.lines) {
        g.lines = static_cast<DevLine*>(VirtualAlloc(nullptr, sizeof(DevLine) * kDevConsoleLines, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!g.lines) return;
    }
    u32 slot;
    if (g.count < kDevConsoleLines) slot = (g.first + g.count++) % kDevConsoleLines;
    else slot = g.first, g.first = (g.first + 1) % kDevConsoleLines;
    DevLine& line = g.lines[slot];
    line.kind = kind;
    line.name_bytes = static_cast<u8>(utf8_truncated_length(name, kDevLineNameBytes));
    copy_bytes(line.name, name.data, line.name_bytes);
    line.text_bytes = static_cast<u8>(utf8_truncated_length(text, kDevLineTextBytes));
    copy_bytes(line.text, text.data, line.text_bytes);
    // One line each: a plugin's text may hold line breaks or controls.
    for (u32 i = 0; i < line.text_bytes; ++i)
        if (static_cast<u8>(line.text[i]) < 0x20 || line.text[i] == 0x7f) line.text[i] = ' ';
    SYSTEMTIME now{};
    GetLocalTime(&now);
    line.second_of_day = (now.wHour * 60u + now.wMinute) * 60u + now.wSecond;
    ++g.revision;
}

u32 application_dev_console_count() noexcept { return g.count; }

const DevLine* application_dev_console_line(u32 index) noexcept {
    return index < g.count ? &g.lines[(g.first + index) % kDevConsoleLines] : nullptr;
}

u32 application_dev_console_revision() noexcept { return g.revision; }

void application_dev_console_clear() noexcept {
    g.count = g.first = 0;
    ++g.revision;
}

void application_dev_console_log(void* runtime, u64 owner_id, u32 level, Utf8View text) noexcept {
    if (!g.enabled || level > MC_LOG_ERROR) return;
    application_dev_console_add(static_cast<DevLineKind>(level), owner_name(static_cast<const PluginRuntime*>(runtime), owner_id), text);
}

void application_dev_console_strike(void* runtime, u64 owner_id, PluginCallKind kind, u32 strike, u64 elapsed_us) noexcept {
    if (!g.enabled) return;
    DevText w{};
    w.add(kCallNames[static_cast<u32>(kind) <= static_cast<u32>(PluginCallKind::last) ? static_cast<u32>(kind) : 0]);
    w.add(utf8_literal(" ran past its time budget: "));
    w.number(elapsed_us / 1000);
    w.add(utf8_literal(" ms. Strike "));
    w.number(strike < kPluginCallOverrunLimit ? strike : kPluginCallOverrunLimit);
    w.add(utf8_literal(" of "));
    w.number(kPluginCallOverrunLimit);
    w.add(strike >= kPluginCallOverrunLimit ? utf8_literal("; it is turned off.") : utf8_literal("."));
    application_dev_console_add(DevLineKind::strike, owner_name(static_cast<const PluginRuntime*>(runtime), owner_id), w.view());
}

void application_dev_load_error_text(const PluginLoadResult& load, DevText* text) noexcept {
    static constexpr Utf8View kProblems[]{
        utf8_literal("a pointer leaves the image or the code"), utf8_literal("its size"), utf8_literal("the ABI"),
        utf8_literal("flags or reserved fields"), utf8_literal("the owner id"), utf8_literal("a string"),
        utf8_literal("the permissions"), utf8_literal("a reserved field"), utf8_literal("the state"),
        utf8_literal("settings"), utf8_literal("hooks"), utf8_literal("commands"), utf8_literal("menus"),
        utf8_literal("buttons"), utf8_literal("network hosts")};
    DevText& w = *text;
    switch (load.error) {
    case PluginLoadError::directory: w.add(utf8_literal("The plugins folder couldn't be opened.")); break;
    case PluginLoadError::open:
    case PluginLoadError::read: w.add(utf8_literal("The DLL couldn't be read (is the build still writing it?).")); break;
    case PluginLoadError::not_pe:
    case PluginLoadError::machine: w.add(utf8_literal("It isn't an x64 DLL.")); break;
    case PluginLoadError::entry_point: w.add(utf8_literal("The DLL has an entry point; link with /NOENTRY.")); break;
    case PluginLoadError::tls: w.add(utf8_literal("The DLL uses thread-local storage.")); break;
    case PluginLoadError::sections: w.add(utf8_literal("The DLL's sections are malformed, or writable and executable.")); break;
    case PluginLoadError::exports: w.add(utf8_literal("The DLL must export only maicord_plugin_v6, as data.")); break;
    case PluginLoadError::load: w.add(utf8_literal("Windows couldn't load the DLL (does it import anything?).")); break;
    case PluginLoadError::descriptor: {
        w.add(utf8_literal("The descriptor is invalid: "));
        const u32 problem = static_cast<u32>(load.problem);
        w.add(problem >= 1 && problem <= sizeof(kProblems) / sizeof(kProblems[0]) ? kProblems[problem - 1] : utf8_literal("it fails a rule"));
        if (load.index) {
            w.add(utf8_literal(" #"));
            w.number(load.index);
        }
        w.add(utf8_literal("."));
        break;
    }
    case PluginLoadError::api_level: w.add(utf8_literal("It needs a newer MaiCord.")); break;
    case PluginLoadError::permissions: w.add(utf8_literal("It needs permissions that weren't approved.")); break;
    case PluginLoadError::release: w.add(utf8_literal("The old build couldn't be unloaded.")); break;
    case PluginLoadError::digest: w.add(utf8_literal("The file changed while it was being read.")); break;
    default: w.add(utf8_literal("It couldn't be loaded.")); break;
    }
}

void application_dev_enable_error_text(const PluginEnableResult& result, DevText* text) noexcept {
    if (result.error == PluginEnableError::load) return application_dev_load_error_text(result.load, text);
    DevText& w = *text;
    switch (result.error) {
    case PluginEnableError::start_failed: w.add(utf8_literal("start failed or ran past its budget.")); break;
    case PluginEnableError::duplicate_owner: w.add(utf8_literal("another plugin has the same owner id.")); break;
    case PluginEnableError::capacity: w.add(utf8_literal("too many plugins are on.")); break;
    case PluginEnableError::release: w.add(utf8_literal("the old build couldn't be unloaded.")); break;
    default: w.add(utf8_literal("it didn't start.")); break;
    }
}

} // namespace maicord::app_detail
