#include "maicord/core/win32_process.hpp"
#include "maicord/app/user_client_configuration.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/protocol/client_user_agent.hpp"

#include "maicord/platform/random.hpp"

#include <Windows.h>

namespace maicord {
namespace {

using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*) noexcept;

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
        return UserClientConfigurationResult{
            0, UserClientConfigurationError::invalid_input};
    }
    SecureZeroMemory(storage, sizeof(*storage));

    u8 uuid[16]{};
    const SecureRandomResult random = secure_random_fill(Slice<u8>{uuid, sizeof(uuid)});
    if (!random.ok()) {
        SecureZeroMemory(uuid, sizeof(uuid));
        return UserClientConfigurationResult{
            static_cast<u32>(random.platform_status),
            UserClientConfigurationError::random};
    }
    uuid[6] = static_cast<u8>((uuid[6] & 0x0F) | 0x40);
    uuid[8] = static_cast<u8>((uuid[8] & 0x3F) | 0x80);
    write_uuid(uuid, storage->client_launch_id);
    SecureZeroMemory(uuid, sizeof(uuid));

    wchar_t locale_wide[LOCALE_NAME_MAX_LENGTH]{};
    const int locale_characters =
        GetUserDefaultLocaleName(locale_wide, LOCALE_NAME_MAX_LENGTH);
    if (locale_characters <= 1) {
        const u32 native_error = thread_last_error();
        SecureZeroMemory(storage, sizeof(*storage));
        return UserClientConfigurationResult{
            native_error, UserClientConfigurationError::locale};
    }
    const int locale_bytes = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, locale_wide, locale_characters - 1,
        storage->locale, kGatewayUserMaximumLocaleBytes, nullptr, nullptr);
    SecureZeroMemory(locale_wide, sizeof(locale_wide));
    if (locale_bytes <= 0 ||
        locale_bytes > static_cast<int>(kGatewayUserMaximumLocaleBytes)) {
        const u32 native_error = thread_last_error();
        SecureZeroMemory(storage, sizeof(*storage));
        return UserClientConfigurationResult{
            native_error, UserClientConfigurationError::locale};
    }
    storage->locale[locale_bytes] = 0;
    storage->timezone_bytes = host_iana_timezone({storage->timezone, sizeof(storage->timezone)});

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto rtl_get_version =
        ntdll ? reinterpret_cast<RtlGetVersionFunction>(
                    GetProcAddress(ntdll, "RtlGetVersion"))
              : nullptr;
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (!rtl_get_version || rtl_get_version(&version) < 0) {
        const u32 native_error = thread_last_error();
        SecureZeroMemory(storage, sizeof(*storage));
        return UserClientConfigurationResult{
            native_error, UserClientConfigurationError::version};
    }
    const u32 os_version_bytes = write_os_version(
        version, storage->os_version,
        static_cast<u32>(sizeof(storage->os_version)));
    SecureZeroMemory(&version, sizeof(version));
    if (os_version_bytes == 0) {
        SecureZeroMemory(storage, sizeof(*storage));
        return UserClientConfigurationResult{
            0, UserClientConfigurationError::version};
    }

    GatewayUserClientConfiguration configuration{};
    configuration.properties = GatewayUserProperties{
        utf8_literal("Windows"),
        utf8_literal("MaiCord"),
        utf8_literal("MaiCord"),
        Utf8View{storage->locale, static_cast<u32>(locale_bytes)},
        utf8_literal(kClientUserAgent),
        utf8_literal("0.0.1"),
        Utf8View{storage->os_version, os_version_bytes},
        utf8_literal("stable"),
        Utf8View{storage->client_launch_id, kGatewayUserLaunchIdBytes},
        kClientBuildNumber,
    };
    configuration.status = GatewayUserStatus::online;
    storage->configuration = configuration;
    if (!gateway_user_client_configuration_validate(storage->configuration)) {
        SecureZeroMemory(storage, sizeof(*storage));
        return UserClientConfigurationResult{
            0, UserClientConfigurationError::validation};
    }
    return UserClientConfigurationResult{
        0, UserClientConfigurationError::none};
}

} // namespace maicord
