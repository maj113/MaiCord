#include "maicord/app/account_module_api.hpp"
#include "maicord/app/account_module_version.hpp"
#include "maicord/app/account_paths.hpp"
#include "maicord/app/account_profile_promotion.hpp"
#include "maicord/app/captcha_challenge.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/app/login_mfa_policy.hpp"
#include "maicord/app/remote_auth_client.hpp"
#include "maicord/app/remote_auth_ticket_exchange.hpp"
#include "maicord/app/user_auth_coordinator.hpp"
#include "maicord/core/time.hpp"
#include "maicord/media/deflate.hpp"
#include "maicord/protocol/rest.hpp"
#include "maicord/protocol/gateway_commands.hpp"
#include "maicord/protocol/gateway_endpoint.hpp"
#include "maicord/protocol/gateway_session.hpp"
#include "maicord/protocol/user_auth.hpp"

#include <Windows.h>

namespace maicord {
namespace {

inline constexpr u32 kCommandIterations = 2'000'000;
inline constexpr u32 kMetadataIterations = 1'000'000;
inline constexpr u32 kWebauthnIterations = 250'000;
inline constexpr u32 kCaptchaIterations = 250'000;
inline constexpr u32 kPathIterations = 1'000'000;
inline constexpr u32 kProfilePromotionIterations = 32;
inline constexpr u32 kMfaCodeIterations = 2'000'000;
volatile u64 g_checksum = 0;

u32 text_length(const char* text) noexcept {
    u32 bytes = 0;
    while (text[bytes] != 0) {
        ++bytes;
    }
    return bytes;
}

void write_text(const char* text) noexcept {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, text_length(text), &written, nullptr);
}

void write_u64(u64 value) noexcept {
    char reverse[24]{};
    u32 count = 0;
    do {
        reverse[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    char output[24]{};
    for (u32 index = 0; index < count; ++index) {
        output[index] = reverse[count - index - 1];
    }
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output, count, &written, nullptr);
}

void write_metric(const char* name, u64 iterations, u64 elapsed_us) noexcept {
    write_text(name);
    write_text("=");
    write_u64(iterations * 1'000'000ull / elapsed_us);
    write_text(" ops/s\r\n");
}

template <usize Count>
bool append_literal(char* output, u32 capacity, u32* offset, const char (&literal)[Count]) noexcept {
    if (!output || !offset || Count == 0 || *offset > capacity || Count - 1 > capacity - *offset) {
        return false;
    }
    for (u32 index = 0; index < static_cast<u32>(Count - 1); ++index) {
        output[(*offset)++] = literal[index];
    }
    return true;
}

bool write_dense_captcha(char* output, u32 capacity) noexcept {
    if (!output || capacity != 507) {
        return false;
    }
    u32 offset = 0;
    if (!append_literal(
            output, capacity, &offset,
            R"({"captcha_key":["captcha-required"],"captcha_service":"hcaptcha","captcha_sitekey":"site-key","captcha_rqdata":"rq-data","captcha_rqtoken":"rq-token","padding":[)")) {
        return false;
    }
    for (u32 index = 0; index < 100; ++index) {
        if ((index != 0 && !append_literal(output, capacity, &offset, ",")) ||
            !append_literal(output, capacity, &offset, "[]")) {
            return false;
        }
    }
    if (!append_literal(output, capacity, &offset, R"(],"filler":")") || offset + 2 > capacity) {
        return false;
    }
    while (offset + 2 < capacity) {
        output[offset++] = 'x';
    }
    output[offset++] = '"';
    output[offset++] = '}';
    return offset == capacity;
}

bool profile_benchmark_path(wchar_t* output, u32 capacity) noexcept {
    if (!output || capacity == 0) {
        return false;
    }
    const DWORD characters = GetTempPathW(capacity, output);
    if (characters == 0 || characters >= capacity) {
        return false;
    }
    constexpr wchar_t prefix[] = L"MaiCord-profile-promotion-";
    constexpr wchar_t suffix[] = L".profile";
    constexpr u32 prefix_characters = static_cast<u32>(sizeof(prefix) / sizeof(prefix[0])) - 1;
    constexpr u32 suffix_characters = static_cast<u32>(sizeof(suffix) / sizeof(suffix[0])) - 1;
    wchar_t reverse[10]{};
    u32 digits = 0;
    u32 process_id = GetCurrentProcessId();
    do {
        reverse[digits++] = static_cast<wchar_t>(L'0' + process_id % 10);
        process_id /= 10;
    } while (process_id != 0);
    if (characters + prefix_characters + digits + suffix_characters + 1 > capacity) {
        return false;
    }
    u32 offset = characters;
    for (u32 index = 0; index < prefix_characters; ++index) {
        output[offset++] = prefix[index];
    }
    for (u32 index = 0; index < digits; ++index) {
        output[offset++] = reverse[digits - index - 1];
    }
    for (u32 index = 0; index < suffix_characters; ++index) {
        output[offset++] = suffix[index];
    }
    output[offset] = 0;
    return true;
}

u32 account_module_runtime_bytes() noexcept {
    wchar_t path[kAccountProfileMaximumPathCharacters]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, kAccountProfileMaximumPathCharacters);
    if (length == 0 || length >= kAccountProfileMaximumPathCharacters) {
        return 0;
    }
    u32 separator = length;
    while (separator != 0 && path[separator - 1] != L'\\' && path[separator - 1] != L'/') {
        --separator;
    }
    constexpr u32 name_length =
        static_cast<u32>(sizeof(kAccountModuleFileNameWide) / sizeof(kAccountModuleFileNameWide[0])) - 1;
    if (separator + name_length + 1 > kAccountProfileMaximumPathCharacters) {
        return 0;
    }
    for (u32 index = 0; index < name_length; ++index) {
        path[separator + index] = kAccountModuleFileNameWide[index];
    }
    path[separator + name_length] = 0;
    HMODULE module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        return 0;
    }
    const auto get_api =
        reinterpret_cast<AccountModuleGetApiFunction>(
            GetProcAddress(module,
                           MAKEINTRESOURCEA(kAccountModuleApiOrdinal)));
    AccountModuleApi api{};
    const u32 bytes =
        get_api && get_api(kAccountModuleApiVersion, &api) && api.runtime_bytes ? api.runtime_bytes() : 0;
    FreeLibrary(module);
    return bytes;
}

