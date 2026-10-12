#pragma once
#include "maicord/core/slice.hpp"

namespace maicord {
struct UpdateServiceConfig;
struct UpdateHandoffChild;
}

namespace maicord::app_detail {
// Win32's complete command-line limit includes the terminating NUL. This is
// separate from the updater child's existing 1024-unit protocol payload bound.
inline constexpr u32 kApplicationUpdateStartupCommandCharacters=32767;
inline constexpr u32 kApplicationUpdateStartupRefusalExitCode=24;
enum class ApplicationUpdateStartupCommandKind : u8 { ordinary, update_child, invalid };
enum class ApplicationUpdateStartupMode : u8 { ordinary, refused, child, dispatched };
enum class ApplicationUpdateStartupReason : u8 { none, unenrolled_child, invalid_command };
struct ApplicationUpdateStartupDecision {
    ApplicationUpdateStartupMode mode;
    ApplicationUpdateStartupReason reason;
    u32 exit_code;
};
// Bounded entry adapter for GetCommandLineW's NUL-terminated borrowed buffer.
// Null/empty or missing NUL within the Win32 bound is refused by gate().
ConstSlice<wchar_t> application_update_startup_command(const wchar_t*) noexcept;
// Reserves the ASCII case-insensitive --maicord-update-child token family in
// any argument after argv0, including quoted/fragmented or malformed variants.
// It does not parse IDs/handles, validate protocol payloads, or confer trust.
ApplicationUpdateStartupCommandKind application_update_startup_classify(ConstSlice<wchar_t>) noexcept;
// Unenrolled gate: every requested child mode fails closed before ordinary
// startup. Used when the build has no updater configuration.
ApplicationUpdateStartupDecision application_update_startup_gate(ConstSlice<wchar_t>) noexcept;
// Enrolled dispatcher. A child command is admitted only after the trusted
// enrollment, journal and signed installed package independently validate the
// own module path (mode child; confirm later). An ordinary launch hands off to
// a newer authenticated active generation and exits once it confirms startup
// (mode dispatched, exit 0); every other case is ordinary. Root, key and
// expected identity never come from the command line.
ApplicationUpdateStartupDecision application_update_startup_run(ConstSlice<wchar_t>, const UpdateServiceConfig&) noexcept;
// Child handoff capabilities retained until confirmation; null otherwise.
UpdateHandoffChild* application_update_startup_child() noexcept;

// An ordinary launch from outside a generation publishes its own module here;
// handoff children (and their restarts) inherit it through the environment.
inline constexpr wchar_t kApplicationUpdateDispatcherVariable[] = L"MAICORD_DISPATCHER_PATH";
// <...>\generations\g-<64 lowercase hex>\<file>: a per-version folder the
// updater installs and later deletes.
inline bool application_update_startup_generation_module(ConstSlice<wchar_t> path) noexcept {
    constexpr wchar_t folder[] = L"\\generations\\g-";
    constexpr u32 folder_count = sizeof(folder) / sizeof(wchar_t) - 1, hex_count = 64;
    if (!path.data) return false;
    u32 leaf = path.count;
    while (leaf && path[leaf - 1] != L'\\') --leaf;
    if (leaf == path.count || leaf < folder_count + hex_count + 1) return false;
    const u32 start = leaf - 1 - hex_count - folder_count;
    for (u32 i = 0; i < folder_count; ++i) {
        wchar_t c = path[start + i];
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + (L'a' - L'A'));
        if (c != folder[i]) return false;
    }
    for (u32 i = start + folder_count; i < leaf - 1; ++i)
        if (!((path[i] >= L'0' && path[i] <= L'9') || (path[i] >= L'a' && path[i] <= L'f'))) return false;
    return true;
}
// Executable recorded by the Start-menu shortcut and notification registration:
// the own module, or for a generation the inherited dispatcher (absolute and
// itself outside a generation). Empty when no stable path is known.
inline ConstSlice<wchar_t> application_update_startup_registration_path(ConstSlice<wchar_t> module,
    ConstSlice<wchar_t> inherited_dispatcher) noexcept {
    if (!module.data || !module.count) return {};
    if (!application_update_startup_generation_module(module)) return module;
    const auto& d = inherited_dispatcher;
    const bool absolute = d.data && d.count >= 4 &&
        ((((d[0] | 0x20) >= L'a' && (d[0] | 0x20) <= L'z') && d[1] == L':' && d[2] == L'\\') ||
         (d[0] == L'\\' && d[1] == L'\\' && d[2] != L'?' && d[2] != L'.'));
    return absolute && !application_update_startup_generation_module(d) ? d : ConstSlice<wchar_t>{};
}
} // namespace maicord::app_detail
