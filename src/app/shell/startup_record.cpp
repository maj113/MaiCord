#include "startup_record.hpp"
#include "../update/updates.hpp"
#include "maicord/core/log_event.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/os/thread.hpp"
#if defined(_WIN32)
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_process.hpp"
#else
#include "maicord/app/account/client_identity_linux.hpp"
#include "maicord/sys/linux/libc.hpp"
#include "maicord/sys/linux/process.hpp"
#endif

namespace maicord::app_detail {
namespace {
constexpr const char* kComponent = "start";

// The processor's brand string (CPUID leaves 0x80000002-0x80000004), without its padding.
const char* cpu_brand(char (&brand)[49]) noexcept {
    u32 registers[4]{};
    const auto cpuid = [&](u32 leaf) noexcept {
#if defined(_WIN32)
        __cpuid(reinterpret_cast<int*>(registers), static_cast<int>(leaf));
#else
        __asm__ volatile("cpuid"
                         : "=a"(registers[0]), "=b"(registers[1]), "=c"(registers[2]), "=d"(registers[3])
                         : "a"(leaf), "c"(0u));
#endif
    };
    brand[0] = brand[48] = 0;
    cpuid(0x80000000u);
    if (registers[0] < 0x80000004u)
        return brand;
    for (u32 leaf = 0; leaf < 3; ++leaf) {
        cpuid(0x80000002u + leaf);
        copy_bytes(brand + leaf * 16, registers, 16);
    }
    // Vendors pad the 48 bytes with spaces on either side.
    for (u32 end = c_string_length(brand); end && brand[end - 1] == ' ';)
        brand[--end] = 0;
    const char* start = brand;
    while (*start == ' ')
        ++start;
    return start;
}

// A UTF-16 locale name, ASCII in practice; anything else becomes '?'.
#if defined(_WIN32)
void narrow(const wchar_t* text, char* output, u32 capacity) noexcept {
    u32 used = 0;
    for (; text[used] && used + 1 < capacity; ++used)
        output[used] = text[used] < 0x80 ? static_cast<char>(text[used]) : '?';
    output[used] = 0;
}
#endif
} // namespace

void application_log_start_record() noexcept {
    char version[48];
    version[application_update_build_version_text({version, sizeof(version) - 1})] = 0;
    LogEvent banner("==== MaiCord");
    banner.append(" ");
    banner.append(version);
    banner.append(" start ====");
    banner.write(LogLevel::info, kComponent);

    char commit[kApplicationUpdatesCommitChars + 1]{};
    const Utf8View own_commit = application_update_build_commit();
    copy_bytes(commit, own_commit.data, own_commit.bytes);
    LogEvent build("build");
    build.field("version", version).field("commit", commit[0] ? commit : "unknown");
    bool development = false;
    for (const char* at = version; *at; ++at)
        development |= *at == '-';
    build.field("flavour", development ? "development" : "release");
#if defined(MAICORD_PERF_PROBE)
    build.field("probe", "perf");
#endif
#if defined(MAICORD_TEST_HOOKS)
    build.field("test_hooks", u64{1});
#endif
    build.write(LogLevel::info, kComponent);

    char brand[49];
    LogEvent machine("machine");
    machine.quoted("cpu", cpu_brand(brand));
    machine.field("cores", processor_count()).field("ram_mib", system_memory_total_kib() >> 10);
    LogEvent system("system");
#if defined(_WIN32)
    // KUSER_SHARED_DATA, the page every process maps at 0x7FFE0000: NtBuildNumber, NtMajorVersion, NtMinorVersion.
    const auto shared = [](usize offset) noexcept { return *reinterpret_cast<const volatile u32*>(0x7FFE0000ull + offset); };
    char os_version[40];
    u32 used = decimal_write(os_version, shared(0x26C));
    os_version[used++] = '.';
    used += decimal_write(os_version + used, shared(0x270));
    os_version[used++] = '.';
    used += decimal_write(os_version + used, shared(0x260) & 0xFFFFu);
    os_version[used] = 0;
    system.field("os", "Windows").field("version", os_version);
    wchar_t locale_wide[LOCALE_NAME_MAX_LENGTH]{};
    char locale[LOCALE_NAME_MAX_LENGTH];
    if (GetUserDefaultLocaleName(locale_wide, LOCALE_NAME_MAX_LENGTH) <= 1)
        locale_wide[0] = 0;
    narrow(locale_wide, locale, sizeof(locale));
#else
    LinuxClientEnvironmentBuffer buffer;
    const LinuxClientEnvironment environment = linux_client_environment_read(&buffer);
    char text[256];
    system.field("os", "Linux");
    copy_bytes(text, environment.kernel_release.data, minimum(environment.kernel_release.bytes, 255u));
    text[minimum(environment.kernel_release.bytes, 255u)] = 0;
    system.field("kernel", text);
    text[linux_client_distro(environment, {text, sizeof(text) - 1})] = 0;
    system.quoted("distro", text[0] ? text : "unknown");
    if (process_libc_is_musl()) {
        system.field("libc", "musl");
    } else {
        using Version = const char* (*)();
        const auto glibc_version = reinterpret_cast<Version>(dlsym(nullptr, "gnu_get_libc_version"));
        system.field("libc", "glibc").field("glibc", glibc_version ? glibc_version() : "unknown");
    }
    system.field("runtime", linux_client_runtime_environment(environment).data);
    char locale[64];
    locale[linux_client_system_locale(environment, {locale, sizeof(locale) - 1})] = 0;
#endif
    system.write(LogLevel::info, kComponent);
    machine.field("locale", locale[0] ? locale : "unknown").field("pid", process_id());
    machine.write(LogLevel::info, kComponent);
}

void application_log_start_display(Window window) noexcept {
    const WindowMetrics metrics = window_metrics(window);
    LogEvent("display")
        .field("count", window_display_count())
        .field("dpi", metrics.dpi)
        .field("scale_percent", metrics.scale_120 * 100 / 120)
        .write(LogLevel::info, kComponent);
}

void application_log_start_install() noexcept {
    const ApplicationUpdatesView view = application_updates_view();
    LogEvent("install")
        .field("kind", application_update_install_kind())
        .field("channel", view.channel == UpdateChannel::preview ? "preview" : "stable")
        .field("updates", view.state == ApplicationUpdateState::disabled ? "off" : "on")
        .write(LogLevel::info, kComponent);
}

} // namespace maicord::app_detail
