#include "maicord/platform/desktop_integration.hpp"
#include "maicord/core/win32_process.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"

#include <stddef.h>
#include "maicord/core/win32_user.hpp"
#include "maicord/sys/registry.hpp"

namespace maicord {
static_assert(kDesktopShowHide == SW_HIDE && kDesktopShowMinimizedNoActive == SW_SHOWMINNOACTIVE &&
              kDesktopShowDefault == SW_SHOWDEFAULT && kDesktopActivateInactive == WA_INACTIVE);
namespace {
constexpr u32 kRegistryText = 2048;
constexpr wchar_t kStartupArgument[] = L"--startup";
constexpr wchar_t kUrlArgument[] = L"--url";

bool space(wchar_t c) noexcept {
    return c == L' ' || c == L'\t';
}
u32 wide_length(const wchar_t* text) noexcept {
    u32 n = 0;
    while (text && text[n])
        ++n;
    return n;
}
bool equal_literal(const wchar_t* text, u32 units, const wchar_t* literal) noexcept {
    const u32 n = wide_length(literal);
    if (units != n)
        return false;
    for (u32 i = 0; i < n; ++i)
        if (text[i] != literal[i])
            return false;
    return true;
}
bool prefix_folded(ConstSlice<wchar_t> text, u32 at, const wchar_t* literal) noexcept {
    const u32 n = wide_length(literal);
    if (at > text.count || text.count - at < n)
        return false;
    for (u32 i = 0; i < n; ++i)
        if (ascii_lower(text[at + i]) != literal[i])
            return false;
    return true;
}

// Appends wide text; false once the capacity (including the NUL) is exceeded.
struct WideBuilder {
    wchar_t* data;
    u32 capacity, count;
    bool ok;
    __declspec(noinline) WideBuilder& add(const wchar_t* text, u32 units) noexcept {
        if (!ok || units >= capacity - count) {
            ok = false;
            return *this;
        }
        copy_bytes(data + count, text, units * sizeof(wchar_t));
        count += units;
        data[count] = 0;
        return *this;
    }
    WideBuilder& add(const wchar_t* text) noexcept {
        return add(text, wide_length(text));
    }
    WideBuilder& add(ConstSlice<wchar_t> text) noexcept {
        return add(text.data, text.count);
    }
};

bool executable_valid(ConstSlice<wchar_t> executable) noexcept {
    if (!executable.data || executable.count < 4 || executable.count > 1024)
        return false;
    for (u32 i = 0; i < executable.count; ++i)
        if (!executable[i] || executable[i] == L'"')
            return false;
    return true;
}
bool scope_valid(const DesktopRegistryScope& s) noexcept {
    return s.root && s.run_key && s.approved_key && s.classes_key && s.value_name && s.protocol && *s.value_name &&
           *s.protocol;
}

// The program named by a registered command: its quoted (or leading
// unquoted) first token, compared ordinally without case.
bool command_names(const wchar_t* command, u32 units, ConstSlice<wchar_t> executable) noexcept {
    u32 at = 0;
    while (at < units && space(command[at]))
        ++at;
    u32 first = at, last = at;
    if (at < units && command[at] == L'"') {
        first = ++at;
        while (at < units && command[at] != L'"')
            ++at;
        if (at == units)
            return false;
        last = at;
    } else {
        while (at < units && !space(command[at]))
            ++at;
        last = at;
    }
    return last - first == executable.count &&
           CompareStringOrdinal(command + first, static_cast<int>(last - first), executable.data,
                                static_cast<int>(executable.count), TRUE) == CSTR_EQUAL;
}

// REG_SZ read; false when absent, not a string or too long.
bool read_string(HKEY root, const wchar_t* key, const wchar_t* value, wchar_t* out, u32 capacity, u32* units) noexcept {
    DWORD bytes = capacity * sizeof(wchar_t);
    const LSTATUS status = RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, out, &bytes);
    if (status != ERROR_SUCCESS || bytes < sizeof(wchar_t))
        return false;
    *units = static_cast<u32>(bytes / sizeof(wchar_t)) - 1;
    return true;
}
bool write_string(HKEY root, const wchar_t* key, const wchar_t* value, const wchar_t* text) noexcept {
    HKEY opened{};
    if (RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &opened, nullptr) != ERROR_SUCCESS)
        return false;
    const LSTATUS status =
        RegSetValueExW(opened, value, 0, REG_SZ, reinterpret_cast<const BYTE*>(text),
                       static_cast<DWORD>((wide_length(text) + 1) * sizeof(wchar_t)));
    RegCloseKey(opened);
    return status == ERROR_SUCCESS;
}
bool delete_value(HKEY root, const wchar_t* key, const wchar_t* value) noexcept {
    const LSTATUS status = RegDeleteKeyValueW(root, key, value);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}
