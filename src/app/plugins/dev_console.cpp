#include "dev_console.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/os/local_time.hpp"

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

constexpr char kCallNames[] =
    "a call\0query\0start\0stop\0a settings call\0a menu call\0pre_send\0observe\0a command\0a button\0a timer\0"
    "a keybind\0an event\0decorate\0a modal\0pre_edit\0";
static_assert(name_list_count(kCallNames) == static_cast<u32>(PluginCallKind::last) + 1);

} // namespace

void DevText::add(const Utf8View& text) noexcept {
    const u32 room = kDevLineTextBytes - used;
    const u32 bytes = utf8_truncated_length(text, room);
    copy_bytes(data + used, text.data, bytes);
    used += bytes;
}

__declspec(noinline) void DevText::add(const char* text) noexcept {
    add(Utf8View{text, c_string_length(text)});
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
        if (g.lines) pages_release(g.lines);
        g = {};
    }
    ++g.revision;
}

bool application_dev_console_enabled() noexcept { return g.enabled; }

void application_dev_console_add(DevLineKind kind, const Utf8View& name, const Utf8View& text) noexcept {
    if (!g.enabled) return;
    if (!g.lines) {
        g.lines = static_cast<DevLine*>(commit_pages(sizeof(DevLine) * kDevConsoleLines));
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
    CalendarTime now{};
    local_time_now(&now);
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
    w.add(name_list_at(kCallNames, static_cast<u32>(kind) <= static_cast<u32>(PluginCallKind::last) ? static_cast<u32>(kind) : 0));
    w.add(" ran past its time budget: ");
    w.number(elapsed_us / 1000);
    w.add(" ms. Strike ");
    w.number(strike < kPluginCallOverrunLimit ? strike : kPluginCallOverrunLimit);
    w.add(" of ");
    w.number(kPluginCallOverrunLimit);
    w.add(strike >= kPluginCallOverrunLimit ? utf8_literal("; it is turned off.") : utf8_literal("."));
    application_dev_console_add(DevLineKind::strike, owner_name(static_cast<const PluginRuntime*>(runtime), owner_id), w.view());
}

void application_dev_load_error_text(const PluginLoadResult& load, DevText* text) noexcept {
    static constexpr char kProblems[] =
        "a pointer leaves the image or the code\0its size\0the ABI\0flags or reserved fields\0the owner id\0a string\0"
        "the permissions\0a reserved field\0the state\0settings\0hooks\0commands\0menus\0buttons\0network hosts\0";
    DevText& w = *text;
    switch (load.error) {
    case PluginLoadError::directory: w.add("The plugins folder couldn't be opened."); break;
#if defined(_WIN32)
    case PluginLoadError::open:
    case PluginLoadError::read: w.add("The DLL couldn't be read (is the build still writing it?)."); break;
    case PluginLoadError::not_pe:
    case PluginLoadError::machine: w.add("It isn't an x64 DLL."); break;
    case PluginLoadError::entry_point: w.add("The DLL has an entry point; link with /NOENTRY."); break;
    case PluginLoadError::tls: w.add("The DLL uses thread-local storage."); break;
    case PluginLoadError::sections: w.add("The DLL's sections are malformed, or writable and executable."); break;
    case PluginLoadError::exports: w.add("The DLL must export only maicord_plugin_v6, as data."); break;
    case PluginLoadError::load: w.add("Windows couldn't load the DLL (does it import anything?)."); break;
#else
    case PluginLoadError::open:
    case PluginLoadError::read: w.add("The file couldn't be read (is the build still writing it, or is it writable by others?)."); break;
    case PluginLoadError::not_pe:
    case PluginLoadError::machine: w.add("It isn't an x86-64 shared object."); break;
    case PluginLoadError::entry_point: w.add("The shared object has an entry point or an initializer; link with -nostdlib."); break;
    case PluginLoadError::tls: w.add("The shared object uses thread-local storage."); break;
    case PluginLoadError::sections:
        w.add("The shared object's segments are malformed, writable and executable, or the stack is executable.");
        break;
    case PluginLoadError::exports: w.add("The shared object must export only maicord_plugin_v6, as data."); break;
    case PluginLoadError::load: w.add("The system couldn't load the shared object."); break;
    case PluginLoadError::imports: w.add("The shared object imports something; link with -nostdlib and no libraries."); break;
    case PluginLoadError::relocations:
        w.add("The shared object has relocations other than relative ones; build with -fvisibility=hidden and -Wl,-Bsymbolic.");
        break;
#endif
    case PluginLoadError::descriptor: {
        w.add("The descriptor is invalid: ");
        const u32 problem = static_cast<u32>(load.problem);
        w.add(problem >= 1 && problem <= name_list_count(kProblems) ? name_list_at(kProblems, problem - 1) : utf8_literal("it fails a rule"));
        if (load.index) {
            w.add(" #");
            w.number(load.index);
        }
        w.add(".");
        break;
    }
    case PluginLoadError::api_level: w.add("It needs a newer MaiCord."); break;
    case PluginLoadError::permissions: w.add("It needs permissions that weren't approved."); break;
    case PluginLoadError::release: w.add("The old build couldn't be unloaded."); break;
    case PluginLoadError::digest: w.add("The file changed while it was being read."); break;
    default: w.add("It couldn't be loaded."); break;
    }
}

void application_dev_enable_error_text(const PluginEnableResult& result, DevText* text) noexcept {
    if (result.error == PluginEnableError::load) return application_dev_load_error_text(result.load, text);
    DevText& w = *text;
    switch (result.error) {
    case PluginEnableError::start_failed: w.add("start failed or ran past its budget."); break;
    case PluginEnableError::duplicate_owner: w.add("another plugin has the same owner id."); break;
    case PluginEnableError::capacity: w.add("too many plugins are on."); break;
    case PluginEnableError::release: w.add("the old build couldn't be unloaded."); break;
    default: w.add("it didn't start."); break;
    }
}

} // namespace maicord::app_detail
