#include "maicord/core/win32_process.hpp"
#include "maicord/app/user_client_configuration.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/protocol/client_user_agent.hpp"

#include "maicord/platform/random.hpp"
#include "maicord/sys/registry.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {

// Wipes the partly built storage.
UserClientConfigurationResult fail(UserClientConfigurationStorage* storage, UserClientConfigurationError error,
                                   u32 native_error = 0) noexcept {
    SecureZeroMemory(storage, sizeof(*storage));
    return UserClientConfigurationResult{native_error, error};
}

using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*) noexcept;
SRWLOCK launch_lock = SRWLOCK_INIT;
char launch_id[kGatewayUserLaunchIdBytes + 1]{};
char launch_signature[kGatewayUserLaunchIdBytes + 1]{};

// The launch signature's bits that flag detected client mods, all clear for
// an unmodified client (docs.discord.food, Client Properties).
constexpr u8 kLaunchSignatureModBits[] = {119, 108, 100, 91, 84, 75, 61, 55, 48, 38, 24, 11};

void uuid_set_version4(u8* uuid) noexcept {
    uuid[6] = static_cast<u8>((uuid[6] & 0x0F) | 0x40);
    uuid[8] = static_cast<u8>((uuid[8] & 0x3F) | 0x80);
}

void write_uuid(const u8* source, char* output) noexcept {
    u32 destination = 0;
    for (u32 index = 0; index < 16; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            output[destination++] = '-';
        }
        output[destination++] = ascii_hex_digit(source[index] >> 4);
        output[destination++] = ascii_hex_digit(source[index]);
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

// The locales the desktop host ships (its locales\*.pak).
constexpr char kHostLocales[] =
    "af\0am\0ar\0bg\0bn\0ca\0cs\0da\0de\0el\0en-GB\0en-US\0es-419\0es\0et\0fa\0fi\0fil\0fr\0gu\0he\0hi\0hr\0hu\0id\0it\0"
    "ja\0kn\0ko\0lt\0lv\0ml\0mr\0ms\0nb\0nl\0pl\0pt-BR\0pt-PT\0ro\0ru\0sk\0sl\0sr\0sv\0sw\0ta\0te\0th\0tr\0uk\0ur\0vi\0"
    "zh-CN\0zh-TW\0";
// Discord's enabled interface locales, in its list order.
constexpr char kDiscordLocales[] =
    "en-US\0en-GB\0zh-CN\0zh-TW\0cs\0da\0nl\0fr\0de\0el\0hu\0it\0ja\0ko\0pl\0pt-BR\0ru\0es-419\0es-ES\0sv-SE\0tr\0bg\0uk\0"
    "fi\0no\0hr\0ro\0lt\0th\0vi\0hi\0";

// The first subtag.
Utf8View language_of(Utf8View locale) noexcept {
    u32 end = 0;
    while (end < locale.bytes && locale.data[end] != '-' && locale.data[end] != '_')
        ++end;
    return {locale.data, end};
}

u32 emit(Utf8View value, Slice<char> output) noexcept {
    if (value.bytes > output.count)
        return 0;
    copy_bytes(output.data, value.data, value.bytes);
    return value.bytes;
}

// A shipped locale equal to candidate, ignoring case; empty when none.
Utf8View host_locale_named(Utf8View candidate) noexcept {
    for (NameList list{kHostLocales}; list; list.next())
        if (ascii_equal_ignore_case(list.name(), candidate))
            return list.name();
    return {};
}

} // namespace