// Task Manager's StartupApproved record: an odd first byte means disabled.
bool startup_disabled(const DesktopRegistryScope& s) noexcept {
    u8 data[16]{};
    DWORD bytes = sizeof(data);
    return RegGetValueW(s.root, s.approved_key, s.value_name, RRF_RT_REG_BINARY, nullptr, data, &bytes) ==
               ERROR_SUCCESS &&
           bytes && (data[0] & 1);
}
bool protocol_path(const DesktopRegistryScope& s, const wchar_t* suffix, wchar_t* out, u32 capacity) noexcept {
    WideBuilder b{out, capacity, 0, true};
    b.add(s.classes_key).add(L"\\").add(s.protocol).add(suffix);
    return b.ok;
}

struct ForwardPayload {
    u32 version, flags, units;
    wchar_t url[kDesktopUrlCapacity];
};
constexpr u32 kForwardHeaderBytes = 12;
constexpr u32 kPublishedMagic = 0x4D435057u;
struct PublishedWindow {
    u32 magic, process;
    u64 window;
};
static_assert(offsetof(ForwardPayload, url) == kForwardHeaderBytes);

bool decimal_id(ConstSlice<wchar_t> text, u32 first, u32 last, u64* value) noexcept {
    if (first >= last || last - first > 20)
        return false;
    u64 result = 0;
    for (u32 i = first; i < last; ++i) {
        const wchar_t c = text[i];
        if (c < L'0' || c > L'9')
            return false;
        const u64 digit = static_cast<u64>(c - L'0');
        if (result > (~0ull - digit) / 10)
            return false;
        result = result * 10 + digit;
    }
    *value = result;
    return result != 0;
}
bool segment_is(ConstSlice<wchar_t> text, u32 first, u32 last, const wchar_t* literal) noexcept {
    const u32 n = wide_length(literal);
    if (last - first != n)
        return false;
    for (u32 i = 0; i < n; ++i)
        if (ascii_lower(text[first + i]) != literal[i])
            return false;
    return true;
}
} // namespace

bool desktop_launch_parse(ConstSlice<wchar_t> line, DesktopLaunch* launch) noexcept {
    if (!launch)
        return false;
    clear_bytes(launch, sizeof(*launch));
    if (!line.data || !line.count)
        return true;
    for (u32 i = 0; i < line.count; ++i)
        if (!line[i])
            return false;
    u32 at = 0;
    while (at < line.count && space(line[at]))
        ++at;
    // argv0: quotes protect spaces; backslashes are literal.
    bool quoted = false;
    while (at < line.count && (quoted || !space(line[at]))) {
        if (line[at] == L'"')
            quoted = !quoted;
        ++at;
    }
    if (quoted)
        return false;
    enum class Expect : u8 { option, url, url_after_separator } expect = Expect::option;
    wchar_t argument[kDesktopUrlCapacity];
    while (true) {
        while (at < line.count && space(line[at]))
            ++at;
        if (at == line.count)
            break;
        u32 units = 0;
        bool overflow = false;
        quoted = false;
        // CommandLineToArgvW's rules: 2n backslashes before a quote are n
        // backslashes and a delimiter, 2n+1 are n backslashes and a quote.
        const auto put = [&](wchar_t c) noexcept {
            if (units < kDesktopUrlCapacity)
                argument[units++] = c;
            else
                overflow = true;
        };
        while (at < line.count && (quoted || !space(line[at]))) {
            if (line[at] == L'\\') {
                u32 slashes = 0;
                while (at < line.count && line[at] == L'\\') {
                    ++slashes;
                    ++at;
                }
                const bool quote = at < line.count && line[at] == L'"';
                for (u32 i = 0; i < (quote ? slashes / 2 : slashes); ++i)
                    put(L'\\');
                if (quote) {
                    ++at;
                    if (slashes & 1)
                        put(L'"');
                    else
                        quoted = !quoted;
                }
            } else if (line[at] == L'"') {
                quoted = !quoted;
                ++at;
            } else {
                put(line[at++]);
            }
        }
        if (quoted)
            return false;
        const bool url_argument = expect != Expect::option ||
                                  (!launch->url_present && prefix_folded({argument, units}, 0, L"discord:"));
        if (expect == Expect::url && equal_literal(argument, units, L"--")) {
            expect = Expect::url_after_separator;
            continue;
        }
        if (url_argument) {
            if (overflow)
                return false;
            copy_bytes(launch->url, argument, units * sizeof(wchar_t));
            launch->url_units = units;
            launch->url_present = true;
            expect = Expect::option;
            continue;
        }
        if (!overflow && equal_literal(argument, units, kStartupArgument))
            launch->startup = true;
        else if (!overflow && equal_literal(argument, units, kUrlArgument))
            expect = Expect::url;
    }
    return true;
}

