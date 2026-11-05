#include "maicord/app/account/user_client_configuration.hpp"
#include "maicord/protocol/gateway/commands.hpp"
#if !defined(_WIN32)
#include "maicord/app/account/client_identity_linux.hpp"
#include "gateway/client_internal.hpp"
#endif

// The client properties' bytes. The Windows layout is pinned so the Linux layout cannot move it; the Linux fixtures
// give the identity builder a host (environment variables, kernel release and release files) and compare what it
// reports against what the official Linux client reports for that host (docs.discord.food/reference, Client
// Properties; discord_desktop_core's crash-reporter metadata; the web client's property builder).
namespace maicord {
namespace {

constexpr char kLaunchId[] = "11111111-2222-4333-8444-555555555555";
constexpr char kSignature[] = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";

Utf8View text(const char* value) noexcept {
    return {value, c_string_length(value)};
}

bool properties_json_equal(const GatewayUserProperties& properties, const char* expected) noexcept {
    u8 output[2048];
    const GatewayCommandWriteResult written = gateway_write_user_properties(properties, {output, sizeof(output)});
    return written.ok() && utf8_equal({reinterpret_cast<const char*>(output), written.bytes}, text(expected));
}

void test_windows_layout(void (*check)(bool, const char*) noexcept) noexcept {
    GatewayUserProperties properties{
        utf8_literal("Windows"), utf8_literal("Discord Client"), utf8_literal("stable"), utf8_literal("1.0.9267"),
        utf8_literal("10.0.26100"), utf8_literal("x64"), utf8_literal("x64"), utf8_literal("en-US"),
        text(kLaunchId),
        utf8_literal("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
                     "discord/1.0.9267 Chrome/152.0.7977.169 Electron/44.10.0 Safari/537.36"),
        utf8_literal("44.10.0"), utf8_literal("26100"), 663'810, 97'793, text(kSignature)};
    check(properties_json_equal(
              properties,
              "{\"os\":\"Windows\",\"browser\":\"Discord Client\",\"release_channel\":\"stable\","
              "\"client_version\":\"1.0.9267\",\"os_version\":\"10.0.26100\",\"os_version_is_release\":true,"
              "\"os_sdk_version\":\"26100\",\"os_arch\":\"x64\","
              "\"app_arch\":\"x64\",\"system_locale\":\"en-US\",\"has_client_mods\":false,"
              "\"client_launch_id\":\"11111111-2222-4333-8444-555555555555\","
              "\"browser_user_agent\":\"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like "
              "Gecko) discord/1.0.9267 Chrome/152.0.7977.169 Electron/44.10.0 Safari/537.36\","
              "\"browser_version\":\"44.10.0\",\"client_build_number\":663810,"
              "\"native_build_number\":97793,\"client_event_source\":null,"
              "\"launch_signature\":\"aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee\"}"),
          "the Windows client's properties keep their order, with os_sdk_version right after os_version_is_release");
    properties.os_sdk_version = {};
    u8 output[2048];
    check(gateway_write_user_properties(properties, {output, sizeof(output)}).error != GatewayCommandError::none,
          "the Windows layout requires the SDK version");
    // Fields the Windows layout does not know are ignored without linux_desktop.
    properties.os_sdk_version = utf8_literal("26100");
    properties.window_manager = utf8_literal("x,y");
    properties.distro = utf8_literal("Distro");
    properties.runtime_environment = utf8_literal("native");
    properties.display_server = utf8_literal("x11");
    check(properties_json_equal(properties, "{\"os\":\"Windows\",\"browser\":\"Discord Client\",\"release_channel\":"
                                            "\"stable\",\"client_version\":\"1.0.9267\",\"os_version\":"
                                            "\"10.0.26100\",\"os_version_is_release\":true,"
                                            "\"os_sdk_version\":\"26100\",\"os_arch\":\"x64\",\"app_arch\":\"x64\","
                                            "\"system_locale\":\"en-US\",\"has_client_mods\":false,"
                                            "\"client_launch_id\":\"11111111-2222-4333-8444-555555555555\","
                                            "\"browser_user_agent\":\"Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                            "AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.9267 "
                                            "Chrome/152.0.7977.169 Electron/44.10.0 Safari/537.36\","
                                            "\"browser_version\":\"44.10.0\","
                                            "\"client_build_number\":663810,\"native_build_number\":97793,"
                                            "\"client_event_source\":null,"
                                            "\"launch_signature\":\"aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee\"}"),
          "the Linux fields are not written into a Windows object");
}

#if !defined(_WIN32)

constexpr char kLinuxUserAgent[] =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.168 "
    "Chrome/152.0.7977.169 Electron/44.10.0 Safari/537.36";

// A built host: the storage its views point into and the properties with the fixed launch IDs.
struct Built {
    UserClientConfigurationStorage storage;
    GatewayUserProperties properties;
    UserClientConfigurationResult result;
};

void build(const LinuxClientEnvironment& environment, Built* built) noexcept {
    built->result = linux_client_identity_build(environment, &built->storage, &built->properties);
    built->properties.client_launch_id = text(kLaunchId);
    built->properties.launch_signature = text(kSignature);
}

LinuxClientEnvironment ubuntu_wayland() noexcept {
    LinuxClientEnvironment environment{};
    environment.kernel_release = utf8_literal("6.8.0-48-generic");
    environment.lang = utf8_literal("en_US.UTF-8");
    environment.xdg_current_desktop = utf8_literal("ubuntu:GNOME");
    environment.gdm_session = utf8_literal("ubuntu");
    environment.xdg_session_type = utf8_literal("wayland");
    environment.os_release = utf8_literal("PRETTY_NAME=\"Ubuntu 24.04.1 LTS\"\nNAME=\"Ubuntu\"\nVERSION_ID=\"24.04\"\n");
    return environment;
}

void test_exact_object(void (*check)(bool, const char*) noexcept) noexcept {
    static Built ubuntu;
    build(ubuntu_wayland(), &ubuntu);
    check(ubuntu.result.ok() &&
              properties_json_equal(
                  ubuntu.properties,
                  "{\"os\":\"Linux\",\"browser\":\"Discord Client\",\"release_channel\":\"stable\","
                  "\"client_version\":\"1.0.168\",\"os_version\":\"6.8.0-48-generic\","
                  "\"os_version_is_release\":true,\"os_arch\":\"x64\",\"app_arch\":\"x64\","
                  "\"system_locale\":\"en-US\",\"has_client_mods\":false,"
                  "\"client_launch_id\":\"11111111-2222-4333-8444-555555555555\","
                  "\"browser_user_agent\":\"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like "
                  "Gecko) discord/1.0.168 Chrome/152.0.7977.169 Electron/44.10.0 Safari/537.36\","
                  "\"browser_version\":\"44.10.0\",\"window_manager\":\"ubuntu:GNOME,ubuntu\","
                  "\"distro\":\"Ubuntu 24.04.1 LTS\",\"runtime_environment\":\"native\","
                  "\"display_server\":\"wayland\",\"client_build_number\":663810,"
                  "\"native_build_number\":97685,\"client_event_source\":null,"
                  "\"launch_signature\":\"aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee\"}"),
          "a GNOME Wayland Ubuntu host reports the Linux client's exact property object");
    // The request-time additions follow launch_signature, as on Windows.
    GatewayUserProperties later = ubuntu.properties;
    later.client_heartbeat_session_id = text(kLaunchId);
    later.client_app_state = utf8_literal("focused");
    u8 output[2048];
    const GatewayCommandWriteResult written = gateway_write_user_properties(later, {output, sizeof(output)});
    const char tail[] = "\"launch_signature\":\"aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee\","
                        "\"client_heartbeat_session_id\":\"11111111-2222-4333-8444-555555555555\","
                        "\"client_app_state\":\"focused\"}";
    check(written.ok() && written.bytes > sizeof(tail) &&
              bytes_equal(output + written.bytes - (sizeof(tail) - 1), tail, sizeof(tail) - 1),
          "the heartbeat session and app state follow launch_signature");
    check(utf8_equal(ubuntu.properties.browser_user_agent, text(kLinuxUserAgent)) &&
              utf8_equal(ubuntu.properties.os_arch, utf8_literal("x64")) &&
              ubuntu.properties.os_sdk_version.bytes == 0 && ubuntu.properties.linux_desktop,
          "the user agent is the Linux Electron host's and no SDK version is reported");
}

// The Gateway client keeps its properties in GatewayUserRuntime: what it stores and loads back must still be the Linux
// object, or the identify it sends (and the preflight at start) is the Windows one, which needs an os_sdk_version.
void test_runtime_round_trip(void (*check)(bool, const char*) noexcept) noexcept {
    static Built host;
    static GatewayUserRuntime user;
    build(ubuntu_wayland(), &host);
    GatewayUserClientConfiguration configuration{};
    configuration.properties = host.properties;
    client_detail::store_user_configuration(&user, configuration);
    char heartbeat_id[kGatewayUserLaunchIdBytes];
    const GatewayUserIdentify identify = client_detail::user_identify(user, text("token"), heartbeat_id);
    u8 before[2048], after[2048];
    const GatewayCommandWriteResult wrote = gateway_write_user_properties(host.properties, {before, sizeof(before)});
    const GatewayCommandWriteResult loaded = gateway_write_user_properties(identify.properties, {after, sizeof(after)});
    check(wrote.ok() && loaded.ok() && wrote.bytes == loaded.bytes && bytes_equal(before, after, wrote.bytes) &&
              identify.properties.linux_desktop,
          "a Linux property object is the same after the Gateway runtime stores and loads it");
    u8 command[kGatewayUserMaximumCommandBytes];
    check(gateway_write_user_identify(identify, {command, sizeof(command)}).ok(),
          "the identify the Gateway client builds from its stored properties is valid on Linux");
}

void test_other_hosts(void (*check)(bool, const char*) noexcept) noexcept {
    // A Flatpak KDE X11 host in French, the interface languages in $LANGUAGE, lsb-release naming the distribution.
    static Built french;
    LinuxClientEnvironment environment{};
    environment.kernel_release = utf8_literal("5.15.153.1-microsoft-standard-WSL2");
    environment.language = utf8_literal("fr:en_GB:C:de");
    environment.lang = utf8_literal("fr_FR.UTF-8");
    environment.xdg_current_desktop = utf8_literal("KDE");
    environment.xdg_session_type = utf8_literal("x11");
    environment.flatpak = true;
    environment.snap = true;
    environment.lsb_release =
        utf8_literal("DISTRIB_ID=LinuxMint\nDISTRIB_DESCRIPTION=\"Linux Mint 21.3 Virginia\"\n");
    environment.os_release = utf8_literal("PRETTY_NAME=\"Linux Mint 21.3\"\n");
    build(environment, &french);
    check(french.result.ok() && utf8_equal(french.properties.system_locale, utf8_literal("fr")) &&
              utf8_equal({french.storage.accept_language, french.storage.accept_language_bytes},
                         utf8_literal("fr,en-GB;q=0.9,de;q=0.8")) &&
              utf8_equal(french.properties.os_version, utf8_literal("5.15.153.1-microsoft-standard-WSL2")) &&
              utf8_equal(french.properties.window_manager, utf8_literal("KDE,unknown")) &&
              utf8_equal(french.properties.distro, utf8_literal("Linux Mint 21.3 Virginia")) &&
              utf8_equal(french.properties.runtime_environment, utf8_literal("flatpak")) &&
              utf8_equal(french.properties.display_server, utf8_literal("x11")) &&
              utf8_equal({french.storage.discord_locale, french.storage.discord_locale_bytes}, utf8_literal("fr")),
          "$LANGUAGE leads the locale and the language list; lsb-release names the distro; Flatpak outranks Snap");

    // Nothing set, nothing readable: the fallbacks and the omitted distro.
    static Built bare;
    LinuxClientEnvironment minimal{};
    minimal.kernel_release = utf8_literal("6.1.0");
    minimal.lang = utf8_literal("C.UTF-8");
    minimal.lc_messages = utf8_literal("POSIX");
    minimal.appimage = true;
    build(minimal, &bare);
    check(bare.result.ok() && utf8_equal(bare.properties.system_locale, utf8_literal("en-US")) &&
              utf8_equal({bare.storage.accept_language, bare.storage.accept_language_bytes}, utf8_literal("en-US")) &&
              utf8_equal(bare.properties.window_manager, utf8_literal("unknown,unknown")) &&
              bare.properties.distro.bytes == 0 &&
              utf8_equal(bare.properties.runtime_environment, utf8_literal("appimage")) &&
              utf8_equal(bare.properties.display_server, utf8_literal("unknown")),
          "a host that names nothing reports en-US, unknown,unknown, no distro and unknown display server");
    u8 output[2048];
    const GatewayCommandWriteResult written = gateway_write_user_properties(bare.properties, {output, sizeof(output)});
    bool has_distro = false;
    for (u32 at = 0; written.ok() && at + 8 <= written.bytes; ++at)
        has_distro = has_distro || bytes_equal(output + at, "\"distro\"", 8);
    check(written.ok() && !has_distro, "an absent distro is left out of the object");

    // Set but empty: Node's `??` keeps the empty string, so the window manager shows its comma and the display
    // server is omitted.
    static Built empty;
    LinuxClientEnvironment blank{};
    blank.kernel_release = utf8_literal("6.1.0");
    blank.xdg_current_desktop = utf8_literal("");
    blank.xdg_session_type = utf8_literal("");
    blank.lc_all = utf8_literal("");
    blank.lang = utf8_literal("de_AT.UTF-8");
    build(blank, &empty);
    check(empty.result.ok() && utf8_equal(empty.properties.window_manager, utf8_literal(",unknown")) &&
              empty.properties.display_server.bytes == 0 && utf8_equal(empty.properties.system_locale, utf8_literal("de")),
          "set-but-empty variables keep their empty value, and an empty $LC_ALL does not override $LANG");

    // Locale precedence and normalisation.
    char locale[16];
    const auto locale_of = [&](const char* language, const char* lc_all, const char* lc_messages, const char* lang) {
        LinuxClientEnvironment host{};
        host.language = language ? text(language) : Utf8View{};
        host.lc_all = lc_all ? text(lc_all) : Utf8View{};
        host.lc_messages = lc_messages ? text(lc_messages) : Utf8View{};
        host.lang = lang ? text(lang) : Utf8View{};
        const u32 bytes = linux_client_system_locale(host, {locale, sizeof(locale)});
        return Utf8View{locale, bytes};
    };
    check(utf8_equal(locale_of(nullptr, nullptr, nullptr, nullptr), utf8_literal("en-US")) &&
              utf8_equal(locale_of(nullptr, "pt_BR.UTF-8", "de_DE", "en_US"), utf8_literal("pt-BR")) &&
              utf8_equal(locale_of(nullptr, nullptr, "de_DE@euro", "en_US"), utf8_literal("de-DE")) &&
              utf8_equal(locale_of("sl_SI:en", "C", "de_DE", "en_US"), utf8_literal("sl-SI")) &&
              utf8_equal(locale_of("C:en_GB", nullptr, nullptr, "de_DE"), utf8_literal("en-GB")) &&
              utf8_equal(locale_of(nullptr, "C", nullptr, "sr_RS@latin"), utf8_literal("sr-RS")) &&
              utf8_equal(locale_of(nullptr, nullptr, nullptr, "bad locale!"), utf8_literal("en-US")),
          "the locale comes from LANGUAGE, LC_ALL, LC_MESSAGES then LANG, as a BCP 47 tag");

    // The distribution text: the client's PRETTY_NAME pattern and lsb_release's description.
    char distro[kGatewayUserMaximumHostBytes + 1];
    const auto distro_of = [&](const char* lsb, const char* os) {
        LinuxClientEnvironment host{};
        host.lsb_release = lsb ? text(lsb) : Utf8View{};
        host.os_release = os ? text(os) : Utf8View{};
        const u32 bytes = linux_client_distro(host, {distro, sizeof(distro)});
        return Utf8View{distro, bytes};
    };
    check(utf8_equal(distro_of(nullptr, "NAME=Arch\nPRETTY_NAME=\"Arch Linux\"\nID=arch\n"), utf8_literal("Arch Linux")) &&
              utf8_equal(distro_of(nullptr, "PRETTY_NAME=Fedora Linux 40 (Workstation Edition)"),
                         utf8_literal("Fedora Linux 40 (Workstation Edition)")) &&
              utf8_equal(distro_of(nullptr, "PRETTY_NAME_EXTRA=\"no\"\nPRETTY_NAME=\"Debian GNU/Linux 12 (bookworm)\""),
                         utf8_literal("Debian GNU/Linux 12 (bookworm)")) &&
              utf8_equal(distro_of(nullptr, "PRETTY_NAME='quoted'\n"), utf8_literal("'quoted'")) &&
              utf8_equal(distro_of("DISTRIB_DESCRIPTION=Pop!_OS 22.04 LTS\n", "PRETTY_NAME=\"Ubuntu\"\n"),
                         utf8_literal("Pop!_OS 22.04 LTS")) &&
              utf8_equal(distro_of("DISTRIB_ID=Ubuntu\n", "PRETTY_NAME=\"Ubuntu 22.04\"\n"), utf8_literal("Ubuntu 22.04")) &&
              distro_of(nullptr, "PRETTY_NAME=\"\"\n").bytes == 0 && distro_of(nullptr, "PRETTY_NAME=\"a\"b\"\n").bytes == 0 &&
              distro_of(nullptr, "NAME=Arch\n").bytes == 0 && distro_of(nullptr, nullptr).bytes == 0 &&
              distro_of(nullptr, "PRETTY_NAME=\"\xff\xfe\"\n").bytes == 0,
          "the distro is lsb-release's description, else os-release's PRETTY_NAME read as the client reads it");

    // A host that reports no kernel release cannot be described.
    static Built none;
    LinuxClientEnvironment nokernel{};
    build(nokernel, &none);
    check(!none.result.ok() && none.result.error == UserClientConfigurationError::version,
          "an unknown kernel release is an error rather than an invented os_version");
}

// The real host: whatever it is, the reading is well formed and builds a configuration the Gateway accepts.
void test_running_host(void (*check)(bool, const char*) noexcept) noexcept {
    static LinuxClientEnvironmentBuffer buffer;
    const LinuxClientEnvironment environment = linux_client_environment_read(&buffer);
    check(environment.kernel_release.bytes != 0 && environment.kernel_release.bytes <= kGatewayUserMaximumVersionBytes,
          "the running host's kernel release is read from uname");
    static Built host;
    build(environment, &host);
    u8 output[2048];
    check(host.result.ok() && gateway_write_user_properties(host.properties, {output, sizeof(output)}).ok() &&
              utf8_equal(host.properties.os_version, environment.kernel_release),
          "the running host builds properties the Gateway writer accepts");
}

#endif

} // namespace

void test_client_identity(void (*check)(bool, const char*) noexcept) noexcept {
    test_windows_layout(check);
#if !defined(_WIN32)
    test_exact_object(check);
    test_runtime_round_trip(check);
    test_other_hosts(check);
    test_running_host(check);
#endif
}

} // namespace maicord
