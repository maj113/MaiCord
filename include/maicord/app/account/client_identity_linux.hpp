#pragma once

#include "maicord/app/account/user_client_configuration.hpp"

namespace maicord {

// What the Linux desktop client reads from its host to describe it (discord_desktop_core's crash-reporter metadata
// and the web client's property builder; docs.discord.food/reference, Client Properties). A view whose data is null
// is an unset variable or missing file; a non-null empty one is set and empty.
struct LinuxClientEnvironment {
    // os.release(): uname's release.
    Utf8View kernel_release;
    // $LANGUAGE, $LC_ALL, $LC_MESSAGES, $LANG: Chromium's locale sources, in its order of precedence.
    Utf8View language;
    Utf8View lc_all;
    Utf8View lc_messages;
    Utf8View lang;
    // $XDG_CURRENT_DESKTOP, $GDMSESSION and $XDG_SESSION_TYPE.
    Utf8View xdg_current_desktop;
    Utf8View gdm_session;
    Utf8View xdg_session_type;
    // $FLATPAK_ID; $SNAP or $SNAP_NAME; $APPIMAGE or $APPDIR (each set and non-empty).
    bool flatpak;
    bool snap;
    bool appimage;
    // The text of /etc/lsb-release and /etc/os-release.
    Utf8View lsb_release;
    Utf8View os_release;
};

// Bytes behind the views linux_client_environment_read returns.
struct LinuxClientEnvironmentBuffer {
    char kernel_release[65];
    char lsb_release[4096];
    char os_release[4096];
};

// Reads the running host: uname, the process environment and the two release files.
LinuxClientEnvironment linux_client_environment_read(LinuxClientEnvironmentBuffer* buffer) noexcept;

// The locale Chromium takes from the environment before the shipped locales narrow it (an input of
// client_host_locale): the first usable of the first $LANGUAGE entry, $LC_ALL, $LC_MESSAGES and $LANG, as a BCP 47
// tag (encoding and modifier dropped, "_" as "-"; "C", "POSIX" and empty skipped), else en-US.
u32 linux_client_system_locale(const LinuxClientEnvironment& environment, Slice<char> output) noexcept;
// The languages of $LANGUAGE as tags, each ended by a NUL (the input of client_accept_language).
u32 linux_client_preferred_languages(const LinuxClientEnvironment& environment, Slice<char> output) noexcept;
// "<XDG_CURRENT_DESKTOP>,<GDMSESSION>", an unset one as "unknown".
u32 linux_client_window_manager(const LinuxClientEnvironment& environment, Slice<char> output) noexcept;
// The distribution description (lsb_release -ds, else os-release's PRETTY_NAME); 0 when the host names none.
u32 linux_client_distro(const LinuxClientEnvironment& environment, Slice<char> output) noexcept;
// "native", "flatpak", "snap" or "appimage".
Utf8View linux_client_runtime_environment(const LinuxClientEnvironment& environment) noexcept;
// $XDG_SESSION_TYPE, "unknown" when unset; empty when set and empty.
Utf8View linux_client_display_server(const LinuxClientEnvironment& environment) noexcept;

// Fills the host-derived part of a Linux configuration: storage's locale, discord_locale, accept_language, os_version,
// window_manager, distro, display_server and timezone, and every property except the launch ID and signature.
UserClientConfigurationResult linux_client_identity_build(const LinuxClientEnvironment& environment,
                                                          UserClientConfigurationStorage* storage,
                                                          GatewayUserProperties* properties) noexcept;

} // namespace maicord