bool desktop_link_parse(ConstSlice<wchar_t> url, DesktopLinkTarget* target) noexcept {
    if (!target)
        return false;
    reset_value(target);
    if (!url.data || url.count > kDesktopUrlCapacity || !prefix_folded(url, 0, L"discord:"))
        return false;
    u32 end = url.count;
    for (u32 i = 0; i < url.count; ++i) {
        const wchar_t c = url[i];
        if (c == L'?' || c == L'#') {
            end = i;
            break;
        }
        // Printable ASCII only; percent escapes and spaces are refused.
        if (c <= L' ' || c >= 0x7f || c == L'%' || c == L'\\' || c == L'"')
            return false;
    }
    u32 at = 8;
    if (end - at >= 2 && url[at] == L'/' && url[at + 1] == L'/') {
        at += 2;
        u32 host_end = at;
        while (host_end < end && url[host_end] != L'/')
            ++host_end;
        const bool host = host_end == at || segment_is(url, at, host_end, L"-") ||
                          segment_is(url, at, host_end, L"discord.com") ||
                          segment_is(url, at, host_end, L"www.discord.com") ||
                          segment_is(url, at, host_end, L"canary.discord.com") ||
                          segment_is(url, at, host_end, L"ptb.discord.com") ||
                          segment_is(url, at, host_end, L"discordapp.com");
        if (!host)
            return false;
        at = host_end;
    }
    u32 first[5]{}, last[5]{};
    u32 count = 0;
    while (at < end) {
        while (at < end && url[at] == L'/')
            ++at;
        if (at == end)
            break;
        if (count == 5)
            return false;
        first[count] = at;
        while (at < end && url[at] != L'/')
            ++at;
        last[count++] = at;
    }
    if (count == 0 || (count == 1 && segment_is(url, first[0], last[0], L"app"))) {
        target->kind = DesktopLinkKind::open;
        return true;
    }
    if (count == 2 && segment_is(url, first[0], last[0], L"invite")) {
        const u32 bytes = last[1] - first[1];
        if (bytes < 2 || bytes > kDesktopInviteCodeMaximumBytes)
            return false;
        for (u32 i = 0; i < bytes; ++i) {
            const wchar_t c = url[first[1] + i];
            if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'-'))
                return false;
            target->code[i] = static_cast<char>(c);
        }
        target->code_bytes = static_cast<u8>(bytes);
        target->kind = DesktopLinkKind::invite;
        return true;
    }
    if ((count == 3 || count == 4) && segment_is(url, first[0], last[0], L"channels")) {
        DesktopLinkTarget parsed{};
        if (!segment_is(url, first[1], last[1], L"@me") && !decimal_id(url, first[1], last[1], &parsed.guild_id))
            return false;
        if (!decimal_id(url, first[2], last[2], &parsed.channel_id) ||
            (count == 4 && !decimal_id(url, first[3], last[3], &parsed.message_id)))
            return false;
        parsed.kind = count == 4 ? DesktopLinkKind::message : DesktopLinkKind::channel;
        *target = parsed;
        return true;
    }
    return false;
}

DesktopRegistryScope desktop_registry_user_scope() noexcept {
    return {HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run",
            L"Software\\Classes",
            L"MaiCord",
            L"discord"};
}

