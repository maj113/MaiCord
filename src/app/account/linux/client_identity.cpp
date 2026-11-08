// The Linux desktop client's identity: the host-derived client properties of docs.discord.food/reference (Client
// Properties), read as discord_desktop_core and the web client read them.
#include "maicord/app/account/client_identity_linux.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/protocol/http/client_user_agent.hpp"
#include "maicord/sys/linux/calls.hpp"
#include "maicord/sys/linux/process.hpp"

namespace maicord {
namespace {

// The longest environment-derived text kept of a window manager's two parts: a longer one reads as unset.
constexpr u32 kPartBytes = 120;

Utf8View environment_view(const char* name) noexcept {
    const char* value = environment_get(name);
    if (!value)
        return {};
    u32 bytes = 0;
    while (value[bytes])
        ++bytes;
    return {value, bytes};
}

// A variable that is set and not empty.
bool environment_nonempty(const char* name) noexcept {
    const char* value = environment_get(name);
    return value && *value;
}

u32 emit(Utf8View text, Slice<char> output) noexcept {
    if (text.bytes >= output.count)
        return 0;
    if (text.bytes)
        copy_bytes(output.data, text.data, text.bytes);
    output.data[text.bytes] = 0;
    return text.bytes;
}

// Text that fits `limit` bytes and is valid UTF-8; the values the writer would refuse are treated as absent.
bool usable(Utf8View text, u32 limit) noexcept {
    return text.data && text.bytes <= limit && utf8_validate(text).valid;
}

// One locale variable as a BCP 47 tag, written to output (kGatewayUserMaximumLocaleBytes); 0 when it names none
// ("C", "POSIX", empty or malformed).
u32 locale_tag(Utf8View raw, char* output) noexcept {
    u32 bytes = 0;
    while (bytes < raw.bytes && raw.data[bytes] != '.' && raw.data[bytes] != '@')
        ++bytes;
    if (bytes == 0 || bytes >= kGatewayUserMaximumLocaleBytes)
        return 0;
    const Utf8View name{raw.data, bytes};
    if (utf8_equal(name, utf8_literal("C")) || utf8_equal(name, utf8_literal("POSIX")))
        return 0;
    for (u32 index = 0; index < bytes; ++index) {
        const char c = raw.data[index];
        if (!ascii_is_alnum(c) && c != '_' && c != '-')
            return 0;
        output[index] = c == '_' ? '-' : c;
    }
    output[bytes] = 0;
    return bytes;
}

// The entry of a colon-separated $LANGUAGE list that starts at `at`; `at` moves past its separator.
Utf8View language_entry(Utf8View list, u32* at) noexcept {
    u32 end = *at;
    while (end < list.bytes && list.data[end] != ':')
        ++end;
    const Utf8View entry{list.data + *at, end - *at};
    *at = end < list.bytes ? end + 1 : list.bytes + 1;
    return entry;
}

// The first line of text that starts with `key` and "=", its remainder after the "="; null data when none follows
// `from` (an offset, moved past the line).
Utf8View next_assignment(Utf8View text, Utf8View key, u32* from) noexcept {
    for (u32 line = *from; line < text.bytes;) {
        u32 end = line;
        while (end < text.bytes && text.data[end] != '\n')
            ++end;
        const u32 length = end - line;
        const u32 next = end + 1;
        if (length > key.bytes && bytes_equal(text.data + line, key.data, key.bytes) &&
            text.data[line + key.bytes] == '=') {
            *from = next;
            return {text.data + line + key.bytes + 1, length - key.bytes - 1};
        }
        line = next;
    }
    *from = text.bytes;
    return {};
}

// os-release's PRETTY_NAME as the client's /^PRETTY_NAME="?([^"\n]+)"?$/m reads it.
Utf8View pretty_name(Utf8View os_release) noexcept {
    u32 from = 0;
    for (;;) {
        const Utf8View rest = next_assignment(os_release, utf8_literal("PRETTY_NAME"), &from);
        if (!rest.data)
            return {};
        u32 first = rest.bytes && rest.data[0] == '"' ? 1u : 0u;
        u32 last = first;
        while (last < rest.bytes && rest.data[last] != '"')
            ++last;
        if (last == first)
            continue;
        const u32 after = last < rest.bytes ? last + 1 : last;
        if (after == rest.bytes)
            return {rest.data + first, last - first};
    }
}

// lsb-release's DISTRIB_DESCRIPTION, without the quotes around it.
Utf8View lsb_description(Utf8View lsb_release) noexcept {
    u32 from = 0;
    Utf8View value = ascii_trim(next_assignment(lsb_release, utf8_literal("DISTRIB_DESCRIPTION"), &from));
    if (value.bytes >= 2 && (value.data[0] == '"' || value.data[0] == '\'') &&
        value.data[value.bytes - 1] == value.data[0])
        value = {value.data + 1, value.bytes - 2};
    return value;
}

// The text of a file up to buffer's capacity, null data when it cannot be read.
Utf8View read_text(const char* path, char* buffer, u32 capacity) noexcept {
    const long file = sys_open(path, kOpenReadOnly);
    if (syscall_failed(file))
        return {};
    u32 size = 0;
    while (size < capacity) {
        const long got = sys_read(static_cast<int>(file), buffer + size, capacity - size);
        if (syscall_failed(got) || got == 0)
            break;
        size += static_cast<u32>(got);
    }
    sys_close(static_cast<int>(file));
    return {buffer, size};
}

} // namespace

LinuxClientEnvironment linux_client_environment_read(LinuxClientEnvironmentBuffer* buffer) noexcept {
    LinuxClientEnvironment environment{};
    Utsname name{};
    if (!syscall_failed(sys_uname(&name))) {
        u32 bytes = 0;
        while (bytes < sizeof(name.release) - 1 && name.release[bytes])
            ++bytes;
        copy_bytes(buffer->kernel_release, name.release, bytes);
        buffer->kernel_release[bytes] = 0;
        environment.kernel_release = {buffer->kernel_release, bytes};
    }
    environment.language = environment_view("LANGUAGE");
    environment.lc_all = environment_view("LC_ALL");
    environment.lc_messages = environment_view("LC_MESSAGES");
    environment.lang = environment_view("LANG");
    environment.xdg_current_desktop = environment_view("XDG_CURRENT_DESKTOP");
    environment.gdm_session = environment_view("GDMSESSION");
    environment.xdg_session_type = environment_view("XDG_SESSION_TYPE");
    environment.flatpak = environment_nonempty("FLATPAK_ID");
    environment.snap = environment_nonempty("SNAP") || environment_nonempty("SNAP_NAME");
    environment.appimage = environment_nonempty("APPIMAGE") || environment_nonempty("APPDIR");
    environment.lsb_release = read_text("/etc/lsb-release", buffer->lsb_release, sizeof(buffer->lsb_release));
    environment.os_release = read_text("/etc/os-release", buffer->os_release, sizeof(buffer->os_release));
    return environment;
}

u32 linux_client_system_locale(const LinuxClientEnvironment& environment, Slice<char> output) noexcept {
    char tag[kGatewayUserMaximumLocaleBytes];
    for (u32 at = 0; at <= environment.language.bytes && environment.language.data;) {
        if (const u32 bytes = locale_tag(language_entry(environment.language, &at), tag))
            return emit({tag, bytes}, output);
    }
    const Utf8View variables[] = {environment.lc_all, environment.lc_messages, environment.lang};
    for (const Utf8View& variable : variables)
        if (variable.data)
            if (const u32 bytes = locale_tag(variable, tag))
                return emit({tag, bytes}, output);
    return emit(utf8_literal("en-US"), output);
}

u32 linux_client_preferred_languages(const LinuxClientEnvironment& environment, Slice<char> output) noexcept {
    u32 used = 0;
    char tag[kGatewayUserMaximumLocaleBytes];
    for (u32 at = 0; environment.language.data && at <= environment.language.bytes;) {
        const u32 bytes = locale_tag(language_entry(environment.language, &at), tag);
        if (!bytes)
            continue;
        if (used + bytes + 1 > output.count)
            break;
        copy_bytes(output.data + used, tag, bytes + 1);
        used += bytes + 1;
    }
    return used;
}

u32 linux_client_window_manager(const LinuxClientEnvironment& environment, Slice<char> output) noexcept {
    const auto part = [](Utf8View value) noexcept {
        return usable(value, kPartBytes) ? value : utf8_literal("unknown");
    };
    const Utf8View desktop = part(environment.xdg_current_desktop);
    const Utf8View session = part(environment.gdm_session);
    if (desktop.bytes + 1 + session.bytes >= output.count)
        return 0;
    copy_bytes(output.data, desktop.data, desktop.bytes);
    output.data[desktop.bytes] = ',';
    copy_bytes(output.data + desktop.bytes + 1, session.data, session.bytes);
    const u32 bytes = desktop.bytes + 1 + session.bytes;
    output.data[bytes] = 0;
    return bytes;
}

u32 linux_client_distro(const LinuxClientEnvironment& environment, Slice<char> output) noexcept {
    // Missing files read as empty text, which names nothing.
    Utf8View description = ascii_trim(lsb_description(environment.lsb_release));
    if (!description.bytes)
        description = ascii_trim(pretty_name(environment.os_release));
    if (!description.bytes || !usable(description, kGatewayUserMaximumHostBytes))
        return 0;
    return emit(description, output);
}

Utf8View linux_client_runtime_environment(const LinuxClientEnvironment& environment) noexcept {
    if (environment.flatpak)
        return utf8_literal("flatpak");
    if (environment.snap)
        return utf8_literal("snap");
    if (environment.appimage)
        return utf8_literal("appimage");
    return utf8_literal("native");
}

Utf8View linux_client_display_server(const LinuxClientEnvironment& environment) noexcept {
    if (!environment.xdg_session_type.data)
        return utf8_literal("unknown");
    return usable(environment.xdg_session_type, kGatewayUserMaximumRuntimeBytes) ? environment.xdg_session_type
                                                                                 : Utf8View{};
}

UserClientConfigurationResult linux_client_identity_build(const LinuxClientEnvironment& environment,
                                                          UserClientConfigurationStorage* storage,
                                                          GatewayUserProperties* properties) noexcept {
    // navigator.languages[0]: the locale Chromium resolved from the environment among the locales the host ships.
    char system_locale[kGatewayUserMaximumLocaleBytes];
    const u32 system_bytes = linux_client_system_locale(environment, {system_locale, sizeof(system_locale)});
    if (!system_bytes)
        return {0, UserClientConfigurationError::locale};
    const u32 locale_bytes =
        client_host_locale({system_locale, system_bytes}, {storage->locale, kGatewayUserMaximumLocaleBytes});
    const Utf8View host_locale{storage->locale, locale_bytes};
    storage->discord_locale_bytes =
        client_discord_locale(host_locale, {storage->discord_locale, kGatewayUserMaximumLocaleBytes});
    char preferred[kRestUserAcceptLanguageMaximumBytes];
    const u32 preferred_bytes = linux_client_preferred_languages(environment, {preferred, sizeof(preferred)});
    storage->accept_language_bytes = client_accept_language(
        host_locale, {preferred, preferred_bytes}, {storage->accept_language, kRestUserAcceptLanguageMaximumBytes});
    storage->timezone_bytes = host_iana_timezone({storage->timezone, sizeof(storage->timezone)});

    // os.release(): the kernel release.
    const Utf8View release = environment.kernel_release;
    if (!release.data || !release.bytes || release.bytes > kGatewayUserMaximumVersionBytes ||
        !utf8_validate(release).valid)
        return {0, UserClientConfigurationError::version};
    emit(release, {storage->os_version, sizeof(storage->os_version)});

    const u32 manager_bytes =
        linux_client_window_manager(environment, {storage->window_manager, sizeof(storage->window_manager)});
    const u32 distro_bytes = linux_client_distro(environment, {storage->distro, sizeof(storage->distro)});
    const u32 display_bytes = emit(linux_client_display_server(environment),
                                   {storage->display_server, sizeof(storage->display_server)});

    *properties = GatewayUserProperties{
        utf8_literal(kClientOperatingSystem),
        utf8_literal(kClientBrowser),
        utf8_literal("stable"),
        utf8_literal(kClientVersion),
        Utf8View{storage->os_version, release.bytes},
        // Discord ships the Linux client for x86-64 only, and MaiCord's Linux build is x86-64.
        utf8_literal("x64"),
        utf8_literal("x64"),
        host_locale,
        {},
        utf8_literal(kClientUserAgent),
        utf8_literal(kClientElectronVersion),
        {},
        kClientBuildNumber,
        kClientNativeBuildNumber,
        {},
        {},
        {},
        true,
        Utf8View{storage->window_manager, manager_bytes},
        Utf8View{storage->distro, distro_bytes},
        linux_client_runtime_environment(environment),
        Utf8View{storage->display_server, display_bytes},
    };
    return {0, UserClientConfigurationError::none};
}

} // namespace maicord