u32 run() noexcept {
    const Clock clock = clock_initialize();
    if (clock.frequency <= 0) {
        return 1;
    }
    u64 checksum = 0;
    const GatewayUserProperties properties{utf8_literal("Windows"),
                                           utf8_literal("Discord Client"),
                                           utf8_literal("stable"),
                                           utf8_literal("1.0.9256"),
                                           utf8_literal("10.0.19045"),
                                           utf8_literal("x64"),
                                           utf8_literal("x64"),
                                           utf8_literal("en-US"),
                                           utf8_literal("00112233-4455-6677-8899-aabbccddeeff"),
                                           utf8_literal("Agent/1.0 (Windows 10; Win64; x64)"),
                                           utf8_literal("42.9.0"),
                                           utf8_literal("19045"),
                                           603738};
    u8 output[kGatewayUserMaximumCommandBytes]{};

    GatewayUserIdentify identify{
        utf8_literal("benchmark-user-token"), properties, 0, GatewayUserStatus::online, true, false};
    i64 begin = clock_now();
    for (u32 iteration = 0; iteration < kCommandIterations; ++iteration) {
        identify.since_ms = iteration;
        identify.afk = (iteration & 1u) != 0;
        const GatewayCommandWriteResult written =
            gateway_write_user_identify(identify, Slice<u8>{output, sizeof(output)});
        if (!written.ok()) {
            return 2;
        }
        checksum += written.bytes + output[written.bytes - 1];
    }
    const u64 identify_us = clock_microseconds(clock, begin, clock_now());

    GatewayResume resume{utf8_literal("benchmark-user-token"), utf8_literal("benchmark-session"), 0};
    begin = clock_now();
    for (u32 iteration = 0; iteration < kCommandIterations; ++iteration) {
        resume.sequence = iteration;
        const GatewayCommandWriteResult written =
            gateway_write_resume(resume, Slice<u8>{output, sizeof(output)});
        if (!written.ok()) {
            return 3;
        }
        checksum += written.bytes + output[written.bytes - 1];
    }
    const u64 resume_us = clock_microseconds(clock, begin, clock_now());

    GatewayUserClientConfiguration configuration{properties, 0, GatewayUserStatus::online, false, false};
    begin = clock_now();
    for (u32 iteration = 0; iteration < kMetadataIterations; ++iteration) {
        configuration.since_ms = iteration;
        configuration.has_since = (iteration & 1u) != 0;
        if (!gateway_user_client_configuration_validate(configuration)) {
            return 4;
        }
        checksum += configuration.since_ms & 31u;
    }
    const u64 configuration_us = clock_microseconds(clock, begin, clock_now());

    u8 json[1024]{};
    u8 base64[2048]{};
    begin = clock_now();
    for (u32 iteration = 0; iteration < kMetadataIterations; ++iteration) {
        GatewayUserProperties current = properties;
        current.client_build_number += iteration & 1u;
        const RestUserPropertiesResult written = write_rest_user_super_properties(
            current, Slice<u8>{json, sizeof(json)}, Slice<u8>{base64, sizeof(base64)});
        if (!written.ok()) {
            return 5;
        }
        checksum += written.bytes + base64[written.bytes - 1];
    }
    const u64 properties_us = clock_microseconds(clock, begin, clock_now());

    GatewayEndpoint endpoint{};
    begin = clock_now();
    for (u32 iteration = 0; iteration < kMetadataIterations; ++iteration) {
        if (gateway_endpoint_parse_with_policy(utf8_literal("wss://gateway.discord.gg"),
                                               GatewayEndpointPolicy::user_v9_json_zlib_stream,
                                               &endpoint) != GatewayEndpointError::none) {
            return 6;
        }
        checksum += endpoint.target_bytes + (iteration & 1u);
    }
    const u64 endpoint_us = clock_microseconds(clock, begin, clock_now());

    u8 auth_output[512]{};
    UserAuthPasswordRequest password{utf8_literal("benchmark@example.test"),
                                     utf8_literal("benchmark-password"), kUserAuthDefaultLoginSource,
                                     false};
    begin = clock_now();
    for (u32 iteration = 0; iteration < kCommandIterations; ++iteration) {
        password.undelete = (iteration & 1u) != 0;
        const UserAuthWriteResult written =
            user_auth_write_password_request(password, Slice<u8>{auth_output, sizeof(auth_output)});
        if (!written.ok()) {
            return 7;
        }
        checksum += written.bytes + auth_output[written.bytes - 1];
    }
    const u64 auth_write_us = clock_microseconds(clock, begin, clock_now());

    begin = clock_now();
    for (u32 iteration = 0; iteration < kCommandIterations; ++iteration) {
        const UserAuthWriteResult written = user_auth_write_request_head(
            UserAuthOperation::password, 120 + (iteration & 7u), UserAuthCaptchaHeaders{},
            Slice<u8>{auth_output, sizeof(auth_output)});
        if (!written.ok()) {
            return 8;
        }
        checksum += written.bytes + auth_output[written.bytes - 1];
    }
    const u64 auth_head_us = clock_microseconds(clock, begin, clock_now());

    char webauthn_credential[2 * 1024]{};
    for (u32 index = 0; index < static_cast<u32>(sizeof(webauthn_credential)); ++index) {
        webauthn_credential[index] = static_cast<char>('A' + (index & 15u));
    }
    u8 webauthn_output[4 * 1024]{};
    const UserAuthMfaRequest webauthn{
        Utf8View{webauthn_credential, static_cast<u32>(sizeof(webauthn_credential))},
        utf8_literal("benchmark-mfa-ticket"), utf8_literal("MFA Login"),
        utf8_literal("benchmark-login-instance"), UserAuthMfaType::webauthn};
    begin = clock_now();
    for (u32 iteration = 0; iteration < kWebauthnIterations; ++iteration) {
        const UserAuthWriteResult written =
            user_auth_write_mfa_request(webauthn, Slice<u8>{webauthn_output, sizeof(webauthn_output)});
        if (!written.ok()) {
            return 9;
        }
        checksum += written.bytes + webauthn_output[iteration & 1u];
    }
    const u64 webauthn_write_us = clock_microseconds(clock, begin, clock_now());

    CaptchaChallenge captcha{};
    if (!captcha_challenge_initialize(&captcha).ok()) {
        return 10;
    }
    u32 captcha_structurals[kRemoteAuthTicketExchangeStructuralCount]{};
    UserAuthResponse captcha_response{};
    begin = clock_now();
    for (u32 iteration = 0; iteration < kCaptchaIterations; ++iteration) {
        const CaptchaChallengeResult decoded = captcha_challenge_decode(
            &captcha, 400,
            utf8_literal(
                R"({"captcha_key":["captcha-required"],"captcha_service":"hcaptcha","captcha_sitekey":"site-key","captcha_rqdata":"rq-data","captcha_rqtoken":"rq-token","captcha_session_id":"session-id","should_serve_invisible":true,"user_flow":"login"})"),
            Slice<u32>{captcha_structurals,
                       static_cast<u32>(sizeof(captcha_structurals) / sizeof(captcha_structurals[0]))},
            16, &captcha_response);
        if (!decoded.ok()) {
            captcha_challenge_release(&captcha);
            return 10;
        }
        checksum += captcha_response.captcha_rqtoken_bytes + (iteration & 1u);
    }
    const u64 captcha_decode_us = clock_microseconds(clock, begin, clock_now());

    char dense_captcha[507]{};
    if (!write_dense_captcha(dense_captcha, static_cast<u32>(sizeof(dense_captcha)))) {
        captcha_challenge_release(&captcha);
        return 10;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kCaptchaIterations; ++iteration) {
        const CaptchaChallengeResult decoded = captcha_challenge_decode(
            &captcha, 400, Utf8View{dense_captcha, static_cast<u32>(sizeof(dense_captcha))},
            Slice<u32>{captcha_structurals,
                       static_cast<u32>(sizeof(captcha_structurals) / sizeof(captcha_structurals[0]))},
            16, &captcha_response);
        if (!decoded.ok()) {
            SecureZeroMemory(dense_captcha, sizeof(dense_captcha));
            captcha_challenge_release(&captcha);
            return 10;
        }
        checksum += captcha_response.captcha_rqtoken_bytes + (iteration & 1u);
    }
    const u64 dense_captcha_decode_us = clock_microseconds(clock, begin, clock_now());
    SecureZeroMemory(dense_captcha, sizeof(dense_captcha));
    captcha_challenge_release(&captcha);

    AccountPaths paths{};
    begin = clock_now();
    for (u32 iteration = 0; iteration < kPathIterations; ++iteration) {
        if (!account_paths_build_slot(L"C:\\Users\\Benchmark\\AppData\\Local", 0, &paths).ok()) {
            return 11;
        }
        checksum += static_cast<u64>(paths.profile[iteration & 31u]);
    }
    const u64 path_us = clock_microseconds(clock, begin, clock_now());

    constexpr u8 backup_code[] = {'A', 'B', 'C', 'D', '-', 'E', 'F', 'G', 'H'};
    SecureBuffer normalized_mfa{};
    if (!secure_buffer_initialize(&normalized_mfa, kLoginMfaNormalizedCodeBytes).ok()) {
        return 12;
    }
    begin = clock_now();
    for (u32 iteration = 0; iteration < kMfaCodeIterations; ++iteration) {
        const LoginMfaCodeResult prepared = login_mfa_prepare_code(
            UserAuthMfaType::backup, ConstSlice<u8>{backup_code, static_cast<u32>(sizeof(backup_code))},
            &normalized_mfa);
        if (!prepared.ok()) {
            secure_buffer_release(&normalized_mfa);
            return 12;
        }
        checksum += normalized_mfa.bytes + (iteration & 1u);
    }
    const u64 mfa_code_us = clock_microseconds(clock, begin, clock_now());
    secure_buffer_release(&normalized_mfa);

    wchar_t profile_path[kAccountProfileMaximumPathCharacters]{};
    if (!profile_benchmark_path(profile_path, kAccountProfileMaximumPathCharacters)) {
        return 12;
    }
    DeleteFileW(profile_path);
    constexpr u8 profile_credential[] = {'b', 'e', 'n', 'c', 'h', 'm', 'a', 'r', 'k', '-', 'u',
                                         's', 'e', 'r', '-', 's', 'e', 's', 's', 'i', 'o', 'n'};
    begin = clock_now();
    for (u32 iteration = 0; iteration < kProfilePromotionIterations; ++iteration) {
        AccountProfile source{};
        AccountProfilePromotion promotion{};
        const SecureBufferResult initialized = secure_buffer_initialize(&source.credential, 64);
        const SecureBufferResult assigned =
            initialized.ok()
                ? secure_buffer_assign(
                      &source.credential,
                      ConstSlice<u8>{profile_credential, static_cast<u32>(sizeof(profile_credential))})
                : initialized;
        source.metadata = AccountProfileMetadata{0x8877665500000000ull + iteration + 1};
        const AccountProfilePromotionResult staged =
            assigned.ok() ? account_profile_promotion_stage(&promotion, &source)
                          : AccountProfilePromotionResult{};
        const AccountProfilePromotionResult committed =
            staged.ok() ? account_profile_promotion_commit(&promotion, profile_path)
                        : AccountProfilePromotionResult{};
        account_profile_release(&source);
        account_profile_promotion_release(&promotion);
        if (!committed.ok()) {
            DeleteFileW(profile_path);
            return 12;
        }
        checksum += committed.bytes + iteration;
    }
    const u64 profile_promotion_us = clock_microseconds(clock, begin, clock_now());
    if (!DeleteFileW(profile_path)) {
        return 12;
    }
    g_checksum = checksum;

    if (identify_us == 0 || resume_us == 0 || configuration_us == 0 || properties_us == 0 ||
        endpoint_us == 0 || auth_write_us == 0 || auth_head_us == 0 || webauthn_write_us == 0 ||
        captcha_decode_us == 0 || dense_captcha_decode_us == 0 || path_us == 0 ||
        profile_promotion_us == 0 || mfa_code_us == 0) {
        return 13;
    }
    write_metric("account_user_gateway_identify", kCommandIterations, identify_us);
    write_metric("account_user_gateway_resume", kCommandIterations, resume_us);
    write_metric("account_user_gateway_configuration_validate", kMetadataIterations, configuration_us);
    write_metric("account_user_super_properties", kMetadataIterations, properties_us);
    write_metric("account_user_gateway_endpoint", kMetadataIterations, endpoint_us);
    write_metric("account_auth_password_write", kCommandIterations, auth_write_us);
    write_metric("account_auth_request_head_write", kCommandIterations, auth_head_us);
    write_metric("account_auth_webauthn_write", kWebauthnIterations, webauthn_write_us);
    write_metric("account_auth_captcha_decode", kCaptchaIterations, captcha_decode_us);
    write_metric("account_auth_dense_captcha_decode", kCaptchaIterations, dense_captcha_decode_us);
    write_metric("account_path_build", kPathIterations, path_us);
    write_metric("account_profile_promotion_commit", kProfilePromotionIterations, profile_promotion_us);
    write_metric("account_auth_mfa_code_prepare", kMfaCodeIterations, mfa_code_us);
    write_text("account_user_gateway_runtime_bytes=");
    write_u64(gateway_client_user_runtime_bytes());
    write_text(" bytes\r\naccount_gateway_session_inline_bytes=");
    write_u64(sizeof(GatewaySession));
    write_text(" bytes\r\naccount_gateway_zlib_stream_bytes=");
    write_u64(sizeof(ZlibStreamInflater));
    write_text(" bytes\r\naccount_user_rest_runtime_bytes=");
    write_u64(rest_client_user_runtime_bytes(kRestClientMinimumResponseBytes));
    write_text(" bytes\r\naccount_user_auth_runtime_bytes=");
    write_u64(rest_client_user_auth_runtime_bytes(kUserAuthMaximumResponseBytes));
    write_text(" bytes\r\naccount_auth_coordinator_runtime_bytes=");
    write_u64(user_auth_coordinator_runtime_bytes());
    write_text(" bytes\r\naccount_remote_auth_runtime_bytes=");
    write_u64(remote_auth_client_runtime_bytes());
    write_text(" bytes\r\naccount_module_runtime_bytes=");
    write_u64(account_module_runtime_bytes());
    write_text(" bytes\r\naccount_profile_promotion_owner_bytes=");
    write_u64(sizeof(AccountProfilePromotion));
    write_text(" bytes\r\n");
    return 0;
}

} // namespace
} // namespace maicord

extern "C" __declspec(noreturn) void WINAPI maicord_account_benchmark_entry() noexcept {
    ExitProcess(static_cast<UINT>(maicord::run()));
}