DesktopRegistration desktop_startup_query(const DesktopRegistryScope& s, ConstSlice<wchar_t> executable) noexcept {
    wchar_t command[kRegistryText];
    u32 units = 0;
    if (!scope_valid(s) || !read_string(s.root, s.run_key, s.value_name, command, kRegistryText, &units))
        return DesktopRegistration::absent;
    if (!executable_valid(executable) || !command_names(command, units, executable))
        return DesktopRegistration::other;
    return startup_disabled(s) ? DesktopRegistration::absent : DesktopRegistration::ours;
}

bool desktop_startup_set(const DesktopRegistryScope& s, ConstSlice<wchar_t> executable, bool enable) noexcept {
    if (!scope_valid(s))
        return false;
    if (!enable)
        return delete_value(s.root, s.run_key, s.value_name) && delete_value(s.root, s.approved_key, s.value_name);
    wchar_t command[kRegistryText];
    WideBuilder b{command, kRegistryText, 0, true};
    b.add(L"\"").add(executable).add(L"\" ").add(kStartupArgument);
    return executable_valid(executable) && b.ok && write_string(s.root, s.run_key, s.value_name, command) &&
           (!startup_disabled(s) || delete_value(s.root, s.approved_key, s.value_name));
}

DesktopRegistration desktop_protocol_query(const DesktopRegistryScope& s, ConstSlice<wchar_t> executable) noexcept {
    wchar_t key[kRegistryText], command[kRegistryText];
    u32 units = 0;
    if (!scope_valid(s) || !protocol_path(s, L"\\shell\\open\\command", key, kRegistryText))
        return DesktopRegistration::absent;
    if (!read_string(s.root, key, nullptr, command, kRegistryText, &units)) {
        wchar_t root_key[kRegistryText];
        HKEY opened{};
        // A class without a command still belongs to someone else.
        if (protocol_path(s, L"", root_key, kRegistryText) &&
            RegOpenKeyExW(s.root, root_key, 0, KEY_READ, &opened) == ERROR_SUCCESS) {
            RegCloseKey(opened);
            return DesktopRegistration::other;
        }
        return DesktopRegistration::absent;
    }
    return executable_valid(executable) && command_names(command, units, executable) ? DesktopRegistration::ours
                                                                                     : DesktopRegistration::other;
}

bool desktop_protocol_set(const DesktopRegistryScope& s, ConstSlice<wchar_t> executable, bool enable) noexcept {
    wchar_t root_key[kRegistryText], icon_key[kRegistryText], command_key[kRegistryText];
    if (!scope_valid(s) || !protocol_path(s, L"", root_key, kRegistryText) ||
        !protocol_path(s, L"\\DefaultIcon", icon_key, kRegistryText) ||
        !protocol_path(s, L"\\shell\\open\\command", command_key, kRegistryText))
        return false;
    if (!enable) {
        if (desktop_protocol_query(s, executable) != DesktopRegistration::ours)
            return true;
        const LSTATUS tree = RegDeleteTreeW(s.root, root_key);
        if (tree != ERROR_SUCCESS && tree != ERROR_FILE_NOT_FOUND)
            return false;
        const LSTATUS key = RegDeleteKeyW(s.root, root_key);
        return key == ERROR_SUCCESS || key == ERROR_FILE_NOT_FOUND;
    }
    if (!executable_valid(executable))
        return false;
    wchar_t icon[kRegistryText], command[kRegistryText];
    WideBuilder i{icon, kRegistryText, 0, true}, c{command, kRegistryText, 0, true};
    i.add(L"\"").add(executable).add(L"\",0");
    c.add(L"\"").add(executable).add(L"\" ").add(kUrlArgument).add(L" -- \"%1\"");
    return i.ok && c.ok && write_string(s.root, root_key, nullptr, L"URL:Discord Protocol") &&
           write_string(s.root, root_key, L"URL Protocol", L"") && write_string(s.root, icon_key, nullptr, icon) &&
           write_string(s.root, command_key, nullptr, command);
}

HANDLE desktop_instance_mutex(const wchar_t* name, bool* existed) noexcept {
    if (existed)
        *existed = false;
    if (!name || !*name)
        return nullptr;
    const HANDLE mutex = CreateMutexW(nullptr, TRUE, name);
    if (mutex && existed)
        *existed = GetLastError() == ERROR_ALREADY_EXISTS;
    return mutex;
}