u32 client_host_locale(Utf8View locale, Slice<char> output) noexcept {
    if (const Utf8View exact = host_locale_named(locale); exact.data)
        return emit(exact, output);
    const Utf8View language = language_of(locale);
    if (language.bytes && language.bytes < locale.bytes) {
        const Utf8View region{locale.data + language.bytes + 1, locale.bytes - language.bytes - 1};
        const auto is = [](Utf8View value, Utf8View name) noexcept {
            return ascii_equal_ignore_case(value, name);
        };
        // Chromium's regional fallbacks, then the bare language.
        Utf8View mapped = language;
        if (is(language, utf8_literal("es")) && !is(region, utf8_literal("es")))
            mapped = utf8_literal("es-419");
        else if (is(language, utf8_literal("pt")))
            mapped = utf8_literal("pt-PT");
        else if (is(language, utf8_literal("zh")))
            mapped = is(region, utf8_literal("hk")) || is(region, utf8_literal("mo"))
                         ? utf8_literal("zh-TW")
                         : utf8_literal("zh-CN");
        else if (is(language, utf8_literal("en")))
            mapped = is(region, utf8_literal("lr")) || is(region, utf8_literal("ph"))
                         ? utf8_literal("en-US")
                         : utf8_literal("en-GB");
        if (const Utf8View resolved = host_locale_named(mapped); resolved.data)
            return emit(resolved, output);
    }
    constexpr Utf8View aliases[][2] = {
        {utf8_literal("en"), utf8_literal("en-US")}, {utf8_literal("iw"), utf8_literal("he")},
        {utf8_literal("no"), utf8_literal("nb")},    {utf8_literal("pt"), utf8_literal("pt-BR")},
        {utf8_literal("tl"), utf8_literal("fil")},   {utf8_literal("zh"), utf8_literal("zh-CN")},
    };
    for (const auto& alias : aliases)
        if (ascii_equal_ignore_case(language, alias[0]))
            return emit(alias[1], output);
    return emit(utf8_literal("en-US"), output);
}

u32 client_discord_locale(Utf8View host_locale, Slice<char> output) noexcept {
    for (NameList list{kDiscordLocales}; list; list.next())
        if (utf8_equal(list.name(), host_locale))
            return emit(list.name(), output);
    const Utf8View language = language_of(host_locale);
    for (NameList list{kDiscordLocales}; list; list.next())
        if (utf8_equal(list.name(), language))
            return emit(list.name(), output);
    if (utf8_equal(language, utf8_literal("zh")) && host_locale.bytes > 3 &&
        utf8_equal(language_of({host_locale.data + 3, host_locale.bytes - 3}), utf8_literal("Hant")))
        return emit(utf8_literal("zh-TW"), output);
    for (NameList list{kDiscordLocales}; list; list.next())
        if (utf8_equal(language_of(list.name()), language))
            return emit(list.name(), output);
    return emit(utf8_literal("en-US"), output);
}

u32 client_accept_language(Utf8View host_locale, const wchar_t* preferred, Slice<char> output) noexcept {
    u32 used = emit(host_locale, output);
    if (!used)
        return 0;
    u32 weight = 9;
    const wchar_t* next = nullptr;
    for (const wchar_t* entry = preferred; entry && *entry; entry = next + 1) {
        for (next = entry; *next;)
            ++next;
        char language[kGatewayUserMaximumLocaleBytes];
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, entry, -1, language,
                                              sizeof(language), nullptr, nullptr);
        if (bytes <= 1)
            continue;
        const Utf8View name{language, static_cast<u32>(bytes - 1)};
        bool tag = true;
        for (u32 index = 0; index < name.bytes; ++index)
            tag = tag && (ascii_is_alnum(name.data[index]) || name.data[index] == '-');
        if (!tag || utf8_equal(name, host_locale))
            continue;
        const u32 required = 1 + name.bytes + 6;
        if (used + required > output.count)
            break;
        output.data[used++] = ',';
        used += emit(name, {output.data + used, output.count - used});
        copy_bytes(output.data + used, ";q=0.", 5);
        used += 5;
        output.data[used++] = static_cast<char>('0' + weight);
        if (weight > 1)
            --weight;
    }
    return used;
}

bool user_client_random_uuid(char* output) noexcept {
    u8 uuid[16]{};
    if (!secure_random_fill(Slice<u8>{uuid, sizeof(uuid)}).ok())
        return false;
    uuid_set_version4(uuid);
    write_uuid(uuid, output);
    SecureZeroMemory(uuid, sizeof(uuid));
    return true;
}

