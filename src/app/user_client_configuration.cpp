#include "maicord/core/win32_process.hpp"
#include "maicord/app/user_client_configuration.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/client_user_agent.hpp"

#include "maicord/platform/random.hpp"

#include <Windows.h>

namespace maicord {
namespace {

using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*) noexcept;
SRWLOCK launch_lock = SRWLOCK_INIT;
char launch_id[kGatewayUserLaunchIdBytes + 1]{};

u8 hexadecimal(u8 value) noexcept {
    return value < 10 ? static_cast<u8>('0' + value)
                      : static_cast<u8>('a' + value - 10);
}

void write_uuid(const u8* source, char* output) noexcept {
    u32 destination = 0;
    for (u32 index = 0; index < 16; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            output[destination++] = '-';
        }
        output[destination++] = static_cast<char>(hexadecimal(source[index] >> 4));
        output[destination++] = static_cast<char>(hexadecimal(source[index] & 0x0F));
    }
    output[destination] = 0;
}

u32 write_os_version(const OSVERSIONINFOW& version, char* output,
                     u32 capacity) noexcept {
    const u32 required = decimal_digits(version.dwMajorVersion) + decimal_digits(version.dwMinorVersion) +
                         decimal_digits(version.dwBuildNumber) + 2;
    if (required >= capacity) {
        return 0;
    }
    u32 offset = decimal_write(output, version.dwMajorVersion);
    output[offset++] = '.';
    offset += decimal_write(output + offset, version.dwMinorVersion);
    output[offset++] = '.';
    offset += decimal_write(output + offset, version.dwBuildNumber);
    output[offset] = 0;
    return offset;
}

} // namespace

UserClientConfigurationResult user_client_configuration_initialize(
    UserClientConfigurationStorage* storage) noexcept {
    if (!storage) {
        return UserClientConfigurationResult{0, UserClientConfigurationError::invalid_input};
    }
    SecureZeroMemory(storage, sizeof(*storage));

    AcquireSRWLockExclusive(&launch_lock);
    if (!launch_id[0]) {
        u8 uuid[16]{};
        const SecureRandomResult random = secure_random_fill(Slice<u8>{uuid, sizeof(uuid)});
        if (!random.ok()) {
            SecureZeroMemory(uuid, sizeof(uuid));
            ReleaseSRWLockExclusive(&launch_lock);
            return UserClientConfigurationResult{static_cast<u32>(random.platform_status),
                                                 UserClientConfigurationError::random};
        }
        uuid[6] = static_cast<u8>((uuid[6] & 0x0F) | 0x40);
        uuid[8] = static_cast<u8>((uuid[8] & 0x3F) | 0x80);
        write_uuid(uuid, launch_id);
        SecureZeroMemory(uuid, sizeof(uuid));
    }
    copy_bytes(storage->client_launch_id, launch_id, sizeof(launch_id));
    ReleaseSRWLockExclusive(&launch_lock);

    wchar_t locale_wide[LOCALE_NAME_MAX_LENGTH]{};
    const int locale_characters = GetUserDefaultLocaleName(locale_wide, LOCALE_NAME_MAX_LENGTH);
    if (locale_characters <= 1) {
        const u32 native_error = thread_last_error();
        SecureZeroMemory(storage, sizeof(*storage));
        return UserClientConfigurationResult{native_error, UserClientConfigurationError::locale};
    }
    const int locale_bytes =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, locale_wide, locale_characters - 1,
                            storage->locale, kGatewayUserMaximumLocaleBytes, nullptr, nullptr);
    SecureZeroMemory(locale_wide, sizeof(locale_wide));
    if (locale_bytes <= 0 || locale_bytes > static_cast<int>(kGatewayUserMaximumLocaleBytes)) {
        const u32 native_error = thread_last_error();
        SecureZeroMemory(storage, sizeof(*storage));
        return UserClientConfigurationResult{native_error, UserClientConfigurationError::locale};
    }
    storage->locale[locale_bytes] = 0;
    storage->timezone_bytes = host_iana_timezone({storage->timezone, sizeof(storage->timezone)});

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto rtl_get_version =
        ntdll ? reinterpret_cast<RtlGetVersionFunction>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (!rtl_get_version || rtl_get_version(&version) < 0) {
        const u32 native_error = thread_last_error();
        SecureZeroMemory(storage, sizeof(*storage));
        return UserClientConfigurationResult{native_error, UserClientConfigurationError::version};
    }
    const u32 os_version_bytes =
        write_os_version(version, storage->os_version, static_cast<u32>(sizeof(storage->os_version)));
    SecureZeroMemory(&version, sizeof(version));
    if (os_version_bytes == 0) {
        SecureZeroMemory(storage, sizeof(*storage));
        return UserClientConfigurationResult{0, UserClientConfigurationError::version};
    }

    // os_version is major.minor.build, and the SDK version is its build.
    u32 sdk_offset = os_version_bytes;
    while (storage->os_version[sdk_offset - 1] != '.')
        --sdk_offset;
    GatewayUserClientConfiguration configuration{};
    configuration.properties = GatewayUserProperties{
        utf8_literal("Windows"),
        utf8_literal(kClientBrowser),
        utf8_literal("stable"),
        utf8_literal(kClientVersion),
        Utf8View{storage->os_version, os_version_bytes},
        utf8_literal("x64"),
        utf8_literal("x64"),
        Utf8View{storage->locale, static_cast<u32>(locale_bytes)},
        Utf8View{storage->client_launch_id, kGatewayUserLaunchIdBytes},
        utf8_literal(kClientUserAgent),
        utf8_literal(kClientElectronVersion),
        Utf8View{storage->os_version + sdk_offset, os_version_bytes - sdk_offset},
        kClientBuildNumber,
    };
    configuration.status = GatewayUserStatus::online;
    storage->configuration = configuration;
    u8 validation[kGatewayUserMaximumCommandBytes]{};
    const auto valid = gateway_write_user_identify({utf8_literal("metadata"), configuration.properties,
                                                    configuration.since_ms, configuration.status,
                                                    configuration.has_since, configuration.afk},
                                                   {validation, sizeof(validation)});
    SecureZeroMemory(validation, sizeof(validation));
    if (!valid.ok()) {
        SecureZeroMemory(storage, sizeof(*storage));
        return UserClientConfigurationResult{0, UserClientConfigurationError::validation};
    }
    return UserClientConfigurationResult{0, UserClientConfigurationError::none};
}

} // namespace maicord