HANDLE desktop_instance_publish(const wchar_t* name, HWND window) noexcept {
    if (!name || !*name || !window)
        return nullptr;
    const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                              sizeof(PublishedWindow), name);
    if (!mapping)
        return nullptr;
    auto* view = static_cast<PublishedWindow*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(PublishedWindow)));
    if (!view) {
        CloseHandle(mapping);
        return nullptr;
    }
    const PublishedWindow published{kPublishedMagic, process_id(), reinterpret_cast<u64>(window)};
    copy_bytes(view, &published, sizeof(published));
    UnmapViewOfFile(view);
    return mapping;
}

bool desktop_instance_name(const wchar_t* kind, ConstSlice<wchar_t> profile, wchar_t* out, u32 capacity) noexcept {
    if (!kind || !out || !profile.data || !profile.count)
        return false;
    // FNV-1a over the case-folded path; stable across processes.
    u64 hash = kHash64Seed;
    for (u32 i = 0; i < profile.count; ++i) {
        const wchar_t c = ascii_lower(profile[i] == L'/' ? L'\\' : profile[i]);
        hash = hash64_mix(hash64_mix(hash, static_cast<u64>(c & 0xff)), static_cast<u64>(c >> 8));
    }
    wchar_t hex[17]{};
    for (u32 i = 0; i < 16; ++i)
        hex[i] = L"0123456789abcdef"[(hash >> (60 - i * 4)) & 15];
    WideBuilder b{out, capacity, 0, true};
    b.add(L"Local\\MaiCord.").add(kind).add(L".").add(hex, 16);
    return b.ok;
}

DesktopForwardResult desktop_instance_forward(const wchar_t* name, const DesktopLaunch& launch,
                                              u32 timeout_ms) noexcept {
    if (!name || !*name)
        return DesktopForwardResult::no_window;
    const HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
    if (!mapping)
        return DesktopForwardResult::no_window;
    PublishedWindow published{};
    if (const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(PublishedWindow))) {
        copy_bytes(&published, view, sizeof(published));
        UnmapViewOfFile(view);
    }
    CloseHandle(mapping);
    const HWND window = reinterpret_cast<HWND>(published.window);
    DWORD process = 0;
    if (published.magic != kPublishedMagic || !window || !IsWindow(window) ||
        !GetWindowThreadProcessId(window, &process) || process != published.process)
        return DesktopForwardResult::no_window;
    if (process != process_id())
        AllowSetForegroundWindow(process);
    ForwardPayload payload{};
    payload.version = 1;
    const u32 units = launch.url_present ? minimum(launch.url_units, kDesktopUrlCapacity) : 0;
    payload.flags = launch.url_present ? 1u : 0u;
    payload.units = units;
    copy_bytes(payload.url, launch.url, units * sizeof(wchar_t));
    COPYDATASTRUCT data{kDesktopForwardMagic, kForwardHeaderBytes + units * static_cast<DWORD>(sizeof(wchar_t)),
                        &payload};
    DWORD_PTR result = 0;
    if (!SendMessageTimeoutW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG | SMTO_BLOCK,
                             timeout_ms, &result))
        return DesktopForwardResult::refused;
    return result == TRUE ? DesktopForwardResult::forwarded : DesktopForwardResult::refused;
}

bool desktop_instance_receive(const COPYDATASTRUCT* data, DesktopForwardMessage* message) noexcept {
    if (!data || !message || data->dwData != kDesktopForwardMagic || !data->lpData ||
        data->cbData < kForwardHeaderBytes || data->cbData > sizeof(ForwardPayload))
        return false;
    const auto* bytes = static_cast<const u8*>(data->lpData);
    u32 version = 0, flags = 0, units = 0;
    copy_bytes(&version, bytes, 4);
    copy_bytes(&flags, bytes + 4, 4);
    copy_bytes(&units, bytes + 8, 4);
    if (version != 1 || (flags & ~1u) || units > kDesktopUrlCapacity || (!(flags & 1) && units) ||
        data->cbData != kForwardHeaderBytes + units * sizeof(wchar_t))
        return false;
    message->url_present = (flags & 1) != 0;
    message->url_units = units;
    copy_bytes(message->url, bytes + kForwardHeaderBytes, units * sizeof(wchar_t));
    return true;
}

void desktop_window_reveal(HWND window) noexcept {
    if (!window)
        return;
    if (!IsWindowVisible(window))
        ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
    else if (IsIconic(window))
        ShowWindow(window, SW_RESTORE);
    SetForegroundWindow(window);
}

} // namespace maicord