UserClientConfigurationResult user_client_configuration_initialize(
    UserClientConfigurationStorage* storage) noexcept {
    if (!storage) {
        return UserClientConfigurationResult{0, UserClientConfigurationError::invalid_input};
    }
    SecureZeroMemory(storage, sizeof(*storage));

    AcquireSRWLockExclusive(&launch_lock);
    if (!launch_id[0]) {
        u8 uuid[32]{};
        const SecureRandomResult random = secure_random_fill(Slice<u8>{uuid, sizeof(uuid)});
        if (!random.ok()) {
            SecureZeroMemory(uuid, sizeof(uuid));
            ReleaseSRWLockExclusive(&launch_lock);
            return UserClientConfigurationResult{static_cast<u32>(random.platform_status),
                                                 UserClientConfigurationError::random};
        }
        u8* signature = uuid + 16;
        uuid_set_version4(uuid);
        uuid_set_version4(signature);
        // Bit n of the big-endian 128-bit value.
        for (const u8 bit : kLaunchSignatureModBits)
            signature[15 - bit / 8] = static_cast<u8>(signature[15 - bit / 8] & ~(1u << (bit % 8)));
        write_uuid(uuid, launch_id);
        write_uuid(signature, launch_signature);
        SecureZeroMemory(uuid, sizeof(uuid));
    }
    copy_bytes(storage->client_launch_id, launch_id, sizeof(launch_id));
    copy_bytes(storage->launch_signature, launch_signature, sizeof(launch_signature));
    ReleaseSRWLockExclusive(&launch_lock);

    // The host takes its locale from ICU's default, the user's format locale.
    wchar_t locale_wide[LOCALE_NAME_MAX_LENGTH]{};
    const int locale_characters = GetUserDefaultLocaleName(locale_wide, LOCALE_NAME_MAX_LENGTH);
    if (locale_characters <= 1)
        return fail(storage, UserClientConfigurationError::locale, thread_last_error());
    char system_locale[kGatewayUserMaximumLocaleBytes];
    const int system_bytes =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, locale_wide, locale_characters - 1,
                            system_locale, sizeof(system_locale), nullptr, nullptr);
    if (system_bytes <= 0)
        return fail(storage, UserClientConfigurationError::locale, thread_last_error());
    const u32 locale_bytes = client_host_locale({system_locale, static_cast<u32>(system_bytes)},
                                                {storage->locale, kGatewayUserMaximumLocaleBytes});
    const Utf8View host_locale{storage->locale, locale_bytes};
    storage->discord_locale_bytes =
        client_discord_locale(host_locale, {storage->discord_locale, kGatewayUserMaximumLocaleBytes});
    // navigator.languages: the host locale, then the Windows language list.
    wchar_t preferred[512]{};
    DWORD preferred_bytes = sizeof(preferred) - 2 * sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\International\\User Profile", L"Languages",
                     RRF_RT_REG_MULTI_SZ, nullptr, preferred, &preferred_bytes) != ERROR_SUCCESS)
        preferred[0] = 0;
    storage->accept_language_bytes = client_accept_language(
        host_locale, preferred, {storage->accept_language, kRestUserAcceptLanguageMaximumBytes});
    storage->timezone_bytes = host_iana_timezone({storage->timezone, sizeof(storage->timezone)});

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto rtl_get_version =
        ntdll ? reinterpret_cast<RtlGetVersionFunction>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (!rtl_get_version || rtl_get_version(&version) < 0)
        return fail(storage, UserClientConfigurationError::version, thread_last_error());
    const u32 os_version_bytes =
        write_os_version(version, storage->os_version, static_cast<u32>(sizeof(storage->os_version)));
    SecureZeroMemory(&version, sizeof(version));
    if (os_version_bytes == 0)
        return fail(storage, UserClientConfigurationError::version);

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
        host_locale,
        Utf8View{storage->client_launch_id, kGatewayUserLaunchIdBytes},
        utf8_literal(kClientUserAgent),
        utf8_literal(kClientElectronVersion),
        Utf8View{storage->os_version + sdk_offset, os_version_bytes - sdk_offset},
        kClientBuildNumber,
        kClientNativeBuildNumber,
        Utf8View{storage->launch_signature, kGatewayUserLaunchIdBytes},
    };
    configuration.status = GatewayUserStatus::online;
    storage->configuration = configuration;
    u8 validation[kGatewayUserMaximumCommandBytes]{};
    const auto valid = gateway_write_user_identify({utf8_literal("metadata"), configuration.properties,
                                                    configuration.since_ms, configuration.status,
                                                    configuration.has_since, configuration.afk},
                                                   {validation, sizeof(validation)});
    SecureZeroMemory(validation, sizeof(validation));
    if (!valid.ok())
        return fail(storage, UserClientConfigurationError::validation);
    return UserClientConfigurationResult{0, UserClientConfigurationError::none};
}

} // namespace maicord
