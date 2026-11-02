#pragma once

#include "maicord/app/plugins/binding.hpp"

// The Developer console (plugin design 13): a small in-memory ring of what a plugin author wants to see while a
// build runs inside MaiCord. Lines come from the plugins' own ctx.log calls, from the host's budget strikes and
// from reloads of the dev folder's builds. Nothing here is written to disk or to the application log, and the
// host's own lines are fixed texts: no message content, names of users or channels, tokens or URLs. A plugin's
// log text is the plugin's own, shown only here. Everything is off, and holds no memory, outside Developer Mode.
// UI thread only.
namespace maicord::app_detail {

enum class DevLineKind : u8 { debug, info, warning, error, strike, reload, problem };

inline constexpr u32 kDevConsoleLines = 64;
inline constexpr u32 kDevLineNameBytes = 24;
inline constexpr u32 kDevLineTextBytes = 160;

struct DevLine {
    DevLineKind kind;
    u8 name_bytes, text_bytes;
    u32 second_of_day; // local time
    char name[kDevLineNameBytes];
    char text[kDevLineTextBytes];
};

// Developer Mode turned on or off. Turning it off frees the lines.
void application_dev_console_enable(bool on) noexcept;
bool application_dev_console_enabled() noexcept;
// Adds a line (control characters become spaces, long text is cut); name is the plugin's or the file's.
void application_dev_console_add(DevLineKind, const Utf8View& name, const Utf8View& text) noexcept;
// index 0 is the oldest of application_dev_console_count() lines; null past the end. Valid until the next add.
u32 application_dev_console_count() noexcept;
const DevLine* application_dev_console_line(u32 index) noexcept;
// Changes with every add and clear.
u32 application_dev_console_revision() noexcept;
void application_dev_console_clear() noexcept;

// The PluginRuntime sinks (sink_context is the PluginRuntime, which names the plugin by its owner id).
void application_dev_console_log(void* runtime, u64 owner_id, u32 level, Utf8View text) noexcept;
void application_dev_console_strike(void* runtime, u64 owner_id, PluginCallKind kind, u32 strike, u64 elapsed_us) noexcept;

// One line of text under construction; what doesn't fit is cut.
struct DevText {
    char data[kDevLineTextBytes];
    u32 used;
    void add(const Utf8View& text) noexcept;
    void add(const char* literal) noexcept; // counted here, so a call site passes one pointer
    void number(u64 value) noexcept;
    Utf8View view() const noexcept { return {data, used}; }
};

// A sentence for why a build didn't load or start: "The DLL has an entry point; link with /NOENTRY."
void application_dev_load_error_text(const PluginLoadResult& load, DevText* text) noexcept;
void application_dev_enable_error_text(const PluginEnableResult& result, DevText* text) noexcept;

} // namespace maicord::app_detail
